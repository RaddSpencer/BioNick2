// D3D11 renderer for the Xenos PM4 stream.
//
// Overview
//  - gpu_exec (imports.cpp) feeds register writes and predicated-in type-3 packets here, on the GPU thread.
//  - Shaders: Xenos microcode is translated to HLSL text (translate_shader) and compiled with D3DCompile; results are
//    cached on disk in <exe dir>\shadercache. All stages share a fixed 16-interpolator signature.
//  - Vertices: vfetch is done in the vertex shader from a ByteAddressBuffer ring holding the byte-swapped guest data.
//  - EDRAM: each (base, format, pitch) gets its own host render target, no aliasing between formats and no MSAA.
//    Tiled rendering works because every tile hits the same host target through its window offset.
//  - Resolves: copied to a host "surface" keyed by guest destination address, never written back to guest memory.
//    Texture lookups and VdSwap find them by address.
//
// Env switches: BCR2_NOGPU (null GPU), BCR2_GPUTRACE=n (log first n draws/resolves), BCR2_SHOTDIR + BCR2_SHOTEVERY (BMP
// frame dumps), BCR2_SHADERDUMP=dir (write HLSL), BCR2_GPUWIDE=1 (log every unknown format once, on by default).
#define NOMINMAX
#include "gpu.h"
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---- registers ----
enum : uint32_t {
    R_SURFACE_INFO = 0x2000, R_COLOR_INFO = 0x2001, R_DEPTH_INFO = 0x2002, R_COLOR1_INFO = 0x2003, R_COLOR2_INFO = 0x2004, R_COLOR3_INFO = 0x2005,
    R_SCREEN_TL = 0x200E, R_SCREEN_BR = 0x200F, R_WINDOW_OFFSET = 0x2080, R_WINDOW_TL = 0x2081, R_WINDOW_BR = 0x2082,
    R_INDX_OFFSET = 0x2102, R_RESET_INDX = 0x2103, R_COLOR_MASK = 0x2104, R_BLEND_RED = 0x2105,
    R_STENCIL_BF = 0x210C, R_STENCIL = 0x210D, R_ALPHA_REF = 0x210E, R_VPORT_XSCALE = 0x210F,
    R_PROGRAM_CNTL = 0x2180, R_CONTEXT_MISC = 0x2181,
    R_DEPTHCONTROL = 0x2200, R_BLEND0 = 0x2201, R_COLORCONTROL = 0x2202, R_CLIP_CNTL = 0x2204, R_MODE_CNTL = 0x2205, R_VTE_CNTL = 0x2206,
    R_MODECONTROL = 0x2208, R_BLEND1 = 0x2209, R_VTX_CNTL = 0x2302,
    R_COPY_CONTROL = 0x2318, R_COPY_DEST_BASE = 0x2319, R_COPY_DEST_PITCH = 0x231A, R_COPY_DEST_INFO = 0x231B,
    R_DEPTH_CLEAR = 0x231D, R_COLOR_CLEAR = 0x231E, R_COLOR_CLEAR_LO = 0x231F,
    R_OFFSET_FRONT_SCALE = 0x2380,
    R_DMA_BASE = 0x21FA, R_DMA_SIZE = 0x21FB,
    R_CONST = 0x4000, R_FETCH = 0x4800, R_BOOL = 0x4900, R_LOOP = 0x4908,
};

static uint32_t g_regs[0x10000];
static bool g_constDirty = true;
// Display gamma ramp the game programs into the display controller. Scanout applies it, so presenting must too.
static uint32_t g_lutEntries[256], g_lutIndex = 0;
static bool g_lutDirty = false, g_lutSet = false;
static uint8_t* g_base = nullptr;
static int g_trace = 0;

bool gpu_enabled() { static const bool on = getenv("BCR2_NOGPU") == nullptr; return on; }
bool gpu_tracing() { return g_trace > 1000; }

// ---- frame timing: printed as [perf] every 300 swaps ----
static double g_perfTicksToMs = 0;
static double g_tDraw = 0, g_tResolve = 0, g_tTex = 0, g_tPresent = 0;
static std::atomic<uint64_t> g_waitTicks[3];                                // ring empty, WAIT_REG_MEM, interrupt
static uint64_t g_nCompiles = 0, g_nUploads = 0, g_upBytes = 0;
void gpu_note_wait(int kind, uint64_t qpcTicks) { g_waitTicks[kind] += qpcTicks; }
struct PerfScope {
    double& acc; LARGE_INTEGER a;
    explicit PerfScope(double& t) : acc(t) { QueryPerformanceCounter(&a); }
    ~PerfScope() { LARGE_INTEGER b; QueryPerformanceCounter(&b); acc += (double)(b.QuadPart - a.QuadPart) * g_perfTicksToMs; }
};

void gpu_reg(uint32_t r, uint32_t v) {
    if (r >= 0x10000) return;
    g_regs[r] = v;
    if (r >= R_CONST && r < R_CONST + 0x800) g_constDirty = true;
    if (r == 0x1922) g_lutIndex = v & 0xFF;                                 // DC_LUT_RW_INDEX
    else if (r == 0x1925) {                                                 // DC_LUT_30_COLOR: 10 bits each, red high, auto-increments
        g_lutEntries[g_lutIndex] = v;
        g_lutIndex = (g_lutIndex + 1) & 0xFF;
        g_lutDirty = g_lutSet = true;
    }
    if (r >= 0x1900 && r < 0x1B00 && r != 0x1925) {                         // other display controller registers, for reference
        static int shown = 0;
        if (shown++ < 40) fprintf(stderr, "[dc] reg %04X = %08X\n", r, v);
    }
}

static inline float reg_f(uint32_t r) { float f; memcpy(&f, &g_regs[r], 4); return f; }
static inline const uint8_t* phys(uint32_t a) { return g_base + (0xA0000000u | (a & 0x1FFFFFFFu)); }
static inline int sext(uint32_t v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }

static void logf_once(const char* key, const char* fmt, ...) {
    static std::set<std::string> seen;
    if (!seen.insert(key).second) return;
    va_list a; va_start(a, fmt); fprintf(stderr, "[gfx] "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n"); va_end(a);
}
static std::string S(const char* fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); return buf;
}
static uint64_t fnv64(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// ---- device ----
static HWND g_hwnd = nullptr;
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<ID3D11DeviceContext1> g_ctx1;
static ComPtr<IDXGISwapChain> g_swap;
static ComPtr<ID3D11RenderTargetView> g_backRtv;
static ComPtr<ID3D11Texture2D> g_backTex;
static ComPtr<ID3D11SamplerState> g_presentSampler;
static ComPtr<ID3D11PixelShader> g_psPresentPlain;
static uint32_t g_backW = 1280, g_backH = 720;
static bool g_devOk = false, g_noOverwriteSrv = false;
static ComPtr<ID3D11Buffer> g_cbFloat, g_cbState, g_cbBlit, g_vbRing, g_ibRing;
static ComPtr<ID3D11ShaderResourceView> g_vbSrv;
static ComPtr<ID3D11GeometryShader> g_gsRect;
static ComPtr<ID3D11VertexShader> g_vsBlit;
static ComPtr<ID3D11PixelShader> g_psBlit, g_psBlitDepth, g_psBlitRaw, g_psColorToDepth, g_psPresentLut;
static ComPtr<ID3D11Texture2D> g_lutTex;
static ComPtr<ID3D11ShaderResourceView> g_lutSrv;
static ComPtr<ID3D11SamplerState> g_blitSampler;
static const uint32_t kVbRingSize = 96u << 20, kIbRingSize = 16u << 20;
static uint32_t g_vbPos = 0, g_ibPos = 0;
static uint64_t g_frame = 0, g_draws = 0, g_resolves = 0, g_drawsFrame = 0;

// Display modes. The window thread only asks; the GPU thread owns the swap chain and makes the change between frames.
// Alt+Enter or F11: windowed fullscreen. Shift+Alt+Enter: exclusive fullscreen. BCR2_DISPLAY=window|borderless|fullscreen picks the starting mode.
enum DisplayMode { kWindowed = 0, kBorderless = 1, kExclusive = 2 };
static std::atomic<int> g_modeWanted{kWindowed};
static std::atomic<bool> g_sizeChanged{false};
static int g_modeNow = kWindowed;

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { fprintf(stderr, "[gfx] window closed, exiting\n"); fflush(stderr); ExitProcess(0); }
    if (m == WM_SIZE) g_sizeChanged = true;
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_modeWanted.load() != kWindowed) { SetCursor(nullptr); return TRUE; }
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29))) || (m == WM_KEYDOWN && w == VK_F11)) {
        if (!(l & (1 << 30))) {                                             // ignore auto-repeat
            int want = (m == WM_SYSKEYDOWN && (GetKeyState(VK_SHIFT) & 0x8000)) ? kExclusive : kBorderless;
            g_modeWanted = g_modeWanted.load() == kWindowed ? want : kWindowed;
        }
        return 0;
    }
    if (m == WM_SYSCHAR && w == VK_RETURN) return 0;                        // no error beep
    return DefWindowProcW(h, m, w, l);
}
static DWORD WINAPI window_thread(LPVOID ev) {
    WNDCLASSW wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"bcr2win";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT rc{ 0, 0, 1280, 720 }; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(L"bcr2win", L"Bionic Commando Rearmed 2 (recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    SetEvent((HANDLE)ev);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

static ComPtr<ID3DBlob> compile_cached(const std::string& src, const char* entry, const char* target) {
    static const std::string dir = [] {
        wchar_t w[MAX_PATH]; GetModuleFileNameW(nullptr, w, MAX_PATH);
        char a[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, w, -1, a, MAX_PATH, nullptr, nullptr);
        std::string s = a; s = s.substr(0, s.find_last_of('\\')) + "\\shadercache";
        CreateDirectoryA(s.c_str(), nullptr);
        return s;
    }();
    uint64_t h = fnv64(src.data(), src.size()); h = fnv64(target, strlen(target), h);
    std::string path = dir + "\\" + S("%016llX.dxbc", (unsigned long long)h);
    ComPtr<ID3DBlob> blob;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0 && SUCCEEDED(D3DCreateBlob(n, &blob))) { if (fread(blob->GetBufferPointer(), 1, n, f) != (size_t)n) blob.Reset(); }
        fclose(f);
        if (blob) return blob;
    }
    ComPtr<ID3DBlob> err;
    g_nCompiles++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_ENABLE_STRICTNESS * 0, 0, &blob, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[gfx] shader compile failed (%s): %s\n", target, err ? (const char*)err->GetBufferPointer() : "?");
        static int nd = 0;
        if (nd++ < 8) {
            std::string dp = dir + "\\" + S("failed_%016llX.hlsl", (unsigned long long)h);
            if (FILE* f = fopen(dp.c_str(), "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); fprintf(stderr, "[gfx]   source written to %s\n", dp.c_str()); }
        }
        return nullptr;
    }
    if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f); fclose(f); }
    return blob;
}

static const char* kVOut = "struct VOut { float4 pos : SV_Position; float4 o[16] : TEXCOORD; };\n";

