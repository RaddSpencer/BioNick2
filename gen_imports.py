import re, urllib.request, os

SRC = {
  0: ("xboxkrnl", ["https://raw.githubusercontent.com/xenia-canary/xenia-canary/canary_experimental/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc"]),
  1: ("xam",      ["https://raw.githubusercontent.com/xenia-canary/xenia-canary/canary_experimental/src/xenia/kernel/xam/xam_table.inc"]),
}
pat = re.compile(r"XE_EXPORT\(\s*\w+\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)")
names = {0: {}, 1: {}}
for lib, (tag, urls) in SRC.items():
    local = f"{tag}_table.inc"
    text = None
    if os.path.exists(local):
        text = open(local, errors="ignore").read()
    else:
        for u in urls:
            try:
                text = urllib.request.urlopen(u, timeout=30).read().decode("utf-8", "ignore")
                open(local, "w").write(text); break
            except Exception as e:
                print("download failed:", u, e)
    if text:
        for m in pat.finditer(text):
            names[lib][int(m.group(1), 16)] = m.group(2)
    print(tag, "names loaded:", len(names[lib]))

recs = []
for l in open("imports.txt"):
    if l.startswith("#"): continue
    p = l.split()
    recs.append((int(p[0]), int(p[1], 16), int(p[3][5:]), int(p[4][8:])))

thunk = {}
for lib, addr, typ, ordv in recs:
    if typ == 1: thunk[(lib, ordv)] = addr

os.makedirs(r"host\generated", exist_ok=True)
libname = {0: "xboxkrnl", 1: "xam"}
def nm(lib, o): return names[lib].get(o, f"{libname[lib]}_ord_{o}")

with open(r"host\generated\import_table.inc", "w") as f:
    for lib, addr, typ, ordv in recs:
        if typ == 0:
            t = thunk.get((lib, ordv), 0)
            f.write(f'    {{ 0x{addr:08X}, 0x{t:08X}, {lib}, {ordv}, "{nm(lib, ordv)}" }},\n')
mapping = {}
for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+),\s*(\w+)\s*\}", open(r"ppc\ppc_func_mapping.cpp").read()):
    mapping[int(m.group(1), 16)] = m.group(2)

seen = set()
with open(r"host\generated\import_stubs.cpp", "w") as f:
    f.write('#include "ppc_context.h"\n#include <cstdint>\n')
    f.write("void ppc_import_dispatch(PPCContext& ctx, uint8_t* base, int lib, int ordinal);\n\n")
    unnamed = 0
    for (lib, ordv), addr in sorted(thunk.items(), key=lambda kv: kv[1]):
        sym = mapping.get(addr)
        if sym is None: sym = f"sub_{addr:08X}"
        if sym.startswith("sub_"): unnamed += 1
        if sym in seen: continue
        seen.add(sym)
        f.write(f"PPC_FUNC({sym}) {{ ppc_import_dispatch(ctx, base, {lib}, {ordv}); }}\n")
print(len(thunk), "thunk overrides,", unnamed, "without an import name,",
      sum(1 for r in recs if r[2]==0 and (r[0], r[3]) not in thunk), "variables")