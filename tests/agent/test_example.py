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