static bool device_init() {
    if (g_trace == 0) if (const char* t = getenv("BCR2_GPUTRACE")) g_trace = atoi(t);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, window_thread, ev, 0, nullptr);
    WaitForSingleObject(ev, 10000);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = 1280; sd.BufferDesc.Height = 720; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want + 1, 1, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) { fprintf(stderr, "[gfx] D3D11 device creation failed: %08lX; running without rendering\n", hr); return false; }
    g_ctx.As(&g_ctx1);
    {   // Queue at most one finished frame ahead of the display. The default of three adds visible input lag.
        ComPtr<IDXGIDevice1> dxgi;
        if (SUCCEEDED(g_dev.As(&dxgi))) dxgi->SetMaximumFrameLatency(1);
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
    if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt)))) g_noOverwriteSrv = opt.MapNoOverwriteOnDynamicBufferSRV && !getenv("BCR2_NONOOVERWRITE");
    g_swap->GetBuffer(0, IID_PPV_ARGS(&g_backTex));
    g_dev->CreateRenderTargetView(g_backTex.Get(), nullptr, &g_backRtv);
    {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory)))) factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (const char* mode = getenv("BCR2_DISPLAY")) {
            if (!strcmp(mode, "borderless")) g_modeWanted = kBorderless;
            else if (!strcmp(mode, "fullscreen")) g_modeWanted = kExclusive;
        }
        D3D11_SAMPLER_DESC sd2{}; sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd2.MaxLOD = D3D11_FLOAT32_MAX;
        g_dev->CreateSamplerState(&sd2, &g_presentSampler);
    }

    auto mkcb = [](UINT size, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = size; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&d, nullptr, &out);
    };
    mkcb(8192, g_cbFloat); mkcb(4096, g_cbState); mkcb(32, g_cbBlit);
    D3D11_BUFFER_DESC vb{}; vb.ByteWidth = kVbRingSize; vb.BindFlags = D3D11_BIND_SHADER_RESOURCE; vb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (g_noOverwriteSrv) { vb.Usage = D3D11_USAGE_DYNAMIC; vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; }
    else vb.Usage = D3D11_USAGE_DEFAULT;
    g_dev->CreateBuffer(&vb, nullptr, &g_vbRing);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx.NumElements = kVbRingSize / 4; sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    g_dev->CreateShaderResourceView(g_vbRing.Get(), &sv, &g_vbSrv);
    D3D11_BUFFER_DESC ib{}; ib.ByteWidth = kIbRingSize; ib.Usage = D3D11_USAGE_DYNAMIC; ib.BindFlags = D3D11_BIND_INDEX_BUFFER; ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&ib, nullptr, &g_ibRing);

    // Rectangle list: emit the fourth corner, opposite the corner between the two shorter edges.
    std::string gs = std::string(kVOut) +
        "[maxvertexcount(4)] void main(triangle VOut v[3], inout TriangleStream<VOut> os) {\n"
        "  float2 p0=v[0].pos.xy/v[0].pos.w, p1=v[1].pos.xy/v[1].pos.w, p2=v[2].pos.xy/v[2].pos.w;\n"
        "  float d01=dot(p1-p0,p1-p0), d12=dot(p2-p1,p2-p1), d20=dot(p0-p2,p0-p2);\n"
        "  int c = (d12>=d01 && d12>=d20) ? 0 : ((d20>=d01) ? 1 : 2);\n"
        "  VOut C=v[c], A=v[(c+1)%3], B=v[(c+2)%3], D;\n"
        "  D.pos=A.pos+B.pos-C.pos; for(int i=0;i<16;i++) D.o[i]=A.o[i]+B.o[i]-C.o[i];\n"
        "  os.Append(A); os.Append(C); os.Append(B); os.Append(D); }\n";
    if (auto b = compile_cached(gs, "main", "gs_5_0")) g_dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_gsRect);
    const char* blitVs = "float4 main(uint i : SV_VertexID) : SV_Position { float2 t=float2((i<<1)&2,i&2); return float4(t.x*2-1,1-t.y*2,0,1); }\n";
    if (auto b = compile_cached(blitVs, "main", "vs_5_0")) g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vsBlit);
    const char* blitPs =
        "Texture2D src:register(t0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { float4 c=src.Load(int3(int2(p.xy*scale+ofs),0)); return swaprb? c.bgra : c; }\n";
    if (auto b = compile_cached(blitPs, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psBlit);
    const char* blitDs =
        "Texture2D src:register(t0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { float d=src.Load(int3(int2(p.xy*scale+ofs),0)).r; return float4(d,d,d,1); }\n";
    if (auto b = compile_cached(blitDs, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psBlitDepth);
    const char* blitRaw =
        "Texture2D<float> src:register(t0); Texture2D<uint2> sten:register(t1); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { int3 c=int3(int2(p.xy*scale+ofs),0); float x=src.Load(c); uint d=(x>=0.5)?((uint)round(x*16777216.0)-1u):(uint)round(x*16777215.0); d=min(d,0xFFFFFFu); uint s=sten.Load(c).g;\n"
        "  uint w=(d<<8)|(s&255); return float4(w&255,(w>>8)&255,(w>>16)&255,(w>>24)&255)/255.0; }\n";
    if (auto b = compile_cached(blitRaw, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psBlitRaw);
    // Present samples with a filter, so the picture can be scaled. At 1:1 every sample lands on a texel centre, which gives the texel unchanged.
    const char* presentLut =
        "Texture2D src:register(t0); Texture2D lut:register(t1); SamplerState smp:register(s0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { float w,h; src.GetDimensions(w,h); float4 c=src.SampleLevel(smp,(p.xy*scale+ofs)/float2(w,h),0); if(swaprb) c=c.bgra;\n"
        "  float3 u=(saturate(c.rgb)*255.0+0.5)/256.0;\n"
        "  return float4(lut.SampleLevel(smp,float2(u.r,0.5),0).r, lut.SampleLevel(smp,float2(u.g,0.5),0).g, lut.SampleLevel(smp,float2(u.b,0.5),0).b, 1.0); }\n";
    if (auto b = compile_cached(presentLut, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psPresentLut);
    const char* presentPlain =
        "Texture2D src:register(t0); SamplerState smp:register(s0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { float w,h; src.GetDimensions(w,h); float4 c=src.SampleLevel(smp,(p.xy*scale+ofs)/float2(w,h),0); return float4(swaprb? c.bgr : c.rgb,1.0); }\n";
    if (auto b = compile_cached(presentPlain, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psPresentPlain);
    {
        D3D11_TEXTURE2D_DESC d{}; d.Width = 256; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R10G10B10A2_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &g_lutTex);
        if (g_lutTex) g_dev->CreateShaderResourceView(g_lutTex.Get(), nullptr, &g_lutSrv);
    }
    // EDRAM aliasing: as 8_8_8_8 the sample is A<<24|B<<16|G<<8|R, as D24S8 the same dword is D24<<8|S8. Depth tiles
    // also have their 40-pixel left and right halves swapped relative to colour (the game's restore shader assumes this).
    const char* c2d =
        "Texture2D src:register(t0);\n"
        "float main(float4 p:SV_Position):SV_Depth { int x=(int)p.x; x+=((x%80)<40)?40:-40; float4 c=src.Load(int3(x,(int)p.y,0));\n"
        "  uint a=(uint)(c.a*255.0+0.5), b=(uint)(c.b*255.0+0.5), g=(uint)(c.g*255.0+0.5);\n"
        // Lossless: the game redraws meshes with depth EQUAL, so this must round to exactly the same D24 value.
        // Above 0.5 a float can only hold multiples of 2^-24, and (d+1)*2^-24 is the one that rounds back to d.
        "  uint d=(a<<16)|(b<<8)|g; return (d>=0x800000u)?(float)(d+1u)*5.9604644775390625e-8:(float)d/16777215.0; }\n";
    if (auto b = compile_cached(c2d, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColorToDepth);
    fprintf(stderr, "[gfx] D3D11 ready, feature level %X, noOverwriteSrv=%d\n", (unsigned)got, (int)g_noOverwriteSrv);
    return true;
}

// ---- shader translation ----
struct TexUse { uint32_t slot, dim; int magf = -1, minf = -1, mipf = -1; };
struct VfUse { uint32_t slot, stride; };
struct XShader {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::vector<TexUse> tex;
    std::vector<VfUse> vf;
    bool ok = false;
    uint64_t hash = 0;
};

// Declarations shared by both stages.
static const char* kCommon = R"(
cbuffer CF : register(b0) { float4 c[512]; };
cbuffer CS : register(b1) {
  uint4 bools[2]; uint4 loops[8];
  float4 vpNdc;     // x scale, y scale, x offset, y offset
  float4 vpZ;       // z scale, z offset, xy_fmt, z_fmt
  float4 vpW;       // w0_fmt, index offset
  float4 alpha;     // func, ref
  uint4 vfo[24];
  int4 tfsw[32];
  float4 tfp[32];   // uv scale x/y, exp scale, cube/unused
  uint4 tfsg[32];
  float4 tfi[32];   // 1/width, 1/height of the bound host texture
};
Texture2D T2[16] : register(t0);
TextureCube TC[16] : register(t16);
SamplerState SS[16] : register(s0);
ByteAddressBuffer VB : register(t64);
float4 texfix(float4 t, int n) {
  uint4 g = tfsg[n];
  float4 b = t * 2.0 - 1.0;
  float4 s = (t >= 0.5) ? (t * 255.0 - 256.0) / 127.0 : t * 255.0 / 127.0;
  // Xenos piecewise-linear gamma to linear
  float4 lin = (t < 0.25) ? t * 0.25 : ((t < 0.375) ? t * 0.5 - 0.0625 : ((t < 0.75) ? t - 0.25 : t * 2.0 - 1.0));
  t = (g == 1) ? s : ((g == 2) ? b : ((g == 3) ? lin : t));
  t *= tfp[n].z;
  float v6[6] = { t.x, t.y, t.z, t.w, 0.0, 1.0 };
  int4 w = min(tfsw[n], 5);
  return float4(v6[w.x], v6[w.y], v6[w.z], v6[w.w]);
}
float3 cubedir(float3 u) {
  float sc = (u.x - 1.5) * 2.0, tc = (u.y - 1.5) * 2.0;
  int f = (int)(u.z + 0.5);
  if (f == 0) return float3(1, -tc, -sc);
  if (f == 1) return float3(-1, -tc, sc);
  if (f == 2) return float3(sc, 1, tc);
  if (f == 3) return float3(sc, -1, -tc);
  if (f == 4) return float3(sc, -tc, 1);
  return float3(-sc, -tc, -1);
}
)";

struct Translator {
    const uint32_t* ucode; uint32_t words; bool isVs;
    std::string body;
    std::set<int> usedRegs;
    bool useArray = false, needRel = false, writesDepth = false, hasLoop = false;
    XShader* out;
    std::map<uint32_t, TexUse> texs; std::map<uint32_t, uint32_t> vfs;
    int uniq = 0;
    uint32_t prevFull[3] = { 0, 0, 0 };

    std::string R(int n) {
        n &= 63; usedRegs.insert(n);
        return useArray ? S("r[%d]", n) : S("r%d", n);
    }
    std::string Rrel(int n, bool rel) {
        if (!rel) return R(n);
        needRel = true;
        return S("r[clamp(aL+%d,0,63)]", n & 63);
    }
    static char comp(int i) { return "xyzw"[i & 3]; }
    static int swz(uint32_t s, int c) { return ((s >> (2 * c)) + c) & 3; }

    static uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)); }

    std::string src_base(const uint32_t* d, int i) {     // i = 1..3, unswizzled value with abs (no negate)
        uint32_t d1 = d[1], d2 = d[2];
        uint32_t reg = i == 1 ? bits(d2, 16, 8) : i == 2 ? bits(d2, 8, 8) : bits(d2, 0, 8);
        bool isTemp = i == 1 ? bits(d2, 31, 1) : i == 2 ? bits(d2, 30, 1) : bits(d2, 29, 1);
        bool s1t = bits(d2, 31, 1), s2t = bits(d2, 30, 1);
        std::string e;
        if (isTemp) {
            e = Rrel(reg & 63, (reg & 0x40) != 0);
            if (reg & 0x80) e = "abs(" + e + ")";
        } else {
            bool c0 = bits(d1, 31, 1), c1 = bits(d1, 30, 1), arel = bits(d1, 29, 1);
            bool addressed = i == 1 ? c0 : i == 2 ? (s1t ? c0 : c1) : ((s1t && s2t) ? c0 : c1);
            int idx = reg + (isVs ? 0 : 256);
            if (addressed) e = S("c[clamp(%s+%d,0,511)]", arel ? "a0" : "aL", idx);
            else e = S("c[%d]", idx);
            if (bits(d[0], 7, 1)) e = "abs(" + e + ")";
        }
        return e;
    }
    std::string src_vec(const uint32_t* d, int i) {
        uint32_t sw = bits(d[1], i == 1 ? 16 : i == 2 ? 8 : 0, 8);
        bool neg = bits(d[1], i == 1 ? 26 : i == 2 ? 25 : 24, 1);
        std::string e = src_base(d, i);
        if (sw != 0) e += S(".%c%c%c%c", comp(swz(sw, 0)), comp(swz(sw, 1)), comp(swz(sw, 2)), comp(swz(sw, 3)));
        return neg ? "-(" + e + ")" : e;
    }

    void emit_alu(const uint32_t* d) {
        uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
        uint32_t vdst = bits(d0, 0, 6), sdst = bits(d0, 8, 6), vmask = bits(d0, 16, 4), smask = bits(d0, 20, 4);
        bool vrel = bits(d0, 6, 1), srel = bits(d0, 14, 1), exp = bits(d0, 15, 1), vclamp = bits(d0, 24, 1), sclamp = bits(d0, 25, 1);
        uint32_t sop = bits(d0, 26, 6), vop = bits(d2, 24, 5);
        bool pred = bits(d1, 28, 1), predc = bits(d1, 27, 1);
        std::string& B = body;
        if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
        B += "{\n";
        // vector result needed?
        uint32_t c0Mask = 0, c1Mask = 0, vRes = vmask, sRes = smask;
        if (exp) {
            vRes = vmask & ~smask; sRes = smask & ~vmask;
            c1Mask = vmask & smask;
            if (srel) c0Mask = 0xF & ~(vmask | smask);
        }
        bool vSide = (vop >= 20 && vop <= 27) || vop == 29;
        if (vRes || vSide) {
            B += " float4 s1=" + src_vec(d, 1) + ", s2=" + src_vec(d, 2) + ", s3=" + src_vec(d, 3) + ", v=0;\n";
            switch (vop) {
              case 0: B += " v=s1+s2;\n"; break;
              case 1: B += " v=s1*s2;\n"; break;
              case 2: B += " v=max(s1,s2);\n"; break;
              case 3: B += " v=min(s1,s2);\n"; break;
              case 4: B += " v=(s1==s2)?1.0:0.0;\n"; break;
              case 5: B += " v=(s1>s2)?1.0:0.0;\n"; break;
              case 6: B += " v=(s1>=s2)?1.0:0.0;\n"; break;
              case 7: B += " v=(s1!=s2)?1.0:0.0;\n"; break;
              case 8: B += " v=frac(s1);\n"; break;
              case 9: B += " v=trunc(s1);\n"; break;
              case 10: B += " v=floor(s1);\n"; break;
              case 11: B += " v=s1*s2+s3;\n"; break;
              case 12: B += " v=(s1==0.0)?s2:s3;\n"; break;
              case 13: B += " v=(s1>=0.0)?s2:s3;\n"; break;
              case 14: B += " v=(s1>0.0)?s2:s3;\n"; break;
              case 15: B += " v=dot(s1,s2).xxxx;\n"; break;
              case 16: B += " v=dot(s1.xyz,s2.xyz).xxxx;\n"; break;
              case 17: B += " v=(dot(s1.xy,s2.xy)+s3.x).xxxx;\n"; break;
              case 18:
                B += " { float3 dd=float3(s1.z,s1.w,s1.x); float3 aa=abs(dd); float tc,sc,ma,fc;\n"
                     "   if(aa.z>=aa.x&&aa.z>=aa.y){tc=-dd.y;sc=dd.z<0?-dd.x:dd.x;ma=2*dd.z;fc=dd.z<0?5:4;}\n"
                     "   else if(aa.y>=aa.x){tc=dd.y<0?-dd.z:dd.z;sc=dd.x;ma=2*dd.y;fc=dd.y<0?3:2;}\n"
                     "   else{tc=-dd.y;sc=dd.x<0?dd.z:-dd.z;ma=2*dd.x;fc=dd.x<0?1:0;}\n"
                     "   v=float4(tc,sc,ma,fc); }\n";
                break;
              case 19: B += " v=max(max(s1.x,s1.y),max(s1.z,s1.w)).xxxx;\n"; break;
              case 20: case 21: case 22: case 23: {
                const char* o = vop == 20 ? "==" : vop == 21 ? "!=" : vop == 22 ? ">" : ">=";
                // src0 is the outer nesting counter (0 = active), src1 is the value tested
                B += S(" p=(s1.w==0.0)&&(s2.w%s0.0);\n v=((s1.x==0.0)&&(s2.x%s0.0))?0.0:(s1.x+1.0).xxxx;\n", o, o);
                break; }
              case 24: case 25: case 26: case 27: {
                const char* o = vop == 24 ? "==" : vop == 25 ? ">" : vop == 26 ? ">=" : "!=";
                if (!isVs) B += S(" if(any(s1%ss2)) discard;\n", o);
                B += S(" v=any(s1%ss2)?1.0:0.0;\n", o);
                break; }
              case 28: B += " v=float4(1.0,s1.y*s2.y,s1.z,s2.w);\n"; break;
              case 29: B += " a0=clamp((int)floor(s1.w+0.5),-256,255); v=max(s1,s2);\n"; break;
              default: logf_once(S("vop%u", vop).c_str(), "unknown vector opcode %u", vop); break;
            }
            if (vclamp) B += " v=saturate(v);\n";
        }
        bool sNeeded = sop != 50 || sRes;
        if (sNeeded) {
            uint32_t sw = bits(d1, 0, 8);
            bool neg = bits(d1, 24, 1);
            std::string ng = neg ? "-" : "";
            std::string base = src_base(d, 3);
            int ca = (((sw >> 6) & 3) + 3) & 3, cb = (sw & 3) & 3;
            if (sop >= 42 && sop <= 47) {
                // mulsc/addsc/subsc: the constant comes from src3, the temp index is packed in opcode bit 0, src3_sel and the swizzle
                uint32_t treg = (sop & 1) | (bits(d2, 29, 1) << 1) | (sw & 0x3C);
                uint32_t creg = bits(d2, 0, 8);
                bool c0 = bits(d1, 31, 1), c1 = bits(d1, 30, 1), arel = bits(d1, 29, 1);
                bool s1t = bits(d2, 31, 1), s2t = bits(d2, 30, 1);
                bool addressed = (s1t && s2t) ? c0 : c1;
                int idx = creg + (isVs ? 0 : 256);
                std::string ce = addressed ? S("c[clamp(%s+%d,0,511)]", arel ? "a0" : "aL", idx) : S("c[%d]", idx);
                if (bits(d0, 7, 1)) ce = "abs(" + ce + ")";
                std::string te = R(treg);
                if (bits(d0, 7, 1)) te = "abs(" + te + ")";
                B += " float a=" + ng + "(" + ce + S(".%c", comp(ca)) + "), b=" + ng + "(" + te + S(".%c", comp(cb)) + "), sr=0;\n";
            } else {
                B += " float a=" + ng + "(" + base + S(".%c", comp(ca)) + "), b=" + ng + "(" + base + S(".%c", comp(cb)) + "), sr=0;\n";
            }
            const char* FM = "3.4028235e38";
            switch (sop) {
              case 0: case 44: case 45: B += " sr=a+b;\n"; break;
              case 1: B += " sr=a+ps;\n"; break;
              case 2: case 42: case 43: B += " sr=a*b;\n"; break;
              case 3: B += " sr=a*ps;\n"; break;
              case 4: B += S(" sr=(ps==-%s||isinf(ps)||isnan(ps)||isnan(b)||b<=0.0)?-%s:a*ps;\n", FM, FM); break;
              case 5: B += " sr=max(a,b);\n"; break;
              case 6: B += " sr=min(a,b);\n"; break;
              case 7: B += " sr=(a==0.0)?1.0:0.0;\n"; break;
              case 8: B += " sr=(a>0.0)?1.0:0.0;\n"; break;
              case 9: B += " sr=(a>=0.0)?1.0:0.0;\n"; break;
              case 10: B += " sr=(a!=0.0)?1.0:0.0;\n"; break;
              case 11: B += " sr=frac(a);\n"; break;
              case 12: B += " sr=trunc(a);\n"; break;
              case 13: B += " sr=floor(a);\n"; break;
              case 14: B += " sr=pow(2.0,a);\n"; break;
              case 15: B += S(" sr=log2(abs(a)); if(isinf(sr)||isnan(sr)) sr=-%s;\n", FM); break;
              case 16: B += " sr=log2(abs(a));\n"; break;
              case 17: B += S(" sr=clamp(1.0/a,-%s,%s);\n", FM, FM); break;
              case 18: B += " sr=1.0/a; if(isinf(sr)||isnan(sr)) sr=0.0;\n"; break;
              case 19: B += " sr=1.0/a;\n"; break;
              case 20: B += S(" sr=clamp(1.0/sqrt(abs(a)),-%s,%s);\n", FM, FM); break;
              case 21: B += " sr=1.0/sqrt(abs(a)); if(isinf(sr)||isnan(sr)) sr=0.0;\n"; break;
              case 22: B += " sr=1.0/sqrt(abs(a));\n"; break;
              case 23: B += " a0=clamp((int)floor(a+0.5),-256,255); sr=max(a,b);\n"; break;
              case 24: B += " a0=clamp((int)floor(a),-256,255); sr=max(a,b);\n"; break;
              case 25: case 46: case 47: B += " sr=a-b;\n"; break;
              case 26: B += " sr=a-ps;\n"; break;
              case 27: B += " p=(a==0.0); sr=p?0.0:1.0;\n"; break;
              case 28: B += " p=(a!=0.0); sr=p?0.0:1.0;\n"; break;
              case 29: B += " p=(a>0.0); sr=p?0.0:1.0;\n"; break;
              case 30: B += " p=(a>=0.0); sr=p?0.0:1.0;\n"; break;
              case 31: B += " if(a==0.0){p=false;sr=1.0;}else if(a==1.0){p=true;sr=0.0;}else{p=false;sr=a;}\n"; break;
              case 32: B += " sr=a-1.0; if(sr<=0.0){p=true;sr=0.0;}else{p=false;}\n"; break;
              case 33: B += S(" p=false; sr=%s;\n", FM); break;
              case 34: B += " p=(a==0.0); sr=a;\n"; break;
              case 35: case 36: case 37: case 38: case 39: {
                const char* cnd = sop == 35 ? "a==0.0" : sop == 36 ? "a>0.0" : sop == 37 ? "a>=0.0" : sop == 38 ? "a!=0.0" : "a==1.0";
                if (!isVs) B += S(" if(%s) discard;\n", cnd);
                B += S(" sr=(%s)?1.0:0.0;\n", cnd);
                break; }
              case 40: B += " sr=sqrt(a);\n"; break;
              case 48: B += " sr=sin(a);\n"; break;
              case 49: B += " sr=cos(a);\n"; break;
              case 50: B += " sr=ps;\n"; break;
              default: logf_once(S("sop%u", sop).c_str(), "unknown scalar opcode %u", sop); break;
            }
            if (sclamp) B += " sr=saturate(sr);\n";
        }
        auto maskstr = [](uint32_t m) { std::string s; for (int i = 0; i < 4; i++) if (m & (1u << i)) s += comp(i); return s; };
        if (exp) {
            std::string dst;
            if (isVs) {
                if (vdst == 62) dst = "oPos";
                else if (vdst == 63) dst = "oPts";
                else if (vdst < 16) dst = S("oI[%u]", vdst);
                else { logf_once(S("vexp%u", vdst).c_str(), "ignored VS export register %u (memexport?)", vdst); }
            } else {
                if (vdst < 4) dst = S("oC[%u]", vdst);
                else if (vdst == 61) { dst = "oDepth"; writesDepth = true; }
                else { logf_once(S("pexp%u", vdst).c_str(), "ignored PS export register %u", vdst); }
            }
            if (!dst.empty()) {
                if (dst == "oDepth") {
                    if (vRes & 1) B += " oDepth=v.x;\n"; else if (sRes & 1) B += " oDepth=sr;\n";
                } else {
                    if (vRes) B += " " + dst + "." + maskstr(vRes) + "=v." + maskstr(vRes) + ";\n";
                    if (sRes) B += " " + dst + "." + maskstr(sRes) + "=sr;\n";
                    if (c0Mask) B += " " + dst + "." + maskstr(c0Mask) + "=0.0;\n";
                    if (c1Mask) B += " " + dst + "." + maskstr(c1Mask) + "=1.0;\n";
                }
            }
        } else {
            if (vRes) B += " " + Rrel(vdst, vrel) + "." + maskstr(vRes) + "=v." + maskstr(vRes) + ";\n";
            if (sRes) B += " " + Rrel(sdst, srel) + "." + maskstr(sRes) + "=sr;\n";
        }
        if (sNeeded) B += " ps=sr;\n";
        B += "}\n";
    }

    void emit_vfetch(const uint32_t* d) {
        uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
        bool mini = bits(d1, 30, 1);
        if (!mini) memcpy(prevFull, d, 12);
        const uint32_t* f = mini ? prevFull : d;
        uint32_t srcReg = bits(f[0], 5, 6), srcSw = bits(f[0], 30, 2), slot = bits(f[0], 20, 5) * 3 + bits(f[0], 25, 2);
        uint32_t stride = bits(f[2], 0, 8);
        bool rounded = bits(f[1], 15, 1);
        uint32_t dst = bits(d0, 12, 6), dsw = bits(d1, 0, 12), fmt = bits(d1, 16, 6);
        bool isSigned = bits(d1, 12, 1), isInt = bits(d1, 13, 1), rfNoZero = bits(d1, 14, 1);
        int exp = sext(bits(d1, 24, 6), 6);
        int offset = sext(bits(d2, 8, 23), 23);
        bool pred = bits(d1, 31, 1), predc = bits(d2, 31, 1);
        auto it = vfs.find(slot);
        if (it == vfs.end() || it->second < stride) vfs[slot] = stride;
        std::string& B = body;
        if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
        B += "{\n";
        std::string idxExpr = R(srcReg) + S(".%c", comp(srcSw));
        B += S(" uint vi=(uint)((int)%s(%s)); uint va=vfo[%u].%c+vi*%uu+%du; uint w0=VB.Load(va), w1=0, w2=0, w3=0; float4 fv=float4(0,0,0,1);\n",
               rounded ? "round" : "", idxExpr.c_str(), slot >> 2, comp(slot & 3), stride * 4, offset * 4);
        // integer component extraction: x = (w >> off) & mask, sign/normalise as requested
        auto intcomp = [&](const char* dw, int off, int bitsw) {
            std::string v;
            if (isSigned) v = S("float((int)(%s<<%d)>>%d)", dw, 32 - off - bitsw, 32 - bitsw);
            else v = S("float((%s>>%d)&%uu)", dw, off, bitsw == 32 ? 0xFFFFFFFFu : ((1u << bitsw) - 1));
            if (!isInt) {
                float umax = (float)(bitsw == 32 ? 4294967295.0 : (double)((1u << bitsw) - 1));
                float smax = (float)(bitsw == 32 ? 2147483647.0 : (double)((1u << (bitsw - 1)) - 1));
                if (isSigned) v = rfNoZero ? S("((2.0*%s+1.0)/%.1f)", v.c_str(), umax) : S("max(%s/%.1f,-1.0)", v.c_str(), smax);
                else v = S("(%s/%.1f)", v.c_str(), umax);
            }
            return v;
        };
        auto setc = [&](char c, const std::string& e) { B += S(" fv.%c=", c) + e + ";\n"; };
        auto f16 = [](const char* dw, int off) { return S("f16tof32((%s>>%d)&0xFFFFu)", dw, off); };
        switch (fmt) {
          case 6: setc('x', intcomp("w0", 0, 8)); setc('y', intcomp("w0", 8, 8)); setc('z', intcomp("w0", 16, 8)); setc('w', intcomp("w0", 24, 8)); break;
          case 7: setc('x', intcomp("w0", 0, 10)); setc('y', intcomp("w0", 10, 10)); setc('z', intcomp("w0", 20, 10)); setc('w', intcomp("w0", 30, 2)); break;
          case 16: setc('x', intcomp("w0", 0, 11)); setc('y', intcomp("w0", 11, 11)); setc('z', intcomp("w0", 22, 10)); break;
          case 17: setc('x', intcomp("w0", 0, 10)); setc('y', intcomp("w0", 10, 11)); setc('z', intcomp("w0", 21, 11)); break;
          case 25: setc('x', intcomp("w0", 0, 16)); setc('y', intcomp("w0", 16, 16)); break;
          case 26: B += " w1=VB.Load(va+4);\n"; setc('x', intcomp("w0", 0, 16)); setc('y', intcomp("w0", 16, 16)); setc('z', intcomp("w1", 0, 16)); setc('w', intcomp("w1", 16, 16)); break;
          case 31: setc('x', f16("w0", 0)); setc('y', f16("w0", 16)); break;
          case 32: B += " w1=VB.Load(va+4);\n"; setc('x', f16("w0", 0)); setc('y', f16("w0", 16)); setc('z', f16("w1", 0)); setc('w', f16("w1", 16)); break;
          case 33: setc('x', intcomp("w0", 0, 32)); break;
          case 34: B += " w1=VB.Load(va+4);\n"; setc('x', intcomp("w0", 0, 32)); setc('y', intcomp("w1", 0, 32)); break;
          case 35: B += " w1=VB.Load(va+4); w2=VB.Load(va+8); w3=VB.Load(va+12);\n"; setc('x', intcomp("w0", 0, 32)); setc('y', intcomp("w1", 0, 32)); setc('z', intcomp("w2", 0, 32)); setc('w', intcomp("w3", 0, 32)); break;
          case 36: B += " fv.x=asfloat(w0);\n"; break;
          case 37: B += " w1=VB.Load(va+4); fv.xy=float2(asfloat(w0),asfloat(w1));\n"; break;
          case 57: B += " w1=VB.Load(va+4); w2=VB.Load(va+8); fv.xyz=float3(asfloat(w0),asfloat(w1),asfloat(w2));\n"; break;
          case 38: B += " w1=VB.Load(va+4); w2=VB.Load(va+8); w3=VB.Load(va+12); fv=float4(asfloat(w0),asfloat(w1),asfloat(w2),asfloat(w3));\n"; break;
          default: logf_once(S("vfmt%u", fmt).c_str(), "unknown vertex format %u", fmt); break;
        }
        if (exp) B += S(" fv.xyzw*=%.9g;\n", std::ldexp(1.0, exp));
        for (int i = 0; i < 4; i++) {
            uint32_t s = (dsw >> (3 * i)) & 7;
            if (s == 7) continue;
            std::string val = s < 4 ? S("fv.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
            B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
        }
        B += "}\n";
    }

    void emit_tfetch(const uint32_t* d) {
        uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
        uint32_t op = bits(d0, 0, 5), src = bits(d0, 5, 6), dst = bits(d0, 12, 6), slot = bits(d0, 20, 5), srcSw = bits(d0, 26, 6);
        uint32_t dsw = bits(d1, 0, 12), dim = bits(d2, 14, 2);
        bool pred = bits(d1, 31, 1), predc = bits(d2, 31, 1);
        std::string& B = body;
        if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
        B += "{\n";
        std::string sreg = R(src);
        if (op == 24) { B += " lodReg=" + sreg + S(".%c;\n", comp(srcSw & 3)) + "}\n"; return; }
        if (op == 25 || op == 26) { B += "}\n"; return; }                // setGradientH/V: unused for now
        if (op == 18) {                                                     // getGradients
            std::string g = isVs ? "float4(0,0,0,0)" : "float4(ddx(" + sreg + ".x),ddx(" + sreg + ".y),ddy(" + sreg + ".x),ddy(" + sreg + ".y))";
            B += " float4 t=" + g + ";\n";
            for (int i = 0; i < 4; i++) {
                uint32_t s = (dsw >> (3 * i)) & 7;
                if (s == 7) continue;
                std::string val = s < 4 ? S("t.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
                B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
            }
            B += "}\n"; return;
        }
        if (op != 1 && op != 17) {
            logf_once(S("fop%u", op).c_str(), "unhandled fetch opcode %u", op);
            B += "}\n"; return;
        }
        if (slot >= 16) { logf_once("tf16", "texture fetch slot %u beyond sampler range", slot); B += "}\n"; return; }
        TexUse u{ slot, dim };
        uint32_t magf = bits(d1, 12, 2), minf = bits(d1, 14, 2), mipf = bits(d1, 16, 2);
        if (magf != 3) u.magf = magf;
        if (minf != 3) u.minf = minf;
        if (mipf != 3) u.mipf = mipf;
        auto it = texs.find(slot);
        if (it == texs.end()) texs[slot] = u;
        bool denorm = bits(d0, 25, 1), useRegLod = bits(d1, 29, 1), useCompLod = bits(d1, 28, 1);
        int bias = sext(bits(d2, 2, 7), 7);
        float ox = sext(bits(d2, 16, 5), 5) * 0.5f, oy = sext(bits(d2, 21, 5), 5) * 0.5f;
        std::string uvw = sreg + S(".%c%c%c", comp(srcSw & 3), comp((srcSw >> 2) & 3), comp((srcSw >> 4) & 3));
        std::string lod = useRegLod ? "lodReg" : "0.0";
        std::string smp;
        bool explicitLod = isVs || hasLoop || useRegLod;
        if (dim == 3) {
            B += " float3 uv=cubedir(" + uvw + ");\n";
            smp = explicitLod ? S("TC[%u].SampleLevel(SS[%u],uv,%s)", slot, slot, lod.c_str()) : S("TC[%u].Sample(SS[%u],uv)", slot, slot);
        } else {
            B += " float2 uv=" + uvw + ".xy;\n";
            if (dim == 0) B += " uv.y=0.5;\n";
            if (denorm) B += S(" uv*=tfi[%u].xy;\n", slot);
            else B += S(" uv*=tfscale(%u);\n", slot);
            if (ox != 0 || oy != 0) B += S(" uv+=float2(%g,%g)*tfi[%u].xy;\n", ox, oy, slot);
            if (explicitLod) smp = S("T2[%u].SampleLevel(SS[%u],uv,%s)", slot, slot, lod.c_str());
            else if (bias) smp = S("T2[%u].SampleBias(SS[%u],uv,%g)", slot, slot, bias / 16.0f);
            else smp = S("T2[%u].Sample(SS[%u],uv)", slot, slot);
        }
        if (op == 17) {                                                     // getCompTexLOD
            if (dim == 3 || explicitLod) B += " float4 t=0;\n";
            else B += S(" float4 t=T2[%u].CalculateLevelOfDetail(SS[%u],uv).xxxx;\n", slot, slot);
            if (useCompLod) {}
        } else {
            B += " float4 t=texfix(" + smp + S(",%u);\n", slot);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t s = (dsw >> (3 * i)) & 7;
            if (s == 7) continue;
            std::string val = s < 4 ? S("t.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
            B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
        }
        B += "}\n";
    }

    void emit_exec(uint32_t addr, uint32_t count, uint32_t seq) {
        for (uint32_t k = 0; k < count; k++, seq >>= 2) {
            uint32_t off = (addr + k) * 3;
            if (off + 3 > words) { logf_once("execoob", "exec beyond shader end"); return; }
            const uint32_t* d = ucode + off;
            if (seq & 1) {
                uint32_t op = d[0] & 0x1F;
                if (op == 0) emit_vfetch(d); else emit_tfetch(d);
            } else emit_alu(d);
        }
    }

    struct Cf { uint32_t w0, w1, op; };
    std::vector<Cf> cfs;

    void decode_cf() {
        uint32_t bound = words / 3;
        for (uint32_t i = 0; i < bound && (i + 1) * 3 <= words; i++) {
            const uint32_t* t = ucode + i * 3;
            Cf a{ t[0], t[1] & 0xFFFF, 0 }, b{ (t[1] >> 16) | (t[2] << 16), t[2] >> 16, 0 };
            a.op = a.w1 >> 12; b.op = b.w1 >> 12;
            for (Cf* c : { &a, &b }) {
                cfs.push_back(*c);
                if ((c->op >= 1 && c->op <= 6) || c->op == 13 || c->op == 14) bound = std::min(bound, c->w0 & 0xFFF);
            }
        }
    }

    void walk() {
        // Is this straight-line code? (no jumps, calls, or loops)
        bool jumps = false;
        for (auto& c : cfs) if (c.op == 9 || c.op == 11 || c.op == 7) jumps = true;
        for (auto& c : cfs) if (c.op == 7) hasLoop = true;
        body.clear();
        std::string& B = body;
        std::vector<std::pair<uint32_t, std::string>> loopEnds;
        for (uint32_t i = 0; i < cfs.size(); i++) {
            const Cf& c = cfs[i];
            uint32_t w0 = c.w0, w1 = c.w1, op = c.op;
            std::string guard = jumps ? S("if (pc <= %u) ", i) : "";
            auto boolCond = [&](uint32_t b, bool cond) {
                return S("(((bools[%u].%c>>%u)&1)==%uu)", b >> 7, comp((b >> 5) & 3), b & 31, cond ? 1 : 0);
            };
            switch (op) {
              case 0: case 12: case 15: break;                              // nop, alloc, mark fetch done
              case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {
                uint32_t addr = w0 & 0xFFF, cnt = (w0 >> 12) & 7, seq = (w0 >> 16) & 0xFFF;
                bool isEnd = op == 2 || op == 4 || op == 6 || op == 14;
                std::string cond;
                if (op == 3 || op == 4 || op == 13 || op == 14) cond = boolCond((w1 >> 2) & 0xFF, (w1 >> 10) & 1);
                else if (op == 5 || op == 6) cond = S("(p==%s)", ((w1 >> 10) & 1) ? "true" : "false");
                B += guard + "{\n";
                if (!cond.empty()) B += "if " + cond + " {\n";
                emit_exec(addr, cnt, seq);
                if (!cond.empty()) B += "}\n";
                if (isEnd && jumps) B += "pc=0x7FFFFFFF;\n";
                B += "}\n";
                if (isEnd && !jumps) goto done;
                break; }
              case 7: {                                                     // loop start
                uint32_t id = (w0 >> 16) & 31;
                int u = uniq++;
                B += guard + S("{}\n{ uint lc=loops[%u].%c; int lcnt=lc&255; aL=(int)(lc<<16)>>24; int lstep=(int)(lc<<8)>>24; int li%d=0;\n"
                               "[loop] while(li%d<lcnt) {\n", id >> 2, comp(id & 3), u, u);
                loopEnds.push_back({ i, S("li%d++; aL+=lstep; if(li%d>=lcnt) break;\n}\n}\n", u, u) });
                break; }
              case 8: {                                                     // loop end
                bool brk = (w0 >> 21) & 1, cnd = (w1 >> 10) & 1;
                if (brk) B += S("if (p==%s) break;\n", cnd ? "true" : "false");
                if (!loopEnds.empty()) { B += loopEnds.back().second; loopEnds.pop_back(); }
                break; }
              case 9: case 10:
                logf_once("cfcall", "shader uses call/return: not implemented");
                break;
              case 11: {                                                    // cond jump
                uint32_t target = w0 & 0x1FFF;
                bool uncond = (w0 >> 13) & 1, predd = (w0 >> 14) & 1;
                std::string cond;
                if (uncond) cond = "(true)";
                else if (predd) cond = S("(p==%s)", ((w1 >> 10) & 1) ? "true" : "false");
                else cond = boolCond((w1 >> 2) & 0xFF, (w1 >> 10) & 1);
                if (target <= i) logf_once("backjmp", "backward jump in shader: not implemented");
                B += guard + "if " + cond + S(" pc=%u;\n", target);
                break; }
              default: break;
            }
        }
    done:
        while (!loopEnds.empty()) { B += loopEnds.back().second; loopEnds.pop_back(); }
    }

    std::string decls() {
        std::string d;
        if (useArray) d += " float4 r[64]; for(int ri=0;ri<64;ri++) r[ri]=0;\n";
        else for (int n : usedRegs) d += S(" float4 r%d=0;\n", n);
        d += " bool p=false; int a0=0; int aL=0; float ps=0.0; float lodReg=0.0; int pc=0;\n";
        return d;
    }
};

static std::string uv_helper() {
    return "#define tfscale(n) tfp[n].xy\n";     // normalised lookups multiply by the uv scale; set per bind
}

static XShader* build_shader(const std::vector<uint32_t>& code, bool isVs, uint32_t paramGen) {
    static std::unordered_map<uint64_t, XShader*> cache;
    uint64_t h = fnv64(code.data(), code.size() * 4, isVs ? 0x1234 : 0x5678);
    h = fnv64(&paramGen, 4, h);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
    XShader* sh = new XShader();
    sh->hash = h;
    cache[h] = sh;
    Translator T; T.ucode = code.data(); T.words = (uint32_t)code.size(); T.isVs = isVs; T.out = sh;
    T.decode_cf();
    T.walk();
    if (T.needRel) { T.useArray = true; T.usedRegs.clear(); T.needRel = false; memset(T.prevFull, 0, sizeof(T.prevFull)); T.walk(); }
    std::string src = std::string(kCommon) + uv_helper() + kVOut;
    if (isVs) {
        // VTE: optionally divide-undo, then map guest screen space onto the full host render target
        src += "VOut main(uint vid : SV_VertexID) {\n float4 oI[16]; for(int i=0;i<16;i++) oI[i]=0; float4 oPos=float4(0,0,0,1), oPts=0;\n";
        src += T.decls();
        if (T.useArray) src += " r[0].x=(float)((int)vid+(int)vpW.y);\n";
        else if (T.usedRegs.count(0)) src += " r0.x=(float)((int)vid+(int)vpW.y);\n";
        src += T.body;
        src += " VOut o; for(int i=0;i<16;i++) o.o[i]=oI[i];\n"
               " float w=oPos.w; if(vpW.x==0.0) w=1.0/w;\n"
               " float2 xy=oPos.xy; if(vpZ.z!=0.0) xy*=w;\n"
               " float z=oPos.z; if(vpZ.w!=0.0) z*=w;\n"
               // precise: separate shaders drawing the same mesh must produce identical z for depth EQUAL passes
               " precise float4 finalPos=float4(xy*vpNdc.xy+w*vpNdc.zw, z*vpZ.x+w*vpZ.y, w);\n o.pos=finalPos;\n"
               " return o; }\n";
    } else {
        src += "struct POut { float4 c0:SV_Target0; float4 c1:SV_Target1; float4 c2:SV_Target2; float4 c3:SV_Target3;";
        if (T.writesDepth) src += " float d:SV_Depth;";
        src += " };\nPOut main(VOut In) {\n float4 oC[4]; for(int i=0;i<4;i++) oC[i]=float4(0,0,0,0); float oDepth=In.pos.z;\n";
        // Interpolators arrive in r0..r15. Registers used only as temps get harmless initial values.
        std::string d = T.decls();
        src += d;
        for (int i = 0; i < 16; i++) {
            if (T.useArray) src += S(" r[%d]=In.o[%d];\n", i, i);
            else if (T.usedRegs.count(i)) src += S(" r%d=In.o[%d];\n", i, i);
        }
        if (paramGen < 16) {
            if (T.useArray) src += S(" r[%u]=float4(In.pos.xy,0,0);\n", paramGen);
            else if (T.usedRegs.count(paramGen)) src += S(" r%u=float4(In.pos.xy,0,0);\n", paramGen);
        }
        src += T.body;
        src += " uint af=(uint)alpha.x; if(af!=7){ float aa=oC[0].a, rf=alpha.y; bool atok=true;\n"
               "  if(af==0) atok=false; else if(af==1) atok=aa<rf; else if(af==2) atok=aa==rf; else if(af==3) atok=aa<=rf;\n"
               "  else if(af==4) atok=aa>rf; else if(af==5) atok=aa!=rf; else if(af==6) atok=aa>=rf; if(!atok) discard; }\n";
        // 8_8_8_8_GAMMA targets store piecewise-linear gamma (colour only). Inverse of the ramp in texfix.
        src += " if(alpha.z!=0.0){ float3 lc=saturate(oC[0].rgb);\n"
               "  oC[0].rgb=(lc<0.0625)?lc*4.0:((lc<0.125)?lc*2.0+0.125:((lc<0.5)?lc+0.25:lc*0.5+0.5)); }\n";
        // BCR2_DBGPS="<ps hash>:<hlsl float4 expr>" replaces that pixel shader's first output, to view one input at a time
        if (const char* dbg = getenv("BCR2_DBGPS")) {
            std::string ds = dbg, hs = S("%016llX:", (unsigned long long)h);
            if (ds.rfind(hs, 0) == 0) { src += " oC[0]=" + ds.substr(hs.size()) + ";\n"; fprintf(stderr, "[gfx] debug override on ps %s\n", hs.c_str()); }
        }
        src += " POut o; o.c0=oC[0]; o.c1=oC[1]; o.c2=oC[2]; o.c3=oC[3];";
        if (T.writesDepth) src += " o.d=oDepth;";
        src += " return o; }\n";
    }
    if (const char* dd = getenv("BCR2_SHADERDUMP")) {
        std::string p = std::string(dd) + S("\\%s_%016llX.hlsl", isVs ? "vs" : "ps", (unsigned long long)h);
        if (FILE* f = fopen(p.c_str(), "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    for (auto& kv : T.texs) sh->tex.push_back(kv.second);
    for (auto& kv : T.vfs) sh->vf.push_back({ kv.first, kv.second });
    auto blob = compile_cached(src, "main", isVs ? "vs_5_0" : "ps_5_0");
    if (blob) {
        if (isVs) sh->ok = SUCCEEDED(g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sh->vs));
        else sh->ok = SUCCEEDED(g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sh->ps));
    }
    static int total = 0, bad = 0;
    total++; if (!sh->ok) bad++;
    if (total <= 3 || (total % 50) == 0 || !sh->ok) fprintf(stderr, "[gfx] shaders translated=%d failed=%d (last %s %u dwords, %zu tex, %zu vf)\n", total, bad, isVs ? "vs" : "ps", T.words, sh->tex.size(), sh->vf.size());
    return sh;
}

static inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---- render targets ----
struct Rt {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv, stencilSrv;
    uint32_t w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint64_t seq = 0;                                                       // write order, for colour/depth EDRAM aliasing
};
static std::unordered_map<uint64_t, Rt*> g_rts;
static uint64_t g_rtSeq = 0;

static DXGI_FORMAT rt_dxgi(uint32_t f) {
    switch (f) {
      case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
      case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
      case 3: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case 4: return DXGI_FORMAT_R16G16_UNORM;
      case 5: return DXGI_FORMAT_R16G16B16A16_UNORM;
      case 6: return DXGI_FORMAT_R16G16_FLOAT;
      case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case 14: return DXGI_FORMAT_R32_FLOAT;
      case 15: return DXGI_FORMAT_R32G32_FLOAT;
    }
    logf_once(S("rtf%u", f).c_str(), "unknown colour target format %u", f);
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}
// Host targets are as tall as the pitch could reach inside 10 MB of EDRAM at 32 bpp, since the height is never stated.
static void rt_size(uint32_t pitch, uint32_t& w, uint32_t& h) { w = std::max(pitch, 16u); h = std::clamp(align_up(2621440u / w, 8), 720u, 8192u); }

static Rt* get_color_rt(uint32_t base, uint32_t fmt, uint32_t pitch) {
    uint64_t key = ((uint64_t)base << 32) | ((uint64_t)fmt << 16) | (pitch & 0xFFFF);
    auto it = g_rts.find(key);
    if (it != g_rts.end()) return it->second;
    Rt* r = new Rt(); rt_size(pitch, r->w, r->h); r->fmt = rt_dxgi(fmt);
    D3D11_TEXTURE2D_DESC d{}; d.Width = r->w; d.Height = r->h; d.MipLevels = 1; d.ArraySize = 1; d.Format = r->fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &r->tex);
    g_dev->CreateRenderTargetView(r->tex.Get(), nullptr, &r->rtv);
    g_dev->CreateShaderResourceView(r->tex.Get(), nullptr, &r->srv);
    float z[4] = { 0, 0, 0, 0 }; g_ctx->ClearRenderTargetView(r->rtv.Get(), z);
    fprintf(stderr, "[gfx] colour target base=%X fmt=%u pitch=%u -> %ux%u\n", base, fmt, pitch, r->w, r->h);
    return g_rts[key] = r;
}
static Rt* get_depth_rt(uint32_t base, uint32_t pitch) {
    uint64_t key = ((uint64_t)base << 32) | (0xFFFFull << 16) | (pitch & 0xFFFF);
    auto it = g_rts.find(key);
    if (it != g_rts.end()) return it->second;
    Rt* r = new Rt(); rt_size(pitch, r->w, r->h); r->fmt = DXGI_FORMAT_R24G8_TYPELESS;
    D3D11_TEXTURE2D_DESC d{}; d.Width = r->w; d.Height = r->h; d.MipLevels = 1; d.ArraySize = 1; d.Format = r->fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &r->tex);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_dev->CreateDepthStencilView(r->tex.Get(), &dv, &r->dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r->tex.Get(), &sv, &r->srv);
    sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    g_dev->CreateShaderResourceView(r->tex.Get(), &sv, &r->stencilSrv);
    g_ctx->ClearDepthStencilView(r->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    fprintf(stderr, "[gfx] depth target base=%X pitch=%u -> %ux%u\n", base, pitch, r->w, r->h);
    return g_rts[key] = r;
}

// ---- resolved surfaces, keyed by guest address ----
struct Surface {
    uint32_t base = 0, end = 0, w = 0, h = 0, pitch = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv;
    // Depth surfaces also keep the bytes guest memory would hold (D24 << 8 | S8), because shaders read depth as k_8_8_8_8.
    ComPtr<ID3D11Texture2D> rawTex; ComPtr<ID3D11RenderTargetView> rawRtv; ComPtr<ID3D11ShaderResourceView> rawSrv;
    uint64_t guestHash = 0;
    bool swapped = false;
};
static std::map<uint32_t, Surface*> g_surfaces;

static uint64_t guest_sample_hash(uint32_t addr, uint32_t size) {
    // sparse: enough to notice the guest overwriting a resolve destination with new CPU data
    uint64_t h = 0;
    uint32_t step = std::max(size / 64u, 256u) & ~7u;
    for (uint32_t o = 0; o + 8 <= size; o += step) { uint64_t v; memcpy(&v, phys(addr + o), 8); h = (h ^ v) * 0x100000001B3ull; }
    return h;
}

static Surface* make_surface(uint32_t base, uint32_t pitch, uint32_t height, uint32_t bytes, DXGI_FORMAT fmt) {
    auto it = g_surfaces.find(base);
    if (it != g_surfaces.end()) { delete it->second; g_surfaces.erase(it); }
    Surface* s = new Surface(); s->base = base; s->end = base + bytes; s->w = pitch; s->h = height; s->pitch = pitch; s->fmt = fmt;
    D3D11_TEXTURE2D_DESC d{}; d.Width = pitch; d.Height = height; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &s->tex);
    if (!s->tex) { delete s; return nullptr; }
    g_dev->CreateRenderTargetView(s->tex.Get(), nullptr, &s->rtv);
    g_dev->CreateShaderResourceView(s->tex.Get(), nullptr, &s->srv);
    if (fmt == DXGI_FORMAT_R32_FLOAT) {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dev->CreateTexture2D(&d, nullptr, &s->rawTex);
        g_dev->CreateRenderTargetView(s->rawTex.Get(), nullptr, &s->rawRtv);
        g_dev->CreateShaderResourceView(s->rawTex.Get(), nullptr, &s->rawSrv);
    }
    fprintf(stderr, "[gfx] resolve surface base=%X %ux%u fmt=%d\n", base, pitch, height, (int)fmt);
    g_surfaces[base] = s;
    s->guestHash = guest_sample_hash(base, bytes);
    return s;
}
static Surface* surface_at(uint32_t addr) {
    auto it = g_surfaces.upper_bound(addr);
    if (it == g_surfaces.begin()) return nullptr;
    --it;
    return addr < it->second->end ? it->second : nullptr;
}

// ---- tiling ----
static inline uint32_t tiled2d(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bpbLog2) {
    uint32_t outer = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oib = (outer | inner) << bpbLog2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1), ylsb = y & 1;
    return (ylsb << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
static inline uint32_t log2_ceil(uint32_t v) { uint32_t n = 0; while ((1u << n) < v) n++; return n; }

// ---- textures ----
struct TexFmt { uint32_t bpb, block; DXGI_FORMAT dxgi; int conv; };   // conv: 0 direct, otherwise unpack to RGBA8
static bool tex_fmt(uint32_t f, TexFmt& o) {
    switch (f) {
      case 2: case 8: case 9: case 39: o = { 1, 1, DXGI_FORMAT_R8_UNORM, 0 }; return true;
      case 3: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1 }; return true;
      case 4: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 2 }; return true;
      case 5: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 3 }; return true;
      case 6: case 14: case 50: o = { 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0 }; return true;
      case 7: case 54: o = { 4, 1, DXGI_FORMAT_R10G10B10A2_UNORM, 0 }; return true;
      case 10: case 40: o = { 2, 1, DXGI_FORMAT_R8G8_UNORM, 0 }; return true;
      case 13: case 25: o = { 4, 1, DXGI_FORMAT_R16G16_UNORM, 0 }; return true;
      case 15: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4 }; return true;
      case 16: case 55: o = { 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 5 }; return true;
      case 17: case 56: o = { 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 6 }; return true;
      case 18: case 51: o = { 8, 4, DXGI_FORMAT_BC1_UNORM, 0 }; return true;
      case 19: case 52: o = { 16, 4, DXGI_FORMAT_BC2_UNORM, 0 }; return true;
      case 20: case 53: o = { 16, 4, DXGI_FORMAT_BC3_UNORM, 0 }; return true;
      case 21: case 26: o = { 8, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 0 }; return true;
      case 22: case 23: case 33: case 36: o = { 4, 1, DXGI_FORMAT_R32_FLOAT, 0 }; return true;
      case 24: o = { 2, 1, DXGI_FORMAT_R16_UNORM, 0 }; return true;
      case 30: o = { 2, 1, DXGI_FORMAT_R16_FLOAT, 0 }; return true;
      case 31: o = { 4, 1, DXGI_FORMAT_R16G16_FLOAT, 0 }; return true;
      case 32: o = { 8, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0 }; return true;
      case 37: o = { 8, 1, DXGI_FORMAT_R32G32_FLOAT, 0 }; return true;
      case 38: o = { 16, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 }; return true;
      case 49: o = { 16, 4, DXGI_FORMAT_BC5_UNORM, 0 }; return true;
      case 59: o = { 8, 4, DXGI_FORMAT_BC4_UNORM, 0 }; return true;
    }
    return false;
}
static inline void swap_block(uint8_t* p, uint32_t n, uint32_t endian) {
    switch (endian) {
      case 1: for (uint32_t i = 0; i + 2 <= n; i += 2) std::swap(p[i], p[i + 1]); break;
      case 2: for (uint32_t i = 0; i + 4 <= n; i += 4) *(uint32_t*)(p + i) = _byteswap_ulong(*(uint32_t*)(p + i)); break;
      case 3: for (uint32_t i = 0; i + 4 <= n; i += 4) { uint32_t v = *(uint32_t*)(p + i); *(uint32_t*)(p + i) = (v >> 16) | (v << 16); } break;
    }
}
static void unpack_rgba8(int conv, const uint8_t* s, uint8_t* d) {
    auto s5 = [](uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); };
    auto s6 = [](uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); };
    auto s4 = [](uint32_t v) { return (uint8_t)((v << 4) | v); };
    uint32_t v = conv >= 5 ? *(const uint32_t*)s : *(const uint16_t*)s;
    switch (conv) {
      case 1: d[0] = s5(v & 31); d[1] = s5((v >> 5) & 31); d[2] = s5((v >> 10) & 31); d[3] = (v >> 15) ? 255 : 0; break;
      case 2: d[0] = s5(v & 31); d[1] = s6((v >> 5) & 63); d[2] = s5((v >> 11) & 31); d[3] = 255; break;
      case 3: d[0] = s6(v & 63); d[1] = s5((v >> 6) & 31); d[2] = s5((v >> 11) & 31); d[3] = 255; break;
      case 4: d[0] = s4(v & 15); d[1] = s4((v >> 4) & 15); d[2] = s4((v >> 8) & 15); d[3] = s4((v >> 12) & 15); break;
      case 5: d[0] = (uint8_t)((v & 0x7FF) >> 3); d[1] = (uint8_t)(((v >> 11) & 0x7FF) >> 3); d[2] = (uint8_t)((v >> 22) >> 2); d[3] = 255; break;
      case 6: d[0] = (uint8_t)((v & 0x3FF) >> 2); d[1] = (uint8_t)(((v >> 10) & 0x7FF) >> 3); d[2] = (uint8_t)((v >> 21) >> 3); d[3] = 255; break;
    }
}

