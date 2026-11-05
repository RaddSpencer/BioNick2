#include "ppc_config.h"
#include "ppc_context.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")

void shadow_dump();

extern thread_local PPCContext* g_ctx;
void watch_arm_if_in(uint32_t addr, uint32_t size);

using ImportFn = void (*)(PPCContext&, uint8_t*);

struct ImportEntry { uint32_t slot, thunk; int lib, ordinal; const char* name; };
static const ImportEntry kImports[] = {
#include "import_table.inc"
};
const ImportEntry* host_import_table(size_t& count) { count = sizeof(kImports) / sizeof(kImports[0]); return kImports; }

static std::unordered_map<uint32_t, ImportFn>& registry() { static std::unordered_map<uint32_t, ImportFn> r; return r; }
void set_content_root(const char* p);
static uint32_t key(int lib, int ord) { return (uint32_t(lib) << 16) | uint32_t(ord); }
static const char* import_name(int lib, int ord) {
    for (auto& e : kImports) if (e.lib == lib && e.ordinal == ord) return e.name;
    return "?";
}
static void register_by_name(const char* name, ImportFn fn) {
    for (auto& e : kImports) if (!strcmp(e.name, name) && e.thunk) { registry()[key(e.lib, e.ordinal)] = fn; return; }
    fprintf(stderr, "warning: import '%s' not in this title's table\n", name);
}

// ---- thread watchdog: samples each guest thread's registers every 5 s ----
static thread_local bool t_afterWake = false;
static uint8_t* g_wdBase = nullptr;
struct TraceSlot { std::atomic<PPCContext*> ctx{nullptr}; DWORD tid = 0; HANDLE h = nullptr; ULONGLONG lastCpu = 0; };
static TraceSlot g_slots[32];
static std::atomic<int> g_slotN{0};
static ULONGLONG ft64(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static void watchdog_thread() {
	int sample = 0;
    for (;;) {
        Sleep(3000);
		sample++;
        fprintf(stderr, "[watch] ---- cpu sample ----\n");
        int n = g_slotN.load();
        for (int i = 0; i < n && i < 32; i++) {
            TraceSlot& s = g_slots[i];
            if (!s.h) continue;
            DWORD code = 0;
            if (!GetExitCodeThread(s.h, &code) || code != STILL_ACTIVE) continue;       // thread gone: don't touch its context
            FILETIME c, e, k, u;
            if (!GetThreadTimes(s.h, &c, &e, &k, &u)) continue;
            ULONGLONG cpu = ft64(k) + ft64(u), d = cpu - s.lastCpu;
            s.lastCpu = cpu;
            PPCContext* x = s.ctx.load();
            if (x && d > 1000000)
                fprintf(stderr, "[watch] BUSY tid=%lu cpu=%llums lr=%08X r1=%08X r3=%08X\n",
                        s.tid, d / 10000, (uint32_t)x->lr, x->r1.u32, x->r3.u32);
            else if (x && sample == 2)
                fprintf(stderr, "[watch] idle tid=%lu lr=%08X r1=%08X r3=%08X\n",
                        s.tid, (uint32_t)x->lr, x->r1.u32, x->r3.u32);
        }
		if (g_wdBase) {
            auto R = [&](uint32_t a) { return _byteswap_ulong(*(uint32_t*)(g_wdBase + a)); };
            uint32_t fp = R(0x40002000 + 10896);
            fprintf(stderr, "[watch] gpu: fenceHW=%08X sub=%08X wptr=%08X rptr=%08X f11012=%08X f13408=%08X\n",
                    R(0xA0002000), R(0x40002000 + 10908), R(0x7FC80714), R(0x7FC80710),
                    R(0x40002000 + 11012), R(0x40002000 + 13408));
        }
    }
}
static void trace_register(PPCContext* c) {
    int i = g_slotN.fetch_add(1);
    if (i < 32) {
        g_slots[i].tid = GetCurrentThreadId();
        g_slots[i].h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_slots[i].tid);
        g_slots[i].ctx.store(c);
    }
    if (i == 0) std::thread(watchdog_thread).detach();
}

void ppc_import_dispatch(PPCContext& ctx, uint8_t* base, int lib, int ordinal) {
	g_wdBase = base;
    static thread_local bool t_registered = false;
    if (!t_registered) { t_registered = true; trace_register(&ctx); }
    auto it = registry().find(key(lib, ordinal));
    if (it != registry().end()) { it->second(ctx, base); return; }
    fprintf(stderr, "[imp] lib=%u ord=%u r3=%08X lr=%08X\n", (unsigned)lib, (unsigned)ordinal, ctx.r3.u32, (uint32_t)ctx.lr);
    fprintf(stderr, "UNIMPLEMENTED IMPORT: %s (%s ordinal %d) called from lr=%08X\n"
            "  r3=%08X r4=%08X r5=%08X r6=%08X\n",
            import_name(lib, ordinal), lib == 0 ? "xboxkrnl" : "xam", ordinal,
            (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

// ---- guest memory helpers (guest is big-endian) ----
static uint32_t rd32(uint8_t* base, uint32_t a) { return _byteswap_ulong(*(uint32_t*)(base + a)); }
static void wr32(uint8_t* base, uint32_t a, uint32_t v) { *(uint32_t*)(base + a) = _byteswap_ulong(v); }
static void wr64(uint8_t* base, uint32_t a, uint64_t v) { *(uint64_t*)(base + a) = _byteswap_uint64(v); }
static const uint32_t STATUS_SUCCESS_ = 0, STATUS_NO_MEMORY_ = 0xC0000017;
static const uint32_t STATUS_OBJECT_NAME_NOT_FOUND_ = 0xC0000034, STATUS_OBJECT_PATH_NOT_FOUND_ = 0xC000003A;
static const uint32_t STATUS_END_OF_FILE_ = 0xC0000011, STATUS_INVALID_HANDLE_ = 0xC0000008;
static const uint32_t STATUS_ACCESS_DENIED_ = 0xC0000022, STATUS_INVALID_INFO_CLASS_ = 0xC0000003;

// ---- virtual memory: simple bump allocator in 0x40000000..0x6FFFFFFF ----
static std::mutex g_vmLock;
static uint32_t g_vmNext = 0x40000000;
static const uint32_t kVmEnd = 0x70000000;

static void NtAllocateVirtualMemory_(PPCContext& ctx, uint8_t* base) {
    uint32_t baseP = ctx.r3.u32, sizeP = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t addr = rd32(base, baseP), size = rd32(base, sizeP);
    bool bigPages = (type & 0x60000000) != 0;
    uint32_t pageSize = bigPages ? 0x10000 : 0x1000;
    size = (size + pageSize - 1) & ~(pageSize - 1);
    std::lock_guard<std::mutex> g(g_vmLock);
    if (addr == 0 || addr == 0xFFFFFFFF) {
        addr = (g_vmNext + 0xFFFF) & ~0xFFFFu;
        if (uint64_t(addr) + ((size + 0xFFFF) & ~0xFFFFu) > kVmEnd) { ctx.r3.u64 = STATUS_NO_MEMORY_; return; }
        g_vmNext = addr + ((size + 0xFFFF) & ~0xFFFFu);
    } else {
        addr &= ~0xFFFu;
    }
    if (!VirtualAlloc(base + addr, size, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "NtAllocateVirtualMemory: commit %08X+%X failed (%lu)\n", addr, size, GetLastError());
        ctx.r3.u64 = STATUS_NO_MEMORY_; return;
    }
    watch_arm_if_in(addr, size);
    wr32(base, baseP, addr); wr32(base, sizeP, size);
    fprintf(stderr, "[mem] NtAllocateVirtualMemory -> %08X size %X type %08X\n", addr, size, type);
    ctx.r3.u64 = STATUS_SUCCESS_;
}


static void NtFreeVirtualMemory_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = STATUS_SUCCESS_; } // leaks for now

// MmQueryStatistics(MM_STATISTICS* out)  -- struct: u32 length, then page counts
static void MmQueryStatistics_(PPCContext& ctx, uint8_t* base) {
    uint32_t p = ctx.r3.u32;
    if (!p) { ctx.r3.u64 = 0xC000000D; return; }          // STATUS_INVALID_PARAMETER
    wr32(base, p + 0x00, 0x38);                           // length
    wr32(base, p + 0x04, 0x20000);                        // total physical pages (512 MB)
    wr32(base, p + 0x08, 0x01000);                        // kernel pages
    wr32(base, p + 0x0C, 0x18000);                        // pages available to the title
    wr32(base, p + 0x10, 0x20000000);                     // title total virtual bytes
    wr32(base, p + 0x14, 0x00100000);                     // title reserved virtual bytes
    wr32(base, p + 0x18, 0x08000);                        // title physical pages
    wr32(base, p + 0x1C, 0x00400);                        // title pool pages
    wr32(base, p + 0x20, 0x00040);                        // stack pages
    wr32(base, p + 0x24, 0x00800);                        // image pages
    wr32(base, p + 0x28, 0x01000);                        // heap pages
    wr32(base, p + 0x2C, 0x00400);                        // virtual pages
    wr32(base, p + 0x30, 0x00100);                        // page table pages
    wr32(base, p + 0x34, 0x00100);                        // cache pages
    ctx.r3.u64 = 0;                                       // STATUS_SUCCESS
}

// NtQueryVirtualMemory(base_address, MEMORY_BASIC_INFORMATION* out)
static void NtQueryVirtualMemory_(PPCContext& ctx, uint8_t* base) {
    uint32_t addr = ctx.r3.u32, out = ctx.r4.u32;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + addr, &mbi, sizeof(mbi))) { ctx.r3.u64 = 0xC0000005; return; }
    uint32_t rbase = (uint32_t)((uint8_t*)mbi.BaseAddress - base);
    uint64_t rsize = std::min<uint64_t>(mbi.RegionSize, 0x100000000ull - rbase);
    if (rsize > 0xFFFFFFFFull) rsize = 0xFFFFFFFFull;
    wr32(base, out + 0x00, rbase);                 // base_address
    wr32(base, out + 0x04, rbase & ~0xFFFFu);      // allocation_base
    wr32(base, out + 0x08, PAGE_READWRITE);        // allocation_protect
    wr32(base, out + 0x0C, (uint32_t)rsize);       // region_size
    wr32(base, out + 0x10, mbi.State);             // MEM_COMMIT/RESERVE/FREE share Windows values
    wr32(base, out + 0x14, mbi.Protect ? mbi.Protect : PAGE_READWRITE);
    wr32(base, out + 0x18, mbi.Type);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

// XGetVideoMode(XVIDEO_MODE* out)  -- void
static void XGetVideoMode_(PPCContext& ctx, uint8_t* base) {
    uint32_t p = ctx.r3.u32;
    if (!p) return;
    memset(base + p, 0, 0x30);
    wr32(base, p + 0x00, 1280);          // display width
    wr32(base, p + 0x04, 720);           // display height
    wr32(base, p + 0x08, 0);             // interlaced
    wr32(base, p + 0x0C, 1);             // widescreen
    wr32(base, p + 0x10, 1);             // HD
    wr32(base, p + 0x14, 0x42700000);    // refresh rate 60.0f (big-endian float bits)
    wr32(base, p + 0x18, 1);             // video standard
    wr32(base, p + 0x1C, 0x4A);
    wr32(base, p + 0x20, 0x01);
}

// ExTerminateThread(exit_code) -- ends the calling guest thread
static void ExTerminateThread_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[thread] ExTerminateThread code=%u thread=%lu\n", ctx.r3.u32, GetCurrentThreadId());
    ExitThread(ctx.r3.u32);
}

// MmAllocatePhysicalMemoryEx(type, size, protect, min_addr, max_addr, alignment) -> guest addr or 0
static uint32_t g_physBump = 0xA0000000;
static std::mutex g_physLock;
static std::unordered_map<uint32_t, uint32_t> g_physSizes;    // allocation address -> rounded size
static void MmAllocatePhysicalMemoryEx_(PPCContext& ctx, uint8_t* base) {
    uint32_t size  = ctx.r4.u32;
    uint32_t align = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    if (align & (align - 1)) align = 0x1000;                  // must be a power of two
    uint32_t sz = (size + 0xFFF) & ~0xFFFu;
    std::lock_guard<std::mutex> g(g_physLock);
    uint32_t addr = (g_physBump + align - 1) & ~(align - 1);
    if ((uint64_t)addr + sz > 0xC0000000ull) { ctx.r3.u64 = 0; return; }    // out of physical range
    g_physBump = addr + sz;
    g_physSizes[addr] = sz;
    fprintf(stderr, "[mem] MmAllocatePhysicalMemoryEx size=%X align=%X -> %08X\n", size, align, addr);
    ctx.r3.u32 = addr;
}
// MmQueryAllocationSize(addr) -> rounded size of the allocation, or 0 if unknown
static void MmQueryAllocationSize_(PPCContext& ctx, uint8_t*) {
    uint32_t addr = ctx.r3.u32, sz = 0;
    {
        std::lock_guard<std::mutex> g(g_physLock);
        auto it = g_physSizes.find(addr);
        if (it != g_physSizes.end()) sz = it->second;
    }
    if (!sz) fprintf(stderr, "[mem] MmQueryAllocationSize %08X -> unknown lr=%08X\n", addr, (uint32_t)ctx.lr);
    ctx.r3.u64 = sz;
}

#include "gpu.h"
// ---- GPU interface (stage 1: record parameters; a null GPU will consume them later) ----
struct GpuState {
    uint32_t intrCb = 0, intrCtx = 0;       // VdSetGraphicsInterruptCallback
    uint32_t rbBase = 0, rbSizeLog2 = 0;    // VdInitializeRingBuffer
    uint32_t wbAddr = 0, wbBlockLog2 = 0;   // VdEnableRingBufferRPtrWriteBack
    uint32_t idAddr = 0;                    // VdSetSystemCommandBufferGpuIdentifierAddress
} g_gpu;

// ---- null GPU: consumes PM4 packets, performs fence/write-back side effects, draws nothing ----
static uint64_t g_gpuCounter = 0;
static std::set<uint32_t> g_seenOps;
static std::atomic<uint32_t> g_cpIntrIssued{0}, g_cpIntrDone{0};   // CP interrupts queued / completed

unsigned dbg_intr_issued() { return (unsigned)g_cpIntrIssued.load(); }
unsigned dbg_intr_done()   { return (unsigned)g_cpIntrDone.load(); }

bool dbg_trace_on() { unsigned n = (unsigned)g_cpIntrIssued.load(); return n >= 3000 && n < 3014; }
unsigned dbg_trace_seq() { static std::atomic<unsigned> s{0}; return s.fetch_add(1); }

static uint32_t g_batchRptr = 0, g_batchMask = 0, g_batchLast = 0xFFFFFFFFu;
static thread_local uint32_t t_ibGa[8], t_ibSz[8];
// fence-theory trace: last-seen bin state (not acted on), and fence writes in the part of an IB we abandon
static uint32_t g_binMaskLo = 0, g_binMaskHi = 0, g_binSelLo = 0, g_binSelHi = 0;
static void note_lost(const uint32_t* p, uint32_t i, uint32_t n, int depth, const char* kind) {
    static std::atomic<int> nl{0};
    if (nl++ >= 1500) return;
    uint32_t t3 = 0;
    for (uint32_t k = i; k < n; k++) if ((p[k] >> 30) == 3) t3++;
    fprintf(stderr, "[lost] %s depth=%d ib=%08X i=%u n=%u hdr=%08X rest=%u restC=%u fences:", kind, depth, depth ? t_ibGa[depth & 7] : 0u, i, n, p[i], n - i, t3);
    for (uint32_t k = i; k + 3 < n; k++)
        if ((p[k] & 0xFFFFFFFEu) == 0xC0025800u && (p[k + 2] & 0x1FFFFFFCu) == 0x2000u) fprintf(stderr, " %X", p[k + 3]);
    fprintf(stderr, "\n");
}
static void publish_rptr(uint8_t* base, uint32_t rp) {
    PPC_STORE_U32(0x7FC80710, rp);                                   // CP_RB_RPTR register
    if (g_gpu.wbAddr) PPC_STORE_U32(0xA0000000u | g_gpu.wbAddr, rp); // read-pointer write-back
}

// ---- in-order fence commit ----
struct FenceQ {
    std::mutex m;
    std::map<uint32_t, uint32_t> pend;                 // held fence value -> pointer word that came with it
    std::chrono::steady_clock::time_point last{std::chrono::steady_clock::now()};
    std::atomic<bool> started{false};
};
static FenceQ g_fq;

