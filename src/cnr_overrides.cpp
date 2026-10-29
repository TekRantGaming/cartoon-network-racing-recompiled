// Runtime overrides for Cartoon Network Racing: library functions the ps2_analyzer signature DB missed, bound to the
// runtime's handlers. Bindings here take effect at load time without regenerating the recompiled code: every guest call
// goes through the function table that replaceFunction() patches. Evidence for each binding: docs/FINDINGS.md.
//
// Addresses are per ELF build (USA and EU are separate compiles), so each binding names both. 0 = not located yet.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <cstdint>
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

    void apply(PS2Runtime &runtime, bool eu)
    {
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
    }

    void applyUsa(PS2Runtime &runtime) { apply(runtime, false); }
    void applyEu(PS2Runtime &runtime) { apply(runtime, true); }
}

PS2_REGISTER_GAME_OVERRIDE("cartoon-network-racing-usa", "SLUS_214.38", 0x00100008u, 0u, applyUsa);
PS2_REGISTER_GAME_OVERRIDE("cartoon-network-racing-eu", "SLES_543.06", 0x00100008u, 0u, applyEu);
