// GS output through PCSX2's hardware renderer (ext/kzgs).
// Adapted from KillzoneRecomp's src/kz_gs.cpp (GPL-3.0), without the Killzone-specific display fixes and trace tools.
#include "cnr_gs.h"

#include "kzgs.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_host_gs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <iostream>
#include <memory>
#include <mutex>

namespace
{
    // Delegates to the CPU backend for uploads/VRAM access but skips drawing and presentation, which kzgs does.
    class SkipDrawBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *vram, uint32_t vramSize) override { m_cpu.Initialize(vram, vramSize); }
        void Reset() override { m_cpu.Reset(); }
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override { m_cpu.LoadClut(tex0, texclut); }
        void BeginTransfer(const GSTransferCommand &command) override { m_cpu.BeginTransfer(command); }
        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { m_cpu.UploadImage(data, sizeBytes); }
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason reason) override { m_cpu.Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override { return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes); }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override { return m_cpu.ReadVram(psm, base, bw, x, y); }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override { m_cpu.WriteVram(psm, base, bw, x, y, value); }
        void SnapshotVram(std::vector<uint8_t> &out) const override { m_cpu.SnapshotVram(out); }
        GSTransferSnapshot GetTransferSnapshot() const override { return m_cpu.GetTransferSnapshot(); }

    private:
        GSCpuBackend m_cpu;
    };

    struct GsState
    {
        PS2Runtime *runtime = nullptr;
        CnrVideoConfig cfg;
        alignas(16) uint8_t privRegs[0x2000] = {};
        std::atomic<bool> attached{false};
        std::atomic<uint64_t> frames{0};
        std::atomic<uint64_t> packetsSinceVsync{0};

        std::mutex pendingMutex; // main thread -> GS feeding thread
        bool resizePending = false;
        int resizeW = 0, resizeH = 0;

        std::string shotDir;
        int shotInterval = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point nextShot = start;
        int shotIndex = 0;
    };

    GsState &gs()
    {
        static GsState s;
        return s;
    }

    KzgsConfig toKzgs(const CnrVideoConfig &c, int windowHeight)
    {
        KzgsConfig k;
        switch (c.renderer)
        {
        case CnrRenderer::D3D11: k.renderer = KzgsRenderer::D3D11; break;
        case CnrRenderer::D3D12: k.renderer = KzgsRenderer::D3D12; break;
        case CnrRenderer::Vulkan: k.renderer = KzgsRenderer::Vulkan; break;
        }
        // Auto: smallest multiple of 448 lines that covers the window height.
        k.upscale = c.upscale > 0 ? c.upscale : std::clamp((std::max(windowHeight, 448) + 447) / 448, 1, 8);
        k.vsync = c.vsync;
        k.aspect = KzgsAspect::Ratio4_3;
        // Off: full-frame output is never deinterlaced by PCSX2's Automatic mode, and other modes shift or blur it.
        k.interlaceMode = 1;
        // PCSX2's vertex-shader sprite expansion corrupts frames on NVIDIA under D3D11.
        k.vertexShaderExpand = k.renderer != KzgsRenderer::D3D11;
        return k;
    }

    // Runtime GSRegisters -> PCSX2 GSPrivRegSet layout (see kzgs.h), into `out` (0x2000 bytes).
    void snapshotPrivRegs(GsState &s, uint8_t *out)
    {
        const GSRegisters &r = s.runtime->memory().gs();
        auto put = [out](uint32_t off, uint64_t v) { std::memcpy(out + off, &v, 8); };
        // sceGsResetGraph is HLE in the runtime and never programs the CRTC sync registers. PCSX2 derives the video
        // mode from SMODE1.CMOD/LC and field flipping from SYNCV.VFP, so fall back to the values libgraph writes.
        // PAL differs from NTSC only in CMOD (3 instead of 2; PCSX2 GSRegs.h).
        constexpr uint64_t kNtscSmode1 = 0x0000000740834504ull; // CMOD=2 (NTSC), LC=32
        constexpr uint64_t kPalSmode1 = kNtscSmode1 | (1ull << 13); // CMOD=3 (PAL)
        constexpr uint64_t kNtscSyncv = 0x00C7800601A01801ull;  // VFP=1
        put(0x000, r.pmode);
        put(0x010, r.smode1 ? r.smode1 : (s.cfg.pal ? kPalSmode1 : kNtscSmode1));
        put(0x020, r.smode2);
        put(0x030, r.srfsh);
        put(0x040, r.synch1);
        put(0x050, r.synch2);
        put(0x060, r.syncv ? r.syncv : kNtscSyncv);
        put(0x070, r.dispfb1);
        put(0x080, r.display1);
        put(0x090, r.dispfb2);
        put(0x0A0, r.display2);
        put(0x0B0, r.extbuf);
        put(0x0C0, r.extdata);
        put(0x0D0, r.extwrite);
        put(0x0E0, r.bgcolor);
        put(0x1000, r.csr.load(std::memory_order_acquire));
        put(0x1010, r.imr);
        put(0x1040, r.busdir);
        put(0x1080, r.siglblid);
    }

    void saveShot(GsState &s)
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!kzgsReadback(rgba, w, h) || w <= 0 || h <= 0)
            return;
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFF;
        const long long secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s.start).count();
        char name[96];
        std::snprintf(name, sizeof(name), "/gs_%03d_t%llds_f%llu.png", s.shotIndex++, secs,
                      static_cast<unsigned long long>(s.frames.load(std::memory_order_relaxed)));
        if (SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4))
        {
            SDL_SavePNG(surf, (s.shotDir + name).c_str());
            SDL_DestroySurface(surf);
        }
    }

    // CNR_GS_FORMATS=1: every 5 s, a histogram of texture uploads (BITBLTBUF.DPSM per GIF path, with qwords) and of the
    // texture formats drawn with (TEX0 PSM / CPSM / CSM). Debug aid for texture corruption.
    struct FormatStats
    {
        bool enabled = std::getenv("CNR_GS_FORMATS") && std::getenv("CNR_GS_FORMATS")[0] == '1';
        std::map<std::tuple<int, uint32_t>, std::pair<uint64_t, uint64_t>> uploads; // (path, dpsm) -> (count, image qw)
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> tex0;           // (psm, cpsm, csm) -> count
        uint32_t lastDpsm[4] = {};
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    };

    FormatStats &formats()
    {
        static FormatStats f;
        return f;
    }

    void noteAd(FormatStats &f, int path, uint64_t val, uint64_t addr)
    {
        switch (addr & 0xFF)
        {
        case 0x50: // BITBLTBUF
            f.lastDpsm[path & 3] = static_cast<uint32_t>((val >> 56) & 0x3F);
            break;
        case 0x06: // TEX0_1
        case 0x07: // TEX0_2
            ++f.tex0[{static_cast<uint32_t>((val >> 20) & 0x3F), static_cast<uint32_t>((val >> 51) & 0xF),
                      static_cast<uint32_t>((val >> 55) & 1)}];
            break;
        default:
            break;
        }
    }

    void inspectFormats(int path, const uint8_t *data, uint32_t size)
    {
        FormatStats &f = formats();
        uint32_t off = 0;
        while (off + 16 <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + off, 8);
            std::memcpy(&hi, data + off + 8, 8);
            off += 16;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFF);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xF);
            if (nreg == 0)
                nreg = 16;
            if (flg == 0) // PACKED
            {
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16 <= size; ++r, off += 16)
                        if (((hi >> (4 * r)) & 0xF) == 0xE)
                        {
                            uint64_t val, addr;
                            std::memcpy(&val, data + off, 8);
                            std::memcpy(&addr, data + off + 8, 8);
                            noteAd(f, path, val, addr);
                        }
            }
            else if (flg == 1) // REGLIST
                off += ((nloop * nreg + 1) / 2) * 16;
            else // IMAGE
            {
                auto &u = f.uploads[{path, f.lastDpsm[path & 3]}];
                ++u.first;
                u.second += nloop;
                off += nloop * 16;
            }
            if ((lo >> 15) & 1)
                break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - f.last >= std::chrono::seconds(5))
        {
            f.last = now;
            std::string line = "[gsfmt] uploads(path:dpsm=count/qw):";
            for (const auto &[k, v] : f.uploads)
                line += " " + std::to_string(std::get<0>(k)) + ":" + std::to_string(std::get<1>(k)) + "=" +
                        std::to_string(v.first) + "/" + std::to_string(v.second);
            line += " | tex0(psm/cpsm/csm=count):";
            for (const auto &[k, v] : f.tex0)
                line += " " + std::to_string(std::get<0>(k)) + "/" + std::to_string(std::get<1>(k)) + "/" +
                        std::to_string(std::get<2>(k)) + "=" + std::to_string(v);
            std::fprintf(stderr, "%s\n", line.c_str());
            f.uploads.clear();
            f.tex0.clear();
        }
    }

    void onGifPacket(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (formats().enabled)
            inspectFormats(path, data, sizeBytes);
        kzgsGifTransfer(path, data, sizeBytes / 16u);
        // One thread feeds kzgs at a time (the VIF1 worker, or the EE thread with the worker idle): no locked add needed.
        GsState &s = gs();
        s.packetsSinceVsync.store(s.packetsSinceVsync.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    }

    struct VsyncJob
    {
        int field = 0;
        alignas(16) uint8_t regs[0x2000] = {};
    };

    // The GS half of a guest vblank, run after every GIF packet queued before it (PS2HostGs::vsyncOrdered).
    void runVsync(void *arg)
    {
        std::unique_ptr<VsyncJob> job(static_cast<VsyncJob *>(arg));
        GsState &s = gs();
        if (!s.attached.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard<std::mutex> lock(s.pendingMutex);
            if (s.resizePending)
            {
                kzgsResize(s.resizeW, s.resizeH);
                s.resizePending = false;
            }
        }
        std::memcpy(s.privRegs, job->regs, sizeof(s.privRegs));
        // Unchanged frames (no new packets, same display registers) are not presented again. A busy GS thread drops this
        // vblank's presentation instead of blocking the EE; the next vblank shows the newest frame.
        static uint8_t lastDisplayRegs[0xF0] = {};
        static bool presentedOnce = false;
        const bool newPackets = s.packetsSinceVsync.load(std::memory_order_relaxed) != 0;
        const bool regsChanged = std::memcmp(lastDisplayRegs, s.privRegs, sizeof(lastDisplayRegs)) != 0;
        if (!presentedOnce || newPackets || regsChanged)
        {
            if (kzgsVsyncTry(job->field, regsChanged || !presentedOnce))
            {
                s.packetsSinceVsync.store(0, std::memory_order_relaxed);
                std::memcpy(lastDisplayRegs, s.privRegs, sizeof(lastDisplayRegs));
                presentedOnce = true;
            }
        }
        s.frames.fetch_add(1, std::memory_order_relaxed);
        // CNR_GS_STATS=1: every 5 s, guest vblanks and game frames (vblanks with new GIF packets) per second.
        static const bool stats = std::getenv("CNR_GS_STATS") && std::getenv("CNR_GS_STATS")[0] == '1';
        if (stats)
        {
            static uint64_t flips = 0, vblanks = 0;
            static auto last = std::chrono::steady_clock::now();
            if (newPackets) // the game drew something since the previous vblank
                ++flips;
            ++vblanks;
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - last).count();
            if (secs >= 5.0)
            {
                std::fprintf(stderr, "[gsstats] vblank %.1f/s, game frames %.1f/s\n", vblanks / secs, flips / secs);
                flips = vblanks = 0;
                last = now;
            }
        }
        if (!s.shotDir.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= s.nextShot)
            {
                s.nextShot = now + std::chrono::seconds(s.shotInterval);
                saveShot(s);
            }
        }
    }

    void onVsync(uint64_t, int field)
    {
        GsState &s = gs();
        auto *job = new VsyncJob;
        job->field = field;
        snapshotPrivRegs(s, job->regs);
        ps2RunAfterQueuedGif(&runVsync, job);
    }

    void onLog(int level, const char *msg)
    {
        static const bool verbose = std::getenv("CNR_GS_DEBUG") && std::getenv("CNR_GS_DEBUG")[0] == '1';
        if (level <= 1 || verbose)
            std::cerr << "[kzgs] " << msg << std::endl;
    }
}

