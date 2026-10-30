// SPU2 audio. Adapted from KillzoneRecomp's src/kz_audio.cpp (GPL-3.0) without its WAV tap and voice monitor.
#include "cnr_audio.h"

#include "kzspu2.h"

#include "ps2x/iop/iop_host_spu2.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kTargetFrames = kRate * 40 / 1000; // audio thread keeps ~40 ms queued
    constexpr uint32_t kHighFrames = kRate * 150 / 1000;  // above this (producer ahead of real time): skip to target

    SDL_AudioStream *g_stream = nullptr;
    float g_gain = 1.0f;

    // CNR_AUDIO_WAV=<file>: record the SPU2's mixed output (debug aid; the header is rewritten every second so the file
    // stays valid when the process is killed).
    FILE *g_wav = nullptr;
    uint64_t g_wavFrames = 0, g_wavHeaderAt = 0;

    void writeWavHeader(FILE *f, uint64_t frames)
    {
        const uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(frames * 4u, 0xFFFFFFFFull - 36u));
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        u32(36u + data);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(1);
        u16(2);
        u32(kRate);
        u32(kRate * 4);
        u16(4);
        u16(16);
        std::fwrite("data", 1, 4, f);
        u32(data);
        std::fseek(f, 0, SEEK_END);
    }

    void onSamples(void *, const int16_t *stereo, uint32_t frames)
    {
        if (!g_wav)
            return;
        std::fwrite(stereo, 4, frames, g_wav);
        g_wavFrames += frames;
        if (g_wavFrames - g_wavHeaderAt >= kRate)
        {
            g_wavHeaderAt = g_wavFrames;
            writeWavHeader(g_wav, g_wavFrames);
            std::fflush(g_wav);
        }
    }

    bool envFlag(const char *name, bool def)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return def;
        return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
    }

    // ---- SDL audio thread ---------------------------------------------------------------------------------------------
    void SDLCALL feed(void *, SDL_AudioStream *stream, int additional, int)
    {
        const uint32_t avail = kzspu2AvailableFrames();
        if (avail > kHighFrames)
            kzspu2SkipFrames(avail - kTargetFrames);

        int16_t buf[1024 * 2];
        uint32_t need = static_cast<uint32_t>(std::max(additional, 0)) / 4u;
        while (need)
        {
            const uint32_t chunk = std::min<uint32_t>(need, 1024);
            const uint32_t got = kzspu2ReadSamples(buf, chunk);
            if (got < chunk)
                std::memset(buf + got * 2, 0, (chunk - got) * 4);
            SDL_PutAudioStreamData(stream, buf, static_cast<int>(chunk * 4));
            need -= chunk;
        }
    }

    bool openDevice()
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            std::cerr << "[cnr_audio] SDL audio init failed: " << SDL_GetError() << std::endl;
            return false;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(kRate)};
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
        if (!g_stream)
        {
            std::cerr << "[cnr_audio] no audio device: " << SDL_GetError() << std::endl;
            return false;
        }
        SDL_SetAudioStreamGain(g_stream, g_gain);
        SDL_ResumeAudioStreamDevice(g_stream);
        std::cout << "[cnr_audio] output: " << SDL_GetCurrentAudioDriver() << ", 48 kHz stereo" << std::endl;
        return true;
    }

    // ---- pacing ---------------------------------------------------------------------------------------------------------
    // The runtime's EE clock can run ahead of wall time, and the SPU2 mixes by IOP time. The SPU2 gets its own clock
    // that advances by IOP time scaled by a factor <= 1 tracking wall time, so the device queue does not overfill.
    // The SPU2 still always moves with IOP time, so IOP code waiting on SPU2 DMA/IRQ keeps working. CNR_AUDIO_PACE=0 off.
    constexpr double kIopHz = 36864000.0;
    bool g_pace = true;
    double g_factor = 1.0;
    bool g_paceInit = false;
    uint64_t g_lastIop = 0;
    double g_spu = 0;
    std::chrono::steady_clock::time_point g_paceT0, g_paceLastAdjust;
    double g_spuAtT0 = 0;

    uint64_t toSpu(uint64_t iop)
    {
        if (!g_paceInit)
        {
            g_paceInit = true;
            g_lastIop = iop;
            g_spu = g_spuAtT0 = static_cast<double>(iop);
            g_paceT0 = g_paceLastAdjust = std::chrono::steady_clock::now();
        }
        if (iop > g_lastIop)
        {
            g_spu += static_cast<double>(iop - g_lastIop) * (g_pace ? g_factor : 1.0);
            g_lastIop = iop;
        }
        return static_cast<uint64_t>(g_spu);
    }

    void adjustPace()
    {
        if (!g_pace)
            return;
        static uint32_t calls = 0; // reading the clock on every IOP slice was measurable
        if ((++calls & 255u) != 0u)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_paceLastAdjust < std::chrono::milliseconds(50))
            return;
        g_paceLastAdjust = now;
        const double wall = g_spuAtT0 + std::chrono::duration<double>(now - g_paceT0).count() * kIopHz;
        const double ahead = (g_spu - wall) / kIopHz; // seconds the SPU2 is ahead of real time
        const double desired = std::clamp(1.0 - ahead / 2.0, 0.8, 1.0);
        g_factor += (desired - g_factor) * 0.25;
        if (ahead < -0.25) // the IOP fell behind real time (stall): do not try to catch up later
            g_spuAtT0 -= (-ahead - 0.25) * kIopHz;
    }

    // ---- IOP hooks ----------------------------------------------------------------------------------------------------
    uint16_t hookRead16(uint32_t a, uint64_t c) { return kzspu2Read16(a, toSpu(c)); }
    void hookWrite16(uint32_t a, uint16_t v, uint64_t c) { kzspu2Write16(a, v, toSpu(c)); }
    void hookDmaStart(int core, uint16_t *ram, uint32_t madr, uint32_t halfwords, bool toSpu2, uint64_t c)
    {
        if (toSpu2)
            kzspu2DmaWrite(core, ram, halfwords, madr, toSpu(c));
        else
            kzspu2DmaRead(core, ram, halfwords, madr, toSpu(c));
    }
    uint32_t hookDmaMadr(int core) { return kzspu2DmaMadr(core); }
    void hookAdvance(uint64_t c)
    {
        kzspu2Advance(toSpu(c));
        adjustPace();
    }
    // The SPU2's next event is in its own clock; the IOP wants its cycle.
    uint64_t hookNextEvent()
    {
        const uint64_t e = kzspu2NextEventCycle();
        if (e == UINT64_MAX)
            return e;
        if (static_cast<double>(e) <= g_spu)
            return g_lastIop + 1;
        const double f = g_pace ? g_factor : 1.0;
        return g_lastIop + static_cast<uint64_t>(std::ceil((static_cast<double>(e) - g_spu) / f));
    }

    void onIrq(void *) { ps2x::iop::iopSpu2RaiseIrq(); }
    void onDmaComplete(void *, int core) { ps2x::iop::iopSpu2DmaComplete(core); }
    void onLog(void *, int level, const char *msg)
    {
        if (level >= 1)
            std::cerr << "[kzspu2] " << msg << std::endl;
    }
}

