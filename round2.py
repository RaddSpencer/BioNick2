import re, glob, bisect
BASE = 0x82000000
mapping = open(r"ppc\ppc_func_mapping.cpp").read()
allstarts = sorted(int(m.group(1), 16) for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+),", mapping))
extra = {int(m.group(1), 16) for m in re.finditer(r"address\s*=\s*0x([0-9A-Fa-f]+)", open("functions_extra.toml").read())}
orig = [a for a in allstarts if a not in extra]          # starts that existed before round 1

targets, report = {}, []
for path in sorted(glob.glob(r"ppc\ppc_recomp.*.cpp")):
    fn = None
    lines = open(path, errors="ignore").read().split("\n")
    for i, l in enumerate(lines):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", l)
        if m: fn = m.group(1)
        m = re.search(r"// ERROR ([0-9A-F]+)\s*$", l)
        if m:
            t = int(m.group(1), 16)
            targets.setdefault(t, []).append(fn)
            ctx = " | ".join(x.strip() for x in lines[max(0, i-3):i+2])
            report.append(f"fn={fn} target={t:08X} {ctx}")

open("round2_ctx.txt", "w").write("\n".join(report) + "\n")
entries = []
for t in sorted(targets):
    status = "ALREADY-ENTRY" if t in extra else ""
    j = bisect.bisect_right(orig, t)
    nxt = orig[j] if j < len(orig) else t + 4
    entries.append((t, nxt - t, status, targets[t]))
with open("functions_round2.toml", "w") as f:
    for t, s, st, callers in entries:
        if st: continue
        f.write(f"    {{ address = 0x{t:X}, size = 0x{s:X} }},\n")
for t, s, st, callers in entries:
    print(f"{t:08X} size 0x{s:X} callers {','.join(callers)} {st}")