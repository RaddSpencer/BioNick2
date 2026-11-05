import glob, sys
name = sys.argv[1]
for p in glob.glob(r"ppc\ppc_recomp.*.cpp"):
    L = open(p, errors="ignore").read().split("\n")
    for i, l in enumerate(L):
        if l.startswith(f"PPC_FUNC_IMPL(__imp__{name})"):
            j = i + 1
            while j < len(L) and not L[j].startswith("PPC_FUNC_IMPL(") and not L[j].startswith("__attribute__"):
                j += 1
            print(f"{p}: function spans lines {i}-{j} ({j-i} lines)")
            for k in range(i, j):
                if "PPC_CALL_INDIRECT_FUNC" in L[k]:
                    print(f"--- indirect call at line {k}")
                    print("\n".join(L[max(i, k-14):k+3]))