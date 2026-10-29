// Runtime overrides for Cartoon Network Racing: library functions the ps2_analyzer signature DB missed, bound to the
// runtime's handlers. Bindings here take effect at load time without regenerating the recompiled code: every guest call
// goes through the function table that replaceFunction() patches. Evidence for each binding: docs/FINDINGS.md.
//
// Addresses are per ELF build (USA and EU are separate compiles), so each binding names both. 0 = not located yet.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <iostream>
#include <string>

namespace
{
    struct Binding
    {
        const char *name;
        uint32_t usa; // SLUS_214.38
        uint32_t eu;  // SLES_543.06
        PS2Runtime::RecompiledFunction fn;
    };

    // Runtime stub handlers leave ctx->pc alone; a function entered straight from the scheduler (a guest yield exactly at
    // the call) would be entered again forever. Return to $ra like the guest function would.
    template <void (*Handler)(uint8_t *, R5900Context *, PS2Runtime *)>
    void returning(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        Handler(rdram, ctx, runtime);
        ctx->pc = ra;
    }

    constexpr Binding kBindings[] = {
        // sceSifGetSreg(reg): libsif reads its own sreg array; the runtime receives SIF_CMD_SET_SREG instead.
        // The boot waits in FUN_00226180 until sreg 31 (set by IOPMAIN.IRX) is non-zero.
        {"sceSifGetSreg", 0x0042F030u, 0x0042F030u, &returning<ps2_stubs::sceSifGetSreg>},
        // isceSifSendCmd: same arguments as sceSifSendCmd (0x42F678, DB-bound), interrupt flag set. Unbound, the recompiled
        // libsif DMAs the packet but no IOP handler runs. The EE answers IOPMAIN.IRX's file lookups (command 0x0C, from
        // its SIF command handler FUN_0021ff70) through it.
        {"isceSifSendCmd", 0x0042F6B8u, 0x0042F6B8u, &returning<ps2_stubs::sceSifSendCmd>},
    };

    // sceGsSyncV (DB-bound; USA 0x3B9DF8, EU 0x3B9DE0) is the game's vblank wait, called by its frame limiter
    // (FUN_0032e3e0, EU 0x32E340) once per game frame. Wrapped for the frame rate override and frame statistics.
    PS2Runtime::RecompiledFunction g_syncVOrig = nullptr;
    bool g_frameStats = false;

    // Frame rate (CNR_FPS=<hz>, cnr_main.cpp sets the vblank rate). The limiter measures a frame in vblanks (counter
    // 0x4C40D8) and turns it into seconds with a seconds-per-vblank float; the video mode setup FUN_0033c810 (EU 0x33C770)
    // stores the refresh rate (50, 59.94 or 60) and 1/rate in four globals. With the vblank rate raised they are set to
    // the new rate, so a game frame still advances the game by the real time it took. The mode setup can run again
    // (display mode changes), so they are re-applied every frame.
    // The game simulates in fixed steps of seconds-per-vblank (at least one per frame, FUN_003831f8), so with these set
    // it steps at the new rate; logic that turns durations into whole units per step needs help (fadeHook below).
    // docs/FINDINGS.md "Frame rates above 60".
    struct RateAddrs
    {
        uint32_t hz, secondsPerVblank, hzCopy, secondsPerVblankCopy;
    };
    constexpr RateAddrs kRateUsa{0x0059C220u, 0x0059B974u, 0x0059C214u, 0x0059BAD8u};
    constexpr RateAddrs kRateEu{0x0059C2A0u, 0x0059B9F4u, 0x0059C294u, 0x0059BB58u};
    RateAddrs g_rateAddrs{};
    float g_targetHz = 0.0f;

