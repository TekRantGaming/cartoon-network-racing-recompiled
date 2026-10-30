// Cartoon Network Racing runner. Replaces ps2xRuntime/src/main.cpp.
//
// Usage: cnr.exe [--eu] [--elf <SLUS_214.38 | SLES_543.06>] [--iso <disc image>]
//                [--game_data_root=<dir>] [--user_data_root=<dir>] [--cnr_from_launcher=true]
// The TRG Launcher (launcher/project.cpp) starts the game with --game_data_root=<dir>, the folder holding the disc's small
// files and the image as disc.iso, and --cnr_from_launcher=true, which turns on holding Back and Start (or Esc) to go back
// to it. Its settings are in <exe dir>/<exe name>.toml; without that file the game uses the defaults and switches below.
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
//   CNR_TEST_BACKGROUND=1  the window stays where it opens and never takes the focus (launcher test runs)
//   CNR_GS=cpu             the runtime's software GS instead of PCSX2's (PS2X_HEADLESS_SHOTS for frames)
//   CNR_SAVE_DIR=<dir>     memory cards in <dir>/<USA|EU>/mc0, mc1 (default: saves/ next to cnr.exe)
//   CNR_WIDESCREEN=0       original 4:3 instead of 16:9 Hor+; CNR_HUD_CENTRE=1: the 16:9 HUD stays in the centre 4:3 area
//   CNR_GS_DUMP=<s>        write one frame's GIF packets (scripts/gsdump.py); CNR_FRAME_STATS=1: game frames/s

#include "cnr_audio.h"
#include "cnr_gs.h"
#include "cnr_input.h"
#include "cnr_ipu.h"
#include "cnr_profiler.h"
#include "cnr_settings.h"
#include "cnr_vu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_arena.h"
#include "runtime/ps2_host_gs.h"
#include "runtime/ps2_pad_provider.h"

#include <SDL3/SDL.h>
#include <xmmintrin.h>

