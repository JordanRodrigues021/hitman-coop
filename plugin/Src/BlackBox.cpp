#include "BlackBox.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t c_Entries = 8192; // a power of two
constexpr size_t c_Text = 200;

struct Entry {
    uint64_t seq;    // 0: never written
    uint64_t wallMs; // FILETIME in ms
    double game;
    uint32_t thread;
    char level;
    char category[11];
    char text[c_Text];
};

Entry g_Ring[c_Entries];
std::atomic<uint64_t> g_Next { 1 };
SRWLOCK g_Lock = SRWLOCK_INIT;
double (*g_Clock)() = nullptr;
const char* g_Section = "";
thread_local const char* t_Guarded = nullptr;

uint64_t WallMs() {
    FILETIME s_Time;
    GetSystemTimeAsFileTime(&s_Time);
    return ((static_cast<uint64_t>(s_Time.dwHighDateTime) << 32) | s_Time.dwLowDateTime) / 10000;
}

void Put(std::string_view p_Category, const char* p_Text, size_t p_Length, char p_Level) {
    const uint64_t s_Wall = WallMs();
    const double s_Game = g_Clock ? g_Clock() : 0.0;
    AcquireSRWLockExclusive(&g_Lock);
    const uint64_t s_Seq = g_Next.fetch_add(1, std::memory_order_relaxed);
    Entry& s_Entry = g_Ring[s_Seq & (c_Entries - 1)];
    s_Entry.seq = 0; // half written: a crash dump skips it
    s_Entry.wallMs = s_Wall;
    s_Entry.game = s_Game;
    s_Entry.thread = GetCurrentThreadId();
    s_Entry.level = p_Level;
    const size_t s_Cat = p_Category.size() < sizeof(s_Entry.category) - 1 ? p_Category.size() : sizeof(s_Entry.category) - 1;
    std::memcpy(s_Entry.category, p_Category.data(), s_Cat);
    s_Entry.category[s_Cat] = 0;
    const size_t s_Len = p_Length < c_Text - 1 ? p_Length : c_Text - 1;
    std::memcpy(s_Entry.text, p_Text, s_Len);
    s_Entry.text[s_Len] = 0;

    for (size_t i = 0; i < s_Len; ++i)
        if (static_cast<unsigned char>(s_Entry.text[i]) < 0x20)
            s_Entry.text[i] = ' ';

    s_Entry.seq = s_Seq;
    ReleaseSRWLockExclusive(&g_Lock);
}

// "14:02:11.123 +3261.71 D standin  | text" into p_Out; the length written.
int Format(const Entry& p_Entry, char* p_Out, size_t p_Size) {
    FILETIME s_File, s_Local;
    const uint64_t s_Ticks = p_Entry.wallMs * 10000;
    s_File.dwLowDateTime = static_cast<DWORD>(s_Ticks);
    s_File.dwHighDateTime = static_cast<DWORD>(s_Ticks >> 32);
    SYSTEMTIME s_Time {};

    if (FileTimeToLocalFileTime(&s_File, &s_Local))
        FileTimeToSystemTime(&s_Local, &s_Time);

    return std::snprintf(p_Out, p_Size, "%02u:%02u:%02u.%03u %+10.2f %c %-10s| %s\r\n", s_Time.wHour, s_Time.wMinute, s_Time.wSecond,
        s_Time.wMilliseconds, p_Entry.game, p_Entry.level ? p_Entry.level : 'D', p_Entry.category, p_Entry.text);
}

// The sequence numbers kept right now: [first, last).
void Range(size_t p_Last, uint64_t& p_First, uint64_t& p_End) {
    p_End = g_Next.load(std::memory_order_relaxed);
    p_First = p_End > c_Entries ? p_End - c_Entries : 1;

    if (p_Last && p_End - p_First > p_Last)
        p_First = p_End - p_Last;
}
} // namespace

void BlackBox::SetClock(double (*p_Now)()) {
    g_Clock = p_Now;
}

void BlackBox::Note(const char* p_Category, const char* p_Format, ...) {
    char s_Text[c_Text];
    va_list s_Args;
    va_start(s_Args, p_Format);
    const int s_Length = std::vsnprintf(s_Text, sizeof(s_Text), p_Format, s_Args);
    va_end(s_Args);

    if (s_Length > 0)
        Put(p_Category, s_Text, static_cast<size_t>(s_Length), 'D');
}

