#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The mod's flight recorder: the last several thousand things the mod, its session and the game
// did, kept in memory whatever the log level is. The log file stays short enough for a player to
// read; this holds the detail (every debug line, every engine event the mod watches, each NPC that
// changed hands and why) for the minutes before a crash or a bug report, and is written out with
// both: mods\HitmanCoop-blackbox.txt.
//
// Writers are any thread. The crash handler reads the ring as it is, without locks or allocation.
namespace BlackBox {

// The game's clock for each entry (the mod's Now()).
void SetClock(double (*p_Now)());

// One entry. printf-style; the category is a short word, as in the log.
void Note(const char* p_Category, const char* p_Format, ...);
void NoteText(std::string_view p_Category, std::string_view p_Text, char p_Level = 'D');

// What the mod is doing right now, for the crash report: a string literal, set on entry and put
// back on exit. Only the game's main thread sets it.
const char* Section();

struct Scope {
    explicit Scope(const char* p_Section);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* m_Previous;
};

// A fault in here is expected to be caught by the mod (see CrashTrace): the report says so.
struct Guarded {
    explicit Guarded(const char* p_What);
    ~Guarded();

private:
    const char* m_Previous;
};
const char* GuardedWhat();

// Oldest first, one line each. p_Last = 0: all of it.
std::string Dump(size_t p_Last = 0);
bool Save(const std::string& p_Path);
// For the crash handler: straight to a file handle, the last p_Last entries (0: all).
void WriteRaw(void* p_FileHandle, size_t p_Last);

size_t Count();
// Seconds between the oldest entry kept and now.
double Span();

} // namespace BlackBox