// caller holds g_fq.m
static void fence_commit(uint8_t* base, uint32_t v, uint32_t ptrw) {
    PPC_STORE_U32(0xA0002004u, ptrw);
    PPC_STORE_U32(0xA0002000u, v);
    g_fq.last = std::chrono::steady_clock::now();
    for (;;) {                                          // release any held values that are now next in line
        while (!g_fq.pend.empty() && g_fq.pend.begin()->first <= v) g_fq.pend.erase(g_fq.pend.begin());
        auto it = g_fq.pend.find(v + 2);
        if (it == g_fq.pend.end()) break;
        v = it->first;
        PPC_STORE_U32(0xA0002004u, it->second);
        PPC_STORE_U32(0xA0002000u, v);
        g_fq.pend.erase(it);
    }
}

// if the gap never closes, publish the lowest held value so the game cannot deadlock
static void fence_watchdog(uint8_t* base) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lk(g_fq.m);
        if (g_fq.pend.empty()) continue;
        if (std::chrono::steady_clock::now() - g_fq.last < std::chrono::milliseconds(200)) continue;
        auto it = g_fq.pend.begin();
        static int nf = 0;
        if (nf++ < 200) fprintf(stderr, "[fw] FORCED %X (cur=%X, held=%zu)\n", it->first, PPC_LOAD_U32(0xA0002000u), g_fq.pend.size());
        uint32_t v = it->first, pw = it->second;
        g_fq.pend.erase(it);
        fence_commit(base, v, pw);
    }
}

static void gpu_exec(uint8_t* base, const uint32_t* p, uint32_t n, int depth) {
    static const bool kGpuOn = gpu_enabled();
    uint32_t i = 0;
    uint32_t hist_i[8] = {0}, hist_h[8] = {0}; unsigned hn = 0;     // last 8 packet starts (debug)
    while (i < n) {
        if (depth == 0) {
            uint32_t rp = (g_batchRptr + i) & g_batchMask;
            if (rp != g_batchLast) { g_batchLast = rp; publish_rptr(base, rp); }
        }
        uint32_t hdr = p[i], type = hdr >> 30;
        hist_i[hn & 7] = i; hist_h[hn & 7] = hdr; hn++;
        if (type == 0) {                                                     // register writes
            uint32_t reg = hdr & 0x7FFF, cw = ((hdr >> 16) & 0x3FFF) + 1;
            bool one = ((hdr >> 15) & 1) != 0;
            if (i + 1 + cw > n) {
                static int ntr0 = 0;
                if (ntr0++ < 8) {
                    fprintf(stderr, "[gpu] truncated type-0 packet depth=%d ibGa=%08X ibSz=%u i=%u n=%u hdr=%08X ring=%X words(-8..+8):",
                            depth, depth ? t_ibGa[depth & 7] : 0u, depth ? t_ibSz[depth & 7] : 0u, i, n, hdr, (g_batchRptr + i) & g_batchMask);
                    for (int64_t k = (int64_t)i - 8; k <= (int64_t)i + 8; k++) {
                        if (k >= 0 && k < (int64_t)n) fprintf(stderr, " %08X", p[k]);
                        else fprintf(stderr, " --------");
                    }
                    fprintf(stderr, "\n");
                    fprintf(stderr, "[gpu]   prev packets (i:hdr):");
                    if (ntr0 <= 3) {
                        fprintf(stderr, "[gpu]   raw[i-48..i+2]:");
                        for (int64_t k = (int64_t)i - 48; k <= (int64_t)i + 2; k++)
                            if (k >= 0 && k < (int64_t)n) fprintf(stderr, " %08X", p[k]);
                        fprintf(stderr, "\n");
                    }
                    for (unsigned k = 0; k < 8 && k < hn; k++) { unsigned s = (hn - 1 - k) & 7; fprintf(stderr, " %u:%08X", hist_i[s], hist_h[s]); }
                    fprintf(stderr, "\n[gpu]   head:");
                    for (uint32_t k = 0; k < 12 && k < n; k++) fprintf(stderr, " %08X", p[k]);
                    if (depth) {
                        uint32_t diff = 0, first = 0xFFFFFFFFu;
                        for (uint32_t k = 0; k < n; k++)
                            if (PPC_LOAD_U32(t_ibGa[depth & 7] + k * 4) != p[k]) { if (first == 0xFFFFFFFFu) first = k; diff++; }
                        fprintf(stderr, "\n[gpu]   live-diff: %u of %u dwords differ now (first at %d)", diff, n, (int)first);
                    }
                    fprintf(stderr, "\n");
                }
                note_lost(p, i, n, depth, "type0");
                return;
            }
            static bool scratchMapped = false;
            if (!scratchMapped) { scratchMapped = true; VirtualAlloc(base + 0xA0003000u, 0x1000, MEM_COMMIT, PAGE_READWRITE); }
            for (uint32_t k = 0; k < cw; k++) {
                uint32_t r = one ? reg : reg + k, val = p[i + 1 + k];
                if (kGpuOn) gpu_reg(r, val);
                if (r >= 0x578 && r <= 0x57F) {                            // CP scratch regs are mirrored to memory at phys 0x3000
                    uint32_t a = 0xA0003000u + (r - 0x578) * 4;
                    PPC_STORE_U32(a, val);
                    PPC_STORE_U32(0x7FC80000u + r * 4, val);                      // also expose scratch regs on the MMIO page
                    static int logged = 0;
                    if (logged++ < 12) fprintf(stderr, "[gpu] scratch reg %03X = %08X -> wrote at %08X\n", r, val, a);
                }
            }
            i += 1 + cw;
            continue;
        }
        if (type == 1)      {
            if (kGpuOn && i + 2 < n) { gpu_reg(hdr & 0x7FF, p[i + 1]); gpu_reg((hdr >> 11) & 0x7FF, p[i + 2]); }
            i += 3; continue;
        }
        if (type == 2)      { i += 1; continue; }                              // nop
        uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1, op = (hdr >> 8) & 0x7F;
        const uint32_t* b = p + i + 1;
        if (i + 1 + cnt > n) { fprintf(stderr, "[gpu] truncated packet op=%02X\n", op); note_lost(p, i, n, depth, "type3"); return; }
        if (hdr & 1) {                                  // log only: what a predicate check would decide here
            static uint32_t predFire[128], predSkip[128]; static uint32_t predN = 0;
            bool fire = (((uint64_t)g_binSelHi << 32 | g_binSelLo) & ((uint64_t)g_binMaskHi << 32 | g_binMaskLo)) != 0;
            (fire ? predFire : predSkip)[op]++;
            if ((++predN % 5000) == 0) {
                fprintf(stderr, "[pred] n=%u:", predN);
                for (int k = 0; k < 128; k++) if (predFire[k] || predSkip[k]) fprintf(stderr, " %02X=%u/%u", k, predFire[k], predSkip[k]);
                fprintf(stderr, "\n");
            }
            // predicated packet: skip unless select & mask != 0 (BCR2_NOPREDICATE restores the old always-run behaviour)
            static const bool kPredicate = getenv("BCR2_NOPREDICATE") == nullptr;
            if (kPredicate && !fire) { i += 1 + cnt; continue; }
        }
        if (op >= 0x60 && op <= 0x63) {
            static std::atomic<int> nb{0}, nbt{0};
            bool tiledIb = depth == 1 && t_ibSz[1] == 0xEA7;
            if (nb++ < 300 || (tiledIb && nbt++ < 5000))
                fprintf(stderr, "[bin] op=%02X val=%08X hdr=%08X depth=%d ib1=%08X i=%u\n", op, b[0], hdr, depth, depth >= 1 ? t_ibGa[1] : 0u, i);
        }
        if (op == 0x60) g_binMaskLo = b[0]; else if (op == 0x61) g_binMaskHi = b[0];
        else if (op == 0x62) g_binSelLo = b[0]; else if (op == 0x63) g_binSelHi = b[0];
        if (g_seenOps.insert(op).second)
            fprintf(stderr, "[gpu] first packet op=0x%02X body=%u dwords (depth %d)\n", op, cnt, depth);
        if (kGpuOn) gpu_packet(base, op, b, cnt);       // renderer: state, shaders, draws, resolves, present
        switch (op) {
          case 0x3F: case 0x37: {                         // INDIRECT_BUFFER(_PFD)
            uint32_t ga = 0xA0000000u | (b[0] & ~3u), sz = b[1] & 0xFFFFF;
            if (dbg_trace_on()) fprintf(stderr, "[tr] #%u gpu IB ga=%08X sz=%X depth=%d\n", dbg_trace_seq(), ga, sz, depth);
            if (depth < 4 && sz < 0x100000) {
                std::vector<uint32_t> v(sz);
                for (uint32_t k = 0; k < sz; k++) { uint32_t a = ga + k * 4; v[k] = PPC_LOAD_U32(a); }
				t_ibGa[(depth + 1) & 7] = ga; t_ibSz[(depth + 1) & 7] = sz;
                {   // track whether the same IB address keeps its content between executions
                    static std::mutex ibm; static std::map<uint32_t, std::pair<uint32_t, uint32_t>> ibs;   // ga -> (size, hash)
                    static int nchg = 0, nsame = 0;
                    uint32_t h = 2166136261u; for (uint32_t k = 0; k < sz; k++) h = (h ^ v[k]) * 16777619u;
                    std::lock_guard<std::mutex> lk(ibm);
                    auto it = ibs.find(ga);
                    if (it == ibs.end()) { /* first time */ }
                    else if (it->second.first != sz || it->second.second != h) {
                        if (nchg++ < 40) fprintf(stderr, "[ibchg] ga=%08X sz %X->%X hash %08X->%08X depth=%d head=%08X %08X %08X %08X\n",
                                                 ga, it->second.first, sz, it->second.second, h, depth, v.size() > 0 ? v[0] : 0, v.size() > 1 ? v[1] : 0, v.size() > 2 ? v[2] : 0, v.size() > 3 ? v[3] : 0);
                    } else if (nsame++ < 10) fprintf(stderr, "[ibsame] ga=%08X sz=%X executed again with identical content\n", ga, sz);
                    ibs[ga] = { sz, h };
                }
                gpu_exec(base, v.data(), sz, depth + 1);
            }
            break; }
          case 0x58: {                                    // EVENT_WRITE_SHD: fence / timestamp write
            uint32_t init = b[0], ga = 0xA0000000u | (b[1] & ~3u);
            uint32_t data = ((init >> 31) & 1) ? (uint32_t)(++g_gpuCounter) : b[2];
            static int shdLogged = 0;
            if (shdLogged++ < 20)
                fprintf(stderr, "[gpu] EVENT_WRITE_SHD init=%08X addr=%08X data=%08X -> wrote %08X at %08X\n",
                        b[0], b[1], b[2], data, ga);
            static uint32_t s_prev4 = 0;                    // value of the pointer word before its latest write
            uint32_t pa = ga & 0x1FFFFFFCu;
            if (pa == 0x2004u) s_prev4 = PPC_LOAD_U32(ga);
            if (pa == 0x2000u) {
                static std::atomic<int> nfc{0};
                int q = nfc++;
                {   // uncapped totals, for long runs
                    static uint32_t totStale = 0, totJump = 0;
                    uint32_t cur = PPC_LOAD_U32(ga);
                    if (data < cur) totStale++; else if (data > cur + 2) totJump++;
                    if (((q + 1) % 2000) == 0)
                        fprintf(stderr, "[fsum] writes=%d stale=%u jumps=%u fence=%X t=%lu\n", q + 1, totStale, totJump, cur, GetTickCount());
                }
                if (q < 8000)
                    fprintf(stderr, "[fc] #%d t=%lu d=%X old=%X hdr=%08X depth=%d ib1=%08X ib2=%08X i=%u ring=%X sub=%X wptr=%X mask=%08X sel=%08X\n",
                            q, GetTickCount(), data, PPC_LOAD_U32(ga), hdr, depth, depth >= 1 ? t_ibGa[1] : 0u, depth >= 2 ? t_ibGa[2] : 0u, i,
                            (g_batchRptr + i) & g_batchMask, PPC_LOAD_U32(0x40002000 + 10908), PPC_LOAD_U32(0x7FC80714),
                            g_binMaskLo, g_binSelLo);
            }
            if (pa == 0x2000u) {                            // fence: publish strictly in order
                std::lock_guard<std::mutex> lk(g_fq.m);
                if (!g_fq.started.exchange(true)) std::thread(fence_watchdog, base).detach();
                uint32_t oldv = PPC_LOAD_U32(ga);
                uint32_t ptrw = PPC_LOAD_U32(0xA0002004u);  // pointer word written just before this
                static int fw = 0;
                if (oldv < 0x01000000u && data < oldv) {    // stale write
                    PPC_STORE_U32(0xA0002004u, s_prev4);
                    if (fw++ < 4000) fprintf(stderr, "[fw] %X old=%X STALE\n", data, oldv);
                    break;
                }
                if (false && oldv < 0x01000000u && data > oldv && data != oldv + 2) {   // gap: hold it (disabled)
                    g_fq.pend[data] = ptrw;
                    PPC_STORE_U32(0xA0002004u, s_prev4);
                    if (fw++ < 4000) fprintf(stderr, "[fw] %X old=%X HELD (n=%zu)\n", data, oldv, g_fq.pend.size());
                    break;
                }
                if (data != oldv && fw++ < 4000) fprintf(stderr, "[fw] %X old=%X\n", data, oldv);
                fence_commit(base, data, ptrw);
                break;
            }
            PPC_STORE_U32(ga, data);
            break; }
          case 0x3C: {                                    // WAIT_REG_MEM: poll until the CPU acknowledges
            uint32_t fn = b[0] & 7, ref = b[2], msk = b[3];
            bool mem = (b[0] & 0x10) != 0;
            if (!mem) break;                              // register polls are not emulated
            uint32_t ga = 0xA0000000u | ((b[1] & ~3u) & 0x1FFFFFFFu);
            auto ok = [&](uint32_t v) -> bool {
                v &= msk;
                switch (fn) { case 0: return false; case 1: return v < ref; case 2: return v <= ref;
                              case 3: return v == ref; case 4: return v != ref; case 5: return v >= ref;
                              case 6: return v > ref; default: return true; }
            };
            static int wl = 0;
            uint32_t cur = PPC_LOAD_U32(ga);
            if (wl < 12) { wl++; fprintf(stderr, "[wrm] fn=%u mem=%u ga=%08X ref=%X mask=%X cur=%X\n", fn, (unsigned)mem, ga, ref, msk, cur); }
            auto t0 = std::chrono::steady_clock::now();
            int w = 0;
            while (!ok(PPC_LOAD_U32(ga))) {
                if (w++ < 2000) std::this_thread::yield();
                else std::this_thread::sleep_for(std::chrono::microseconds(100));
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) {
                    static int to = 0;
                    if (to++ < 10) fprintf(stderr, "[wrm] TIMEOUT ga=%08X ref=%X cur=%X fn=%u\n", ga, ref, PPC_LOAD_U32(ga), fn);
                    break;
                }
            }
            break; }
          case 0x54: {                                    // INTERRUPT: block until the guest ISR has run
            static int logged = 0; if (logged++ < 8) fprintf(stderr, "[gpu] INTERRUPT packet cpumask=%08X\n", b[0]);
            {
                static std::atomic<int> nis{0};
                if (nis++ < 4000)
                    fprintf(stderr, "[intr-src] hdr=%08X depth=%d ib1=%08X i=%u cpumask=%X scr57C=%08X scr57D=%08X fence=%X mask=%08X:%08X sel=%08X:%08X\n",
                            hdr, depth, depth >= 1 ? t_ibGa[1] : 0u, i, b[0],
                            PPC_LOAD_U32(0xA0003010u), PPC_LOAD_U32(0xA0003014u), PPC_LOAD_U32(0xA0002000u),
                            g_binMaskHi, g_binMaskLo, g_binSelHi, g_binSelLo);
            }
            static uint32_t intrN = 0; intrN++;
            if (dbg_trace_on()) fprintf(stderr, "[tr] #%u gpu INTERRUPT scr57C=%08X ctx=%08X i=%u\n", dbg_trace_seq(), PPC_LOAD_U32(0xA0003010u), PPC_LOAD_U32(0xA0003014u), i);
            if (intrN >= 20000 && intrN < 20030) {
                uint32_t is = PPC_LOAD_U32(0x40002000 + 10900);
                fprintf(stderr, "[intr] n=%u depth=%d ibGa=%08X ibSz=%X i=%u ring=%X cpumask=%X istr20=%08X sub=%X fence=%X\n",
                        intrN, depth, depth ? t_ibGa[depth & 7] : 0u, depth ? t_ibSz[depth & 7] : 0u, i,
                        (g_batchRptr + i) & g_batchMask, b[0], is > 0x10000 ? PPC_LOAD_U32(is + 20) : 0u,
                        PPC_LOAD_U32(0x40002000 + 10908), PPC_LOAD_U32(PPC_LOAD_U32(0x40002000 + 10896)));
            }
            if (intrN < 12 || (intrN >= 500 && intrN < 512) || (intrN >= 3000 && intrN < 3012)) {
                uint32_t is = PPC_LOAD_U32(0x40002000 + 10900);
                fprintf(stderr, "[intr2] n=%u depth=%d ibGa=%08X i=%u scr578=%X scr579=%X scr57C=%08X scr57D=%08X istr16=%08X istr20=%08X sub=%X fence=%X\n",
                        intrN, depth, depth ? t_ibGa[depth & 7] : 0u, i,
                        PPC_LOAD_U32(0xA0003000u), PPC_LOAD_U32(0xA0003004u), PPC_LOAD_U32(0xA0003010u), PPC_LOAD_U32(0xA0003014u),
                        is > 0x10000 ? PPC_LOAD_U32(is + 16) : 0u, is > 0x10000 ? PPC_LOAD_U32(is + 20) : 0u,
                        PPC_LOAD_U32(0x40002000 + 10908), PPC_LOAD_U32(PPC_LOAD_U32(0x40002000 + 10896)));
            }
            if (g_gpu.intrCb) {
                uint32_t t = g_cpIntrIssued.fetch_add(1) + 1;
                for (int w = 0; w < 20000 && g_cpIntrDone.load() < t; w++) {
                    if (w < 2000) std::this_thread::yield();
                    else std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                if (g_cpIntrDone.load() < t) fprintf(stderr, "[gpu] INTERRUPT: ISR did not complete in time\n");
                if (intrN == 20000 || intrN == 20002) {
                    fprintf(stderr, "[ibhead] n=%u:", intrN);
                    for (uint32_t k = 0; k < 24 && k < n; k++) fprintf(stderr, " %08X", p[k]);
                    fprintf(stderr, "\n");
                    fprintf(stderr, "[ibintr] n=%u (i-40..i+13):", intrN);
                    for (int64_t k = (int64_t)i - 40; k < (int64_t)i + 14; k++)
                        if (k >= 0 && k < (int64_t)n) fprintf(stderr, " %08X", p[k]);
                    fprintf(stderr, "\n");
                }
            }
            break; }
          case 0x64: {                                    // XE_SWAP (frame present)
            static int swapLogged = 0;
            if (swapLogged++ < 20) fprintf(stderr, "[gpu] SWAP packet\n");
            break; }
        }
        i += 1 + cnt;
    }
}

