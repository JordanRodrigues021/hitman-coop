#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

// The mod's hotkeys: one key (keyboard or mouse button), with Ctrl, Shift or Alt if wanted, for each
// action. Read with GetAsyncKeyState on the game thread, bound in the CO-OP window's Hotkeys tab.
namespace Hotkeys {
    enum Modifier : uint8_t {
        Mod_Ctrl = 1 << 0,
        Mod_Shift = 1 << 1,
        Mod_Alt = 1 << 2,
    };

    struct Bind {
        uint16_t vk = 0; // 0: none
        uint8_t mods = 0;

        bool operator==(const Bind&) const = default;
        uint32_t Pack() const { return vk | (static_cast<uint32_t>(mods) << 16); }
        static Bind Unpack(uint32_t p_Packed) { return { static_cast<uint16_t>(p_Packed & 0xFFFF), static_cast<uint8_t>(p_Packed >> 16) }; }
    };

    enum class Action : int {
        Mark,
        Quick1, Quick2, Quick3, Quick4, Quick5, Quick6, Quick7, Quick8,
        Regroup,
        WhereAreYou,
        ComeHere,
        ReadyCheck,
        Countdown,
        SignalOk,
        SignalStop,
        SignalHelp,
        SignalLook,
        ToggleHud,
        ToggleRadar,
        HideOverlay,
        Count,
    };

    constexpr int c_Count = static_cast<int>(Action::Count);

    struct Info {
        const char* key;     // its setting, "key_<key>"
        const char* name;    // as the Hotkeys tab lists it
        const char* group;
        const char* fallback; // the default binding
    };

    const Info& Describe(Action p_Action);

    // "CTRL+NUMPAD1", "MIDDLE", "F5"; "" for none.
    std::string Name(const Bind& p_Bind);
    // The other way round; false (and none) for "OFF", "" or a name it doesn't know.
    bool Parse(const std::string& p_Text, Bind& p_Out);

    // The bindings, shared by the game thread (which reads them) and the UI (which changes them).
    Bind Get(Action p_Action);
    void Set(Action p_Action, const Bind& p_Bind);
    void ResetAll();

    // Down right now, with its modifiers.
    bool Down(const Bind& p_Bind);
    // The first key (not Esc or the left mouse button) that is down now but wasn't when the capture
    // began, with the modifiers held; false while none is. Esc gives a none bind and true (cancelled).
    void BeginCapture();
    bool Capture(Bind& p_Out, bool& p_Cancelled);
}
