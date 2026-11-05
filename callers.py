import glob, sys
name = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
needle = f"{name}(ctx, base);"
for p in sorted(glob.glob(r"ppc\ppc_recomp.*.cpp")):
    L = open(p, errors="ignore").read().split("\n")
    for i, l in enumerate(L):
        if needle in l and not L[i].startswith("PPC_FUNC"):
            start = max(j for j in range(i) if L[j].startswith("PPC_FUNC_IMPL("))
            print("=" * 70)
            print(p, "line", i, "| in", L[start])
            print("\n".join(L[max(start, i - n):i + 2]))