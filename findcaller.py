import sys
path, line = sys.argv[1], int(sys.argv[2])
L = open(path, errors="ignore").read().split("\n")
start = max(i for i in range(line) if L[i].startswith("PPC_FUNC_IMPL("))
end = start + 1
while end < len(L) and not L[end].startswith("PPC_FUNC_IMPL(") and not L[end].startswith("__attribute__"):
    end += 1
print(L[start], f"-> lines {start}-{end} ({end-start} lines)")
for k in range(start, end):
    if "PPC_CALL_INDIRECT_FUNC" in L[k]:
        print(f"--- indirect call at line {k}")
        print("\n".join(L[max(start, k-14):k+3]))