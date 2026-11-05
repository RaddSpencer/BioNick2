import glob, sys
lr = sys.argv[1].upper()
needle = f"ctx.lr = 0x{lr};"
for p in sorted(glob.glob(r"ppc\ppc_recomp.*.cpp")):
    L = open(p, errors="ignore").read().split("\n")
    for line, l in enumerate(L):
        if needle in l:
            start = max(i for i in range(line) if L[i].startswith("PPC_FUNC_IMPL("))
            end = start + 1
            while end < len(L) and not L[end].startswith("PPC_FUNC_IMPL(") and not L[end].startswith("__attribute__"):
                end += 1
            print(p, "call site line", line, "|", L[start], f"lines {start}-{end}")
            print("\n".join(L[max(start, line-6):line+4]))
            for k in range(start, end):
                if "PPC_CALL_INDIRECT_FUNC" in L[k] and "// bctr" in L[k-1] and "bctrl" not in L[k-1]:
                    print(f"--- tail bctr at line {k}")
                    print("\n".join(L[max(start, k-22):k+3]))