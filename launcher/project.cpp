// Cartoon Network Racing's launcher as a TRG Launcher 2.0 project (ext/trg-launcher/launcher2): everything specific to the
// game is in this one file. The USA and European discs are two games here, because each is its own recompiled program.
//
// The launcher writes games/<id>/<id>.toml; src/cnr_settings.cpp reads it at start. Every key below is read in
// src/cnr_main.cpp, src/cnr_input.cpp or src/cnr_gs.cpp. Run scripts/stage_launcher.py to put the games beside the launcher.

#include <trg/hub.h>

namespace {

using namespace trg::hub;

std::vector<SettingsSection> GameSettings(bool europe) {
  // Only rates that reach a race and hold their speed (docs/FINDINGS.md, Frame rates above 60). 240 stays on the first
  // Loading screen. 144 and 165 are not tested on the European disc yet.
  const std::vector<SettingOption> rates = europe
      ? std::vector<SettingOption>{{"50", "50"}, {"60", "60"}, {"90", "90"}, {"120", "120"}}
      : std::vector<SettingOption>{{"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"}};

  SettingsSection display = {"Display", {
      ChoiceRow("Window mode", "Fullscreen fills your screen. Windowed opens a window you can move and resize.",
                "fullscreen", "true", {{"true", "Fullscreen"}, {"false", "Windowed"}}),
      ResolutionRow("The size the game is drawn at and shown, and the size of its window when windowed. Higher sizes "
                    "look sharper and need a faster graphics card."),
      ChoiceRow("Monitor", "Which screen the game opens on.", "monitor", "0",
                {{"0", "Main"}, {"1", "Second"}, {"2", "Third"}, {"3", "Fourth"}}),
      ToggleRow("Vertical sync", "Matches the picture to your screen and stops tearing. Above your screen's refresh rate "
                "the game still runs at the frame rate you choose, but you only see what the screen can show.",
                "cnr_vsync", true),
      ChoiceRow("Frame rate", europe ? "50 is how the European disc ran on the PlayStation 2. Higher is smoother and the "
                                       "game still runs at the right speed."
                                     : "60 is how the game ran on the PlayStation 2. Higher is smoother and the game "
                                       "still runs at the right speed.",
                "cnr_frame_rate", europe ? "50" : "60", rates),
      ChoiceRow("Picture shape", "16:9 shows more of the track at the sides and keeps the menus and movies in their "
                "original shape. 4:3 is the original picture.", "cnr_widescreen", "true",
                {{"true", "16:9"}, {"false", "4:3 (original)"}}),
      ToggleRow("Race display at the screen edges", "In 16:9, the lap counter, position and minimap sit at the edges of "
                "the screen. Off keeps them in the middle 4:3 area.", "cnr_hud_at_edges", true)
          .When("cnr_widescreen", "true"),
      ToggleRow("Show frame rate", "Shows the frames per second in the corner while you play.", "cnr_show_fps", false),
  }};

  SettingsSection graphics = {"Graphics", {
      ChoiceRow("Renderer", "How the game draws. Direct3D 11 suits most PCs. Try another if the game runs badly.",
                "cnr_renderer", "d3d11", {{"d3d11", "Direct3D 11"}, {"d3d12", "Direct3D 12"}, {"vulkan", "Vulkan"}}),
      ChoiceRow("Render scale", "How many times the PlayStation 2's picture is enlarged. Match resolution follows the "
                "Resolution row under Display. A lower scale is faster and softer.", "cnr_render_scale", "0",
                {{"0", "Match resolution"}, {"1", "Original (1x)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"},
                 {"6", "6x"}, {"7", "7x"}, {"8", "8x"}}),
      ChoiceRow("Anti aliasing", "Smooths jagged edges. SMAA keeps more detail and works with Direct3D 11 only.",
                "cnr_anti_aliasing", "none", {{"none", "Off"}, {"fxaa", "FXAA"}, {"smaa", "SMAA"}}),
      ToggleRow("Edge smoothing", "The PlayStation 2's own edge smoothing, where the game asks for it.", "cnr_edge_aa",
                false),
      ChoiceRow("Texture filtering", "How textures are smoothed. PlayStation 2 smooths only what the game asked for. "
                "Nearest keeps hard pixels.", "cnr_texture_filter", "2",
                {{"2", "PlayStation 2"}, {"1", "Smooth everything"}, {"3", "Smooth, not sprites"}, {"0", "Nearest"}}),
      ChoiceRow("Trilinear filtering", "Softens the steps between texture detail levels.", "cnr_trilinear", "-1",
                {{"-1", "Automatic"}, {"0", "Off"}, {"1", "PlayStation 2"}, {"2", "Always"}}),
      ChoiceRow("Anisotropic filtering", "Keeps road and wall textures sharp when seen at an angle.", "cnr_anisotropy",
                "0", {{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}),
      ToggleRow("Mipmaps", "Uses the game's smaller texture levels in the distance, which stops shimmering.",
                "cnr_mipmaps", true),
      ChoiceRow("Blending accuracy", "How closely see-through effects follow the PlayStation 2. Higher needs a faster "
                "graphics card.", "cnr_blending", "1",
                {{"0", "Minimum"}, {"1", "Basic"}, {"2", "Medium"}, {"3", "High"}, {"4", "Full"}, {"5", "Maximum"}}),
      ChoiceRow("Dithering", "The PlayStation 2's fine pattern that hides colour banding.", "cnr_dithering", "2",
                {{"0", "Off"}, {"1", "Scaled"}, {"2", "Unscaled"}, {"3", "Force 32 bit"}}),
      SliderRow("Sharpening", "Sharpens the finished picture (AMD contrast adaptive sharpening).", "cnr_sharpening", "0",
                0, 100, 5, "%d%%"),
      ChoiceRow("Image scaling", "How the picture is fitted to the window. Sharp keeps pixel edges crisp.", "cnr_scaling",
                "smooth", {{"smooth", "Smooth"}, {"sharp", "Sharp"}, {"nearest", "Nearest"}}),
      ChoiceRow("TV filter", "Makes the picture look like an old television.", "cnr_tv_shader", "0",
                {{"0", "Off"}, {"1", "Scanlines"}, {"2", "Diagonal"}, {"3", "Triangular"}, {"4", "Wave"},
                 {"5", "CRT"}, {"6", "4x smoothing"}, {"7", "Nx smoothing"}}),
      HeadingRow("COLOUR"),
      SliderRow("Brightness", "50 is the original.", "cnr_brightness", "50", 0, 100, 1, "%d"),
      SliderRow("Contrast", "50 is the original.", "cnr_contrast", "50", 0, 100, 1, "%d"),
      SliderRow("Saturation", "50 is the original.", "cnr_saturation", "50", 0, 100, 1, "%d"),
      SliderRow("Gamma", "50 is the original.", "cnr_gamma", "50", 0, 100, 1, "%d"),
      HeadingRow("ADVANCED"),
      ChoiceRow("Half pixel offset", "Lines the picture up when it is enlarged. Native suits this game.",
                "cnr_half_pixel_offset", "4",
                {{"0", "Off"}, {"1", "Normal"}, {"2", "Special"}, {"3", "Special strong"}, {"4", "Native"},
                 {"5", "Native with texture offset"}}),
      ChoiceRow("Native scaling", "Draws glow and blur effects at the PlayStation 2's own size, so they keep their look "
                "when the picture is enlarged.", "cnr_native_scaling", "2",
                {{"0", "Off"}, {"1", "Normal"}, {"2", "Aggressive"}, {"3", "Normal upscaled"},
                 {"4", "Aggressive upscaled"}}),
      ToggleRow("Replacement textures", "Loads your own textures from the game folder's textures/<disc ID>/replacements.",
                "cnr_texture_replacements", false),
      ToggleRow("Save textures", "Saves every texture the game uses to textures/<disc ID>/dumps, as a start for "
                "replacements. Slows the game down.", "cnr_dump_textures", false),
  }};

  const std::vector<SettingOption> pad = {
      {"cross", "Cross"}, {"circle", "Circle"}, {"square", "Square"}, {"triangle", "Triangle"}, {"l1", "L1"},
      {"r1", "R1"}, {"l2", "L2"}, {"r2", "R2"}, {"l3", "L3"}, {"r3", "R3"}, {"start", "Start"}, {"select", "Select"},
      {"none", "Nothing"}};
  const char* acts = "Which PlayStation 2 button the game sees when you press this one.";
  SettingsSection controller = {"Controller", {
      SliderRow("Stick deadzone", "How far a stick moves before the game notices.", "cnr_deadzone", "15", 0, 50, 1,
                "%d%%"),
      SliderRow("Vibration", "How strongly the controller rumbles. The game's own Vibration option must be on too.",
                "cnr_vibration", "100", 0, 100, 5, "%d%%"),
      HeadingRow("BUTTON REMAPPING"),
      ChoiceRow("A acts as", acts, "cnr_remap_a", "cross", pad),
      ChoiceRow("B acts as", acts, "cnr_remap_b", "circle", pad),
      ChoiceRow("X acts as", acts, "cnr_remap_x", "square", pad),
      ChoiceRow("Y acts as", acts, "cnr_remap_y", "triangle", pad),
      ChoiceRow("Left bumper acts as", acts, "cnr_remap_lb", "l1", pad),
      ChoiceRow("Right bumper acts as", acts, "cnr_remap_rb", "r1", pad),
      ChoiceRow("Left trigger acts as", acts, "cnr_remap_lt", "l2", pad),
      ChoiceRow("Right trigger acts as", acts, "cnr_remap_rt", "r2", pad),
      ChoiceRow("Left stick click acts as", acts, "cnr_remap_ls", "l3", pad),
      ChoiceRow("Right stick click acts as", acts, "cnr_remap_rs", "r3", pad),
      ChoiceRow("Back acts as", acts, "cnr_remap_back", "select", pad),
      ChoiceRow("Start acts as", acts, "cnr_remap_start", "start", pad),
  }};

  SettingsSection keyboard = {"Keyboard", {
      ParagraphRow("The direction keys steer and move through the menus."),
      KeyRow("Up", "cnr_key_up", "Up"),
      KeyRow("Down", "cnr_key_down", "Down"),
      KeyRow("Left", "cnr_key_left", "Left"),
      KeyRow("Right", "cnr_key_right", "Right"),
      KeyRow("Cross", "cnr_key_cross", "Z"),
      KeyRow("Circle", "cnr_key_circle", "C"),
      KeyRow("Square", "cnr_key_square", "X"),
      KeyRow("Triangle", "cnr_key_triangle", "V"),
      KeyRow("L1", "cnr_key_l1", "Q"),
      KeyRow("R1", "cnr_key_r1", "E"),
      KeyRow("L2", "cnr_key_l2", "1"),
      KeyRow("R2", "cnr_key_r2", "3"),
      KeyRow("Start", "cnr_key_start", "Return"),
      KeyRow("Select", "cnr_key_select", "Backspace"),
  }};

  SettingsSection audio = {"Audio", {
      SliderRow("Volume", "How loud the game is.", "cnr_volume", "100", 0, 100, 5, "%d%%"),
      ToggleRow("Mute", "Turns all game sound off.", "cnr_mute", false),
  }};

  return {display, graphics, controller, keyboard, audio};
}

ProjectConfig CartoonNetworkRacing() {
  ProjectConfig p;
  p.name = "Cartoon Network Racing Recompiled";
  p.id = "cnr";
  p.version = PROJECT_VERSION;
  p.about = "Cartoon Network Racing as a native PC game, recompiled from the PlayStation 2 disc. It needs your own disc "
            "image: the USA and European discs each have their own version here.";
  p.made_by = "Eutechnyx for The Game Factory, 2006";
  p.console = "PlayStation 2";

  // Art: https://www.steamgriddb.com/game/38630. The two discs share it; GameInfo::tag tells them apart.
  GameInfo usa = {
      "cnr_usa", "Cartoon Network Racing (USA)", "cnr_usa.exe", "CartoonNetworkRacingUSA", 21438,
      ImVec4(0.96f, 0.72f, 0.08f, 1.0f), "Race the Cartoon Network stars.", InstallSource::kPs2Disc,
      38630, "grid/8fb512dd4fc5c4a606d8ebcffd81923d.png", "hero/3df63af7710213251997d6bafc62b084.jpg",
      "logo/6d6d94f587e2aada98f90bb9203381d1.png",
      "1.01", "Cartoon Network Racing (USA).iso", "3,674,963,968 bytes", "C842BFCB98E68F8D453A6D48BBDC8E6A3E35F3EC",
      "A0D9C48B96F5E58B1B9A4A6B5F4E1B85", "F0F4CC11", "", "about 3.7 GB"};
  usa.disc_serial = "SLUS_214.38";
  usa.installed_file = "SLUS_214.38";
  usa.saves_folder = "saves";
  usa.tag = "USA";
  usa.needs_avx2 = true;
  usa.settings = GameSettings(false);

  GameInfo eu = {
      "cnr_eu", "Cartoon Network Racing (Europe)", "cnr_eu.exe", "CartoonNetworkRacingEU", 54306,
      ImVec4(0.96f, 0.72f, 0.08f, 1.0f), "Race the Cartoon Network stars.", InstallSource::kPs2Disc,
      38630, "grid/8fb512dd4fc5c4a606d8ebcffd81923d.png", "hero/3df63af7710213251997d6bafc62b084.jpg",
      "logo/6d6d94f587e2aada98f90bb9203381d1.png",
      "1.00", "Cartoon Network Racing (Europe).iso", "3,672,604,672 bytes", "34696933D56D9D4FF4659AC1875470FE748CA069",
      "EA5F3488A8765E80B823563053D159C8", "1F30EF0C", "", "about 3.7 GB"};
  eu.disc_serial = "SLES_543.06";
  eu.installed_file = "SLES_543.06";
  eu.saves_folder = "saves";
  eu.tag = "Europe";
  eu.needs_avx2 = true;
  eu.settings = GameSettings(true);
  p.games = {usa, eu};

  // Releases: vX holds CartoonNetworkRacing-vX-windows-x64.zip for players; packages-vX holds what the launcher downloads
  // (CartoonNetworkRacing-, CartoonNetworkRacingUSA-, CartoonNetworkRacingEU-vX-windows-x64.zip, SHA256SUMS.txt). The repo
  // must be public: the launcher asks GitHub without signing in. scripts/make_release.py makes all of them.
  p.repo = "TekRantGaming/cartoon-network-racing-recompiled";
  p.packages_tag_prefix = "packages-v";
  p.launcher_zip = "CartoonNetworkRacing";
  p.launcher_exe = "cnr_launcher";
  p.linux_data_folder = "cartoon_network_racing";

  p.setting_prefix = "cnr_";
  p.from_launcher_flag = "--cnr_from_launcher=true";
  p.test_background_env = "CNR_TEST_BACKGROUND";
  p.release_server_env = "CNR_RELEASE_SERVER";
  p.features.frame_rate = false;
  p.features.render_quality = false;
  p.features.controller_options = false;
  p.features.achievement_toasts = false;
  p.features.output_resolution = false;
  p.features.achievements = false;

  p.discord_url = "https://discord.gg/8eakYSvBZS";
  p.steam_art_base = "https://raw.githubusercontent.com/TekRantGaming/cartoon-network-racing-recompiled/main/docs/steam/";
  return p;
}

}  // namespace

int main(int argc, char** argv) { return trg::hub::Run(CartoonNetworkRacing(), argc, argv); }
