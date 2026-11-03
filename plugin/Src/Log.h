#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "coop/Log.h"

// The mod's log, Retail\mods\HitmanCoop.log: everything the mod and its session say, one line each:
//   2026-11-03 14:02:11.123  +3261.71  INFO  net     | connected to host 'Jordan' in 0.4 s ...
// wall-clock time (to line up two players' logs), the game's clock (to line up with the SDK's log
// and videos), the level, a category and the text. IP addresses are masked (players post their
// logs in public), unless [log] show_addresses = true.
//
// The level is [log] level in HitmanCoop.ini (or CO-OP > Settings): info for players, debug
// ("verbose") or trace for chasing a bug. The last session's log is kept as HitmanCoop-previous.log.
namespace CoopLog {

// Starts this session's log at p_Path (the last one renamed to p_Previous first). Lines written
// before this are kept and written first.
void Open(const std::string& p_Path, const std::string& p_Previous);
const std::string& Path();

void SetLevel(coop::LogLevel p_Level);
coop::LogLevel Level();
bool On(coop::LogLevel p_Level);

void SetShowAddresses(bool p_Show);
// The game's clock for the second column (the mod's Now()).
void SetGameClock(double (*p_Now)());
// Every line the log file gets also goes here (the test channel's trace file).
void SetMirror(std::function<void(const std::string&)> p_Mirror);

void Write(coop::LogLevel p_Level, std::string_view p_Category, std::string_view p_Text);
// The older "category: text" trace lines: the category is the word before the first ": ".
void Tagged(coop::LogLevel p_Level, std::string_view p_Line);

inline void Error(std::string_view p_Category, std::string_view p_Text) { Write(coop::LogLevel::Error, p_Category, p_Text); }
inline void Warn(std::string_view p_Category, std::string_view p_Text) { Write(coop::LogLevel::Warn, p_Category, p_Text); }
inline void Info(std::string_view p_Category, std::string_view p_Text) { Write(coop::LogLevel::Info, p_Category, p_Text); }
inline void Debug(std::string_view p_Category, std::string_view p_Text) { Write(coop::LogLevel::Debug, p_Category, p_Text); }

// printf-style, formatted only when the level is on.
template <class... A>
void Fmt(coop::LogLevel p_Level, std::string_view p_Category, const char* p_Format, A... p_Args) {
    if (!On(p_Level))
        return;

    char s_Text[1024];
    std::snprintf(s_Text, sizeof(s_Text), p_Format, p_Args...);
    Write(p_Level, p_Category, s_Text);
}

// "203.0.113.57" -> "203.0.x.x" (loopback and LAN addresses kept: they say nothing about anyone).
std::string MaskAddresses(std::string_view p_Text);

} // namespace CoopLog