void cnrAudioSetVolume(int percent)
{
    g_gain = static_cast<float>(percent) / 100.0f;
    if (g_stream)
        SDL_SetAudioStreamGain(g_stream, g_gain);
}

bool cnrAudioInstall(bool outputDevice)
{
    if (!envFlag("CNR_AUDIO", true))
    {
        std::cout << "[cnr_audio] CNR_AUDIO=0: no sound" << std::endl;
        return false;
    }
    g_pace = envFlag("CNR_AUDIO_PACE", true);

    KzSpu2Config cfg;
    cfg.irq = onIrq;
    cfg.dmaComplete = onDmaComplete;
    cfg.log = onLog;
    cfg.samples = onSamples;
    cfg.ringFrames = kRate / 2; // 500 ms
    if (const char *wav = std::getenv("CNR_AUDIO_WAV"); wav && *wav)
    {
        g_wav = std::fopen(wav, "wb");
        if (g_wav)
            writeWavHeader(g_wav, 0);
    }
    if (!kzspu2Init(cfg))
    {
        std::cerr << "[cnr_audio] kzspu2Init failed" << std::endl;
        return false;
    }

    ps2x::iop::IopHostSpu2 hooks;
    hooks.read16 = hookRead16;
    hooks.write16 = hookWrite16;
    hooks.dmaStart = hookDmaStart;
    hooks.dmaMadr = hookDmaMadr;
    hooks.advance = hookAdvance;
    hooks.nextEvent = hookNextEvent;
    ps2x::iop::iopSetHostSpu2(hooks);

    if (envFlag("CNR_AUDIO_DEVICE", outputDevice))
    {
        if (!openDevice())
            std::cout << "[cnr_audio] NO SOUND: the audio device could not be opened" << std::endl;
    }
    else
        std::cout << "[cnr_audio] no output device; SPU2 still emulated" << std::endl;
    return true;
}
