#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace coop {

// How much the mod's log says. Info is the default for players: what happened, a few lines a
// minute plus a health line every 10 s while playing together. Debug ("verbose") adds the
// reasoning behind it (who runs which NPC and why, every message that was refused); Trace adds
// per-NPC and per-message detail for chasing one bug, and is large.
enum class LogLevel : uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

inline const char* LogLevelName(LogLevel p_Level) {
    switch (p_Level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
    }

    return "?";
}

// "info", "Debug", "verbose" (= debug), "2"... False for anything else.
inline bool ParseLogLevel(std::string_view p_Text, LogLevel& p_Out) {
    std::string s_Lower;

    for (const char c : p_Text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            s_Lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (s_Lower == "error" || s_Lower == "0") p_Out = LogLevel::Error;
    else if (s_Lower == "warn" || s_Lower == "warning" || s_Lower == "1") p_Out = LogLevel::Warn;
    else if (s_Lower == "info" || s_Lower == "normal" || s_Lower == "2") p_Out = LogLevel::Info;
    else if (s_Lower == "debug" || s_Lower == "verbose" || s_Lower == "3") p_Out = LogLevel::Debug;
    else if (s_Lower == "trace" || s_Lower == "4") p_Out = LogLevel::Trace;
    else return false;

    return true;
}

} // namespace coop
