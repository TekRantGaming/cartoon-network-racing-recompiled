#pragma once
// PCSX2's hardware GS renderer (ext/kzgs) attached to the runtime's GIF and vsync hooks.
#include <cstdint>
#include <string>

class PS2Runtime;

enum class CnrRenderer
{
    D3D11,
    D3D12,
    Vulkan,
};

enum class CnrAntiAliasing
{
    Off,
    Fxaa,
    Smaa, // Direct3D 11 only
};

enum class CnrPresentScaling
{
    Smooth,
    Sharp,
    Nearest,
};

// Display settings: from the launcher's settings file (cnr_main.cpp), then CNR_* env vars.
struct CnrVideoConfig
{
    CnrRenderer renderer = CnrRenderer::D3D11;
    int upscale = 0; // 0 = auto: the smallest multiple of the PS2 picture (640 x 448) that covers the output size
    int outputWidth = 0, outputHeight = 0; // the resolution picked in the launcher (0: the window's size)
    bool vsync = true;
    bool pal = false; // EU disc: PAL CRTC fallbacks (50 Hz)
    // 16:9: frames with 3D (widened camera, src/cnr_overrides.cpp) are presented at 16:9 with their 2D overlay narrowed
    // back to its 4:3 proportions; 2D-only frames (movies, loading screens) stay 4:3, pillarboxed.
    bool widescreen = true;
    // widescreen: HUD elements in the outer quarters keep to the screen edges (false: everything in the centre 4:3 area).
    // 2D markers placed over world positions (e.g. an opponent tag) end up slightly off their target either way.
    bool hudAtEdges = true;

    CnrAntiAliasing antiAliasing = CnrAntiAliasing::Off;
    bool edgeAA = false;
    int textureFilter = 2; // KzgsTextureFilter
    int trilinear = -1;
    int anisotropy = 0;
    bool mipmaps = true;
    int blending = 1;
    int dithering = 2;
    int sharpening = 0;
    CnrPresentScaling scaling = CnrPresentScaling::Smooth;
    int tvShader = 0;
    int brightness = 50, contrast = 50, saturation = 50, gamma = 50;
    int halfPixelOffset = 4;
    int nativeScaling = 2;
    bool textureReplacements = false;
    bool dumpTextures = false;
    bool showFps = false;
    const char *serial = "SLUS-21438";
};

bool cnrGsAttach(PS2Runtime &runtime, void *hwnd, int windowWidth, int windowHeight, const CnrVideoConfig &cfg, std::string *error);
void cnrGsDetach();
void cnrGsResize(int width, int height);
// Save a PNG of the GS output to `dir` every `intervalSeconds` (headless debugging).
void cnrGsSetScreenshotSchedule(const std::string &dir, int intervalSeconds);
uint64_t cnrGsFrameCount();
