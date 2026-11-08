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
#include "settings.h"
#include "upgradecycle.h"
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
// The state that last made a Lua call on this thread, so a thrown LuaError can report the script's message and stack.
// lua_State: +8 top, +12 base, +20 ci, +24 savedpc, +32 stack, +40 base_ci. CallInfo (24 bytes): +0 base, +4 func, +12 savedpc.
// Proto: +12 code, +20 lineinfo, +32 source, +48 sizelineinfo. Value tags: 0 nil, 1 bool, 3 number, 4 string, 5 table, 6 function.
static thread_local uint32_t t_lastLuaState = 0;
void lua_dump_error(uint8_t* base, uint32_t state) {
    uint32_t L = state ? state : t_lastLuaState;
    if (!L || IsBadReadPtr(base + L, 64)) { fprintf(stderr, "[lua-err] no lua state\n"); return; }
    uint32_t top = PPC_LOAD_U32(L + 8), stack = PPC_LOAD_U32(L + 32), ci = PPC_LOAD_U32(L + 20), baseCi = PPC_LOAD_U32(L + 40);
    fprintf(stderr, "[lua-err] L=%08X top=%08X stack=%08X\n", L, top, stack);
    for (int i = 1; i <= 8 && top - i * 8 >= stack; i++) {
        uint32_t v = top - i * 8, tag = PPC_LOAD_U32(v + 4), val = PPC_LOAD_U32(v);
        fprintf(stderr, "[lua-err]  stack[-%d] tag=%u val=%08X", i, tag, val);
        if (tag == 4 && !IsBadReadPtr(base + val, 16)) fprintf(stderr, " \"%.400s\"", (const char*)(base + val + 16));
        fputc('\n', stderr);
    }
    uint32_t pc = PPC_LOAD_U32(L + 24);
    for (int depth = 0; depth < 30 && ci >= baseCi && ci; depth++, ci -= 24) {
        uint32_t func = PPC_LOAD_U32(ci + 4);
        if (depth) pc = PPC_LOAD_U32(ci + 12);
        if (PPC_LOAD_U32(func + 4) != 6) { fprintf(stderr, "[lua-err]  #%d not a function\n", depth); continue; }
        uint32_t cl = PPC_LOAD_U32(func);
        if (PPC_LOAD_U8(cl + 6) != 0) { fprintf(stderr, "[lua-err]  #%d [C] %08X\n", depth, PPC_LOAD_U32(cl + 16)); continue; }
        uint32_t p = PPC_LOAD_U32(cl + 16), ts = PPC_LOAD_U32(p + 32), code = PPC_LOAD_U32(p + 12), li = PPC_LOAD_U32(p + 20);
        int idx = (int)(pc - code) / 4 - 1, line = -1;
        if (li && idx >= 0 && idx < (int)PPC_LOAD_U32(p + 48)) line = (int)PPC_LOAD_U32(li + idx * 4);
        fprintf(stderr, "[lua-err]  #%d %s:%d\n", depth, ts ? (const char*)(base + ts + 16) : "?", line);
    }
}
// ---- Lua protected calls ----
// Recompiled guest setjmp/longjmp cannot move the host's call stack: longjmp just returned into luaD_throw, which then
// ran its panic path, so any Lua error ended the game even inside pcall. These replace the only setjmp caller and the
// Lua longjmp caller with a host exception, keeping the guest layout: lua_longjmp at sp+80, status at lj+1360, L+92 errorJmp.
struct GuestLuaThrow { uint32_t lj; };
// sub_8262B5D8 is luaD_rawrunprotected(L, f, ud)
PPC_FUNC(sub_8262B5D8) {
    const uint32_t L = ctx.r3.u32, f = ctx.r4.u32, ud = ctx.r5.u32, oldSp = ctx.r1.u32;
    const uint32_t sp = oldSp - 1472, lj = sp + 80;
    PPC_STORE_U32(oldSp - 8, (uint32_t)ctx.lr);
    PPC_STORE_U32(sp, oldSp);
    PPC_STORE_U32(lj + 1360, 0);
    PPC_STORE_U32(lj, PPC_LOAD_U32(L + 92));
    PPC_STORE_U32(L + 92, lj);
    const PPCContext saved = ctx;
    try {
        ctx.r1.u64 = sp;
        ctx.r3.u64 = L;
        ctx.r4.u64 = ud;
        ctx.lr = 0x8262B638;
        PPC_CALL_INDIRECT_FUNC(f);
    } catch (const GuestLuaThrow& t) {
        if (t.lj != lj) throw;
        auto fpscr = ctx.fpscr;    // keeps the cached copy of the live MXCSR correct
        ctx = saved;
        ctx.fpscr = fpscr;
    }
    PPC_STORE_U32(L + 92, PPC_LOAD_U32(lj));
    ctx.r1.u64 = oldSp;
    ctx.r3.u64 = PPC_LOAD_U32(lj + 1360);
}
// sub_82638FA0 is luaD_throw(L, status). With no protected call active the original runs the game's panic handler.
extern "C" void __imp__sub_82638FA0(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82638FA0) {
    const uint32_t L = ctx.r3.u32, status = ctx.r4.u32, lj = PPC_LOAD_U32(L + 92);
    static std::atomic<uint32_t> nThrows{0};
    uint32_t n = ++nThrows;
    if (n <= 16 || (n % 1000) == 0) {
        fprintf(stderr, "[lua-throw] #%u status=%u protected=%d\n", n, status, lj != 0);
        lua_dump_error(base, L);
    }
    if (!lj) { __imp__sub_82638FA0(ctx, base); return; }
    PPC_STORE_U32(lj + 1360, status);
    throw GuestLuaThrow{ lj };
}

