// See extract.h. Three parts: the STFS package reader, the XEX unpacker (AES + LZX) and the command line.
#include "extract.h"
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

#pragma comment(lib, "bcrypt.lib")

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t le24(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

std::string fmt(const char* f, ...) {
    char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof(b), f, a); va_end(a); return b;
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; uint8_t out[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        for (size_t o = 0; o < len; o += 1 << 24) BCryptHashData(h, (PUCHAR)data + o, (ULONG)std::min<size_t>(1 << 24, len - o), 0);
        BCryptFinishHash(h, out, 32, 0); BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string s; for (uint8_t c : out) s += fmt("%02x", c); return s;
}

// AES-128, CBC with a zero IV (the XEX container's cipher), in place.
bool aes_cbc_decrypt(const uint8_t key[16], uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE k = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 16, 0) == 0) {
        uint8_t iv[16] = {}; ULONG got = 0; ok = true;
        for (size_t o = 0; ok && o < len; o += 1 << 24) {   // chunks of 16 MB (a multiple of the block size); the IV carries on
            ULONG n = (ULONG)std::min<size_t>(1 << 24, len - o);
            uint8_t nextIv[16]; memcpy(nextIv, data + o + n - 16, 16);
            ok = BCryptDecrypt(k, data + o, n, nullptr, iv, 16, data + o, n, &got, 0) == 0;
            memcpy(iv, nextIv, 16);
        }
    }
    if (k) BCryptDestroyKey(k);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// LZX (the Microsoft CAB/XEX flavour), after the layout of libmspack's lzxd. Decodes straight into the output image, which doubles as the window.
// ---------------------------------------------------------------------------------------------------------------------------------------------------
struct Huff {
    int count[17] = {}, first[17] = {}, offset[17] = {};
    std::vector<uint16_t> sorted;
    bool build(const uint8_t* lens, int n) {
        memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++) count[lens[i]]++;
        count[0] = 0;
        int code = 0, off = 0;
        for (int l = 1; l <= 16; l++) { code = (code + count[l - 1]) << 1; first[l] = code; offset[l] = off; off += count[l]; }
        sorted.assign(off ? off : 1, 0);
        int pos[17]; for (int l = 1; l <= 16; l++) pos[l] = offset[l];
        for (int i = 0; i < n; i++) if (lens[i]) sorted[pos[lens[i]]++] = (uint16_t)i;
        return true;
    }
};

struct Lzx {
    const uint8_t* in; size_t inLen, inPos = 0;
    uint64_t buf = 0; int bits = 0;
    std::string err;

    Lzx(const uint8_t* d, size_t n) : in(d), inLen(n) {}
    void ensure(int n) {
        while (bits < n) {
            uint32_t w = 0;
            if (inPos + 1 < inLen) w = in[inPos] | (uint32_t)in[inPos + 1] << 8;
            inPos += 2;
            buf |= (uint64_t)w << (48 - bits);   // the buffer is MSB-justified in 64 bits; 16 new bits go just below those already there
            bits += 16;
        }
    }
    uint32_t peek(int n) { ensure(n); return (uint32_t)(buf >> (64 - n)); }
    void drop(int n) { buf <<= n; bits -= n; }
    uint32_t read(int n) { if (!n) return 0; uint32_t v = peek(n); drop(n); return v; }
    int sym(const Huff& h) {
        uint32_t c16 = peek(16);
        for (int l = 1; l <= 16; l++) {
            int c = (int)(c16 >> (16 - l)) - h.first[l];
            if (c >= 0 && c < h.count[l]) { drop(l); return h.sorted[h.offset[l] + c]; }
        }
        return -1;
    }

    bool readLens(uint8_t* lens, int first, int last) {
        uint8_t pl[20]; Huff pre;
        for (int i = 0; i < 20; i++) pl[i] = (uint8_t)read(4);
        pre.build(pl, 20);
        for (int x = first; x < last;) {
            int z = sym(pre);
            if (z < 0) { err = "bad pretree code"; return false; }
            if (z == 17 || z == 18) {
                int y = z == 17 ? read(4) + 4 : read(5) + 20;
                if (x + y > last) { err = "pretree run too long"; return false; }
                while (y--) lens[x++] = 0;
            } else if (z == 19) {
                int y = read(1) + 4;
                int z2 = sym(pre);
                if (z2 < 0) { err = "bad pretree code"; return false; }
                z = lens[x] - z2; if (z < 0) z += 17;
                if (x + y > last) { err = "pretree run too long"; return false; }
                while (y--) lens[x++] = (uint8_t)z;
            } else {
                z = lens[x] - z; if (z < 0) z += 17;
                lens[x++] = (uint8_t)z;
            }
        }
        return true;
    }

