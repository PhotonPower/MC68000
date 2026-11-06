#!/usr/bin/env python3
"""Cross-checks the decoder against GNU objdump (an independent MC68000 disassembler).

    cmake -S . -B build -DM68K_BUILD_TOOLS=ON && cmake --build build
    python3 tools/crosscheck_objdump.py build/m68k_dump_forms [pattern ...]

Needs binutils for m68k (Debian/Ubuntu: apt install binutils-m68k-linux-gnu).
For each of the 65536 opcode words, validity, length and mnemonic are compared. Differences that
are intended (see the list in KNOWN) are counted separately; everything else is a failure.
"""
import re
import subprocess
import sys

PATTERNS = [
    [0x0010, 0x0020, 0x0030, 0x0040],
    [0x0102, 0x1234, 0x5678, 0x9ABC],
    [0xFFF0, 0x8812, 0x0004, 0x0002],
    [0x0001, 0x0002, 0x0003, 0x0004],
]
SLOT_WORDS = 16  # opcode + 4 extension words + NOP padding (resynchronises objdump at every slot)
OBJDUMP = "m68k-linux-gnu-objdump"
VMA = 0x400000  # load address; the decoder is run at the same addresses so that targets agree


def objdump_all(pattern):
    blob = bytearray()
    for w in range(0x10000):
        words = [w] + PATTERNS[pattern] + [0x4E71] * (SLOT_WORDS - 5)
        for x in words:
            blob += x.to_bytes(2, "big")
    open("/tmp/m68k_all.bin", "wb").write(blob)
    out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "m68k:68000", "--no-show-raw-insn",
                          "--adjust-vma=0x400000", "/tmp/m68k_all.bin"], capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S.*)$", line)
        if not m:
            continue
        addr = int(m.group(1), 16) - VMA
        if addr % (SLOT_WORDS * 2) == 0:
            res[addr // (SLOT_WORDS * 2)] = m.group(2).strip()
    # length of each slot's first instruction = distance to the next listed address
    addrs = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S.*)$", line)
        if m:
            addrs.append(int(m.group(1), 16) - VMA)
    lens = {}
    for i, a in enumerate(addrs[:-1]):
        if a % (SLOT_WORDS * 2) == 0:
            lens[a // (SLOT_WORDS * 2)] = addrs[i + 1] - a
    return res, lens


def num(t):
    t = t.strip()
    neg = t.startswith("-")
    t = t.lstrip("-")
    v = int(t[1:], 16) if t.startswith("$") else int(t, 16) if t.startswith("0x") else int(t)
    return -v if neg else v


def reg_set(items, convert):
    regs = set()
    for it in items.split("/"):
        a, _, b = it.partition("-")
        a, b = convert(a), convert(b or a)
        for n in range(int(a[1]), int(b[1]) + 1):
            regs.add(a[0] + str(n))
    return ("list", frozenset(regs))


def ours_op(o):
    if re.fullmatch(r"[DA][0-7]", o) or o in ("CCR", "SR", "USP"):
        return ("reg", o)
    if re.fullmatch(r"\(A[0-7]\)\+?", o) or re.fullmatch(r"-\(A[0-7]\)", o):
        return ("ind", o)
    if re.fullmatch(r"[DA][0-7]([/-][DA][0-7])+", o):
        return reg_set(o, str)
    m = re.fullmatch(r"(-?\$[0-9A-F]+)\((A[0-7])\)", o)
    if m:
        return ("d16", num(m.group(1)), m.group(2))
    m = re.fullmatch(r"(-?\$[0-9A-F]+)\((A[0-7]),([DA][0-7])\.([WL])\)", o)
    if m:
        return ("idx", num(m.group(1)), m.group(2), m.group(3), m.group(4))
    m = re.fullmatch(r"(\$[0-9A-F]+)\(PC\)", o)
    if m:
        return ("pc", num(m.group(1)))
    m = re.fullmatch(r"(\$[0-9A-F]+)\(PC,([DA][0-7])\.([WL])\)", o)
    if m:
        return ("pcidx", num(m.group(1)), m.group(2), m.group(3))
    m = re.fullmatch(r"(\$[0-9A-F]+)\.W", o)
    if m:
        v = num(m.group(1))
        return ("abs", (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF)
    m = re.fullmatch(r"(\$[0-9A-F]+)\.L", o)
    if m:
        return ("abs", num(m.group(1)))
    if re.fullmatch(r"\$[0-9A-F]+", o):
        return ("abs", num(o))
    if o.startswith("#"):
        return ("imm", num(o[1:]))
    return ("?", o)


def idx_disp(t):
    # objdump prints the 8 bit displacement of an indexed operand in hex without a prefix
    return int(t, 0) if "x" in t else int(t, 16)


def theirs_op(o):
    conv = lambda r: {"%fp": "A6", "%sp": "A7"}.get(r, r.lstrip("%").upper())
    if re.fullmatch(r"%(d[0-7]|a[0-7]|fp|sp|ccr|sr|usp)", o):
        return ("reg", conv(o))
    m = re.fullmatch(r"%(\w+)@([+-]?)", o)
    if m:
        r = conv("%" + m.group(1))
        return ("ind", {"": f"({r})", "+": f"({r})+", "-": f"-({r})"}[m.group(2)])
    if re.fullmatch(r"%\w+(-%\w+)?(/%\w+(-%\w+)?)*", o) and ("/" in o or "-" in o):
        return reg_set(o.replace("%", ""), lambda r: conv("%" + r))
    m = re.fullmatch(r"%pc@\((0x[0-9a-f]+)\)", o)
    if m:
        return ("pc", num(m.group(1)))
    m = re.fullmatch(r"%pc@\((0x[0-9a-f]+),%(\w+):([wl])\)", o)
    if m:
        return ("pcidx", num(m.group(1)), conv("%" + m.group(2)), m.group(3).upper())
    m = re.fullmatch(r"%(\w+)@\((-?\w+)\)", o)
    if m:
        return ("d16", int(m.group(2), 0), conv("%" + m.group(1)))
    m = re.fullmatch(r"%(\w+)@\((-?\w+),%(\w+):([wl])\)", o)
    if m:
        return ("idx", idx_disp(m.group(2)), conv("%" + m.group(1)), conv("%" + m.group(3)), m.group(4).upper())
    if re.fullmatch(r"0x[0-9a-f]+", o):
        return ("abs", num(o))
    if o.startswith("#"):
        return ("imm", int(o[1:], 0))
    return ("?", o)


def split_top(text):
    out, cur, depth = [], "", 0
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out if x.strip()]


def operands_differ(mn, ops, text):
    """None if the operands mean the same, else a short description."""
    parts = text.split(None, 1)
    their_mn = parts[0]
    theirs = split_top(parts[1]) if len(parts) > 1 else []
    ours = [] if ops == "-" else split_top(ops)
    if len(ours) != len(theirs):
        return "operand count"
    size = {"B": 1, "W": 2, "L": 4}.get(mn[-1] if "." in mn else their_mn[-1].upper(), 4)
    if their_mn == "moveq":
        size = 1
    for a, b in zip(ours, theirs):
        x, y = ours_op(a), theirs_op(b)
        if x[0] == "imm" and y[0] == "imm":
            mod = 1 << (8 * size)
            if x[1] % mod != y[1] % mod:
                return f"immediate {a} vs {b}"
        elif x != y:
            return f"operand {a} vs {b}"
    return None


def known(w, reason, text, pattern=0):
    """Intended differences: objdump is more permissive or decodes things the 68000 does not have."""
    if reason == "ours invalid, objdump valid":
        if w >> 12 == 0xF:                       # Line F: coprocessor/MMU instructions (68020+)
            return "line F"
        if w == 0x4AFD:                          # objdump pseudo-instruction (swbeg)
            return "swbeg"
        if 0x6000 <= w < 0x7000 and (w & 1):
            return "branch to odd address"
        if (w & 0xF0C8) == 0x5008 and (w & 0xC0) == 0:   # ADDQ.B/SUBQ.B #n,An
            return "ADDQ/SUBQ.B An"
        if PATTERNS[pattern][0] & 1 and (w & 0xFF00) >= 0x6000 and (w & 0xFF00) < 0x7000 and (w & 0xFF) == 0:
            return "branch to odd address"        # Bcc.W with odd displacement
        if PATTERNS[pattern][0] & 1 and (w & 0xF0F8) == 0x50C8:
            return "branch to odd address"        # DBcc with odd displacement
    return None


def main():
    tool = sys.argv[1]
    patterns = [int(x) for x in sys.argv[2:]] or [0]
    bad_total = 0
    for p in patterns:
        ours = {}
        for line in subprocess.run([tool, str(p)], capture_output=True, text=True, check=True).stdout.splitlines():
            f = line.split(" ", 4)
            ours[int(f[0], 16)] = (f[1] == "1", int(f[2]), f[3], f[4] if len(f) > 4 else "")
        theirs, lens = objdump_all(p)
        diffs = []
        skipped = {}
        for w in range(0x10000):
            valid, length, mn, ops = ours[w]
            text = theirs.get(w, "?")
            their_invalid = text.startswith(".short") or text.startswith(".word") or text.startswith(".byte")
            their_len = lens.get(w, 0)
            their_mn = text.split()[0].lower() if text else ""
            if valid and their_invalid:
                diffs.append((w, "ours valid, objdump invalid", mn + " " + ops, text))
            elif not valid and not their_invalid:
                diffs.append((w, "ours invalid, objdump valid", "DC.W", text))
            elif valid and length != their_len:
                diffs.append((w, f"length {length} vs {their_len}", mn + " " + ops, text))
            elif valid:
                # GNU syntax glues the size to the mnemonic (orib, movew, braw, bnes ...)
                base = mn.split(".")[0].lower()
                names = {base + suffix for suffix in ("", "b", "w", "l", "s")}
                if base == "dbra":
                    names |= {"dbf", "dbra"}
                if their_mn not in names:
                    diffs.append((w, "mnemonic", mn + " " + ops, text))
                else:
                    why = operands_differ(mn, ops, text)
                    if why:
                        diffs.append((w, "operands: " + why.split(" ")[0], mn + " " + ops, text))
        diffs2 = []
        for d in diffs:
            k = known(d[0], d[1], d[3], p)
            if k:
                skipped[k] = skipped.get(k, 0) + 1
            else:
                diffs2.append(d)
        diffs = diffs2
        print(f"pattern {p}: {len(diffs)} differences; intended: {skipped}")
        groups = {}
        for d in diffs:
            key = (d[1], (d[3].split() or ["?"])[0], d[0] >> 12)
            groups.setdefault(key, []).append(d)
        for key, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            w, reason, ours, text = items[0]
            print("  %4d x %-28s objdump %-10s e.g. %04X: ours %-26s objdump %s" % (len(items), reason, key[1], w, ours, text))
        bad_total += len(diffs)
    sys.exit(1 if bad_total else 0)


main()