struct HostTex {
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv;
    uint64_t hash = 0, lastFrame = ~0ull; uint32_t hostW = 0, hostH = 0, levels = 0;
};
static std::unordered_map<uint64_t, HostTex*> g_texCache;

static uint64_t range_hash(uint32_t addr, uint32_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t o = 0; o + 8 <= size; o += 32) { uint64_t v; memcpy(&v, phys(addr + o), 8); h = (h ^ v) * 0x100000001B3ull; }
    return h;
}

struct FetchInfo { uint32_t base, fmt, w, h, pitch, endian, dim, faces, mipBase, maxLevel; bool tiled, packed; };

// ---- mip chain layout, as the console stores it (same rules as Xenia's texture_util.cc) ----
// Levels 1 and up live at the fetch constant's mip address. Each stores a power-of-two-based pitch and height, tile-aligned, on a 4 KB boundary.
// Once the short side reaches 16 texels, every smaller level shares one tile (the packed tail), each at its own offset.
struct MipStore { uint32_t offset = 0, pitchBlocks = 0, rowBytes = 0, stride = 0; };
static uint32_t next_pow2(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }
static uint32_t packed_mip_level(uint32_t w, uint32_t h) {
    uint32_t l = log2_ceil(std::min(w, h));
    return l <= 4 ? 0 : l - 4;
}
static void packed_mip_offset(uint32_t w, uint32_t h, uint32_t level, uint32_t block, uint32_t& x, uint32_t& y) {
    uint32_t lw = log2_ceil(w), lh = log2_ceil(h), ls = std::min(lw, lh);
    uint32_t base = ls > 4 ? ls - 4 : 0, m = level - base;
    x = y = 0;
    if (m < 3) {
        if (lw > lh) y = 16 >> m; else x = 16 >> m;
    } else {
        if (lw > lh) x = (1u << (lw - base)) >> (m - 2); else y = (1u << (lh - base)) >> (m - 2);
    }
    x /= block; y /= block;
}