// ---- vsync: calls the game's graphics interrupt callback (source 0) 60 times a second ----
static void vsync_thread(uint8_t* base) {
    const uint32_t PCR = 0x7E000000, TOBJ = 0x7E010000, STK = 0x7E100000, STKSZ = 0x40000;
    VirtualAlloc(base + PCR, 0x20000, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(base + STK, STKSZ,   MEM_COMMIT, PAGE_READWRITE);
    fprintf(stderr, "[thr] vsync tid=%lu\n", GetCurrentThreadId());
    wr32(base, PCR + 0x100, TOBJ);            // PCR -> thread object
    wr32(base, TOBJ + 0x14C, 0x99);           // thread id
    wr32(base, PCR + 0x70, STK + STKSZ);      // stack base
    wr32(base, PCR + 0x74, STK);              // stack limit
    PPCContext ctx{};
    uint64_t n = 0;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!g_gpu.intrCb) continue;


        auto now = std::chrono::steady_clock::now();
        if (now >= next) {                                    // vsync, 60 Hz
            next += std::chrono::microseconds(16667);
            if (next < now) next = now + std::chrono::microseconds(16667);
            ctx.r1.u32  = STK + STKSZ - 0x100;
            ctx.r13.u32 = PCR;
            *(volatile uint32_t*)(base + 0x7FC86544) = _byteswap_ulong(1);
            ctx.r3.u32  = 0;
            ctx.r4.u32  = g_gpu.intrCtx;
            ctx.lr      = 0;
            uint32_t fn = g_gpu.intrCb;
            PPC_CALL_INDIRECT_FUNC(fn);
            if (++n <= 3) fprintf(stderr, "[gpu] vsync interrupt #%llu delivered\n", (unsigned long long)n);
        }
    }
}
// ---- CP interrupt delivery (source 1) on its own guest thread ----
static void cp_intr_thread(uint8_t* base) {
    const uint32_t PCR = 0x7E200000, TOBJ = 0x7E210000, STK = 0x7E300000, STKSZ = 0x40000;
    VirtualAlloc(base + PCR, 0x20000, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(base + STK, STKSZ,   MEM_COMMIT, PAGE_READWRITE);
    fprintf(stderr, "[thr] cp_intr tid=%lu\n", GetCurrentThreadId());
    wr32(base, PCR + 0x100, TOBJ);
    wr32(base, TOBJ + 0x14C, 0x9A);           // thread id
    wr32(base, PCR + 0x70, STK + STKSZ);
    wr32(base, PCR + 0x74, STK);
    *(volatile uint8_t*)(base + PCR + 268) = 2;   // processor number: INTERRUPT cpumask 4 = CPU 2
    PPCContext ctx{};
    int idleSpins = 0;
    for (;;) {
		{
            static uint32_t lastC = 0x7FFFFFFFu; static int nlogC = 0;
            uint32_t vC = PPC_LOAD_U32(0x40002000 + 11012);
            if (vC != lastC) {
                if (nlogC++ < 600)
                    fprintf(stderr, "[c11012] %08X -> %08X intrDone=%u\n", lastC, vC, (unsigned)g_cpIntrDone.load());
                lastC = vC;
            }
        }
        if (!g_gpu.intrCb || g_cpIntrDone.load() >= g_cpIntrIssued.load()) {
            if (idleSpins < 20000) { idleSpins++; std::this_thread::yield(); }
            else std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        idleSpins = 0;
        ctx.r1.u32  = STK + STKSZ - 0x100;
        ctx.r13.u32 = PCR;
        ctx.r3.u32  = 1;
        ctx.r4.u32  = g_gpu.intrCtx;
        ctx.lr      = 0;
        {
            static uint32_t seen[2] = {0, 0};
            static int nseen = 0;
            uint32_t is = PPC_LOAD_U32(g_gpu.intrCtx + 10900);
            if (is > 0x10000 && is < 0xE0000000u) {
                uint32_t cb = PPC_LOAD_U32(is + 16), cx = PPC_LOAD_U32(is + 20);
                if (cb == 0x826A3158u && cx > 0x10000 && cx < 0xE0000000u) {
                    bool known = false;
                    for (int i = 0; i < nseen; i++) if (seen[i] == cx) known = true;
                    if (!known && nseen < 2) {
                        seen[nseen++] = cx;
                        uint32_t e = PPC_LOAD_U32(cx);
                        fprintf(stderr, "[isr3] ctx=%08X end=%08X tail(ctx+120..):", cx, e);
                        for (int k = 30; k < 100; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(cx + k * 4));
                        fprintf(stderr, "\n");
                        if (e > 0x10000 && e < 0xE0000000u) {
                            fprintf(stderr, "[isr3] after-end @%08X:", e);
                            for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(e + k * 4));
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
        }
        uint32_t fn = g_gpu.intrCb;
        PPC_CALL_INDIRECT_FUNC(fn);
        g_cpIntrDone.fetch_add(1);
    }
}
static std::atomic<bool> g_vsyncStarted{false};

static void gpu_thread(uint8_t* base) {
    const uint32_t ringGa = 0xA0000000u | g_gpu.rbBase;
    const uint32_t ringDw = (8u << g_gpu.rbSizeLog2) / 4, mask = ringDw - 1;
    uint32_t rptr = 0;
    for (;;) {
        uint32_t ra = 0x7FC80714; uint32_t wptr = PPC_LOAD_U32(ra) & mask;
        if (wptr != rptr) {
            uint32_t pend = (wptr - rptr) & mask;
            static uint32_t hw[16], hr[16]; static unsigned hn = 0; static uint32_t lastw = 0xFFFFFFFFu;
            if (wptr != lastw) { hw[hn & 15] = wptr; hr[hn & 15] = rptr; hn++; lastw = wptr; }
            if (((rptr - wptr) & mask) < 0x100) {           // WPTR went backwards: would replay almost the whole ring
                static int bk = 0; static unsigned nreg = 0;
                nreg++;
                if (bk++ < 6) {
                    if (bk == 1) shadow_dump();
                    fprintf(stderr, "[gpu] wptr BEHIND rptr: wptr=%X rptr=%X history(wptr/rptr):", wptr, rptr);
                    for (unsigned k = 0; k < 16; k++) fprintf(stderr, " %X/%X", hw[(hn + k) & 15], hr[(hn + k) & 15]);
                    fprintf(stderr, "\n");
                } else if ((nreg % 50) == 0) {
                    fprintf(stderr, "[gpu] wptr regress total=%u\n", nreg);
                }
                rptr = wptr;                                // adopt the game's pointer, don't re-execute stale ring data
                PPC_STORE_U32(0x7FC80710, rptr);
                if (g_gpu.wbAddr) PPC_STORE_U32(0xA0000000u | g_gpu.wbAddr, rptr);
                continue;
            }
            std::vector<uint32_t> v(pend);
            for (uint32_t k = 0; k < pend; k++) { uint32_t a = ringGa + ((rptr + k) & mask) * 4; v[k] = PPC_LOAD_U32(a); }
            if (!g_vsyncStarted.exchange(true)) {
                std::thread(vsync_thread, base).detach();
                std::thread(cp_intr_thread, base).detach();
            }
            g_batchRptr = rptr; g_batchMask = mask; g_batchLast = 0xFFFFFFFFu;
            gpu_exec(base, v.data(), pend, 0);
            rptr = wptr;
            uint32_t wb = 0xA0000000u | g_gpu.wbAddr;     // read-pointer write-back
            if (g_gpu.wbAddr) PPC_STORE_U32(wb, rptr);
            uint32_t rr = 0x7FC80710; PPC_STORE_U32(rr, rptr);   // CP_RB_RPTR register too
        }
        Sleep(1);
    }
}
static std::atomic<bool> g_gpuWatchStarted{false};

static void VdSetGraphicsInterruptCallback_(PPCContext& ctx, uint8_t*) {
    g_gpu.intrCb = ctx.r3.u32; g_gpu.intrCtx = ctx.r4.u32;
	fprintf(stderr, "[gpu] intrCb=%08X ctx=%08X\n", g_gpu.intrCb, g_gpu.intrCtx);
    fprintf(stderr, "[gpu] interrupt callback=%08X ctx=%08X\n", g_gpu.intrCb, g_gpu.intrCtx);
}
static void VdInitializeRingBuffer_(PPCContext& ctx, uint8_t* base) {
    g_gpu.rbBase = ctx.r3.u32; g_gpu.rbSizeLog2 = ctx.r4.u32;
    fprintf(stderr, "[gpu] ring buffer base=%08X size=2^%u\n", g_gpu.rbBase, g_gpu.rbSizeLog2);
    if (!g_gpuWatchStarted.exchange(true))
    std::thread(gpu_thread, base).detach();
}
static void VdEnableRingBufferRPtrWriteBack_(PPCContext& ctx, uint8_t*) {
    g_gpu.wbAddr = ctx.r3.u32; g_gpu.wbBlockLog2 = ctx.r4.u32;
    fprintf(stderr, "[gpu] rptr writeback addr=%08X block=2^%u\n", g_gpu.wbAddr, g_gpu.wbBlockLog2);
}
static void VdSetSystemCommandBufferGpuIdentifierAddress_(PPCContext& ctx, uint8_t*) {
    g_gpu.idAddr = ctx.r3.u32;
    fprintf(stderr, "[gpu] identifier addr=%08X\n", g_gpu.idAddr);
}

// MmGetPhysicalAddress(addr) -> addr & 0x1FFFFFFF
static void MmGetPhysicalAddress_(PPCContext& ctx, uint8_t*) {
    ctx.r3.u32 &= 0x1FFFFFFF;
}

// KeInitializeDpc(KDPC* dpc, routine, context)  -- void
static void KeInitializeDpc_(PPCContext& ctx, uint8_t* base) {
    uint32_t p = ctx.r3.u32;
    memset(base + p, 0, 0x1C);
    base[p + 0] = 19;                       // object type: DPC
    wr32(base, p + 0x0C, ctx.r4.u32);       // deferred routine
    wr32(base, p + 0x10, ctx.r5.u32);       // context
}

// ---- dispatcher objects living in guest memory (KEVENT / KSEMAPHORE / KTHREAD) ----
static std::mutex g_kwLock;
// ---- blocked-wait tracker (diagnostics) ----
struct WaitRec { bool used; DWORD tid; uint32_t obj, lr; ULONGLONG t0; const char* kind; };
static WaitRec g_waits[64];
static std::mutex g_waitsLock;
static int wait_begin(const char* kind, uint32_t obj, uint32_t lr) {
    std::lock_guard<std::mutex> g(g_waitsLock);
    for (int i = 0; i < 64; i++) if (!g_waits[i].used) { g_waits[i] = { true, GetCurrentThreadId(), obj, lr, GetTickCount64(), kind }; return i; }
    return -1;
}
static void wait_end(int s) { if (s < 0) return; std::lock_guard<std::mutex> g(g_waitsLock); g_waits[s].used = false; }
struct WaitScope { int s; WaitScope(const char* k, uint32_t o, uint32_t lr) : s(wait_begin(k, o, lr)) {} ~WaitScope() { wait_end(s); } };
void dbg_dump_waits(unsigned minMs) {
    std::lock_guard<std::mutex> g(g_waitsLock);
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 64; i++) if (g_waits[i].used && now - g_waits[i].t0 >= minMs)
        fprintf(stderr, "[blocked] tid=%lu %s obj=%08X lr=%08X %llums\n", g_waits[i].tid, g_waits[i].kind, g_waits[i].obj, g_waits[i].lr, (unsigned long long)(now - g_waits[i].t0));
}

// KeResetEvent(event*) -> previous signal state
static void KeResetEvent_(PPCContext& ctx, uint8_t* base) {
    uint32_t obj = ctx.r3.u32;
    std::lock_guard<std::mutex> g(g_kwLock);
    uint32_t prev = PPC_LOAD_U32(obj + 4);
    PPC_STORE_U32(obj + 4, 0);
    ctx.r3.u32 = prev;
}

// Try to satisfy the wait on obj once. Returns true if acquired.
static bool kw_try_acquire(uint8_t* base, uint32_t obj) {
    std::lock_guard<std::mutex> g(g_kwLock);
    uint8_t type = base[obj];
    int32_t state = (int32_t)PPC_LOAD_U32(obj + 4);
    switch (type) {
      case 1:  // synchronization event: consume the signal
        if (state != 0) { PPC_STORE_U32(obj + 4, 0); return true; }
        return false;
      case 5:  // semaphore: take one count
        if (state > 0) { PPC_STORE_U32(obj + 4, (uint32_t)(state - 1)); return true; }
        return false;
      default: // notification event / everything else: signaled if state != 0
        return state != 0;
    }
}

// KeWaitForSingleObject(obj, reason, mode, alertable, timeout*)
static void KeWaitForSingleObject_(PPCContext& ctx, uint8_t* base) {
    uint32_t obj = ctx.r3.u32, tmo = ctx.r7.u32;
	WaitScope ws("KeWait", obj, (uint32_t)ctx.lr);
    static std::atomic<int> logged{0};
    bool watchObj = (obj == 0x40004D54 || obj == 0x40004DC0);
    static std::atomic<int> wlog{0};
    int wl = watchObj ? wlog.fetch_add(1) : 1000;
    if (wl < 40)
        fprintf(stderr, "[%lu] [wait-enter] obj=%08X state=%d tid=%lu\n",
                GetTickCount(), obj, (int)PPC_LOAD_U32(obj + 4), GetCurrentThreadId());
    if (t_afterWake && !watchObj) {
        static std::atomic<int> alog{0};
        if (alog.fetch_add(1) < 30)
            fprintf(stderr, "[%lu] [post-wake wait] tid=%lu obj=%08X type=%u state=%d tmo=%s lr=%08X\n",
                    GetTickCount(), GetCurrentThreadId(), obj, base[obj], (int)PPC_LOAD_U32(obj + 4),
                    tmo ? "yes" : "inf", (uint32_t)ctx.lr);
    }
    if (logged.fetch_add(1) < 12)
        fprintf(stderr, "[wait] KeWaitForSingleObject obj=%08X type=%u state=%d timeout=%s lr=%08X\n",
                obj, base[obj], (int)PPC_LOAD_U32(obj + 4), tmo ? "yes" : "inf", (uint32_t)ctx.lr);
    bool infinite = (tmo == 0);
    int64_t t100ns = infinite ? 0 : (int64_t)PPC_LOAD_U64(tmo);   // negative = relative
    int64_t remainMs = infinite ? 0 : (t100ns < 0 ? (-t100ns) / 10000 : 0);
    for (;;) {
        if (kw_try_acquire(base, obj)) {
            if (watchObj) t_afterWake = true;
            if (wl < 40)
                fprintf(stderr, "[%lu] [wait-exit] obj=%08X tid=%lu\n",
                        GetTickCount(), obj, GetCurrentThreadId());
            ctx.r3.u64 = 0; return;
        }
        if (!infinite) {
            if (remainMs <= 0) { ctx.r3.u64 = 0x102; return; }          // STATUS_TIMEOUT
            remainMs -= 1;
        }
        Sleep(1);
    }
}

static bool kwm_signaled(uint8_t* base, uint32_t obj) {
    uint32_t a = obj + 4;
    return (int32_t)PPC_LOAD_U32(a) != 0;
}
static void kwm_consume(uint8_t* base, uint32_t obj) {
    uint32_t ta = obj, sa = obj + 4;
    uint8_t type = PPC_LOAD_U8(ta);
    int32_t st = (int32_t)PPC_LOAD_U32(sa);
    if (type == 1)      PPC_STORE_U32(sa, 0u);                    // synchronization event: auto-reset
    else if (type == 5) PPC_STORE_U32(sa, (uint32_t)(st - 1));    // semaphore: take one
}
// KeWaitForMultipleObjects(count, objects**, wait_type(0=all,1=any), reason, mode, alertable, timeout*, waitblocks*)
static void KeWaitForMultipleObjects_(PPCContext& ctx, uint8_t* base) {
    uint32_t count = ctx.r3.u32, arr = ctx.r4.u32, tp = ctx.r9.u32;
	WaitScope ws("KeWaitMulti", count ? rd32(base, arr) : 0, (uint32_t)ctx.lr);
    bool waitAll = (ctx.r5.u32 == 0), inf = true;
    ULONGLONG deadline = 0;
    if (tp) {
        int64_t v = (int64_t)_byteswap_uint64(*(uint64_t*)(base + tp));
        inf = false;
        deadline = GetTickCount64() + (v < 0 ? (ULONGLONG)(-v) / 10000 : 0);
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> g(g_kwLock);
            if (waitAll) {
                bool all = true;
                for (uint32_t i = 0; i < count; i++) if (!kwm_signaled(base, rd32(base, arr + i * 4))) { all = false; break; }
                if (all) {
                    for (uint32_t i = 0; i < count; i++) kwm_consume(base, rd32(base, arr + i * 4));
                    ctx.r3.u64 = 0;
                    return;
                }
            } else {
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t obj = rd32(base, arr + i * 4);
                    if (kwm_signaled(base, obj)) { kwm_consume(base, obj); ctx.r3.u64 = i; return; }
                }
            }
        }
        if (!inf && GetTickCount64() >= deadline) { ctx.r3.u64 = 0x102; return; }   // STATUS_TIMEOUT
        Sleep(1);
    }
}

static const bool kLogSet = false;
// KeSetEvent(event*, increment, wait) -> previous signal state
static void KeSetEvent_(PPCContext& ctx, uint8_t* base) {
    uint32_t obj = ctx.r3.u32;
    std::lock_guard<std::mutex> g(g_kwLock);
    uint32_t prev = PPC_LOAD_U32(obj + 4);
    PPC_STORE_U32(obj + 4, 1);
    if (obj == 0x40004D54 || obj == 0x40004DC0)
        if (kLogSet) fprintf(stderr, "[%lu] [set] obj=%08X prev=%u tid=%lu lr=%08X\n",
                GetTickCount(), obj, prev, GetCurrentThreadId(), (uint32_t)ctx.lr);
    ctx.r3.u32 = prev;
}

// ---- critical sections: one host recursive mutex per guest critical section ----
static std::mutex g_csMapLock;
static std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_cs;
static std::recursive_mutex& cs_mutex(uint32_t cs) {
    std::lock_guard<std::mutex> g(g_csMapLock);
    auto& p = g_cs[cs];
    if (!p) p = std::make_unique<std::recursive_mutex>();
    return *p;
}
static void RtlInitializeCriticalSection_(PPCContext& ctx, uint8_t*) { cs_mutex(ctx.r3.u32); ctx.r3.u64 = 0; }
static void RtlEnterCriticalSection_(PPCContext& ctx, uint8_t*) { cs_mutex(ctx.r3.u32).lock(); }
static void RtlLeaveCriticalSection_(PPCContext& ctx, uint8_t*) { cs_mutex(ctx.r3.u32).unlock(); }
static void RtlTryEnterCriticalSection_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = cs_mutex(ctx.r3.u32).try_lock() ? 1 : 0; }

// ---- TLS ----
static uint32_t g_tlsNext = 0;
static thread_local uint32_t t_tls[64];
static void KeTlsAlloc_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = (g_tlsNext < 64) ? g_tlsNext++ : 0xFFFFFFFFu; }
static void KeTlsFree_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
static void KeTlsSetValue_(PPCContext& ctx, uint8_t*) {
    uint32_t i = ctx.r3.u32;
    if (i >= 64) { ctx.r3.u64 = 0; return; }
    t_tls[i] = ctx.r4.u32; ctx.r3.u64 = 1;
}
static void KeTlsGetValue_(PPCContext& ctx, uint8_t*) { uint32_t i = ctx.r3.u32; ctx.r3.u64 = (i < 64) ? t_tls[i] : 0; }

