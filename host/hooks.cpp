#include "ppc_config.h"
#include "ppc_context.h"
#include <windows.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
struct ShRec { char tag; DWORD tid; uint32_t before, after, a4, a5, lr; };
static ShRec g_shRing[64];
static std::atomic<unsigned> g_shN{0};
bool dbg_trace_on(); unsigned dbg_trace_seq();

void shadow_dump();

static void sh_rec(char tag, uint32_t b, uint32_t a, uint32_t a4, uint32_t a5, uint32_t lr) {
    unsigned i = g_shN.fetch_add(1) & 63;
    g_shRing[i] = { tag, GetCurrentThreadId(), b, a, a4, a5, lr };
    static std::atomic<uint32_t> lastAfter{0xFFFFFFFFu};
    uint32_t p = lastAfter.exchange(a);
    uint32_t d = (p - a) & 0x1FFF;
    if (p != 0xFFFFFFFFu && d != 0 && d < 0x100) {
        static std::atomic<int> once{0};
        if (once.fetch_add(1) == 0) {
            fprintf(stderr, "[shring] RETRACT tag=%c prev=%X now=%X before=%X\n", tag, p, a, b);
            shadow_dump();
            fflush(stderr);
        }
    }
}

void shadow_dump() {
    unsigned n = g_shN.load();
    unsigned s = n > 64 ? n - 64 : 0;
    fprintf(stderr, "[shring] n=%u\n", n);
    for (unsigned k = s; k < n; k++) {
        const ShRec& r = g_shRing[k & 63];
        fprintf(stderr, "[shring] %c tid=%lu before=%X after=%X a4=%08X a5=%08X lr=%08X\n",
                r.tag, (unsigned long)r.tid, r.before, r.after, r.a4, r.a5, r.lr);
    }
}

extern "C" void __imp__sub_82698880(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82698880) {
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    uint32_t dev = ctx.r3.u32;
    uint32_t b11012 = PPC_LOAD_U32(dev + 11012), bsub = PPC_LOAD_U32(dev + 10908);
    __imp__sub_82698880(ctx, base);
    if (i < 30 || (i % 300) == 0)
        fprintf(stderr, "[%lu] [vblank] 82698880 #%d f11012 %08X->%08X sub=%08X fenceHW=%08X wptr=%08X\n",
                GetTickCount(), i, b11012, PPC_LOAD_U32(dev + 11012), bsub,
                PPC_LOAD_U32(PPC_LOAD_U32(dev + 10896)), PPC_LOAD_U32(0x7FC80714));
}

extern "C" void __imp__sub_8269D630(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269D630) {
    uint32_t dev = ctx.r3.u32, want = ctx.r4.u32;
    auto pending = [&]() {
        uint32_t cur = PPC_LOAD_U32(PPC_LOAD_U32(dev + 10896));
        uint32_t sub = PPC_LOAD_U32(dev + 10908);
        return want && ((sub - want) < (sub - cur));
    };
    if (!pending()) { __imp__sub_8269D630(ctx, base); return; }

    uint32_t sub = PPC_LOAD_U32(dev + 10908);
    if (want == sub) { __imp__sub_8269D630(ctx, base); return; }   // exact-latest wait: let the original kick

    // Throttle wait on an older fence: give the GPU thread a moment, then stop blocking.
    for (int i = 0; i < 3000 && pending(); i++) Sleep(1);
    if (pending()) {
        static std::atomic<int> skipped{0};
        int n = skipped.fetch_add(1);
        if (n < 5 || (n % 500) == 0)
            fprintf(stderr, "[fence] skipped throttle wait #%d want=%08X cur=%08X sub=%08X\n",
                    n, want, PPC_LOAD_U32(PPC_LOAD_U32(dev + 10896)), PPC_LOAD_U32(dev + 10908));
        return;
    }
    __imp__sub_8269D630(ctx, base);
}

extern "C" void __imp__sub_8269E528(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269E528) {
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    uint32_t dev = ctx.r3.u32;
    if (i < 400)
        fprintf(stderr, "[%lu] [kick] E528 #%d wptr=%08X rptr=%08X wp48=%08X f13408=%08X f10940=%02X rw=%08X rlim=%08X lr=%08X\n",
                GetTickCount(), i, PPC_LOAD_U32(0x7FC80714), PPC_LOAD_U32(0x7FC80710), PPC_LOAD_U32(dev + 48),
                PPC_LOAD_U32(dev + 13408), (unsigned)PPC_LOAD_U8(dev + 10940),
                PPC_LOAD_U32(dev + 13580), PPC_LOAD_U32(dev + 13584), (uint32_t)ctx.lr);
    __imp__sub_8269E528(ctx, base);
}

