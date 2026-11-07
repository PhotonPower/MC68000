"""Self-extracting programs: run until the program jumps into the memory it wrote, dump it, analyse it; and the way from a disk image to there.

The program under test is built here: a payload (a small program that prints a message, assembled for $30000), packed with a small LZ packer
(tests/agent/unpack/lzpack.py) and put behind a decruncher written in 68000 assembler (sfx_onestage.s; sfx_twostage.s copies the decruncher to $50000 first).

Run:  M68KTOOL=build/m68ktool python3 -m unittest discover -s tests/agent -v
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "unpack"))
import lzpack  # noqa: E402

M68KTOOL = os.environ.get("M68KTOOL") or os.path.join(ROOT, "build", "m68ktool")
DRIVE = os.path.join(ROOT, "agent", "m68k_drive.py")
UNPACK = os.path.join(HERE, "unpack")


class Session:
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


def run(*cmd):
    return subprocess.run(list(cmd), capture_output=True, text=True)


class UnpackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(M68KTOOL):
            raise unittest.SkipTest(f"m68ktool not found at {M68KTOOL} (set M68KTOOL)")
        cls.tmp = tempfile.TemporaryDirectory()
        t = cls.tmp.name
        r = run(M68KTOOL, "asm", os.path.join(UNPACK, "payload.s"), "-o", os.path.join(t, "payload.bin"))
        assert r.returncode == 0, r.stdout + r.stderr
        cls.payload = open(os.path.join(t, "payload.bin"), "rb").read()
        packed = lzpack.pack(cls.payload)
        assert lzpack.unpack(packed) == cls.payload
        cls.packed_size = len(packed)
        lines = ["        DC.B    " + ",".join(f"${b:02X}" for b in packed[k:k + 16]) for k in range(0, len(packed), 16)]
        for name in ("onestage", "twostage"):
            src = os.path.join(t, f"{name}.s")
            with open(src, "w") as f:
                f.write(open(os.path.join(UNPACK, f"sfx_{name}.s")).read() + "\n".join(lines) + "\n")
            r = run(M68KTOOL, "asm", src, "--hunk", os.path.join(t, name))
            assert r.returncode == 0, r.stdout + r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def hunk(self, name):
        return os.path.join(self.tmp.name, name)

    def start(self, name):
        s = Session()
        self.assertTrue(s.call(cmd="machine", type="amiga500", os=True)["ok"])
        self.assertTrue(s.call(cmd="load_file", file=self.hunk(name), type="hunk")["ok"])
        return s

    def test_the_packed_program_is_small(self):
        self.assertLess(self.packed_size, len(self.payload) // 10)

    def test_one_stage(self):
        s = self.start("onestage")
        try:
            r = s.call(cmd="run_until_unpacked")
            self.assertTrue(r["ok"], r)
            self.assertEqual(r["stop"], "unpacked")
            self.assertEqual(r["pc"], "0x030000")                                  # the entry of the unpacked program
            self.assertEqual(r["region"]["start"], "0x030000")
            self.assertEqual(r["region"]["size"], len(self.payload))               # what the decruncher wrote is exactly the payload
            out = os.path.join(self.tmp.name, "one.bin")
            d = s.call(cmd="save_memory", addr="0x30000", len=len(self.payload), file=out)
            self.assertTrue(d["ok"], d)
            self.assertEqual(open(out, "rb").read(), self.payload)                  # byte for byte the original
            # it goes on: the unpacked program prints its message and returns to the system
            r = s.call(cmd="run")
            self.assertEqual(r["stop"], "exit")
            self.assertEqual(s.call(cmd="console")["text"], "Unpacked and running\n")
        finally:
            s.close()

    def test_two_stages(self):
        """The decruncher copies itself to $50000 first: a small threshold stops at that copy, `continue` goes on to the unpacked program, the
        default threshold (4096 bytes written) skips the copy by itself."""
        s = self.start("twostage")
        try:
            first = s.call(cmd="run_until_unpacked", min_written=16)
            self.assertEqual(first["stop"], "unpacked")
            self.assertEqual(first["pc"], "0x050000")                              # the copy of the decruncher
            self.assertLess(first["region"]["size"], 200)
            second = s.call(cmd="run_until_unpacked", min_written=16, **{"continue": True})
            self.assertEqual(second["stop"], "unpacked")
            self.assertEqual(second["pc"], "0x030000")
            self.assertEqual(second["region"]["size"], len(self.payload))
            self.assertEqual(s.call(cmd="run")["stop"], "exit")
        finally:
            s.close()
        s = self.start("twostage")
        try:
            r = s.call(cmd="run_until_unpacked")
            self.assertEqual((r["stop"], r["pc"]), ("unpacked", "0x030000"))
        finally:
            s.close()

    def test_the_unpacked_program_is_analysed(self):
        """Dump, load as a raw image at its address, analyse: the routines, the string, a listing that assembles to the same bytes."""
        s = self.start("onestage")
        try:
            self.assertEqual(s.call(cmd="run_until_unpacked")["stop"], "unpacked")
            out = os.path.join(self.tmp.name, "unpacked.bin")
            self.assertTrue(s.call(cmd="save_memory", addr="0x30000", len=len(self.payload), file=out)["ok"])
        finally:
            s.close()
        work = os.path.join(self.tmp.name, "work_unpacked")
        r = run(sys.executable, DRIVE, "--work", work, "init", "--fresh", "--bin", out, "--org", "0x30000", "--entry", "0x30000", "--m68ktool", M68KTOOL)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("2 routines", r.stdout)                                     # the entry and `print`
        status = run(sys.executable, DRIVE, "--work", work, "status").stdout
        self.assertIn("0x030000", status)
        listing = os.path.join(self.tmp.name, "unpacked.s")
        r = run(sys.executable, DRIVE, "--work", work, "listing", "--out", listing)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("VERIFIED", r.stdout)
        text = open(listing).read()
        self.assertIn('DC.B     "dos.library",0', text)
        self.assertIn('DC.B     "Unpacked and running",10,0', text)

    @unittest.skipUnless(shutil.which("xdftool"), "xdftool (pip install amitools) writes the disk image")
    def test_from_a_disk_image(self):
        """A file called `1` on an ADF disk: list, extract, unpack, compare."""
        adf = os.path.join(self.tmp.name, "game.adf")
        for step in (["create"], ["format", "Game", "ofs"], ["write", self.hunk("onestage"), "1"]):
            self.assertEqual(run("xdftool", adf, *step).returncode, 0)
        info = run(M68KTOOL, "adf", "info", adf)
        self.assertIn("DOS0 (OFS)", info.stdout)
        self.assertIn("disk name: \"Game\"", info.stdout)
        ls = run(M68KTOOL, "adf", "ls", adf)
        self.assertIn("  1\n", ls.stdout)
        extracted = os.path.join(self.tmp.name, "extracted_1")
        r = run(M68KTOOL, "adf", "extract", adf, "1", "-o", extracted)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(open(extracted, "rb").read(), open(self.hunk("onestage"), "rb").read())
        s = Session()
        try:
            s.call(cmd="machine", type="amiga500", os=True)
            self.assertTrue(s.call(cmd="load_file", file=extracted, type="hunk")["ok"])
            r = s.call(cmd="run_until_unpacked")
            self.assertEqual((r["stop"], r["pc"]), ("unpacked", "0x030000"))
        finally:
            s.close()


if __name__ == "__main__":
    unittest.main()