// ---- running Lua in the game's own state, for testing ----
// BCR2_GODMODE sets the developers' own switches: the _god_mode flag that makes Damage:add_damage ignore damage on the player, and Global.unlimited_lives.
// Both are toggles in the game's debug menu. BCR2_LUARUN=<lua> runs a chunk as well, and logs a string it returns.
uint64_t gpu_frame_number();
void sub_8264FF78(PPCContext& __restrict__ ctx, uint8_t* base);              // luaL_loadstring(L, s)
void sub_8263C8A8(PPCContext& __restrict__ ctx, uint8_t* base);              // lua_pcall(L, nargs, nresults, errfunc)
static void lua_run_chunk(PPCContext& ctx, uint8_t* base, uint32_t L, const char* text, const char* tag, std::string* result = nullptr) {
    const size_t n = strlen(text) + 1;
    const uint32_t oldSp = ctx.r1.u32, buf = (oldSp - 0x400 - (uint32_t)n) & ~15u, sp = (buf - 0x200) & ~15u;
    memcpy(base + buf, text, n);
    const uint32_t topOff = PPC_LOAD_U32(L + 8) - PPC_LOAD_U32(L + 32);      // L->top against L->stack, which can move
    ctx.r1.u64 = sp;
    ctx.r3.u64 = L; ctx.r4.u64 = buf;
    sub_8264FF78(ctx, base);
    uint32_t status = ctx.r3.u32;
    if (status == 0) {
        ctx.r1.u64 = sp;
        ctx.r3.u64 = L; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 0;
        sub_8263C8A8(ctx, base);
        status = ctx.r3.u32;
    }
    // One value is on top now: the chunk's result, or an error message. Lua strings have tag 4 and their characters at +16.
    const uint32_t top = PPC_LOAD_U32(L + 8);
    if (result && status == 0 && top >= 8 && PPC_LOAD_U32(top - 8 + 4) == 4) *result = (const char*)(base + PPC_LOAD_U32(top - 8) + 16);
    if (top >= 8 && PPC_LOAD_U32(top - 8 + 4) == 4 && (status != 0 || tag)) {
        static int nl = 0;
        if (nl++ < 200000) fprintf(stderr, "[lua-run] %s f=%llu status=%u: %s\n", tag ? tag : "god mode", (unsigned long long)gpu_frame_number(), status, (const char*)(base + PPC_LOAD_U32(top - 8) + 16));
    }
    PPC_STORE_U32(L + 8, PPC_LOAD_U32(L + 32) + topOff);
    ctx.r1.u64 = oldSp;
}