static HostTex* get_texture(const FetchInfo& fi) {
    TexFmt tf;
    if (!tex_fmt(fi.fmt, tf)) { logf_once(S("texf%u", fi.fmt).c_str(), "unsupported texture format %u (%ux%u at %X)", fi.fmt, fi.w, fi.h, fi.base); return nullptr; }
    uint32_t blocksW = align_up(fi.w, tf.block) / tf.block, blocksH = align_up(fi.h, tf.block) / tf.block;
    uint32_t pitchBlocks = std::max(fi.pitch ? fi.pitch / tf.block : blocksW, 1u);
    uint32_t bl2 = 0; while ((1u << bl2) < tf.bpb) bl2++;
    uint32_t pitchAligned = align_up(pitchBlocks, 32), rows = align_up(blocksH, 32);
    uint32_t rowBytes = align_up(pitchBlocks * tf.bpb, 256);
    uint32_t sliceBytes = fi.tiled ? align_up(pitchAligned * rows * tf.bpb, 4096) : align_up(rowBytes * blocksH, 4096);
    uint32_t total = sliceBytes * fi.faces;
    // small packed textures keep their base level inside a tail offset
    uint32_t offX = 0, offY = 0;
    if (fi.packed) {
        uint32_t lw = log2_ceil(fi.w), lh = log2_ceil(fi.h), lmin = std::min(lw, lh);
        if (lmin <= 4) { if (lw > lh) offY = 16; else offX = 16; offX /= tf.block; offY /= tf.block; }
    }
    uint64_t key = fnv64(&fi, sizeof(fi));
    auto& slot = g_texCache[key];
    if (!slot) slot = new HostTex();
    HostTex* ht = slot;
    if (g_trace > 1000) {
        static std::set<uint64_t> seen;
        if (seen.insert(key).second && fi.w <= 32 && fi.h <= 32) {                // small textures: show their layout and raw blocks
            fprintf(stderr, "[texdbg] base=%08X fmt=%u %ux%u pitch=%u tiled=%d packed=%d endian=%u off=%u,%u raw@off:", fi.base, fi.fmt, fi.w, fi.h, fi.pitch, (int)fi.tiled, (int)fi.packed, fi.endian, offX, offY);
            uint32_t a0 = fi.tiled ? tiled2d(offX, offY, pitchAligned, bl2) : offY * rowBytes + offX * tf.bpb;
            for (uint32_t k = 0; k < 2; k++) fprintf(stderr, " %08X", *(const uint32_t*)phys(fi.base + a0 + k * 4));
            fprintf(stderr, " raw@0:");
            for (uint32_t k = 0; k < 2; k++) fprintf(stderr, " %08X", *(const uint32_t*)phys(fi.base + k * 4));
            uint32_t nz = 0; for (uint32_t k = 0; k < total; k += 4) if (*(const uint32_t*)phys(fi.base + k)) nz++;
            fprintf(stderr, " nonzero dwords=%u of %u\n", nz, total / 4);
        }
    }
    if (ht->lastFrame == g_frame && ht->srv) return ht;
    ht->lastFrame = g_frame;
    PerfScope perfTex(g_tTex);
    static const bool kNoMips = getenv("BCR2_NOMIPS") != nullptr;           // for comparison: draw every texture from its full-size image only
    const uint32_t maxLevel = kNoMips ? 0 : fi.maxLevel, packedLevel = fi.packed ? packed_mip_level(fi.w, fi.h) : ~0u;
    MipStore store[16];
    uint32_t mipBytes = 0;
    for (uint32_t s = packedLevel == 0 ? 0 : 1; maxLevel && s <= std::min(maxLevel, packedLevel) && s < 16; s++) {
        uint32_t pb = align_up(align_up(std::max(next_pow2(fi.w) >> s, 1u), tf.block) / tf.block, 32);
        uint32_t rb = pb * tf.bpb;
        if (!fi.tiled) rb = align_up(rb, 256);
        uint32_t mrows = align_up(align_up(std::max(next_pow2(fi.h) >> s, 1u), tf.block) / tf.block, 32);
        store[s].offset = mipBytes; store[s].pitchBlocks = pb; store[s].rowBytes = rb; store[s].stride = align_up(rb * mrows, 4096);
        mipBytes += store[s].stride * fi.faces;
    }
    uint64_t h = range_hash(fi.base, total);
    if (mipBytes) h ^= range_hash(fi.mipBase, mipBytes) * 0x9E3779B97F4A7C15ull;
    if (ht->srv && h == ht->hash) return ht;
    ht->hash = h;
    g_nUploads++; g_upBytes += total + mipBytes;
    uint32_t hw = blocksW * tf.block, hh = blocksH * tf.block;
    uint32_t outBpb = tf.conv ? 4 : tf.bpb;
    std::vector<uint8_t> out((size_t)blocksW * blocksH * outBpb * fi.faces);
    uint8_t tmp[16];
    for (uint32_t f = 0; f < fi.faces; f++) {
        uint32_t faceBase = fi.base + sliceBytes * f;
        uint8_t* dstFace = out.data() + (size_t)f * blocksW * blocksH * outBpb;
        for (uint32_t y = 0; y < blocksH; y++) {
            uint8_t* dst = dstFace + (size_t)y * blocksW * outBpb;
            for (uint32_t x = 0; x < blocksW; x++, dst += outBpb) {
                uint32_t a = fi.tiled ? tiled2d(x + offX, y + offY, pitchAligned, bl2) : (y + offY) * rowBytes + (x + offX) * tf.bpb;
                memcpy(tmp, phys(faceBase + a), tf.bpb);
                swap_block(tmp, tf.bpb, fi.endian);
                if (tf.conv) unpack_rgba8(tf.conv, tmp, dst); else memcpy(dst, tmp, tf.bpb);
            }
        }
    }
    const uint32_t levels = maxLevel + 1;
    bool same = ht->tex && ht->hostW == hw && ht->hostH == hh && ht->levels == levels;
    if (!same) {
        ht->tex.Reset(); ht->srv.Reset();
        D3D11_TEXTURE2D_DESC d{}; d.Width = hw; d.Height = hh; d.MipLevels = levels; d.ArraySize = fi.faces; d.Format = tf.dxgi; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (fi.faces == 6) d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &ht->tex);
        if (FAILED(hr)) { logf_once(S("texc%u", fi.fmt).c_str(), "texture creation failed fmt=%u %ux%u hr=%08lX", fi.fmt, hw, hh, hr); return nullptr; }
        g_dev->CreateShaderResourceView(ht->tex.Get(), nullptr, &ht->srv);
        ht->hostW = hw; ht->hostH = hh; ht->levels = levels;
    }
    for (uint32_t f = 0; f < fi.faces; f++)
        g_ctx->UpdateSubresource(ht->tex.Get(), D3D11CalcSubresource(0, f, levels), nullptr, out.data() + (size_t)f * blocksW * blocksH * outBpb,
                                 blocksW * outBpb, blocksW * blocksH * outBpb);
    for (uint32_t l = 1; l <= maxLevel; l++) {
        const uint32_t s = std::min(l, packedLevel);
        const MipStore& ms = store[s];
        uint32_t ox = 0, oy = 0;
        if (l >= packedLevel) packed_mip_offset(fi.w, fi.h, l, tf.block, ox, oy);
        // what the host level holds, and what the guest stores; they differ only when the base size isn't a whole number of blocks
        uint32_t hbw = align_up(std::max(hw >> l, 1u), tf.block) / tf.block, hbh = align_up(std::max(hh >> l, 1u), tf.block) / tf.block;
        uint32_t gbw = std::min(hbw, align_up(std::max(fi.w >> l, 1u), tf.block) / tf.block), gbh = std::min(hbh, align_up(std::max(fi.h >> l, 1u), tf.block) / tf.block);
        std::vector<uint8_t> lvl((size_t)hbw * hbh * outBpb);
        for (uint32_t f = 0; f < fi.faces; f++) {
            uint32_t faceBase = fi.mipBase + ms.offset + ms.stride * f;
            for (uint32_t y = 0; y < gbh; y++) {
                uint8_t* dst = lvl.data() + (size_t)y * hbw * outBpb;
                for (uint32_t x = 0; x < gbw; x++, dst += outBpb) {
                    uint32_t a = fi.tiled ? tiled2d(x + ox, y + oy, ms.pitchBlocks, bl2) : (y + oy) * ms.rowBytes + (x + ox) * tf.bpb;
                    memcpy(tmp, phys(faceBase + a), tf.bpb);
                    swap_block(tmp, tf.bpb, fi.endian);
                    if (tf.conv) unpack_rgba8(tf.conv, tmp, dst); else memcpy(dst, tmp, tf.bpb);
                }
            }
            g_ctx->UpdateSubresource(ht->tex.Get(), D3D11CalcSubresource(l, f, levels), nullptr, lvl.data(), hbw * outBpb, hbw * hbh * outBpb);
        }
    }
    return ht;
}

// ---- pipeline state caches ----
static std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> g_blendStates;
static std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> g_dsStates;
static std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> g_rsStates;
static std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> g_samplers;

