import re, glob
out = open("errctx.txt", "w")
for path in sorted(glob.glob(r"ppc\ppc_recomp.*.cpp")):
    fn = None
    lines = open(path, errors="ignore").read().split("\n")
    for i, l in enumerate(lines):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", l)
        if m: fn = m.group(1)
        if "// ERROR" in l:
            ctx = " | ".join(x.strip() for x in lines[max(0, i-3):i+2])
            out.write(f"fn={fn} {ctx}\n")