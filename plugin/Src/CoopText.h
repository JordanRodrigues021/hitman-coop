#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

// Words and numbers the game-thread code and the UI share.
inline constexpr const char* c_ModVersion = "0.14.0";

// The game version this build is for (set by plugin/CMakeLists.txt from the SDK it is built
// against): "3.290" normally, "3.280" for the build players who can't update get (the installer
// picks the one for the game it finds).
#ifndef COOP_GAME_TARGET
#define COOP_GAME_TARGET "3.290"
#endif
#ifndef COOP_SDK_VERSION
#define COOP_SDK_VERSION 40201
#endif
inline constexpr const char* c_GameTarget = COOP_GAME_TARGET;
inline constexpr const char* c_SdkName = COOP_SDK_VERSION == 40201 ? "4.2.1" : COOP_SDK_VERSION == 40101 ? "4.1.1" : "4.0.0-rc.1";
// In the DLL as it is, so tools/package.ps1 can tell the builds apart (and the log names it).
inline constexpr const char* c_BuildMark = "HitmanCoop build for game " COOP_GAME_TARGET;

// The self-updater's file, for the game version running rather than this build's: a player who
// updates their game from 3.280 to 3.290 (or has the wrong build) gets the right one by itself.
inline std::string UpdateFileFor(const std::string& p_GameVersion) {
    if (p_GameVersion.rfind("3.280.", 0) == 0)
        return "update-3.280.txt";

    return "update.txt";
}

inline constexpr const char* c_Author = "Jordan Rodrigues";

// The quick messages as they come (each can be rewritten under Settings).
inline constexpr const char* c_DefaultQuickMessages[8] = {
    "Wait for me.", "Go ahead.", "On my way.", "Target down.", "I need a distraction.", "Hide, guards coming!",
    "Meet at my mark.", "Help!",
};

// The game's menus (EGameUIMenu), as the partner reads "X is in ...".
inline constexpr const char* c_MenuNames[] = {
    "the intro", "the pause menu", "their phone", "the map", "a dialog", "a menu", "the inventory", "the inventory",
    "the action menu", "the inventory", "the intel", "the notebook", "the logbook", "a tutorial", "a hint",
    "the emote menu", "the wardrobe",
};

// What the HUD calls each alert level (coop::Threat_*).
inline constexpr const char* c_ThreatNames[] = { "calm", "noticed", "suspicious", "hunted", "being arrested", "in combat" };

// Signals: a gesture over the player's head on the partner's screen.
enum Signal { Signal_Ok, Signal_Stop, Signal_Help, Signal_Look, Signal_Count };
inline constexpr const char* c_SignalKeys[Signal_Count] = { "ok", "stop", "help", "look" };
inline constexpr const char* c_SignalWords[Signal_Count] = { "OK!", "Wait!", "Help me!", "Look over here!" };

inline std::string Clock(double p_Seconds) {
    const int s_Total = static_cast<int>(std::max(0.0, p_Seconds));
    char s_Text[24];

    if (s_Total >= 3600)
        std::snprintf(s_Text, sizeof(s_Text), "%d:%02d:%02d", s_Total / 3600, s_Total / 60 % 60, s_Total % 60);
    else
        std::snprintf(s_Text, sizeof(s_Text), "%d:%02d", s_Total / 60, s_Total % 60);

    return s_Text;
}