static D3D11_BLEND blend_factor(uint32_t f, bool alpha) {
    switch (f) {
      case 0: return D3D11_BLEND_ZERO;
      case 1: return D3D11_BLEND_ONE;
      case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
      case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
      case 6: return D3D11_BLEND_SRC_ALPHA;
      case 7: return D3D11_BLEND_INV_SRC_ALPHA;
      case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
      case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
      case 10: return D3D11_BLEND_DEST_ALPHA;
      case 11: return D3D11_BLEND_INV_DEST_ALPHA;
      case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
      case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
      case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    }
    return D3D11_BLEND_ONE;
}
static D3D11_BLEND_OP blend_op(uint32_t o) {
    switch (o) { case 1: return D3D11_BLEND_OP_SUBTRACT; case 2: return D3D11_BLEND_OP_MIN; case 3: return D3D11_BLEND_OP_MAX; case 4: return D3D11_BLEND_OP_REV_SUBTRACT; }
    return D3D11_BLEND_OP_ADD;
}
static ID3D11BlendState* get_blend(uint32_t mask) {
    uint32_t bc[4] = { g_regs[R_BLEND0], g_regs[R_BLEND1], g_regs[R_BLEND1 + 1], g_regs[R_BLEND1 + 2] };
    uint64_t k = fnv64(bc, sizeof(bc)); k = fnv64(&mask, 4, k);
    auto& e = g_blendStates[k];
    if (e) return e.Get();
    D3D11_BLEND_DESC d{}; d.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 4; i++) {
        auto& t = d.RenderTarget[i];
        uint32_t b = bc[i];
        uint32_t cs = b & 31, co = (b >> 5) & 7, cd = (b >> 8) & 31, as = (b >> 16) & 31, ao = (b >> 21) & 7, ad = (b >> 24) & 31;
        t.BlendEnable = !(cs == 1 && cd == 0 && co == 0 && as == 1 && ad == 0 && ao == 0);
        t.SrcBlend = blend_factor(cs, false); t.DestBlend = blend_factor(cd, false); t.BlendOp = blend_op(co);
        t.SrcBlendAlpha = blend_factor(as, true); t.DestBlendAlpha = blend_factor(ad, true); t.BlendOpAlpha = blend_op(ao);
        if (co == 2 || co == 3) { t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_ONE; }
        if (ao == 2 || ao == 3) { t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_ONE; }
        t.RenderTargetWriteMask = (mask >> (4 * i)) & 0xF;
    }
    g_dev->CreateBlendState(&d, &e);
    return e.Get();
}
static D3D11_STENCIL_OP sten_op(uint32_t o) {
    static const D3D11_STENCIL_OP m[8] = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR_SAT,
                                           D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR };
    return m[o & 7];
}
static D3D11_COMPARISON_FUNC cmp_func(uint32_t f) { return (D3D11_COMPARISON_FUNC)((f & 7) + 1); }   // same order as D3D11, shifted by one
static ID3D11DepthStencilState* get_ds(uint32_t dc, uint32_t sr, uint32_t srbf) {
    uint32_t in[3] = { dc, sr & 0xFFFF00, srbf & 0xFFFF00 };
    uint64_t k = fnv64(in, sizeof(in));
    auto& e = g_dsStates[k];
    if (e) return e.Get();
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = (dc >> 1) & 1;
    d.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = cmp_func((dc >> 4) & 7);
    d.StencilEnable = dc & 1;
    d.StencilReadMask = (sr >> 8) & 0xFF; d.StencilWriteMask = (sr >> 16) & 0xFF;
    d.FrontFace = { sten_op((dc >> 11) & 7), sten_op((dc >> 17) & 7), sten_op((dc >> 14) & 7), cmp_func((dc >> 8) & 7) };
    if ((dc >> 7) & 1) d.BackFace = { sten_op((dc >> 23) & 7), sten_op((dc >> 29) & 7), sten_op((dc >> 26) & 7), cmp_func((dc >> 20) & 7) };
    else d.BackFace = d.FrontFace;
    g_dev->CreateDepthStencilState(&d, &e);
    return e.Get();
}
static ID3D11RasterizerState* get_rs(uint32_t mode, bool rect, bool clipOff, float scale, float offset) {
    uint32_t cull = rect ? 0 : (mode & 3), face = (mode >> 2) & 1;
    uint32_t in[5] = { cull, face, (uint32_t)clipOff, 0, 0 }; memcpy(&in[3], &scale, 4); memcpy(&in[4], &offset, 4);
    uint64_t k = fnv64(in, sizeof(in));
    auto& e = g_rsStates[k];
    if (e) return e.Get();
    D3D11_RASTERIZER_DESC d{}; d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = cull == 1 ? D3D11_CULL_FRONT : cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    static const bool kNoCull = getenv("BCR2_NOCULL") != nullptr;           // debugging: draw both faces
    if (kNoCull) d.CullMode = D3D11_CULL_NONE;
    d.FrontCounterClockwise = face == 0;
    d.DepthClipEnable = !clipOff; d.ScissorEnable = TRUE;
    d.SlopeScaledDepthBias = scale; d.DepthBias = (INT)(offset * 16777216.0f);
    g_dev->CreateRasterizerState(&d, &e);
    return e.Get();
}
static ID3D11SamplerState* get_sampler(const uint32_t* tf, const TexUse& u) {
    uint32_t cx = (tf[0] >> 10) & 7, cy = (tf[0] >> 13) & 7, cz = (tf[0] >> 16) & 7;
    uint32_t mag = u.magf >= 0 ? u.magf : (tf[3] >> 19) & 3, min = u.minf >= 0 ? u.minf : (tf[3] >> 21) & 3, mip = u.mipf >= 0 ? u.mipf : (tf[3] >> 23) & 3;
    uint32_t border = tf[5] & 3;
    // LOD range and bias from the fetch constant. Level 0 is skipped when there's no base image.
    uint32_t minL = (tf[4] >> 2) & 15, maxL = (tf[4] >> 6) & 15;
    if (!(tf[5] >> 12)) minL = maxL = 0;
    else { if (maxL < minL) maxL = minL; if (maxL && !(tf[1] >> 12)) minL = std::max(minL, 1u); }
    if (mip == 2) maxL = minL;                                               // base map: never leave the top level
    int lodBias32 = sext((tf[4] >> 12) & 0x3FF, 10);                         // 5 fractional bits
    uint32_t in[10] = { cx, cy, cz, mag, min, mip, border, minL, maxL, (uint32_t)lodBias32 };
    uint64_t k = fnv64(in, sizeof(in));
    auto& e = g_samplers[k];
    if (e) return e.Get();
    auto am = [](uint32_t c) {
        switch (c) {
          case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
          case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
          case 2: case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
          case 3: case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
          default: return D3D11_TEXTURE_ADDRESS_BORDER;
        }
    };
    D3D11_SAMPLER_DESC d{};
    bool lm = mag == 1, ln = min == 1, lp = mip == 1;
    d.Filter = (D3D11_FILTER)((lp ? 0x1 : 0) | (lm ? 0x4 : 0) | (ln ? 0x10 : 0));
    d.AddressU = am(cx); d.AddressV = am(cy); d.AddressW = am(cz);
    d.ComparisonFunc = D3D11_COMPARISON_NEVER; d.MinLOD = (float)minL; d.MaxLOD = (float)maxL; d.MipLODBias = lodBias32 / 32.0f; d.MaxAnisotropy = 1;
    if (border == 1) d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
    g_dev->CreateSamplerState(&d, &e);
    return e.Get();
}

// ---- draw ----
struct ProbeQ { ComPtr<ID3D11Texture2D> st; ID3D11Resource* res = nullptr; std::string text; };
static std::vector<ProbeQ> g_probeQ;
static void probeq_report() {
    std::map<ID3D11Resource*, uint32_t> last;
    size_t n = 0;
    for (auto& q : g_probeQ) {
        n++;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g_ctx->Map(q.st.Get(), 0, D3D11_MAP_READ, 0, &ms))) continue;
        uint32_t v = *(uint32_t*)ms.pData; g_ctx->Unmap(q.st.Get(), 0);
        auto it = last.find(q.res);
        if (it == last.end() || it->second != v)
            fprintf(stderr, "[probeq] draw#%zu rt=%p %08X -> %08X %s\n", n, q.res, it == last.end() ? 0u : it->second, v, q.text.c_str());
        last[q.res] = v;
    }
    fprintf(stderr, "[probeq] %zu draws read\n", g_probeQ.size());
    g_probeQ.clear();
}
static std::vector<uint32_t> g_vsCode, g_psCode;
static uint32_t g_vsHash = 0, g_psHash = 0;
static XShader* g_vsCur = nullptr; static XShader* g_psCur = nullptr; static uint32_t g_psParamGen = 0xFFFFFFFFu;

struct alignas(16) StateCb {
    uint32_t bools[8]; uint32_t loops[32];
    float vpNdc[4]; float vpZ[4]; float vpW[4]; float alpha[4];
    uint32_t vfo[96];
    int32_t tfsw[32 * 4];
    float tfp[32 * 4];
    uint32_t tfsg[32 * 4];
    float tfi[32 * 4];
};

static uint32_t vb_alloc(uint32_t size, uint8_t*& ptr) {
    size = align_up(size, 16);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (g_noOverwriteSrv) {
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_vbPos + size > kVbRingSize) { g_vbPos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
        g_ctx->Map(g_vbRing.Get(), 0, mode, 0, &ms);
        ptr = (uint8_t*)ms.pData + g_vbPos;
    } else {
        static std::vector<uint8_t> scratch; scratch.resize(size); ptr = scratch.data();
        if (g_vbPos + size > kVbRingSize) g_vbPos = 0;
    }
    uint32_t r = g_vbPos; g_vbPos += size;
    return r;
}
static void vb_commit(uint32_t off, uint32_t size, const uint8_t* ptr) {
    if (g_noOverwriteSrv) { g_ctx->Unmap(g_vbRing.Get(), 0); return; }
    D3D11_BOX box{ off, 0, 0, off + align_up(size, 16), 1, 1 };
    g_ctx->UpdateSubresource(g_vbRing.Get(), 0, &box, ptr, 0, 0);
}
static void upload_cb(ID3D11Buffer* b, const void* p, size_t n) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, p, n); g_ctx->Unmap(b, 0); }
}

static void compute_scissor(int& x0, int& y0, int& x1, int& y1) {
    uint32_t tl = g_regs[R_WINDOW_TL], br = g_regs[R_WINDOW_BR];
    x0 = tl & 0x3FFF; y0 = (tl >> 16) & 0x3FFF; x1 = br & 0x3FFF; y1 = (br >> 16) & 0x3FFF;
    if (!(tl >> 31)) {
        uint32_t wo = g_regs[R_WINDOW_OFFSET];
        int ox = sext(wo & 0x7FFF, 15), oy = sext((wo >> 16) & 0x7FFF, 15);
        x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    }
    uint32_t stl = g_regs[R_SCREEN_TL], sbr = g_regs[R_SCREEN_BR];
    x0 = std::max(x0, sext(stl & 0x7FFF, 15)); y0 = std::max(y0, sext((stl >> 16) & 0x7FFF, 15));
    x1 = std::min(x1, sext(sbr & 0x7FFF, 15)); y1 = std::min(y1, sext((sbr >> 16) & 0x7FFF, 15));
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::max(x1, x0); y1 = std::max(y1, y0);
}

// BCR2_WATCHSURF="hexbase,x,y": during the traced frame, log whenever that pixel of that resolved surface has changed.
static void watch_surface(const char* where) {
    static const char* e = getenv("BCR2_WATCHSURF");
    if (!e || g_trace <= 1000) return;
    uint32_t base = 0, x = 0, y = 0; sscanf(e, "%x,%u,%u", &base, &x, &y);
    auto it = g_surfaces.find(base);
    static uint32_t last = 0xDEADBEEF; static void* lastTex = nullptr;
    if (it == g_surfaces.end()) return;
    Surface* s = it->second;
    D3D11_TEXTURE2D_DESC d; s->tex->GetDesc(&d);
    d.Width = 1; d.Height = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
    g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, s->tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    uint32_t v = *(uint32_t*)ms.pData; g_ctx->Unmap(st.Get(), 0);
    if (v != last || lastTex != s->tex.Get()) { fprintf(stderr, "[watch-surf] %s tex=%p %08X -> %08X\n", where, s->tex.Get(), last, v); last = v; lastTex = s->tex.Get(); }
}

static ID3D11RenderTargetView* g_curRtv[4] = {}; static ID3D11DepthStencilView* g_curDsv = nullptr;
static void invalidate_binds() { memset(g_curRtv, 0, sizeof(g_curRtv)); g_curDsv = nullptr; }

static void issue_draw(uint32_t init, const uint32_t* inlineIdx, uint32_t dmaBase, uint32_t dmaSize);
static void do_resolve();
static void sync_depth_alias(Rt* depth, uint32_t base, uint32_t pitch);

// The game's fast clear: a depth-only rectangle with z always passing and writing, at half pitch with 4x MSAA. It writes a
// constant dword (D24 << 8 | S8) over that part of EDRAM, which also clears any colour target sharing the base. Host targets are
// separate per pitch and format, so apply it to all of them directly. Returns false to fall back to an ordinary draw.
// withColor is the mode=4 form: the same rectangle also writes a colour taken from its vertices, to the bound colour targets.
static bool fast_clear(bool withColor) {
    const uint32_t* vf = &g_regs[R_FETCH];
    if ((vf[0] & 3) != 3) return false;
    uint32_t surf = g_regs[R_SURFACE_INFO], pitch = surf & 0x3FFF, msaa = (surf >> 16) & 3;
    if (!pitch) return false;
    uint32_t hostPitch = pitch << (msaa >= 2 ? 1 : 0);                      // host targets hold pixels, the clear covers samples
    uint32_t vaddr = vf[0] & ~3u, endian = vf[1] & 3;
    uint32_t raw = *(const uint32_t*)phys(vaddr + 8);                       // first vertex: x, y, z
    if (endian == 2) raw = _byteswap_ulong(raw);
    else if (endian == 1) raw = ((raw & 0xFF00FF00u) >> 8) | ((raw & 0x00FF00FFu) << 8);
    else if (endian == 3) raw = (raw >> 16) | (raw << 16);
    float z; memcpy(&z, &raw, 4);
    z = std::clamp(z, 0.0f, 1.0f);
    uint32_t dc = g_regs[R_DEPTHCONTROL], sr = g_regs[R_STENCIL], stencil = 0;
    if ((dc & 1) && ((dc >> 14) & 7) == 2) stencil = sr & 0xFF;             // z-pass op: replace
    uint32_t d24 = std::min((uint32_t)(z * 16777215.0 + 0.5), 0xFFFFFFu), base = g_regs[R_DEPTH_INFO] & 0xFFF;
    uint32_t word = (d24 << 8) | stencil;
    float col[4] = { (word & 255) / 255.0f, ((word >> 8) & 255) / 255.0f, ((word >> 16) & 255) / 255.0f, (word >> 24) / 255.0f };
    ID3D11RenderTargetView* none[1] = { nullptr };
    g_ctx->OMSetRenderTargets(1, none, nullptr);
    invalidate_binds();
    Rt* depth = get_depth_rt(base, hostPitch);
    g_ctx->ClearDepthStencilView(depth->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, z, (UINT8)stencil);
    depth->seq = ++g_rtSeq;
    for (uint32_t f : { 0u, 1u }) {
        uint64_t key = ((uint64_t)base << 32) | ((uint64_t)f << 16) | (hostPitch & 0xFFFF);
        auto it = g_rts.find(key);
        if (it != g_rts.end()) { g_ctx->ClearRenderTargetView(it->second->rtv.Get(), col); it->second->seq = g_rtSeq - 1; }
    }
    if (withColor) {
        float vc[4];
        for (int i = 0; i < 4; i++) {
            uint32_t w = *(const uint32_t*)phys(vaddr + 12 + i * 4);
            if (endian == 2) w = _byteswap_ulong(w);
            else if (endian == 1) w = ((w & 0xFF00FF00u) >> 8) | ((w & 0x00FF00FFu) << 8);
            else if (endian == 3) w = (w >> 16) | (w << 16);
            memcpy(&vc[i], &w, 4);
        }
        static const uint32_t regs[4] = { R_COLOR_INFO, R_COLOR1_INFO, R_COLOR2_INFO, R_COLOR3_INFO };
        uint32_t mask = g_regs[R_COLOR_MASK];
        for (int i = 0; i < 4; i++) {
            if (((mask >> (4 * i)) & 0xF) == 0) continue;
            uint32_t ci = g_regs[regs[i]];
            uint64_t key = ((uint64_t)(ci & 0xFFF) << 32) | ((uint64_t)((ci >> 16) & 0xF) << 16) | (hostPitch & 0xFFFF);
            if (i == 0 || g_rts.count(key)) {                               // later slots may hold stale registers: only touch known targets
                Rt* r = get_color_rt(ci & 0xFFF, (ci >> 16) & 0xF, hostPitch);
                g_ctx->ClearRenderTargetView(r->rtv.Get(), vc);
                r->seq = g_rtSeq - 1;
            }
        }
    }
    static int nl = 0;
    if (nl++ < 6 || g_trace > 0) fprintf(stderr, "[gfx] fast clear base=%X pitch=%u(%u) z=%g stencil=%u word=%08X\n", base, pitch, hostPitch, z, stencil, word);
    return true;
}

