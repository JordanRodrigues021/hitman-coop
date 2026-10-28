#include "coop/GameEvents.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace coop::GameEvents {

// Where the value of "Key": starts (past the colon and spaces), npos if the key isn't there.
static size_t ValueAt(const std::string& p_Json, const char* p_Key) {
    const std::string s_Needle = std::string("\"") + p_Key + "\":";
    const size_t s_At = p_Json.find(s_Needle);

    if (s_At == std::string::npos)
        return std::string::npos;

    size_t s_Value = s_At + s_Needle.size();

    while (s_Value < p_Json.size() && p_Json[s_Value] == ' ')
        ++s_Value;

    return s_Value < p_Json.size() ? s_Value : std::string::npos;
}

bool String(const std::string& p_Json, const char* p_Key, std::string& p_Out) {
    const size_t s_At = ValueAt(p_Json, p_Key);

    if (s_At == std::string::npos || p_Json[s_At] != '"')
        return false;

    p_Out.clear();

    for (size_t i = s_At + 1; i < p_Json.size(); ++i) {
        // \uXXXX (an accented name, as some JSON writers put it) back to UTF-8; others as the letter.
        if (p_Json[i] == '\\' && i + 5 < p_Json.size() && p_Json[i + 1] == 'u') {
            const std::string s_Hex = p_Json.substr(i + 2, 4);
            char* s_HexEnd = nullptr;
            const auto s_Code = static_cast<unsigned>(std::strtoul(s_Hex.c_str(), &s_HexEnd, 16));

            if (s_HexEnd == s_Hex.c_str() + 4) {
                if (s_Code < 0x80)
                    p_Out += static_cast<char>(s_Code);
                else if (s_Code < 0x800) {
                    p_Out += static_cast<char>(0xC0 | (s_Code >> 6));
                    p_Out += static_cast<char>(0x80 | (s_Code & 0x3F));
                }

                i += 5;
                continue;
            }
        }

        if (p_Json[i] == '\\' && i + 1 < p_Json.size()) {
            ++i;
            p_Out += p_Json[i] == 'n' ? ' ' : p_Json[i] == 't' ? ' ' : p_Json[i];
            continue;
        }

        if (p_Json[i] == '"')
            return true;

        p_Out += p_Json[i];
    }

    return false; // never closed
}

bool Bool(const std::string& p_Json, const char* p_Key, bool& p_Out) {
    const size_t s_At = ValueAt(p_Json, p_Key);

    if (s_At == std::string::npos)
        return false;

    if (p_Json.compare(s_At, 4, "true") == 0) {
        p_Out = true;
        return true;
    }

    if (p_Json.compare(s_At, 5, "false") == 0) {
        p_Out = false;
        return true;
    }

    return false;
}

