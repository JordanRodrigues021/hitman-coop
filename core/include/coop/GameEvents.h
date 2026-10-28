#pragma once

#include <set>
#include <string>

#include "Protocol.h"

namespace coop {

// The game's mission events (a name and JSON, as its achievement manager sends them), read for
// co-op: what they mean for the player's status and mission stats, and what the partner hears.
// There is no JSON library in the mod: the few fields needed are pulled out by their keys.
namespace GameEvents {
    // The value after "Key": anywhere in the JSON (the first one). False if missing or another type.
    bool String(const std::string& p_Json, const char* p_Key, std::string& p_Out);
    bool Bool(const std::string& p_Json, const char* p_Key, bool& p_Out);

    // UTF-8 text in plain printable letters (the overlay's font has no accents): "André" -> "Andre".
    std::string PlainText(const std::string& p_Utf8, size_t p_MaxLen = 64);

    // What the partner should hear about our 47 (a key for "Coop_Aware"), empty for nothing.
    std::string Awareness(const std::string& p_Name, const std::string& p_Json);

    // The alert level an AmbientChanged event names (Threat_*), -1 if it isn't one.
    int ThreatOf(const std::string& p_Name, const std::string& p_Json);

    // "UI_CHALLENGES_GLOBAL_FRESH_DISGUISE_NAME" -> "Fresh Disguise".
    std::string ChallengeTitle(const std::string& p_Key);

    // A kill or knock-out, for the partner's kill feed. Sent compact ("TH|Viktor Novikov").
    struct Takedown {
        bool target = false;
        bool accident = false;
        bool headshot = false;
        bool knockout = false;
        std::string name;
    };

    // From a Kill or Pacify event; false for anything else.
    bool ReadTakedown(const std::string& p_Name, const std::string& p_Json, Takedown& p_Out);
    std::string EncodeTakedown(const Takedown& p_Takedown);
    bool DecodeTakedown(const std::string& p_Text, Takedown& p_Out);
    // "Jordan eliminated Viktor Novikov (target), an accident."
    std::string DescribeTakedown(const std::string& p_Who, const Takedown& p_Takedown);

    // One player's mission so far, from their own game's events (not the ones mirroring the partner).
    class Tally {
    public:
        void Reset();
        // True if the event changed the state.
        bool Add(const std::string& p_Name, const std::string& p_Json);
        const MsgPlayerState& State() const { return m_State; }
        MsgPlayerState& State() { return m_State; }

    private:
        MsgPlayerState m_State;
        std::set<std::string> m_Bodies; // found already (each counts once, however many find it)
    };

    // The team's Silent Assassin rating from both players' stats: empty while it holds, else the
    // first reason it doesn't. Unofficial: worked out the way the game scores it, from its events.
    std::string SilentAssassinBroken(const MsgPlayerState& p_A, const MsgPlayerState& p_B);
}

} // namespace coop