static void issue_draw(uint32_t init, const uint32_t* inlineIdx, uint32_t dmaBase, uint32_t dmaSize) {
    uint32_t prim = init & 63, srcSel = (init >> 6) & 3, idx32 = (init >> 11) & 1, num = init >> 16;
    uint32_t edram = g_regs[R_MODECONTROL] & 7;
    if (g_trace > 0) {
        g_trace--;
        fprintf(stderr, "[gfx] draw prim=%u src=%u num=%u mode=%u surf=%08X col=%08X dep=%08X pred win=%08X sc=%08X/%08X mask=%08X dc=%08X vte=%08X vs=%016llX ps=%016llX\n",
                prim, srcSel, num, edram, g_regs[R_SURFACE_INFO], g_regs[R_COLOR_INFO], g_regs[R_DEPTH_INFO], g_regs[R_WINDOW_OFFSET], g_regs[R_WINDOW_TL], g_regs[R_WINDOW_BR],
                g_regs[R_COLOR_MASK], g_regs[R_DEPTHCONTROL], g_regs[R_VTE_CNTL], (unsigned long long)g_vsHash, (unsigned long long)g_psHash);
    }
    watch_surface("before draw/resolve");
    if (edram == 6) { PerfScope perfRes(g_tResolve); do_resolve(); watch_surface("after resolve"); return; }
    PerfScope perfDraw(g_tDraw);
    if (edram == 0 || num == 0 || prim == 0) return;
    if (prim >= 0x10) { logf_once(S("prim%u", prim).c_str(), "unsupported primitive type %u", prim); return; }
    if (prim == 1) logf_once("points", "point list drawn as 1-pixel points (no sprite expansion yet)");
    if (g_vsCode.empty()) return;
    g_draws++; g_drawsFrame++;
    // Stencil-only form of the same trick: a half-pitch 4x MSAA rectangle, no depth test, stencil always/replace. Host targets
    // are per pitch, so apply it to the real depth buffer. Without this, marks from an earlier pass survive into the composite.
    {
        uint32_t dcS = g_regs[R_DEPTHCONTROL], surfS = g_regs[R_SURFACE_INFO];
        if (prim == 8 && srcSel == 2 && edram == 5 && ((surfS >> 16) & 3) >= 2 && (surfS & 0x3FFF) != 0 && (dcS & 3) == 1 &&
            ((dcS >> 8) & 7) == 7 && ((dcS >> 14) & 7) == 2) {
            uint32_t base = g_regs[R_DEPTH_INFO] & 0xFFF, hostPitch = (surfS & 0x3FFF) << 1;
            Rt* d = get_depth_rt(base, hostPitch);
            sync_depth_alias(d, base, hostPitch);
            ID3D11RenderTargetView* none[1] = { nullptr };
            g_ctx->OMSetRenderTargets(1, none, nullptr);
            invalidate_binds();
            g_ctx->ClearDepthStencilView(d->dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, (UINT8)(g_regs[R_STENCIL] & 0xFF));
            static int nl = 0;
            if (nl++ < 4 || g_trace > 0) fprintf(stderr, "[gfx] stencil clear base=%X pitch=%u ref=%u\n", base, hostPitch, g_regs[R_STENCIL] & 0xFF);
            return;
        }
    }
    if (prim == 8 && srcSel == 2 && (g_regs[R_DEPTHCONTROL] & 0x76) == 0x76 &&
        (edram == 5 || (edram == 4 && ((g_regs[R_SURFACE_INFO] >> 16) & 3) != 0)) && fast_clear(edram == 4)) return;

    XShader* vs = build_shader(g_vsCode, true, 0xFFFFFFFFu);
    uint32_t misc = g_regs[R_CONTEXT_MISC], pcntl = g_regs[R_PROGRAM_CNTL];
    uint32_t paramGen = ((pcntl >> 18) & 1) ? ((misc >> 8) & 0xFF) : 0xFFFFFFFFu;
    bool depthOnly = edram == 5;
    XShader* ps = g_psCode.empty() ? nullptr : build_shader(g_psCode, false, paramGen);
    if (g_trace > 0) {
        fprintf(stderr, "[gfx]   shaders vs=%016llX ok=%d ps=%016llX ok=%d vte=%08X mode=%08X blend0=%08X cc=%08X psconst=%08X tf:", (unsigned long long)vs->hash, (int)vs->ok,
                ps ? (unsigned long long)ps->hash : 0ull, ps ? (int)ps->ok : -1, g_regs[R_VTE_CNTL], g_regs[R_MODE_CNTL], g_regs[R_BLEND0], g_regs[R_COLORCONTROL], g_regs[0x2308]);
        if (ps) for (auto& u : ps->tex) { const uint32_t* t = &g_regs[R_FETCH + u.slot * 6]; fprintf(stderr, " %u:fmt=%u,base=%08X,%ux%u,sw=%03X,sg=%02X", u.slot, t[1] & 63, t[1] & 0xFFFFF000u, (t[2] & 0x1FFF) + 1, ((t[2] >> 13) & 0x1FFF) + 1, (t[3] >> 1) & 0xFFF, (t[0] >> 2) & 0xFF); }
        fprintf(stderr, " c509=%g,%g,%g,%g c256=%g,%g,%g,%g c267=%g,%g,%g,%g colfmt=%u", reg_f(R_CONST + 509 * 4), reg_f(R_CONST + 509 * 4 + 1), reg_f(R_CONST + 509 * 4 + 2), reg_f(R_CONST + 509 * 4 + 3),
                reg_f(R_CONST + 256 * 4), reg_f(R_CONST + 256 * 4 + 1), reg_f(R_CONST + 256 * 4 + 2), reg_f(R_CONST + 256 * 4 + 3),
                reg_f(R_CONST + 267 * 4), reg_f(R_CONST + 267 * 4 + 1), reg_f(R_CONST + 267 * 4 + 2), reg_f(R_CONST + 267 * 4 + 3), (g_regs[R_COLOR_INFO] >> 16) & 0xF);
        static const char* traceC = getenv("BCR2_TRACEC");                  // hash of a pixel shader: also print its constants
        if (traceC && ps && S("%016llX", (unsigned long long)ps->hash) == traceC) {
            fprintf(stderr, " PC:");
            for (int k = 256; k < 276; k++) fprintf(stderr, " c%d=%g,%g,%g,%g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
            for (int k = 508; k < 512; k++) fprintf(stderr, " c%d=%g,%g,%g,%g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
        }
        fprintf(stderr, " vsconst=%08X c0=%g,%g,%g,%g c4=%g,%g,%g,%g zs=%g zo=%g clip=%08X", g_regs[0x2307], reg_f(R_CONST), reg_f(R_CONST + 1), reg_f(R_CONST + 2), reg_f(R_CONST + 3),
                reg_f(R_CONST + 16), reg_f(R_CONST + 17), reg_f(R_CONST + 18), reg_f(R_CONST + 19), reg_f(R_VPORT_XSCALE + 4), reg_f(R_VPORT_XSCALE + 5), g_regs[R_CLIP_CNTL]);
        fprintf(stderr, " vf:");
        for (auto& u : vs->vf) { uint32_t f0 = g_regs[R_FETCH + u.slot * 2], f1 = g_regs[R_FETCH + u.slot * 2 + 1]; fprintf(stderr, " %u:addr=%08X,words=%u,end=%u,stride=%u", u.slot, f0 & ~3u, (f1 >> 2) & 0xFFFFFF, f1 & 3, u.stride); }
        fprintf(stderr, "\n");
    }
    if (!vs->ok) return;
    if (ps && !ps->ok) ps = nullptr;

    // ---- render targets ----
    uint32_t surf = g_regs[R_SURFACE_INFO], pitch = surf & 0x3FFF;
    if (pitch == 0) { logf_once("pitch0", "draw with surface pitch 0 ignored"); return; }
    uint32_t mask = g_regs[R_COLOR_MASK];
    ID3D11RenderTargetView* rtvs[4] = {};
    uint32_t rw = 0, rh = 0;
    if (!depthOnly) {
        static const uint32_t regs[4] = { R_COLOR_INFO, R_COLOR1_INFO, R_COLOR2_INFO, R_COLOR3_INFO };
        for (int i = 0; i < 4; i++) {
            if (((mask >> (4 * i)) & 0xF) == 0) continue;
            uint32_t ci = g_regs[regs[i]];
            Rt* r = get_color_rt(ci & 0xFFF, (ci >> 16) & 0xF, pitch);
            // Slots can share an EDRAM base. D3D11 rejects a view bound twice and silently keeps the previous targets,
            // which sent such draws into whatever surface the last resolve had bound. Bind each target once.
            bool dup = false;
            for (int k = 0; k < i; k++) if (rtvs[k] == r->rtv.Get()) dup = true;
            if (!dup) rtvs[i] = r->rtv.Get();
            rw = r->w; rh = r->h;
            r->seq = ++g_rtSeq;
        }
    }
    uint32_t dc = g_regs[R_DEPTHCONTROL];
    ID3D11DepthStencilView* dsv = nullptr;
    if (dc & 3) {
        uint32_t depthBase = g_regs[R_DEPTH_INFO] & 0xFFF;
        Rt* d = get_depth_rt(depthBase, pitch);
        sync_depth_alias(d, depthBase, pitch);
        if (dc & 4) d->seq = ++g_rtSeq;                                     // writes depth from here on
        dsv = d->dsv.Get(); rw = d->w; rh = d->h;
    }
    if (!rw) return;                                                        // nothing would be written
    if (memcmp(rtvs, g_curRtv, sizeof(rtvs)) || dsv != g_curDsv) {
        ID3D11ShaderResourceView* nul[32] = {};
        g_ctx->PSSetShaderResources(0, 32, nul);
        g_ctx->OMSetRenderTargets(4, rtvs, dsv);
        memcpy(g_curRtv, rtvs, sizeof(rtvs)); g_curDsv = dsv;
    }

    // ---- indices ----
    static std::vector<uint32_t> idx;
    idx.clear();
    uint32_t mode = g_regs[R_MODE_CNTL];
    bool hasIdx = srcSel != 2;
    uint32_t reset = g_regs[R_RESET_INDX];
    bool resetOn = (mode >> 21) & 1;
    uint32_t minI = 0xFFFFFFFFu, maxI = 0;
    if (hasIdx) {
        idx.resize(num);
        uint32_t endian = dmaSize >> 30;
        if (srcSel == 0) {
            const uint8_t* src = phys(idx32 ? (dmaBase & ~3u) : (dmaBase & ~1u));
            if (idx32) for (uint32_t i = 0; i < num; i++) {
                uint32_t v = ((const uint32_t*)src)[i];
                if (endian == 1) v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
                else if (endian == 2) v = _byteswap_ulong(v);
                else if (endian == 3) v = (v >> 16) | (v << 16);
                idx[i] = v;
            } else for (uint32_t i = 0; i < num; i++) {
                uint16_t v = ((const uint16_t*)src)[i];
                if (endian == 1) v = _byteswap_ushort(v);
                idx[i] = v;
            }
        } else {
            if (!inlineIdx) return;
            if (idx32) memcpy(idx.data(), inlineIdx, num * 4);
            else for (uint32_t i = 0; i < num; i++) idx[i] = (inlineIdx[i / 2] >> ((i & 1) ? 16 : 0)) & 0xFFFF;
        }
        uint32_t rv = idx32 ? reset : (reset & 0xFFFF);
        for (uint32_t i = 0; i < num; i++) {
            if (resetOn && idx[i] == rv) { idx[i] = 0xFFFFFFFFu; continue; }
            minI = std::min(minI, idx[i]); maxI = std::max(maxI, idx[i]);
        }
    } else { minI = 0; maxI = num - 1; }
    if (minI > maxI) return;

    // ---- topology / conversion to something D3D11 can draw ----
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool rect = false;
    std::vector<uint32_t> conv;
    auto vert = [&](uint32_t i) { return hasIdx ? idx[i] : i; };
    switch (prim) {
      case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
      case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
      case 3: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
      case 4: case 7: break;
      case 6: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
      case 8: rect = true; break;
      case 5: case 15: for (uint32_t i = 2; i < num; i++) { conv.push_back(vert(0)); conv.push_back(vert(i - 1)); conv.push_back(vert(i)); } break;
      case 13: for (uint32_t i = 0; i + 4 <= num; i += 4) { uint32_t a = vert(i), b = vert(i + 1), c = vert(i + 2), d = vert(i + 3); conv.insert(conv.end(), { a, b, c, a, c, d }); } break;
      case 14: for (uint32_t i = 0; i + 4 <= num; i += 2) { uint32_t a = vert(i), b = vert(i + 1), c = vert(i + 2), d = vert(i + 3); conv.insert(conv.end(), { a, b, d, a, d, c }); } break;
      case 12: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; for (uint32_t i = 0; i < num; i++) conv.push_back(vert(i)); conv.push_back(vert(0)); break;
      default: logf_once(S("prim%u", prim).c_str(), "unsupported primitive type %u", prim); return;
    }
    const std::vector<uint32_t>* useIdx = !conv.empty() ? &conv : (hasIdx ? &idx : nullptr);
    uint32_t drawCount = useIdx ? (uint32_t)useIdx->size() : num;
    if (drawCount == 0) return;

    {   // experiment: BCR2_DRAWDELAYUS=n waits n microseconds before each draw reads guest data
        static const int kDelayUs = getenv("BCR2_DRAWDELAYUS") ? atoi(getenv("BCR2_DRAWDELAYUS")) : 0;
        if (kDelayUs > 0) {
            LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a); QueryPerformanceFrequency(&f);
            do { YieldProcessor(); QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < kDelayUs);
        }
        // experiment: BCR2_DRAWSYNC=1 flushes before each draw, =2 also waits for the host GPU to finish everything so far
        static const int kSync = getenv("BCR2_DRAWSYNC") ? atoi(getenv("BCR2_DRAWSYNC")) : 0;
        if (kSync == 1) g_ctx->Flush();
        if (kSync == 2) {
            static ComPtr<ID3D11Query> q;
            if (!q) { D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT; g_dev->CreateQuery(&qd, &q); }
            g_ctx->End(q.Get());
            BOOL done = FALSE;
            while (g_ctx->GetData(q.Get(), &done, sizeof(done), 0) != S_OK || !done) YieldProcessor();
        }
    }
    // ---- vertex data ----
    int32_t indexOffset = (int32_t)(g_regs[R_INDX_OFFSET] & 0xFFFFFF);
    indexOffset = sext(indexOffset, 24);
    static StateCb cb;
    memset(cb.vfo, 0, sizeof(cb.vfo));
    for (auto& u : vs->vf) {
        uint32_t f0 = g_regs[R_FETCH + u.slot * 2], f1 = g_regs[R_FETCH + u.slot * 2 + 1];
        if ((f0 & 3) != 3) { logf_once("vfbad", "vertex fetch constant %u has type %u", u.slot, f0 & 3); continue; }
        uint32_t addr = f0 & ~3u, endian = f1 & 3, words = (f1 >> 2) & 0xFFFFFF;
        uint32_t strideB = u.stride * 4;
        uint64_t first = strideB ? (uint64_t)std::max<int64_t>(0, (int64_t)minI + indexOffset) * strideB : 0;
        uint64_t last = strideB ? ((uint64_t)((int64_t)maxI + indexOffset) + 1) * strideB : (uint64_t)words * 4;
        uint64_t avail = (uint64_t)words * 4;
        if (last > avail) last = avail;
        if (first >= last) { first = 0; last = std::min<uint64_t>(avail, 4096); }
        // Upload the whole stream: the index buffer doesn't say which vertices a shader reads. Instanced meshes compute vertex = index mod instance size
        // in the vertex shader (vs 2090BFF4A9951B2D), so instance 5's indices are large while it reads the first vertices of the stream. A range taken from
        // the indices missed those, and the shader read whatever sat nearby in the ring: stray black geometry and heavy flicker, even with a still camera.
        // BCR2_VBRANGE restores the narrow upload, for comparison.
        static const bool kVbRange = getenv("BCR2_VBRANGE") != nullptr;
        if (!kVbRange) { first = 0; last = avail; }
        uint32_t size = (uint32_t)std::min<uint64_t>(last - first + 64, kVbRingSize / 2);
        if ((first + size) > avail) size = (uint32_t)(avail - first);
        if (size == 0) continue;
        uint8_t* ptr = nullptr;
        uint32_t off = vb_alloc(size, ptr);
        const uint8_t* src = phys(addr + (uint32_t)first);
        if (endian == 2) {
            uint32_t n = size / 4;
            const uint32_t* s = (const uint32_t*)src; uint32_t* d = (uint32_t*)ptr;
            for (uint32_t i = 0; i < n; i++) d[i] = _byteswap_ulong(s[i]);
        } else {
            memcpy(ptr, src, size);
            if (endian) swap_block(ptr, size, endian);
        }
        vb_commit(off, size, ptr);
        cb.vfo[u.slot] = off - (uint32_t)first;
    }

    // ---- textures and samplers ----
    ID3D11ShaderResourceView* srvs[32] = {};
    ID3D11SamplerState* samps[16] = {};
    for (int s = 0; s < 16; s++) {
        cb.tfp[s * 4] = 1; cb.tfp[s * 4 + 1] = 1; cb.tfp[s * 4 + 2] = 1; cb.tfp[s * 4 + 3] = 0;
    }
    auto bind_tex = [&](const TexUse& u) {
        const uint32_t* tf = &g_regs[R_FETCH + u.slot * 6];
        if ((tf[0] & 3) != 2) return;
        uint32_t fmt = tf[1] & 63, base = tf[1] & 0xFFFFF000u, dim = (tf[5] >> 9) & 3;
        uint32_t w = (tf[2] & 0x1FFF) + 1, h = ((tf[2] >> 13) & 0x1FFF) + 1;
        if (dim == 0) h = 1;
        uint32_t sw = (tf[3] >> 1) & 0xFFF;
        for (int i = 0; i < 4; i++) cb.tfsw[u.slot * 4 + i] = (sw >> (3 * i)) & 7;
        for (int i = 0; i < 4; i++) cb.tfsg[u.slot * 4 + i] = (tf[0] >> (2 + i * 2)) & 3;
        cb.tfp[u.slot * 4 + 2] = (float)std::ldexp(1.0, sext((tf[3] >> 13) & 63, 6));
        cb.tfi[u.slot * 4] = 1.0f / w; cb.tfi[u.slot * 4 + 1] = 1.0f / h;
        ID3D11SamplerState* smp = get_sampler(tf, u);
        samps[u.slot] = smp;
        // a resolve destination?
        Surface* sf = surface_at(base);
        if (sf) {
            uint64_t gh = guest_sample_hash(sf->base, sf->end - sf->base);
            if (gh != sf->guestHash) {                                     // guest wrote new CPU data over it: forget the surface
                fprintf(stderr, "[gfx] surface %X overwritten by guest, dropped\n", sf->base);
                g_surfaces.erase(sf->base); delete sf; sf = nullptr;
            }
        }
        if (sf && u.dim != 3) {
            bool wantsRaw = sf->rawSrv && fmt != 22 && fmt != 23;           // depth read as bytes, not as a depth format
            srvs[u.slot] = wantsRaw ? sf->rawSrv.Get() : sf->srv.Get();
            cb.tfp[u.slot * 4] = (float)w / sf->w; cb.tfp[u.slot * 4 + 1] = (float)h / sf->h;
            return;
        }
        FetchInfo fi{};
        fi.base = base; fi.fmt = fmt; fi.w = w; fi.h = h; fi.pitch = ((tf[0] >> 22) & 0x1FF) << 5; fi.endian = (tf[1] >> 6) & 3;
        fi.dim = dim; fi.faces = dim == 3 ? 6 : 1; fi.tiled = tf[0] >> 31; fi.packed = (tf[5] >> 11) & 1;
        if (uint32_t mipPage = tf[5] >> 12) {                               // no mip address means the base image is all there is
            uint32_t sizeMax = 0; while ((2u << sizeMax) <= std::max(w, h)) sizeMax++;
            uint32_t mipMin = std::min((tf[4] >> 2) & 15, sizeMax);
            fi.maxLevel = std::max(std::min((tf[4] >> 6) & 15, sizeMax), mipMin);
            if (fi.maxLevel) fi.mipBase = (mipPage & 0x1FFFF) << 12;
        }
        if (dim == 2) { logf_once("tex3d", "3D texture not implemented"); return; }
        if ((u.dim == 3) != (dim == 3)) { logf_once("texdim", "texture dimension mismatch shader=%u fetch=%u", u.dim, dim); return; }
        HostTex* ht = get_texture(fi);
        if (!ht) return;
        srvs[u.dim == 3 ? 16 + u.slot : u.slot] = ht->srv.Get();
        TexFmt t; tex_fmt(fmt, t);
        if (t.block > 1) { cb.tfp[u.slot * 4] = (float)w / ht->hostW; cb.tfp[u.slot * 4 + 1] = (float)h / ht->hostH; }
    };
    if (ps) for (auto& u : ps->tex) bind_tex(u);
    {
        ID3D11ShaderResourceView* vsr[32] = {};
        ID3D11SamplerState* vsm[16] = {};
        ID3D11ShaderResourceView* psKeep[32]; memcpy(psKeep, srvs, sizeof(srvs));
        ID3D11SamplerState* psKeepS[16]; memcpy(psKeepS, samps, sizeof(samps));
        memset(srvs, 0, sizeof(srvs));
        for (auto& u : vs->tex) bind_tex(u);
        memcpy(vsr, srvs, sizeof(srvs)); memcpy(vsm, samps, sizeof(samps));
        g_ctx->VSSetShaderResources(0, 32, vsr);
        ID3D11ShaderResourceView* vbs[1] = { g_vbSrv.Get() };
        g_ctx->VSSetShaderResources(64, 1, vbs);
        g_ctx->VSSetSamplers(0, 16, vsm);
        g_ctx->PSSetShaderResources(0, 32, psKeep);
        g_ctx->PSSetSamplers(0, 16, psKeepS);
    }

    // ---- viewport mapping into shader constants ----
    uint32_t vte = g_regs[R_VTE_CNTL];
    float xs = (vte & 1) ? reg_f(R_VPORT_XSCALE) : 1, xo = (vte & 2) ? reg_f(R_VPORT_XSCALE + 1) : 0;
    float ys = (vte & 4) ? reg_f(R_VPORT_XSCALE + 2) : 1, yo = (vte & 8) ? reg_f(R_VPORT_XSCALE + 3) : 0;
    float zs = (vte & 16) ? reg_f(R_VPORT_XSCALE + 4) : 1, zo = (vte & 32) ? reg_f(R_VPORT_XSCALE + 5) : 0;
    if ((mode >> 16) & 1) {
        uint32_t wo = g_regs[R_WINDOW_OFFSET];
        xo += sext(wo & 0x7FFF, 15); yo += sext((wo >> 16) & 0x7FFF, 15);
    }
    if (!(g_regs[R_VTX_CNTL] & 1)) { xo += 0.5f; yo += 0.5f; }
    cb.vpNdc[0] = xs * 2.0f / rw; cb.vpNdc[1] = -ys * 2.0f / rh; cb.vpNdc[2] = xo * 2.0f / rw - 1.0f; cb.vpNdc[3] = 1.0f - yo * 2.0f / rh;
    cb.vpZ[0] = zs; cb.vpZ[1] = zo; cb.vpZ[2] = (float)((vte >> 8) & 1); cb.vpZ[3] = (float)((vte >> 9) & 1);
    cb.vpW[0] = (float)((vte >> 10) & 1); cb.vpW[1] = (float)indexOffset; cb.vpW[2] = 0; cb.vpW[3] = 0;
    uint32_t colorCtl = g_regs[R_COLORCONTROL];
    cb.alpha[0] = (colorCtl & 8) ? (float)(colorCtl & 7) : 7.0f; cb.alpha[1] = reg_f(R_ALPHA_REF); cb.alpha[3] = 0;
    cb.alpha[2] = (!depthOnly && ((g_regs[R_COLOR_INFO] >> 16) & 0xF) == 1) ? 1.0f : 0.0f;
    if (g_trace > 0 && cb.alpha[2] != 0) logf_once("gammart", "gamma colour target in use");
    memcpy(cb.bools, &g_regs[R_BOOL], 32);
    memcpy(cb.loops, &g_regs[R_LOOP], 128);
    upload_cb(g_cbState.Get(), &cb, sizeof(cb));
    if (g_constDirty) { upload_cb(g_cbFloat.Get(), &g_regs[R_CONST], 8192); g_constDirty = false; }
    ID3D11Buffer* cbs[2] = { g_cbFloat.Get(), g_cbState.Get() };
    g_ctx->VSSetConstantBuffers(0, 2, cbs);
    g_ctx->PSSetConstantBuffers(0, 2, cbs);
    g_ctx->VSSetShader(vs->vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(ps ? ps->ps.Get() : nullptr, nullptr, 0);
    g_ctx->GSSetShader(rect ? g_gsRect.Get() : nullptr, nullptr, 0);
    if (rect) g_ctx->GSSetConstantBuffers(0, 0, nullptr);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(topo);

    // ---- fixed-function ----
    float blendF[4] = { reg_f(R_BLEND_RED), reg_f(R_BLEND_RED + 1), reg_f(R_BLEND_RED + 2), reg_f(R_BLEND_RED + 3) };
    g_ctx->OMSetBlendState(get_blend(mask), blendF, 0xFFFFFFFFu);
    {   // BCR2_DBGADDITIVE: paint additive scene draws solid magenta, to see where they land
        static const bool kDbg = getenv("BCR2_DBGADDITIVE") != nullptr;
        if (kDbg && prim != 6 && ((g_regs[R_BLEND0] >> 8) & 31) == 1) {
            static ComPtr<ID3D11PixelShader> magenta;
            if (!magenta) if (auto b = compile_cached("float4 main(float4 p:SV_Position):SV_Target0 { return float4(1,0,1,1); }\n", "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &magenta);
            g_ctx->PSSetShader(magenta.Get(), nullptr, 0);
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        }
    }
    uint32_t sr = g_regs[R_STENCIL], srbf = g_regs[R_STENCIL_BF];
    uint32_t dcEff = dsv ? dc : 0;
    {   // BCR2_DBGNOZ: no depth test on blended scene draws, to see whether depth is rejecting them
        static const bool kNoZ = getenv("BCR2_DBGNOZ") != nullptr;
        if (kNoZ && prim != 6 && g_regs[R_BLEND0] != 0x00010001u) dcEff &= ~2u;
    }
    g_ctx->OMSetDepthStencilState(get_ds(dcEff, sr, srbf), sr & 0xFF);
    float polyScale = 0, polyOff = 0;
    if ((mode >> 11) & 3) { polyScale = reg_f(R_OFFSET_FRONT_SCALE) / 16.0f; polyOff = reg_f(R_OFFSET_FRONT_SCALE + 1); }
    bool clipOff = (g_regs[R_CLIP_CNTL] >> 16) & 1;
    g_ctx->RSSetState(get_rs(mode, rect, clipOff, polyScale, polyOff));
    D3D11_VIEWPORT vp{ 0, 0, (float)rw, (float)rh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    int sx0, sy0, sx1, sy1; compute_scissor(sx0, sy0, sx1, sy1);
    D3D11_RECT sc{ sx0, sy0, std::min(sx1, (int)rw), std::min(sy1, (int)rh) };
    g_ctx->RSSetScissorRects(1, &sc);

    struct ProbeAfter {                                                     // BCR2_PROBE="x,y": log draws that change this pixel in target 0
        ID3D11Resource* res = nullptr; uint32_t px = 0, py = 0, rw_, rh_, before = 0; bool have = false;
        bool read(uint32_t& v) {
            D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
            if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
            d.Width = 1; d.Height = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return false;
            D3D11_BOX box{ px, py, 0, px + 1, py + 1, 1 };
            g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, res, 0, &box);
            D3D11_MAPPED_SUBRESOURCE ms;
            if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
            v = *(uint32_t*)ms.pData; g_ctx->Unmap(st.Get(), 0);
            return true;
        }
        ProbeAfter(ID3D11RenderTargetView* rtv, uint32_t rw, uint32_t rh, uint32_t pr) : rw_(rw), rh_(rh) {
            static const char* e = getenv("BCR2_PROBE");
            static const char* ef = getenv("BCR2_PROBEF");                  // "u,v" in 0..1 of the draw's scissor, to follow a spot through a blur chain
            if ((!e && !ef) || g_trace <= 1000 || !rtv) return;
            if (ef) {
                float u = 0, v = 0; sscanf(ef, "%f,%f", &u, &v);
                int a, b, c, f; compute_scissor(a, b, c, f);
                px = (uint32_t)(u * (c - a)); py = (uint32_t)(v * (f - b));
            } else sscanf(e, "%u,%u", &px, &py);
            if (px >= rw || py >= rh) return;
            rtv->GetResource(&res);
            have = read(before);
        }
        ~ProbeAfter() {
            if (!res) return;
            uint32_t v;
            if (have && read(v) && v != before) fprintf(stderr, "[probe] rt=%p at %u,%u %08X -> %08X (above draw, %ux%u)\n", res, px, py, before, v, rw_, rh_);
            res->Release();
        }
    } probe(rtvs[0], rw, rh, 0);
    if (useIdx) {
        uint32_t bytes = drawCount * 4;
        if (bytes > kIbRingSize) { logf_once("ibbig", "index data too large"); return; }
        D3D11_MAP m = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_ibPos + bytes > kIbRingSize) { g_ibPos = 0; m = D3D11_MAP_WRITE_DISCARD; }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g_ctx->Map(g_ibRing.Get(), 0, m, 0, &ms))) return;
        memcpy((uint8_t*)ms.pData + g_ibPos, useIdx->data(), bytes);
        g_ctx->Unmap(g_ibRing.Get(), 0);
        g_ctx->IASetIndexBuffer(g_ibRing.Get(), DXGI_FORMAT_R32_UINT, g_ibPos);
        g_ibPos += bytes;
        g_ctx->DrawIndexed(drawCount, 0, 0);
    } else {
        g_ctx->Draw(drawCount, 0);
    }
    // BCR2_PROBEQ="x,y": like BCR2_PROBE, but only queues a pixel copy after each draw and reads them at the end of the frame, so nothing forces a sync.
    static const char* kProbeQ = getenv("BCR2_PROBEQ");
    if (kProbeQ && g_trace > 1000 && rtvs[0]) {
        uint32_t qx = 0, qy = 0; sscanf(kProbeQ, "%u,%u", &qx, &qy);
        ComPtr<ID3D11Resource> res; rtvs[0]->GetResource(&res);
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res.Get())->GetDesc(&d);
        if (qx < d.Width && qy < d.Height && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            d.Width = 1; d.Height = 1; d.MipLevels = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ProbeQ q; q.res = res.Get();
            if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &q.st))) {
                D3D11_BOX box{ qx, qy, 0, qx + 1, qy + 1, 1 };
                g_ctx->CopySubresourceRegion(q.st.Get(), 0, 0, 0, 0, res.Get(), 0, &box);
                const uint32_t* t0 = &g_regs[R_FETCH];
                q.text = S("num=%u prim=%u idx=%d vs=%016llX ps=%016llX blend0=%08X dc=%08X mask=%08X col=%08X surf=%08X vf95=%08X/%08X c0=%g,%g,%g,%g tf0=%08X",
                           drawCount, prim, useIdx ? 1 : 0, (unsigned long long)vs->hash, ps ? (unsigned long long)ps->hash : 0ull, g_regs[R_BLEND0], g_regs[R_DEPTHCONTROL], g_regs[R_COLOR_MASK], g_regs[R_COLOR_INFO], g_regs[R_SURFACE_INFO],
                           g_regs[R_FETCH + 95 * 2], g_regs[R_FETCH + 95 * 2 + 1], reg_f(R_CONST), reg_f(R_CONST + 1), reg_f(R_CONST + 2), reg_f(R_CONST + 3), t0[1]);
                g_probeQ.push_back(std::move(q));
            }
        }
    }
}