std::string PlainText(const std::string& p_Utf8, size_t p_MaxLen) {
    // U+00C0 to U+00FF (UTF-8 C3 80 to C3 BF) without their accents.
    static constexpr char c_Plain[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string s_Out;

    for (size_t i = 0; i < p_Utf8.size() && s_Out.size() < p_MaxLen; ++i) {
        const auto c = static_cast<unsigned char>(p_Utf8[i]);

        if (c == 0xC3 && i + 1 < p_Utf8.size() && static_cast<unsigned char>(p_Utf8[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(p_Utf8[i + 1]) <= 0xBF)
            s_Out += c_Plain[static_cast<unsigned char>(p_Utf8[++i]) - 0x80];
        else if (c >= 0x20 && c < 0x7F)
            s_Out += static_cast<char>(c);
    }

    return s_Out;
}

std::string Awareness(const std::string& p_Name, const std::string& p_Json) {
    bool s_Flag = false;

    if (p_Name == "Spotted")
        return "spotted";

    if (p_Name == "DisguiseBlown")
        return "disguise_blown";

    if (p_Name == "Trespassing" && Bool(p_Json, "IsTrespassing", s_Flag) && s_Flag)
        return "trespassing";

    if (p_Name == "47_FoundTrespassing")
        return "caught_trespassing";

    if (p_Name == "BodyFound")
        return "body_found";

    if (const int s_Threat = ThreatOf(p_Name, p_Json); s_Threat == Threat_Combat)
        return "combat";
    else if (s_Threat == Threat_Hunted)
        return "hunted";
    else if (s_Threat == Threat_Arrest)
        return "arrest";

    return {};
}

int ThreatOf(const std::string& p_Name, const std::string& p_Json) {
    std::string s_Ambient;

    if (p_Name != "AmbientChanged" || !String(p_Json, "Ambient", s_Ambient))
        return -1;

    // The game's own names (EGameTension without its prefix).
    static constexpr const char* c_Names[] = { "Ambient", "AlertedLow", "AlertedHigh", "Hunting", "Arrest", "Combat" };

    for (int i = 0; i < Threat_Count; ++i)
        if (s_Ambient == c_Names[i])
            return i;

    return -1;
}

std::string ChallengeTitle(const std::string& p_Key) {
    std::string s_Key = p_Key;

    for (const char* s_Prefix : { "UI_CHALLENGES_", "GLOBAL_" })
        if (s_Key.rfind(s_Prefix, 0) == 0)
            s_Key.erase(0, std::strlen(s_Prefix));

    if (s_Key.size() > 5 && s_Key.compare(s_Key.size() - 5, 5, "_NAME") == 0)
        s_Key.resize(s_Key.size() - 5);

    std::string s_Out;
    bool s_WordStart = true;

    for (const char c : PlainText(s_Key, 80)) {
        if (c == '_' || c == ' ') {
            if (!s_Out.empty() && s_Out.back() != ' ')
                s_Out += ' ';

            s_WordStart = true;
            continue;
        }

        const auto u = static_cast<unsigned char>(c);
        s_Out += static_cast<char>(s_WordStart ? std::toupper(u) : std::tolower(u));
        s_WordStart = false;
    }

    while (!s_Out.empty() && s_Out.back() == ' ')
        s_Out.pop_back();

    // Two-letter words are the game's abbreviations ("SA", "KO"), not words: in capitals.
    for (size_t s_Start = 0; s_Start < s_Out.size();) {
        size_t s_End = s_Out.find(' ', s_Start);
        s_End = s_End == std::string::npos ? s_Out.size() : s_End;
        const std::string s_Word = s_Out.substr(s_Start, s_End - s_Start);

        if (s_Word.size() == 2 && s_Word != "Of" && s_Word != "In" && s_Word != "On" && s_Word != "At" && s_Word != "To" && s_Word != "By" &&
            s_Word != "No" && s_Word != "Up")
            for (size_t i = s_Start; i < s_End; ++i)
                s_Out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s_Out[i])));

        s_Start = s_End + 1;
    }

    return s_Out.empty() ? std::string("a challenge") : s_Out;
}

bool ReadTakedown(const std::string& p_Name, const std::string& p_Json, Takedown& p_Out) {
    if (p_Name != "Kill" && p_Name != "Pacify")
        return false;

    p_Out = {};
    p_Out.knockout = p_Name == "Pacify";
    Bool(p_Json, "IsTarget", p_Out.target);
    Bool(p_Json, "Accident", p_Out.accident);
    Bool(p_Json, "IsHeadshot", p_Out.headshot);

    if (std::string s_Name; String(p_Json, "ActorName", s_Name))
        p_Out.name = PlainText(s_Name, 48);

    return true;
}

std::string EncodeTakedown(const Takedown& p_Takedown) {
    std::string s_Out;
    s_Out += p_Takedown.target ? "T" : "";
    s_Out += p_Takedown.accident ? "A" : "";
    s_Out += p_Takedown.headshot ? "H" : "";
    s_Out += p_Takedown.knockout ? "K" : "";
    return s_Out + "|" + PlainText(p_Takedown.name, 48);
}

bool DecodeTakedown(const std::string& p_Text, Takedown& p_Out) {
    const size_t s_Bar = p_Text.find('|');

    if (s_Bar == std::string::npos || s_Bar > 4)
        return false;

    p_Out = {};

    for (size_t i = 0; i < s_Bar; ++i) {
        switch (p_Text[i]) {
            case 'T': p_Out.target = true; break;
            case 'A': p_Out.accident = true; break;
            case 'H': p_Out.headshot = true; break;
            case 'K': p_Out.knockout = true; break;
            default: return false;
        }
    }

    p_Out.name = PlainText(p_Text.substr(s_Bar + 1), 48);
    return true;
}