extern "C" void __imp__sub_8269E188(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269E188) {
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    if (ctx.r7.u32 != 0)
        fprintf(stderr, "[%lu] [e188] r7=%08X r9=%08X ring=%08X size=%08X lr=%08X\n",
                GetTickCount(), ctx.r7.u32, ctx.r9.u32, ctx.r4.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    if (i < 400)
        fprintf(stderr, "[%lu] [kick] E188 (IB submit) #%d ring=%08X buf=%08X size=%08X f13408=%08X f11012=%08X f10941=%02X\n",
                GetTickCount(), i, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                PPC_LOAD_U32(ctx.r3.u32 + 13408), PPC_LOAD_U32(ctx.r3.u32 + 11012),
                (unsigned)PPC_LOAD_U8(ctx.r3.u32 + 10941));
	if (ctx.r7.u32 != 0) {
        static std::atomic<int> dumped{0};
        if (dumped.fetch_add(1) < 2) {
            fprintf(stderr, "[e188-dump] phys=%08X dwords=%u:", ctx.r5.u32, ctx.r6.u32);
            for (uint32_t k = 0; k < ctx.r6.u32 && k < 64; k++)
                fprintf(stderr, "%s%08X", (k % 8) ? " " : "\n  ", PPC_LOAD_U32(0xA0000000u + ctx.r5.u32 + k * 4));
            fprintf(stderr, "\n");
        }
    }
    __imp__sub_8269E188(ctx, base);
}

extern "C" void __imp__sub_8269D208(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269D208) {
    static std::atomic<int> n0{0}, n1{0};
    uint32_t src = ctx.r3.u32;
    int c = (src == 1) ? n1.fetch_add(1) : n0.fetch_add(1);
    if ((src == 1 && (c < 20 || c % 100 == 0)) || (src == 0 && c < 3))
        fprintf(stderr, "[%lu] [isr] source=%u #%d dev=%08X cb=%08X cbctx=%08X\n", GetTickCount(), src, c, ctx.r4.u32,
                PPC_LOAD_U32(PPC_LOAD_U32(ctx.r4.u32 + 10900) + 16), PPC_LOAD_U32(PPC_LOAD_U32(ctx.r4.u32 + 10900) + 20));
    __imp__sub_8269D208(ctx, base);
}

extern "C" void __imp__sub_826A3158(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A3158) {
    {
        static std::atomic<int> bpWait{0}, bpTimeout{0};
        const uint32_t bq = 0x40004D18;                    // worker queue: prod +84, cons +88, 4 payload slots
        auto bt0 = std::chrono::steady_clock::now();
        bool bwaited = false;
        while ((uint32_t)(PPC_LOAD_U32(bq + 84) - PPC_LOAD_U32(bq + 88)) >= 3) {   // keep a free slot: the ring has no full check
            bwaited = true;
            if (std::chrono::steady_clock::now() - bt0 > std::chrono::milliseconds(250)) {
                if (bpTimeout.fetch_add(1) < 10)
                    fprintf(stderr, "[bp] timeout prod=%u cons=%u\n", PPC_LOAD_U32(bq + 84), PPC_LOAD_U32(bq + 88));
                break;
            }
            std::this_thread::yield();
        }
        if (bwaited && bpWait.fetch_add(1) < 10)
            fprintf(stderr, "[bp] waited %lld us, prod=%u cons=%u\n",
                    (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - bt0).count(),
                    PPC_LOAD_U32(bq + 84), PPC_LOAD_U32(bq + 88));
    }
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    uint32_t arg = ctx.r3.u32;
    uint32_t cpu = PPC_LOAD_U8(ctx.r13.u32 + 268);
    uint32_t ev  = 0x40002000u + cpu * 108 + 11388;
    if (i < 4000) {
        uint32_t q = 0x40002000u + cpu * 108 + 11328;
        fprintf(stderr, "[push] isr #%d pl=%08X cpu=%u q=%08X prod=%u cons=%u lr=%08X ctr=%d fence=%X\n",
                i, arg, cpu, q, PPC_LOAD_U32(q + 84), PPC_LOAD_U32(q + 88), (uint32_t)ctx.lr,
                (int)PPC_LOAD_U32(0x40002000u + 11012), PPC_LOAD_U32(0xA0002000u));
    }
    if (i < 20 || i % 100 == 0)
        fprintf(stderr, "[%lu] [cb] 826A3158 #%d r3=%08X cpu=%u ev=%08X type=%u state=%d (before)\n",
                GetTickCount(), i, arg, cpu, ev, base[ev], (int)PPC_LOAD_U32(ev + 4));
    if (i < 3) {
        uint32_t a0 = 0xA0000000u | (arg & 0x1FFFFFFFu);
        fprintf(stderr, "[cb-entry] C0 @%08X:", arg);
        for (int k = 0; k < 12; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(arg + k * 4));
        fprintf(stderr, "\n[cb-entry] A0 @%08X:", a0);
        for (int k = 0; k < 12; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(a0 + k * 4));
        fprintf(stderr, "\n");
        uint32_t link = PPC_LOAD_U32(arg);
        if ((link & 0xC0000000u) == 0xC0000000u) {
            fprintf(stderr, "[cb-chain] @%08X:", link);
            for (int k = 0; k < 24; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(link + k * 4));
            fprintf(stderr, "\n");
        }
    }
    __imp__sub_826A3158(ctx, base);
    if (i < 20 || i % 100 == 0)
        fprintf(stderr, "[%lu] [cb] #%d done: ev state=%d\n", GetTickCount(), i, (int)PPC_LOAD_U32(ev + 4));
}

// the DPC-style producer: r4 = queue, r5 = payload
extern "C" void __imp__sub_826A3120(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A3120) {
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    if (i < 4000) {
        uint32_t q = ctx.r4.u32;
        fprintf(stderr, "[push] dpc #%d pl=%08X q=%08X prod=%u cons=%u lr=%08X bt=", i, ctx.r5.u32, q, PPC_LOAD_U32(q + 84), PPC_LOAD_U32(q + 88), (uint32_t)ctx.lr);
        uint32_t sp = ctx.r1.u32;
        for (int k = 0; k < 4; k++) {
            uint32_t prev = PPC_LOAD_U32(sp);
            if (prev <= sp || prev - sp > 0x10000) break;
            fprintf(stderr, " %08X", PPC_LOAD_U32(prev - 8));
            sp = prev;
        }
        fprintf(stderr, "\n");
    }
    __imp__sub_826A3120(ctx, base);
}

extern "C" void __imp__sub_8269D030(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269D030) {
    uint32_t dev = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, lr = ctx.lr;
    uint32_t b = PPC_LOAD_U32(dev + 10956);
    __imp__sub_8269D030(ctx, base);
    sh_rec('P', b, PPC_LOAD_U32(dev + 10956), a4, a5, lr);
}

extern "C" void __imp__sub_826A2D50(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A2D50) {
    uint32_t obj = ctx.r3.u32;
    __imp__sub_826A2D50(ctx, base);
    if (dbg_trace_on())
        fprintf(stderr, "[tr] #%u wk 2D50 tile=%u/%u flags=%08X m108=%08X -> m356=%08X\n", dbg_trace_seq(),
                PPC_LOAD_U32(obj + 52), PPC_LOAD_U32(obj + 112), PPC_LOAD_U32(obj + 48), PPC_LOAD_U32(obj + 108), PPC_LOAD_U32(obj + 356));
}

extern "C" void __imp__sub_8269DB10(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269DB10) {
	uint32_t dev0 = ctx.r3.u32, shA4 = ctx.r4.u32, shA5 = ctx.r5.u32, shLr = ctx.lr;
	uint32_t shb = PPC_LOAD_U32(dev0 + 10956);
    if (dbg_trace_on()) fprintf(stderr, "[tr] #%u wk db10 cnt=%u rec=%08X %08X\n", dbg_trace_seq(), shA5, PPC_LOAD_U32(shA4), PPC_LOAD_U32(shA4 + 4));
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    uint32_t dev = ctx.r3.u32, cnt = ctx.r5.u32, lr = (uint32_t)ctx.lr;
    uint32_t w0 = PPC_LOAD_U32(0x7FC80714), sh0 = PPC_LOAD_U32(dev + 10956);
    uint8_t f41 = PPC_LOAD_U8(dev + 10941);
    __imp__sub_8269DB10(ctx, base);
	sh_rec('D', shb, PPC_LOAD_U32(dev0 + 10956), shA4, shA5, shLr);
    {
        uint32_t w1 = PPC_LOAD_U32(0x7FC80714);
        if (w1 != w0 && ((w1 - w0) & 0x1FFFu) > 0x1000u) {      // moved backwards
            static int rg = 0;
            if (rg++ < 20)
                fprintf(stderr, "[wptr-back] db10 #%d wptr %08X->%08X shadow %08X tid=%lu lr=%08X\n",
                        i, w0, w1, PPC_LOAD_U32(dev + 10956), GetCurrentThreadId(), lr);
        }
    }
    if (i < 600)
        fprintf(stderr, "[%lu] [db10] #%d cnt=%u f10941=%02X obj21748=%08X wptr %08X->%08X shadow %08X->%08X lr=%08X\n",
                GetTickCount(), i, cnt, (unsigned)f41, PPC_LOAD_U32(dev + 21748),
                w0, PPC_LOAD_U32(0x7FC80714), sh0, PPC_LOAD_U32(dev + 10956), lr);
}

struct PktRec { uint32_t n, payload, hdr, idx, resume, obj; };
static PktRec g_pktRing[64];
static uint32_t g_pktN = 0;   // total callback calls
static uint32_t g_pktP = 0;   // packets recorded (idx==0 only)
static bool g_pktFirstMis = false;

static void pktcb_dump_ring(uint8_t* base) {
    (void)base;
    fprintf(stderr, "[pktring] last packets, oldest first (total calls %u):\n", g_pktN);
    uint32_t n = g_pktP;
    for (uint32_t k = (n > 40 ? n - 40 : 0); k < n; k++) {
        const PktRec& r = g_pktRing[k & 63];
        fprintf(stderr, "[pktring] call#%u payload=%08X hdr=%08X len=%X cnt=%X resume=%08X\n",
                r.n, r.payload, r.hdr, (r.hdr >> 16) & 0x7FFF, r.hdr & 0xFFFF, r.resume);
    }
    fflush(stderr);
}

static inline uint32_t safe_load_u32(uint8_t* base, uint32_t a) {
    if (a < 0x10000u || a > 0xFFFFF000u) return 0xBAD0BAD0u;
    return PPC_LOAD_U32(a);
}
#define SAFE_LOAD(a) safe_load_u32(base, (a))

PPC_FUNC(host_pkt_cb) {
    uint32_t obj = ctx.r31.u32, payload = ctx.r5.u32;
    if (ctx.r6.u32 == 0) {
        PktRec& r = g_pktRing[g_pktP & 63];
        r.n = g_pktN; r.payload = payload; r.idx = 0; r.obj = obj;
        r.hdr = (payload >= 0xC0000004u && payload < 0xE0000000u) ? SAFE_LOAD(payload - 4) : 0xBAD0BAD0u;
        r.resume = SAFE_LOAD(obj + 36);
        g_pktP++;
        if ((payload & 3) && !g_pktFirstMis) {
            g_pktFirstMis = true;
            fprintf(stderr, "[pkt-first-misaligned] payload=%08X\n", payload);
            pktcb_dump_ring(base);
			            {
                uint32_t n = g_pktP; int found = 0;
                for (uint32_t k = n - 1; k > 0 && k + 60 > n; k--) {
                    uint32_t a = g_pktRing[k & 63].payload, b = g_pktRing[(k - 1) & 63].payload;
                    if (a - b != 4u && ++found == 2) {      // 2nd most recent non-sequential hop = the link before the derail
                        fprintf(stderr, "[pkt-jump] from payload %08X to %08X\n", b, a);
                        fprintf(stderr, "[pkt-jump] src @%08X:", b - 16);
                        for (int w = 0; w < 16; w++) fprintf(stderr, " %08X", SAFE_LOAD(b - 16 + w * 4));
                        fprintf(stderr, "\n[pkt-jump] dst @%08X:", a - 32);
                        for (int w = 0; w < 16; w++) fprintf(stderr, " %08X", SAFE_LOAD(a - 32 + w * 4));
                        fprintf(stderr, "\n");
                        uint32_t L = SAFE_LOAD(b);           // the link word itself
                        if (L >= 0xC0000000u && L < 0xE0000000u) {
                            fprintf(stderr, "[pkt-jump] link value %08X, target @%08X:", L, L);
                            for (int w = 0; w < 16; w++) fprintf(stderr, " %08X", SAFE_LOAD(L + w * 4));
                            fprintf(stderr, "\n");
                        }
                        break;
                    }
                }
            }
        }
    }
    {
        static bool firstOut = false;
        if (!firstOut && (payload < 0xC0000000u || payload >= 0xE0000000u)) {
            firstOut = true;
            fprintf(stderr, "[pkt-first-out] call#%u payload=%08X idx=%u obj=%08X cb=%08X +24=%08X +28=%08X +32=%08X +36=%08X +72=%08X +76=%08X m356=%08X m360=%08X\n",
                    g_pktN, payload, ctx.r6.u32, obj, SAFE_LOAD(obj + 16), SAFE_LOAD(obj + 24),
                    SAFE_LOAD(obj + 28), SAFE_LOAD(obj + 32), SAFE_LOAD(obj + 36),
                    SAFE_LOAD(obj + 72), SAFE_LOAD(obj + 76), SAFE_LOAD(obj + 356), SAFE_LOAD(obj + 360));
            uint32_t rs = SAFE_LOAD(obj + 36);
            if (rs > 0x10000 && rs < 0xE0000000u) {
                fprintf(stderr, "[pkt-words] @%08X:", rs - 48);
                for (int k = 0; k < 28; k++) fprintf(stderr, " %08X", SAFE_LOAD(rs - 48 + k * 4));
                fprintf(stderr, "\n");
            }
            pktcb_dump_ring(base);
        }
    }
    if (g_pktN < 8)
        fprintf(stderr, "[pktcb] #%u queue=%08X ctx=%08X payload=%08X idx=%08X\n",
                g_pktN, ctx.r3.u32, ctx.r4.u32, payload, ctx.r6.u32);
    g_pktN++;
    ctx.r3.u64 = 0;
}

static uint32_t install_pkt_cb(uint8_t* base) {
    static uint32_t addr = 0;
    if (addr) return addr;
    for (uint32_t a = PPC_CODE_BASE + PPC_CODE_SIZE - 4; ; a -= 4) {      // find an unused function-table slot
        PPCFunc** slot = (PPCFunc**)(base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (uint64_t(a - PPC_CODE_BASE) * 2));
        if (!*slot) { *slot = (PPCFunc*)host_pkt_cb; addr = a; break; }
    }
    fprintf(stderr, "[pktcb] installed stand-in handler at guest %08X\n", addr);
    return addr;
}

// ---- Lua call tracing (BCR2_LUATRACE): sub_82634458 is luaD_precall(L, func, nresults) in this Lua 5.1 ----
// TValue is 8 bytes {value, tag}; function tag = 6. Closure: +6 isC, +16 Proto*. Proto: +32 source (TString*),
// +60 linedefined. TString: characters at +16.
struct LuaProtoInfo { std::string src; uint32_t line; uint64_t hits; };
static std::mutex g_luaLock;
static std::unordered_map<uint32_t, LuaProtoInfo> g_luaProtos;
static std::string g_luaLastState;
static void lua_trace_call(uint8_t* base, uint32_t func) {
    if (PPC_LOAD_U32(func + 4) != 6) return;
    uint32_t cl = PPC_LOAD_U32(func);
    if (PPC_LOAD_U8(cl + 6) != 0) return;                     // C function
    uint32_t p = PPC_LOAD_U32(cl + 16);
    std::lock_guard<std::mutex> g(g_luaLock);
    auto it = g_luaProtos.find(p);
    if (it == g_luaProtos.end()) {
        LuaProtoInfo info;
        uint32_t ts = PPC_LOAD_U32(p + 32);
        info.src = ts ? std::string((const char*)(base + ts + 16)) : std::string("?");
        info.line = PPC_LOAD_U32(p + 60);
        info.hits = 0;
        it = g_luaProtos.emplace(p, std::move(info)).first;
        static int nNew = 0;
        if (nNew++ < 30000) fprintf(stderr, "[lua] new %s:%u t=%lu\n", it->second.src.c_str(), it->second.line, GetTickCount());
    }
    it->second.hits++;
    const std::string& s = it->second.src;
    if (s.find("/player/states/") != std::string::npos && s != g_luaLastState) {
        static int nState = 0;
        if (nState++ < 20000) fprintf(stderr, "[lua-state] %s -> %s:%u t=%lu\n", g_luaLastState.c_str(), s.c_str(), it->second.line, GetTickCount());
        g_luaLastState = s;
    }
    static DWORD lastTop = 0;
    DWORD now = GetTickCount();
    if (now - lastTop >= 5000) {                              // busiest scripts since the last report
        lastTop = now;
        std::unordered_map<std::string, uint64_t> bySrc;
        for (auto& kv : g_luaProtos) { bySrc[kv.second.src] += kv.second.hits; kv.second.hits = 0; }
        std::vector<std::pair<uint64_t, std::string>> top;
        for (auto& kv : bySrc) if (kv.second) top.push_back({ kv.second, kv.first });
        std::sort(top.rbegin(), top.rend());
        fprintf(stderr, "[lua-top] t=%lu scripts=%zu:", now, top.size());
        for (size_t k = 0; k < top.size() && k < 14; k++) fprintf(stderr, " %s=%llu", top[k].second.c_str(), (unsigned long long)top[k].first);
        fprintf(stderr, "\n");
    }
}
extern "C" void __imp__sub_82634458(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82634458) {
    static const bool kTrace = getenv("BCR2_LUATRACE") != nullptr;
    if (kTrace) lua_trace_call(base, ctx.r4.u32);
    __imp__sub_82634458(ctx, base);
}

// A stub the recompiler has no entry for: the function before it ends in a no-return call, so analysis
// missed the boundary. Its three siblings at 82394938.. have entries. Only ever reached through a pointer.
//   li r6,0x80 ; li r5,9 ; addi r3,r3,0x88 ; b 0x82371540
void sub_82371540(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(host_stub_82394928) {
    ctx.r6.s64 = 0x80;
    ctx.r5.s64 = 9;
    ctx.r3.s64 = ctx.r3.s64 + 0x88;
    sub_82371540(ctx, base);
}
static void install_missing_stubs(uint8_t* base) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    const uint32_t a = 0x82394928u;
    PPCFunc** slot = (PPCFunc**)(base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (uint64_t(a - PPC_CODE_BASE) * 2));
    *slot = (PPCFunc*)host_stub_82394928;     // main.cpp defaults every slot to its reporting stub, so never "empty"
    fprintf(stderr, "[stub] registered %08X\n", a);
}

static const bool kRecover = false;   // true = old __try / early-return recovery
static std::atomic<uint32_t> g_wkPl{0};   // payload the worker most recently started, and its repeat count
static std::atomic<int> g_wkReps{0};
extern "C" void __imp__sub_826A3BF8(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A3BF8) {
    static std::atomic<int> n{0}, faults{0};
    int i = n.fetch_add(1);
    install_missing_stubs(base);
    uint32_t q = ctx.r3.u32, o = PPC_LOAD_U32(q);
    {
        uint32_t job = PPC_LOAD_U32(q + (23 + (PPC_LOAD_U32(q + 88) & 3)) * 4);
        if (job > 0x10000 && job < 0xE0000000u && PPC_LOAD_U32(job + 4) != 0x80000000u) {
            static int bad = 0;
            if (bad++ < 6) {
                fprintf(stderr, "[wk-badjob] #%d ctx=%08X words:", bad, job);
                for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(job + k * 4));
                fprintf(stderr, "\n");
            }
        }
    }
    {
        struct SeenRec { uint64_t sig; uint32_t fence; int reps; };
        static std::unordered_map<uint32_t, SeenRec> seen;   // payload -> (stream signature, fence at last start, repeats at that fence)
        static const bool kSkipReplays = true;
        static const int kMaxSameFenceRepeats = 12;          // > max frames in flight
        static int nskip = 0, nstart = 0;
        for (;;) {
            uint32_t cons = PPC_LOAD_U32(q + 88), prod = PPC_LOAD_U32(q + 84);
            if (prod == cons || PPC_LOAD_U32(o + 72) != 0) break;
            uint32_t pl = PPC_LOAD_U32(q + (23 + (cons & 3)) * 4);
            if (pl < 0x10000 || pl >= 0xE0000000u) break;
            uint32_t endp = PPC_LOAD_U32(pl);
            uint32_t len = (endp > pl) ? (endp - pl) / 4 : 64;
            if (len == 0 || len > 2048) len = 64;
            uint64_t sig = endp;
            for (uint32_t k = 0; k < len; k++) sig = (sig * 1099511628211ull) ^ PPC_LOAD_U32(pl + 4 + k * 4);
            uint32_t fnow = PPC_LOAD_U32(0xA0002000);
            nstart++;
            auto it = seen.find(pl);
            int reps = 0;
            if (it != seen.end() && it->second.sig == sig && it->second.fence == fnow) reps = it->second.reps + 1;
            g_wkPl = pl; g_wkReps = reps;
            {   // uncapped totals, for long runs
                static int totRep = 0, totNeg = 0;
                if (reps > 0) totRep++;
                if ((int32_t)PPC_LOAD_U32(o + 64) < 0) totNeg++;
                if ((nstart % 1000) == 0)
                    fprintf(stderr, "[csum] starts=%d reps=%d negctr=%d skipped=%d t=%lu\n", nstart, totRep, totNeg, nskip, GetTickCount());
            }
            { static int nctr = 0;
              if (nctr++ < 4000)
                  fprintf(stderr, "[ctr] start pl=%08X ctr=%d fence=%08X reps=%d prod=%u cons=%u\n",
                          pl, (int)PPC_LOAD_U32(o + 64), fnow, reps, prod, cons); }
            if (kSkipReplays && reps > kMaxSameFenceRepeats) {
                PPC_STORE_U32(q + 88, cons + 1);
                nskip++;
                if (nskip <= 10 || (nskip % 500) == 0)
                    fprintf(stderr, "[skip] replay ctx=%08X len=%u cons=%u fence=%08X reps=%d (skipped %d of %d starts)\n", pl, len, cons, fnow, reps, nskip, nstart);
                continue;
            }
            seen[pl] = { sig, fnow, reps };
            break;
        }
    }
    if (i < 6) {
        fprintf(stderr, "[worker] #%d q=%08X obj=%08X head=%u tail=%u +16=%08X +356=%08X +360=%08X\n",
                i, q, o, PPC_LOAD_U32(q + 84), PPC_LOAD_U32(q + 88),
                PPC_LOAD_U32(o + 16), PPC_LOAD_U32(o + 356), PPC_LOAD_U32(o + 360));
        if (i < 3) {
            uint32_t rec = PPC_LOAD_U32(q + (23 + (PPC_LOAD_U32(q + 88) & 3)) * 4);
            fprintf(stderr, "[rec] #%d @%08X:", i, rec);
            for (int k = 0; k < 80; k++) {
                if (k && (k % 8) == 0) fprintf(stderr, "\n[rec]            ");
                fprintf(stderr, " %08X", PPC_LOAD_U32(rec + k * 4));
            }
            fprintf(stderr, "\n");
            uint32_t lk = PPC_LOAD_U32(rec + 0x124);
            if ((lk & 0xC0000000u) == 0xC0000000u) {
                fprintf(stderr, "[rec] link target @%08X:", lk);
                for (int k = 0; k < 40; k++) {
                    if (k && (k % 8) == 0) fprintf(stderr, "\n[rec]            ");
                    fprintf(stderr, " %08X", PPC_LOAD_U32(lk + k * 4));
                }
                fprintf(stderr, "\n");
            }
        }
    }
    auto st = [&](const char* tag) {
        fprintf(stderr, "[wk] %s #%d cpu=%u lock=%u m56=%08X part60=%u rel96=%u arr100=%u prod=%u cons=%u m356=%08X m360=%08X\n",
                tag, i, PPC_LOAD_U32(q + 4), PPC_LOAD_U32(o), PPC_LOAD_U32(o + 56), PPC_LOAD_U32(o + 60),
                PPC_LOAD_U32(o + 96), PPC_LOAD_U32(o + 100), PPC_LOAD_U32(q + 84), PPC_LOAD_U32(q + 88),
                PPC_LOAD_U32(o + 356), PPC_LOAD_U32(o + 360));
    };
    if (i < 80) st("enter");
    if (PPC_LOAD_U32(o + 16) == 0) PPC_STORE_U32(o + 16, install_pkt_cb(base));
    if (!kRecover) {
        __imp__sub_826A3BF8(ctx, base);
        {   // keep the outstanding-job counter (obj+64 == dev+11012) from drifting negative while the worker is idle
            static std::atomic<int> nclamp{0};
            int32_t c = (int32_t)PPC_LOAD_U32(o + 64);
            static const bool kNoClamp = getenv("BCR2_NOCLAMP") != nullptr;   // experiment switch
            if (c < 0 && PPC_LOAD_U32(q + 84) == PPC_LOAD_U32(q + 88) && PPC_LOAD_U32(o + 72) == 0) {
                if (!kNoClamp) PPC_STORE_U32(o + 64, 0);
                if (nclamp.fetch_add(1) < 200) fprintf(stderr, "[clamp] obj+64 %d %s\n", c, kNoClamp ? "left alone" : "-> 0");
            }
        }
        if (i < 80) st("exit ");
        return;
    }
    PPCContext saved = ctx;
    __try {
        __imp__sub_826A3BF8(ctx, base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int f = faults.fetch_add(1);
        if (f < 5) {
            uint32_t cnt = PPC_LOAD_U32(o + 28), pay = PPC_LOAD_U32(o + 32), nxt = PPC_LOAD_U32(o + 36);
            fprintf(stderr, "[worker] fault #%d: cnt=%08X payload=%08X resume=%08X mask356=%08X mask360=%08X\n",
                    f, cnt, pay, nxt, PPC_LOAD_U32(o + 356), PPC_LOAD_U32(o + 360));
        }
        ctx = saved;
        PPC_STORE_U32(o + 0, 0);
    }
}


extern "C" void __imp__sub_826A38B0(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A38B0) {
	const uint32_t x_in = ctx.r3.u32;
{
    uint32_t in = ctx.r3.u32;
    bool ok = ((in >= 0xC0000000u && in < 0xE0000000u) || (in >= 0x40000000u && in < 0x50000000u)) && !(in & 3);
    if (!ok) {
        static int bad = 0;
        if (bad++ < 5) {
            uint32_t o = ctx.r4.u32;
            fprintf(stderr, "[exec-bad] in=%08X obj=%08X [+32]=%08X [+36]=%08X [+72]=%08X\n",
                    in, o, PPC_LOAD_U32(o + 32), PPC_LOAD_U32(o + 36), PPC_LOAD_U32(o + 72));
            uint32_t a = (in & ~3u) - 32;
            if (a >= 0xC0000000u && a < 0xDFFFFF00u) {
                fprintf(stderr, "[exec-bad] words @%08X:", a);
                for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(a + k * 4));
                fprintf(stderr, "\n");
            }
            pktcb_dump_ring(base);
        }
        if (kRecover) { ctx.r3.u64 = 0; return; }
    }
}
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    uint32_t in = ctx.r3.u32, obj = ctx.r4.u32;
    if (dbg_trace_on()) fprintf(stderr, "[tr] #%u wk exec in=%08X tile=%u/%u flags=%08X m36=%08X\n", dbg_trace_seq(), in, PPC_LOAD_U32(obj + 52), PPC_LOAD_U32(obj + 112), PPC_LOAD_U32(obj + 48), PPC_LOAD_U32(obj + 36));
    {
        static int xn = 0;
        uint32_t cons = PPC_LOAD_U32(0x40004D18 + 88);
        if (cons >= 20000 && xn < 40) {
            xn++;
            fprintf(stderr, "[exec2] cons=%u in=%08X obj=%08X tile=%u/%u m356=%08X m360=%08X flags=%08X p364=%08X p368=%08X p372=%08X |",
                    cons, in, obj, PPC_LOAD_U32(obj + 52), PPC_LOAD_U32(obj + 112), PPC_LOAD_U32(obj + 356), PPC_LOAD_U32(obj + 360),
                    PPC_LOAD_U32(obj + 48), PPC_LOAD_U32(obj + 364), PPC_LOAD_U32(obj + 368), PPC_LOAD_U32(obj + 372));
            for (int k = 0; k < 32; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(in + k * 4));
            fprintf(stderr, "\n");
        }
    }
    if (i < 60) {
        fprintf(stderr, "[exec-in] #%d in=%08X obj=%08X m356=%08X m360=%08X words:", i, in, obj,
                PPC_LOAD_U32(obj + 356), PPC_LOAD_U32(obj + 360));
        if (in >= 0x40000000u && in < 0xE0000000u)
            for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(in + k * 4));
        fprintf(stderr, "\n");
    }
    __imp__sub_826A38B0(ctx, base);
    if (dbg_trace_on()) fprintf(stderr, "[tr] #%u wk exec-done ret=%08X obj72=%08X tile=%u flags=%08X m356=%08X\n", dbg_trace_seq(),
                                ctx.r3.u32, PPC_LOAD_U32(obj + 72), PPC_LOAD_U32(obj + 52), PPC_LOAD_U32(obj + 48), PPC_LOAD_U32(obj + 356));
	{
		uint32_t out = ctx.r3.u32;
		static int badOut = 0;
		if ((out & 3) && badOut++ < 3) {
			fprintf(stderr, "[exec-misout] in=%08X out=%08X first=%08X\n", x_in, out, PPC_LOAD_U32(x_in));
			uint32_t a = (out & ~3u) - 32;
			if (a >= 0xC0000000u && a < 0xDFFFFF00u) {
				fprintf(stderr, "[exec-misout] words @%08X:", a);
				for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(a + k * 4));
				fprintf(stderr, "\n");
			}
			pktcb_dump_ring(base);
		}
	}
    if (i < 12) {
        uint32_t out = ctx.r3.u32;
        auto ok = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
        fprintf(stderr, "[exec] #%d in=%08X first=%08X obj=%08X -> out=%08X word=%08X out-in=%X\n",
                i, in, ok(in) ? PPC_LOAD_U32(in) : 0, obj,
                out, ok(out) ? PPC_LOAD_U32(out) : 0, out - in);
    }
}

