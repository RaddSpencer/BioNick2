import re, sys
mp, rva = sys.argv[1], int(sys.argv[2], 16)
base, best = 0, None
for line in open(mp, errors='ignore'):
    m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
    if m: base = int(m.group(1), 16); continue
    # MSVC link.exe:  0001:010fc800   sym   00000001410fd800 f obj
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\s', line)
    if m:
        a = int(m.group(2), 16) - base
        name = m.group(1)
    else:
        # lld-link:  00001000 00000234     0                 sym
        t = line.split()
        if len(t) == 4 and re.fullmatch(r'[0-9a-fA-F]{8}', t[0]) and t[2].isdigit() and '(' not in t[3] and not t[3].startswith('.'):
            a, name = int(t[0], 16), t[3]
        else:
            continue
    if a <= rva and (best is None or a > best[0]):
        best = (a, name)
print(best[1], "+", hex(rva - best[0]))