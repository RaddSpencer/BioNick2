import struct, sys
XEX  = r"C:\dev\bcr2\private\default.xex"
BASE = r"E:\games\Bionic Commando - Rearmed 2 (World) (XBLA)\basefile.bin"
IMG  = 0x82000000

x = open(XEX, "rb").read()
img = open(BASE, "rb").read()
assert x[:4] == b"XEX2", x[:4]
hdr_count = struct.unpack(">I", x[0x14:0x18])[0]
opt = {}
for i in range(hdr_count):
    k, v = struct.unpack(">II", x[0x18+8*i:0x20+8*i])
    opt[k] = v
off = opt[0x103FF]                      # import libraries
size, strsz, libcount = struct.unpack(">III", x[off:off+12])
p = off + 12
strtab = x[p:p+strsz]; p += strsz
names, q = [], 0
while q < strsz and len(names) < libcount:
    e = strtab.index(b"\0", q); names.append(strtab[q:e].decode()); q = e + 1
    while q < strsz and strtab[q] == 0: q += 1

out = open("imports.txt", "w")
total = 0
for li in range(libcount):
    lsize = struct.unpack(">I", x[p:p+4])[0]
    name_index, count = struct.unpack(">HH", x[p+0x24:p+0x28])
    recs = struct.unpack(">%dI" % count, x[p+0x28:p+0x28+4*count])
    assert all(IMG <= a < IMG + len(img) for a in recs), f"bad record table in library {li}"
    out.write(f"# library {li}: {names[name_index] if name_index < len(names) else '?'} records={count}\n")
    for a in recs:
        w = struct.unpack(">I", img[a-IMG:a-IMG+4])[0]
        out.write(f"{li} {a:08X} word={w:08X} type={w>>24} ordinal={w & 0xFFFF}\n")
        total += 1
    p += lsize
out.close()
print(libcount, "libraries,", total, "records ->", "imports.txt")