extern "C" void __imp__sub_826A2E78(PPCContext& __restrict__ ctx, uint8_t* base);
// r3 = tile descriptor (+4 count, +8 array of 16-byte rects)
// r4 = chain of records: { u32 end; { u32 patchPtr; u16 rect[4]; u32 pad; } entries[]; u32 next @ end; }
// next == 0xC0000000 terminates. The walker follows `next` with an internal goto, so
// this hook only ever sees the head: the whole chain has to be checked here, up front.
static bool walk_phys_ok(uint32_t p) { return p >= 0xA0000000u && p < 0xE0000000u; }
static bool walk_log(int& n) { n++; return n <= 20 || (n % 256) == 0; }   // first 20, then sampled
PPC_FUNC(sub_826A2E78) {
    static int nbadw = 0;
    if (ctx.r3.u32 < 0x10000 || ctx.r3.u32 >= 0xE0000000u) {
        if (walk_log(nbadw))
            fprintf(stderr, "[walk] bad tile descriptor r3=%08X r4=%08X lr=%08X, skipped\n", ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
        return;
    }
    if (!walk_phys_ok(ctx.r4.u32) || (ctx.r4.u32 & 3)) {
        if (walk_log(nbadw))
            fprintf(stderr, "[walk] bad chain head r3=%08X r4=%08X r5=%08X lr=%08X, skipped\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
        return;
    }
    const char* why = nullptr;
    uint32_t rec = ctx.r4.u32, prevEnd = 0, cutLink = 0, badNext = 0;
    int hops = 0;
    for (;;) {
        uint32_t end = PPC_LOAD_U32(rec);
        if (end < rec + 4 || end - rec > 0x400000 || ((end - rec - 4) & 15)) { why = "end"; cutLink = prevEnd; break; }
        bool entriesOk = true;
        for (uint32_t e = rec + 4; e < end; e += 16)
            if (!walk_phys_ok(PPC_LOAD_U32(e))) { entriesOk = false; break; }
        if (!entriesOk) { why = "entry"; cutLink = prevEnd; break; }
        uint32_t next = PPC_LOAD_U32(end);
        if (next == 0xC0000000u) break;
        if (!walk_phys_ok(next) || (next & 3)) { why = "next"; cutLink = end; badNext = next; break; }
        if (++hops > 4096) { why = "cycle"; cutLink = end; badNext = next; break; }
        prevEnd = end;
        rec = next;
    }
    if (why) {
        if (walk_log(nbadw)) {
            fprintf(stderr, "[walk] bad link why=%s head=%08X hop=%d rec=%08X prevEnd=%08X next=%08X r5=%08X lr=%08X pl=%08X reps=%d %s\n",
                    why, ctx.r4.u32, hops, rec, prevEnd, badNext, ctx.r5.u32, (uint32_t)ctx.lr, g_wkPl.load(), g_wkReps.load(), cutLink ? "truncated" : "skipped");
            fprintf(stderr, "[walk] bad   rec words:");
            for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(rec + k * 4));
            fprintf(stderr, "\n");
        }
        if (!cutLink) return;   // the head record itself is bad, nothing to walk
    }
    uint32_t savedLink = cutLink ? PPC_LOAD_U32(cutLink) : 0;
    if (cutLink) PPC_STORE_U32(cutLink, 0xC0000000u);
    static int n = 0;
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, caller = ctx.lr;
    uint32_t endp = PPC_LOAD_U32(a4);
    bool bad = (a4 < 0x10000) || (endp < a4) || (endp - a4 > 0x400000);
    if (n < 6 || bad) {
        if (n < 40) {
            fprintf(stderr, "[walk] #%d caller=%08X r3=%08X r4=%08X r5=%08X end=%08X cnt=%u %s\n",
                    n, caller, a3, a4, a5, endp, PPC_LOAD_U32(a3 + 4), bad ? "BAD" : "");
            fprintf(stderr, "[walk]   r3 words:");
            for (int k = 0; k < 12; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(a3 + k * 4));
            fprintf(stderr, "\n[walk]   r4 words:");
            for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(a4 + k * 4));
            fprintf(stderr, "\n");
        }
    }
    n++;
    __imp__sub_826A2E78(ctx, base);
    {   // log only: show the words each entry's patch landed in (still terminated by cutLink here)
        static std::atomic<int> np{0};
        if (np.fetch_add(1) < 12) {
            uint32_t r = a4;
            for (int h = 0; h < 8; h++) {
                uint32_t e2 = PPC_LOAD_U32(r);
                for (uint32_t e = r + 4; e < e2; e += 16) {
                    uint32_t t = PPC_LOAD_U32(e);
                    fprintf(stderr, "[patch] head=%08X r5=%08X entry=%08X rect=%08X %08X words(-2..+5):", a4, a5, t, PPC_LOAD_U32(e + 4), PPC_LOAD_U32(e + 8));
                    for (int k = -2; k < 6; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(t + k * 4));
                    fprintf(stderr, "\n");
                }
                uint32_t nx = PPC_LOAD_U32(e2);
                if (nx == 0xC0000000u) break;
                r = nx;
            }
        }
    }
    if (cutLink) PPC_STORE_U32(cutLink, savedLink);
}