std::string DescribeTakedown(const std::string& p_Who, const Takedown& p_Takedown) {
    const bool s_Named = !p_Takedown.name.empty() && p_Takedown.name != "John Doe";
    std::string s_Whom = s_Named ? p_Takedown.name : p_Takedown.target ? "a target" : "someone";

    if (s_Named && p_Takedown.target)
        s_Whom += " (target)";

    std::string s_Line = p_Who + (p_Takedown.knockout ? " knocked out " : " eliminated ") + s_Whom;

    if (p_Takedown.accident)
        s_Line += ", an accident";
    else if (p_Takedown.headshot)
        s_Line += " with a headshot";

    return s_Line + ".";
}

void Tally::Reset() {
    m_State = {};
    m_Bodies.clear();
}

bool Tally::Add(const std::string& p_Name, const std::string& p_Json) {
    const MsgPlayerState s_Before = m_State;
    PlayerStats& s_Stats = m_State.stats;
    const auto Bump = [](uint16_t& p_Value) {
        if (p_Value < 0xFFFF)
            ++p_Value;
    };

    if (p_Name == "Kill") {
        bool s_Target = false;
        Bool(p_Json, "IsTarget", s_Target);
        Bump(s_Stats.kills);
        Bump(s_Target ? s_Stats.targets : s_Stats.nonTargetKills);
    }
    else if (p_Name == "Pacify")
        Bump(s_Stats.knockouts);
    else if (p_Name == "Spotted")
        Bump(s_Stats.spotted);
    else if (p_Name == "BodyFound") {
        // Several NPCs find the same body: it counts once. A body we can't tell apart counts each time.
        std::string s_Id;
        const size_t s_Body = p_Json.find("\"DeadBody\"");

        if (s_Body == std::string::npos || !String(p_Json.substr(s_Body), "RepositoryId", s_Id) || m_Bodies.insert(s_Id).second)
            Bump(s_Stats.bodiesFound);
    }
    else if (p_Name == "ChallengeCompleted")
        Bump(s_Stats.challenges);
    else if (p_Name == "SecuritySystemRecorder") {
        std::string s_What;

        if (String(p_Json, "event", s_What) || String(p_Json, "Event", s_What)) {
            if (s_What == "spotted") {
                Bump(s_Stats.recorded);
                m_State.flags &= ~PlayerFlag_EvidenceGone;
            }
            else if (s_What == "erased" || s_What == "destroyed")
                m_State.flags |= PlayerFlag_EvidenceGone;
        }
    }
    else if (p_Name == "Trespassing") {
        if (bool s_On = false; Bool(p_Json, "IsTrespassing", s_On))
            m_State.flags = s_On ? (m_State.flags | PlayerFlag_Trespassing) : (m_State.flags & ~PlayerFlag_Trespassing);
    }
    else if (const int s_Threat = ThreatOf(p_Name, p_Json); s_Threat >= 0)
        m_State.threat = static_cast<uint8_t>(s_Threat);

    return !(m_State == s_Before);
}

std::string SilentAssassinBroken(const MsgPlayerState& p_A, const MsgPlayerState& p_B) {
    const auto Either = [&](uint16_t PlayerStats::*p_Field) { return p_A.stats.*p_Field + p_B.stats.*p_Field; };

    if (Either(&PlayerStats::nonTargetKills) > 0)
        return "a non-target was killed";

    if (Either(&PlayerStats::bodiesFound) > 0)
        return "a body was found";

    if (Either(&PlayerStats::spotted) > 0)
        return "one of you was spotted";

    if ((p_A.stats.recorded > 0 && !(p_A.flags & PlayerFlag_EvidenceGone)) ||
        (p_B.stats.recorded > 0 && !(p_B.flags & PlayerFlag_EvidenceGone)))
        return "caught on camera (destroy the footage)";

    return {};
}

} // namespace coop::GameEvents
