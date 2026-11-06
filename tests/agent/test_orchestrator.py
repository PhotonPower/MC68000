"""End-to-end test of agent/m68k_annotate.py against a scripted mock of Ollama and the real m68ktool.

Run:  M68KTOOL=build/m68ktool python3 -m unittest discover -s tests/agent -v
"""
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mock_ollama import MockOllama  # noqa: E402

spec = importlib.util.spec_from_file_location("m68k_annotate", os.path.join(ROOT, "agent", "m68k_annotate.py"))
annotate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(annotate)
annotate.Ollama.retry_delay = 0  # never wait between retries in tests

M68KTOOL = os.environ.get("M68KTOOL") or os.path.join(ROOT, "build", "m68ktool")

DEMO = """\
        ORG $1000
start:  LEA msg(PC),A0
        LEA $2200,A1
        BSR print
        LEA sprite(PC),A0
        LEA $2300,A1
        BSR copy8
        MOVE.W #$0F00,$DFF180.L
        ADDQ.W #1,counter
        RTS
print:  MOVE.B (A0)+,D0
        BEQ.S pdone
        MOVE.B D0,(A1)+
        BRA.S print
pdone:  RTS
copy8:  MOVEQ #7,D1
c1:     MOVE.B (A0)+,(A1)+
        DBRA D1,c1
        RTS
msg:    DC.B "HELLO",0
sprite: DC.B $FF,$81,$BD,$A5,$A5,$BD,$81,$FF
counter: DC.W 0
"""