// r3 = tile descriptor, r4 = record chain walked first (see sub_826A2E78)
// r5 = table chain: { u32 end; u32 heads[]; u32 next @ end; }, next == 0xC0000000 terminates;
// each nonzero head is walked with r5=1. The first table record is processed unconditionally,
// so if it is bad the table can't be truncated: do the r4 walk only, as the function would.
extern "C" void __imp__sub_826A2F80(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A2F80) {
    static int nbadt = 0;
    const char* why = nullptr;
    uint32_t head = ctx.r5.u32, rec = head, prevEnd = 0, cutLink = 0, badNext = 0;
    int hops = 0;
    if (!walk_phys_ok(head) || (head & 3)) {
        why = "tablehead";
    } else for (;;) {
        uint32_t end = PPC_LOAD_U32(rec);
        if (end < rec + 4 || end - rec > 0x400000 || ((end - rec - 4) & 3)) { why = "end"; cutLink = prevEnd; break; }
        bool headsOk = true;
        for (uint32_t e = rec + 4; e < end; e += 4) {
            uint32_t h = PPC_LOAD_U32(e);
            if (h && (!walk_phys_ok(h) || (h & 3))) { headsOk = false; break; }
        }
        if (!headsOk) { why = "entry"; cutLink = prevEnd; break; }
        uint32_t next = PPC_LOAD_U32(end);
        if (next == 0xC0000000u) break;
        if (!walk_phys_ok(next) || (next & 3)) { why = "next"; cutLink = end; badNext = next; break; }
        if (++hops > 4096) { why = "cycle"; cutLink = end; badNext = next; break; }
        prevEnd = end;
        rec = next;
    }
    if (why) {
        if (walk_log(nbadt)) {
            fprintf(stderr, "[walk] bad table #%d why=%s r4=%08X head=%08X hop=%d rec=%08X prevEnd=%08X next=%08X lr=%08X pl=%08X reps=%d %s\n",
                    nbadt, why, ctx.r4.u32, head, hops, rec, prevEnd, badNext, (uint32_t)ctx.lr, g_wkPl.load(), g_wkReps.load(), cutLink ? "truncated" : "r4 walk only");
            if (walk_phys_ok(rec)) {
                fprintf(stderr, "[walk] bad   table words:");
                for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", PPC_LOAD_U32(rec + k * 4));
                fprintf(stderr, "\n");
            }
        }
        if (!cutLink) {
            uint32_t lr = ctx.lr;
            ctx.r5.u64 = 0;
            ctx.lr = 0x826A2F9C;
            sub_826A2E78(ctx, base);
            ctx.lr = lr;
            return;
        }
    }
    uint32_t savedLink = cutLink ? PPC_LOAD_U32(cutLink) : 0;
    if (cutLink) PPC_STORE_U32(cutLink, 0xC0000000u);
    __imp__sub_826A2F80(ctx, base);
    if (cutLink) PPC_STORE_U32(cutLink, savedLink);
}

