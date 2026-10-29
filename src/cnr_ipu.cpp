// IPU through kzipu. Adapted from KillzoneRecomp's src/kz_ipu.cpp (GPL-3.0) without its Killzone movie-player shims.
#include "cnr_ipu.h"

#include "kzipu.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_ipu.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    PS2Runtime *g_runtime = nullptr;

    uint8_t *dmaPtr(void *, uint32_t addr, uint32_t bytes, bool)
    {
        if (!g_runtime)
            return nullptr;
        PS2Memory &mem = g_runtime->memory();
        if (addr & 0x80000000u) // SPR bit: scratchpad
        {
            const uint32_t off = addr & 0x3FF0u;
            return off + bytes <= PS2_SCRATCHPAD_SIZE ? mem.getScratchpad() + off : nullptr;
        }
        addr &= 0x1FFFFFF0u;
        return addr + bytes <= PS2_RAM_SIZE ? mem.getRDRAM() + addr : nullptr;
    }

    void onIntc(void *)
    {
        if (g_runtime)
            g_runtime->eeScheduler().dispatchIrq(false, 8u); // INTC_IPU
    }

    void onDmacIrq(void *, int channel)
    {
        ps2HostCompleteDmac(static_cast<uint32_t>(channel));
        if (g_runtime)
            g_runtime->drainCompletedDmacHandlers(g_runtime->memory().getRDRAM());
    }

    void onLog(void *, int level, const char *msg)
    {
        if (level >= 1)
            std::cerr << "[kzipu] " << msg << std::endl;
    }

    // CNR_IPU_STATS=1: once per second, the IPU commands issued (by type), DMA completions and the current state.
    const bool g_stats = std::getenv("CNR_IPU_STATS") && std::getenv("CNR_IPU_STATS")[0] == '1';
    uint32_t g_commands[16] = {};
    std::chrono::steady_clock::time_point g_statsLast = std::chrono::steady_clock::now();

    void statsTick()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_statsLast < std::chrono::seconds(1))
            return;
        g_statsLast = now;
        const KzipuStatus st = kzipuGetStatus();
        std::fprintf(stderr,
                     "[ipu] cmds idec=%u bdec=%u vdec=%u fdec=%u csc=%u pack=%u other=%u | intc=%u from-ends=%u to-ends=%u | "
                     "busy=%d cmd=%u in=%u out=%u waitIn=%d waitOut=%d ch3=%d ch4=%d\n",
                     g_commands[1], g_commands[2], g_commands[3], g_commands[4], g_commands[7], g_commands[8],
                     g_commands[0] + g_commands[5] + g_commands[6] + g_commands[9], st.intcCount, st.fromIpuEnds,
                     st.toIpuEnds, st.busy, st.command, st.inFifoQwc, st.outFifoQwc, st.waitingForInput,
                     st.waitingForOutput, st.fromIpuActive, st.toIpuActive);
        std::memset(g_commands, 0, sizeof(g_commands));
    }

    bool handles(uint32_t a) { return kzipuHandlesAddress(a); }
    uint32_t read32(uint32_t a)
    {
        kzipuStep();
        return kzipuReadReg32(a);
    }
    void write32(uint32_t a, uint32_t v)
    {
        if (g_stats && a == 0x10002000u) // IPU_CMD
            ++g_commands[v >> 28];
        kzipuWriteReg32(a, v);
        kzipuStep();
    }
    uint64_t read64(uint32_t a)
    {
        kzipuStep();
        return kzipuReadReg64(a);
    }
    void write64(uint32_t a, uint64_t v)
    {
        kzipuWriteReg64(a, v);
        kzipuStep();
    }
    void fifoWrite(const void *qw)
    {
        kzipuWriteInFifo(qw, 1);
        kzipuStep();
    }
    void fifoRead(void *qw)
    {
        kzipuStep();
        if (kzipuReadOutFifo(qw, 1) == 0)
            std::memset(qw, 0, 16);
    }
    void dmacEnable(bool enabled)
    {
        kzipuSetDmacEnabled(enabled);
        kzipuStep();
    }
    void poll()
    {
        kzipuStep();
        if (g_stats)
            statsTick();
    }
}

bool cnrIpuInstall()
{
    if (const char *mode = std::getenv("CNR_IPU"); mode && std::strcmp(mode, "off") == 0)
    {
        std::cout << "[cnr] IPU: off (CNR_IPU=off)" << std::endl;
        return false;
    }
    KzipuConfig cfg;
    cfg.dmaPtr = &dmaPtr;
    cfg.intc = &onIntc;
    cfg.dmacIrq = &onDmacIrq;
    cfg.log = &onLog;
    if (!kzipuInit(cfg))
    {
        std::cerr << "[cnr] IPU init failed" << std::endl;
        return false;
    }
    PS2HostIpu hooks;
    hooks.handles = &handles;
    hooks.read32 = &read32;
    hooks.write32 = &write32;
    hooks.read64 = &read64;
    hooks.write64 = &write64;
    hooks.fifoWrite = &fifoWrite;
    hooks.fifoRead = &fifoRead;
    hooks.dmacEnable = &dmacEnable;
    hooks.poll = &poll;
    ps2SetHostIpu(hooks);
    std::cout << "[cnr] IPU: PCSX2 IPU (kzipu)" << std::endl;
    return true;
}

void cnrIpuBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
}