def build_demo(directory):
    """Assembles the demo program; returns (binary path, {symbol: address})."""
    src = os.path.join(directory, "demo.s")
    out = os.path.join(directory, "demo.bin")
    sym = os.path.join(directory, "demo.sym")
    with open(src, "w") as f:
        f.write(DEMO)
    r = subprocess.run([M68KTOOL, "asm", src, "-o", out, "-s", sym], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    symbols = {}
    with open(sym) as f:
        for line in f:
            parts = line.replace("=", " ").split()
            if len(parts) >= 2:
                symbols[parts[0]] = int(parts[1].lstrip("$"), 16)
    return out, symbols


def plans_for(sym):
    h = lambda n: f"0x{sym[n]:06x}"  # noqa: E731
    a = lambda n: f"0x{sym[n]:06X}"  # noqa: E731
    return {
        h("print"): [
            [("run_routine", {"addr": a("print"), "registers": {"A0": a("msg"), "A1": "0x2200"}})],
            [("document_routine", {"addr": a("print"), "name": "D0", "summary": "bad name on purpose"})],  # rejected: a register name
            [("document_routine", {"addr": a("print"), "name": "print_string", "summary": "Copies a zero terminated string.",
                                   "inputs": "A0 = text, A1 = destination", "outputs": "A1 behind the copy", "clobbers": "D0 A0 A1",
                                   "confidence": "high"}),
             ("name_label", {"addr": a("pdone"), "name": "print_done"}),
             ("add_comment", {"addr": a("print"), "text": "fetch the next character"}),
             ("add_comment", {"addr": f"0x{sym['print'] + 4:06X}", "text": "store it"})],
            [("finish_routine", {"summary": "done"})],
            [("finish_routine", {})],
        ],
        h("copy8"): [
            [("run_routine", {"addr": a("copy8"), "registers": {"A0": a("sprite"), "A1": "0x2300"}})],
            [("name_label", {"addr": a("c1"), "name": "copy_loop"}),
             ("document_routine", {"addr": a("copy8"), "name": "copy_sprite_row", "summary": "Copies 8 bytes.",
                                   "inputs": "A0 = source, A1 = destination", "clobbers": "D1 A0 A1", "confidence": "medium"}),
             ("add_comment", {"addr": a("copy8"), "text": "8 bytes, the counter counts 7..0"}),
             ("add_comment", {"addr": a("c1"), "text": "one byte per pass"})],
            [("finish_routine", {})],
            [("finish_routine", {})],
        ],
        h("start"): [
            [("run_routine", {"addr": a("start"), "max_instructions": 100000})],
            [("name_variable", {"addr": a("counter"), "name": "run_counter", "size": 2, "comment": "how often the demo ran"}),
             ("name_variable", {"addr": "0x2200", "name": "text_buffer", "size": 6, "comment": "copy of the greeting"}),
             ("name_variable", {"addr": "0x2300", "name": "sprite_buffer", "size": 8}),
             ("mark_data", {"addr": a("msg"), "length": 6, "kind": "text", "name": "greeting"}),
             ("mark_data", {"addr": a("sprite"), "length": 8, "kind": "bytes", "name": "face_sprite"}),
             ("add_comment", {"addr": a("start"), "text": "program entry"}),
             ("add_comment", {"addr": f"0x{sym['start'] + 8:06X}", "text": "copy the text"}),
             ("document_routine", {"addr": a("start"), "name": "show_greeting_demo", "summary": "Copies a text and a sprite, sets the colour."})],
            [("finish_routine", {})],
            [("finish_routine", {})],
        ],
    }


def run_main(argv):
    """Runs the orchestrator; returns (exit code, printed text)."""
    buf = io.StringIO()
    with redirect_stdout(buf):
        code = annotate.main(argv)
    return code, buf.getvalue()


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class OrchestratorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M68KTOOL):
            raise unittest.SkipTest(f"m68ktool not found at {M68KTOOL} (set M68KTOOL)")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.binary, self.sym = build_demo(self.dir)
        self.project = os.path.join(self.dir, "demo.proj.json")
        self.out = os.path.join(self.dir, "demo_annotated.s")
        self.mocks = []

    def tearDown(self):
        for m in self.mocks:
            m.stop()
        self.tmp.cleanup()

    def mock(self, **kw):
        plans = kw.pop("plans", plans_for(self.sym))
        m = MockOllama(plans, **kw)
        self.url = m.start()
        self.mocks.append(m)
        return m

    def argv(self, *extra, model="mock"):
        return ["--bin", self.binary, "--org", "0x1000", "--machine", "amiga500", "--host", self.url, "--model", model,
                "--project", self.project, "--out", self.out, "--m68ktool", M68KTOOL, "--max-turns", "10", *extra]

    # ------------------------------------------------------------------------------------------------------------------------------
    def test_full_run_is_verified_and_annotated(self):
        mock = self.mock()
        code, text = run_main(self.argv())
        self.assertEqual(code, 0, text)
        self.assertIn("VERIFIED", text)
        listing = read_text(self.out)
        for name in ("print_string", "copy_sprite_row", "show_greeting_demo", "print_done", "copy_loop", "run_counter", "text_buffer",
                     "greeting", "face_sprite"):
            self.assertIn(name, listing)
        self.assertIn("COLOR00", listing)                  # a custom chip register, by name
        self.assertIn("; fetch the next character", listing)
        self.assertIn("confidence: medium", listing)
        self.assertIn("VERIFIED", listing.splitlines()[1] if listing.startswith("; ") else listing)
        # the leaves come first: the two subroutines before the routine that calls them
        order = []
        for m in mock.requests_by_model["mock"]:
            a = m.split("ROUTINE_ADDR=")[1].split()[0].lower()
            if not order or order[-1] != a:
                order.append(a)
        self.assertEqual(order, [f"0x{self.sym['print']:06x}", f"0x{self.sym['copy8']:06x}", f"0x{self.sym['start']:06x}"])
        # the caller's task shows the callees by their real names
        start_prompt = next(m for m in mock.requests_by_model["mock"] if f"ROUTINE_ADDR=0x{self.sym['start']:06X}" in m)
        self.assertIn("print_string", start_prompt)
        self.assertIn("copy_sprite_row", start_prompt)
        # a rejected name came back as an error the model could read
        self.assertTrue(any("reserved" in e for e in mock.tool_errors), mock.tool_errors)
        # run_routine reported memory, hardware writes and registers
        results = "\n".join(mock.tool_results)
        self.assertIn("registers after:", results)
        self.assertIn("memory changed", results)
        self.assertIn("COLOR00", results)
        self.assertIn("returned: True", results)
        # the project file holds the annotations
        proj = json.loads(read_text(self.project))
        self.assertEqual(len(proj["routines"]), 3)
        self.assertEqual({r["name"] for r in proj["routines"]}, {"print_string", "copy_sprite_row", "show_greeting_demo"})

    def test_resume_skips_documented_routines(self):
        self.mock()
        code, _ = run_main(self.argv())
        self.assertEqual(code, 0)
        mock2 = self.mock()
        code, text = run_main(self.argv())
        self.assertEqual(code, 0, text)
        self.assertEqual(mock2.requests, [])           # everything was documented already
        self.assertIn("project loaded", text)

    def test_sloppy_tool_calls(self):
        sym = self.sym
        plans = plans_for(sym)
        h = f"0x{sym['print']:06x}"
        # calls written as text, arguments as a string, numbers as strings, unknown arguments, a wrong tool name
        plans[h] = [
            {"as_text": [("run_routine", {"addr": f"0x{sym['print']:06X}", "registers": {"A0": f"0x{sym['msg']:06X}", "A1": "0x2200"}})]},
            {"json_string": [("document_routine", {"addr": f"0x{sym['print']:06X}", "name": "print_string", "summary": "s", "bogus": 1}),
                             ("name_label", {"addr": f"0x{sym['pdone']:06X}", "name": "print_done"})]},
            [("Functions.add_comment()", {"addr": f"0x{sym['print']:06X}", "text": "a"}),
             ("add_comment", {"addr": f"0x{sym['print'] + 4:06X}", "text": "b"}),
             ("no_such_tool", {})],
            [("finish_routine", {})],
        ]
        mock = self.mock(plans=plans)
        code, text = run_main(self.argv("--only", h))
        self.assertEqual(code, 0, text)
        results = "\n".join(mock.tool_results)
        self.assertIn("ignored unknown argument 'bogus'", results)
        self.assertIn("unknown tool 'no_such_tool'", results)
        self.assertIn("print_string", read_text(self.out))

    def test_incomplete_routine_is_sent_back_once(self):
        sym = self.sym
        plans = plans_for(sym)
        h = f"0x{sym['print']:06x}"
        plans[h] = [
            [("document_routine", {"addr": f"0x{sym['print']:06X}", "name": "print_string", "summary": "s"})],
            [("finish_routine", {})],     # labels and comments are missing: sent back
            [("name_label", {"addr": f"0x{sym['pdone']:06X}", "name": "print_done"}),
             ("add_comment", {"addr": f"0x{sym['print']:06X}", "text": "a"}),
             ("add_comment", {"addr": f"0x{sym['print'] + 4:06X}", "text": "b"})],
            [("finish_routine", {})],
        ]
        mock = self.mock(plans=plans)
        run_main(self.argv("--only", h))
        results = "\n".join(mock.tool_results)
        self.assertIn("not finished yet", results)
        self.assertIn("labels without a real name", results)
        self.assertIn("comment(s)", results)
        self.assertIn("print_done", read_text(self.out))               # the second finish_routine ended the routine

    def test_finish_before_documenting_is_refused(self):
        sym = self.sym
        plans = plans_for(sym)
        h = f"0x{sym['copy8']:06x}"
        plans[h] = [[("finish_routine", {})], [("finish_routine", {})]]
        mock = self.mock(plans=plans)
        code, text = run_main(self.argv("--only", h))
        self.assertTrue(any("document_routine" in e for e in mock.tool_errors))
        self.assertIn("failed", text)

    def test_invalid_tool_call_from_ollama_gets_a_hint(self):
        mock = self.mock(invalid_json_first=2)
        code, text = run_main(self.argv())
        self.assertEqual(code, 0, text)
        self.assertEqual(mock.invalid_json_served, 2)
        self.assertIn("invalid tool call", text)
        self.assertTrue(mock.json_hints)

    def test_cut_off_answers_raise_the_limit(self):
        mock = self.mock(truncate_first=2)
        code, text = run_main(self.argv("--max-tokens", "1000"))
        self.assertEqual(code, 0, text)
        self.assertIn("cut off", text)
        self.assertTrue(mock.hints)
        self.assertGreater(max(o["num_predict"] for o in mock.options_seen), 1000)

    def test_model_without_thinking_is_asked_again(self):
        mock = self.mock(reject_think=True)
        code, text = run_main(self.argv("--think", "true"))
        self.assertEqual(code, 0, text)
        self.assertIn("rejected", mock.think_values)

    def test_one_failing_routine_does_not_end_the_run(self):
        # the second routine fails (a failure before the first answer would be a configuration problem)
        mock = self.mock(crash_on_address=f"0x{self.sym['copy8']:06x}")
        code, text = run_main(self.argv())
        self.assertIn("model error on", text)
        self.assertIn("skipping this routine", text)
        proj = json.loads(read_text(self.project))
        self.assertEqual({r["name"] for r in proj["routines"]}, {"print_string", "show_greeting_demo"})
        self.assertIn("gaps left: 1 of 3", text)
        self.assertEqual(code, 0)   # the listing is still verified, only less annotated

    def test_configuration_errors(self):
        self.mock(capabilities_by_model={"nano": ["completion"]})
        code, _ = run_main(self.argv(model="nano"))
        self.assertEqual(code, 2)                      # no tool support
        self.mock(fail_with=404)
        code, _ = run_main(self.argv())
        self.assertEqual(code, 2)                      # model not found
        self.url = "http://127.0.0.1:9"
        code, _ = run_main(self.argv())
        self.assertEqual(code, 2)                      # nothing listens there

    def test_refinement_pass_only_gets_routines_with_gaps(self):
        plans = plans_for(self.sym)
        del plans[f"0x{self.sym['copy8']:06x}"]        # the fast model gives up on copy8
        refine = {f"0x{self.sym['copy8']:06x}": plans_for(self.sym)[f"0x{self.sym['copy8']:06x}"]}
        mock = self.mock(plans=plans, plans_by_model={"slow": refine})
        report = os.path.join(self.dir, "report.md")
        code, text = run_main(self.argv("--refine-model", "slow", "--report", report))
        self.assertEqual(code, 0, text)
        self.assertIn("== pass 2/2 (refine): model slow", text)
        asked = sorted({m.split("ROUTINE_ADDR=")[1].split()[0].lower() for m in mock.requests_by_model["slow"]})
        self.assertEqual(asked, [f"0x{self.sym['copy8']:06x}"])
        self.assertIn("Known gaps to close", mock.requests_by_model["slow"][0])
        self.assertIn("REFINEMENT PASS", "\n".join(mock.system_prompts))
        self.assertIn("copy_sprite_row", read_text(self.out))
        self.assertIn("0 with gaps", read_text(report))

    def test_other_load_formats(self):
        self.mock()
        # a raw binary needs no project file; --only works with decimal and hex addresses
        code, text = run_main(["--bin", self.binary, "--org", "0x1000", "--host", self.url, "--model", "mock", "--m68ktool", M68KTOOL,
                               "--machine", "generic", "--only", str(self.sym["print"]), "--max-turns", "10"])
        self.assertEqual(code, 0, text)