// ---- trivial ----
static void KeGetCurrentProcessType_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
static void ReturnZero_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
static void NoOp_(PPCContext&, uint8_t*) {}

// ---- time ----
static void KeQuerySystemTime_(PPCContext& ctx, uint8_t* base) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    wr64(base, ctx.r3.u32, t);
}
static void KeQueryPerformanceFrequency_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 50000000; }

// ---- NT string helpers ----
static void RtlInitAnsiString_(PPCContext& ctx, uint8_t* base) {
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = 0, max = 0;
    if (src) { len = (uint16_t)strlen((const char*)(base + src)); max = len + 1; }
    *(uint16_t*)(base + dst + 0) = _byteswap_ushort(len);
    *(uint16_t*)(base + dst + 2) = _byteswap_ushort(max);
    wr32(base, dst + 4, src);
}
static void RtlInitUnicodeString_(PPCContext& ctx, uint8_t* base) {
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = 0, max = 0;
    if (src) { const uint16_t* p = (const uint16_t*)(base + src); while (p[len / 2]) len += 2; max = len + 2; }
    *(uint16_t*)(base + dst + 0) = _byteswap_ushort(len);
    *(uint16_t*)(base + dst + 2) = _byteswap_ushort(max);
    wr32(base, dst + 4, src);
}
static uint32_t nt_to_dos(uint32_t status) {
    switch (status) {
      case 0x00000000u: return 0;
      case 0x80000006u: return 18;    // NO_MORE_FILES
      case 0xC000000Fu: return 2;     // NO_SUCH_FILE -> FILE_NOT_FOUND
      case 0xC0000034u: return 2;     // OBJECT_NAME_NOT_FOUND
      case 0xC000003Au: return 3;     // OBJECT_PATH_NOT_FOUND
      case 0xC0000011u: return 38;    // END_OF_FILE -> HANDLE_EOF
      case 0xC0000022u: return 5;     // ACCESS_DENIED
      case 0xC0000035u: return 183;   // OBJECT_NAME_COLLISION -> ALREADY_EXISTS
      case 0xC000000Du: return 87;    // INVALID_PARAMETER
      case 0xC0000008u: return 6;     // INVALID_HANDLE
      default:          return 317;
    }
}
static void RtlNtStatusToDosError_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[file] RtlNtStatusToDosError status=%08X\n", ctx.r3.u32);
    static int s_inv = 0;
    if (ctx.r3.u32 == 0xC0000008u && s_inv++ < 6)
        fprintf(stderr, "[file] INVALID_HANDLE seen, caller lr=%08X r4=%08X r5=%08X\n",
                (uint32_t)ctx.lr, ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u32 = nt_to_dos(ctx.r3.u32);
    fprintf(stderr, "[file]   -> dos error %u\n", ctx.r3.u32);
}

// ---- file system: d:\ and game:\ map to the extracted content folder (read-only) ----
static std::string g_root;
void set_content_root(const char* p) {
    g_root = p;
    while (!g_root.empty() && (g_root.back() == '\\' || g_root.back() == '/')) g_root.pop_back();
}
static std::mutex g_fileLock;
static std::unordered_map<uint32_t, HANDLE> g_files;
static std::unordered_map<uint32_t, std::string> g_paths;      // handle -> host path
struct DirState { std::vector<WIN32_FIND_DATAA> entries; size_t next = 0; bool init = false; };
static std::unordered_map<uint32_t, DirState> g_dirs;
static std::unordered_map<uint32_t, uint32_t> g_objOfHandle, g_handleOfObj;   // handle <-> guest object pointer
static uint32_t g_nextHandle = 0x1000;

static std::string read_ansi(uint8_t* base, uint32_t strPtr) {
    if (!strPtr) return "";
    uint16_t len = _byteswap_ushort(*(uint16_t*)(base + strPtr));
    uint32_t buf = rd32(base, strPtr + 4);
    return std::string((const char*)(base + buf), len);
}
static bool resolve_path(const std::string& in, std::string& out) {
    std::string low = in;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    static const char* prefixes[] = { "d:\\", "game:\\", "\\device\\cdrom0\\" };
    for (const char* pre : prefixes) {
        size_t n = strlen(pre);
        if (low.compare(0, n, pre) == 0) {
            std::string rest = in.substr(n);
            if (rest.find("..") != std::string::npos) return false;
            out = g_root + "\\" + rest;
            return !g_root.empty();
        }
    }
    return false;
}
static void set_iosb(uint8_t* base, uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    wr32(base, iosb, status); wr32(base, iosb + 4, info);
}

// ---- mounted save content: "root:\..." paths are writable and live under host\saves ----
static std::unordered_map<std::string, std::string> g_contentRoots;   // lower-case root name -> host folder
const uint32_t kContentDataSize = 0x134;                              // sizeof(XCONTENT_DATA)
struct ContentEnum { std::vector<std::vector<uint8_t>> items; size_t next = 0; };
static std::unordered_map<uint32_t, ContentEnum> g_contentEnums;      // enumerator handle -> saves found at creation
static std::string lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
static bool resolve_content_path(const std::string& in, std::string& out) {
    size_t colon = in.find(':');
    if (colon == std::string::npos) return false;
    std::lock_guard<std::mutex> g(g_fileLock);
    auto it = g_contentRoots.find(lower_str(in.substr(0, colon)));
    if (it == g_contentRoots.end()) return false;
    std::string rest = in.substr(colon + 1);
    if (rest.find("..") != std::string::npos) return false;
    while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
    out = rest.empty() ? it->second : it->second + "\\" + rest;
    return true;
}
// NtCreateFile for a path inside mounted content: all dispositions, plus directories
static void create_content_file(PPCContext& ctx, uint8_t* base, const std::string& name, const std::string& path) {
    uint32_t outHandle = ctx.r3.u32, iosb = ctx.r6.u32, disposition = ctx.r10.u32;
    uint32_t options = rd32(base, ctx.r1.u32 + 0x54);
    bool isDir = (options & 1) != 0;                    // FILE_DIRECTORY_FILE
    DWORD attr = GetFileAttributesA(path.c_str());
    bool existed = attr != INVALID_FILE_ATTRIBUTES;
    if (existed && (attr & FILE_ATTRIBUTE_DIRECTORY)) isDir = true;
    static const DWORD kMode[6] = { CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, CREATE_ALWAYS };
    DWORD mode = disposition < 6 ? kMode[disposition] : OPEN_EXISTING;
    if (isDir) {
        if (!existed && mode != OPEN_EXISTING && mode != TRUNCATE_EXISTING) CreateDirectoryA(path.c_str(), nullptr);
        mode = OPEN_EXISTING;
    }
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, mode,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        uint32_t st = (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) ? 0xC0000035u    // OBJECT_NAME_COLLISION
                    : (e == ERROR_PATH_NOT_FOUND) ? STATUS_OBJECT_PATH_NOT_FOUND_
                    : (e == ERROR_FILE_NOT_FOUND) ? STATUS_OBJECT_NAME_NOT_FOUND_ : STATUS_ACCESS_DENIED_;
        fprintf(stderr, "[save] open \"%s\" disposition=%u -> error %lu status=%08X\n", name.c_str(), disposition, e, st);
        set_iosb(base, iosb, st, 0); ctx.r3.u64 = st; return;
    }
    uint32_t gh;
    { std::lock_guard<std::mutex> g(g_fileLock); gh = g_nextHandle; g_nextHandle += 4; g_files[gh] = h; g_paths[gh] = path; }
    wr32(base, outHandle, gh);
    uint32_t info = !existed ? 2 : (disposition == 0 || disposition >= 4) ? 3 : 1;   // CREATED / OVERWRITTEN / OPENED
    set_iosb(base, iosb, STATUS_SUCCESS_, info);
    fprintf(stderr, "[save] open \"%s\" disposition=%u dir=%d -> handle %08X info=%u\n", name.c_str(), disposition, (int)isDir, gh, info);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

static void NtCreateFile_(PPCContext& ctx, uint8_t* base) {
	fprintf(stderr, "[file] NtCreateFile -> status=%08X\n", ctx.r3.u32);
    uint32_t outHandle = ctx.r3.u32, oa = ctx.r5.u32, iosb = ctx.r6.u32, disposition = ctx.r10.u32;
    std::string name = read_ansi(base, rd32(base, oa + 4));
    std::string path;
    if (resolve_content_path(name, path)) { create_content_file(ctx, base, name, path); return; }
    if (!resolve_path(name, path)) {
        fprintf(stderr, "[file] NtCreateFile \"%s\" -> not mapped\n", name.c_str());
        set_iosb(base, iosb, STATUS_OBJECT_NAME_NOT_FOUND_, 0); ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND_; return;
    }
    if (disposition != 1) {   // FILE_OPEN only; writes/creation come later
        fprintf(stderr, "[file] NtCreateFile \"%s\" disposition=%u -> denied (read-only)\n", name.c_str(), disposition);
        set_iosb(base, iosb, STATUS_ACCESS_DENIED_, 0); ctx.r3.u64 = STATUS_ACCESS_DENIED_; return;
    }
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        uint32_t st = (GetLastError() == ERROR_PATH_NOT_FOUND) ? STATUS_OBJECT_PATH_NOT_FOUND_ : STATUS_OBJECT_NAME_NOT_FOUND_;
        fprintf(stderr, "[file] NtCreateFile \"%s\" -> %s\n", name.c_str(), st == STATUS_OBJECT_PATH_NOT_FOUND_ ? "path not found" : "not found");
        set_iosb(base, iosb, st, 0); ctx.r3.u64 = st; return;
    }
    uint32_t gh;
    { std::lock_guard<std::mutex> g(g_fileLock); gh = g_nextHandle; g_nextHandle += 4; g_files[gh] = h; g_paths[gh] = path; }
    wr32(base, outHandle, gh);
    set_iosb(base, iosb, STATUS_SUCCESS_, 1);   // FILE_OPENED
    fprintf(stderr, "[file] NtCreateFile \"%s\" -> handle %08X\n", name.c_str(), gh);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

static void NtOpenFile_(PPCContext& ctx, uint8_t* base) {
    ctx.r10.u64 = 1;                 // FILE_OPEN
    NtCreateFile_(ctx, base);
}

static HANDLE find_file(uint32_t gh) {
    std::lock_guard<std::mutex> g(g_fileLock);
    auto it = g_files.find(gh); return it == g_files.end() ? nullptr : it->second;
}

// NetDll_WSAStartup(caller, wVersionRequested, WSADATA* out)
static void NetDll_WSAStartup_(PPCContext& ctx, uint8_t* base) {
    uint32_t p = ctx.r5.u32;
    if (p) {
        memset(base + p, 0, 400);                       // sizeof(WSADATA) on this platform
        base[p + 0] = 0x02; base[p + 1] = 0x02;         // wVersion      = 0x0202 (big-endian)
        base[p + 2] = 0x02; base[p + 3] = 0x02;         // wHighVersion  = 0x0202
    }
    ctx.r3.u64 = 0;                                     // success
}