extern "C" void __imp__sub_82634458(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82634458) {
    t_lastLuaState = ctx.r3.u32;
    static const bool kTrace = getenv("BCR2_LUATRACE") != nullptr;
    if (kTrace) lua_trace_call(base, ctx.r4.u32);
    static const bool kGod = getenv("BCR2_GODMODE") != nullptr;
    static const char* const kRun = getenv("BCR2_LUARUN");
    static const bool kGunFix = getenv("BCR2_NOGUNFIX") == nullptr;
    static const bool kDfaLimit = getenv("BCR2_NODFALIMIT") == nullptr;
    static const bool kNoMusic = getenv("BCR2_NOMUSIC") != nullptr;
    static const bool kSoundLog = getenv("BCR2_SOUNDLOG") != nullptr;
    static thread_local bool inside = false;
    static const bool kPcMenu = getenv("BCR2_NOPCMENU") == nullptr;
    if (!inside && upgrades_pending()) {   // LT or a right stick click was pressed: cycle the equipped arm upgrade now
        inside = true;
        const PPCContext saved = ctx;
        const uint32_t L = saved.r3.u32, funcOff = saved.r4.u32 - PPC_LOAD_U32(L + 32);
        std::string r;
        lua_run_chunk(ctx, base, L, upgrades_cycle_lua().c_str(), nullptr, &r);
        if (!r.empty()) fprintf(stderr, "[upgrades] %s at frame %llu\n", r.c_str(), (unsigned long long)gpu_frame_number());
        auto fpscr = ctx.fpscr;
        ctx = saved;
        ctx.fpscr = fpscr;
        ctx.r4.u64 = PPC_LOAD_U32(L + 32) + funcOff;
        inside = false;
    }
    {   // The PC Settings menu: installs it into the game's menu data, then collects what the player changed there a few times a second.
        static std::atomic<uint64_t> nextPc{ 200 };
        static bool installed = false;
        uint64_t due = nextPc.load(), f = gpu_frame_number();
        if (kPcMenu && !inside && f >= due && nextPc.compare_exchange_strong(due, f + (installed ? 4 : 30))) {
            inside = true;
            const PPCContext saved = ctx;
            const uint32_t L = saved.r3.u32, funcOff = saved.r4.u32 - PPC_LOAD_U32(L + 32);
            std::string r;
            if (!installed) {
                lua_run_chunk(ctx, base, L, settings_install_lua().c_str(), nullptr, &r);
                static std::string lastR;
                if (r != lastR) { lastR = r; fprintf(stderr, "[settings] menu install at frame %llu: %s\n", (unsigned long long)f, r.empty() ? "(nothing returned)" : r.c_str()); }
                if (r == "installed") { installed = true; settings_splash_menu_ready(); }
            } else {
                lua_run_chunk(ctx, base, L, settings_poll_lua().c_str(), nullptr, &r);
                if (r == "reinstall") installed = false;
                else if (!r.empty()) settings_apply_from_lua(r);
            }
            auto fpscr = ctx.fpscr;
            ctx = saved;
            ctx.fpscr = fpscr;
            ctx.r4.u64 = PPC_LOAD_U32(L + 32) + funcOff;
            inside = false;
        }
    }
    if (kGod || kRun || kGunFix || kNoMusic || kSoundLog || kDfaLimit) {
        // About once a second, while this thread is already inside Lua. New players and levels are picked up the next time around.
        static const uint64_t kEvery = getenv("BCR2_LUAEVERY") ? _atoi64(getenv("BCR2_LUAEVERY")) : 60;   // frames between runs, for tests that need finer timing
        static std::atomic<uint64_t> nextFrame{ 300 };
        uint64_t due = nextFrame.load(), f = gpu_frame_number();
        if (!inside && f >= due && nextFrame.compare_exchange_strong(due, f + kEvery)) {
            inside = true;
            const PPCContext saved = ctx;
            const uint32_t L = saved.r3.u32, funcOff = saved.r4.u32 - PPC_LOAD_U32(L + 32);
            // Player:die with _god_mode set disables the controller and moves the player back, then the death area ragdolls the player anyway: a softlock.
            // So the flag steps aside for that one call, and a fall is an ordinary death and respawn. Lives are topped up so the counter can't go below zero.
            static const char* const kGodLua =
                "if Global then Global.unlimited_lives = true end "
                "if Player and not Player.bcr_god_die then "
                  "local die = Player.die Player.bcr_god_die = true "
                  "Player.die = function(self, killer, is_water) local d = self._unit:bcr_damage() local g = d._god_mode d._god_mode = false die(self, killer, is_water) d._god_mode = g end "
                "end "
                "if managers and managers.game and managers.game._game_data and managers.game._game_data.player_lives then "
                  "local l = managers.game._game_data.player_lives for k, v in pairs(l) do if v < 3 then l[k] = 3 end end "
                "end "
                "if managers and managers.player then for _, p in pairs(managers.player:get_players()) do p:bcr_damage()._god_mode = true end end";
            // Mounted machine guns: _next_shot_time starts at 0 and only advances per shot, so here the gun fired one shot per frame (60 a second) until it
            // caught up with game time, and again after every pause. Xenia and the console fire steadily at 20 a second, and the cause of that difference
            // is not known yet. This starts the gun's clock when firing starts. BCR2_NOGUNFIX turns it off.
            static const char* const kGunLua =
                "if MachinegunTurret and not MachinegunTurret.bcr_fix then MachinegunTurret.bcr_fix = true local fire = MachinegunTurret.fire "
                "MachinegunTurret.fire = function(self, t) if not self._firing and self._next_shot_time < t then self._next_shot_time = t end return fire(self, t) end end";
            // BCR2_NOMUSIC holds the game's music volume at 0. It sets the sound engine's parameter directly, so the saved settings don't change.
            static const char* const kNoMusicLua = "if SoundDevice then SoundDevice:set_rtpc('global_music_volume', 0) end";
            // BCR2_SOUNDLOG logs every sound event posted from Lua with game time and the posting script line, plus the player's state changes. Each run hands
            // back what has collected since the last one, so BCR2_LUAEVERY sets how finely the log's frame numbers follow the events.
            static const char* const kSoundLogLua =
                "local src = Global and Global.music_manager and Global.music_manager.source "
                "local mt = src and getmetatable(src) local idx = mt and mt.__index "
                "if type(idx) ~= 'table' then return nil end "
                "if not rawget(idx, 'bcr_log') then "
                  "local buf = {} local post = idx.post_event "
                  "local now = function() return Application and string.format('%.2f', Application:time()) or '?' end "
                  "local where = function() local d = rawget(_G, 'debug') local g = d and d.getinfo if not g then return '?' end local i = g(3, 'Sl') return i and (tostring(i.short_src) .. ':' .. tostring(i.currentline)) or '?' end "
                  "idx.bcr_log = function(m) buf[#buf + 1] = m end "
                  "idx.bcr_time = now "
                  "idx.bcr_flush = function() if #buf == 0 then return nil end local s = table.concat(buf, ' | ') buf = {} return s end "
                  "idx.post_event = function(self, e, ...) buf[#buf + 1] = now() .. ' ' .. tostring(e) .. ' @' .. where() return post(self, e, ...) end "
                "end "
                "if Player and not Player.bcr_snd then Player.bcr_snd = true local cs = Player.change_state local log, now = idx.bcr_log, idx.bcr_time "
                  "Player.change_state = function(self, name, ...) log(now() .. ' STATE ' .. tostring(name)) return cs(self, name, ...) end end "
                "return idx.bcr_flush()";
            // Death From Above's in-air sound is a loop that only an end event stops, and the console can get stuck playing it forever. The player posts the end when
            // leaving the state, only if it kept a handle to the loop, and one path posts the loop without keeping one. So this watches the events themselves and
            // moves to the end event once a loop has run 5 seconds. The impact event stops the loop too, so it also ends the wait. BCR2_NODFALIMIT turns it off.
            static const char* const kDfaLimitLua =
                "local src = Global and Global.music_manager and Global.music_manager.source "
                "local mt = src and getmetatable(src) local idx = mt and mt.__index "
                "if type(idx) ~= 'table' or not Application then return nil end "
                "if not rawget(idx, 'bcr_dfa') then local post = idx.post_event local d = {} idx.bcr_dfa = d "
                  "idx.post_event = function(self, e, ...) "
                    "if e == 'spencer_dfa_in_air_loop' then d.t = Application:time() elseif e == 'spencer_dfa_in_air_end' or e == 'spencer_dfa_hit' then d.t = nil end "
                    "return post(self, e, ...) end "
                "end "
                "local d = idx.bcr_dfa "
                "if d.t and Application:time() - d.t >= 5 and managers and managers.player then d.t = nil "
                  "for _, p in pairs(managers.player:get_players()) do p:sound_source():post_event('spencer_dfa_in_air_end') "
                    "local b = p:base() if b and b._upd then b._upd.dfa_in_air_loop_sound_event_instance = nil end end "
                  "return 'ended a loop after 5 s' "
                "end return nil";
            if (kDfaLimit) lua_run_chunk(ctx, base, L, kDfaLimitLua, "dfa limit");
            if (kNoMusic) lua_run_chunk(ctx, base, L, kNoMusicLua, nullptr);
            if (kSoundLog) lua_run_chunk(ctx, base, L, kSoundLogLua, "sound");
            if (kGunFix) lua_run_chunk(ctx, base, L, kGunLua, nullptr);
            if (kGod) lua_run_chunk(ctx, base, L, kGodLua, nullptr);
            if (kRun) lua_run_chunk(ctx, base, L, kRun, "test");
            auto fpscr = ctx.fpscr;                                          // keeps the cached copy of the live MXCSR correct
            ctx = saved;
            ctx.fpscr = fpscr;
            ctx.r4.u64 = PPC_LOAD_U32(L + 32) + funcOff;                     // the Lua stack may have moved
            inside = false;
        }
    }
    __imp__sub_82634458(ctx, base);
}

