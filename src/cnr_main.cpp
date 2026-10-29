// Cartoon Network Racing runner. Replaces ps2xRuntime/src/main.cpp.
//
// Usage: cnr.exe [--eu] [--elf <SLUS_214.38 | SLES_543.06>] [--iso <disc image>]
// Defaults (development layout, relative to the working directory):
//   --elf extracted/usa/SLUS_214.38          the ELF's folder is mounted as cdrom0: (MODULES/, CDFILES.DAT, ...)
//   --iso "game files/Cartoon Network Racing (USA).iso"   backs raw sceCdRead sector reads (ARCHIVE.AR, movies)
//   --eu   the EU defaults instead: extracted/eu/SLES_543.06 and "game files/Cartoon Network Racing (Europe).iso"
// The EU (SLES) ELF runs as PAL: 50 Hz vblank and PAL display fallbacks. It needs an EU build (generated/eu).
//
// Development switches (env):
//   CNR_HEADLESS=1         hidden window, no audio device (automated runs)
//   CNR_SECONDS=<n>        stop after n seconds
//   CNR_SHOT_DIR=<dir>     save the GS output as PNG every CNR_SHOT_INTERVAL seconds (default 5)
//   CNR_RENDERER=d3d11|d3d12|vulkan, CNR_UPSCALE=<1..8>
//   CNR_GS=cpu             the runtime's software GS instead of PCSX2's (PS2X_HEADLESS_SHOTS for frames)

#include "cnr_audio.h"
#include "cnr_gs.h"
#include "cnr_input.h"
#include "cnr_ipu.h"
#include "cnr_vu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_arena.h"
#include "runtime/ps2_host_gs.h"
#include "runtime/ps2_pad_provider.h"

#include <SDL3/SDL.h>
#include <xmmintrin.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    struct Options
    {
        std::filesystem::path elf = "extracted/usa/SLUS_214.38";
        std::filesystem::path iso = "game files/Cartoon Network Racing (USA).iso";
    };

    Options parseArgs(int argc, char *argv[])
    {
        Options opts;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--elf" && i + 1 < argc)
                opts.elf = argv[++i];
            else if (arg == "--iso" && i + 1 < argc)
                opts.iso = argv[++i];
            else if (arg == "--eu")
            {
                opts.elf = "extracted/eu/SLES_543.06";
                opts.iso = "game files/Cartoon Network Racing (Europe).iso";
            }
            else
                std::cerr << "[cnr] ignoring unknown argument: " << arg << std::endl;
        }
        return opts;
    }

    bool envOn(const char *name)
    {
        const char *v = std::getenv(name);
        return v && v[0] == '1';
    }

    CnrVideoConfig videoConfigFromEnv(bool pal)
    {
        CnrVideoConfig cfg;
        cfg.pal = pal;
        if (const char *r = std::getenv("CNR_RENDERER"))
            cfg.renderer = _stricmp(r, "d3d12") == 0 ? CnrRenderer::D3D12 : _stricmp(r, "vulkan") == 0 ? CnrRenderer::Vulkan : CnrRenderer::D3D11;
        if (const char *u = std::getenv("CNR_UPSCALE"))
            cfg.upscale = std::atoi(u);
        return cfg;
    }

    void fail(SDL_Window *window, const std::string &msg)
    {
        std::cerr << "[cnr] " << msg << std::endl;
        if (window)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartoon Network Racing", msg.c_str(), window);
    }
}