    void writeFloat(uint8_t *rdram, uint32_t addr, float value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    void applyFrameRate(uint8_t *rdram)
    {
        writeFloat(rdram, g_rateAddrs.hz, g_targetHz);
        writeFloat(rdram, g_rateAddrs.hzCopy, g_targetHz);
        writeFloat(rdram, g_rateAddrs.secondsPerVblank, 1.0f / g_targetHz);
        writeFloat(rdram, g_rateAddrs.secondsPerVblankCopy, 1.0f / g_targetHz);
    }

    // CNR_FRAME_STATS=1: every 5 s, game frames per second and how many vblanks each frame took (a histogram).
    void frameStats(PS2Runtime *runtime)
    {
        static uint64_t lastTick = 0, frames = 0, hist[5] = {};
        static auto last = std::chrono::steady_clock::now();
        const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        if (lastTick != 0)
            ++hist[std::min<uint64_t>(tick - lastTick, 4)];
        lastTick = tick;
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (const double secs = std::chrono::duration<double>(now - last).count(); secs >= 5.0)
        {
            std::fprintf(stderr, "[frames] %.1f game frames/s; vblanks per frame: 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu\n",
                         frames / secs, static_cast<unsigned long long>(hist[0]), static_cast<unsigned long long>(hist[1]),
                         static_cast<unsigned long long>(hist[2]), static_cast<unsigned long long>(hist[3]),
                         static_cast<unsigned long long>(hist[4]));
            frames = 0;
            std::fill(std::begin(hist), std::end(hist), 0ull);
            last = now;
        }
    }

    void syncVHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_targetHz > 0.0f)
            applyFrameRate(rdram);
        if (g_frameStats)
            frameStats(runtime);
        g_syncVOrig(rdram, ctx, runtime);
    }

    // 16:9 Hor+ (CNR_WIDESCREEN=0: off). The camera setup FUN_0033ecf8 (EU 0x33EC58) builds the projection from a
    // constant table K (USA 0x4C4C58, EU 0x4C4C68; = 1, 1, 4/3): x scale = K[1] / tan(fov / 2), y scale from K[0], K[2]
    // and the viewport. Nothing else reads K[1], so K[1] = 3/4 widens the horizontal view by 4/3 and keeps the vertical one.
    void applyWidescreen(PS2Runtime &runtime, bool eu)
    {
        const char *v = std::getenv("CNR_WIDESCREEN");
        if (v && v[0] == '0')
            return;
        const uint32_t k1 = eu ? 0x004C4C6Cu : 0x004C4C5Cu;
        uint8_t *rdram = runtime.memory().getRDRAM();
        float value;
        std::memcpy(&value, rdram + k1, sizeof(value));
        if (value != 1.0f)
        {
            std::cerr << "[cnr] widescreen: unexpected projection constant " << value << ", left alone" << std::endl;
            return;
        }
        value = 0.75f;
        std::memcpy(rdram + k1, &value, sizeof(value));
    }

    // HUD widget fades at raised frame rates. The HUD script loader (FUN_00305340) turns a fade's duration into a whole
    // alpha step per simulation step, (int)(alphaMax / (duration / step)) at +0x36B, and the fade (FUN_00303848, EU
    // 0x303780) adds it until alpha (+0x368) reaches its target (+0x369). With the step at 1/120 s the minimap's fade-in
    // step truncates from 1 to 0, the widget never reports its intro animation finished, and the race intro state never
    // starts the countdown (karts wait on the grid forever). A zero step on a fade that still has distance to go becomes 1.
    // Simulation steps per frame (FUN_003831f8, EU 0x383188), with CNR_FPS. The original: acc += frame time; steps =
    // trunc(acc / step), at least 1, at most 2, the fraction kept. Frames longer than two steps lose the excess. At 60 Hz
    // that is 2/60 s; at 120 Hz the same cap of 2 already loses time on ordinary 3-vblank hitches (race timer ~96% of
    // real time). The cap is scaled to keep the original maximum of 2/60 s per frame.
    struct StepAddrs
    {
        uint32_t fn, secondsPerStep, stepCopy, enabled, remainder;
    };
    constexpr StepAddrs kStepUsa{0x003831F8u, 0x0059B974u, 0x004C5670u, 0x004C5684u, 0x004C567Cu};
    constexpr StepAddrs kStepEu{0x00383188u, 0x0059B9F4u, 0x004C5684u, 0x004C5698u, 0x004C5690u};
    StepAddrs g_step{};
    float g_stepCap = 2.0f;

    void stepCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        auto rf = [rdram](uint32_t a) { float v; std::memcpy(&v, rdram + a, 4); return v; };
        const float step = rf(g_step.secondsPerStep);
        writeFloat(rdram, g_step.stepCopy, step);
        int32_t steps = 1;
        uint32_t enabled;
        std::memcpy(&enabled, rdram + g_step.enabled, 4);
        if (enabled != 0u)
        {
            float whole = 0.0f;
            const float frac = std::modf((ctx->f[12] + rf(g_step.remainder)) / step, &whole);
            writeFloat(rdram, g_step.remainder, frac * step);
            whole = std::min(whole, g_stepCap);
            steps = static_cast<int32_t>(whole);
            if (steps == 0)
                steps = 1;
        }
        setReturnS32(ctx, steps);
        ctx->pc = getRegU32(ctx, 31);
    }

    PS2Runtime::RecompiledFunction g_fadeOrig = nullptr;
    void fadeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint8_t *const w = rdram + (getRegU32(ctx, 4) & 0x01FFFFFFu);
        uint32_t enabled, fadingIn, fadingOut;
        std::memcpy(&enabled, w + 0x360, 4);
        std::memcpy(&fadingIn, w + 0x394, 4);
        std::memcpy(&fadingOut, w + 0x398, 4);
        if (enabled && (fadingIn || fadingOut) && w[0x36B] == 0u && w[0x368] != w[0x369])
            w[0x36B] = 1u;
        g_fadeOrig(rdram, ctx, runtime);
    }

    void apply(PS2Runtime &runtime, bool eu)
    {
        applyWidescreen(runtime, eu);
        for (const Binding &b : kBindings)
        {
            const uint32_t addr = eu ? b.eu : b.usa;
            if (addr == 0u)
            {
                std::cerr << "[cnr] override " << b.name << ": no address for this region yet" << std::endl;
                continue;
            }
            runtime.replaceFunction(addr, b.fn);
        }
        g_frameStats = std::getenv("CNR_FRAME_STATS") && std::getenv("CNR_FRAME_STATS")[0] == '1';
        if (const char *fps = std::getenv("CNR_FPS"); fps && std::atoi(fps) > 0)
        {
            g_targetHz = static_cast<float>(std::atoi(fps));
            g_rateAddrs = eu ? kRateEu : kRateUsa;
            const uint32_t fade = eu ? 0x00303780u : 0x00303848u;
            if ((g_fadeOrig = runtime.lookupFunction(fade)))
                runtime.replaceFunction(fade, &fadeHook);
            g_step = eu ? kStepEu : kStepUsa;
            g_stepCap = std::max(2.0f, std::round(2.0f * g_targetHz / 60.0f));
            runtime.replaceFunction(g_step.fn, &stepCount);
        }
        if (g_frameStats || g_targetHz > 0.0f)
        {
            const uint32_t syncV = eu ? 0x003B9DE0u : 0x003B9DF8u;
            g_syncVOrig = runtime.lookupFunction(syncV);
            if (g_syncVOrig)
                runtime.replaceFunction(syncV, &syncVHook);
        }
    }

    void applyUsa(PS2Runtime &runtime) { apply(runtime, false); }
    void applyEu(PS2Runtime &runtime) { apply(runtime, true); }
}

PS2_REGISTER_GAME_OVERRIDE("cartoon-network-racing-usa", "SLUS_214.38", 0x00100008u, 0u, applyUsa);
PS2_REGISTER_GAME_OVERRIDE("cartoon-network-racing-eu", "SLES_543.06", 0x00100008u, 0u, applyEu);
