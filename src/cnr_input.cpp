#include "cnr_input.h"

#include "cnr_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // DualShock 2 button bits (active low on the wire).
    enum : uint16_t
    {
        PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
        PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
        PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
        PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
    };

    // The launcher's button names: cnr_key_<name>, cnr_remap_<name> and their values.
    struct PadName
    {
        const char *name;
        uint16_t bit;
        const char *defaultKeys;
    };
    // Default keyboard layout. The direction keys drive both the d-pad and the left stick (menus use one, racing the other).
    constexpr PadName kPadNames[] = {
        {"up", PAD_UP, "Up"},         {"down", PAD_DOWN, "Down"},   {"left", PAD_LEFT, "Left"}, {"right", PAD_RIGHT, "Right"},
        {"cross", PAD_CROSS, "Z"},    {"circle", PAD_CIRCLE, "C"},  {"square", PAD_SQUARE, "X"}, {"triangle", PAD_TRIANGLE, "V"},
        {"l1", PAD_L1, "Q"},          {"r1", PAD_R1, "E"},          {"l2", PAD_L2, "1"},        {"r2", PAD_R2, "3"},
        {"l3", PAD_L3, ""},           {"r3", PAD_R3, ""},           {"start", PAD_START, "Return"}, {"select", PAD_SELECT, "Backspace"},
    };

    uint16_t padBit(const std::string &name)
    {
        for (const PadName &p : kPadNames)
            if (name == p.name)
                return p.bit;
        return 0;
    }

    // The launcher's key names (settings_screen.cpp). Mouse buttons are scancodes past the keyboard's range.
    constexpr int kMouseKey = SDL_SCANCODE_COUNT;

    int keyFromName(const std::string &n)
    {
        if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z')
            return SDL_SCANCODE_A + (n[0] - 'A');
        if (n.size() == 1 && n[0] >= '1' && n[0] <= '9')
            return SDL_SCANCODE_1 + (n[0] - '1');
        if (n == "0")
            return SDL_SCANCODE_0;
        if (n.size() >= 2 && n[0] == 'F' && n[1] >= '1' && n[1] <= '9')
        {
            const int f = std::atoi(n.c_str() + 1);
            if (f >= 1 && f <= 12)
                return SDL_SCANCODE_F1 + (f - 1);
        }
        if (n.rfind("Numpad", 0) == 0 && n.size() == 7 && n[6] >= '0' && n[6] <= '9')
            return n[6] == '0' ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (n[6] - '1');
        static const std::map<std::string, int> kNames = {
            {"Backtick", SDL_SCANCODE_GRAVE}, {"Minus", SDL_SCANCODE_MINUS}, {"Plus", SDL_SCANCODE_EQUALS},
            {"Comma", SDL_SCANCODE_COMMA}, {"Period", SDL_SCANCODE_PERIOD}, {"Semicolon", SDL_SCANCODE_SEMICOLON},
            {"Slash", SDL_SCANCODE_SLASH}, {"Backslash", SDL_SCANCODE_BACKSLASH}, {"LBracket", SDL_SCANCODE_LEFTBRACKET},
            {"RBracket", SDL_SCANCODE_RIGHTBRACKET}, {"Quote", SDL_SCANCODE_APOSTROPHE}, {"Return", SDL_SCANCODE_RETURN},
            {"NumpadEnter", SDL_SCANCODE_KP_ENTER}, {"Space", SDL_SCANCODE_SPACE}, {"Tab", SDL_SCANCODE_TAB},
            {"Backspace", SDL_SCANCODE_BACKSPACE}, {"Delete", SDL_SCANCODE_DELETE}, {"Insert", SDL_SCANCODE_INSERT},
            {"Home", SDL_SCANCODE_HOME}, {"End", SDL_SCANCODE_END}, {"PageUp", SDL_SCANCODE_PAGEUP},
            {"PageDown", SDL_SCANCODE_PAGEDOWN}, {"Left", SDL_SCANCODE_LEFT}, {"Right", SDL_SCANCODE_RIGHT},
            {"Up", SDL_SCANCODE_UP}, {"Down", SDL_SCANCODE_DOWN}, {"NumpadPlus", SDL_SCANCODE_KP_PLUS},
            {"NumpadMinus", SDL_SCANCODE_KP_MINUS}, {"NumpadStar", SDL_SCANCODE_KP_MULTIPLY},
            {"NumpadSlash", SDL_SCANCODE_KP_DIVIDE}, {"CapsLock", SDL_SCANCODE_CAPSLOCK}, {"LShift", SDL_SCANCODE_LSHIFT},
            {"RShift", SDL_SCANCODE_RSHIFT}, {"LCtrl", SDL_SCANCODE_LCTRL}, {"RCtrl", SDL_SCANCODE_RCTRL},
            {"LAlt", SDL_SCANCODE_LALT}, {"RAlt", SDL_SCANCODE_RALT}, {"LMB", kMouseKey + SDL_BUTTON_LEFT},
            {"RMB", kMouseKey + SDL_BUTTON_RIGHT}, {"MMB", kMouseKey + SDL_BUTTON_MIDDLE},
        };
        const auto it = kNames.find(n);
        return it == kNames.end() ? -1 : it->second;
    }

    struct KeyBinding
    {
        int key; // SDL_Scancode, or kMouseKey + a mouse button
        uint16_t button;
    };

    // The gamepad's buttons, and what each acts as unless cnr_remap_<name> says otherwise. The triggers are buttons here too.
    struct PhysicalButton
    {
        const char *name;
        int button; // SDL_GamepadButton, or kLeftTrigger / kRightTrigger
        uint16_t pad;
    };
    constexpr int kLeftTrigger = -1, kRightTrigger = -2;
    constexpr PhysicalButton kPhysical[] = {
        {"a", SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS},            {"b", SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},
        {"x", SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE},            {"y", SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
        {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1},      {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
        {"lt", kLeftTrigger, PAD_L2},                          {"rt", kRightTrigger, PAD_R2},
        {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3},         {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3},
        {"back", SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT},         {"start", SDL_GAMEPAD_BUTTON_START, PAD_START},
    };
    constexpr struct
    {
        SDL_GamepadButton button;
        uint16_t pad;
    } kGamepadDpad[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    };

    constexpr uint64_t kBackStartHoldMs = 1000, kEscHoldMs = 1500;
    constexpr float kTriggerPressed = 0.3f;

    struct State
    {
        std::vector<KeyBinding> keys;
        uint16_t remap[std::size(kPhysical)] = {};
        float deadzone = 0.15f;
        int vibration = 100; // percent
        bool rumbleSmall = false;
        uint8_t rumbleLarge = 0;
        bool rumbleSent = false;
        uint64_t rumbleAt = 0;
        bool returnHold = false, returnRequested = false;
        uint64_t backStartSince = 0, escSince = 0;
        std::mutex mutex;
        SDL_Gamepad *gamepad = nullptr;
        uint16_t buttons = 0;          // gamepad + keyboard, PAD_* bits
        float lx = 0, ly = 0, rx = 0, ry = 0; // -1..1
        float l2 = 0, r2 = 0;          // 0..1 analog triggers
    };

    State &state()
    {
        static State s;
        return s;
    }

    // CNR_AUTOPRESS="<seconds>:<button>[+<button>...][/<hold seconds>],...": scripted presses for automated runs, timed
    // from start-up, e.g. "70:start,75:cross,80:up/0.5". Buttons: start select cross circle square triangle l1 r1 l2 r2
    // l3 r3 up down left right. Default hold 0.2 s.
    struct ScriptedPress
    {
        double at = 0, hold = 0.2;
        uint16_t buttons = 0;
    };

    std::vector<ScriptedPress> parseScript()
    {
        static const std::pair<const char *, uint16_t> kNames[] = {
            {"start", PAD_START}, {"select", PAD_SELECT}, {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE},
            {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE}, {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2},
            {"r2", PAD_R2}, {"l3", PAD_L3}, {"r3", PAD_R3}, {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT},
            {"right", PAD_RIGHT}};
        std::vector<ScriptedPress> out;
        const char *env = std::getenv("CNR_AUTOPRESS");
        if (!env)
            return out;
        std::string spec(env);
        size_t pos = 0;
        while (pos < spec.size())
        {
            const size_t end = std::min(spec.find(',', pos), spec.size());
            const std::string item = spec.substr(pos, end - pos);
            pos = end + 1;
            const size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            ScriptedPress p;
            p.at = std::atof(item.c_str());
            std::string names = item.substr(colon + 1);
            if (const size_t slash = names.find('/'); slash != std::string::npos)
            {
                p.hold = std::atof(names.c_str() + slash + 1);
                names.resize(slash);
            }
            size_t n = 0;
            while (n < names.size())
            {
                const size_t plus = std::min(names.find('+', n), names.size());
                const std::string name = names.substr(n, plus - n);
                n = plus + 1;
                for (const auto &[label, bit] : kNames)
                    if (name == label)
                        p.buttons |= bit;
            }
            out.push_back(p);
        }
        return out;
    }

    uint16_t scriptedButtons()
    {
        static const std::vector<ScriptedPress> script = parseScript();
        if (script.empty())
            return 0;
        const double t = SDL_GetTicks() / 1000.0;
        uint16_t buttons = 0;
        for (const ScriptedPress &p : script)
            if (t >= p.at && t < p.at + p.hold)
                buttons |= p.buttons;
        return buttons;
    }

    uint8_t toStickByte(float v)
    {
        return static_cast<uint8_t>(std::clamp(std::lround(128.0f + v * 127.5f), 0l, 255l));
    }

    float axis(SDL_Gamepad *g, SDL_GamepadAxis a, float deadzone)
    {
        const float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
        if (std::fabs(v) < deadzone)
            return 0.0f;
        return std::clamp((v - std::copysign(deadzone, v)) / (1.0f - deadzone), -1.0f, 1.0f);
    }

    // The bindings: the launcher's settings, or the defaults above in development runs.
    void loadBindings(State &s)
    {
        for (const PadName &p : kPadNames)
        {
            const std::string list = cnrSetting((std::string("cnr_key_") + p.name).c_str(), p.defaultKeys);
            size_t pos = 0;
            while (pos < list.size())
            {
                const size_t end = std::min(list.find(',', pos), list.size());
                const int key = keyFromName(list.substr(pos, end - pos));
                pos = end + 1;
                if (key >= 0)
                    s.keys.push_back({key, p.bit});
            }
        }
        for (size_t i = 0; i < std::size(kPhysical); ++i)
        {
            const std::string target = cnrSetting((std::string("cnr_remap_") + kPhysical[i].name).c_str());
            s.remap[i] = target.empty() ? kPhysical[i].pad : padBit(target);
        }
        s.deadzone = std::clamp(cnrSettingInt("cnr_deadzone", 15), 0, 90) / 100.0f;
        s.vibration = std::clamp(cnrSettingInt("cnr_vibration", 100), 0, 100);
    }

    void applyRumble(State &s, uint64_t now)
    {
        const bool on = s.rumbleSmall || s.rumbleLarge != 0;
        if (!s.gamepad || (!on && !s.rumbleSent) || (s.rumbleSent && on && now < s.rumbleAt + 400))
            return;
        const uint32_t large = s.rumbleLarge * 257u * s.vibration / 100u;
        const uint32_t small = s.rumbleSmall ? 65535u * s.vibration / 100u : 0u;
        SDL_RumbleGamepad(s.gamepad, static_cast<Uint16>(large), static_cast<Uint16>(small), on ? 1000 : 0);
        s.rumbleSent = on;
        s.rumbleAt = now;
    }

    // Back and Start held for a second, or Esc for a second and a half: back to the launcher.
    void checkReturnHold(State &s, bool backStart, bool esc, uint64_t now)
    {
        s.backStartSince = backStart ? (s.backStartSince ? s.backStartSince : now) : 0;
        s.escSince = esc ? (s.escSince ? s.escSince : now) : 0;
        if ((s.backStartSince && now - s.backStartSince >= kBackStartHoldMs) || (s.escSince && now - s.escSince >= kEscHoldMs))
            s.returnRequested = true;
    }

    void openFirstGamepad(State &s)
    {
        if (s.gamepad)
            return;
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (ids && count > 0)
        {
            s.gamepad = SDL_OpenGamepad(ids[0]);
            if (s.gamepad)
                std::cout << "[cnr] gamepad: " << SDL_GetGamepadName(s.gamepad) << std::endl;
        }
        SDL_free(ids);
    }
}

void cnrInputInit()
{
    SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    loadBindings(s);
    openFirstGamepad(s);
}

void cnrInputSetRumble(bool small, uint8_t large)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.rumbleSmall = small;
    s.rumbleLarge = large;
}

