#include "cnr_input.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

    struct KeyBinding
    {
        SDL_Scancode key;
        uint16_t button;
    };
    // Default keyboard layout. Arrow keys drive both the d-pad and the left stick (menus use one, racing the other).
    constexpr KeyBinding kKeys[] = {
        {SDL_SCANCODE_RETURN, PAD_START}, {SDL_SCANCODE_BACKSPACE, PAD_SELECT},
        {SDL_SCANCODE_Z, PAD_CROSS},      {SDL_SCANCODE_X, PAD_SQUARE},
        {SDL_SCANCODE_C, PAD_CIRCLE},     {SDL_SCANCODE_V, PAD_TRIANGLE},
        {SDL_SCANCODE_Q, PAD_L1},         {SDL_SCANCODE_E, PAD_R1},
        {SDL_SCANCODE_1, PAD_L2},         {SDL_SCANCODE_3, PAD_R2},
        {SDL_SCANCODE_UP, PAD_UP},        {SDL_SCANCODE_DOWN, PAD_DOWN},
        {SDL_SCANCODE_LEFT, PAD_LEFT},    {SDL_SCANCODE_RIGHT, PAD_RIGHT},
    };

    struct GamepadBinding
    {
        SDL_GamepadButton button;
        uint16_t pad;
    };
    constexpr GamepadBinding kGamepad[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS},          {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE},          {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1},     {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3},        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT},          {SDL_GAMEPAD_BUTTON_START, PAD_START},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP},           {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT},       {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    };

    struct State
    {
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

    float axis(SDL_Gamepad *g, SDL_GamepadAxis a)
    {
        constexpr float kDeadzone = 0.15f;
        const float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
        if (std::fabs(v) < kDeadzone)
            return 0.0f;
        return std::clamp((v - std::copysign(kDeadzone, v)) / (1.0f - kDeadzone), -1.0f, 1.0f);
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
    openFirstGamepad(s);
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
    for (const KeyBinding &k : kKeys)
        if (keys[k.key])
            buttons |= k.button;
    if (keys[SDL_SCANCODE_LEFT]) lx -= 1;
    if (keys[SDL_SCANCODE_RIGHT]) lx += 1;
    if (keys[SDL_SCANCODE_UP]) ly -= 1;
    if (keys[SDL_SCANCODE_DOWN]) ly += 1;
    if (buttons & PAD_L2) l2 = 1;
    if (buttons & PAD_R2) r2 = 1;

    if (s.gamepad)
    {
        for (const GamepadBinding &b : kGamepad)
            if (SDL_GetGamepadButton(s.gamepad, b.button))
                buttons |= b.pad;
        lx += axis(s.gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        ly += axis(s.gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        rx += axis(s.gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        ry += axis(s.gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
        l2 = std::max(l2, SDL_GetGamepadAxis(s.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f);
        r2 = std::max(r2, SDL_GetGamepadAxis(s.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f);
    }
    buttons |= scriptedButtons();
    if (buttons & PAD_L2) l2 = 1;
    if (buttons & PAD_R2) r2 = 1;
    if (l2 > 0.3f) buttons |= PAD_L2;
    if (r2 > 0.3f) buttons |= PAD_R2;

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
