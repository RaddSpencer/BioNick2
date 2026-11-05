import re, struct, sys
BASE = 0x82000000
img = open(r"E:\games\Bionic Commando - Rearmed 2 (World) (XBLA)\basefile.bin", "rb").read()

# function start addresses known to the recompiler
starts = sorted(int(m.group(1), 16) for m in
    re.finditer(r"\{\s*0x([0-9A-Fa-f]+),", open(r"ppc\ppc_func_mapping.cpp").read()))
import bisect

def word(a): return struct.unpack(">I", img[a-BASE:a-BASE+4])[0]

rows = []
for line in open("errors_summary.txt"):
    a, n = line.split(); a = int(a, 16); n = int(n)
    i = bisect.bisect_right(starts, a) - 1
    prev = starts[i] if i >= 0 else 0
    nxt = starts[i+1] if i+1 < len(starts) else 0
    rows.append((a, n, prev, nxt))

rows.sort()
with open("classify.txt", "w") as f:
    for a, n, prev, nxt in rows:
        ws = " ".join(f"{word(a+4*k):08X}" for k in range(6))
        f.write(f"{a:08X} x{n:<3} in/after fn {prev:08X} (+0x{a-prev:X}) next fn {nxt:08X} | prev {word(a-4):08X} | {ws}\n")
print("wrote classify.txt")