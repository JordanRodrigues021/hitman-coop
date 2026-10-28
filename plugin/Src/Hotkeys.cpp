#include "Hotkeys.h"

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {
    // Every action, its setting, what the Hotkeys tab calls it and its default. The number pad
    // keys never clash with the game's own controls.
    constexpr Hotkeys::Info c_Actions[Hotkeys::c_Count] = {
        { "mark", "Mark what you aim at", "Marks", "MIDDLE" },
        { "quick1", "Quick message 1", "Quick messages", "NUMPAD1" },
        { "quick2", "Quick message 2", "Quick messages", "NUMPAD2" },
        { "quick3", "Quick message 3", "Quick messages", "NUMPAD3" },
        { "quick4", "Quick message 4", "Quick messages", "NUMPAD4" },
        { "quick5", "Quick message 5", "Quick messages", "NUMPAD5" },
        { "quick6", "Quick message 6", "Quick messages", "NUMPAD6" },
        { "quick7", "Quick message 7", "Quick messages", "NUMPAD7" },
        { "quick8", "Quick message 8", "Quick messages", "NUMPAD8" },
        { "regroup", "Regroup (go to your partner)", "Teamwork", "NUMPAD0" },
        { "where", "Where are you? (both name tags pulse)", "Teamwork", "NUMPAD9" },
        { "comehere", "Ask your partner to come to you", "Teamwork", "" },
        { "ready", "Ready check / answer \"ready\"", "Teamwork", "DECIMAL" },
        { "countdown", "3-2-1 countdown on both screens", "Teamwork", "ADD" },
        { "signal_ok", "Signal: OK", "Signals", "DIVIDE" },
        { "signal_stop", "Signal: wait", "Signals", "MULTIPLY" },
        { "signal_help", "Signal: help", "Signals", "SUBTRACT" },
        { "signal_look", "Signal: look here", "Signals", "" },
        { "toggle_hud", "Show / hide the HUD", "Display", "" },
        { "toggle_radar", "Show / hide the radar", "Display", "" },
        { "hide_overlay", "Hide everything (screenshot mode)", "Display", "" },
    };

    std::array<std::atomic<uint32_t>, Hotkeys::c_Count> g_Binds {};
    std::array<bool, 256> g_CaptureHeld {};

    struct Named {
        uint16_t vk;
        const char* name;
    };

    // Keys with names of their own (letters, digits and F keys are worked out).
    constexpr Named c_Names[] = {
        { VK_MBUTTON, "MIDDLE" }, { VK_RBUTTON, "RIGHTMOUSE" }, { VK_XBUTTON1, "MOUSE4" }, { VK_XBUTTON2, "MOUSE5" },
        { VK_DECIMAL, "DECIMAL" }, { VK_ADD, "ADD" }, { VK_SUBTRACT, "SUBTRACT" }, { VK_MULTIPLY, "MULTIPLY" }, { VK_DIVIDE, "DIVIDE" },
        { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" }, { VK_HOME, "HOME" }, { VK_END, "END" }, { VK_PRIOR, "PAGEUP" },
        { VK_NEXT, "PAGEDOWN" }, { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
        { VK_SPACE, "SPACE" }, { VK_RETURN, "ENTER" }, { VK_TAB, "TAB" }, { VK_BACK, "BACKSPACE" }, { VK_CAPITAL, "CAPSLOCK" },
        { VK_PAUSE, "PAUSE" }, { VK_SCROLL, "SCROLLLOCK" }, { VK_OEM_1, "SEMICOLON" }, { VK_OEM_PLUS, "EQUALS" },
        { VK_OEM_COMMA, "COMMA" }, { VK_OEM_MINUS, "MINUS" }, { VK_OEM_PERIOD, "PERIOD" }, { VK_OEM_2, "SLASH" },
        { VK_OEM_3, "BACKQUOTE" }, { VK_OEM_4, "LEFTBRACKET" }, { VK_OEM_5, "BACKSLASH" }, { VK_OEM_6, "RIGHTBRACKET" },
        { VK_OEM_7, "QUOTE" },
    };

    std::string KeyName(uint16_t p_Vk) {
        if ((p_Vk >= 'A' && p_Vk <= 'Z') || (p_Vk >= '0' && p_Vk <= '9'))
            return std::string(1, static_cast<char>(p_Vk));

        if (p_Vk >= VK_F1 && p_Vk <= VK_F24)
            return "F" + std::to_string(p_Vk - VK_F1 + 1);

        if (p_Vk >= VK_NUMPAD0 && p_Vk <= VK_NUMPAD9)
            return "NUMPAD" + std::to_string(p_Vk - VK_NUMPAD0);

        for (const auto& s_Named : c_Names)
            if (s_Named.vk == p_Vk)
                return s_Named.name;

        return {};
    }

    uint16_t KeyCode(const std::string& p_Name) {
        if (p_Name.size() == 1 && std::isalnum(static_cast<unsigned char>(p_Name[0])))
            return static_cast<uint16_t>(p_Name[0]);

        if (p_Name == "MOUSE3")
            return VK_MBUTTON;

        if (p_Name.size() >= 2 && p_Name.size() <= 3 && p_Name[0] == 'F' && std::isdigit(static_cast<unsigned char>(p_Name[1]))) {
            const int s_N = std::atoi(p_Name.c_str() + 1);
            return s_N >= 1 && s_N <= 24 ? static_cast<uint16_t>(VK_F1 + s_N - 1) : 0;
        }

        if (p_Name.size() == 7 && p_Name.rfind("NUMPAD", 0) == 0 && std::isdigit(static_cast<unsigned char>(p_Name[6])))
            return static_cast<uint16_t>(VK_NUMPAD0 + (p_Name[6] - '0'));

        for (const auto& s_Named : c_Names)
            if (p_Name == s_Named.name)
                return s_Named.vk;

        return 0;
    }

    bool Held(int p_Vk) { return (GetAsyncKeyState(p_Vk) & 0x8000) != 0; }

    uint8_t HeldMods() {
        return static_cast<uint8_t>((Held(VK_CONTROL) ? Hotkeys::Mod_Ctrl : 0) | (Held(VK_SHIFT) ? Hotkeys::Mod_Shift : 0) |
            (Held(VK_MENU) ? Hotkeys::Mod_Alt : 0));
    }

    bool Bindable(int p_Vk) {
        return p_Vk != VK_LBUTTON && p_Vk != VK_ESCAPE && p_Vk != VK_CONTROL && p_Vk != VK_SHIFT && p_Vk != VK_MENU &&
            p_Vk != VK_LCONTROL && p_Vk != VK_RCONTROL && p_Vk != VK_LSHIFT && p_Vk != VK_RSHIFT && p_Vk != VK_LMENU &&
            p_Vk != VK_RMENU && p_Vk != VK_LWIN && p_Vk != VK_RWIN && !KeyName(static_cast<uint16_t>(p_Vk)).empty();
    }

    struct Defaults {
        Defaults() { Hotkeys::ResetAll(); }
    } g_Defaults;
}

const Hotkeys::Info& Hotkeys::Describe(Action p_Action) { return c_Actions[static_cast<int>(p_Action)]; }

std::string Hotkeys::Name(const Bind& p_Bind) {
    const std::string s_Key = KeyName(p_Bind.vk);

    if (s_Key.empty())
        return {};

    return std::string(p_Bind.mods & Mod_Ctrl ? "CTRL+" : "") + (p_Bind.mods & Mod_Shift ? "SHIFT+" : "") +
        (p_Bind.mods & Mod_Alt ? "ALT+" : "") + s_Key;
}

bool Hotkeys::Parse(const std::string& p_Text, Bind& p_Out) {
    std::string s_Text;

    for (const char c : p_Text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            s_Text += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    p_Out = {};

    for (bool s_More = true; s_More;) {
        s_More = false;

        for (const auto& [s_Prefix, s_Mod] : { std::pair { "CTRL+", Mod_Ctrl }, std::pair { "SHIFT+", Mod_Shift }, std::pair { "ALT+", Mod_Alt } }) {
            if (s_Text.rfind(s_Prefix, 0) == 0) {
                s_Text.erase(0, std::char_traits<char>::length(s_Prefix));
                p_Out.mods |= s_Mod;
                s_More = true;
            }
        }
    }

    p_Out.vk = KeyCode(s_Text);

    if (p_Out.vk == 0)
        p_Out = {};

    return p_Out.vk != 0;
}

Hotkeys::Bind Hotkeys::Get(Action p_Action) { return Bind::Unpack(g_Binds[static_cast<int>(p_Action)].load()); }

void Hotkeys::Set(Action p_Action, const Bind& p_Bind) { g_Binds[static_cast<int>(p_Action)] = p_Bind.Pack(); }

void Hotkeys::ResetAll() {
    for (int i = 0; i < c_Count; ++i) {
        Bind s_Bind;
        Parse(c_Actions[i].fallback, s_Bind);
        g_Binds[i] = s_Bind.Pack();
    }
}

bool Hotkeys::Down(const Bind& p_Bind) {
    // Its modifiers must be held; others may be too (Ctrl is crouch in the game).
    return p_Bind.vk != 0 && Held(p_Bind.vk) && (HeldMods() & p_Bind.mods) == p_Bind.mods;
}

void Hotkeys::BeginCapture() {
    for (int i = 1; i < 256; ++i)
        g_CaptureHeld[i] = Held(i);
}

bool Hotkeys::Capture(Bind& p_Out, bool& p_Cancelled) {
    p_Cancelled = false;

    if (Held(VK_ESCAPE)) {
        p_Out = {};
        p_Cancelled = true;
        return true;
    }

    for (int i = 1; i < 256; ++i) {
        const bool s_Held = Held(i);

        if (s_Held && !g_CaptureHeld[i] && Bindable(i)) {
            p_Out = { static_cast<uint16_t>(i), HeldMods() };
            return true;
        }

        g_CaptureHeld[i] = s_Held;
    }

    return false;
}