    bool decode(uint8_t* out, size_t outLen, int windowBits) {
        static const int kSlots[] = { 30, 32, 34, 36, 38, 42, 50 };
        if (windowBits < 15 || windowBits > 21) { err = "unsupported LZX window"; return false; }
        const int slots = kSlots[windowBits - 15];
        uint32_t extraBits[64], base[65]; base[0] = 0;
        for (int i = 0; i < 51; i++) { extraBits[i] = i < 4 ? 0 : std::min(17, i / 2 - 1); base[i + 1] = base[i] + (1u << extraBits[i]); }
        const int mainN = 256 + slots * 8;
        std::vector<uint8_t> mainLens(mainN + 8, 0), lenLens(249 + 8, 0); uint8_t alignedLens[8] = {};
        Huff mainT, lenT, alignT;
        uint32_t R0 = 1, R1 = 1, R2 = 1;
        int blockType = 0; uint32_t blockRemaining = 0, blockLength = 0;
        bool headerRead = false; uint32_t intelFile = 0, intelCur = 0; bool intelStarted = false;
        size_t pos = 0;

        while (pos < outLen) {
            const size_t frameStart = pos, target = std::min(outLen, pos + 32768);
            if (!headerRead) {
                if (read(1)) { uint32_t hi = read(16), lo = read(16); intelFile = hi << 16 | lo; }
                headerRead = true;
            }
            while (pos < target) {
                if (blockRemaining == 0) {
                    if (blockType == 3 && (blockLength & 1)) { if (inPos < inLen) inPos++; }   // an uncompressed block is padded to a 16-bit boundary
                    blockType = (int)read(3);
                    uint32_t hi = read(16), lo = read(8);
                    blockRemaining = blockLength = hi << 8 | lo;
                    if (blockType == 2) {
                        for (int i = 0; i < 8; i++) alignedLens[i] = (uint8_t)read(3);
                        alignT.build(alignedLens, 8);
                    }
                    if (blockType == 1 || blockType == 2) {
                        if (!readLens(mainLens.data(), 0, 256) || !readLens(mainLens.data(), 256, mainN)) return false;
                        if (mainLens[0xE8]) intelStarted = true;
                        if (!readLens(lenLens.data(), 0, 249)) return false;
                        mainT.build(mainLens.data(), mainN); lenT.build(lenLens.data(), 249);
                    } else if (blockType == 3) {
                        intelStarted = true;
                        ensure(16);
                        if (bits > 16) inPos -= 2;
                        bits = 0; buf = 0;
                        if (inPos + 12 > inLen) { err = "truncated uncompressed block"; return false; }
                        R0 = in[inPos] | in[inPos + 1] << 8 | in[inPos + 2] << 16 | (uint32_t)in[inPos + 3] << 24; inPos += 4;
                        R1 = in[inPos] | in[inPos + 1] << 8 | in[inPos + 2] << 16 | (uint32_t)in[inPos + 3] << 24; inPos += 4;
                        R2 = in[inPos] | in[inPos + 1] << 8 | in[inPos + 2] << 16 | (uint32_t)in[inPos + 3] << 24; inPos += 4;
                    } else { err = fmt("bad LZX block type %d", blockType); return false; }
                    if (blockRemaining == 0) continue;
                }
                const size_t before = pos;
                size_t run = std::min<size_t>(blockRemaining, target - pos);
                if (blockType == 3) {
                    if (inPos + run > inLen) { err = "truncated uncompressed block"; return false; }
                    memcpy(out + pos, in + inPos, run); inPos += run; pos += run;
                } else {
                    long todo = (long)run;
                    while (todo > 0) {
                        int m = sym(mainT);
                        if (m < 0) { err = "bad main code"; return false; }
                        if (m < 256) { out[pos++] = (uint8_t)m; todo--; continue; }
                        m -= 256;
                        uint32_t len = m & 7;
                        if (len == 7) { int f = sym(lenT); if (f < 0) { err = "bad length code"; return false; } len += f; }
                        len += 2;
                        uint32_t slot = m >> 3, off;
                        if (slot == 0) off = R0;
                        else if (slot == 1) { off = R1; R1 = R0; R0 = off; }
                        else if (slot == 2) { off = R2; R2 = R0; R0 = off; }
                        else {
                            uint32_t eb = extraBits[slot], v = 0;
                            if (blockType == 2 && eb >= 3) {
                                if (eb > 3) v = read(eb - 3) << 3;
                                int a = sym(alignT); if (a < 0) { err = "bad aligned code"; return false; }
                                v += a;
                            } else v = read(eb);
                            off = base[slot] - 2 + v;
                            R2 = R1; R1 = R0; R0 = off;
                        }
                        if (off > pos) { err = fmt("LZX match before the start (offset %u at %zu)", off, pos); return false; }
                        if (pos + len > outLen) { err = "LZX match past the end"; return false; }
                        for (uint32_t i = 0; i < len; i++, pos++) out[pos] = out[pos - off];
                        todo -= (long)len;
                    }
                }
                blockRemaining -= (uint32_t)(pos - before);
                if (pos > target) { err = "LZX data runs over a frame boundary"; return false; }
            }
            // The stream is realigned to a 16-bit boundary at every frame end.
            if (bits > 0) ensure(16);
            if (bits & 15) drop(bits & 15);
            // E8 call-address translation, when the stream asked for it.
            if (intelStarted && intelFile && (frameStart >> 15) < 32768 && target - frameStart > 10) {
                uint8_t* d = out + frameStart; uint8_t* e = d + (target - frameStart) - 10; int32_t cur = (int32_t)intelCur;
                while (d < e) {
                    if (*d++ != 0xE8) { cur++; continue; }
                    int32_t a = (int32_t)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24);
                    if (a >= -cur && a < (int32_t)intelFile) {
                        int32_t rel = a >= 0 ? a - cur : a + (int32_t)intelFile;
                        d[0] = (uint8_t)rel; d[1] = (uint8_t)(rel >> 8); d[2] = (uint8_t)(rel >> 16); d[3] = (uint8_t)(rel >> 24);
                    }
                    d += 4; cur += 5;
                }
            }
            intelCur += (uint32_t)(target - frameStart);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// XEX2: default.xex -> the executable image
// ---------------------------------------------------------------------------------------------------------------------------------------------------
bool xex_to_image(const std::vector<uint8_t>& xex, std::vector<uint8_t>& image, std::string& err) {
    if (xex.size() < 0x18 || memcmp(xex.data(), "XEX2", 4) != 0) { err = "default.xex is not an XEX2 file"; return false; }
    const uint8_t* x = xex.data();
    const uint32_t dataOff = be32(x + 8), secOff = be32(x + 0x10), optCount = be32(x + 0x14);
    if (secOff + 0x158 > xex.size() || dataOff > xex.size()) { err = "default.xex headers are damaged"; return false; }
    uint32_t fmtOff = 0;
    for (uint32_t i = 0; i < optCount && 0x18 + i * 8 + 8 <= xex.size(); i++) {
        uint32_t id = be32(x + 0x18 + i * 8), val = be32(x + 0x18 + i * 8 + 4);
        if (id == 0x000003FF) fmtOff = val;
    }
    if (!fmtOff || fmtOff + 8 > xex.size()) { err = "default.xex has no file format header"; return false; }
    const uint32_t infoSize = be32(x + fmtOff);
    const uint16_t encType = be16(x + fmtOff + 4), compType = be16(x + fmtOff + 6);
    const uint32_t imageSize = be32(x + secOff + 4);
    uint8_t fileKey[16]; memcpy(fileKey, x + secOff + 0x150, 16);
    if (imageSize == 0 || imageSize > (1u << 30)) { err = "default.xex reports an odd image size"; return false; }

    std::vector<uint8_t> data(xex.begin() + dataOff, xex.end());
    data.resize(data.size() & ~(size_t)15);

    if (encType == 1) {   // the file key is itself AES-encrypted with the console key. Retail first, then the development key.
        static const uint8_t kRetail[16] = { 0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91 };
        static const uint8_t kDev[16] = {};
        bool good = false;
        for (const uint8_t* ck : { kRetail, kDev }) {
            uint8_t key[16]; memcpy(key, fileKey, 16);
            if (!aes_cbc_decrypt(ck, key, 16)) { err = "Windows AES is not available"; return false; }
            std::vector<uint8_t> probe(data.begin(), data.begin() + std::min<size_t>(data.size(), 64));
            if (probe.size() < 16 || !aes_cbc_decrypt(key, probe.data(), probe.size() & ~(size_t)15)) continue;
            // An uncompressed image starts with "MZ"; a compressed one starts with a block header whose size is plausible.
            bool plausible = compType == 0 ? (probe[0] == 'M' && probe[1] == 'Z') : compType == 1 ? true : (be32(probe.data()) > 0 && be32(probe.data()) < xex.size());
            if (plausible) { if (!aes_cbc_decrypt(key, data.data(), data.size())) { err = "AES failed"; return false; } good = true; break; }
        }
        if (!good) { err = "could not decrypt default.xex (unknown key)"; return false; }
    } else if (encType != 0) { err = fmt("unknown XEX encryption type %u", encType); return false; }

    image.assign(imageSize, 0);
    if (compType == 0) {
        memcpy(image.data(), data.data(), std::min<size_t>(imageSize, data.size()));
    } else if (compType == 1) {   // "basic": pairs of (bytes of data, bytes of zeros)
        size_t n = (infoSize - 8) / 8, s = 0, d = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t ds = be32(x + fmtOff + 8 + i * 8), zs = be32(x + fmtOff + 8 + i * 8 + 4);
            if (s + ds > data.size() || d + ds + zs > image.size()) { err = "basic-compressed XEX is damaged"; return false; }
            memcpy(image.data() + d, data.data() + s, ds); s += ds; d += ds + zs;
        }
    } else if (compType == 2) {   // "normal": LZX, with the chunk sizes and block hashes stripped out first
        const uint32_t windowSize = be32(x + fmtOff + 8);
        uint32_t firstBlock = be32(x + fmtOff + 12);
        int wbits = 0; while ((1u << wbits) < windowSize) wbits++;
        std::vector<uint8_t> stream; stream.reserve(data.size());
        size_t p = 0; uint32_t blk = firstBlock;
        while (blk) {
            if (p + blk > data.size() || blk < 24) { err = "compressed XEX blocks are damaged"; return false; }
            const size_t next = p + blk; const uint32_t nextSize = be32(data.data() + p);
            p += 24;
            for (;;) {
                if (p + 2 > data.size()) { err = "compressed XEX chunk is damaged"; return false; }
                uint32_t cs = data[p] << 8 | data[p + 1]; p += 2;
                if (!cs) break;
                if (p + cs > data.size()) { err = "compressed XEX chunk is damaged"; return false; }
                stream.insert(stream.end(), data.begin() + p, data.begin() + p + cs); p += cs;
            }
            p = next; blk = nextSize;
        }
        Lzx lz(stream.data(), stream.size());
        if (!lz.decode(image.data(), image.size(), wbits)) { err = "LZX: " + lz.err; return false; }
    } else { err = fmt("unknown XEX compression type %u", compType); return false; }

    if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z') { err = "the unpacked image is not a PE file (wrong key or damaged data)"; return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// STFS package
// ---------------------------------------------------------------------------------------------------------------------------------------------------
struct Stfs {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t base = 0, fileSize = 0;
    int shift = 0;   // 0: hash tables are one block, 1: two
    ~Stfs() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }

    bool readAt(uint64_t off, void* dst, size_t n) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        uint8_t* d = (uint8_t*)dst;
        while (n) { DWORD got = 0; if (!ReadFile(h, d, (DWORD)std::min<size_t>(n, 1u << 28), &got, nullptr) || !got) return false; d += got; n -= got; }
        return true;
    }
    // Data block n sits after the hash tables that come before it: one level-0 table per 170 blocks, one level-1 per 170*170, one level-2 per 170^3.
    uint64_t backing(uint32_t n) const {
        uint64_t b = n + (uint64_t)((n + 0xAA) / 0xAA << shift);
        if (n >= 0xAA) b += (uint64_t)((n + 0x70E4) / 0x70E4 << shift);
        if (n >= 0x70E4) b += (uint64_t)((n + 0x4AF768) / 0x4AF768 << shift);
        return b;
    }
    uint64_t offset(uint32_t n) const { return base + backing(n) * 0x1000; }
    // The next block of a file that is not stored in one run comes from the level-0 hash table that precedes the block's group.
    bool nextBlock(uint32_t n, uint32_t& next) {
        uint32_t groupFirst = n / 0xAA * 0xAA;
        uint64_t table = offset(groupFirst) - 0x1000;
        uint8_t e[24];
        if (!readAt(table + (uint64_t)(n % 0xAA) * 24, e, 24)) return false;
        next = be24(e + 0x15);
        return true;
    }
};

