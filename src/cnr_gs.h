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

// Display settings. The future launcher will fill these; for now they come from defaults and CNR_* env vars.
struct CnrVideoConfig
{
    CnrRenderer renderer = CnrRenderer::D3D11;
    int upscale = 0; // 0 = auto from the window height
    bool vsync = true;
    bool pal = false; // EU disc: PAL CRTC fallbacks (50 Hz)
    // 16:9: frames with 3D (widened camera, src/cnr_overrides.cpp) are presented at 16:9 with their 2D overlay narrowed
    // back to its 4:3 proportions; 2D-only frames (movies, loading screens) stay 4:3, pillarboxed.
    bool widescreen = true;
    // widescreen: HUD elements in the outer quarters keep to the screen edges (false: everything in the centre 4:3 area).
    // 2D markers placed over world positions (e.g. an opponent tag) end up slightly off their target either way.
    bool hudAtEdges = true;
};

bool cnrGsAttach(PS2Runtime &runtime, void *hwnd, int windowWidth, int windowHeight, const CnrVideoConfig &cfg, std::string *error);
void cnrGsDetach();
void cnrGsResize(int width, int height);
// Save a PNG of the GS output to `dir` every `intervalSeconds` (headless debugging).
void cnrGsSetScreenshotSchedule(const std::string &dir, int intervalSeconds);
uint64_t cnrGsFrameCount();