// NtReadFile(handle, event, apc, apc_ctx, iosb, buffer, length, byte_offset*)
static void NtReadFile_(PPCContext& ctx, uint8_t* base) {
    uint32_t gh = ctx.r3.u32;
    HANDLE h = find_file(gh);
    uint32_t iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, offP = ctx.r10.u32;
    std::string pn;
    { std::lock_guard<std::mutex> g(g_fileLock); auto it = g_paths.find(gh); if (it != g_paths.end()) pn = it->second; }
    if (!h) { fprintf(stderr, "[file] read: invalid handle %08X\n", gh); ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    int64_t off = -1;
    if (offP) {
        uint64_t o = _byteswap_uint64(*(uint64_t*)(base + offP));
        if (o != 0xFFFFFFFFFFFFFFFFull) { off = (int64_t)o; LARGE_INTEGER li; li.QuadPart = (LONGLONG)o; SetFilePointerEx(h, li, nullptr, FILE_BEGIN); }
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, base + buf, len, &got, nullptr);
    uint32_t status = !ok ? STATUS_ACCESS_DENIED_ : (got == 0 && len > 0 ? STATUS_END_OF_FILE_ : STATUS_SUCCESS_);
    fprintf(stderr, "[file] read %s off=%lld len=%u -> got=%lu status=%08X\n", pn.c_str(), (long long)off, len, got, status);
    set_iosb(base, iosb, status, got);
    ctx.r3.u64 = status;
}

// NtWriteFile(handle, event, apc, apc_ctx, iosb, buffer, length, byte_offset*): only content files are writable
static void NtWriteFile_(PPCContext& ctx, uint8_t* base) {
    uint32_t gh = ctx.r3.u32, iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, offP = ctx.r10.u32;
    HANDLE h = find_file(gh);
    if (!h) { fprintf(stderr, "[save] write: invalid handle %08X\n", gh); ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    if (offP) {
        uint64_t o = _byteswap_uint64(*(uint64_t*)(base + offP));
        if (o != 0xFFFFFFFFFFFFFFFFull) { LARGE_INTEGER li; li.QuadPart = (LONGLONG)o; SetFilePointerEx(h, li, nullptr, FILE_BEGIN); }
    }
    DWORD put = 0;
    BOOL ok = WriteFile(h, base + buf, len, &put, nullptr);
    uint32_t status = ok ? STATUS_SUCCESS_ : STATUS_ACCESS_DENIED_;
    fprintf(stderr, "[save] write handle=%08X len=%u -> put=%lu status=%08X\n", gh, len, put, status);
    set_iosb(base, iosb, status, put);
    ctx.r3.u64 = status;
}
// NtFlushBuffersFile(handle, iosb)
static void NtFlushBuffersFile_(PPCContext& ctx, uint8_t* base) {
    HANDLE h = find_file(ctx.r3.u32);
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    FlushFileBuffers(h);
    set_iosb(base, ctx.r4.u32, STATUS_SUCCESS_, 0);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

// NtQueryInformationFile(handle, iosb, info*, length, class)
static void NtQueryInformationFile_(PPCContext& ctx, uint8_t* base) {
    fprintf(stderr, "[file] NtQueryInformationFile handle=%08X class=%u len=%u lr=%08X\n",
        ctx.r3.u32, ctx.r7.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    HANDLE h = find_file(ctx.r3.u32);
    fprintf(stderr, "[file] query class %u on handle %08X\n", ctx.r7.u32, ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    if (cls == 5) {          // FileStandardInformation
        LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
        BY_HANDLE_FILE_INFORMATION bi{}; GetFileInformationByHandle(h, &bi);
        wr64(base, info + 0x00, ((uint64_t)sz.QuadPart + 0xFFF) & ~0xFFFull);
        wr64(base, info + 0x08, (uint64_t)sz.QuadPart);
        wr32(base, info + 0x10, 1);
        base[info + 0x14] = 0;
        base[info + 0x15] = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        set_iosb(base, iosb, STATUS_SUCCESS_, 0x18);
        ctx.r3.u64 = STATUS_SUCCESS_;
    } else if (cls == 14) {  // FilePositionInformation
        LARGE_INTEGER z{}, pos{}; SetFilePointerEx(h, z, &pos, FILE_CURRENT);
        wr64(base, info, (uint64_t)pos.QuadPart);
        set_iosb(base, iosb, STATUS_SUCCESS_, 8);
        ctx.r3.u64 = STATUS_SUCCESS_;
    } else if (cls == 34) {  // FileNetworkOpenInformation
        LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
        BY_HANDLE_FILE_INFORMATION bi{}; GetFileInformationByHandle(h, &bi);
        auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        wr64(base, info + 0x00, ft(bi.ftCreationTime));
        wr64(base, info + 0x08, ft(bi.ftLastAccessTime));
        wr64(base, info + 0x10, ft(bi.ftLastWriteTime));
        wr64(base, info + 0x18, ft(bi.ftLastWriteTime));
        wr64(base, info + 0x20, ((uint64_t)sz.QuadPart + 0xFFF) & ~0xFFFull);
        wr64(base, info + 0x28, (uint64_t)sz.QuadPart);
        wr32(base, info + 0x30, bi.dwFileAttributes ? bi.dwFileAttributes : 0x80);
        set_iosb(base, iosb, STATUS_SUCCESS_, 0x38);
        ctx.r3.u64 = STATUS_SUCCESS_;
    } else {
        fprintf(stderr, "[file] NtQueryInformationFile: unsupported class %u\n", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS_;
    }
}

// NtSetInformationFile(handle, iosb, info*, length, class)
static void NtSetInformationFile_(PPCContext& ctx, uint8_t* base) {
    HANDLE h = find_file(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    if (cls == 14) {         // FilePositionInformation
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)_byteswap_uint64(*(uint64_t*)(base + info));
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        set_iosb(base, iosb, STATUS_SUCCESS_, 0);
        ctx.r3.u64 = STATUS_SUCCESS_;
    } else if (cls == 19 || cls == 20) {   // FileAllocationInformation / FileEndOfFileInformation
        LARGE_INTEGER li, cur{}, zero{};
        li.QuadPart = (LONGLONG)_byteswap_uint64(*(uint64_t*)(base + info));
        SetFilePointerEx(h, zero, &cur, FILE_CURRENT);
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        BOOL ok = SetEndOfFile(h);
        SetFilePointerEx(h, cur, nullptr, FILE_BEGIN);
        uint32_t st = ok ? STATUS_SUCCESS_ : STATUS_ACCESS_DENIED_;
        set_iosb(base, iosb, st, 0);
        ctx.r3.u64 = st;
    } else {
        fprintf(stderr, "[file] NtSetInformationFile: unsupported class %u\n", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS_;
    }
}

// NtQueryDirectoryFile(handle, event, apc, apc_ctx, iosb, buffer, length, file_mask*, restart_scan [stack])
static void NtQueryDirectoryFile_(PPCContext& ctx, uint8_t* base) {
	{
    uint32_t m = ctx.r10.u32; char mb[96] = "(null)";
    if (m) { uint16_t l = PPC_LOAD_U16(m); uint32_t p = PPC_LOAD_U32(m + 4);
             if (l > 90) l = 90; for (int i = 0; i < l; i++) mb[i] = PPC_LOAD_U8(p + i); mb[l] = 0; }
    fprintf(stderr, "[file] NtQueryDirectoryFile handle=%08X mask=\"%s\" lr=%08X\n", ctx.r3.u32, mb, (uint32_t)ctx.lr);
    }
	fprintf(stderr, "[file] NtQueryDirectoryFile handle=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    uint32_t gh = ctx.r3.u32, iosb = ctx.r7.u32, info = ctx.r8.u32, len = ctx.r9.u32, maskP = ctx.r10.u32;
    bool restart = rd32(base, ctx.r1.u32 + 0x54) != 0;
    std::lock_guard<std::mutex> g(g_fileLock);
    auto pit = g_paths.find(gh);
    if (pit == g_paths.end()) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    DirState& ds = g_dirs[gh];
    if (!ds.init || restart) {
        ds.entries.clear(); ds.next = 0; ds.init = true;
        std::string mask = maskP ? read_ansi(base, maskP) : "";
        if (mask.empty()) mask = "*";
        WIN32_FIND_DATAA fd;
        HANDLE fh = FindFirstFileA((pit->second + "\\" + mask).c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) ds.entries.push_back(fd); }
            while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
    }
    if (ds.next >= ds.entries.size()) {
        // First query that matches nothing -> NO_SUCH_FILE; later calls -> NO_MORE_FILES
        uint32_t st = (ds.next == 0) ? 0xC000000Fu : 0x80000006u;
        set_iosb(base, iosb, st, 0);
        ctx.r3.u64 = st; return;
    }
    const WIN32_FIND_DATAA& fd = ds.entries[ds.next];
    uint32_t nameLen = (uint32_t)strlen(fd.cFileName);
    if (len < 0x40 + nameLen) { set_iosb(base, iosb, 0x80000005, 0); ctx.r3.u64 = 0x80000005; return; }  // BUFFER_OVERFLOW
    auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    wr32(base, info + 0x00, 0);                        // next entry offset (one entry per call)
    wr32(base, info + 0x04, 0);
    wr64(base, info + 0x08, ft(fd.ftCreationTime));
    wr64(base, info + 0x10, ft(fd.ftLastAccessTime));
    wr64(base, info + 0x18, ft(fd.ftLastWriteTime));
    wr64(base, info + 0x20, ft(fd.ftLastWriteTime));
    wr64(base, info + 0x28, size);                     // end of file
    wr64(base, info + 0x30, (size + 0xFFF) & ~0xFFFull);
    wr32(base, info + 0x38, fd.dwFileAttributes);
    wr32(base, info + 0x3C, nameLen);
    memcpy(base + info + 0x40, fd.cFileName, nameLen);
    ds.next++;
    set_iosb(base, iosb, STATUS_SUCCESS_, 0x40 + nameLen);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

// NtCreateEvent(handle*, object_attributes*, event_type, initial_state)
static void NtCreateEvent_(PPCContext& ctx, uint8_t* base) {
    HANDLE h = CreateEventA(nullptr, ctx.r5.u32 == 0 /*manual reset*/, ctx.r6.u32 != 0, nullptr);
    uint32_t gh;
    { std::lock_guard<std::mutex> g(g_fileLock); gh = g_nextHandle; g_nextHandle += 4; g_files[gh] = h; }
    wr32(base, ctx.r3.u32, gh);
	fprintf(stderr, "[evt] create handle=%08X type=%u initial=%u lr=%08X\n", gh, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

static void NtClose_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_fileLock);
    auto it = g_files.find(ctx.r3.u32);
    if (it != g_files.end()) { CloseHandle(it->second); g_files.erase(it); }
    g_paths.erase(ctx.r3.u32); g_dirs.erase(ctx.r3.u32); g_contentEnums.erase(ctx.r3.u32);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

static std::atomic<uint32_t> g_listenerNext{0x2000};
static void XamNotifyCreateListener_(PPCContext& ctx, uint8_t*) {
    uint32_t gh = g_listenerNext.fetch_add(4);
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, nullptr);      // manual-reset, not signalled
    { std::lock_guard<std::mutex> g(g_fileLock); g_files[gh] = h; }
    ctx.r3.u64 = gh;
}

// ---- interlocked singly-linked lists: header {u32 next; u16 depth; u16 sequence}, entry {u32 next} ----
static std::mutex g_slistLock;
static void InterlockedPopEntrySList_(PPCContext& ctx, uint8_t* base) {
    std::lock_guard<std::mutex> g(g_slistLock);
    uint32_t hdr = ctx.r3.u32, first = rd32(base, hdr);
    if (first) {
        wr32(base, hdr, rd32(base, first));
        uint16_t depth = _byteswap_ushort(*(uint16_t*)(base + hdr + 4)), seq = _byteswap_ushort(*(uint16_t*)(base + hdr + 6));
        *(uint16_t*)(base + hdr + 4) = _byteswap_ushort(depth ? depth - 1 : 0);
        *(uint16_t*)(base + hdr + 6) = _byteswap_ushort(seq + 1);
    }
    ctx.r3.u64 = first;
}
static void InterlockedPushEntrySList_(PPCContext& ctx, uint8_t* base) {
    std::lock_guard<std::mutex> g(g_slistLock);
    uint32_t hdr = ctx.r3.u32, entry = ctx.r4.u32, first = rd32(base, hdr);
    wr32(base, entry, first);
    wr32(base, hdr, entry);
    uint16_t depth = _byteswap_ushort(*(uint16_t*)(base + hdr + 4)), seq = _byteswap_ushort(*(uint16_t*)(base + hdr + 6));
    *(uint16_t*)(base + hdr + 4) = _byteswap_ushort(depth + 1);
    *(uint16_t*)(base + hdr + 6) = _byteswap_ushort(seq + 1);
    ctx.r3.u64 = first;
}
static void InterlockedFlushSList_(PPCContext& ctx, uint8_t* base) {
    std::lock_guard<std::mutex> g(g_slistLock);
    uint32_t hdr = ctx.r3.u32, first = rd32(base, hdr);
    wr32(base, hdr, 0);
    *(uint16_t*)(base + hdr + 4) = 0;
    ctx.r3.u64 = first;
}

// ---- events and waits (handles share the file-handle table) ----
static void NtClearEvent_(PPCContext& ctx, uint8_t*) {
    HANDLE h = find_file(ctx.r3.u32);
    if (h) ResetEvent(h);
    ctx.r3.u64 = h ? STATUS_SUCCESS_ : STATUS_INVALID_HANDLE_;
}
static void NtSetEvent_(PPCContext& ctx, uint8_t* base) {
    HANDLE h = find_file(ctx.r3.u32);
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    SetEvent(h);
    { uint32_t bt1 = 0, bt2 = 0, sp = ctx.r1.u32;
      if (sp > 0x10000 && sp < 0xE0000000u) {
          uint32_t s1 = PPC_LOAD_U32(sp);
          if (s1 > 0x10000 && s1 < 0xE0000000u) {
              bt1 = PPC_LOAD_U32(s1 - 8);
              uint32_t s2 = PPC_LOAD_U32(s1);
              if (s2 > 0x10000 && s2 < 0xE0000000u) bt2 = PPC_LOAD_U32(s2 - 8);
          }
      }
      fprintf(stderr, "[evt] set tid=%lu handle=%08X lr=%08X bt=%08X %08X\n",
              GetCurrentThreadId(), ctx.r3.u32, (uint32_t)ctx.lr, bt1, bt2); }
    if (ctx.r4.u32) wr32(base, ctx.r4.u32, 0);
    ctx.r3.u64 = STATUS_SUCCESS_;
}
static void NtResetEvent_(PPCContext& ctx, uint8_t* base) {
    HANDLE h = find_file(ctx.r3.u32);
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    ResetEvent(h);
    if (ctx.r4.u32) wr32(base, ctx.r4.u32, 0);
    ctx.r3.u64 = STATUS_SUCCESS_;
}
static void NtWaitForSingleObjectEx_(PPCContext& ctx, uint8_t* base) {
    HANDLE h = find_file(ctx.r3.u32);
    uint32_t tp = ctx.r6.u32;
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
	WaitScope ws("NtWait", ctx.r3.u32, (uint32_t)ctx.lr);
    DWORD ms = INFINITE;
    if (tp) {
        int64_t v = (int64_t)_byteswap_uint64(*(uint64_t*)(base + tp));
        ms = (v < 0) ? (DWORD)std::min<uint64_t>((uint64_t)(-v) / 10000, 0xFFFFFFFEull) : 0;
    }
	if (ms == INFINITE) fprintf(stderr, "[evt] wait-inf handle=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    DWORD r = WaitForSingleObject(h, ms);
    ctx.r3.u64 = (r == WAIT_OBJECT_0) ? STATUS_SUCCESS_ : (r == WAIT_TIMEOUT ? 0x102 : 0xC0000001);
}

static void NtWaitForMultipleObjectsEx_(PPCContext& ctx, uint8_t* base) {
    uint32_t count = ctx.r3.u32, arr = ctx.r4.u32, tp = ctx.r8.u32;
    bool waitAll = (ctx.r5.u32 == 0);
    if (count == 0 || count > 64) { ctx.r3.u64 = 0xC000000D; return; }       // STATUS_INVALID_PARAMETER
    HANDLE hs[64];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t gh = rd32(base, arr + i * 4);
        hs[i] = find_file(gh);
        if (!hs[i]) {
            static int s_bad = 0;
            if (s_bad++ < 8) fprintf(stderr, "[wait] WaitMultiple: unknown handle %08X (index %u of %u) lr=%08X\n", gh, i, count, (uint32_t)ctx.lr);
            ctx.r3.u64 = STATUS_INVALID_HANDLE_;
            return;
        }
    }
    DWORD ms = INFINITE;
    if (tp) {
        int64_t v = (int64_t)_byteswap_uint64(*(uint64_t*)(base + tp));
        ms = (v < 0) ? (DWORD)std::min<uint64_t>((uint64_t)(-v) / 10000, 0xFFFFFFFEull) : 0;
    }
	WaitScope ws("NtWaitMulti", rd32(base, arr), (uint32_t)ctx.lr);
    DWORD r = WaitForMultipleObjects(count, hs, waitAll ? TRUE : FALSE, ms);
    if (r < WAIT_OBJECT_0 + count)  ctx.r3.u64 = r - WAIT_OBJECT_0;           // index of the signalled object (0 for wait-all)
    else if (r == WAIT_TIMEOUT)     ctx.r3.u64 = 0x102;
    else                            ctx.r3.u64 = 0xC0000001;
}

// ---- console settings: ExGetXConfigSetting(category, setting, buffer*, buffer_size, required_size*) ----
static void ExGetXConfigSetting_(PPCContext& ctx, uint8_t* base) {
    uint32_t cat = ctx.r3.u32, setting = ctx.r4.u32, buf = ctx.r5.u32, bufSize = ctx.r6.u32, reqP = ctx.r7.u32;
    uint32_t value = 0, size = 4;
    if (cat == 3 && setting == 9) value = 1;                 // user language: English
    else fprintf(stderr, "[xconfig] category %u setting %u -> returning 0\n", cat, setting);
    if (reqP) wr32(base, reqP, size);
    if (bufSize < size) { ctx.r3.u64 = 0xC0000023; return; } // STATUS_BUFFER_TOO_SMALL
    if (buf) wr32(base, buf, value);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

// ---- guest threads ----
static std::atomic<uint32_t> g_nextTid{2};
static uint32_t vm_alloc(uint8_t* base, uint32_t size) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    std::lock_guard<std::mutex> g(g_vmLock);
    uint32_t addr = (g_vmNext + 0xFFFF) & ~0xFFFFu;
    if (uint64_t(addr) + size > kVmEnd) return 0;
    g_vmNext = addr + size;
    VirtualAlloc(base + addr, size, MEM_COMMIT, PAGE_READWRITE);
    return addr;
}
struct ThreadStart { uint8_t* base; uint32_t entry, arg0, arg1, pcr, stackTop; };
static DWORD WINAPI GuestThreadProc(LPVOID p) {
    std::unique_ptr<ThreadStart> ts((ThreadStart*)p);
    PPCContext ctx{};
    g_ctx = &ctx;
    ctx.r1.u64 = ts->stackTop - 0x100;
    ctx.r13.u64 = ts->pcr;
    ctx.r3.u64 = ts->arg0; ctx.r4.u64 = ts->arg1;
    PPCFunc* fn = *(PPCFunc**)(ts->base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (uint64_t(ts->entry) - PPC_CODE_BASE) * 2);
    fn(ctx, ts->base);
    return ctx.r3.u32;
}
// ExCreateThread(handle*, stack_size, thread_id*, xapi_startup, start_address, start_context, flags)
static void ExCreateThread_(PPCContext& ctx, uint8_t* base) {
    uint32_t hP = ctx.r3.u32, stackSize = ctx.r4.u32, idP = ctx.r5.u32, startup = ctx.r6.u32;
    uint32_t start = ctx.r7.u32, startCtx = ctx.r8.u32, flags = ctx.r9.u32;
    if (stackSize < 0x10000) stackSize = 0x10000;
    stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
    uint32_t pcr = vm_alloc(base, 0x10000), stack = vm_alloc(base, stackSize);
    if (!pcr || !stack) { ctx.r3.u64 = STATUS_NO_MEMORY_; return; }
    uint32_t tobj = pcr + 0x8000, tid = g_nextTid++;
    wr32(base, pcr + 0x100, tobj);
    wr32(base, tobj + 0x14C, tid);
    wr32(base, pcr + 0x70, stack + stackSize);
    wr32(base, pcr + 0x74, stack);
    auto* ts = new ThreadStart{ base, 0, 0, 0, pcr, stack + stackSize };
    if (startup) { ts->entry = startup; ts->arg0 = start; ts->arg1 = startCtx; }
    else         { ts->entry = start;   ts->arg0 = startCtx; ts->arg1 = 0; }
    HANDLE h = CreateThread(nullptr, 64ull << 20, GuestThreadProc, ts,
                            CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    uint32_t gh;
    { std::lock_guard<std::mutex> g(g_fileLock); gh = g_nextHandle; g_nextHandle += 4; g_files[gh] = h;
      g_objOfHandle[gh] = tobj; g_handleOfObj[tobj] = gh; }
    if (hP) wr32(base, hP, gh);
    if (idP) wr32(base, idP, tid);
    fprintf(stderr, "[thread] ExCreateThread id=%u entry=%08X arg=%08X stack=%08X+%X flags=%X%s\n",
            tid, ts->entry, ts->arg0, stack, stackSize, flags, (flags & 1) ? " (suspended)" : "");
    if (!(flags & 1)) ResumeThread(h);
    ctx.r3.u64 = STATUS_SUCCESS_;
}
// NtResumeThread(handle, suspend_count*)
static void NtResumeThread_(PPCContext& ctx, uint8_t* base) {
    fprintf(stderr, "[thread] resume handle=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    HANDLE h = find_file(ctx.r3.u32);
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    DWORD prev = ResumeThread(h);
    if (ctx.r4.u32) wr32(base, ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

// ObReferenceObjectByHandle(handle, object_type, out_object*)
static uint32_t g_objChunk = 0, g_objUsed = 0;
static void ObReferenceObjectByHandle_(PPCContext& ctx, uint8_t* base) {
    uint32_t gh = ctx.r3.u32, outP = ctx.r5.u32;
    std::lock_guard<std::mutex> g(g_fileLock);
    if (!g_files.count(gh)) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    uint32_t obj;
    auto it = g_objOfHandle.find(gh);
    if (it != g_objOfHandle.end()) obj = it->second;
    else {
        if (!g_objChunk || g_objUsed + 0x100 > 0x10000) { g_objChunk = vm_alloc(base, 0x10000); g_objUsed = 0; }
        obj = g_objChunk + g_objUsed; g_objUsed += 0x100;
        g_objOfHandle[gh] = obj; g_handleOfObj[obj] = gh;
    }
    if (outP) wr32(base, outP, obj);
    ctx.r3.u64 = STATUS_SUCCESS_;
}

// ---- thread configuration (object-pointer based) ----
static void KeSetAffinityThread_(PPCContext& ctx, uint8_t* base) {
    if (ctx.r5.u32) wr32(base, ctx.r5.u32, 0);     // previous affinity
    ctx.r3.u64 = 0;
}
static void KeSetBasePriorityThread_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }    // previous priority
static void KeResumeThread_(PPCContext& ctx, uint8_t*) {
	fprintf(stderr, "[thread] resume handle=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> g(g_fileLock);
        auto it = g_handleOfObj.find(ctx.r3.u32);
        if (it != g_handleOfObj.end()) { auto f = g_files.find(it->second); if (f != g_files.end()) h = f->second; }
    }
    ctx.r3.u64 = h ? ResumeThread(h) : 0;           // returns the previous suspend count
}

// KeDelayExecutionThread(processor_mode, alertable, interval*)
static void KeDelayExecutionThread_(PPCContext& ctx, uint8_t* base) {
	WaitScope ws("Delay", 0, (uint32_t)ctx.lr);
    uint32_t ip = ctx.r5.u32;
    if (ip) {
        int64_t v = (int64_t)_byteswap_uint64(*(uint64_t*)(base + ip));
        DWORD ms = (v < 0) ? (DWORD)std::min<uint64_t>((uint64_t)(-v) / 10000, 0xFFFFFFFEull) : 0;
        if (ms == 0) SwitchToThread(); else Sleep(ms);
    } else {
        Sleep(INFINITE);
    }
    ctx.r3.u64 = 0;
}

static void RtlFillMemoryUlong_(PPCContext& ctx, uint8_t* base) {
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32, pat = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= len; i += 4) wr32(base, dst + i, pat);
}

// VdQueryVideoFlags() -> bit0 widescreen, bit1 width>=1024, bit2 width>=1920
static void VdQueryVideoFlags_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 3; }

// VdIsHSIOTrainingSucceeded() -> bool
static void VdIsHSIOTrainingSucceeded_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }

// VdGetCurrentDisplayGamma(int* type, float* gamma)
static void VdGetCurrentDisplayGamma_(PPCContext& ctx, uint8_t* base) {
    if (ctx.r3.u32) wr32(base, ctx.r3.u32, 2);                 // gamma type
    if (ctx.r4.u32) wr32(base, ctx.r4.u32, 0x400CCCCD);        // 2.2f
}

// VdGetSystemCommandBuffer(void* outBuf /*0x94 bytes*/, uint32_t* outToken)
static void VdGetSystemCommandBuffer_(PPCContext& ctx, uint8_t* base) {
    uint32_t p0 = ctx.r3.u32, p1 = ctx.r4.u32;
    if (p0) { memset(base + p0, 0, 0x94); wr32(base, p0, 0xBEEF0000); }
    if (p1) wr32(base, p1, 0xBEEF0001);
    ctx.r3.u64 = 0;
}

// VdSwap(cmdbuf*, fetch*, ..., frontbuffer in r8): fill the reserved 64-dword slot with an XE_SWAP packet
static void VdSwap_(PPCContext& ctx, uint8_t* base) {
    uint32_t buf = ctx.r3.u32, front = ctx.r8.u32;
    if (front) { uint32_t fa = front; front = PPC_LOAD_U32(fa); }      // r8 points at a word holding the front buffer's address
    if (buf) {
        wr32(base, buf + 0, 0xC0000000u | (62u << 16) | (0x64u << 8));   // type-3, 63-dword body, opcode XE_SWAP
        wr32(base, buf + 4, 0x53574150u);                                 // 'SWAP'
        wr32(base, buf + 8, front);                                       // frontbuffer pointer
        for (uint32_t i = 3; i < 64; i++) wr32(base, buf + i * 4, 0);
    }
    ctx.r3.u64 = 0;
}

// VdGetCurrentDisplayInformation(X_DISPLAY_INFO* out) - 0x58-byte struct; layout partly inferred
static void VdGetCurrentDisplayInformation_(PPCContext& ctx, uint8_t* base) {
    uint32_t p = ctx.r3.u32;
    if (p) {
        memset(base + p, 0, 0x58);
        *(uint16_t*)(base + p + 0x00) = _byteswap_ushort(1280);   // front buffer width
        *(uint16_t*)(base + p + 0x02) = _byteswap_ushort(720);    // front buffer height
        wr32(base, p + 0x18, 0x42700000);                          // 60.0f refresh
        *(uint16_t*)(base + p + 0x50) = _byteswap_ushort(1280);   // display width
        *(uint16_t*)(base + p + 0x52) = _byteswap_ushort(720);    // display height
    }
    ctx.r3.u64 = 0;
}

// ---- real controller: the first connected XInput pad acts as the game's user 0 ----
static std::atomic<int> g_hostPad{-1};
static bool host_pad_read(XINPUT_STATE& s) {
    int pad = g_hostPad.load();
    if (pad >= 0) {
        if (XInputGetState((DWORD)pad, &s) == ERROR_SUCCESS) return true;
        fprintf(stderr, "[input] controller %d disconnected\n", pad);
        g_hostPad = -1;
    }
    static std::atomic<ULONGLONG> nextScan{0};                // scanning empty slots is slow, so only every 2s
    ULONGLONG now = GetTickCount64();
    if (now < nextScan.load()) return false;
    nextScan = now + 2000;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        if (XInputGetState(i, &s) == ERROR_SUCCESS) {
            g_hostPad = (int)i;
            fprintf(stderr, "[input] controller %lu connected, used as user 0\n", i);
            return true;
        }
    }
    return false;
}
// XamInputSetState(user, flags, {u16 leftMotor; u16 rightMotor}*): rumble
static void XamInputSetState_(PPCContext& ctx, uint8_t* base) {
    uint32_t user = ctx.r3.u32, vib = ctx.r5.u32;
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = 0x48F; return; }
    int pad = g_hostPad.load();
    if (pad >= 0 && vib) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = _byteswap_ushort(*(uint16_t*)(base + vib));
        v.wRightMotorSpeed = _byteswap_ushort(*(uint16_t*)(base + vib + 2));
        XInputSetState((DWORD)pad, &v);
    }
    ctx.r3.u64 = 0;
}

// XamInputGetState(user, flags, XINPUT_STATE* out): 16 bytes {u32 packet; u16 buttons; u8 lt, rt; s16 lx, ly, rx, ry}
static void XamInputGetState_(PPCContext& ctx, uint8_t* base) {
    uint32_t user = ctx.r3.u32, st = ctx.r5.u32;
    static std::atomic<uint32_t> calls{0};
    uint32_t nc = ++calls;
    if (nc <= 8 || (nc % 3000) == 0)
        fprintf(stderr, "[input] GetState call #%u user=%u lr=%08X t=%lu\n", nc, user, (uint32_t)ctx.lr, GetTickCount());
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = 0x48F; return; }   // ERROR_DEVICE_NOT_CONNECTED
    if (st) memset(base + st, 0, 16);
    // experiment: BCR2_AUTOINPUT plays a pattern on user 0, one character per 500ms, from 15s after the first poll.
    // "<once>|<loop>": S Start, B Back, A/X/Y buttons, b = B button, U/D/L/R d-pad, u/d/l/r left stick, T right trigger, . nothing.
    // Any value without a '|' (such as "1") uses the original Start, A loop.
    static const char* kAutoEnv = getenv("BCR2_AUTOINPUT");
    if (kAutoEnv && st) {
        static const std::string kPat = kAutoEnv;
        static const size_t kBar = kPat.find('|');
        static const std::string kOnce = kBar == std::string::npos ? "" : kPat.substr(0, kBar);
        static const std::string kLoop = kBar == std::string::npos ? "S.A." : kPat.substr(kBar + 1);
        static const ULONGLONG t0 = GetTickCount64();
        ULONGLONG el = GetTickCount64() - t0;
        uint16_t buttons = 0;
        int16_t lx = 0, ly = 0;
        uint8_t rt = 0;
        uint32_t cycle = 0;
        if (el >= 15000) {
            cycle = (uint32_t)((el - 15000) / 500);
            char c = '.';
            if (cycle < kOnce.size()) c = kOnce[cycle];
            else if (!kLoop.empty()) c = kLoop[(cycle - kOnce.size()) % kLoop.size()];
            switch (c) {
              case 'S': buttons = 0x0010; break;   // Start
              case 'B': buttons = 0x0020; break;   // Back
              case 'A': buttons = 0x1000; break;
              case 'b': buttons = 0x2000; break;   // B button
              case 'X': buttons = 0x4000; break;
              case 'Y': buttons = 0x8000; break;
              case 'U': buttons = 0x0001; break;
              case 'D': buttons = 0x0002; break;
              case 'L': buttons = 0x0004; break;
              case 'R': buttons = 0x0008; break;
              case 'u': ly = 32000; break;
              case 'd': ly = -32000; break;
              case 'l': lx = -32000; break;
              case 'r': lx = 32000; break;
              case 'T': rt = 255; break;           // right trigger
            }
        }
        static std::atomic<uint32_t> lastCycle{0xFFFFFFFFu};
        uint32_t prev = lastCycle.exchange(cycle);
        if (prev != cycle && cycle < 24) fprintf(stderr, "[input] press step=%u buttons=%04X lx=%d ly=%d t=%lu\n", cycle, buttons, lx, ly, GetTickCount());
        wr32(base, st, cycle + 1);                                  // packet number changes with the state
        *(uint16_t*)(base + st + 4) = _byteswap_ushort(buttons);
        base[st + 7] = rt;
        *(uint16_t*)(base + st + 8) = _byteswap_ushort((uint16_t)lx);
        *(uint16_t*)(base + st + 10) = _byteswap_ushort((uint16_t)ly);
    }
    if (!kAutoEnv && st) {                                    // a simulated pattern takes priority over a real pad
        XINPUT_STATE xs;
        if (host_pad_read(xs)) {
            const XINPUT_GAMEPAD& g = xs.Gamepad;             // same button bits as the console
            wr32(base, st, xs.dwPacketNumber);
            *(uint16_t*)(base + st + 4) = _byteswap_ushort(g.wButtons);
            base[st + 6] = g.bLeftTrigger;
            base[st + 7] = g.bRightTrigger;
            *(uint16_t*)(base + st + 8)  = _byteswap_ushort((uint16_t)g.sThumbLX);
            *(uint16_t*)(base + st + 10) = _byteswap_ushort((uint16_t)g.sThumbLY);
            *(uint16_t*)(base + st + 12) = _byteswap_ushort((uint16_t)g.sThumbRX);
            *(uint16_t*)(base + st + 14) = _byteswap_ushort((uint16_t)g.sThumbRY);
        }
    }
    ctx.r3.u64 = 0;
}
// XamInputGetCapabilities(user, flags, XINPUT_CAPABILITIES* out): 20 bytes
static void XamInputGetCapabilities_(PPCContext& ctx, uint8_t* base) {
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = 0x48F; return; }
    if (caps) { memset(base + caps, 0, 20); base[caps] = 1; base[caps + 1] = 1; }  // type = gamepad, subtype = gamepad
    ctx.r3.u64 = 0;
}

// XAudioGetSpeakerConfig(uint32_t* out): report stereo
static void XAudioGetSpeakerConfig_(PPCContext& ctx, uint8_t* base) {
    if (ctx.r3.u32) wr32(base, ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}
// ---- null audio output: asks the client for a frame every 256 samples at 48 kHz (5.33 ms) and discards the result ----
static std::atomic<uint32_t> g_audioCb{0}, g_audioArg{0};
static std::atomic<uint64_t> g_audioFrames{0};
static void audio_thread(uint8_t* base) {
    const uint32_t PCR = 0x7E400000, TOBJ = 0x7E410000, STK = 0x7E500000, STKSZ = 0x40000;
    const uint32_t kArgWord = TOBJ + 0x1000;  // guest word holding the client's argument
    VirtualAlloc(base + PCR, 0x20000, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(base + STK, STKSZ,   MEM_COMMIT, PAGE_READWRITE);
    fprintf(stderr, "[thr] audio tid=%lu\n", GetCurrentThreadId());
    wr32(base, PCR + 0x100, TOBJ);            // PCR -> thread object
    wr32(base, TOBJ + 0x14C, 0x9B);           // thread id
    wr32(base, PCR + 0x70, STK + STKSZ);      // stack base
    wr32(base, PCR + 0x74, STK);              // stack limit
    PPCContext ctx{};
    const auto kFrame = std::chrono::microseconds(5333);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        uint32_t cb = g_audioCb.load();
        if (!cb) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); next = std::chrono::steady_clock::now(); continue; }
        auto now = std::chrono::steady_clock::now();
        if (now < next) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        next += kFrame;
        if (next < now - std::chrono::milliseconds(100)) next = now;   // don't try to catch up after a long stall
        ctx.r1.u32  = STK + STKSZ - 0x100;
        ctx.r13.u32 = PCR;
        wr32(base, kArgWord, g_audioArg.load());
        ctx.r3.u64  = kArgWord;                               // the callback dereferences r3 to get its argument
        ctx.lr      = 0;
        PPC_CALL_INDIRECT_FUNC(cb);
        uint64_t n = ++g_audioFrames;
        if (n <= 3 || (n % 18750) == 0) fprintf(stderr, "[audio] frame #%llu t=%lu\n", (unsigned long long)n, GetTickCount());
    }
}
// XAudioRegisterRenderDriverClient(callback_struct*, uint32_t* out_driver)
// the struct {u32 callback; u32 arg} lives on the caller's stack, so copy it now
static void XAudioRegisterRenderDriverClient_(PPCContext& ctx, uint8_t* base) {
    uint32_t cbs = ctx.r3.u32, cb = cbs ? rd32(base, cbs) : 0, arg = cbs ? rd32(base, cbs + 4) : 0;
    static const bool kNoAudio = getenv("BCR2_NOAUDIO") != nullptr;   // experiment switch: never call the client
    fprintf(stderr, "[audio] register callback=%08X arg=%08X lr=%08X%s\n", cb, arg, (uint32_t)ctx.lr, kNoAudio ? " (disabled)" : "");
    if (ctx.r4.u32) wr32(base, ctx.r4.u32, 0x41550001);
    if (cb && !kNoAudio) {
        g_audioArg = arg;
        g_audioCb = cb;
        static std::atomic<bool> started{false};
        if (!started.exchange(true)) std::thread(audio_thread, base).detach();
    }
    ctx.r3.u64 = 0;
}
static void XAudioUnregisterRenderDriverClient_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[audio] unregister driver=%08X frames=%llu lr=%08X\n", ctx.r3.u32, (unsigned long long)g_audioFrames.load(), (uint32_t)ctx.lr);
    g_audioCb = 0;
    ctx.r3.u64 = 0;
}

static void XAudioGetVoiceCategoryVolume_(PPCContext& ctx, uint8_t* base) {
    if (ctx.r4.u32) wr32(base, ctx.r4.u32, 0x3F800000);   // 1.0f
    ctx.r3.u64 = 0;
}

static void XamUserGetSigninState_(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64 = (ctx.r3.u32 == 0) ? 1 : 0;   // user 0: signed in locally
}

static void NetDll_socket_(PPCContext& ctx, uint8_t*)         { ctx.r3.s64 = -1; }        // INVALID_SOCKET
static void NetDll_WSAGetLastError_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 10047; }     // WSAEAFNOSUPPORT
static void NetErr_(PPCContext& ctx, uint8_t*) { ctx.r3.s64 = -1; }   // SOCKET_ERROR