extern "C" void __imp__sub_8269CEE8(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8269CEE8) {
    static int n = 0, blocked = 0;
    uint32_t dev = ctx.r3.u32, fp = PPC_LOAD_U32(dev + 10896);
    uint32_t cur = PPC_LOAD_U32(fp + 4), x = ctx.r4.u32, y = ctx.r5.u32;
    uint32_t d = (y - cur) & 3u;
    bool willBlock = (d == 2 || d == 3 || (d == 1 && x > (cur & ~3u)));
    if (n < 6 || (willBlock && blocked < 40)) {
        if (willBlock) blocked++;
        fprintf(stderr, "[wait2] n=%d %s r4=%08X r5=%08X fence=%X ptr4=%08X sub=%X lr=%08X\n",
                n, willBlock ? "BLOCK" : "pass", x, y, PPC_LOAD_U32(fp), cur,
                PPC_LOAD_U32(dev + 10908), (uint32_t)ctx.lr);
        if (willBlock && blocked <= 3) {
            uint32_t sp = ctx.r1.u32;
            for (int k = 0; k < 8; k++) {
                uint32_t prev = PPC_LOAD_U32(sp);
                if (prev <= sp || prev >= 0x70080000) break;
                fprintf(stderr, "[wait2]   bt%d lr=%08X\n", k, PPC_LOAD_U32(prev - 8));
                sp = prev;
            }
        }
    }
    n++;
    __imp__sub_8269CEE8(ctx, base);
}