// ---- resolve ----
static void blit(ID3D11ShaderResourceView* src, ID3D11PixelShader* ps, ID3D11RenderTargetView* dst, uint32_t dw, uint32_t dh,
                 int dx0, int dy0, int dx1, int dy1, int sx, int sy, bool swaprb, float scaleX = 1, float scaleY = 1, ID3D11ShaderResourceView* src2 = nullptr, const float* ofsOverride = nullptr) {
    ID3D11ShaderResourceView* nul[32] = {};
    g_ctx->PSSetShaderResources(0, 32, nul);
    g_ctx->OMSetRenderTargets(1, &dst, nullptr);
    invalidate_binds();
    struct { float ofs[2]; float scale[2]; uint32_t sw; uint32_t pad[3]; } p{ { (float)(sx - dx0), (float)(sy - dy0) }, { scaleX, scaleY }, swaprb ? 1u : 0u, {} };
    if (ofsOverride) { p.ofs[0] = ofsOverride[0]; p.ofs[1] = ofsOverride[1]; }
    upload_cb(g_cbBlit.Get(), &p, sizeof(p));
    ID3D11Buffer* cb = g_cbBlit.Get();
    g_ctx->PSSetConstantBuffers(2, 1, &cb);
    g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g_ctx->OMSetDepthStencilState(get_ds(0, 0, 0), 0);
    g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
    D3D11_VIEWPORT vp{ 0, 0, (float)dw, (float)dh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{ dx0, dy0, dx1, dy1 };
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->PSSetShaderResources(0, 1, &src);
    if (src2) g_ctx->PSSetShaderResources(1, 1, &src2);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 2, nul);
    static const char* probeEnv = getenv("BCR2_PROBE");                     // also report the probed pixel after each resolve blit
    if (probeEnv && g_trace > 1000) {
        uint32_t px = 0, py = 0; sscanf(probeEnv, "%u,%u", &px, &py);
        ID3D11Resource* res = nullptr; dst->GetResource(&res);
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
        if (px < d.Width && py < d.Height && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            d.Width = 1; d.Height = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> st;
            if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
                D3D11_BOX box{ px, py, 0, px + 1, py + 1, 1 };
                g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, res, 0, &box);
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
                    fprintf(stderr, "[probe-blit] dst=%p %ux%u rect=%d,%d-%d,%d src=%d,%d value=%08X\n", res, dw, dh, dx0, dy0, dx1, dy1, sx, sy, *(uint32_t*)ms.pData);
                    g_ctx->Unmap(st.Get(), 0);
                }
            }
        }
        res->Release();
    }
}

