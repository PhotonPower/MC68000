"""The example program of the whole chain: assembler -> hunk file -> loader -> emulator with OS calls -> analysis -> orchestrator (dry run).

Run:  M68KTOOL=build/m68ktool python3 -m unittest discover -s tests/agent -v
"""
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mock_ollama import MockOllama  # noqa: E402

spec = importlib.util.spec_from_file_location("m68k_annotate", os.path.join(ROOT, "agent", "m68k_annotate.py"))
annotate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(annotate)

M68KTOOL = os.environ.get("M68KTOOL") or os.path.join(ROOT, "build", "m68ktool")
SOURCE = os.path.join(ROOT, "examples", "amiga", "hello_amiga.s")
EXPECTED_OUTPUT = "Hello from the MC68000 example\nsum=150\nwords=5\n"


class Session:
    """A minimal client of `m68ktool serve`."""

    def __init__(self):
        self.p = subprocess.Popen([M68KTOOL, "serve", "--allow-files"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def call(self, **req):
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=5)
        self.p.stdout.close()


class ExampleProgramTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M68KTOOL):
            raise unittest.SkipTest(f"m68ktool not found at {M68KTOOL} (set M68KTOOL)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.plain = os.path.join(cls.tmp.name, "hello_amiga")
        cls.named = os.path.join(cls.tmp.name, "hello_amiga_symbols")
        for out, extra in ((cls.plain, []), (cls.named, ["--hunk-symbols"])):
            r = subprocess.run([M68KTOOL, "asm", SOURCE, "--hunk", out, *extra], capture_output=True, text=True)
            assert r.returncode == 0, r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_runs_in_the_emulator(self):
        s = Session()
        try:
            self.assertTrue(s.call(cmd="machine", type="amiga500", os=True)["ok"])
            loaded = s.call(cmd="load_file", file=self.plain, type="hunk")
            self.assertTrue(loaded["ok"], loaded)
            self.assertEqual(loaded["entry"], "0x020000")
            self.assertEqual(len(loaded["hunks"]), 3)
            run = s.call(cmd="run")
            self.assertEqual(run["stop"], "exit")
            self.assertIn("D0 = 0", run["message"])
            self.assertEqual(s.call(cmd="console")["text"], EXPECTED_OUTPUT)
            calls = s.call(cmd="libcalls")["calls"]
            names = [c["function"] for c in calls]
            self.assertEqual(names[:2], ["OpenLibrary", "Output"])
            self.assertEqual(names[-1], "CloseLibrary")
            self.assertEqual(names.count("Write"), 7)
            self.assertTrue(all(c["emulated"] for c in calls))
        finally:
            s.close()

    def test_symbols_are_optional(self):
        s = Session()
        try:
            s.call(cmd="machine", type="amiga500", os=True)
            without = s.call(cmd="load_file", file=self.plain, type="hunk")
            self.assertEqual(without["symbols"], {})
            with_ = s.call(cmd="load_file", file=self.named, type="hunk")
            self.assertGreaterEqual(len(with_["symbols"]), 15)
            self.assertIn("print_dec", with_["symbols"])
            self.assertIn("cmd_sum", with_["symbols"])
        finally:
            s.close()

    def test_analysis_finds_the_structure(self):
        s = Session()
        try:
            s.call(cmd="machine", type="amiga500", os=True)
            s.call(cmd="load_file", file=self.plain, type="hunk")
            a = s.call(cmd="analyze", entries=["0x020000"])
            self.assertTrue(a["ok"], a)
            self.assertEqual(a["jump_tables"], 1)
            self.assertEqual(a["routines"], 7)        # start, print, dispatch, print_dec and the three cases of the jump table
            tables = s.call(cmd="jump_tables")["tables"]
            self.assertEqual((tables[0]["kind"], tables[0]["entry_count"], tables[0]["followed"]), ("offset", 3, True))
            regions = s.call(cmd="data_regions", apply=True)
            kinds = sorted(r["kind"] for r in regions["regions"] if r.get("applied"))
            # four strings with a terminator, the table of numbers, and the scalar variables the code accesses with a fixed size:
            # dosbase and stdout (longwords), total (a word)
            self.assertEqual(kinds, ["longs"] * 2 + ["text"] * 4 + ["words"] * 2)
            s.call(cmd="analyze", entries=["0x020000"])
            listing = s.call(cmd="listing", verify=True)
            self.assertTrue(listing["verified"], listing.get("problem"))
            text = listing["listing"]
            self.assertIn('DC.B     "Hello from the MC68000 example",10,0', text)
            self.assertIn("jump table of the jump at", text)
            self.assertIn("DC.L     $00000000", text)                                # a longword variable of the BSS
            self.assertIn("DC.W     $000A,$0014,$001E,$0028,$0032,$0000", text)     # the table the loop walks through with ADD.W (A2)+,D4 (and its padding word)
        finally:
            s.close()

    def _server_with_example(self):
        s = annotate.M68kServer(M68KTOOL)
        s.ok(cmd="machine", type="amiga500", chip_kb=512, os=True)
        s.ok(cmd="load_file", file=self.plain, type="hunk", addr="0x20000")
        s.ok(cmd="analyze", entries=["0x020000"], reset=True)
        s.ok(cmd="checkpoint", name="baseline")
        tools = annotate.Tools(s)
        print_routine = next(r for r in s.ok(cmd="routines")["routines"] if r["addr"] == "0x02004A")
        tools.range = (print_routine["start"], print_routine["end"])
        return s, tools

    def test_run_routine_does_not_pretend_that_a_wild_jump_returned(self):
        """`print` calls Write through the library base in the variable dosbase. Run on its own that variable is 0: JSR -48(A6) jumps into empty
        memory. The report must not call that a return, must say what happened, and must not count the caller's own set-up as a change."""
        s, tools = self._server_with_example()
        try:
            text = tools.t_run_routine("0x02004A", registers={"A0": "0x030000"}, memory=[{"addr": "0x030000", "hex": "48692074686572650A00"}])
        finally:
            s.close()
        self.assertIn("returned: False", text)
        self.assertIn("did not return", text)
        self.assertIn("EMPTY memory (first at 0xFFFFD0)", text)
        self.assertIn("memory changed: nothing", text)          # the string the caller wrote is not a change of the routine
        self.assertNotIn("stop: returned", text)

    def test_run_routine_with_the_state_the_program_would_have(self):
        """The same routine, after the program entry has run as `setup` (it opens dos.library and stores the base and the output handle in the
        variables): a clean return, the text on the console, one Write; the setup's own output and library calls are not part of the report."""
        s, tools = self._server_with_example()
        try:
            text = tools.t_run_routine("0x02004A", registers={"A0": "0x030000"}, setup=[{"addr": "0x020000"}],
                                       memory=[{"addr": "0x030000", "hex": "48692074686572650A00"}])
        finally:
            s.close()
        self.assertIn("setup 0x020000: returned, 378 instructions", text)
        self.assertIn("stop: returned  returned: True", text)
        self.assertNotIn("EMPTY memory", text)
        self.assertEqual(text.count("dos.library -48 Write"), 1)                # the one Write of `print`; the seven of the entry are not counted
        self.assertNotIn("OpenLibrary", text)
        self.assertIn("text written to the console (dos Write): 'Hi there\\n'", text)
        self.assertNotIn("Hello from", text)
        self.assertIn("memory changed: nothing", text)

    def test_run_routine_reports_a_setup_that_does_not_return(self):
        s, tools = self._server_with_example()
        try:
            text = tools.t_run_routine("0x02004A", setup=[{"addr": "0x02004A", "registers": {"A0": "0x030000"}}], max_instructions=500)
            self.assertIn("setup 0x02004A: did NOT return", text)
            self.assertTrue(tools.dispatch("run_routine", {"addr": "0x02004A", "setup": [{"nope": 1}]}).startswith("error"))
        finally:
            s.close()

    def test_dis_of_a_hunk_file(self):
        """`m68ktool dis` puts a hunk file where load_file does ($20000), so that the absolute address 4 stays an address and is not a label in the
        code; it follows the jump table (the cases are code); an object file is refused with a hint."""
        r = subprocess.run([M68KTOOL, "dis", self.named], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout
        self.assertIn("020000  48E7 3F3E", out)
        self.assertIn("MOVEA.L  $0004.W,A6", out)
        self.assertNotIn("L_000004", out)
        self.assertIn("JMP      cmdbase(PC,D0.W)", out)
        self.assertIn("cmd_sum:", out)
        self.assertIn("ADD.W    (A2)+,D4", out)         # the body of a case of the table is code
        self.assertNotIn("DC.B     $70,$02", out)
        # --org still wins
        r2 = subprocess.run([M68KTOOL, "dis", self.named, "--org", "0x30000"], capture_output=True, text=True)
        self.assertIn("030000  48E7 3F3E", r2.stdout)
        obj = os.path.join(self.tmp.name, "object.o")
        with open(obj, "wb") as f:
            f.write(bytes([0, 0, 3, 0xE7, 0, 0, 0, 2, 0x61, 0x62]))
        r3 = subprocess.run([M68KTOOL, "dis", obj], capture_output=True, text=True)
        self.assertEqual(r3.returncode, 1)
        self.assertIn("HUNK_UNIT", r3.stderr)
        self.assertIn("vlink", r3.stderr)

    def test_dry_run_of_the_orchestrator_contacts_no_model(self):
        mock = MockOllama({})
        url = mock.start()
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                code = annotate.main(["--hunk", self.plain, "--dry-run", "--host", url, "--m68ktool", M68KTOOL])
        finally:
            mock.stop()
        text = out.getvalue()
        self.assertEqual(code, 0, text)
        self.assertEqual(mock.requests, [])
        self.assertIn("== dry run: no model is contacted ==", text)
        self.assertIn("7 routines, 7 still without documentation", text)
        self.assertIn("jump table (offset)", text)
        self.assertIn("library calls (JSR offset(A6)): -552 x1, -414 x1, -60 x1, -48 x4", text)
        self.assertIn("without --dry-run", text)
        self.assertNotIn("WARNING", text)

    def test_dry_run_warns_when_the_context_is_too_small(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = annotate.main(["--hunk", self.plain, "--dry-run", "--num-ctx", "4096", "--m68ktool", M68KTOOL])
        self.assertEqual(code, 0)
        self.assertIn("WARNING: the largest routine", out.getvalue())


if __name__ == "__main__":
    unittest.main()
