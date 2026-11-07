"""A small LZ packer for the test decruncher (tests/agent/unpack/sfx_*.s): control byte c < $80: literal run of c+1 bytes; $80..$FE: copy (c&$7F)+3 bytes (at most 129: $FF is the end) from `offset` back
(the next byte, 1..255); $FF: end."""
import sys

def pack(data):
    out = bytearray(); i = 0; lit = bytearray()
    def flush():
        nonlocal lit
        while lit:
            run = lit[:128]; lit = lit[128:]
            out.append(len(run) - 1); out.extend(run)
    while i < len(data):
        best_len = 0; best_off = 0
        for off in range(1, min(255, i) + 1):
            n = 0
            while n < 129 and i + n < len(data) and data[i + n] == data[i + n - off]: n += 1
            if n > best_len: best_len, best_off = n, off
        if best_len >= 3:
            flush(); out.append(0x80 | (best_len - 3)); out.append(best_off); i += best_len
        else:
            lit.append(data[i]); i += 1
    flush(); out.append(0xFF)
    return bytes(out)

def unpack(blob):
    out = bytearray(); i = 0
    while True:
        c = blob[i]; i += 1
        if c == 0xFF: return bytes(out)
        if c < 0x80:
            out.extend(blob[i:i + c + 1]); i += c + 1
        else:
            n = (c & 0x7F) + 3; off = blob[i]; i += 1
            for _ in range(n): out.append(out[-off])

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    blob = pack(data)
    assert unpack(blob) == data
    lines = []
    for k in range(0, len(blob), 16):
        lines.append("        DC.B    " + ",".join(f"${b:02X}" for b in blob[k:k + 16]))
    open(sys.argv[2], "w").write("\n".join(lines) + "\n")
    print(f"{len(data)} bytes -> {len(blob)} bytes")