// The game restores depth by drawing depth bytes into a colour target at the depth buffer's EDRAM base. Host targets are
// separate, so carry the newer colour contents over when that depth buffer is used next. Stencil restarts at zero.
static void sync_depth_alias(Rt* depth, uint32_t base, uint32_t pitch) {
    for (uint32_t fmt : { 0u, 1u }) {
        uint64_t key = ((uint64_t)base << 32) | ((uint64_t)fmt << 16) | (pitch & 0xFFFF);
        auto it = g_rts.find(key);
        if (it == g_rts.end() || it->second->seq <= depth->seq) continue;
        Rt* c = it->second;
        ID3D11ShaderResourceView* nul[32] = {};
        g_ctx->PSSetShaderResources(0, 32, nul);
        g_ctx->OMSetRenderTargets(0, nullptr, depth->dsv.Get());
        invalidate_binds();
        g_ctx->ClearDepthStencilView(depth->dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);
        g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_psColorToDepth.Get(), nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        g_ctx->OMSetDepthStencilState(get_ds(0x76, 0, 0), 0);               // z on, z write, func always
        g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
        D3D11_VIEWPORT vp{ 0, 0, (float)depth->w, (float)depth->h, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc{ 0, 0, (LONG)depth->w, (LONG)depth->h };
        g_ctx->RSSetScissorRects(1, &sc);
        ID3D11ShaderResourceView* s = c->srv.Get();
        g_ctx->PSSetShaderResources(0, 1, &s);
        g_ctx->Draw(3, 0);
        g_ctx->PSSetShaderResources(0, 1, nul);
        depth->seq = ++g_rtSeq;
        logf_once("alias", "colour target at EDRAM base %X carried into depth (aliasing)", base);
    }
}

static void do_resolve() {
    g_resolves++;
    uint32_t cc = g_regs[R_COPY_CONTROL], surf = g_regs[R_SURFACE_INFO], pitch = surf & 0x3FFF;
    uint32_t srcSel = cc & 7, cmd = (cc >> 20) & 3;
    bool colorClear = (cc >> 8) & 1, depthClear = (cc >> 9) & 1, isDepth = srcSel >= 4;
    const uint32_t* vf = &g_regs[R_FETCH];
    if ((vf[0] & 3) != 3) { logf_once("resvf", "resolve without vertex fetch 0"); return; }
    uint32_t vaddr = vf[0] & ~3u, endian = vf[1] & 3;
    float v[6];
    for (int i = 0; i < 6; i++) {
        uint32_t raw = *(const uint32_t*)phys(vaddr + i * 4);
        if (endian == 2) raw = _byteswap_ulong(raw);
        else if (endian == 1) raw = ((raw & 0xFF00FF00u) >> 8) | ((raw & 0x00FF00FFu) << 8);
        else if (endian == 3) raw = (raw >> 16) | (raw << 16);
        memcpy(&v[i], &raw, 4);
    }
    float half = (g_regs[R_VTX_CNTL] & 1) ? 0.0f : 0.5f;
    auto px = [&](float f) { return ((int)std::floor((f + half) * 256.0f + 0.5f) + 127) >> 8; };
    int x0 = std::min({ px(v[0]), px(v[2]), px(v[4]) }), x1 = std::max({ px(v[0]), px(v[2]), px(v[4]) });
    int y0 = std::min({ px(v[1]), px(v[3]), px(v[5]) }), y1 = std::max({ px(v[1]), px(v[3]), px(v[5]) });
    uint32_t mode = g_regs[R_MODE_CNTL];
    if ((mode >> 16) & 1) {
        uint32_t wo = g_regs[R_WINDOW_OFFSET];
        int ox = sext(wo & 0x7FFF, 15), oy = sext((wo >> 16) & 0x7FFF, 15);
        x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    }
    int sx0, sy0, sx1, sy1; compute_scissor(sx0, sy0, sx1, sy1);
    x0 = std::clamp(x0, sx0, sx1); x1 = std::clamp(x1, sx0, sx1); y0 = std::clamp(y0, sy0, sy1); y1 = std::clamp(y1, sy0, sy1);
    x0 &= ~7; y0 &= ~7; x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
    int pa = (int)(pitch & ~7u);
    x1 = std::min(x1, pa); x0 = std::min(x0, pa);
    if (x1 <= x0 || y1 <= y0) return;

    uint32_t destBase = g_regs[R_COPY_DEST_BASE] & 0x1FFFFFFFu, destPitch = g_regs[R_COPY_DEST_PITCH] & 0x3FFF, destH = (g_regs[R_COPY_DEST_PITCH] >> 16) & 0x3FFF;
    uint32_t di = g_regs[R_COPY_DEST_INFO];
    bool destSwap = (di >> 24) & 1;
    uint32_t destFmt = (di >> 7) & 63;
    static const uint32_t regsC[4] = { R_COLOR_INFO, R_COLOR1_INFO, R_COLOR2_INFO, R_COLOR3_INFO };
    uint32_t ci = g_regs[regsC[srcSel & 3]];
    Rt* colorSrc = (!isDepth || colorClear) ? get_color_rt(ci & 0xFFF, (ci >> 16) & 0xF, pitch) : nullptr;
    Rt* depthRt = (isDepth || depthClear) ? get_depth_rt(g_regs[R_DEPTH_INFO] & 0xFFF, pitch) : nullptr;

    static int nlog = 0;
    if (nlog++ < 60 || g_trace > 0)
        fprintf(stderr, "[gfx] resolve src=%u cmd=%u rect=%d,%d-%d,%d dest=%X pitch=%u h=%u fmt=%u swap=%d clr=%d/%d win=%08X surf=%08X col=%08X dep=%08X cc=%08X dc=%08X\n",
                srcSel, cmd, x0, y0, x1, y1, destBase, destPitch, destH, destFmt, (int)destSwap, (int)colorClear, (int)depthClear, g_regs[R_WINDOW_OFFSET], surf, ci, g_regs[R_DEPTH_INFO], g_regs[R_COLOR_CLEAR], g_regs[R_DEPTH_CLEAR]);

    if (cmd != 3 && destBase != 0 && destPitch != 0) {
        Rt* src = isDepth ? depthRt : colorSrc;
        DXGI_FORMAT fmt = isDepth ? DXGI_FORMAT_R32_FLOAT : src->fmt;
        uint32_t bytesPerPx = 4; { TexFmt t; if (tex_fmt(destFmt, t) && t.block == 1) bytesPerPx = t.bpb; }
        uint32_t bl2 = bytesPerPx == 1 ? 0 : bytesPerPx == 2 ? 1 : bytesPerPx == 4 ? 2 : bytesPerPx == 8 ? 3 : 4;
        uint32_t pitchAligned = align_up(destPitch, 32);
        uint32_t heightAligned = align_up(destH ? destH : (uint32_t)y1, 32);
        uint32_t bytes = pitchAligned * heightAligned * bytesPerPx;
        // The destination base carries the tile's offset. Find the surface this belongs to and turn the offset back into x,y.
        Surface* sf = surface_at(destBase);
        int ox = 0, oy = 0;
        if (sf && (sf->pitch != destPitch || sf->fmt != fmt)) {
            if (sf->base == destBase) sf = nullptr;
            else if (sf->pitch != destPitch) sf = nullptr;
        }
        if (sf && sf->fmt != fmt) sf = nullptr;
        if (sf && sf->base != destBase) {
            uint32_t delta = destBase - sf->base;
            // macro-tile aligned: 32x32 blocks, bytes per macro tile = 32*32*bpp
            uint32_t mt = delta / (1024u * bytesPerPx);
            ox = (int)((mt % (pitchAligned / 32)) * 32); oy = (int)((mt / (pitchAligned / 32)) * 32);
        }
        if (!sf) sf = make_surface(destBase, destPitch, std::max(destH ? destH : (uint32_t)y1, 1u), bytes, fmt);
        if (sf) {
            int dx0 = x0 + ox, dy0 = y0 + oy, dx1 = x1 + ox, dy1 = y1 + oy;
            blit(src->srv.Get(), isDepth ? g_psBlitDepth.Get() : g_psBlit.Get(), sf->rtv.Get(), sf->w, sf->h, dx0, dy0, std::min(dx1, (int)sf->w), std::min(dy1, (int)sf->h), x0, y0, destSwap && !isDepth);
            if (isDepth && sf->rawRtv)
                blit(src->srv.Get(), g_psBlitRaw.Get(), sf->rawRtv.Get(), sf->w, sf->h, dx0, dy0, std::min(dx1, (int)sf->w), std::min(dy1, (int)sf->h), x0, y0, false, 1, 1, src->stencilSrv.Get());
            sf->guestHash = guest_sample_hash(sf->base, sf->end - sf->base);
            sf->swapped = destSwap && !isDepth;
        }
    }
    if (colorClear && colorSrc) {
        uint32_t cv = g_regs[R_COLOR_CLEAR];
        float col[4] = { ((cv >> 16) & 255) / 255.0f, ((cv >> 8) & 255) / 255.0f, (cv & 255) / 255.0f, (cv >> 24) / 255.0f };
        D3D11_RECT r{ x0, y0, x1, y1 };
        if (g_ctx1) g_ctx1->ClearView(colorSrc->rtv.Get(), col, &r, 1);
        else g_ctx->ClearRenderTargetView(colorSrc->rtv.Get(), col);
    }
    if (depthClear && depthRt) {
        uint32_t dv = g_regs[R_DEPTH_CLEAR];
        ID3D11RenderTargetView* none[1] = { nullptr };
        g_ctx->OMSetRenderTargets(1, none, nullptr);
        invalidate_binds();
        g_ctx->ClearDepthStencilView(depthRt->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, (float)((dv >> 8) & 0xFFFFFF) / 16777215.0f, (UINT8)(dv & 0xFF));
        depthRt->seq = ++g_rtSeq;
    }
}

// ---- present ----
// BCR2_SHOTBURST=n with BCR2_SHOTDIR: dump the n frames after each simulated press, to count frames to the visible response.
static std::atomic<int> g_shotBurstLeft{0};
void gpu_note_press() {
    static const int kBurst = getenv("BCR2_SHOTBURST") ? atoi(getenv("BCR2_SHOTBURST")) : 0;
    fprintf(stderr, "[lat] press seen by guest at frame=%llu\n", (unsigned long long)g_frame);
    if (kBurst > 0) g_shotBurstLeft = kBurst;
}

static std::atomic<bool> g_presentFollowsDisplay{false};
void gpu_set_display_locked(bool locked) { g_presentFollowsDisplay = locked; }

// The display the window is on, for the vblank thread. Refreshed when the window changes monitor.
static std::mutex g_outputLock;
static ComPtr<IDXGIOutput> g_output;
static void track_output() {
    static HMONITOR last = nullptr;
    HMONITOR m = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    if (m == last) return;
    ComPtr<IDXGIOutput> o;
    if (FAILED(g_swap->GetContainingOutput(&o))) return;
    last = m;
    std::lock_guard<std::mutex> lk(g_outputLock);
    g_output = o;
}
bool gpu_wait_vblank() {
    ComPtr<IDXGIOutput> o;
    { std::lock_guard<std::mutex> lk(g_outputLock); o = g_output; }
    return o && SUCCEEDED(o->WaitForVBlank());
}

// What reached the screen, as opposed to what was submitted: refresh rate, frames discarded before scanout, and Present-to-scanout time.
static void note_display_stats(LARGE_INTEGER presented, uint64_t frame) {
    static LARGE_INTEGER sent[64]{};
    static UINT lastId = 0; static UINT64 lastRefresh = 0; static LONGLONG lastSync = 0;
    static uint64_t nShown = 0, nSkipped = 0, nRefreshes = 0, nDisjoint = 0, lastLog = 0; static LONGLONG syncTicks = 0;
    static double latSum = 0, latMax = 0;
    UINT id = 0;
    if (SUCCEEDED(g_swap->GetLastPresentCount(&id))) sent[id & 63] = presented;
    DXGI_FRAME_STATISTICS fs{};
    HRESULT hr = g_swap->GetFrameStatistics(&fs);
    if (FAILED(hr)) nDisjoint++;
    else if (fs.PresentCount != lastId) {
        if (lastId) {
            UINT adv = fs.PresentCount - lastId;
            nShown++; nSkipped += adv - 1;
            nRefreshes += (uint64_t)(fs.SyncRefreshCount - lastRefresh);
            syncTicks += fs.SyncQPCTime.QuadPart - lastSync;
            if (sent[fs.PresentCount & 63].QuadPart) {
                double ms = (double)(fs.SyncQPCTime.QuadPart - sent[fs.PresentCount & 63].QuadPart) * g_perfTicksToMs;
                latSum += ms; latMax = std::max(latMax, ms);
            }
        }
        lastId = fs.PresentCount; lastRefresh = fs.SyncRefreshCount; lastSync = fs.SyncQPCTime.QuadPart;
    }
    if (frame - lastLog >= 300) {
        double secs = (double)syncTicks * g_perfTicksToMs / 1000.0;
        fprintf(stderr, "[disp] frame=%llu presents=%llu shown=%llu discarded=%llu refreshes=%llu hz=%.3f present_to_scan avg=%.1f max=%.1f failed=%llu\n",
                (unsigned long long)frame, (unsigned long long)(frame - lastLog), (unsigned long long)nShown, (unsigned long long)nSkipped, (unsigned long long)nRefreshes,
                secs > 0 ? nRefreshes / secs : 0.0, nShown ? latSum / nShown : 0.0, latMax, (unsigned long long)nDisjoint);
        lastLog = frame; nShown = nSkipped = nRefreshes = nDisjoint = 0; syncTicks = 0; latSum = latMax = 0;
    }
}

static void write_shot() {
    static const char* dir = getenv("BCR2_SHOTDIR");
    if (!dir) return;
    static const int every = getenv("BCR2_SHOTEVERY") ? std::max(atoi(getenv("BCR2_SHOTEVERY")), 1) : 60;
    bool burst = g_shotBurstLeft.load() > 0;
    if (burst) g_shotBurstLeft--;
    if (!burst && (g_frame % every) != 0) return;
    D3D11_TEXTURE2D_DESC d; g_backTex->GetDesc(&d);
    d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    g_ctx->CopyResource(st.Get(), g_backTex.Get());
    D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    std::string p = std::string(dir) + S("\\shot_%06llu.bmp", (unsigned long long)g_frame);
    if (FILE* f = fopen(p.c_str(), "wb")) {
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        uint32_t imgSize = d.Width * d.Height * 4;
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + imgSize;
        ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
        std::vector<uint8_t> row(d.Width * 4);
        for (uint32_t y = 0; y < d.Height; y++) {
            const uint8_t* s = (const uint8_t*)ms.pData + y * ms.RowPitch;
            for (uint32_t x = 0; x < d.Width; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = 255; }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    g_ctx->Unmap(st.Get(), 0);
}

// BCR2_DUMPFRAME=n with BCR2_SHOTDIR: log frame n's draws and resolves, then write each resolved surface as a BMP.
static const int64_t g_dumpFrame = getenv("BCR2_DUMPFRAME") ? _atoi64(getenv("BCR2_DUMPFRAME")) : -1;
static void dump_surfaces() {
    const char* dir = getenv("BCR2_SHOTDIR");
    if (!dir) return;
    std::vector<std::pair<Surface*, bool>> all;
    for (auto& kv : g_surfaces) { all.push_back({ kv.second, false }); if (kv.second->rawTex) all.push_back({ kv.second, true }); }
    for (auto& ent : all) {
        Surface* s = ent.first;
        ID3D11Texture2D* from = ent.second ? s->rawTex.Get() : s->tex.Get();
        D3D11_TEXTURE2D_DESC d; from->GetDesc(&d);
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) continue;
        g_ctx->CopyResource(st.Get(), from);
        D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) continue;
        std::string p = std::string(dir) + S("\\surf_%08X_f%d%s.bmp", s->base, (int)s->fmt, ent.second ? "_raw" : "");
        if (FILE* f = fopen(p.c_str(), "wb")) {
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            uint32_t imgSize = d.Width * d.Height * 4;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + imgSize;
            ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            std::vector<uint8_t> row(d.Width * 4);
            for (uint32_t y = 0; y < d.Height; y++) {
                const uint8_t* src = (const uint8_t*)ms.pData + y * ms.RowPitch;
                for (uint32_t x = 0; x < d.Width; x++) {
                    uint8_t r, g, b;
                    if (d.Format == DXGI_FORMAT_R32_FLOAT) { float v; memcpy(&v, src + x * 4, 4); r = g = b = (uint8_t)std::clamp(v * 255.0f, 0.0f, 255.0f); }
                    else { r = src[x * 4]; g = src[x * 4 + 1]; b = src[x * 4 + 2]; }
                    row[x * 4] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        g_ctx->Unmap(st.Get(), 0);
    }
    fprintf(stderr, "[gfx] dumped %zu surfaces at frame %llu\n", g_surfaces.size(), (unsigned long long)g_frame);
}

// Between frames: switch display mode if asked to, and match the back buffer to the window.
static void update_display() {
    static WINDOWPLACEMENT savedPlace{ sizeof(WINDOWPLACEMENT) };
    static LONG_PTR savedStyle = 0;
    int want = g_modeWanted.load();
    if (g_modeNow == kExclusive && want == kExclusive) {                    // Alt+Tab or another app can end exclusive mode behind our back
        BOOL fs = FALSE;
        if (SUCCEEDED(g_swap->GetFullscreenState(&fs, nullptr)) && !fs) {
            fprintf(stderr, "[display] exclusive fullscreen was lost, back to a window\n");
            g_modeNow = kWindowed; g_modeWanted = want = kWindowed; g_sizeChanged = true;
        }
    }
    if (want != g_modeNow) {
        if (g_modeNow == kExclusive) g_swap->SetFullscreenState(FALSE, nullptr);
        if (g_modeNow == kBorderless) {
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, savedStyle);
            SetWindowPlacement(g_hwnd, &savedPlace);
            SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        if (want == kBorderless) {
            savedStyle = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
            GetWindowPlacement(g_hwnd, &savedPlace);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        if (want == kExclusive) {
            HRESULT hr = g_swap->SetFullscreenState(TRUE, nullptr);
            if (FAILED(hr)) { fprintf(stderr, "[display] exclusive fullscreen failed: %08lX\n", hr); g_modeWanted = want = kWindowed; }
        }
        fprintf(stderr, "[display] mode %d -> %d\n", g_modeNow, want);
        g_modeNow = want; g_sizeChanged = true;
    }
    if (!g_sizeChanged.exchange(false)) return;
    RECT rc{}; GetClientRect(g_hwnd, &rc);
    uint32_t w = (uint32_t)(rc.right - rc.left), h = (uint32_t)(rc.bottom - rc.top);
    if (w == 0 || h == 0 || (w == g_backW && h == g_backH)) return;         // minimised, or nothing to do
    ID3D11RenderTargetView* none = nullptr;
    g_ctx->OMSetRenderTargets(1, &none, nullptr);
    invalidate_binds();
    g_backRtv.Reset(); g_backTex.Reset();
    g_ctx->Flush();
    HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) fprintf(stderr, "[display] ResizeBuffers %ux%u failed: %08lX\n", w, h, hr);
    else { g_backW = w; g_backH = h; fprintf(stderr, "[display] back buffer %ux%u\n", w, h); }
    g_swap->GetBuffer(0, IID_PPV_ARGS(&g_backTex));
    g_dev->CreateRenderTargetView(g_backTex.Get(), nullptr, &g_backRtv);
}

static void do_swap(uint32_t front) {
    update_display();
    if ((int64_t)g_frame == g_dumpFrame) { probeq_report(); dump_surfaces(); g_trace = 0; }
    if ((int64_t)g_frame + 1 == g_dumpFrame) g_trace = 100000;
    uint32_t fp = front & 0x1FFFFFFFu;
    Surface* s = surface_at(fp);
    static int nl = 0;
    if (nl++ < 24) fprintf(stderr, "[gfx] swap front=%08X surface=%s draws=%llu resolves=%llu t=%lu\n", front, s ? "yes" : "no", (unsigned long long)g_drawsFrame, (unsigned long long)g_resolves, GetTickCount());
    if (s) {
        if (s->base != fp) logf_once("frontoff", "front buffer %X inside surface %X", fp, s->base);
        // A swapped resolve holds what guest memory would hold (B in the low byte); scanout undoes that.
        static const bool kNoRamp = getenv("BCR2_NOGAMMARAMP") != nullptr;
        if (g_lutDirty && g_lutTex) {
            uint32_t texel[256];
            for (int i = 0; i < 256; i++) {                                 // register holds red in the high bits, the texture in the low bits
                uint32_t v = g_lutEntries[i];
                texel[i] = ((v >> 20) & 0x3FF) | (((v >> 10) & 0x3FF) << 10) | ((v & 0x3FF) << 20) | (3u << 30);
            }
            g_ctx->UpdateSubresource(g_lutTex.Get(), 0, nullptr, texel, sizeof(texel), 0);
            g_lutDirty = false;
            static int nl = 0;
            if (nl++ < 4) fprintf(stderr, "[gfx] display gamma ramp loaded: in 64 -> %u, 128 -> %u, 192 -> %u, 255 -> %u (of 1023)\n", g_lutEntries[64] & 0x3FF, g_lutEntries[128] & 0x3FF, g_lutEntries[192] & 0x3FF, g_lutEntries[255] & 0x3FF);
        }
        bool useRamp = g_lutSet && !kNoRamp && g_psPresentLut && g_lutSrv;
        // Keep 16:9 in whatever size the back buffer is now, with black bars where it doesn't fit.
        const float fit = std::min(g_backW / 1280.0f, g_backH / 720.0f);
        const int pw = std::max((int)std::lround(1280.0f * fit), 1), ph = std::max((int)std::lround(720.0f * fit), 1);
        const int px = ((int)g_backW - pw) / 2, py = ((int)g_backH - ph) / 2;
        const float black[4] = { 0, 0, 0, 1 };
        if (pw != (int)g_backW || ph != (int)g_backH) g_ctx->ClearRenderTargetView(g_backRtv.Get(), black);
        const float sxs = (float)s->w / pw, sys = (float)s->h / ph, ofs[2] = { -px * sxs, -py * sys };
        ID3D11SamplerState* presentSamp = g_presentSampler.Get();
        g_ctx->PSSetSamplers(0, 1, &presentSamp);
        blit(s->srv.Get(), useRamp ? g_psPresentLut.Get() : g_psPresentPlain.Get(), g_backRtv.Get(), g_backW, g_backH, px, py, px + pw, py + ph, 0, 0, s->swapped, sxs, sys, useRamp ? g_lutSrv.Get() : nullptr, ofs);
    } else {
        float c[4] = { 0.05f, 0.0f, 0.1f, 1 };
        g_ctx->ClearRenderTargetView(g_backRtv.Get(), c);
    }
    write_shot();
    HRESULT hr;
    LARGE_INTEGER presented; QueryPerformanceCounter(&presented);
    {   // A tick that follows the display can wait for its refresh: nothing piles up, and no frame is discarded when one arrives close to the deadline.
        static const int kForce = getenv("BCR2_PRESENTINTERVAL") ? atoi(getenv("BCR2_PRESENTINTERVAL")) : -1;
        UINT interval = kForce >= 0 ? (UINT)kForce : (g_presentFollowsDisplay.load() ? 1u : 0u);
        // A stall such as a load can leave one extra frame queued, and at a steady 1:1 rate that adds a full refresh of latency forever.
        // Every Present then blocks, so after 30 of those in a row, leave one frame out to empty the queue.
        static int blocked = 0;
        static const bool kNoDrain = getenv("BCR2_NODRAIN") != nullptr;
        if (interval == 1 && blocked >= 30 && !kNoDrain) {
            static uint64_t nDrain = 0;
            if (nDrain++ < 40) fprintf(stderr, "[disp] frame=%llu queue backed up, left one frame out (#%llu)\n", (unsigned long long)g_frame, (unsigned long long)nDrain);
            blocked = 0;
            hr = S_OK;
        } else {
            PerfScope perfPresent(g_tPresent); hr = g_swap->Present(interval, 0);
            LARGE_INTEGER done; QueryPerformanceCounter(&done);
            blocked = (double)(done.QuadPart - presented.QuadPart) * g_perfTicksToMs > 1.0 ? blocked + 1 : 0;
        }
    }
    {
        track_output();
        note_display_stats(presented, g_frame);
    }
    {
        static LARGE_INTEGER last{}; static uint64_t lastFrame = 0, lastDraws = 0, lastRes = 0;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        static LARGE_INTEGER prevSwap{}; static std::vector<float> intervals;
        if (prevSwap.QuadPart) intervals.push_back((float)((double)(now.QuadPart - prevSwap.QuadPart) * g_perfTicksToMs));
        {   // one line per late frame, saying where the time went
            static double pD = 0, pT = 0, pR = 0, pP = 0; static uint64_t pW0 = 0, pW1 = 0, pW2 = 0, pCompiles = 0, pUploads = 0, pUpBytes = 0;
            uint64_t w0 = g_waitTicks[0].load(), w1 = g_waitTicks[1].load(), w2 = g_waitTicks[2].load();
            double ms = prevSwap.QuadPart ? (double)(now.QuadPart - prevSwap.QuadPart) * g_perfTicksToMs : 0;
            if (ms > 20.0 && g_frame > 300) {
                static int nSpike = 0;
                if (nSpike++ < 400)
                    fprintf(stderr, "[spike] frame=%llu ms=%.1f draw=%.1f tex=%.1f res=%.1f pres=%.1f ring=%.1f reg=%.1f intr=%.1f compiles=%llu uploads=%llu upKB=%llu\n",
                            (unsigned long long)g_frame, ms, g_tDraw >= pD ? g_tDraw - pD : g_tDraw, g_tTex >= pT ? g_tTex - pT : g_tTex,
                            g_tResolve >= pR ? g_tResolve - pR : g_tResolve, g_tPresent >= pP ? g_tPresent - pP : g_tPresent,
                            (w0 >= pW0 ? w0 - pW0 : w0) * g_perfTicksToMs, (w1 >= pW1 ? w1 - pW1 : w1) * g_perfTicksToMs, (w2 >= pW2 ? w2 - pW2 : w2) * g_perfTicksToMs,
                            (unsigned long long)(g_nCompiles - pCompiles), (unsigned long long)(g_nUploads - pUploads), (unsigned long long)((g_upBytes - pUpBytes) / 1024));
            }
            pD = g_tDraw; pT = g_tTex; pR = g_tResolve; pP = g_tPresent; pW0 = w0; pW1 = w1; pW2 = w2; pCompiles = g_nCompiles; pUploads = g_nUploads; pUpBytes = g_upBytes;
        }
        prevSwap = now;
        if (g_frame + 1 - lastFrame >= 300) {
            uint64_t n = g_frame + 1 - lastFrame;
            if (!intervals.empty()) {                                        // pacing matters as much as the average
                std::sort(intervals.begin(), intervals.end());
                size_t c = intervals.size(), late = 0;
                for (float v : intervals) if (v > 20.0f) late++;
                fprintf(stderr, "[pace] frame=%llu p1=%.1f p50=%.1f p99=%.1f max=%.1f over20ms=%zu\n", (unsigned long long)g_frame, intervals[c / 100], intervals[c / 2], intervals[c * 99 / 100], intervals[c - 1], late);
                intervals.clear();
            }
            if (last.QuadPart) {
                double ms = (double)(now.QuadPart - last.QuadPart) * g_perfTicksToMs;
                double w0 = g_waitTicks[0].exchange(0) * g_perfTicksToMs, w1 = g_waitTicks[1].exchange(0) * g_perfTicksToMs, w2 = g_waitTicks[2].exchange(0) * g_perfTicksToMs;
                fprintf(stderr, "[perf] frame=%llu fps=%.1f ms/frame=%.2f draws=%.0f res=%.0f | draw=%.2f (tex=%.2f) resolve=%.2f present=%.2f | wait ring=%.2f reg=%.2f intr=%.2f\n",
                        (unsigned long long)g_frame, n * 1000.0 / ms, ms / n, (double)(g_draws - lastDraws) / n, (double)(g_resolves - lastRes) / n,
                        g_tDraw / n, g_tTex / n, g_tResolve / n, g_tPresent / n, w0 / n, w1 / n, w2 / n);
            }
            last = now; lastFrame = g_frame + 1; lastDraws = g_draws; lastRes = g_resolves;
            g_tDraw = g_tTex = g_tResolve = g_tPresent = 0;
        }
    }
    if (FAILED(hr)) logf_once("present", "Present failed: %08lX", hr);
    // Flip model unbinds the back buffer; keep our view tracking honest.
    invalidate_binds();
    g_frame++;
    g_drawsFrame = 0;
    if ((g_frame % 600) == 0) fprintf(stderr, "[gfx] frame=%llu draws=%llu resolves=%llu\n", (unsigned long long)g_frame, (unsigned long long)g_draws, (unsigned long long)g_resolves);
}

// ---- packets ----
void gpu_packet(uint8_t* base, uint32_t op, const uint32_t* b, uint32_t cnt) {
    static bool inited = false;
    if (!inited) {
        inited = true; g_base = base;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_perfTicksToMs = 1000.0 / (double)f.QuadPart;
        g_devOk = device_init();
    }
    if (!g_devOk) return;
    auto load_shader = [&](uint32_t type, const uint32_t* data, uint32_t n, bool fromMem, uint32_t addr) {
        auto& code = type == 0 ? g_vsCode : g_psCode;
        code.resize(n);
        if (fromMem) for (uint32_t i = 0; i < n; i++) code[i] = _byteswap_ulong(*(const uint32_t*)phys(addr + i * 4));
        else memcpy(code.data(), data, n * 4);
    };
    auto const_base = [](uint32_t ot, uint32_t& idx) {
        idx = ot & 0x7FF;
        switch ((ot >> 16) & 0xFF) {
          case 0: idx += R_CONST; return true;
          case 1: idx += R_FETCH; return true;
          case 2: idx += R_BOOL; return true;
          case 3: idx += R_LOOP; return true;
          case 4: idx += 0x2000; return true;
        }
        return false;
    };
    switch (op) {
      case 0x21: {                                                          // REG_RMW
        uint32_t ri = b[0], reg = ri & 0x1FFF, v = g_regs[reg];
        if (ri >> 31) v &= g_regs[b[1] & 0x1FFF]; else v &= b[1];
        if ((ri >> 30) & 1) v |= g_regs[b[2] & 0x1FFF]; else v |= b[2];
        gpu_reg(reg, v);
        break; }
      case 0x22: {                                                          // DRAW_INDX
        if (cnt < 2) break;
        uint32_t init = b[1], sel = (init >> 6) & 3;
        gpu_reg(0x21FC, init);
        if (sel == 0 && cnt >= 4) issue_draw(init, nullptr, b[2], b[3]);
        else if (sel == 2) issue_draw(init, nullptr, 0, 0);
        else issue_draw(init, cnt > 2 ? b + 2 : nullptr, 0, 0);
        break; }
      case 0x36: {                                                          // DRAW_INDX_2
        uint32_t init = b[0], sel = (init >> 6) & 3;
        gpu_reg(0x21FC, init);
        if (sel == 2) issue_draw(init, nullptr, 0, 0);
        else if (sel == 1) issue_draw(init, b + 1, 0, 0);
        else if (cnt >= 3) issue_draw(init, nullptr, b[1], b[2]);
        break; }
      case 0x2D: {                                                          // SET_CONSTANT
        uint32_t idx; if (!const_base(b[0], idx)) break;
        for (uint32_t k = 1; k < cnt; k++) gpu_reg(idx++, b[k]);
        break; }
      case 0x2F: {                                                          // LOAD_ALU_CONSTANT
        uint32_t idx; if (!const_base(b[1], idx)) break;
        uint32_t addr = b[0] & 0x3FFFFFFC, n = b[2] & 0xFFF;
        for (uint32_t k = 0; k < n; k++) gpu_reg(idx++, _byteswap_ulong(*(const uint32_t*)phys(addr + k * 4)));
        break; }
      case 0x55: case 0x56: {                                               // SET_CONSTANT2, SET_SHADER_CONSTANTS
        uint32_t idx = b[0] & 0xFFFF;
        for (uint32_t k = 1; k < cnt; k++) gpu_reg(idx++, b[k]);
        break; }
      case 0x27: load_shader(b[0] & 3, nullptr, b[1] & 0xFFFF, true, b[0] & ~3u); break;   // IM_LOAD
      case 0x2B: load_shader(b[0] & 3, b + 2, b[1] & 0xFFFF, false, 0); break;             // IM_LOAD_IMMEDIATE
      case 0x5B: {                                                          // EVENT_WRITE_ZPD: occlusion query report
        // The game draws glows and light halos only if its query reports visible samples. Answer as Xenia does by default: a
        // finished query (marked 0xFFFFFEED) gets a fixed count, so everything counts as visible. Fields are stored little-endian.
        static const bool kNoFake = getenv("BCR2_NOOCCLUSIONFAKE") != nullptr;
        uint32_t addr = g_regs[0x2325];                                     // RB_SAMPLE_COUNT_ADDR
        if (kNoFake || !addr) break;
        uint32_t* c = (uint32_t*)phys(addr & ~3u);                          // Total_A/B, ZFail_A/B, ZPass_A/B, StencilFail_A/B
        const uint32_t kDone = _byteswap_ulong(0xFFFFFEEDu);
        bool endZPass = c[4] == kDone && c[5] == kDone, endZFail = c[2] == kDone && c[3] == kDone;
        memset(c, 0, 32);
        if (endZPass || endZFail) { c[4] = 1000; c[0] = 1000; }
        static int nl = 0;
        if (nl++ < 4) fprintf(stderr, "[gfx] occlusion query report at %08X finished=%d\n", addr, (int)(endZPass || endZFail));
        break; }
      case 0x64: do_swap(b[1]); break;                                      // XE_SWAP
      default: break;
    }
}
