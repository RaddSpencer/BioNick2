import glob, sys
lr = sys.argv[1].upper()
n = int(sys.argv[2]) if len(sys.argv) > 2 else 70
needle = f"ctx.lr = 0x{lr};"
for p in sorted(glob.glob(r"ppc\ppc_recomp.*.cpp")):
    L = open(p, errors="ignore").read().split("\n")
    for i, l in enumerate(L):
        if needle in l:
            start = max(j for j in range(i) if L[j].startswith("PPC_FUNC_IMPL("))
            print(p, "| function:", L[start])
            print("\n".join(L[max(start, i - n):i + 3]))