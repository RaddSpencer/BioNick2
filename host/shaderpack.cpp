// Reads the game's shader bundle: unpacks it and finds the shader containers inside. Used to compile shaders ahead of time.
#include "shaderpack.h"
#include <cstdio>

namespace {

// A small zlib inflate, after the method of zlib's "puff": slow by inflate standards, which is fine for a few MB at first run.
struct Bits {
    const uint8_t* in; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool bad = false;
    uint32_t get(int need) {
        uint32_t v = buf;
        while (cnt < need) {
            if (pos >= n) { bad = true; return 0; }
            v |= (uint32_t)in[pos++] << cnt; cnt += 8;
        }
        buf = need == 32 ? 0 : v >> need; cnt -= need;
        return need == 32 ? v : v & ((1u << need) - 1);
    }
};
struct Huff { uint16_t count[16]; uint16_t symbol[320]; };
int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)b.get(1);
        if (b.bad) return -1;
        int c = h.count[len];
        if (code - c < first) return h.symbol[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}
void build(Huff& h, const uint8_t* lens, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[lens[i]]++;
    h.count[0] = 0;
    uint16_t offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++) if (lens[i]) h.symbol[offs[lens[i]]++] = (uint16_t)i;
}
bool codes(Bits& b, uint8_t* out, size_t cap, size_t& w, const Huff& lc, const Huff& dc) {
    static const uint16_t lb[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const uint8_t  le[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const uint16_t db[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const uint8_t  de[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    for (;;) {
        int s = decode(b, lc);
        if (s < 0) return false;
        if (s < 256) { if (w >= cap) return false; out[w++] = (uint8_t)s; continue; }
        if (s == 256) return true;
        s -= 257;
        if (s >= 29) return false;
        size_t len = lb[s] + b.get(le[s]);
        int d = decode(b, dc);
        if (d < 0 || d >= 30) return false;
        size_t dist = db[d] + b.get(de[d]);
        if (b.bad || dist > w || w + len > cap) return false;
        for (size_t i = 0; i < len; i++, w++) out[w] = out[w - dist];
    }
}
// Inflates one raw deflate stream, appending to out. Returns false on bad data.
bool inflate_stream(const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t& w) {
    Bits b{ in, n };
    static Huff fixedL, fixedD; static bool fixedReady = false;
    if (!fixedReady) {
        uint8_t l[320]; int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(fixedL, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(fixedD, l, 30);
        fixedReady = true;
    }
    for (;;) {
        int last = (int)b.get(1), type = (int)b.get(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > n) return false;
            size_t len = in[b.pos] | (size_t)in[b.pos + 1] << 8;
            b.pos += 4;
            if (b.pos + len > n || w + len > cap) return false;
            memcpy(out + w, in + b.pos, len); w += len; b.pos += len;
        } else if (type == 1) {
            if (!codes(b, out, cap, w, fixedL, fixedD)) return false;
        } else if (type == 2) {
            static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            int nlen = (int)b.get(5) + 257, ndist = (int)b.get(5) + 1, ncode = (int)b.get(4) + 4;
            if (b.bad || nlen > 286 || ndist > 30) return false;
            uint8_t l[320] = {};
            for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)b.get(3);
            Huff cl; build(cl, l, 19);
            uint8_t all[320] = {}; int i = 0;
            while (i < nlen + ndist) {
                int s = decode(b, cl);
                if (s < 0) return false;
                if (s < 16) { all[i++] = (uint8_t)s; continue; }
                int prev = 0, rep;
                if (s == 16) { if (i == 0) return false; prev = all[i - 1]; rep = 3 + (int)b.get(2); }
                else if (s == 17) rep = 3 + (int)b.get(3);
                else rep = 11 + (int)b.get(7);
                if (b.bad || i + rep > nlen + ndist) return false;
                while (rep--) all[i++] = (uint8_t)prev;
            }
            Huff lc, dc; build(lc, all, nlen); build(dc, all + nlen, ndist);
            if (!codes(b, out, cap, w, lc, dc)) return false;
        } else return false;
        if (last) return true;
    }
}
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

} // namespace

bool shaderpack_load(const std::string& path, std::vector<uint8_t>& data, std::vector<PackShader>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw((size_t)n);
    const bool read = n > 12 && fread(raw.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!read) return false;
    // The bundle: total size in 8 bytes, then chunks of a 4-byte size and either zlib data or stored bytes.
    const uint64_t total = (uint64_t)be32(&raw[0]) << 32 | be32(&raw[4]);
    if (total == 0 || total > (1ull << 30)) return false;
    data.resize((size_t)total);
    size_t at = 8, w = 0;
    while (at + 4 <= raw.size() && w < total) {
        const uint32_t c = be32(&raw[at]); at += 4;
        if (c < 2 || at + c > raw.size()) { fprintf(stderr, "[shader] bad chunk size %u at %zu of %zu\n", c, at, raw.size()); return false; }
        uint32_t c2 = c;
        if (raw[at] == 0x78) {
            // The last chunk can hold more than the stated total. Once that much is out, the rest doesn't matter.
            if (!inflate_stream(&raw[at + 2], c - 2, data.data(), (size_t)total, w) && w < total) { fprintf(stderr, "[shader] bad zlib data in the chunk at %zu, %zu bytes unpacked so far\n", at, w); return false; }
        } else {
            if (w + c > total) c2 = (uint32_t)(total - w);
            memcpy(&data[w], &raw[at], c2); w += c2;
        }
        at += c;
    }
    if (w != total) { fprintf(stderr, "[shader] unpacked %zu of %llu bytes\n", w, (unsigned long long)total); return false; }
    // Containers: 102A1100 is a pixel shader, 102A1101 a vertex shader. Their headers are checked, since the bundle holds other data too.
    const size_t size = data.size();
    for (size_t i = 0; i + 36 < size; i++) {
        if (data[i] != 0x10 || data[i + 1] != 0x2A || data[i + 2] != 0x11 || data[i + 3] > 1) continue;
        const uint32_t virt = be32(&data[i + 4]), phys = be32(&data[i + 8]), hdr = be32(&data[i + 24]);
        if (virt == 0 || virt > 0x100000 || phys == 0 || phys > 0x100000 || hdr >= virt || i + hdr + 8 >= size) continue;
        const uint32_t codeOff = be32(&data[i + hdr]), bytes = be32(&data[i + hdr + 4]);
        const uint64_t code = (uint64_t)i + virt + codeOff;
        if (bytes == 0 || (bytes & 3) || bytes > phys || code + bytes > size) continue;
        out.push_back({ data[i + 3] == 1, (uint32_t)code, bytes / 4 });
    }
    return true;
}