struct Entry { std::string name; bool dir = false, consecutive = false; uint32_t blocks = 0, start = 0, size = 0; uint16_t parent = 0xFFFF; };

bool make_dirs(const std::string& path) {   // path is a directory
    std::string p;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '\\' || path[i] == '/') {
            if (!p.empty() && p.back() != ':') { if (!CreateDirectoryA(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false; }
            if (i < path.size()) p += '\\';
        } else p += path[i];
    }
    return true;
}

ExtractResult extract_impl(const std::string& pkg, const std::string& outDir,
                           const std::function<void(unsigned long long, unsigned long long, const std::string&)>& progress) {
    ExtractResult r;
    Stfs s;
    s.h = CreateFileA(pkg.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (s.h == INVALID_HANDLE_VALUE) { r.error = "cannot open " + pkg; return r; }
    LARGE_INTEGER sz; GetFileSizeEx(s.h, &sz); s.fileSize = (uint64_t)sz.QuadPart;
    std::vector<uint8_t> hdr(0x1000);
    if (s.fileSize < 0xC000 || !s.readAt(0, hdr.data(), hdr.size())) { r.error = "this is not a content package (too small)"; return r; }
    if (memcmp(hdr.data(), "LIVE", 4) && memcmp(hdr.data(), "PIRS", 4) && memcmp(hdr.data(), "CON ", 4)) { r.error = "this is not an Xbox 360 content package (no LIVE/PIRS/CON header)"; return r; }
    const uint32_t titleId = be32(&hdr[0x360]);
    if (titleId != 0x584109DE) { r.error = fmt("this package is for title %08X, not Bionic Commando Rearmed 2 (584109DE)", titleId); return r; }
    const uint32_t headerSize = be32(&hdr[0x340]);
    s.base = ((uint64_t)headerSize + 0xFFF) & 0xF000;
    const uint8_t* vd = &hdr[0x379];
    if (vd[0] != 0x24) { r.error = "the package has an unexpected volume descriptor"; return r; }
    s.shift = (vd[2] & 1) ? 0 : 1;
    const uint32_t ftBlocks = vd[3] | vd[4] << 8, ftStart = le24(vd + 5);
    if (!ftBlocks || ftBlocks > 4096) { r.error = "the package's file table looks damaged"; return r; }

    // file table
    std::vector<uint8_t> ft((size_t)ftBlocks * 0x1000);
    for (uint32_t i = 0; i < ftBlocks; i++) if (!s.readAt(s.offset(ftStart + i), &ft[(size_t)i * 0x1000], 0x1000)) { r.error = "cannot read the package's file table"; return r; }
    std::vector<Entry> es;
    for (size_t o = 0; o + 0x40 <= ft.size(); o += 0x40) {
        const uint8_t* e = &ft[o];
        const int nl = e[0x28] & 0x3F;
        if (!nl) break;
        Entry en; en.name.assign((const char*)e, nl);
        en.consecutive = (e[0x28] & 0x40) != 0; en.dir = (e[0x28] & 0x80) != 0;
        en.blocks = le24(e + 0x29); en.start = le24(e + 0x2F); en.parent = be16(e + 0x32); en.size = be32(e + 0x34);
        es.push_back(en);
    }
    if (es.empty()) { r.error = "the package contains no files"; return r; }
    auto pathOf = [&](size_t i) {
        std::string p = es[i].name; int guard = 0;
        for (uint16_t par = es[i].parent; par != 0xFFFF && par < es.size() && guard < 64; par = es[par].parent, guard++) p = es[par].name + "\\" + p;
        return p;
    };

    unsigned long long total = 0; for (auto& e : es) if (!e.dir) total += e.size;
    const std::string root = outDir + "\\extracted_out";
    if (!make_dirs(root)) { r.error = "cannot create " + root; return r; }
    unsigned long long done = 0; DWORD lastTick = 0;
    std::vector<uint8_t> blockBuf(0x1000 * 64);
    std::vector<uint8_t> xexData;

    for (size_t i = 0; i < es.size(); i++) {
        const Entry& e = es[i];
        const std::string rel = pathOf(i), full = root + "\\" + rel;
        if (e.dir) { make_dirs(full); continue; }
        const size_t slash = full.find_last_of('\\');
        if (slash != std::string::npos) make_dirs(full.substr(0, slash));
        const bool isXex = _stricmp(rel.c_str(), "default.xex") == 0;
        HANDLE out = CreateFileA(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) { r.error = "cannot create " + full; return r; }
        uint64_t remaining = e.size; uint32_t blk = e.start;
        if (isXex) xexData.clear();
        while (remaining) {
            // gather a run of blocks that are next to each other in the file
            size_t run = 1;
            uint32_t next = blk + 1;
            if (e.consecutive) {
                const uint64_t want = (remaining + 0xFFF) / 0x1000;
                while (run < blockBuf.size() / 0x1000 && run < want && s.backing(blk + (uint32_t)run) == s.backing(blk) + run) run++;
            }
            const size_t n = (size_t)std::min<uint64_t>(remaining, run * 0x1000ull);
            if (!s.readAt(s.offset(blk), blockBuf.data(), n)) { CloseHandle(out); r.error = "the package ends early (" + rel + ")"; return r; }
            DWORD w = 0;
            if (!WriteFile(out, blockBuf.data(), (DWORD)n, &w, nullptr) || w != n) { CloseHandle(out); r.error = "cannot write " + full + " (disk full?)"; return r; }
            if (isXex) xexData.insert(xexData.end(), blockBuf.begin(), blockBuf.begin() + n);
            remaining -= n; done += n;
            if (remaining) {
                if (e.consecutive) next = blk + (uint32_t)run;
                else if (!s.nextBlock(blk, next)) { CloseHandle(out); r.error = "cannot follow the block chain of " + rel; return r; }
                blk = next;
            }
            if (progress && GetTickCount() - lastTick > 200) { lastTick = GetTickCount(); progress(done, total, rel); }
        }
        CloseHandle(out);
        r.files++;
    }
    r.bytes = done;
    if (progress) progress(done, total, "");

    if (xexData.empty()) { r.error = "the package has no default.xex"; return r; }
    std::vector<uint8_t> image; std::string err;
    if (progress) progress(done, total, "unpacking default.xex");
    if (!xex_to_image(xexData, image, err)) { r.error = err; return r; }
    const std::string basePath = outDir + "\\basefile.bin";
    HANDLE bf = CreateFileA(basePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (bf == INVALID_HANDLE_VALUE) { r.error = "cannot create " + basePath; return r; }
    bool wrote = true;
    for (size_t o = 0; o < image.size() && wrote; o += 1 << 24) { DWORD w = 0; DWORD n = (DWORD)std::min<size_t>(1 << 24, image.size() - o); wrote = WriteFile(bf, image.data() + o, n, &w, nullptr) && w == n; }
    CloseHandle(bf);
    if (!wrote) { r.error = "cannot write " + basePath; return r; }
    r.baseSha256 = sha256_hex(image.data(), image.size());
    // The image the recompiled code was built from.
    r.baseKnown = r.baseSha256 == "632d2918a86989af24cb302fdd0210ca8e0ee222d1bf90fa68f9511184efe0bc";
    r.ok = true;
    return r;
}

}  // namespace

ExtractResult extract_package(const std::string& package, const std::string& outDir,
                              const std::function<void(unsigned long long, unsigned long long, const std::string&)>& progress) {
    return extract_impl(package, outDir, progress);
}

int extract_main(int argc, char** argv) {
    // argv[1] is "--extract"
    if (argc < 4) { fprintf(stderr, "usage: bcr2 --extract <package file> <output folder>\n"); return 2; }
    const DWORD t0 = GetTickCount();
    auto r = extract_package(argv[2], argv[3], [&](unsigned long long d, unsigned long long t, const std::string& f) {
        fprintf(stderr, "\r%5.1f%%  %-60.60s", t ? 100.0 * d / t : 100.0, f.c_str());
    });
    fprintf(stderr, "\n");
    if (!r.ok) { fprintf(stderr, "extraction failed: %s\n", r.error.c_str()); return 1; }
    printf("%llu files, %.2f GB, %.1f s\nbasefile.bin sha256 %s%s\n", r.files, r.bytes / 1e9, (GetTickCount() - t0) / 1000.0, r.baseSha256.c_str(),
           r.baseKnown ? "  (the image this build was made from)" : "  (NOT the image this build was made from)");
    return r.baseKnown ? 0 : 3;
}

#ifdef BCR2_EXTRACT_MAIN
int main(int argc, char** argv) {
    char* a[4] = { argv[0], (char*)"--extract", argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr };
    return extract_main(argc + 1, a);
}
#endif