#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    struct Options
    {
        std::filesystem::path elf = "extracted/usa/SLUS_214.38";
        std::filesystem::path iso = "game files/Cartoon Network Racing (USA).iso";
        std::filesystem::path userData; // saves, instead of the folder next to the game (the launcher's test runs)
        bool fromLauncher = false;
    };

    bool hasPrefix(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

    Options parseArgs(int argc, char *argv[])
    {
        Options opts;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (hasPrefix(arg, "--game_data_root="))
            {
                const std::filesystem::path root = std::filesystem::u8path(arg.substr(arg.find('=') + 1));
                opts.elf = root / (std::filesystem::exists(root / "SLES_543.06") ? "SLES_543.06" : "SLUS_214.38");
                opts.iso = root / "disc.iso";
            }
            else if (hasPrefix(arg, "--user_data_root="))
                opts.userData = std::filesystem::u8path(arg.substr(arg.find('=') + 1));
            else if (arg == "--cnr_from_launcher=true")
                opts.fromLauncher = true;
            else if (arg == "--elf" && i + 1 < argc)
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

    // Settings the launcher wrote that the game reads as environment switches (cnr_overrides.cpp). A switch already set wins.
    void settingToEnv(const char *name, const std::string &value)
    {
        if (!std::getenv(name) && !value.empty())
            _putenv_s(name, value.c_str());
    }

    void applyGameSettings()
    {
        if (!cnrSettingsLoaded())
            return;
        settingToEnv("CNR_WIDESCREEN", cnrSettingBool("cnr_widescreen", true) ? "1" : "0");
        settingToEnv("CNR_HUD_CENTRE", cnrSettingBool("cnr_hud_at_edges", true) ? "0" : "1");
        settingToEnv("CNR_RENDERER", cnrSetting("cnr_renderer"));
        const int upscale = cnrSettingInt("cnr_render_scale", 0);
        if (upscale > 0)
            settingToEnv("CNR_UPSCALE", std::to_string(upscale));
    }

    CnrVideoConfig videoConfig(bool pal, int outputWidth, int outputHeight)
    {
        CnrVideoConfig cfg;
        cfg.pal = pal;
        cfg.serial = pal ? "SLES-54306" : "SLUS-21438";
        cfg.outputWidth = outputWidth;
        cfg.outputHeight = outputHeight;
        if (const char *r = std::getenv("CNR_RENDERER"))
            cfg.renderer = _stricmp(r, "d3d12") == 0 ? CnrRenderer::D3D12 : _stricmp(r, "vulkan") == 0 ? CnrRenderer::Vulkan : CnrRenderer::D3D11;
        if (const char *u = std::getenv("CNR_UPSCALE"))
            cfg.upscale = std::atoi(u);
        cfg.widescreen = !(std::getenv("CNR_WIDESCREEN") && std::getenv("CNR_WIDESCREEN")[0] == '0');
        cfg.hudAtEdges = !envOn("CNR_HUD_CENTRE");
        cfg.vsync = cnrSettingBool("cnr_vsync", true);
        const std::string aa = cnrSetting("cnr_anti_aliasing", "none");
        cfg.antiAliasing = aa == "fxaa" ? CnrAntiAliasing::Fxaa : aa == "smaa" ? CnrAntiAliasing::Smaa : CnrAntiAliasing::Off;
        cfg.edgeAA = cnrSettingBool("cnr_edge_aa", false);
        cfg.textureFilter = cnrSettingInt("cnr_texture_filter", cfg.textureFilter);
        cfg.trilinear = cnrSettingInt("cnr_trilinear", cfg.trilinear);
        cfg.anisotropy = cnrSettingInt("cnr_anisotropy", cfg.anisotropy);
        cfg.mipmaps = cnrSettingBool("cnr_mipmaps", cfg.mipmaps);
        cfg.blending = cnrSettingInt("cnr_blending", cfg.blending);
        cfg.dithering = cnrSettingInt("cnr_dithering", cfg.dithering);
        cfg.sharpening = cnrSettingInt("cnr_sharpening", cfg.sharpening);
        const std::string scaling = cnrSetting("cnr_scaling", "smooth");
        cfg.scaling = scaling == "sharp" ? CnrPresentScaling::Sharp : scaling == "nearest" ? CnrPresentScaling::Nearest : CnrPresentScaling::Smooth;
        cfg.tvShader = cnrSettingInt("cnr_tv_shader", cfg.tvShader);
        cfg.brightness = cnrSettingInt("cnr_brightness", cfg.brightness);
        cfg.contrast = cnrSettingInt("cnr_contrast", cfg.contrast);
        cfg.saturation = cnrSettingInt("cnr_saturation", cfg.saturation);
        cfg.gamma = cnrSettingInt("cnr_gamma", cfg.gamma);
        cfg.halfPixelOffset = cnrSettingInt("cnr_half_pixel_offset", cfg.halfPixelOffset);
        cfg.nativeScaling = cnrSettingInt("cnr_native_scaling", cfg.nativeScaling);
        cfg.textureReplacements = cnrSettingBool("cnr_texture_replacements", false);
        cfg.dumpTextures = cnrSettingBool("cnr_dump_textures", false);
        cfg.showFps = cnrSettingBool("cnr_show_fps", false);
        return cfg;
    }

    std::filesystem::path exeDirectory()
    {
        wchar_t buffer[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::filesystem::path(buffer).parent_path() : std::filesystem::current_path();
    }

    // Memory cards (slot 1 = mc0, slot 2 = mc1) live next to the game: <exe dir>/saves/<USA|EU>/mc0, or under
    // CNR_SAVE_DIR. loadELF points them at the ELF's folder, so this runs after it. Saves made there by earlier builds
    // (extracted/<region>/mc0) are copied over once, while the new folder is still empty.
    void setupSaves(bool pal, const std::filesystem::path &userData)
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        const std::filesystem::path oldRoot = paths.mcRoot;
        const char *custom = std::getenv("CNR_SAVE_DIR");
        const std::filesystem::path base = custom && *custom ? std::filesystem::path(custom)
                                           : !userData.empty() ? userData
                                                               : exeDirectory() / "saves";
        const std::filesystem::path root = std::filesystem::absolute(base / (pal ? "EU" : "USA")).lexically_normal();
        std::error_code ec;
        const bool fresh = !std::filesystem::exists(root / "mc0", ec);
        std::filesystem::create_directories(root / "mc0", ec);
        std::filesystem::create_directories(root / "mc1", ec);
        if (fresh && !oldRoot.empty() && std::filesystem::is_directory(oldRoot, ec) && oldRoot != root / "mc0")
        {
            std::filesystem::copy(oldRoot, root / "mc0", std::filesystem::copy_options::recursive, ec);
            if (!ec)
                std::cout << "[cnr] copied existing saves from " << oldRoot.string() << std::endl;
        }
        paths.mcRoot = root / "mc0";
        PS2Runtime::setIoPaths(paths);
        std::cout << "[cnr] memory cards: " << root.string() << std::endl;
    }

    bool g_fromLauncher = false;

    void fail(SDL_Window *window, const std::string &msg)
    {
        std::cerr << "[cnr] " << msg << std::endl;
        if (window || g_fromLauncher)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartoon Network Racing", msg.c_str(), window);
    }

    // Where and how large the window opens: the launcher's Monitor, Window mode and Resolution rows. A resolution larger
    // than the screen still draws at that size; only the window stays on the screen.
    struct WindowPlan
    {
        int x = 0, y = 0, width = 1280, height = 720;
        bool fullscreen = false;
        int outputWidth = 0, outputHeight = 0; // the resolution picked in the launcher (0: the window's size)
    };

    WindowPlan planWindow(bool widescreen)
    {
        WindowPlan w;
        w.height = widescreen ? 720 : 960;
        int count = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        const int monitor = cnrSettingInt("monitor", 0);
        const SDL_DisplayID display = displays && count > 0 ? displays[monitor >= 0 && monitor < count ? monitor : 0] : SDL_GetPrimaryDisplay();
        SDL_free(displays);
        w.x = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        w.y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        if (!cnrSettingsLoaded())
            return w;
        SDL_Rect bounds{0, 0, 1920, 1080}, usable{0, 0, 1920, 1080};
        SDL_GetDisplayBounds(display, &bounds);
        SDL_GetDisplayUsableBounds(display, &usable);
        w.fullscreen = cnrSettingBool("fullscreen", true);
        w.outputWidth = cnrSettingInt("window_width", 0);
        w.outputHeight = cnrSettingInt("window_height", 0);
        if (w.outputWidth <= 0 || w.outputHeight <= 0)
        {
            w.outputWidth = w.fullscreen ? bounds.w : usable.w;
            w.outputHeight = w.fullscreen ? bounds.h : usable.h;
        }
        if (w.fullscreen)
        {
            w.width = bounds.w;
            w.height = bounds.h;
            return w;
        }
        // 4:3 keeps the picture's shape: the width follows the height.
        const int shapedWidth = widescreen ? w.outputWidth : w.outputHeight * 4 / 3;
        const float fit = std::min({1.0f, static_cast<float>(usable.w) / shapedWidth, static_cast<float>(usable.h) / w.outputHeight});
        w.width = static_cast<int>(shapedWidth * fit);
        w.height = static_cast<int>(w.outputHeight * fit);
        return w;
    }
}

int main(int argc, char *argv[])
{
    try
    {
        // The scheduler paces guest vblanks with OS sleeps. At Windows' default 15.6 ms timer resolution they overshoot
        // and vblanks arrive in uneven pairs (measured: median interval 15.5 ms, p90 29 ms), so the game misses frames.
        timeBeginPeriod(1);
        // Windows 11 ignores that request while the process has no visible window (minimised, occluded, or the hidden
        // CNR_HEADLESS window): idle waits then overslept a whole 15.6 ms tick and races ran at ~40 fps. Opt out.
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

        const Options opts = parseArgs(argc, argv);
        g_fromLauncher = opts.fromLauncher;
        const std::filesystem::path exeFile = [] {
            wchar_t buffer[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return std::filesystem::path(n > 0 && n < MAX_PATH ? buffer : L"cnr.exe");
        }();
        if (opts.fromLauncher)
        {
            // A window program started by the launcher has nowhere to print: keep the log beside the game.
            const std::filesystem::path logs = exeDirectory() / "logs";
            std::error_code ec;
            std::filesystem::create_directories(logs, ec);
            _wfreopen((logs / "game.log").c_str(), L"w", stdout);
            _wfreopen((logs / "game-errors.log").c_str(), L"w", stderr);
        }
        cnrLoadSettings(exeDirectory() / (exeFile.stem().wstring() + L".toml"));
        applyGameSettings();
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
        // The launcher's Frame rate row: anything but the disc's own rate goes through the same switch as CNR_FPS.
        if (const int rate = cnrSettingInt("cnr_frame_rate", 0); rate > 0 && rate != (pal ? 50 : 60))
            settingToEnv("CNR_FPS", std::to_string(rate));
        // CNR_FPS=<hz>: guest vblank rate (frame rate) other than the disc's 60/50 Hz.
        if (const char *fps = std::getenv("CNR_FPS"); fps && std::atoi(fps) > 0)
        {
            ps2SetVblankPeriodMicros(static_cast<uint32_t>(1000000 / std::atoi(fps)));
            std::cout << "[cnr] vblank rate " << std::atoi(fps) << " Hz" << std::endl;
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

        if (const char *prof = std::getenv("CNR_PROFILE"))
        {
            double start = 0, duration = 20;
            std::sscanf(prof, "%lf,%lf", &start, &duration);
            const char *out = std::getenv("CNR_PROFILE_OUT");
            cnrStartProfiler(start, duration, out && *out ? out : "build/runs/profile.txt");
        }

        cnrInputInit();
        if (opts.fromLauncher)
            cnrInputEnableReturnHold();
        ps2SetPadProvider(&cnrPadProvider);

        // Our SDL window + kzgs replace the runtime's raylib window, so the runtime always runs window-less.
        _putenv_s("PS2X_HEADLESS", "1");

        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            fail(nullptr, std::string("SDL video init failed: ") + SDL_GetError());
            return 1;
        }
        const bool widescreen = !(std::getenv("CNR_WIDESCREEN") && std::getenv("CNR_WIDESCREEN")[0] == '0');
        const bool testBackground = envOn("CNR_TEST_BACKGROUND");
        const WindowPlan plan = planWindow(widescreen);
        int winW = plan.width, winH = plan.height;
        const SDL_PropertiesID windowProps = SDL_CreateProperties();
        SDL_SetStringProperty(windowProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Cartoon Network Racing");
        SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_X_NUMBER, plan.x);
        SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_Y_NUMBER, plan.y);
        SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, winW);
        SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, winH);
        SDL_SetBooleanProperty(windowProps, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, headless);
        SDL_SetBooleanProperty(windowProps, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, !headless);
        SDL_SetBooleanProperty(windowProps, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, !headless);
        // Fullscreen is a borderless window over the whole screen; the Resolution row still sets the size the game draws at.
        SDL_SetBooleanProperty(windowProps, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, plan.fullscreen && !headless);
        SDL_Window *window = SDL_CreateWindowWithProperties(windowProps);
        SDL_DestroyProperties(windowProps);
        if (!window)
        {
            fail(nullptr, std::string("could not create the window: ") + SDL_GetError());
            return 1;
        }
        void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        SDL_GetWindowSizeInPixels(window, &winW, &winH);
        if (plan.fullscreen)
            SDL_HideCursor();
        // The launcher allowed this (AllowSetForegroundWindow): the game comes up in front of it.
        if (opts.fromLauncher && !testBackground && !headless)
            SetForegroundWindow(static_cast<HWND>(hwnd));

        std::string error;
        if (!cnrVuInstall(&error))
        {
            fail(window, "VU recompiler init failed: " + error);
            return 1;
        }
        cnrIpuInstall();
        cnrAudioInstall(!headless);
        cnrAudioSetVolume(cnrSettingBool("cnr_mute", false) ? 0 : cnrSettingInt("cnr_volume", 100));

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
        else if (!cnrGsAttach(runtime, hwnd, winW, winH, videoConfig(pal, plan.outputWidth, plan.outputHeight), &error))
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
        // The runtime's HLE pools (RPC/TLS tables, 0x31000 bytes) go into the unused EE kernel area as well, so the game's
        // heap runs up to its top-of-RAM stack as on a PS2 (512 KB more; races ran out of heap without it).
        if (!(std::getenv("CNR_POOLS") && std::getenv("CNR_POOLS")[0] == '0'))
            ps2SetRuntimePools(0x00020000u);
        if (!runtime.loadELF(elf.string()))
        {
            fail(window, "failed to load ELF: " + elf.string());
            return 1;
        }
        setupSaves(pal, opts.userData);

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
            if (cnrInputReturnRequested())
                runtime.requestStop();
            const auto now = std::chrono::steady_clock::now();
            if (headless && now >= nextHeartbeat)
            {
                nextHeartbeat += std::chrono::seconds(5);
                std::cout << "[cnr] t=" << std::chrono::duration_cast<std::chrono::seconds>(now - start).count()
                          << "s frames=" << cnrGsFrameCount() << std::endl;
            }
            // CNR_RAMDUMP=<s>[,<s>...]: write EE RAM to build/runs/ram_<s>.bin at those times (debugging; not synchronised).
            static std::vector<int> ramDumps = []() {
                std::vector<int> v;
                if (const char *e = std::getenv("CNR_RAMDUMP"))
                    for (const char *p = e; *p;)
                    {
                        v.push_back(std::atoi(p));
                        while (*p && *p != ',')
                            ++p;
                        if (*p == ',')
                            ++p;
                    }
                return v;
            }();
            if (!ramDumps.empty() && now - start >= std::chrono::seconds(ramDumps.front()))
            {
                const std::string name = "build/runs/ram_" + std::to_string(ramDumps.front()) + ".bin";
                if (FILE *f = std::fopen(name.c_str(), "wb"))
                {
                    std::fwrite(runtime.memory().getRDRAM(), 1, 32u << 20, f);
                    std::fclose(f);
                }
                std::cout << "[cnr] RAM dump " << name << " frames=" << cnrGsFrameCount() << std::endl;
                ramDumps.erase(ramDumps.begin());
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
