#include "ppc_config.h"
#include "ppc_context.h"
#include "ppc_recomp_shared.h"
#include <string>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

struct ImportEntry { uint32_t slot, thunk; int lib, ordinal; const char* name; };
const ImportEntry* host_import_table(size_t& count);
void register_all_imports();
void set_content_root(const char* p);

static uint8_t* g_base;

// ---- PM4 audit v2 (diagnostic only) ----
struct ShdDst { uint32_t addr, sw, init, n, maxv, lastv; };
struct Pm4Audit {
	uint32_t intrMask[64] = {};
    uint32_t type[4] = {}, op[128] = {};
    uint32_t ib = 0, intr = 0, bad = 0, zero = 0, shd = 0;
    ShdDst dst[24]; uint32_t ndst = 0;
    uint8_t fenceSeen[0x400] = {};
};
static uint32_t s_audit_fp = 0;
static uint32_t pm4_rd(uint32_t phys) {
    return _byteswap_ulong(*(const uint32_t*)(g_base + (0xA0000000u | (phys & 0x1FFFFFFFu))));
}
static void pm4_walk(uint32_t phys, uint32_t dwords, int depth, Pm4Audit& a) {
    if (dwords > 0x100000) { a.bad++; return; }
    uint32_t i = 0;
    while (i < dwords) {
        uint32_t h = pm4_rd(phys + i * 4);
        if (h == 0) { a.zero++; return; }
        uint32_t t = h >> 30;
        a.type[t]++;
        if (t == 3) {
            uint32_t cnt = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0x7F;
            if (i + 1 + cnt > dwords) { a.bad++; return; }
            a.op[op]++;
            uint32_t p = phys + (i + 1) * 4;
            if (op == 0x58 && cnt >= 3) {
                uint32_t init = pm4_rd(p), ad = pm4_rd(p + 4), v = pm4_rd(p + 8);
                a.shd++;
                uint32_t key = ad & ~3u;
                ShdDst* d = nullptr;
                for (uint32_t k = 0; k < a.ndst; k++) if (a.dst[k].addr == key) { d = &a.dst[k]; break; }
                if (!d && a.ndst < 24) { d = &a.dst[a.ndst++]; *d = ShdDst{ key, ad & 3u, init, 0, 0, 0 }; }
                if (d) { d->n++; if (v > d->maxv) d->maxv = v; d->lastv = v; }
                if ((key & 0x1FFFFFFFu) == (s_audit_fp & 0x1FFFFFFCu) && v < 0x400) a.fenceSeen[v] = 1;
            } else if (op == 0x54) {
                a.intr++; a.intrMask[pm4_rd(p) & 0x3F]++;
            } else if ((op == 0x3F || op == 0x37) && cnt >= 2 && depth < 4) {
                a.ib++;
                pm4_walk(pm4_rd(p) & 0x1FFFFFFC, pm4_rd(p + 4) & 0xFFFFF, depth + 1, a);
            }
            i += 1 + cnt;
        } else if (t == 0) i += 2 + ((h >> 16) & 0x3FFF);
        else if (t == 1) i += 3;
        else i += 1;
    }
}



