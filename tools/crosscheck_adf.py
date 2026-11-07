#!/usr/bin/env python3
"""Compares `m68ktool adf` with disks written by amitools' xdftool (an independent implementation of the AmigaDOS file system).

    pip install amitools
    python3 tools/crosscheck_adf.py build/m68ktool [disks]

Per disk (random OFS or FFS, random names, sizes from 0 to 120 KB, so that extension blocks are needed, subdirectories): `adf ls -R` must list exactly the
files and directories xdftool wrote (with their sizes), and `adf extract` must give back every file byte for byte. Exit code 0 if everything matches.
"""
import os
import random
import subprocess
import sys
import tempfile


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    tool = sys.argv[1] if len(sys.argv) > 1 else "build/m68ktool"
    disks = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    if run(["xdftool", "--help"]).returncode not in (0, 1, 2):
        print("xdftool (pip install amitools) is needed")
        return 2
    rng = random.Random(20261107)
    alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    bad = 0
    files_checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        for d in range(disks):
            fs = rng.choice(["ofs", "ffs"])
            adf = os.path.join(tmp, f"d{d}.adf")
            assert run(["xdftool", adf, "create"]).returncode == 0
            assert run(["xdftool", adf, "format", f"Disk{d}", fs]).returncode == 0
            expected = {}      # path -> bytes (None for a directory)
            dirs = [""]
            for _ in range(rng.randint(1, 12)):
                parent = rng.choice(dirs)
                name = "".join(rng.choice(alphabet) for _ in range(rng.randint(1, 20)))
                path = (parent + "/" if parent else "") + name
                if path.upper() in {p.upper() for p in expected}:
                    continue
                if rng.random() < 0.2 and len(dirs) < 4:
                    if run(["xdftool", adf, "makedir", path]).returncode == 0:
                        expected[path] = None
                        dirs.append(path)
                    continue
                size = rng.choice([0, 1, 487, 488, 489, 511, 512, 513, rng.randint(0, 3000), rng.randint(30000, 120000)])
                content = bytes(rng.randrange(256) for _ in range(size))
                src = os.path.join(tmp, "src.bin")
                with open(src, "wb") as f:
                    f.write(content)
                if run(["xdftool", adf, "write", src, path]).returncode == 0:
                    expected[path] = content
            r = run([tool, "adf", "ls", adf, "-R"])
            if r.returncode != 0:
                print(f"disk {d} ({fs}): adf ls failed: {r.stderr.strip()}")
                bad += 1
                continue
            listed = {}
            for line in r.stdout.splitlines():
                size, _, path = line.strip().partition("  ")
                listed[path] = None if size == "<dir>" else int(size)
            want = {p: (None if c is None else len(c)) for p, c in expected.items()}
            if listed != want:
                print(f"disk {d} ({fs}): listing differs\n  ours:      {sorted(listed.items())}\n  xdftool:   {sorted(want.items())}")
                bad += 1
                continue
            for path, content in expected.items():
                if content is None:
                    continue
                out = os.path.join(tmp, "out.bin")
                r = run([tool, "adf", "extract", adf, path, "-o", out])
                files_checked += 1
                if r.returncode != 0 or open(out, "rb").read() != content:
                    print(f"disk {d} ({fs}): {path}: extraction differs ({r.stderr.strip()})")
                    bad += 1
    print(f"{disks} disks, {files_checked} files compared, {bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