// The game's memset. The recompiler ended it at its first conditional return (bdzlr at 827E9BBC) and made the rest a separate function that nothing calls, so
// sizes leaving 2 or 3 bytes after the last whole word left those bytes unwritten. The sound engine's avoid-repeat bitmap is 2 bytes: its second byte kept garbage,
// which marked entries as already played, so the picker ran off the end of its list and half of the voice lines never started. This is the whole function.
PPC_FUNC(sub_827E9B30) {
    memset(base + ctx.r3.u32, ctx.r4.u8, ctx.r5.u32);
}
// BCR2_RANSEQLOG: the sound engine's random and sequence containers: what each one picked. sub_82792340 picks an index, sub_82792918 turns it into a child object.
// Objects carry their id at +12. sub_82767E28 finds an object by id, which shows whether the id it was handed exists.
extern "C" void __imp__sub_82792918(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82792918) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    if (!kLog) { __imp__sub_82792918(ctx, base); return; }
    const uint32_t self = ctx.r3.u32, idxOut = ctx.r5.u32, idOut = ctx.r6.u32, sp = ctx.r1.u32;
    __imp__sub_82792918(ctx, base);
    const uint32_t node = ctx.r3.u32;
    fprintf(stderr, "[ranseq] f=%llu container=%08X mode=%02X%02X avoid=%u -> index=%u id=%08X node=%08X nodeid=%08X sp=%08X\n", (unsigned long long)gpu_frame_number(), PPC_LOAD_U32(self + 12), base[self + 278], base[self + 279], (unsigned)PPC_LOAD_U16(self + 276), (unsigned)PPC_LOAD_U16(idxOut), PPC_LOAD_U32(idOut), node, node ? PPC_LOAD_U32(node + 12) : 0, sp);
}
extern "C" void __imp__sub_82792050(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82792050) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    if (!kLog) { __imp__sub_82792050(ctx, base); return; }
    const uint32_t self = ctx.r3.u32, before = PPC_LOAD_U32(self + 260);
    __imp__sub_82792050(ctx, base);
    const uint32_t st = ctx.r3.u32;
    fprintf(stderr, "[ranseq] state of %08X: was %08X now %08X returned %08X", PPC_LOAD_U32(self + 12), before, PPC_LOAD_U32(self + 260), st);
    if (st) fprintf(stderr, " fields total=%u remaining=%u played=%u", PPC_LOAD_U32(st + 16), (unsigned)PPC_LOAD_U16(st + 20), (unsigned)PPC_LOAD_U16(st + 22));
    fprintf(stderr, "\n");
}
extern "C" void __imp__sub_82792340(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82792340) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    if (!kLog) { __imp__sub_82792340(ctx, base); return; }
    const uint32_t self = ctx.r3.u32, st = ctx.r4.u32, flag = ctx.r5.u32;
    __imp__sub_82792340(ctx, base);
    fprintf(stderr, "[ranseq] pick in %08X: index=%u ok=%u remaining=%u played=%u\n", PPC_LOAD_U32(self + 12), ctx.r3.u32, (unsigned)base[flag], (unsigned)PPC_LOAD_U16(st + 20), (unsigned)PPC_LOAD_U16(st + 22));
}
extern "C" void __imp__sub_82791800(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82791800) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    if (!kLog) { __imp__sub_82791800(ctx, base); return; }
    const uint32_t self = ctx.r3.u32, count = ctx.r4.u32;
    __imp__sub_82791800(ctx, base);
    fprintf(stderr, "[ranseq] create state for %08X count=%u -> %08X\n", PPC_LOAD_U32(self + 12), count, ctx.r3.u32);
}
extern "C" void __imp__sub_827915E0(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_827915E0) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    if (!kLog) { __imp__sub_827915E0(ctx, base); return; }
    const uint32_t self = ctx.r3.u32, st = ctx.r4.u32, idx = ctx.r5.u32, played = PPC_LOAD_U32(st + 36), avoid = PPC_LOAD_U32(st + 40);
    const uint8_t pb = base[played], ab = base[avoid];
    __imp__sub_827915E0(ctx, base);
    fprintf(stderr, "[ranseq]   %08X available(%u) = %08X (raw r3 %016llX) played@%08X=%02X avoid@%08X=%02X\n", PPC_LOAD_U32(self + 12), idx, ctx.r3.u32, (unsigned long long)ctx.r3.u64, played, pb, avoid, ab);
}
extern "C" void __imp__sub_827ED460(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_827ED460) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    const uint32_t lr = (uint32_t)ctx.lr;
    __imp__sub_827ED460(ctx, base);
    if (kLog && lr >= 0x82792340u && lr < 0x82792900u) fprintf(stderr, "[ranseq]   rand() = %u (raw %016llX) lr %08X\n", ctx.r3.u32, (unsigned long long)ctx.r3.u64, lr);
}
extern "C" void __imp__sub_82767E28(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82767E28) {
    static const bool kLog = getenv("BCR2_RANSEQLOG") != nullptr;
    if (!kLog) { __imp__sub_82767E28(ctx, base); return; }
    const uint32_t id = ctx.r4.u32, lr = (uint32_t)ctx.lr;
    __imp__sub_82767E28(ctx, base);
    if (lr >= 0x82790000u && lr < 0x82794000u) fprintf(stderr, "[ranseq] lookup id=%08X -> %08X (lr %08X)\n", id, ctx.r3.u32, lr);
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
    static const bool kVerbose = getenv("BCR2_VERBOSE") != nullptr;         // hundreds per second, each an unbuffered write
    if (kVerbose) fprintf(stderr, "[err] GetLastError -> %u (caller lr=%08X)\n", ctx.r3.u32, lr);
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
// BCR2_ROPELOG: the state of the bionic arm wire each time it is rebuilt and each time it is submitted to the renderer.
// The rope object holds its node list through +712, its index list at +892..+896; submit and rebuild run on the render side.
uint64_t gpu_frame_number();
void hw_watch_set(uint32_t phys);
static const bool g_ropeLog = getenv("BCR2_ROPELOG") != nullptr;
static inline uint32_t be32_at(uint8_t* base, uint32_t a) { return _byteswap_ulong(*(uint32_t*)(base + a)); }
extern "C" void __imp__sub_823706A0(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_823706A0) {                                                     // Rope: rebuild geometry from the node list
    if (!g_ropeLog) { __imp__sub_823706A0(ctx, base); return; }
    uint32_t self = ctx.r3.u32, def = be32_at(base, self + 712);
    uint32_t b = def ? be32_at(base, def + 8) : 0, e = def ? be32_at(base, def + 12) : 0;
    static const uint64_t kWatchAt = getenv("BCR2_ROPEWATCH") ? _atoi64(getenv("BCR2_ROPEWATCH")) : 0;
    static bool watched = false;
    if (kWatchAt && !watched && gpu_frame_number() >= kWatchAt) { watched = true; hw_watch_set(be32_at(base, self + 708) + 12); }   // who changes the node count, on the game logic's copy (+712 is the render copy)
    __imp__sub_823706A0(ctx, base);
    uint32_t ib = be32_at(base, self + 892), ie = be32_at(base, self + 896);
    fprintf(stderr, "[rope] build f=%llu tid=%lu this=%08X def=%08X nodes=%d indices=%d\n", (unsigned long long)gpu_frame_number(), GetCurrentThreadId(), self, def, (int)(e - b) / 12, (int)(ie - ib) / 2);
}
static inline long long rope_us() { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); return q.QuadPart * 1000000 / f.QuadPart; }
extern "C" void __imp__sub_82377938(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82377938) {                                                     // Rope: game logic rewrites the node list
    if (!g_ropeLog) { __imp__sub_82377938(ctx, base); return; }
    const long long a = rope_us();
    __imp__sub_82377938(ctx, base);
    fprintf(stderr, "[rope] logic f=%llu tid=%lu t=%lld..%lld\n", (unsigned long long)gpu_frame_number(), GetCurrentThreadId(), a, rope_us());
}
extern "C" void __imp__sub_8236F370(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_8236F370) {                                                     // Rope (at +84): frame sync callback, flag 8 copies the node list for rendering
    if (!g_ropeLog) { __imp__sub_8236F370(ctx, base); return; }
    uint32_t self = ctx.r3.u32 - 84, flags = ctx.r4.u32, src = be32_at(base, self + 708);
    long long a = rope_us();
    int n = src ? (int)(be32_at(base, src + 12) - be32_at(base, src + 8)) / 12 : -1;
    __imp__sub_8236F370(ctx, base);
    fprintf(stderr, "[rope] sync f=%llu tid=%lu t=%lld..%lld flags=%X srcnodes=%d\n", (unsigned long long)gpu_frame_number(), GetCurrentThreadId(), a, rope_us(), flags, n);
}
extern "C" void __imp__sub_82371260(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82371260) {                                                     // Rope (at +384): submit to the renderer
    if (!g_ropeLog) { __imp__sub_82371260(ctx, base); return; }
    uint32_t self = ctx.r3.u32 - 384;
    {   // once: the render queue this rope submits itself to
        static bool once = false;
        if (!once) { once = true; uint32_t q = ctx.r4.u32, vt = be32_at(base, q); fprintf(stderr, "[rope] queue=%08X vtable=%08X add=%08X atom=%08X\n", q, vt, be32_at(base, vt + 196), self + 384 + 684); }
    }
    uint32_t dirty = *(base + self + 780);
    uint32_t ib0 = be32_at(base, self + 892), ie0 = be32_at(base, self + 896);
    __imp__sub_82371260(ctx, base);
    uint32_t ib = be32_at(base, self + 892), ie = be32_at(base, self + 896);
    fprintf(stderr, "[rope] submit f=%llu tid=%lu this=%08X dirty=%u indices %d -> %d\n", (unsigned long long)gpu_frame_number(), GetCurrentThreadId(), self, dirty, (int)(ie0 - ib0) / 2, (int)(ie - ib) / 2);
}