static void pm4_audit() {
    uint32_t dev = 0x40002000;
    auto g = [&](uint32_t ad) { return _byteswap_ulong(*(const uint32_t*)(g_base + ad)); };
    s_audit_fp = g(dev + 10896);
    uint32_t w = *(const uint32_t*)(g_base + 0x7FC80714);
    if (w > 0xFFFF) w = _byteswap_ulong(w);
    if (w > 0x20000) w = 0x20000;
    static Pm4Audit a; a = Pm4Audit();
    pm4_walk(0x4000, w, 0, a);
    fprintf(stderr, "[audit] ring dwords=%u ib=%u intr=%u shd=%u zero-stops=%u bad=%u\n", w, a.ib, a.intr, a.shd, a.zero, a.bad);
    fprintf(stderr, "[audit] intr masks:");
    for (int m = 0; m < 64; m++) if (a.intrMask[m]) fprintf(stderr, " %X=%u", m, a.intrMask[m]);
    fprintf(stderr, "\n");
    for (uint32_t k = 0; k < a.ndst; k++)
        fprintf(stderr, "[audit] dst %08X sw=%u init=%08X n=%u max=%X last=%X\n",
                a.dst[k].addr, a.dst[k].sw, a.dst[k].init, a.dst[k].n, a.dst[k].maxv, a.dst[k].lastv);
    fprintf(stderr, "[audit] fence-word values present:");
    for (int v = 0; v < 0x400; v++) if (a.fenceSeen[v]) fprintf(stderr, " %X", v);
    fprintf(stderr, "\n[audit] fenceptr=%08X fence=%X ptr4=%08X sub=%X [dev+48]=%08X f13408=%08X f11012=%d\n",
            s_audit_fp, g(s_audit_fp), g(s_audit_fp + 4), g(dev + 10908), g(dev + 48), g(dev + 13408), (int)g(dev + 11012));
    auto dumpw = [&](const char* tag, uint32_t ad, int n) {
        fprintf(stderr, "[audit] %s @%08X:", tag, ad);
        for (int k = 0; k < n; k++) fprintf(stderr, " %08X", g(ad + k * 4));
        fprintf(stderr, "\n");
    };
    dumpw("q2", 0x40004D18, 27);     // cpu2 callback queue
    dumpw("q3", 0x40004D84, 27);     // cpu3 callback queue
    dumpw("ev2", 0x40004D54, 4);     // cpu2 event (type byte at +0, signal state at +4)
    dumpw("ev3", 0x40004DC0, 4);     // cpu3 event
    dumpw("wobj", 0x40004AC4, 28);   // worker object (+0 busy, +24/+28 idx/count, +32 payload, +36 resume, +60/+100 participants)
    dumpw("wobj+8",   0x40004ACC, 8);   // obj+8..+36: word2 = +16 (callback), word3 = +20 (ctx)
    dumpw("wobj+340", 0x40004C18, 12);  // obj+340..+384: word4 = +356, word5 = +360, word9 = +376
    dumpw("dev24200", 0x40007E88, 1);   // worker CPU mask (dev+24200)
    fflush(stderr);
}

thread_local PPCContext* g_ctx = nullptr;

static constexpr uint32_t kStackBase   = 0x70000000, kStackSize = 0x80000;  // from the XEX header
static constexpr uint32_t kPcrBase     = 0x70100000, kPcrSize   = 0x10000;
static constexpr uint32_t kKernelData  = 0x7F000000, kKernelDataSize = 0x10000;
static constexpr uint32_t kThreadObj   = 0x70110000, kThreadObjSize = 0x1000;

typedef void* (WINAPI *PFN_VirtualAlloc2_)(HANDLE, void*, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef void* (WINAPI *PFN_MapViewOfFile3_)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

// Reserve 4 GB of guest space. The three 512 MB windows at A0/C0/E0000000 are views of ONE section,
// so they alias the same physical memory like on the console.
static uint8_t* reserve_guest_memory() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto pVA2  = kb ? (PFN_VirtualAlloc2_)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
    auto pMap3 = kb ? (PFN_MapViewOfFile3_)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
    if (!pVA2 || !pMap3) { fprintf(stderr, "VirtualAlloc2/MapViewOfFile3 unavailable (need Windows 10 1803+)\n"); exit(1); }

    const uint64_t kSpan = 0x100000000ull, kPhys = 0x20000000ull;
    HANDLE proc = GetCurrentProcess();

    uint8_t* b = (uint8_t*)pVA2(proc, nullptr, kSpan, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!b) { fprintf(stderr, "reserve failed: %lu\n", GetLastError()); exit(1); }

    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)kPhys, nullptr);
    if (!sec) { fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError()); exit(1); }

    // Split into [0,A0) [A0,C0) [C0,E0) [E0,4G), always carving from the front of what is left.
    const uint64_t cuts[3] = { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull };
    uint64_t prev = 0;
    for (uint64_t cut : cuts) {
        if (!VirtualFree(b + prev, (SIZE_T)(cut - prev), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            fprintf(stderr, "split [%08llX,%08llX) failed: %lu\n", prev, cut, GetLastError()); exit(1);
        }
        prev = cut;
    }
    for (uint64_t w : cuts)                                   // map the same section into each window
        if (!pMap3(sec, proc, b + w, 0, (SIZE_T)kPhys, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            fprintf(stderr, "map of %08llX failed: %lu\n", w, GetLastError()); exit(1);
        }
    // the low part becomes a normal reservation so commit() works as before
    if (!pVA2(proc, b, 0xA0000000ull, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0)) {
        fprintf(stderr, "convert of low placeholder failed: %lu\n", GetLastError()); exit(1);
    }
    return b;
}

static void commit(uint32_t addr, size_t size) {
    if (!VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "commit of %08X (+%zX) failed: %lu\n", addr, size, GetLastError());
        exit(1);
    }
}
static void store_be32(uint32_t addr, uint32_t v) { *(uint32_t*)(g_base + addr) = _byteswap_ulong(v); }

