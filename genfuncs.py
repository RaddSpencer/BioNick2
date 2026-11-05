import re
out = ["# extra entry points for branches into the middle of functions"]
entries = []
for l in open("classify.txt"):
    m = re.match(r"([0-9A-F]{8}) x\d+\s+in/after fn [0-9A-F]+ \(\+0x[0-9A-F]+\) next fn ([0-9A-F]{8})", l)
    if not m: continue
    a, nxt = int(m.group(1), 16), int(m.group(2), 16)
    size = nxt - a
    assert size > 0 and size % 4 == 0, (hex(a), hex(nxt))
    entries.append((a, size))
out.append("functions = [")
for a, s in entries:
    out.append(f"    {{ address = 0x{a:X}, size = 0x{s:X} }},")
out.append("]")
open("functions_extra.toml", "w").write("\n".join(out) + "\n")
print(len(entries), "entries")