bool cnrGsAttach(PS2Runtime &runtime, void *hwnd, int windowWidth, int windowHeight, const CnrVideoConfig &cfg, std::string *error)
{
    (void)windowWidth;
    GsState &s = gs();
    s.runtime = &runtime;
    s.cfg = cfg;
    kzgsSetLogCallback(&onLog);
    snapshotPrivRegs(s, s.privRegs);
    const KzgsConfig kc = toKzgs(cfg, windowHeight);
    if (!kzgsOpen(hwnd, kc, s.privRegs, error))
        return false;
    runtime.gs().setRasterBackend(std::make_unique<SkipDrawBackend>());
    PS2HostGs hooks;
    hooks.gifPacket = &onGifPacket;
    hooks.vsync = &onVsync;
    hooks.vsyncOrdered = true;
    // kzgs alone consumes the GIF stream. CNR_SW_GS=1 keeps the runtime's software GS running in parallel (debug).
    hooks.exclusive = !(std::getenv("CNR_SW_GS") && std::getenv("CNR_SW_GS")[0] == '1');
    ps2SetHostGs(hooks);
    s.attached = true;
    static const char *kRendererNames[] = {"D3D11", "D3D12", "Vulkan"};
    std::cout << "[cnr] hardware GS attached (" << kRendererNames[static_cast<int>(kc.renderer)] << ", upscale "
              << kc.upscale << "x)" << std::endl;
    return true;
}

void cnrGsDetach()
{
    GsState &s = gs();
    if (!s.attached.load())
        return;
    ps2FlushQueuedGif(); // vsync jobs still queued on the VIF1 worker use kzgs
    s.attached = false;
    ps2SetHostGs(PS2HostGs{});
    kzgsClose();
}

void cnrGsResize(int width, int height)
{
    GsState &s = gs();
    std::lock_guard<std::mutex> lock(s.pendingMutex);
    s.resizePending = true;
    s.resizeW = width;
    s.resizeH = height;
}

void cnrGsSetScreenshotSchedule(const std::string &dir, int intervalSeconds)
{
    GsState &s = gs();
    s.shotDir = dir;
    s.shotInterval = std::max(1, intervalSeconds);
    s.start = std::chrono::steady_clock::now();
    s.nextShot = s.start + std::chrono::seconds(s.shotInterval);
    if (!dir.empty())
        std::filesystem::create_directories(dir);
}

uint64_t cnrGsFrameCount()
{
    return gs().frames.load(std::memory_order_relaxed);
}