static void XGetGameRegion_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0xFFFF; }
static void XamInputGetKeystrokeEx_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0x10D2; }   // ERROR_EMPTY

// XMsgInProcessCall(app, message, buffer*, size): unknown XAM service request -> report "not found" and log it
static void XMsgInProcessCall_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[xam] XMsgInProcessCall app=%08X msg=%08X buf=%08X size=%08X lr=%08X\n",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    ctx.r3.u64 = 0x490;   // ERROR_NOT_FOUND
}

// XOVERLAPPED: +0 InternalLow (status, 997 while pending), +4 InternalHigh (length), +0xC hEvent,
// +0x10 completion routine, +0x18 extended error. Finishes the operation right away.
static void complete_overlapped(uint8_t* base, uint32_t ov, uint32_t status, uint32_t length = 0) {
    wr32(base, ov + 0x04, length);
    wr32(base, ov + 0x18, status);
    wr32(base, ov + 0x00, status);
    uint32_t ev = rd32(base, ov + 0x0C), routine = rd32(base, ov + 0x10);
    if (ev) { HANDLE h = find_file(ev); if (h) SetEvent(h); }
    if (routine) fprintf(stderr, "[xam] overlapped %08X has completion routine %08X, not called\n", ov, routine);
}

// the single fake save device reported to the game
static const uint32_t kFakeDeviceId = 1;