static uint8_t* g_watchPage = nullptr;
static uint32_t g_watchLast32 = 0;
static uint8_t g_watchLast = 0;
static thread_local bool t_watchStep = false;
static constexpr uint32_t kWatchTarget = 0x40004AD4;
static constexpr uint32_t kWatchTargets[] = { 0x40004AD4, 0x40004AD8, 0x40004C28, 0x40004C2C };  // obj+16, +20, +356, +360
static constexpr int kNumWatch = (int)(sizeof(kWatchTargets) / sizeof(kWatchTargets[0]));
static uint32_t g_watchLastArr[kNumWatch] = {};

void watch_arm_if_in(uint32_t addr, uint32_t size) {
//    const uint32_t target = kWatchTarget;
    const uint32_t target = 0;                     // watch disabled
    if (!g_watchPage) {
        if (target >= addr && target < addr + size) {
            g_watchPage = g_base + (target & ~0xFFFu);
            DWORD old; VirtualProtect(g_watchPage, 0x1000, PAGE_READONLY, &old);
            fprintf(stderr, "[watch] armed on guest %08X\n", target);
        }
        return;
    }
    uint32_t P = (uint32_t)(g_watchPage - g_base);
    if (addr < P + 0x1000 && (uint64_t)addr + size > P) {      // a later commit resets the protection
        DWORD old; VirtualProtect(g_watchPage, 0x1000, PAGE_READONLY, &old);
        fprintf(stderr, "[watch] re-armed after commit %08X+%X\n", addr, size);
    }
}

