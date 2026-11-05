import struct, re
BASE = 0x82000000
d = open(r"E:\games\Bionic Commando - Rearmed 2 (World) (XBLA)\basefile.bin", "rb").read()
LO, HI = 0x82190000, 0x82A49C64
def W(a): return struct.unpack(">I", d[a-BASE:a-BASE+4])[0]
def H(a): return struct.unpack(">H", d[a-BASE:a-BASE+2])[0]
def B(a): return d[a-BASE]
def sx16(v): return v - 0x10000 if v & 0x8000 else v

existing = set()
for m in re.finditer(r"base\s*=\s*(?:0x)?([0-9A-Fa-f]+)", open(r"C:\dev\bcr2\switch_tables.toml").read()):
    existing.add(int(m.group(1), 16))

def analyze(Bc):
    st = {}
    alias = {}
    def idx_of(r):
        s = st.get(r)
        if s and s[0] == 'idx': return s[1], s[2]
        if s and s[0] in ('ld', 'tgt'): return None
        return alias.get(r, r), 1      # a constant here came from another path: treat as variable
    for a in range(Bc - 4*40, Bc, 4):
        w = W(a); op = w >> 26
        rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31
        xo10 = (w >> 1) & 0x3FF
        if op == 15 and rA == 0: st[rD] = ('c', (sx16(w & 0xFFFF) << 16) & 0xFFFFFFFF)
        elif op == 14:
            if rA == 0: st[rD] = ('c', sx16(w & 0xFFFF) & 0xFFFFFFFF)
            elif st.get(rA, ('',))[0] == 'c': st[rD] = ('c', (st[rA][1] + sx16(w & 0xFFFF)) & 0xFFFFFFFF)
            else: st.pop(rD, None)
        elif op == 24:                               # ori rA,rS,uimm
            if rD == rA and (w & 0xFFFF) == 0: pass  # nop
            elif st.get(rD, ('',))[0] == 'c': st[rA] = ('c', st[rD][1] | (w & 0xFFFF))
            else: st.pop(rA, None)
        elif op == 21:                               # rlwinm rA,rS,SH,MB,ME
            sh = (w >> 11) & 31; src = st.get(rD)
            if src and src[0] == 'ld':    st[rA] = ('ld', src[1], src[2], src[3], src[4] + sh, src[5])
            elif src and src[0] == 'idx': st[rA] = ('idx', src[1], src[2] << sh)
            elif src and src[0] == 'c':   st.pop(rA, None)
            else: st[rA] = ('idx', alias.get(rD, rD), 1 << sh)
        elif op == 31 and xo10 in (279, 87):         # lhzx / lbzx
            size = 2 if xo10 == 279 else 1
            res = None
            if st.get(rA, ('',))[0] == 'c':
                i = idx_of(rB)
                if i: res = ('ld', st[rA][1], i[0], i[1], 0, size)
            if res is None and st.get(rB, ('',))[0] == 'c':
                i = idx_of(rA)
                if i: res = ('ld', st[rB][1], i[0], i[1], 0, size)
            if res: st[rD] = res
            else: st.pop(rD, None)
        elif op == 31 and ((w >> 1) & 0x1FF) == 266:  # add rD,rA,rB
            x, y = st.get(rA), st.get(rB)
            if x and y and x[0] == 'ld' and y[0] == 'c': st[rD] = ('tgt', x, y[1])
            elif x and y and y[0] == 'ld' and x[0] == 'c': st[rD] = ('tgt', y, x[1])
            else: st.pop(rD, None)
        elif op == 31 and xo10 == 467:               # mtspr
            spr = (((w >> 11) & 31) << 5) | ((w >> 16) & 31)
            if spr == 9: st['ctr'] = st.get(rD)
        elif op == 31 and xo10 == 444 and rD == rB:  # mr rA,rS
            alias[rA] = alias.get(rD, rD)
            if rD in st: st[rA] = st[rD]
            else: st.pop(rA, None)
           
        elif op in (32, 33, 34, 35, 40, 41, 42, 43) or op == 31: st.pop(rD, None)
    t = st.get('ctr')
    if not t or t[0] != 'tgt': return None, "no indexed-offset pattern"
    ld, code = t[1], t[2]
    _, tbl, root, mult, shift, size = ld
    if mult != size: return None, f"index multiplier {mult} vs element size {size}"
    n = None
    cands = {root} | {r for r, v in alias.items() if v == root}
    n = None; breg = root
    for a in range(Bc - 4, Bc - 4*40, -4):
        w = W(a)
        if (w >> 26) == 10 and ((w >> 16) & 31) in cands:
            n = (w & 0xFFFF) + 1; breg = (w >> 16) & 31; break
    if n is None: return None, "no cmplwi bound"
    labels = []
    for i in range(n):
        v = H(tbl + 2*i) if size == 2 else B(tbl + i)
        lab = (code + (v << shift)) & 0xFFFFFFFF
        if lab % 4 or not (LO <= lab < HI) or abs(lab - Bc) > 0x40000: return None, f"bad label {lab:08X} at entry {i}"
        labels.append(lab)
    return dict(base=Bc, r=breg, labels=labels, tbl=tbl, code=code, n=n, size=size, inline=(tbl == Bc + 4)), None

out, ok, un, sus = open("switch_tables_new.toml", "w"), [], [], []
for a in range(LO, HI, 4):
    if W(a) == 0x4E800420 and a not in existing:
        r, why = analyze(a)
        if r:
            ok.append(r)
            out.write("[[switch]]\nbase = 0x%X\nr = %d\nlabels = [%s]\n\n" % (a, r['r'], ", ".join("0x%X" % l for l in r['labels'])))
            print("bctr %08X: r%d, %d entries (%d-byte), table %08X (%s), code base %08X" % (a, r['r'], r['n'], r['size'], r['tbl'], "inline" if r['inline'] else "NOT inline", r['code']))
        else:
            un.append((a, why))
            ws = [W(a - 4*k) for k in range(1, 17)]
            has_add = any((x >> 26) == 31 and ((x >> 1) & 0x1FF) == 266 for x in ws[:6])
            has_idx = any((x >> 26) == 31 and ((x >> 1) & 0x3FF) in (23, 87, 279) for x in ws)
            if has_add and has_idx: sus.append((a, why))
out.close()
print(len(ok), "tables found,", len(un), "other bctr left alone,", len(sus), "still look like tables")
with open("bctr_suspicious.txt", "w") as f:
    for a, why in sus:
        f.write("%08X %s | %s\n" % (a, why, " ".join("%08X" % W(a - 4*k) for k in range(16, 0, -1))))