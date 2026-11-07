"""agent/m68k_drive.py: the annotation tools driven by hand, one command per process, the state in a work directory.

Run:  M68KTOOL=build/m68ktool python3 -m unittest discover -s tests/agent -v
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
M68KTOOL = os.environ.get("M68KTOOL") or os.path.join(ROOT, "build", "m68ktool")
DRIVE = os.path.join(ROOT, "agent", "m68k_drive.py")
SOURCE = os.path.join(ROOT, "examples", "amiga", "hello_amiga.s")


class DriveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M68KTOOL):
            raise unittest.SkipTest(f"m68ktool not found at {M68KTOOL} (set M68KTOOL)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.hunk = os.path.join(cls.tmp.name, "hello_amiga")
        r = subprocess.run([M68KTOOL, "asm", SOURCE, "--hunk", cls.hunk], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        cls.work = os.path.join(cls.tmp.name, "work")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def drive(self, *args, work=None, expect=0):
        r = subprocess.run([sys.executable, DRIVE, "--work", work or self.work, *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, expect, f"{args}: {r.stdout}\n{r.stderr}")
        return r.stdout, r.stderr

    def test_a_whole_session(self):
        """init -> status -> show -> run_routine (with setup) -> annotate -> finish -> listing: what a model does in a loop, one command per process."""
        out, _ = self.drive("init", "--fresh", "--hunk", self.hunk, "--m68ktool", M68KTOOL, "--lang", "Deutsch")
        self.assertIn("7 routines", out)
        out, _ = self.drive("status")
        self.assertIn("7 routines, processing order", out)
        self.assertIn("0 of 7 finished.", out)
        self.assertIn("Next: `show 0x02004A`", out)

        out, _ = self.drive("show", "0x02004A")
        self.assertIn("ROUTINE_ADDR=0x02004A", out)
        self.assertIn("library call JSR -48(A6)", out)
        self.assertIn("MOVEM.L  D0-D3/A0-A1/A6,-(A7)", out)
        out, _ = self.drive("show", "0x02004A", "--system")
        self.assertIn("expert MC68000 and Commodore Amiga reverse engineer", out)

        # isolated, the routine runs into empty memory; with the program entry as setup it works
        out, _ = self.drive("tool", "run_routine", json.dumps({"addr": "0x02004A", "registers": {"A0": "0x030000"},
                                                               "memory": [{"addr": "0x030000", "hex": "48692074686572650A00"}]}))
        self.assertIn("returned: False", out)
        self.assertIn("EMPTY memory", out)
        out, _ = self.drive("tool", "run_routine", json.dumps({"addr": "0x02004A", "registers": {"A0": "0x030000"}, "setup": [{"addr": "0x020000"}],
                                                               "memory": [{"addr": "0x030000", "hex": "48692074686572650A00"}]}))
        self.assertIn("stop: returned  returned: True", out)
        self.assertIn("'Hi there\\n'", out)

        # annotations survive the end of the process
        self.drive("tool", "document_routine", json.dumps({"addr": "0x02004A", "name": "print_string", "summary": "Writes a zero terminated string to the output.",
                                                           "inputs": "A0 = text", "confidence": "high"}))
        # the completeness check sends the model back once (placeholder labels, numbered variables, no comments) ...
        out, _ = self.drive("finish", "0x02004A")
        self.assertIn("not finished yet", out)
        self.assertIn("labels without a real name", out)
        self.assertIn("memory locations used by number", out)
        with open(os.path.join(self.work, "state.json")) as f:
            self.assertEqual(json.load(f)["warned"], ["0x02004A"])
        for name, addr in (("find_end", "0x020052"), ("do_write", "0x02005A")):
            self.drive("tool", "name_label", json.dumps({"addr": addr, "name": name}))
        self.drive("tool", "name_variable", json.dumps({"addr": "0x020130", "name": "dosbase", "size": 4}))
        self.drive("tool", "name_variable", json.dumps({"addr": "0x020134", "name": "stdout", "size": 4}))
        self.drive("tool", "add_comment", json.dumps({"addr": "0x02004E", "text": "remember the start of the text"}))
        self.drive("tool", "add_comment", json.dumps({"addr": "0x020056", "text": "count the length"}))
        # ... and the second call is accepted (the memory of the first call is in the work directory)
        out, _ = self.drive("finish", "0x02004A")
        self.assertIn("ok: routine finished", out)
        out, _ = self.drive("status")
        self.assertIn("1 of 7 finished.", out)
        self.assertIn("print_string", out)
        self.assertIn("finished, confidence high", out)
        self.assertIn("Next: `show 0x020070`", out)
        out, _ = self.drive("report")
        self.assertIn("6 of 7 routines are not finished or still have gaps.", out)

        out, _ = self.drive("listing", "--out", os.path.join(self.tmp.name, "out.s"))
        self.assertIn("VERIFIED", out)
        with open(os.path.join(self.tmp.name, "out.s")) as f:
            text = f.read()
        self.assertIn("print_string:", text)
        self.assertIn("find_end:", text)
        self.assertIn("dosbase", text)
        self.assertIn("; remember the start of the text", text)
        self.assertIn("VERIFIED", text.splitlines()[1])

    def test_errors_are_plain(self):
        work = os.path.join(self.tmp.name, "empty")
        _, err = self.drive("status", work=work, expect=1)
        self.assertIn("run `init` first", err)
        _, err = self.drive("init", work=os.path.join(self.tmp.name, "w2"), expect=1)
        self.assertIn("init needs the program", err)
        _, err = self.drive("init", "--hunk", self.hunk, "--project", "x.json", work=os.path.join(self.tmp.name, "w3"), expect=1)
        self.assertIn("--project is set by the work directory", err)
        r = subprocess.run([sys.executable, DRIVE, "--work", os.path.join(self.tmp.name, "w4"), "init", "--hunk", self.hunk, "--bogus"], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)                                   # an option nobody knows is refused right away
        # a work directory that is set up: bad calls
        out, _ = self.drive("init", "--fresh", "--hunk", self.hunk, "--m68ktool", M68KTOOL, work=os.path.join(self.tmp.name, "w5"))
        w5 = os.path.join(self.tmp.name, "w5")
        _, err = self.drive("show", "0x999999", work=w5, expect=1)
        self.assertIn("no routine at 0x999999", err)
        _, err = self.drive("tool", "run_routine", "{not json", work=w5, expect=1)
        self.assertIn("not valid JSON", err)
        out, _ = self.drive("tool", "no_such_tool", "{}", work=w5, expect=1)
        self.assertIn("unknown tool", out)
        _, err = self.drive("tool", "finish_routine", "{}", work=w5, expect=1)
        self.assertIn("needs the routine", err)
        out, _ = self.drive("tool", "document_routine", json.dumps({"addr": "0x02004A", "name": "D0", "summary": "x"}), work=w5, expect=1)
        self.assertIn("error", out)                                           # a register name is not a name
        out, _ = self.drive("finish", "0x02004A", work=w5, expect=1)
        self.assertIn("document_routine for this routine first", out)         # not documented yet


if __name__ == "__main__":
    unittest.main()
