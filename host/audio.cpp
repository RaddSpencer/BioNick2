// Host sound output. The game mixes 256 samples per channel at 48 kHz, and the device's buffer level paces the game's audio callback,
// so the two clocks can't drift apart.
// Env switches: BCR2_NOSOUND (no device, keep the 5.33 ms clock), BCR2_AUDIODUMP=file (raw 48 kHz stereo floats as sent to the device).
#define NOMINMAX
#include "audio.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kFrameSamples = 256;
constexpr int kChannels = 6;
constexpr UINT32 kTargetFrames = 1536;                  // keep 32 ms queued: enough to ride out a late callback, small enough not to be heard as lag
const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

ComPtr<IAudioClient> g_client;
ComPtr<IAudioRenderClient> g_render;
UINT32 g_bufferFrames = 0;
ULONGLONG g_nextTry = 0;

void device_lost(const char* what, HRESULT hr) {
    fprintf(stderr, "[audio] %s failed: %08lX; retrying in 2 s\n", what, hr);
    g_render.Reset(); g_client.Reset();
    g_nextTry = GetTickCount64() + 2000;
}

bool device_open() {
    static const bool kNoSound = getenv("BCR2_NOSOUND") != nullptr;
    if (kNoSound) return false;
    if (g_client) return true;
    if (GetTickCount64() < g_nextTry) return false;
    g_nextTry = GetTickCount64() + 2000;
    static const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)g_client.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { device_lost("opening the default device", hr); return false; }
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; f.Format.nChannels = 2; f.Format.nSamplesPerSec = 48000; f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = 8; f.Format.nAvgBytesPerSec = 48000 * 8; f.Format.cbSize = sizeof(f) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32; f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; f.SubFormat = kSubtypeFloat;
    hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              1000000 /* 100 ms */, 0, &f.Format, nullptr);
    if (FAILED(hr)) { device_lost("IAudioClient::Initialize", hr); return false; }
    hr = g_client->GetBufferSize(&g_bufferFrames);
    if (FAILED(hr)) { device_lost("IAudioClient::GetBufferSize", hr); return false; }
    hr = g_client->GetService(IID_PPV_ARGS(&g_render));
    if (FAILED(hr)) { device_lost("IAudioClient::GetService", hr); return false; }
    hr = g_client->Start();
    if (FAILED(hr)) { device_lost("IAudioClient::Start", hr); return false; }
    fprintf(stderr, "[audio] output open: 48 kHz stereo, device buffer %u samples\n", g_bufferFrames);
    return true;
}

// Logged only when the set of channels carrying signal changes, plus now and then, so a long run stays readable.
void note_levels(const float peak[kChannels]) {
    static float win[kChannels]{};
    static uint64_t n = 0;
    static unsigned lastMask = 0xFFFFFFFFu;
    for (int c = 0; c < kChannels; c++) win[c] = std::max(win[c], peak[c]);
    if (++n % 375 != 0) return;                         // every 2 s
    unsigned mask = 0;
    for (int c = 0; c < kChannels; c++) if (win[c] > 1e-4f) mask |= 1u << c;
    static int nLog = 0;
    if (mask != lastMask || nLog++ < 8 || (n % 7500) == 0)
        fprintf(stderr, "[audio] peak L=%.3f R=%.3f C=%.3f LFE=%.3f SL=%.3f SR=%.3f t=%llu\n", win[0], win[1], win[2], win[3], win[4], win[5], (unsigned long long)GetTickCount64());
    lastMask = mask;
    for (float& v : win) v = 0;
}
}  // namespace

int audio_out_room() {
    if (!device_open()) return -1;
    UINT32 pad = 0;
    HRESULT hr = g_client->GetCurrentPadding(&pad);
    if (FAILED(hr)) { device_lost("reading the device level", hr); return -1; }
    {   // A device that stops taking samples (an endpoint change, a driver that went to sleep) leaves its buffer full for good: the game keeps running on its
        // own clock and the sound is gone. A buffer that stays full for 1.5 s is not draining, so close the device and open it again.
        static ULONGLONG fullSince = 0;
        const ULONGLONG now = GetTickCount64();
        if (pad >= kTargetFrames) {
            if (!fullSince) fullSince = now;
            else if (now - fullSince > 1500) { fullSince = 0; device_lost("the device stopped taking samples", S_OK); return -1; }
        } else fullSince = 0;
    }
    return pad >= kTargetFrames ? 0 : (int)((kTargetFrames - pad + kFrameSamples - 1) / kFrameSamples);
}

void audio_out_submit(const uint8_t* frame) {
    float ch[kChannels][kFrameSamples], peak[kChannels];
    const uint32_t* in = (const uint32_t*)frame;
    for (int c = 0; c < kChannels; c++) {
        float p = 0;
        for (int i = 0; i < kFrameSamples; i++) {
            uint32_t v = _byteswap_ulong(in[c * kFrameSamples + i]);
            memcpy(&ch[c][i], &v, 4);
            p = std::max(p, std::fabs(ch[c][i]));
        }
        peak[c] = p;
    }
    note_levels(peak);
    float out[kFrameSamples * 2];
    for (int i = 0; i < kFrameSamples; i++) {           // the usual 5.1 to stereo fold-down, without the sub
        out[i * 2]     = std::clamp(ch[0][i] + 0.707f * (ch[2][i] + ch[4][i]), -1.0f, 1.0f);
        out[i * 2 + 1] = std::clamp(ch[1][i] + 0.707f * (ch[2][i] + ch[5][i]), -1.0f, 1.0f);
    }
    static FILE* dump = getenv("BCR2_AUDIODUMP") ? fopen(getenv("BCR2_AUDIODUMP"), "wb") : nullptr;
    if (dump) fwrite(out, sizeof(float), kFrameSamples * 2, dump);
    if (!g_render) return;
    BYTE* dst = nullptr;
    HRESULT hr = g_render->GetBuffer(kFrameSamples, &dst);
    if (hr == AUDCLNT_E_BUFFER_TOO_LARGE) return;       // a full device buffer only happens after a clock fallback; drop this frame
    if (FAILED(hr)) { device_lost("getting a device buffer", hr); return; }
    memcpy(dst, out, sizeof(out));
    hr = g_render->ReleaseBuffer(kFrameSamples, 0);
    if (FAILED(hr)) device_lost("releasing a device buffer", hr);
}
