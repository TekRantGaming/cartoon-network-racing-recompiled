// VU1/VU0 through ext/kzvu (PCSX2 microVU). Adapted from KillzoneRecomp's src/kz_vu.cpp (GPL-3.0) without its
// capture/verify debug paths.
#include "cnr_vu.h"

#include "kzvu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_vu.h"
#include "runtime/ps2_host_vu0.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    PS2Runtime *g_runtime = nullptr;
    uint64_t g_codeGeneration = ~0ull;
    uint64_t g_vu0CodeGeneration = ~0ull;
    constexpr uint32_t kBudget = 1u << 24;    // run VU1 programs to their E bit (the runtime's VIF1 is synchronous)
    constexpr uint32_t kVu0Budget = 1u << 20; // only stops a runaway VU0 loop

    void onXgkick(void *, const uint8_t *packet, uint32_t bytes, uint32_t)
    {
        if (g_runtime)
            g_runtime->memory().submitGifPacket(GifPathId::Path1, packet, bytes);
    }

    void prepare(uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t codeGeneration)
    {
        if (codeGeneration != g_codeGeneration)
        {
            kzvuMicroWritten(0, kKzvuCodeSize);
            g_codeGeneration = codeGeneration;
        }
        kzvuSetFBRST(fbrst);
        kzvuSetTop(top, itop);
    }

    void finish(uint32_t *vpuStat)
    {
        for (int i = 0; i < 16 && kzvuRunning() && (kzvuVpuStat() & 0x0600u) == 0u; ++i)
            kzvuContinue(kBudget);
        if (vpuStat)
            *vpuStat = (*vpuStat & ~0x0600u) | (kzvuVpuStat() & 0x0600u);
    }

    // MPG wrote this range of VU1 code memory (VIF1 worker thread, before any MSCAL that uses it): only the programs
    // overlapping it need recompiling.
    void onCodeWritten(uint32_t addr, uint32_t bytes)
    {
        kzvuMicroWritten(addr, bytes);
    }

    void onMscal(uint32_t startPc, uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t gen, uint32_t *vpuStat)
    {
        prepare(top, itop, fbrst, gen);
        kzvuExecute(startPc, kBudget);
        finish(vpuStat);
    }

    void onMscnt(uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t gen, uint32_t *vpuStat)
    {
        prepare(top, itop, fbrst, gen);
        kzvuExecute(0xFFFFFFFFu, kBudget);
        finish(vpuStat);
    }

    // VCALLMS / VCALLMSR: runs straight between the context's COP2 registers and VU0.
    void onVu0Call(R5900Context *ctx, uint32_t startPc, uint64_t codeGeneration)
    {
        if (codeGeneration != g_vu0CodeGeneration)
        {
            kzvu0MicroWritten(0, kKzvu0CodeSize);
            g_vu0CodeGeneration = codeGeneration;
        }
        Kzvu0Host host;
        host.vf = reinterpret_cast<uint32_t(*)[4]>(ctx->vu0_vf);
        host.vi = ctx->vi;
        host.acc = reinterpret_cast<uint32_t *>(&ctx->vu0_acc);
        host.status = &ctx->vu0_status;
        host.mac = &ctx->vu0_mac_flags;
        host.clip = &ctx->vu0_clip_flags;
        host.clip2 = &ctx->vu0_clip_flags2;
        host.r = reinterpret_cast<uint32_t *>(&ctx->vu0_r);
        host.i = reinterpret_cast<uint32_t *>(&ctx->vu0_i);
        host.q = reinterpret_cast<uint32_t *>(&ctx->vu0_q);
        const Kzvu0CallOut out = kzvu0Call(host, startPc, kVu0Budget, ctx->vu0_fbrst);
        ctx->vu0_tpc = out.tpc;
        ctx->vu0_pc = out.tpc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0xFFu) | out.vpuStat;
    }

    bool envIs(const char *name, const char *value)
    {
        const char *v = std::getenv(name);
        return v && std::strcmp(v, value) == 0;
    }
}

bool cnrVuInstall(std::string *error)
{
    const bool vu1Builtin = envIs("CNR_VU", "builtin");
    const bool vu0Builtin = envIs("CNR_VU0", "interp");
    if (vu1Builtin && vu0Builtin)
    {
        std::cout << "[cnr] VU0/VU1: PS2Recomp interpreters" << std::endl;
        return true;
    }

    KzvuConfig vc;
    vc.useJit = !envIs("CNR_VU", "interp");
    vc.vu0UseJit = !envIs("CNR_VU0", "pcsx2interp");
    // Clamp operands and results to +-max (the PS2 has no NaN/Inf). KillzoneRecomp needed this for VU0 because the
    // recompiled EE side can still produce IEEE specials; a safe default until CNR's needs are measured.
    vc.vu0ClampMode = 3;
    if (const char *c = std::getenv("CNR_VU0_CLAMP"))
        vc.vu0ClampMode = std::atoi(c);
    vc.xgkick = &onXgkick;
    if (!kzvuInit(vc, error))
        return false;
    if (!vu1Builtin)
    {
        PS2HostVu1 hooks;
        hooks.codeMem = kzvuCodeMem();
        hooks.dataMem = kzvuDataMem();
        hooks.mscal = &onMscal;
        hooks.mscnt = &onMscnt;
        if (!(std::getenv("CNR_VU_FULL_INVALIDATE") && std::getenv("CNR_VU_FULL_INVALIDATE")[0] == '1'))
            hooks.codeWritten = &onCodeWritten;
        // VU1 runs on the runtime's VIF1 worker thread; kzvu keeps the two VUs' VPU_STAT/FBRST apart per thread.
        hooks.bindWorkerThread = &kzvuBindVu1Thread;
        ps2SetHostVu1(hooks);
        std::cout << "[cnr] VU1: " << (vc.useJit ? "microVU recompiler" : "PCSX2 interpreter") << std::endl;
    }
    if (!vu0Builtin)
    {
        PS2HostVu0 hooks0;
        hooks0.codeMem = kzvu0CodeMem();
        hooks0.dataMem = kzvu0DataMem();
        hooks0.callms = &onVu0Call;
        ps2SetHostVu0(hooks0);
        std::cout << "[cnr] VU0: " << (vc.vu0UseJit ? "microVU recompiler" : "PCSX2 interpreter") << ", clamp mode "
                  << vc.vu0ClampMode << std::endl;
    }
    return true;
}

void cnrVuBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
}