// XamShowDeviceSelectorUI(user, contentType, contentFlags, u64 bytesRequested, u32* deviceId, overlapped*)
// no UI: picks the fake device as if the player had confirmed it
static void XamShowDeviceSelectorUI_(PPCContext& ctx, uint8_t* base) {
    uint32_t dev = ctx.r7.u32, ov = ctx.r8.u32;
    fprintf(stderr, "[xam] XamShowDeviceSelectorUI user=%u type=%u flags=%08X bytes=%llX dev=%08X ov=%08X lr=%08X\n",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (unsigned long long)ctx.r6.u64, dev, ov, (uint32_t)ctx.lr);
    if (dev) wr32(base, dev, kFakeDeviceId);
    if (ov) { complete_overlapped(base, ov, 0); ctx.r3.u64 = 997; }   // ERROR_IO_PENDING: caller polls the overlapped
    else ctx.r3.u64 = 0;
}

// XamContentGetDeviceState(deviceId, overlapped*): nonzero tells the game the device has gone away
static void XamContentGetDeviceState_(PPCContext& ctx, uint8_t* base) {
    uint32_t status = (ctx.r3.u32 == kFakeDeviceId) ? 0 : 0x48F;      // ERROR_DEVICE_NOT_CONNECTED
    uint32_t ov = ctx.r4.u32;
    if (ov) { complete_overlapped(base, ov, status); ctx.r3.u64 = 997; }
    else ctx.r3.u64 = status;
}

// XamContentGetDeviceData(deviceId, X_CONTENT_DEVICE_DATA*):
// {u32 id; u32 type; u64 totalBytes; u64 freeBytes; wchar name[28]} = 0x50 bytes, big-endian
static void XamContentGetDeviceData_(PPCContext& ctx, uint8_t* base) {
    uint32_t id = ctx.r3.u32, out = ctx.r4.u32;
    fprintf(stderr, "[xam] XamContentGetDeviceData id=%u out=%08X lr=%08X\n", id, out, (uint32_t)ctx.lr);
    if (id != kFakeDeviceId) { ctx.r3.u64 = 0x48F; return; }
    if (out) {
        memset(base + out, 0, 0x50);
        wr32(base, out + 0x00, id);
        wr32(base, out + 0x04, 1);                                    // hard drive
        wr64(base, out + 0x08, 0x40000000ull);                        // 1 GB total
        wr64(base, out + 0x10, 0x40000000ull);                        // 1 GB free
        const char* name = "Hard Drive";
        for (int k = 0; name[k] && k < 27; k++) *(uint16_t*)(base + out + 0x18 + k * 2) = _byteswap_ushort((uint16_t)name[k]);
    }
    ctx.r3.u64 = 0;
}

// XamContentCreateEnumerator(user, deviceId, type, flags, itemsPerEnum, u32* bufferSize, u32* handle)
// sizeof(XCONTENT_DATA) = 0x134: {u32 device; u32 type; wchar name[128]; char file[42]; pad}
static std::atomic<uint32_t> g_enumNext{0x3000};
static std::string save_base();
// each save is a folder in host\saves; "<folder>.xcontent" beside it keeps the XCONTENT_DATA the game created it with.
// A folder without one still lists, under its folder name.
static std::vector<std::vector<uint8_t>> list_saves(uint32_t type) {
    std::vector<std::vector<uint8_t>> out;
    std::string root = save_base();
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string name = fd.cFileName;
        if (name == "." || name == ".." || name.size() > 41) continue;
        std::vector<uint8_t> item(kContentDataSize, 0);
        bool meta = false;
        if (FILE* f = fopen((root + "\\" + name + ".xcontent").c_str(), "rb")) {
            meta = fread(item.data(), 1, kContentDataSize, f) == kContentDataSize;
            fclose(f);
        }
        if (!meta) {
            std::fill(item.begin(), item.end(), 0);
            *(uint32_t*)&item[4] = _byteswap_ulong(type);
            for (size_t k = 0; k < name.size(); k++) *(uint16_t*)&item[8 + k * 2] = _byteswap_ushort((uint16_t)(unsigned char)name[k]);
            memcpy(&item[0x108], name.data(), name.size());
        }
        *(uint32_t*)&item[0] = _byteswap_ulong(kFakeDeviceId);
        if (_byteswap_ulong(*(uint32_t*)&item[4]) == type) out.push_back(std::move(item));
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    return out;
}
static void XamContentCreateEnumerator_(PPCContext& ctx, uint8_t* base) {
    uint32_t id = ctx.r4.u32, type = ctx.r5.u32, items = ctx.r7.u32, sizeOut = ctx.r8.u32, handleOut = ctx.r9.u32;
    uint32_t gh = g_enumNext.fetch_add(4);
    ContentEnum e;
    if (id == kFakeDeviceId || id == 0) e.items = list_saves(type);    // 0 = any device
    fprintf(stderr, "[save] XamContentCreateEnumerator user=%u dev=%u type=%u flags=%08X items=%u -> %08X found=%zu lr=%08X\n",
            ctx.r3.u32, id, type, ctx.r6.u32, items, gh, e.items.size(), (uint32_t)ctx.lr);
    { std::lock_guard<std::mutex> g(g_fileLock); g_contentEnums[gh] = std::move(e); }
    if (sizeOut) wr32(base, sizeOut, items * kContentDataSize);
    if (handleOut) wr32(base, handleOut, gh);
    ctx.r3.u64 = 0;
}

// XamEnumerate(handle, flags, buffer, size, u32* itemsReturned, overlapped*): hands out what the enumerator found,
// then ERROR_NO_MORE_FILES
static void XamEnumerate_(PPCContext& ctx, uint8_t* base) {
    const uint32_t kNoMoreFiles = 18;
    uint32_t gh = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32, itemsOut = ctx.r7.u32, ov = ctx.r8.u32, n = 0;
    {
        std::lock_guard<std::mutex> g(g_fileLock);
        auto it = g_contentEnums.find(gh);
        if (it != g_contentEnums.end()) {
            ContentEnum& e = it->second;
            uint32_t fit = size / kContentDataSize;
            while (n < fit && e.next < e.items.size()) {
                memcpy(base + buf + n * kContentDataSize, e.items[e.next].data(), kContentDataSize);
                e.next++; n++;
            }
        }
    }
    uint32_t status = n ? 0 : kNoMoreFiles;
    fprintf(stderr, "[save] XamEnumerate handle=%08X size=%u itemsOut=%08X ov=%08X lr=%08X -> items=%u status=%u\n",
            gh, size, itemsOut, ov, (uint32_t)ctx.lr, n, status);
    if (itemsOut) wr32(base, itemsOut, n);
    if (ov) { complete_overlapped(base, ov, status, n); ctx.r3.u64 = 997; }
    else ctx.r3.u64 = status;
}