void BlackBox::NoteText(std::string_view p_Category, std::string_view p_Text, char p_Level) {
    // A long line goes in as several entries, so nothing is cut off.
    size_t s_At = 0;

    do {
        const size_t s_Take = p_Text.size() - s_At < c_Text - 1 ? p_Text.size() - s_At : c_Text - 1;
        Put(p_Category, p_Text.data() + s_At, s_Take, s_At ? '+' : p_Level);
        s_At += s_Take;
    } while (s_At < p_Text.size() && s_At < 6 * (c_Text - 1));
}

const char* BlackBox::Section() {
    return g_Section;
}

BlackBox::Scope::Scope(const char* p_Section) : m_Previous(g_Section) {
    g_Section = p_Section;
}

BlackBox::Scope::~Scope() {
    g_Section = m_Previous;
}

BlackBox::Guarded::Guarded(const char* p_What) : m_Previous(t_Guarded) {
    t_Guarded = p_What;
}

BlackBox::Guarded::~Guarded() {
    t_Guarded = m_Previous;
}

const char* BlackBox::GuardedWhat() {
    return t_Guarded;
}

std::string BlackBox::Dump(size_t p_Last) {
    std::string s_Out;
    char s_Line[c_Text + 96];
    AcquireSRWLockShared(&g_Lock);
    uint64_t s_First = 0, s_End = 0;
    Range(p_Last, s_First, s_End);
    s_Out.reserve(static_cast<size_t>(s_End - s_First) * 96);

    for (uint64_t s_Seq = s_First; s_Seq < s_End; ++s_Seq) {
        const Entry& s_Entry = g_Ring[s_Seq & (c_Entries - 1)];

        if (s_Entry.seq != s_Seq)
            continue;

        const int s_Length = Format(s_Entry, s_Line, sizeof(s_Line));

        if (s_Length > 0)
            s_Out.append(s_Line, static_cast<size_t>(s_Length) < sizeof(s_Line) ? s_Length : sizeof(s_Line) - 1);
    }

    ReleaseSRWLockShared(&g_Lock);
    return s_Out;
}

bool BlackBox::Save(const std::string& p_Path) {
    const std::string s_Text = Dump();
    FILE* s_File = nullptr;

    if (fopen_s(&s_File, p_Path.c_str(), "wb") != 0 || !s_File)
        return false;

    std::fwrite(s_Text.data(), 1, s_Text.size(), s_File);
    std::fclose(s_File);
    return true;
}

void BlackBox::WriteRaw(void* p_FileHandle, size_t p_Last) {
    // No lock (the crashed thread may hold it) and no allocation: entries being written are
    // skipped by their sequence number.
    char s_Line[c_Text + 96];
    uint64_t s_First = 0, s_End = 0;
    Range(p_Last, s_First, s_End);

    for (uint64_t s_Seq = s_First; s_Seq < s_End; ++s_Seq) {
        const Entry& s_Entry = g_Ring[s_Seq & (c_Entries - 1)];

        if (s_Entry.seq != s_Seq)
            continue;

        const int s_Length = Format(s_Entry, s_Line, sizeof(s_Line));
        DWORD s_Written = 0;

        if (s_Length > 0)
            WriteFile(static_cast<HANDLE>(p_FileHandle), s_Line, static_cast<DWORD>(static_cast<size_t>(s_Length) < sizeof(s_Line) ? s_Length : sizeof(s_Line) - 1),
                &s_Written, nullptr);
    }
}

size_t BlackBox::Count() {
    const uint64_t s_End = g_Next.load(std::memory_order_relaxed);
    return static_cast<size_t>(s_End - 1 < c_Entries ? s_End - 1 : c_Entries);
}

double BlackBox::Span() {
    uint64_t s_First = 0, s_End = 0;
    AcquireSRWLockShared(&g_Lock);
    Range(0, s_First, s_End);
    double s_Span = 0.0;

    if (s_End > s_First) {
        const Entry& s_Oldest = g_Ring[s_First & (c_Entries - 1)];

        if (s_Oldest.seq == s_First)
            s_Span = static_cast<double>(WallMs() - s_Oldest.wallMs) / 1000.0;
    }

    ReleaseSRWLockShared(&g_Lock);
    return s_Span;
}