int main(int argc, char *argv[])
{
    try
    {
        const Options opts = parseArgs(argc, argv);
        const bool headless = envOn("CNR_HEADLESS");
        const std::filesystem::path elf = std::filesystem::absolute(opts.elf).lexically_normal();
        if (!std::filesystem::exists(elf))
        {
            fail(nullptr, "ELF not found: " + elf.string());
            return 1;
        }

        // SLES_543.06 (EU) is a PAL build: 50 Hz vblank. SLUS_214.38 (USA) runs at the runtime's NTSC default.
        const bool pal = elf.filename().string().rfind("SLES", 0) == 0;
        if (pal)
        {
            ps2SetVblankPeriodMicros(20000u);
            std::cout << "[cnr] PAL disc (EU): 50 Hz" << std::endl;
        }

        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (!opts.iso.empty() && std::filesystem::exists(opts.iso))
        {
            paths.cdImage = std::filesystem::absolute(opts.iso).lexically_normal();
            std::cout << "[cnr] CD image: " << paths.cdImage.string() << std::endl;
        }
        else
            std::cout << "[cnr] no CD image (" << opts.iso.string() << "): raw sector reads will fail" << std::endl;
        PS2Runtime::setIoPaths(paths);

        cnrInputInit();
        ps2SetPadProvider(&cnrPadProvider);

        // Our SDL window + kzgs replace the runtime's raylib window, so the runtime always runs window-less.
        _putenv_s("PS2X_HEADLESS", "1");

        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            fail(nullptr, std::string("SDL video init failed: ") + SDL_GetError());
            return 1;
        }
        int winW = 1280, winH = 960;
        SDL_Window *window = SDL_CreateWindow("Cartoon Network Racing", winW, winH,
                                              headless ? SDL_WINDOW_HIDDEN : (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
        if (!window)
        {
            fail(nullptr, std::string("could not create the window: ") + SDL_GetError());
            return 1;
        }
        void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        SDL_GetWindowSizeInPixels(window, &winW, &winH);

        std::string error;
        if (!cnrVuInstall(&error))
        {
            fail(window, "VU recompiler init failed: " + error);
            return 1;
        }
        cnrIpuInstall();
        cnrAudioInstall(!headless);

        PS2Runtime runtime;
        cnrVuBindRuntime(runtime);
        cnrIpuBindRuntime(runtime);
        if (!runtime.initialize("Cartoon Network Racing"))
        {
            fail(window, "failed to initialize the PS2 runtime");
            return 1;
        }
        const bool cpuGs = std::getenv("CNR_GS") && std::strcmp(std::getenv("CNR_GS"), "cpu") == 0;
        if (cpuGs)
            std::cout << "[cnr] CNR_GS=cpu: using the runtime's software GS" << std::endl;
        else if (!cnrGsAttach(runtime, hwnd, winW, winH, videoConfigFromEnv(pal), &error))
        {
            fail(window, "could not start the renderer: " + error);
            return 1;
        }
        if (const char *dir = std::getenv("CNR_SHOT_DIR"))
            cnrGsSetScreenshotSchedule(dir, std::getenv("CNR_SHOT_INTERVAL") ? std::atoi(std::getenv("CNR_SHOT_INTERVAL")) : 5);

        // The game claims all remaining heap at start-up (FUN_0022a860 probes malloc from 128 MB down) and manages it
        // itself, so the runtime's own guest buffers and interrupt stacks must stay out of it: they go into the unused
        // area below the ELF (loaded at 0x100000).
        ps2SetRuntimeArena(0x00080000u, 0x000B0000u, 0x000B0000u, 0x00100000u);
        if (!runtime.loadELF(elf.string()))
        {
            fail(window, "failed to load ELF: " + elf.string());
            return 1;
        }

        std::atomic<bool> runtimeDone{false};
        std::thread runtimeThread([&]() {
            // The PS2 FPU/VUs flush denormals to zero; recompiled float code runs on SSE, so match it (FTZ | DAZ).
            _mm_setcsr(_mm_getcsr() | 0x8040u);
            runtime.run();
            runtimeDone.store(true);
        });

        const int stopAfter = std::getenv("CNR_SECONDS") ? std::atoi(std::getenv("CNR_SECONDS")) : 0;
        const auto start = std::chrono::steady_clock::now();
        auto nextHeartbeat = start + std::chrono::seconds(5);
        while (!runtimeDone.load())
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                cnrInputOnEvent(e);
                switch (e.type)
                {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    runtime.requestStop();
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    cnrGsResize(e.window.data1, e.window.data2);
                    break;
                default:
                    break;
                }
            }
            cnrInputPoll();
            const auto now = std::chrono::steady_clock::now();
            if (headless && now >= nextHeartbeat)
            {
                nextHeartbeat += std::chrono::seconds(5);
                std::cout << "[cnr] t=" << std::chrono::duration_cast<std::chrono::seconds>(now - start).count()
                          << "s frames=" << cnrGsFrameCount() << std::endl;
            }
            if (stopAfter > 0 && now - start >= std::chrono::seconds(stopAfter))
                runtime.requestStop();
            SDL_Delay(1);
        }
        runtime.requestStop();
        runtimeThread.join();
        cnrGsDetach();
        cnrInputShutdown();
        SDL_DestroyWindow(window);
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[cnr] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[cnr] fatal exception: unknown" << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