static std::string save_base() {                       // host\saves, beside the build folder
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string s = exe;
    size_t p = s.find_last_of('\\');
    s = (p == std::string::npos ? std::string(".") : s.substr(0, p)) + "\\..\\saves";
    CreateDirectoryA(s.c_str(), nullptr);
    return s;
}
// XCONTENT_DATA: file name is at +0x108, up to 42 chars; keep only what is safe as one folder name
static bool content_folder(uint8_t* base, uint32_t data, std::string& out) {
    char nm[43] = {};
    memcpy(nm, base + data + 0x108, 42);
    std::string f = nm;
    if (f.empty() || f.find("..") != std::string::npos || f.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
    out = save_base() + "\\" + f;
    return true;
}
static uint32_t finish_xam(PPCContext& ctx, uint8_t* base, uint32_t ov, uint32_t status) {
    if (ov) { complete_overlapped(base, ov, status); return 997; }
    return status;
}

// XamContentCreateEx(user, root, data*, flags, disposition*, license*, cacheSize, u64 contentSize, overlapped* [stack])
// flags & 0xF: 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing
static void XamContentCreateEx_(PPCContext& ctx, uint8_t* base) {
    std::string root((const char*)(base + ctx.r4.u32));
    uint32_t data = ctx.r5.u32, flags = ctx.r6.u32, dispOut = ctx.r7.u32, licOut = ctx.r8.u32;
    uint32_t ov = rd32(base, ctx.r1.u32 + 0x54), mode = flags & 0xF;
    std::string dir;
    uint32_t status = 0, disp = 0;
    if (!content_folder(base, data, dir)) status = 87;  // ERROR_INVALID_PARAMETER
    else {
        DWORD attr = GetFileAttributesA(dir.c_str());
        bool exists = attr != INVALID_FILE_ATTRIBUTES;
        if (mode == 1 && exists) status = 183;          // ERROR_ALREADY_EXISTS
        else if ((mode == 3 || mode == 5) && !exists) status = 3;   // ERROR_PATH_NOT_FOUND
        else {
            if (exists && (mode == 2 || mode == 5)) {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
                exists = false;
            }
            if (!exists) CreateDirectoryA(dir.c_str(), nullptr);
            disp = exists ? 2 : 1;                      // opened / created
            std::string metaPath = dir + ".xcontent";
            if (!exists || GetFileAttributesA(metaPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                if (FILE* f = fopen(metaPath.c_str(), "wb")) { fwrite(base + data, 1, kContentDataSize, f); fclose(f); }
            }
            std::lock_guard<std::mutex> g(g_fileLock);
            g_contentRoots[lower_str(root)] = dir;
        }
    }
    fprintf(stderr, "[save] XamContentCreateEx root=\"%s\" folder=\"%s\" flags=%08X -> status=%u disp=%u ov=%08X lr=%08X\n",
            root.c_str(), dir.c_str(), flags, status, disp, ov, (uint32_t)ctx.lr);
    if (dispOut) wr32(base, dispOut, disp);
    if (licOut) wr32(base, licOut, 0xFFFFFFFF);
    ctx.r3.u64 = finish_xam(ctx, base, ov, status);
}
// XamContentClose(root, overlapped*)
static void XamContentClose_(PPCContext& ctx, uint8_t* base) {
    std::string root((const char*)(base + ctx.r3.u32));
    uint32_t ov = ctx.r4.u32;
    { std::lock_guard<std::mutex> g(g_fileLock); g_contentRoots.erase(lower_str(root)); }
    fprintf(stderr, "[save] XamContentClose root=\"%s\" ov=%08X lr=%08X\n", root.c_str(), ov, (uint32_t)ctx.lr);
    ctx.r3.u64 = finish_xam(ctx, base, ov, 0);
}
// XamContentDelete(user, data*, overlapped*)
static void XamContentDelete_(PPCContext& ctx, uint8_t* base) {
    std::string dir;
    uint32_t ov = ctx.r5.u32, status = 87;
    if (content_folder(base, ctx.r4.u32, dir)) {
        std::error_code ec;
        status = std::filesystem::remove_all(dir, ec) > 0 ? 0 : 2;    // ERROR_FILE_NOT_FOUND
        std::filesystem::remove(dir + ".xcontent", ec);
    }
    fprintf(stderr, "[save] XamContentDelete folder=\"%s\" -> status=%u\n", dir.c_str(), status);
    ctx.r3.u64 = finish_xam(ctx, base, ov, status);
}

// XamContentGetCreator(user, data*, bool* isCreator, u64* xuid, overlapped*): every local save belongs to the local profile
static void XamContentGetCreator_(PPCContext& ctx, uint8_t* base) {
    uint32_t isOut = ctx.r5.u32, xuidOut = ctx.r6.u32, ov = ctx.r7.u32;
    std::string dir;
    uint32_t status = 2;                                // ERROR_FILE_NOT_FOUND
    if (content_folder(base, ctx.r4.u32, dir) && GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        status = 0;
        if (isOut) wr32(base, isOut, 1);
        if (xuidOut) wr64(base, xuidOut, 0xE000000000000001ull);
    }
    fprintf(stderr, "[save] XamContentGetCreator folder=\"%s\" -> status=%u lr=%08X\n", dir.c_str(), status, (uint32_t)ctx.lr);
    ctx.r3.u64 = finish_xam(ctx, base, ov, status);
}

// XamUserGetName(user, char* buffer, size): a fixed local profile for user 0
static void XamUserGetName_(PPCContext& ctx, uint8_t* base) {
    uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, size = ctx.r5.u32;
    if (user != 0) { ctx.r3.u64 = 0x534; return; }      // ERROR_NO_SUCH_USER
    const char* name = "Player";
    if (buf && size) {
        uint32_t n = (uint32_t)strlen(name);
        if (n >= size) n = size - 1;
        memcpy(base + buf, name, n);
        base[buf + n] = 0;
    }
    ctx.r3.u64 = 0;
}

// XMsgStartIORequest(app, message, overlapped*, buffer, size): asynchronous request to a system service.
// App 0xFB is the game-services (presence, context, stats) app: nothing to do offline, so report success.
static void XMsgStartIORequest_(PPCContext& ctx, uint8_t* base) {
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, ov = ctx.r5.u32, buf = ctx.r6.u32, size = ctx.r7.u32;
    uint32_t status = (app == 0xFB) ? 0 : 0x490;        // ERROR_NOT_FOUND for any other service
    static std::atomic<int> n{0};
    if (n++ < 200)
        fprintf(stderr, "[xam] XMsgStartIORequest app=%08X msg=%08X ov=%08X buf=%08X size=%u -> %u lr=%08X\n",
                app, msg, ov, buf, size, status, (uint32_t)ctx.lr);
    ctx.r3.u64 = finish_xam(ctx, base, ov, status);
}

// XamContentGetLicenseMask(uint32_t* mask, overlapped*): report fully licensed
static void XamContentGetLicenseMask_(PPCContext& ctx, uint8_t* base) {
    if (ctx.r3.u32) wr32(base, ctx.r3.u32, 0xFFFFFFFF);
    ctx.r3.u64 = 0;   // ERROR_SUCCESS
}

// Guest spin locks: a 32-bit word in guest memory, 0 = free. Operate on the raw (big-endian) bytes atomically.
static inline volatile LONG* spin_word(uint8_t* base, uint32_t a) { return (volatile LONG*)(base + a); }
static void KfAcquireSpinLock_(PPCContext& ctx, uint8_t* base) {
    volatile LONG* p = spin_word(base, ctx.r3.u32);
    const LONG want = (LONG)_byteswap_ulong(1);
    while (InterlockedCompareExchange(p, want, 0) != 0) YieldProcessor();
    ctx.r3.u64 = 0;                                   // previous IRQL
}
static void KfReleaseSpinLock_(PPCContext& ctx, uint8_t* base) {
    InterlockedExchange(spin_word(base, ctx.r3.u32), 0);
}
static void KeAcquireSpinLockAtRaisedIrql_(PPCContext& ctx, uint8_t* base) {
    volatile LONG* p = spin_word(base, ctx.r3.u32);
    const LONG want = (LONG)_byteswap_ulong(1);
    while (InterlockedCompareExchange(p, want, 0) != 0) YieldProcessor();
}
static void KeTryToAcquireSpinLockAtRaisedIrql_(PPCContext& ctx, uint8_t* base) {
    const LONG want = (LONG)_byteswap_ulong(1);
    ctx.r3.u64 = (InterlockedCompareExchange(spin_word(base, ctx.r3.u32), want, 0) == 0) ? 1 : 0;
}

void register_all_imports() {
    register_by_name("NtAllocateVirtualMemory",            NtAllocateVirtualMemory_);
    register_by_name("NtFreeVirtualMemory",                NtFreeVirtualMemory_);
    register_by_name("NtQueryVirtualMemory",               NtQueryVirtualMemory_);
    register_by_name("RtlInitializeCriticalSection",       RtlInitializeCriticalSection_);
    register_by_name("RtlInitializeCriticalSectionAndSpinCount", RtlInitializeCriticalSection_);
    register_by_name("RtlEnterCriticalSection",            RtlEnterCriticalSection_);
    register_by_name("RtlLeaveCriticalSection",            RtlLeaveCriticalSection_);
    register_by_name("RtlTryEnterCriticalSection",         RtlTryEnterCriticalSection_);
    register_by_name("KeTlsAlloc",                         KeTlsAlloc_);
    register_by_name("KeTlsFree",                          KeTlsFree_);
    register_by_name("KeTlsSetValue",                      KeTlsSetValue_);
    register_by_name("KeTlsGetValue",                      KeTlsGetValue_);
    register_by_name("KeGetCurrentProcessType",            KeGetCurrentProcessType_);
    register_by_name("XexCheckExecutablePrivilege",        ReturnZero_);
    register_by_name("XNotifyGetNext",                     ReturnZero_);
    register_by_name("XNotifyPositionUI",                  NoOp_);
    register_by_name("NetDll_XNetStartup",                 ReturnZero_);
    register_by_name("NetDll_XNetCleanup",                 ReturnZero_);
    register_by_name("KeQuerySystemTime",                  KeQuerySystemTime_);
    register_by_name("KeQueryPerformanceFrequency",        KeQueryPerformanceFrequency_);
    register_by_name("RtlInitAnsiString",                  RtlInitAnsiString_);
    register_by_name("RtlInitUnicodeString",               RtlInitUnicodeString_);
    register_by_name("RtlNtStatusToDosError",              RtlNtStatusToDosError_);
    register_by_name("NtCreateFile",                       NtCreateFile_);
    register_by_name("NtReadFile",                         NtReadFile_);
    register_by_name("NtQueryInformationFile",             NtQueryInformationFile_);
    register_by_name("NtClose",                            NtClose_);
    register_by_name("NtSetInformationFile",               NtSetInformationFile_);
    register_by_name("NtCreateEvent",                      NtCreateEvent_);
    register_by_name("InterlockedPopEntrySList",           InterlockedPopEntrySList_);
    register_by_name("InterlockedPushEntrySList",          InterlockedPushEntrySList_);
    register_by_name("InterlockedFlushSList",              InterlockedFlushSList_);
    register_by_name("NtClearEvent",               NtClearEvent_);
    register_by_name("NtSetEvent",                 NtSetEvent_);
    register_by_name("NtResetEvent",               NtResetEvent_);
    register_by_name("NtWaitForSingleObjectEx",    NtWaitForSingleObjectEx_);
	register_by_name("NtWaitForMultipleObjectsEx", NtWaitForMultipleObjectsEx_);
    register_by_name("ExGetXConfigSetting", ExGetXConfigSetting_);
    register_by_name("NtOpenFile", NtOpenFile_);
    register_by_name("NtQueryDirectoryFile", NtQueryDirectoryFile_);
    register_by_name("ExCreateThread",  ExCreateThread_);
    register_by_name("NtResumeThread",  NtResumeThread_);
    register_by_name("ObReferenceObjectByHandle", ObReferenceObjectByHandle_);
    register_by_name("ObDereferenceObject",       NoOp_);
    register_by_name("KeSetAffinityThread",     KeSetAffinityThread_);
    register_by_name("KeSetBasePriorityThread", KeSetBasePriorityThread_);
    register_by_name("KeResumeThread",           KeResumeThread_);
    register_by_name("KeDelayExecutionThread", KeDelayExecutionThread_);
	register_by_name("XamGetSystemVersion", ReturnZero_);
	register_by_name("NetDll_XNetCreateKey",     ReturnZero_);
    register_by_name("NetDll_XNetRegisterKey",   ReturnZero_);
    register_by_name("NetDll_XNetUnregisterKey", ReturnZero_);
    register_by_name("NetDll_WSAStartup", NetDll_WSAStartup_);
    register_by_name("NetDll_WSACleanup", ReturnZero_);
    register_by_name("MmQueryStatistics", MmQueryStatistics_);
	register_by_name("XamNotifyCreateListener", XamNotifyCreateListener_);
    register_by_name("XGetVideoMode", XGetVideoMode_);
    register_by_name("ExTerminateThread", ExTerminateThread_);
    register_by_name("MmAllocatePhysicalMemoryEx", MmAllocatePhysicalMemoryEx_);
    register_by_name("MmFreePhysicalMemory",       NoOp_);
    register_by_name("VdInitializeEngines",                          NoOp_);
    register_by_name("VdSetGraphicsInterruptCallback",               VdSetGraphicsInterruptCallback_);
    register_by_name("VdInitializeRingBuffer",                       VdInitializeRingBuffer_);
    register_by_name("VdEnableRingBufferRPtrWriteBack",              VdEnableRingBufferRPtrWriteBack_);
    register_by_name("VdSetSystemCommandBufferGpuIdentifierAddress", VdSetSystemCommandBufferGpuIdentifierAddress_);
    register_by_name("MmGetPhysicalAddress", MmGetPhysicalAddress_);
	register_by_name("KiApcNormalRoutineNop", NoOp_);
	register_by_name("KeInitializeDpc", KeInitializeDpc_);
	register_by_name("KeWaitForSingleObject", KeWaitForSingleObject_);
	register_by_name("KeWaitForMultipleObjects", KeWaitForMultipleObjects_);
    register_by_name("KeSetEvent",            KeSetEvent_);

register_by_name("RtlFillMemoryUlong", RtlFillMemoryUlong_);
	register_by_name("VdQueryVideoMode", XGetVideoMode_);
	register_by_name("VdQueryVideoFlags", VdQueryVideoFlags_);
	register_by_name("VdEnableDisableClockGating", NoOp_);
register_by_name("VdSetDisplayMode",           NoOp_);
register_by_name("VdPersistDisplay",           NoOp_);
register_by_name("VdCallGraphicsNotificationRoutines", NoOp_);
register_by_name("VdRetrainEDRAM",              ReturnZero_);
register_by_name("VdRetrainEDRAMWorker",        ReturnZero_);
register_by_name("VdIsHSIOTrainingSucceeded", VdIsHSIOTrainingSucceeded_);
register_by_name("VdGetCurrentDisplayGamma", VdGetCurrentDisplayGamma_);
register_by_name("VdGetSystemCommandBuffer",         VdGetSystemCommandBuffer_);
register_by_name("VdInitializeScalerCommandBuffer",  ReturnZero_);
register_by_name("VdSwap", VdSwap_);
register_by_name("KeEnterCriticalRegion", NoOp_);
register_by_name("KeLeaveCriticalRegion", NoOp_);
register_by_name("KeResetEvent", KeResetEvent_);
register_by_name("VdGetCurrentDisplayInformation", VdGetCurrentDisplayInformation_);

register_by_name("XamInputGetState",         XamInputGetState_);
register_by_name("XamInputGetCapabilities",  XamInputGetCapabilities_);
register_by_name("XamInputSetState",         XamInputSetState_);
register_by_name("XAudioGetSpeakerConfig",            XAudioGetSpeakerConfig_);
register_by_name("XAudioRegisterRenderDriverClient",  XAudioRegisterRenderDriverClient_);
register_by_name("XAudioUnregisterRenderDriverClient", XAudioUnregisterRenderDriverClient_);
register_by_name("XAudioSubmitRenderDriverFrame",     ReturnZero_);
register_by_name("XAudioGetVoiceCategoryVolume",  XAudioGetVoiceCategoryVolume_);
register_by_name("XAudioSetVoiceCategoryVolume",  ReturnZero_);

register_by_name("NetDll_socket",                     NetDll_socket_);
register_by_name("NetDll_WSAGetLastError",            NetDll_WSAGetLastError_);
register_by_name("NetDll_closesocket",                ReturnZero_);
register_by_name("NetDll_XNetGetEthernetLinkStatus",  ReturnZero_);    // 0 = no link
register_by_name("NetDll_XNetStartup",                ReturnZero_);
register_by_name("NetDll_XNetCleanup",                ReturnZero_);
register_by_name("NetDll_ioctlsocket",   NetErr_);
register_by_name("NetDll_setsockopt",    NetErr_);
register_by_name("NetDll_getsockopt",    NetErr_);
register_by_name("NetDll_bind",          NetErr_);
register_by_name("NetDll_connect",       NetErr_);
register_by_name("NetDll_listen",        NetErr_);
register_by_name("NetDll_accept",        NetErr_);
register_by_name("NetDll_send",          NetErr_);
register_by_name("NetDll_sendto",        NetErr_);
register_by_name("NetDll_recv",          NetErr_);
register_by_name("NetDll_recvfrom",      NetErr_);
register_by_name("NetDll_select",        NetErr_);
register_by_name("NetDll_getsockname",   NetErr_);
register_by_name("NetDll_shutdown",      NetErr_);
register_by_name("NetDll_XNetGetTitleXnAddr", ReturnZero_);

register_by_name("XamUserGetSigninState", XamUserGetSigninState_);
register_by_name("XGetGameRegion", XGetGameRegion_);
register_by_name("XamInputGetKeystrokeEx", XamInputGetKeystrokeEx_);
register_by_name("XamInputGetKeystroke",   XamInputGetKeystrokeEx_);
register_by_name("XMsgInProcessCall", XMsgInProcessCall_);
register_by_name("XamContentGetLicenseMask", XamContentGetLicenseMask_);
register_by_name("XamShowDeviceSelectorUI", XamShowDeviceSelectorUI_);
register_by_name("XamContentGetDeviceState", XamContentGetDeviceState_);
register_by_name("XamContentGetDeviceData", XamContentGetDeviceData_);
register_by_name("XamContentCreateEnumerator", XamContentCreateEnumerator_);
register_by_name("XamEnumerate", XamEnumerate_);
register_by_name("XMsgStartIORequest", XMsgStartIORequest_);
register_by_name("XamUserGetName", XamUserGetName_);
register_by_name("MmQueryAllocationSize", MmQueryAllocationSize_);
register_by_name("XamContentCreateEx", XamContentCreateEx_);
register_by_name("XamContentClose", XamContentClose_);
register_by_name("XamContentDelete", XamContentDelete_);
register_by_name("XamContentGetCreator", XamContentGetCreator_);
register_by_name("NtWriteFile", NtWriteFile_);
register_by_name("NtFlushBuffersFile", NtFlushBuffersFile_);
register_by_name("XNotifyGetNext", ReturnZero_);
register_by_name("KfAcquireSpinLock",                      KfAcquireSpinLock_);
register_by_name("KfReleaseSpinLock",                      KfReleaseSpinLock_);
register_by_name("KeAcquireSpinLockAtRaisedIrql",          KeAcquireSpinLockAtRaisedIrql_);
register_by_name("KeReleaseSpinLockFromRaisedIrql",        KfReleaseSpinLock_);
register_by_name("KeTryToAcquireSpinLockAtRaisedIrql",     KeTryToAcquireSpinLockAtRaisedIrql_);

register_by_name("ExRegisterTitleTerminateNotification", NoOp_);
	
}
	
void set_content_root(const char* p);