LIBDEMO = """\
        ORG $1000
start:  MOVEA.L 4.W,A6
        LEA gfx(PC),A1
        MOVEQ #0,D0
        JSR -552(A6)
        MOVEA.L D0,A6
        JSR -222(A6)
        MOVEA.L 4.W,A6
        MOVEQ #20,D0
        MOVEQ #0,D1
        JSR -198(A6)
        RTS
gfx:    DC.B "graphics.library",0
"""


class LibraryCallReportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M68KTOOL):
            raise unittest.SkipTest(f"m68ktool not found at {M68KTOOL} (set M68KTOOL)")

    def test_run_routine_reports_library_calls(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "lib.s")
            binary = os.path.join(d, "lib.bin")
            with open(src, "w") as f:
                f.write(LIBDEMO)
            r = subprocess.run([M68KTOOL, "asm", src, "-o", binary], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            plans = {"0x001000": [[("run_routine", {"addr": "0x001000"})]]}
            mock = MockOllama(plans)
            url = mock.start()
            try:
                code, text = run_main(["--bin", binary, "--org", "0x1000", "--host", url, "--model", "mock", "--m68ktool", M68KTOOL,
                                       "--max-turns", "2", "--only", "0x1000"])
            finally:
                mock.stop()
            results = "\n".join(mock.tool_results)
            self.assertIn("library calls made: 3", results)
            self.assertIn("exec.library -552 OpenLibrary", results)
            self.assertIn("[graphics.library]", results)
            self.assertIn("(unknown function at offset -222) [NOT emulated", results)
            self.assertIn("exec.library -198 AllocMem", results)
            self.assertIn("NOTE: calls that are not emulated", results)
            self.assertIn("NOT emulated", "\n".join(mock.system_prompts))


class HelperTest(unittest.TestCase):
    def test_argument_coercion(self):
        c, notes = annotate.prepare_args("run_routine", {"addr": "0x1000", "max_instructions": "500", "keep_state": "true", "x": 1,
                                                          "registers": '{"D0": 1}'})
        self.assertEqual(c["max_instructions"], 500)
        self.assertIs(c["keep_state"], True)
        self.assertEqual(c["registers"], {"D0": 1})
        self.assertTrue(notes)
        c, _ = annotate.prepare_args("read_memory", {"addr": 4096, "length": "$20"})
        self.assertEqual(c["addr"], 4096)       # a JSON number is a decimal address
        self.assertEqual(c["length"], 32)
        with self.assertRaises(ValueError):
            annotate.prepare_args("read_memory", {})
        with self.assertRaises(ValueError):
            annotate.prepare_args("run_routine", {"addr": "x", "keep_state": "maybe"})
        with self.assertRaises(ValueError):
            annotate.prepare_args("nope", {})

    def test_address_normalisation(self):
        n = annotate.norm_addr
        self.assertEqual(n("0x1000"), "0x1000")
        self.assertEqual(n("001000"), "0x001000")
        self.assertEqual(n("DFF180"), "0xDFF180")
        self.assertEqual(n("main"), "main")
        self.assertEqual(n("4096"), "4096")      # plain decimal stays decimal
        self.assertEqual(n(4096), 4096)
        with self.assertRaises(ValueError):
            n("")

    def test_tool_names_and_text_calls(self):
        self.assertEqual(annotate.clean_tool_name("functions.run_routine"), "run_routine")
        self.assertEqual(annotate.clean_tool_name("Run_Routine()"), "run_routine")
        calls = annotate.calls_from_content('x <tool_call>{"name": "a", "arguments": {"b": 1}}</tool_call>')
        self.assertEqual(calls[0]["function"]["name"], "a")
        self.assertEqual(annotate.calls_from_content("```json\n{\"name\": \"a\", \"arguments\": {}}\n```")[0]["function"]["arguments"], {})
        self.assertEqual(annotate.calls_from_content("no call here"), [])

    def test_compaction_keeps_the_task(self):
        msgs = [{"role": "system", "content": "s"}, {"role": "user", "content": "task"}]
        msgs += [{"role": "tool", "content": "x" * 5000} for _ in range(10)]
        annotate.compact_messages(msgs, max_chars=10000)
        self.assertEqual(msgs[1]["content"], "task")
        self.assertIn("removed to save context", msgs[2]["content"])
        self.assertEqual(len(msgs[-1]["content"]), 5000)   # the newest results stay intact

    def test_vector_names(self):
        self.assertIn("address error", annotate.vector_name(3))
        self.assertEqual(annotate.vector_name(35), "TRAP #3")


if __name__ == "__main__":
    unittest.main()