extern "C" void __imp__sub_826A2FE8(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_826A2FE8) {
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    uint32_t obj = ctx.r3.u32, q = ctx.r4.u32, sp = ctx.r5.u32;
    auto ok = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
    uint32_t w0 = ok(sp) ? PPC_LOAD_U32(sp) : 0;
    uint32_t s72 = PPC_LOAD_U32(obj + 72), s36 = PPC_LOAD_U32(obj + 36), s60 = PPC_LOAD_U32(obj + 60);
    __imp__sub_826A2FE8(ctx, base);
    if (i < 30) {
        uint32_t out = ctx.r3.u32;
        fprintf(stderr, "[pop] #%d obj=%08X sp=%08X word=%08X +72=%08X +36=%08X +60=%u -> out=%08X word=%08X\n",
                i, obj, sp, w0, s72, s36, s60, out, ok(out) ? PPC_LOAD_U32(out) : 0);
    }
}

extern "C" void __imp__sub_8251B6B0(PPCContext&, uint8_t*);
PPC_FUNC(sub_8251B6B0) {
    fprintf(stderr, "[trace] sub_8251B6B0 thread=%lu lr=%08X r3=%08X r4=%08X r5=%08X\n",
            GetCurrentThreadId(), (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    for (uint32_t off = 0; off < 0x80; off += 16) {
        const uint32_t* w = (const uint32_t*)(base + ctx.r3.u32 + off);
        fprintf(stderr, "[obj +%02X] %08X %08X %08X %08X\n", off,
                _byteswap_ulong(w[0]), _byteswap_ulong(w[1]), _byteswap_ulong(w[2]), _byteswap_ulong(w[3]));
    }
    __imp__sub_8251B6B0(ctx, base);
}
extern "C" void __imp__sub_8251B6A8(PPCContext&, uint8_t*);
PPC_FUNC(sub_8251B6A8) {
    fprintf(stderr, "[trace] sub_8251B6A8 lr=%08X r3=%08X\n", (uint32_t)ctx.lr, ctx.r3.u32);
    __imp__sub_8251B6A8(ctx, base);
}

static void trace_cctor(PPCContext& ctx, uint8_t* base, const char* tag) {
    uint32_t src = ctx.r4.u32;
    fprintf(stderr, "[cctor] %s thread=%lu lr=%08X this=%08X src=%08X srcflag=%02X\n",
            tag, GetCurrentThreadId(), (uint32_t)ctx.lr, ctx.r3.u32, src, PPC_LOAD_U8(src + 76));
    for (int i = 0; i < 0x50; i += 16) {
        fprintf(stderr, "[cctor]   +%02X %08X %08X %08X %08X\n", i,
                PPC_LOAD_U32(src+i), PPC_LOAD_U32(src+i+4), PPC_LOAD_U32(src+i+8), PPC_LOAD_U32(src+i+12));
    }
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 8; i++) {
        uint32_t prev = PPC_LOAD_U32(sp);
        if (prev <= sp || prev >= 0x70080000) break;
        fprintf(stderr, "[cctor]   bt%d lr=%08X\n", i, PPC_LOAD_U32(prev - 8));
        sp = prev;
    }
}

extern "C" void __imp__sub_8253F530(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8253F530) { trace_cctor(ctx, base, "F530"); __imp__sub_8253F530(ctx, base); }

extern "C" void __imp__sub_82531230(PPCContext& ctx, uint8_t* base);
static void dump_str(uint8_t* base, uint32_t p) {
    if (!p) { fprintf(stderr, "[err]   msg=null\n"); return; }
    char buf[200]; int n = 0;
    for (int i = 0; i < 400 && n < 190; i++) {
        uint8_t c = PPC_LOAD_U8(p + i);
        if (c == 0 && i > 0 && PPC_LOAD_U8(p + i - 1) == 0) break;   // end (also handles UTF-16)
        if (c == 0) continue;
        buf[n++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    buf[n] = 0;
    fprintf(stderr, "[err]   msg=\"%s\"\n", buf);
}
PPC_FUNC(sub_82531230) {
    fprintf(stderr, "[err] sub_82531230 thread=%lu code=%u flag=%u lr=%08X this=%08X msgptr=%08X\n",
            GetCurrentThreadId(), ctx.r4.u32, ctx.r6.u32 & 0xFF, (uint32_t)ctx.lr, ctx.r3.u32, ctx.r5.u32);
    dump_str(base, ctx.r5.u32);
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 6; i++) {
        uint32_t prev = PPC_LOAD_U32(sp);
        if (prev <= sp || prev >= 0x70080000) break;
        fprintf(stderr, "[err]   bt%d lr=%08X\n", i, PPC_LOAD_U32(prev - 8));
        sp = prev;
    }
    __imp__sub_82531230(ctx, base);
}

extern "C" void __imp__sub_826C6488(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_826C6488) {
    uint32_t lr = (uint32_t)ctx.lr;
    __imp__sub_826C6488(ctx, base);
    fprintf(stderr, "[err] GetLastError -> %u (caller lr=%08X)\n", ctx.r3.u32, lr);
}

extern "C" void __imp__sub_82252508(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82252508) {
    static std::atomic<int> n{0}, nslow{0};
    uint32_t callerLr = (uint32_t)ctx.lr;
    uint32_t bt[4] = {0, 0, 0, 0};
    { uint32_t sp = ctx.r1.u32;
      for (int k = 0; k < 4; k++) {
          if (sp < 0x10000 || sp >= 0xE0000000u) break;
          sp = PPC_LOAD_U32(sp);
          if (sp < 0x10000 || sp >= 0xE0000000u) break;
          bt[k] = PPC_LOAD_U32(sp - 8);
      } }
    uint32_t obj = ctx.r3.u32, flag = ctx.r5.u32 & 0xFF;
    uint32_t p = PPC_LOAD_U32(obj + 152);
    int i = n.fetch_add(1);
    if (i < 12)
        fprintf(stderr, "[spin] enter #%d tid=%lu obj=%08X flag=%u *152=%08X[%08X] lr=%08X bt=%08X %08X %08X %08X\n",
                i, GetCurrentThreadId(), obj, flag, p, p > 0x10000 ? PPC_LOAD_U32(p) : 0u,
                callerLr, bt[0], bt[1], bt[2], bt[3]);
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_82252508(ctx, base);
    long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 200 && nslow.fetch_add(1) < 40)
        fprintf(stderr, "[spin] SLOW %ld ms tid=%lu obj=%08X lr=%08X bt=%08X %08X %08X %08X *152=%08X[%08X]\n",
                ms, GetCurrentThreadId(), obj, callerLr, bt[0], bt[1], bt[2], bt[3], p, p > 0x10000 ? PPC_LOAD_U32(p) : 0u);
}