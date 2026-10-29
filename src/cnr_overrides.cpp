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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <iostream>

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

    // CNR_FRAME_STATS=1: wraps sceGsSyncV (the game's once-per-frame vblank wait) and prints, every 5 s, game frames
    // per second and how many vblanks each frame took (a histogram), to see whether the game keeps its frame rate.
    PS2Runtime::RecompiledFunction g_syncVOrig = nullptr;

    void syncVStats(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
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
        if (const char *v = std::getenv("CNR_FRAME_STATS"); v && v[0] == '1')
        {
            const uint32_t syncV = eu ? 0x003B9DE0u : 0x003B9DF8u; // sceGsSyncV (DB-bound)
            g_syncVOrig = runtime.lookupFunction(syncV);
            if (g_syncVOrig)
                runtime.replaceFunction(syncV, &syncVStats);
        }
    }

    void applyUsa(PS2Runtime &runtime) { apply(runtime, false); }
    void applyEu(PS2Runtime &runtime) { apply(runtime, true); }
}

PS2_REGISTER_GAME_OVERRIDE("cartoon-network-racing-usa", "SLUS_214.38", 0x00100008u, 0u, applyUsa);
PS2_REGISTER_GAME_OVERRIDE("cartoon-network-racing-eu", "SLES_543.06", 0x00100008u, 0u, applyEu);