// BCR2_BUCKETWATCH=frame: from that frame, a data breakpoint on the atom list of a render bucket, to name the code that sorts and draws it
extern "C" void __imp__sub_825585E0(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_825585E0) {
    static const uint64_t kAt = getenv("BCR2_BUCKETWATCH") ? _atoi64(getenv("BCR2_BUCKETWATCH")) : 0;
    static bool done = false;
    if (kAt && !done && gpu_frame_number() >= kAt) { done = true; fprintf(stderr, "[bucket] %08X\n", ctx.r3.u32); hw_watch_set(ctx.r3.u32 + 56); }
    __imp__sub_825585E0(ctx, base);
}

// BCR2_ATOMLOG=first,last game frames: each render bucket's order before and after its sort, and the state each atom selects.
static bool atom_log_on() {
    static uint64_t a = 0, b = 0; static const char* k = getenv("BCR2_ATOMLOG");
    if (!k) return false;
    static bool parsed = (sscanf(k, "%llu,%llu", (unsigned long long*)&a, (unsigned long long*)&b), true); (void)parsed;
    const uint64_t f = gpu_frame_number(); return f >= a && f <= b;
}
static void atom_list(const char* tag, uint8_t* base, uint32_t bucket) {
    uint32_t b = be32_at(base, bucket + 56), e = be32_at(base, bucket + 60);
    std::string s;
    for (uint32_t p = b; p < e; p += 4) {
        uint32_t at = be32_at(base, p); uint32_t d = be32_at(base, at + 8);
        char t[64]; snprintf(t, sizeof t, " %08X/%08X/%08X/%u", at, d, be32_at(base, at + 16), be32_at(base, at + 44)); s += t;
    }
    fprintf(stderr, "[atoms] %s f=%llu bucket=%08X mode=%d n=%u:%s\n", tag, (unsigned long long)gpu_frame_number(), bucket, (int)be32_at(base, bucket + 8), (e - b) / 4, s.c_str());
}
extern "C" void __imp__sub_825672E8(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_825672E8) {                                                     // sort one render bucket
    if (!atom_log_on()) { __imp__sub_825672E8(ctx, base); return; }
    uint32_t bucket = ctx.r3.u32;
    atom_list("in", base, bucket);
    __imp__sub_825672E8(ctx, base);
    atom_list("out", base, bucket);
}
extern "C" void __imp__sub_82662708(PPCContext& __restrict__ ctx, uint8_t* base);
PPC_FUNC(sub_82662708) {                                                     // set the state for one atom
    if (!atom_log_on()) { __imp__sub_82662708(ctx, base); return; }
    uint32_t q = ctx.r3.u32, at = ctx.r4.u32, mat = ctx.r5.u32, pass = ctx.r6.u32, bk = ctx.r7.u32;
    uint32_t c0 = be32_at(base, q + 0x10000 - 32408), c1 = be32_at(base, q + 0x10000 - 32412);
    __imp__sub_82662708(ctx, base);
    fprintf(stderr, "[atomstate] f=%llu bucket#%u atom=%08X n=%u mat=%08X pass=%u tech %08X->%08X shader %08X->%08X\n", (unsigned long long)gpu_frame_number(), bk, at, be32_at(base, at + 44), mat, pass, c0, be32_at(base, q + 0x10000 - 32408), c1, be32_at(base, q + 0x10000 - 32412));
}