void cnrInputEnableReturnHold()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.returnHold = true;
}

bool cnrInputReturnRequested()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.returnRequested;
}

void cnrInputShutdown()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.gamepad)
        SDL_CloseGamepad(s.gamepad);
    s.gamepad = nullptr;
}

void cnrInputOnEvent(const SDL_Event &e)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (e.type == SDL_EVENT_GAMEPAD_ADDED)
        openFirstGamepad(s);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && s.gamepad && SDL_GetGamepadID(s.gamepad) == e.gdevice.which)
    {
        SDL_CloseGamepad(s.gamepad);
        s.gamepad = nullptr;
        openFirstGamepad(s);
    }
}

void cnrInputPoll()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    uint16_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0;

    const bool *keys = SDL_GetKeyboardState(nullptr);
    const SDL_MouseButtonFlags mouse = SDL_GetMouseState(nullptr, nullptr);
    for (const KeyBinding &k : s.keys)
        if (k.key >= kMouseKey ? (mouse & SDL_BUTTON_MASK(k.key - kMouseKey)) != 0 : keys[k.key])
            buttons |= k.button;
    if (buttons & PAD_LEFT) lx -= 1;
    if (buttons & PAD_RIGHT) lx += 1;
    if (buttons & PAD_UP) ly -= 1;
    if (buttons & PAD_DOWN) ly += 1;
    if (buttons & PAD_L2) l2 = 1;
    if (buttons & PAD_R2) r2 = 1;

    if (s.gamepad)
    {
        for (size_t i = 0; i < std::size(kPhysical); ++i)
        {
            const PhysicalButton &b = kPhysical[i];
            if (b.button == kLeftTrigger || b.button == kRightTrigger)
            {
                const float v = SDL_GetGamepadAxis(s.gamepad, b.button == kLeftTrigger ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
                if (s.remap[i] == PAD_L2) // analog travel when it still acts as a trigger
                    l2 = std::max(l2, v);
                else if (s.remap[i] == PAD_R2)
                    r2 = std::max(r2, v);
                else if (v > kTriggerPressed)
                    buttons |= s.remap[i];
            }
            else if (SDL_GetGamepadButton(s.gamepad, static_cast<SDL_GamepadButton>(b.button)))
                buttons |= s.remap[i];
        }
        for (const auto &b : kGamepadDpad)
            if (SDL_GetGamepadButton(s.gamepad, b.button))
                buttons |= b.pad;
        lx += axis(s.gamepad, SDL_GAMEPAD_AXIS_LEFTX, s.deadzone);
        ly += axis(s.gamepad, SDL_GAMEPAD_AXIS_LEFTY, s.deadzone);
        rx += axis(s.gamepad, SDL_GAMEPAD_AXIS_RIGHTX, s.deadzone);
        ry += axis(s.gamepad, SDL_GAMEPAD_AXIS_RIGHTY, s.deadzone);
    }
    buttons |= scriptedButtons();
    if (buttons & PAD_L2) l2 = 1;
    if (buttons & PAD_R2) r2 = 1;
    if (l2 > kTriggerPressed) buttons |= PAD_L2;
    if (r2 > kTriggerPressed) buttons |= PAD_R2;

    const uint64_t now = SDL_GetTicks();
    applyRumble(s, now);
    if (s.returnHold)
        checkReturnHold(s, s.gamepad && SDL_GetGamepadButton(s.gamepad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(s.gamepad, SDL_GAMEPAD_BUTTON_START),
                        keys[SDL_SCANCODE_ESCAPE], now);

    s.buttons = buttons;
    s.lx = std::clamp(lx, -1.0f, 1.0f);
    s.ly = std::clamp(ly, -1.0f, 1.0f);
    s.rx = std::clamp(rx, -1.0f, 1.0f);
    s.ry = std::clamp(ry, -1.0f, 1.0f);
    s.l2 = std::clamp(l2, 0.0f, 1.0f);
    s.r2 = std::clamp(r2, 0.0f, 1.0f);
}

bool cnrPadProvider(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;
    std::memset(data, 0, 32);
    data[1] = 0x73; // analog mode
    if (port != 0 || slot != 0)
    {
        // Only player 1 is mapped; other ports read as an idle pad.
        data[2] = data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
        return true;
    }
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const uint16_t activeLow = static_cast<uint16_t>(~s.buttons);
    data[2] = static_cast<uint8_t>(activeLow & 0xFF);
    data[3] = static_cast<uint8_t>(activeLow >> 8);
    data[4] = toStickByte(s.rx);
    data[5] = toStickByte(s.ry);
    data[6] = toStickByte(s.lx);
    data[7] = toStickByte(s.ly);
    // Pressure bytes (pressure mode): right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2.
    // Digital buttons read as fully pressed; the triggers keep their analog travel.
    static constexpr uint16_t kPressureOrder[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                                    PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; ++i)
        data[8 + i] = (s.buttons & kPressureOrder[i]) ? 0xFF : 0x00;
    data[18] = static_cast<uint8_t>(std::lround(s.l2 * 255.0f));
    data[19] = static_cast<uint8_t>(std::lround(s.r2 * 255.0f));
    return true;
}