static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code >= 0xC000008Du && code <= 0xC0000093u) {      // STATUS_FLOAT_*
        ep->ContextRecord->MxCsr = (ep->ContextRecord->MxCsr | 0x1F80u) & ~0x3Fu;
        ep->ContextRecord->FltSave.MxCsr = ep->ContextRecord->MxCsr;
        ep->ContextRecord->FltSave.ControlWord |= 0x3F;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && g_watchPage && ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        if (a >= g_watchPage && a < g_watchPage + 0x1000) {
            DWORD old; VirtualProtect(g_watchPage, 0x1000, PAGE_READWRITE, &old);
            ep->ContextRecord->EFlags |= 0x100;     // single-step so we can re-protect right after the write
            t_watchStep = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && t_watchStep) {
        t_watchStep = false;
        DWORD old; VirtualProtect(g_watchPage, 0x1000, PAGE_READONLY, &old);
        for (int wi = 0; wi < kNumWatch; wi++) {
            uint32_t cur = *(uint32_t*)(g_watchPage + (kWatchTargets[wi] & 0xFFFu));   // raw (big-endian) bytes
            if (cur == g_watchLastArr[wi]) continue;
            fprintf(stderr, "[watch] word %08X changed %08X -> %08X thread=%lu guest lr=%08X r1=%08X r3=%08X\n",
                    kWatchTargets[wi], _byteswap_ulong(g_watchLastArr[wi]), _byteswap_ulong(cur), GetCurrentThreadId(),
                    g_ctx ? (uint32_t)g_ctx->lr : 0, g_ctx ? g_ctx->r1.u32 : 0, g_ctx ? g_ctx->r3.u32 : 0);
            if (g_ctx) {
                uint32_t sp = g_ctx->r1.u32;
                fprintf(stderr, "[watch]    bt:");
                for (int k = 0; k < 8 && sp >= 0x40000000u && sp < 0x80000000u; k++) {
                    uint32_t cs = _byteswap_ulong(*(uint32_t*)(g_base + sp));
                    if (cs <= sp || cs - sp > 0x100000 || cs >= 0x80000000u) break;
                    fprintf(stderr, " %08X", _byteswap_ulong(*(uint32_t*)(g_base + cs - 8)));
                    sp = cs;
                }
                fprintf(stderr, "\n");
            }
            g_watchLastArr[wi] = cur;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == DBG_PRINTEXCEPTION_C || code == 0x406D1388 || code == DBG_PRINTEXCEPTION_WIDE_C) return EXCEPTION_CONTINUE_SEARCH;
    if (code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;   // host C++ throw: GuestLuaThrow is how Lua errors unwind to their pcall

    fprintf(stderr, "\n*** EXCEPTION %08lX at host rip=%p\n", code, ep->ExceptionRecord->ExceptionAddress);
    fprintf(stderr, "    rip offset=%llX\n",
            (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress
                                 - (uintptr_t)GetModuleHandleW(nullptr)));
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        uint64_t a = ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "    %s host addr %016llX (guest %08llX)\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                (unsigned long long)a, (unsigned long long)(a - (uint64_t)g_base));
    }
    if (g_ctx) {
        fprintf(stderr, "    guest lr=%08X ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r11=%08X r27=%08X r30=%08X r31=%08X\n",
                (uint32_t)g_ctx->lr, g_ctx->ctr.u32, g_ctx->r1.u32, g_ctx->r3.u32, g_ctx->r4.u32,
                g_ctx->r5.u32, g_ctx->r11.u32, g_ctx->r27.u32, g_ctx->r30.u32, g_ctx->r31.u32);
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void UnresolvedIndirect(PPCContext& ctx, uint8_t* base) {
    fprintf(stderr, "\n*** INDIRECT CALL to an address with no recompiled function\n"
            "    call site return address (lr) = %08X target (ctr) = %08X\n"
            "    r3=%08X r4=%08X r5=%08X r9=%08X r10=%08X r11=%08X r31=%08X\n",
            (uint32_t)ctx.lr, ctx.ctr.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r9.u32, ctx.r10.u32, ctx.r11.u32, ctx.r31.u32);
    fflush(stderr);
    ExitProcess(3);
}
static DWORD WINAPI GuestMain(LPVOID) {
    PPCContext ctx{};
    g_ctx = &ctx;
    AddVectoredExceptionHandler(1, CrashHandler);
    ctx.r1.u64  = kStackBase + kStackSize - 0x100;
    ctx.r13.u64 = kPcrBase;                      // PCR pointer; contents TBD
    // TODO: initialise ctx.fpscr (host MXCSR) once we check ppc_context.h
    _xstart(ctx, g_base);                        // XEX entry point (CRT startup)
    fprintf(stderr, "guest entry returned, r3=%08X\n", ctx.r3.u32);
    return 0;
}

unsigned dbg_intr_issued();
unsigned dbg_intr_done();

void dbg_dump_waits(unsigned minMs);
int main(int argc, char** argv) {
    {   // Windows 11 throttles a process whose window is covered or unfocused: it may ignore the 1 ms timer below, so Sleep(1) can take about 15 ms, and it may
        // move threads to slower cores. The game's threads are timed in milliseconds, so opt out. BCR2_ALLOWTHROTTLE keeps the Windows default, for comparison.
        if (!getenv("BCR2_ALLOWTHROTTLE")) {
            struct { ULONG version, control, state; } pt{ 1, 0x1 | 0x4, 0 };   // PROCESS_POWER_THROTTLING_CURRENT_VERSION; EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION; opted out of both
            if (!SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4 /* ProcessPowerThrottling */, &pt, sizeof(pt)))
                fprintf(stderr, "[main] could not opt out of power throttling: %lu\n", GetLastError());
        }
    }
    timeBeginPeriod(1);
    // stderr is written from every thread, and unbuffered each call is a file write that can stall the frame.
    // Buffer it and flush from a background thread. The crash handler flushes too. BCR2_LOGUNBUFFERED restores the old behaviour.
    if (!getenv("BCR2_LOGUNBUFFERED")) {
        setvbuf(stderr, nullptr, _IOFBF, 1 << 20);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { for (;;) { Sleep(500); fflush(stderr); } return 0; }, nullptr, 0, nullptr);
    }
    if (argc < 2) { fprintf(stderr, "usage: bcr2 <basefile.bin>\n"); return 1; }

    // 4 GB reserve: guest pointers are plain 32-bit offsets from base
    g_base = reserve_guest_memory();
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (;;) {
            Sleep(3000);
            auto gw = [](uint32_t a) { return _byteswap_ulong(*(const uint32_t*)(g_base + a)); };
            auto gr = [](uint32_t a) { uint32_t v = *(const uint32_t*)(g_base + a); return v > 0xFFFF ? _byteswap_ulong(v) : v; };
            uint32_t dev = 0x40002000, wo = 0x40004AC4, q2 = 0x40004D18;
            fprintf(stderr, "[st] fence=%X sub=%X wptr=%X rptr=%X shadow=%X dev48=%08X f13408=%08X fl=%08X | wk lock=%u part=%u arr=%u | q2 prod=%u cons=%u ev2=%u\n",
                    gw(gw(dev + 10896)), gw(dev + 10908), gr(0x7FC80714), gr(0x7FC80710), gw(dev + 10956),
                    gw(dev + 48), gw(dev + 13408), gw(dev + 10940),
                    gw(wo), gw(wo + 60), gw(wo + 100), gw(q2 + 84), gw(q2 + 88), gw(0x40004D54 + 4));
			fprintf(stderr, "[st2] m16=%08X m20=%08X m24=%u m28=%u m32=%08X m36=%08X m40=%u m48=%08X m52=%u m56=%08X m96=%u m364=%08X m368=%08X m372=%08X m376=%u | q3 prod=%u cons=%u ev3=%u | cpumask=%08X\n",
				gw(wo+16), gw(wo+20), gw(wo+24), gw(wo+28), gw(wo+32), gw(wo+36), gw(wo+40), gw(wo+48), gw(wo+52), gw(wo+56), gw(wo+96), gw(wo+364), gw(wo+368), gw(wo+372), gw(wo+376),
				gw(0x40004D84+84), gw(0x40004D84+88), gw(0x40004DC0+4), gw(dev+24200));
            fprintf(stderr, "[st3] intr issued=%u done=%u\n", dbg_intr_issued(), dbg_intr_done());
            { uint32_t t = gw(wo + 36);
              fprintf(stderr, "[st4] m36=%08X words(-4..+7):", t);
              if (t > 0x10000) for (int k = -4; k < 8; k++) fprintf(stderr, " %08X", gw(t + k * 4));   // 0 until the worker first runs: guest 0 is unmapped
              fprintf(stderr, "\n"); }
            { uint32_t ob = 0x40350200;
              fprintf(stderr, "[st5] obj+120..188:");
              for (int k = 120; k <= 188; k += 4) fprintf(stderr, " %08X", gw(ob + k));
              uint32_t p152 = gw(ob + 152), p172 = gw(ob + 172);
              fprintf(stderr, " | *152=%08X", p152);
              if (p152 > 0x10000) fprintf(stderr, "[%08X]", gw(p152));
              fprintf(stderr, " *172=%08X", p172);
              if (p172 > 0x10000) fprintf(stderr, "[%08X]", gw(p172));
              fprintf(stderr, "\n"); }
			dbg_dump_waits(1500);
            fflush(stderr);
        }
        return 0;
    }, nullptr, 0, nullptr);

	if (const char* dumpDir = getenv("BCR2_DUMPDIR")) {   // experiment: snapshot guest memory at fixed times
		static std::string s_dumpDir = dumpDir;
		CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
			const char* atEnv = getenv("BCR2_DUMPAT");     // optional snapshot times in seconds, "a,b,c" (ascending)
			int kAt[16] = { 10, 60 }, nAt = 2;
			if (atEnv) {
				nAt = 0;
				for (const char* s = atEnv; *s && nAt < 16; ) {
					char* e = nullptr;
					kAt[nAt++] = (int)strtol(s, &e, 10);
					s = (*e == ',') ? e + 1 : e;
					if (e == s && *e) break;
				}
			}
			const bool kVirtualOnly = nAt > 2;              // many snapshots: skip physical memory to save time
			int prev = 0;
			for (int ia = 0; ia < nAt; ia++) {
				int at = kAt[ia];
				if (at <= prev) continue;
				Sleep((at - prev) * 1000); prev = at;
				std::string path = s_dumpDir + "\\dump_" + std::to_string(at) + ".bin";
				FILE* f = fopen(path.c_str(), "wb");
				if (!f) { fprintf(stderr, "[dump] cannot open %s\n", path.c_str()); continue; }
				static const uint64_t kRanges[][2] = { { 0x40000000ull, 0x70000000ull }, { 0xA0000000ull, 0xC0000000ull } };
				uint64_t total = 0;
				for (auto& r : kRanges) {
					if (kVirtualOnly && r[0] >= 0xA0000000ull) continue;
					uint64_t a = r[0];
					while (a < r[1]) {
						MEMORY_BASIC_INFORMATION mi{};
						if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) break;
						uint64_t end = (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base);
						if (end > r[1]) end = r[1];
						if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
							uint32_t hdr[2] = { (uint32_t)a, (uint32_t)(end - a) };
							fwrite(hdr, 4, 2, f);
							fwrite(g_base + a, 1, (size_t)(end - a), f);
							total += end - a;
						}
						a = end;
					}
				}
				fclose(f);
				fprintf(stderr, "[dump] t=%ds wrote %llu MB to %s\n", at, (unsigned long long)(total >> 20), path.c_str());
			}
			return 0;
		}, nullptr, 0, nullptr);
	}

	CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
		fprintf(stderr, "[audit] thread started\n"); fflush(stderr);
		for (int k = 0; k < 3; k++) { Sleep(10000); fprintf(stderr, "[audit] t=%ds\n", (k + 1) * 10); pm4_audit(); }
		return 0;
	}, nullptr, 0, nullptr);

	CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
    Sleep(20000); pm4_audit();
    Sleep(15000); pm4_audit();
    return 0;
}, nullptr, 0, nullptr);
    {
        *(volatile uint32_t*)(g_base + 0xA0000100u) = 0x11223344u;
        fprintf(stderr, "[alias] A0=%08X C0=%08X E0=%08X\n",
                *(volatile uint32_t*)(g_base + 0xA0000100u),
                *(volatile uint32_t*)(g_base + 0xC0000100u),
                *(volatile uint32_t*)(g_base + 0xE0000100u));
        *(volatile uint32_t*)(g_base + 0xA0000100u) = 0;
    }
    if (!g_base) { fprintf(stderr, "could not reserve 4 GB\n"); return 1; }
    VirtualAlloc(g_base + 0x7FC80000, 0x10000, MEM_COMMIT, PAGE_READWRITE);
	VirtualAlloc(g_base + 0x7FEA0000, 0x10000, MEM_COMMIT, PAGE_READWRITE);   // XMA decoder registers (stubbed as plain RAM)

    // image + indirect-call table directly after it
    const uint64_t tableBytes = (PPC_CODE_SIZE * 2 + 0xFFFF) & ~0xFFFFull;
    commit((uint32_t)PPC_IMAGE_BASE, (size_t)(PPC_IMAGE_SIZE + tableBytes));
    commit(kStackBase, kStackSize);
    commit(kPcrBase, kPcrSize);
    commit(kKernelData, kKernelDataSize);
    commit(kThreadObj, kThreadObjSize);
    store_be32(kPcrBase + 0x100, kThreadObj);               // PCR: current thread object
    store_be32(kThreadObj + 0x14C, 1);                      // thread object: thread id
    store_be32(kPcrBase + 0x70, kStackBase + kStackSize);   // PCR: stack base (high)
    store_be32(kPcrBase + 0x74, kStackBase);                // PCR: stack limit (low)

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<char> img((std::istreambuf_iterator<char>(f)), {});
    if (img.size() < PPC_IMAGE_SIZE) { fprintf(stderr, "image too small: %zX\n", img.size()); return 1; }
    memcpy(g_base + PPC_IMAGE_BASE, img.data(), (size_t)PPC_IMAGE_SIZE);

    {   // default every call-table slot to the reporting stub
        uint64_t* t = (uint64_t*)(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
        for (uint64_t i = 0; i < PPC_CODE_SIZE / 4; i++) t[i] = (uint64_t)&UnresolvedIndirect;
    }

    // function table
    size_t n = 0;
    for (auto* m = PPCFuncMappings; m->host; ++m, ++n) {
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE) continue;
        *(PPCFunc**)(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (uint64_t(m->guest) - PPC_CODE_BASE) * 2) = m->host;
    }
    fprintf(stderr, "function table: %zu entries\n", n);

    // import slots: functions point at their thunk, variables get a zeroed block
    size_t cnt = 0; const ImportEntry* imps = host_import_table(cnt);
    uint32_t varAddr = kKernelData;
    for (size_t i = 0; i < cnt; i++) {
        if (imps[i].thunk) store_be32(imps[i].slot, imps[i].thunk);
        else { store_be32(imps[i].slot, varAddr); varAddr += 0x100; }
    }

    register_all_imports();
    if (argc > 2) set_content_root(argv[2]);
	
    // generous host stack: recompiled code recurses on the host stack
    HANDLE t = CreateThread(nullptr, 256ull << 20, GuestMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(t, INFINITE);
    return 0;
}