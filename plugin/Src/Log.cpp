#include "Log.h"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace {
std::mutex g_Mutex;
std::FILE* g_File = nullptr;
std::string g_Path;
uint64_t g_Bytes = 0;
bool g_Full = false;
std::atomic<int> g_Level = static_cast<int>(coop::LogLevel::Info);
bool g_ShowAddresses = false;
double (*g_GameClock)() = nullptr;
std::function<void(const std::string&)> g_Mirror;
std::vector<std::string> g_Early; // lines written before Open

// A long session at info is well under 1 MB; these only stop a runaway.
uint64_t CapFor(coop::LogLevel p_Level) {
    switch (p_Level) {
        case coop::LogLevel::Trace: return 256ull << 20;
        case coop::LogLevel::Debug: return 64ull << 20;
        default: return 16ull << 20;
    }
}

bool IsPrivate(int a, int b) {
    return a == 127 || a == 10 || a == 0 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
}

std::string Stamp() {
    SYSTEMTIME s_Time;
    GetLocalTime(&s_Time);
    char s_Text[64];
    const double s_Game = g_GameClock ? g_GameClock() : 0.0;
    std::snprintf(s_Text, sizeof(s_Text), "%04u-%02u-%02u %02u:%02u:%02u.%03u %+10.2f", s_Time.wYear, s_Time.wMonth, s_Time.wDay,
        s_Time.wHour, s_Time.wMinute, s_Time.wSecond, s_Time.wMilliseconds, s_Game);
    return s_Text;
}

// Under g_Mutex.
void Emit(const std::string& p_Line) {
    if (g_Mirror)
        g_Mirror(p_Line);

    if (!g_File) {
        if (g_Path.empty() && g_Early.size() < 2000)
            g_Early.push_back(p_Line);

        return;
    }

    if (g_Full)
        return;

    std::fputs(p_Line.c_str(), g_File);
    std::fputc('\n', g_File);
    g_Bytes += p_Line.size() + 1;

    if (g_Bytes > CapFor(static_cast<coop::LogLevel>(g_Level.load()))) {
        std::fputs("(log full: nothing more is written this session; lower [log] level to log less)\n", g_File);
        g_Full = true;
    }

    // Each line at once: a crash must not take the last lines with it.
    std::fflush(g_File);
}
} // namespace

namespace CoopLog {

std::string MaskAddresses(std::string_view p_Text) {
    if (g_ShowAddresses)
        return std::string(p_Text);

    std::string s_Out;
    s_Out.reserve(p_Text.size());
    size_t i = 0;

    while (i < p_Text.size()) {
        // An IPv4 address: four numbers 0-255 with dots, not inside a longer number or version.
        const bool s_Start = std::isdigit(static_cast<unsigned char>(p_Text[i])) &&
            (i == 0 || (!std::isalnum(static_cast<unsigned char>(p_Text[i - 1])) && p_Text[i - 1] != '.' && p_Text[i - 1] != '_'));

        if (s_Start) {
            int s_Parts[4] {};
            size_t j = i;
            int n = 0;
            bool s_Ok = true;

            for (; n < 4; ++n) {
                size_t s_Digits = 0;
                int s_Value = 0;

                while (j < p_Text.size() && std::isdigit(static_cast<unsigned char>(p_Text[j])) && s_Digits < 4) {
                    s_Value = s_Value * 10 + (p_Text[j] - '0');
                    ++j;
                    ++s_Digits;
                }

                if (s_Digits == 0 || s_Digits > 3 || s_Value > 255) {
                    s_Ok = false;
                    break;
                }

                s_Parts[n] = s_Value;

                if (n < 3) {
                    if (j >= p_Text.size() || p_Text[j] != '.') {
                        s_Ok = false;
                        break;
                    }

                    ++j;
                }
            }

            const bool s_Ends = j >= p_Text.size() || (!std::isdigit(static_cast<unsigned char>(p_Text[j])) &&
                (p_Text[j] != '.' || j + 1 >= p_Text.size() || !std::isdigit(static_cast<unsigned char>(p_Text[j + 1]))));

            if (s_Ok && n == 4 && s_Ends) {
                if (IsPrivate(s_Parts[0], s_Parts[1]))
                    s_Out.append(p_Text.substr(i, j - i));
                else
                    s_Out += std::to_string(s_Parts[0]) + "." + std::to_string(s_Parts[1]) + ".x.x";

                i = j;
                continue;
            }
        }

        s_Out += p_Text[i++];
    }

    return s_Out;
}

void Open(const std::string& p_Path, const std::string& p_Previous) {
    std::lock_guard s_Lock(g_Mutex);

    if (g_File) {
        std::fclose(g_File);
        g_File = nullptr;
    }

    std::error_code s_Error;

    if (!p_Previous.empty())
        std::filesystem::rename(p_Path, p_Previous, s_Error);

    g_Path = p_Path;
    g_Bytes = 0;
    g_Full = false;
    g_File = _fsopen(p_Path.c_str(), "w", _SH_DENYWR); // others may read it while the game runs

    if (!g_File)
        return;

    for (const auto& s_Line : g_Early) {
        std::fputs(s_Line.c_str(), g_File);
        std::fputc('\n', g_File);
        g_Bytes += s_Line.size() + 1;
    }

    g_Early.clear();
    std::fflush(g_File);
}

const std::string& Path() {
    return g_Path;
}

void SetLevel(coop::LogLevel p_Level) {
    g_Level = static_cast<int>(p_Level);
}

coop::LogLevel Level() {
    return static_cast<coop::LogLevel>(g_Level.load());
}

bool On(coop::LogLevel p_Level) {
    return static_cast<int>(p_Level) <= g_Level.load();
}

void SetShowAddresses(bool p_Show) {
    std::lock_guard s_Lock(g_Mutex);
    g_ShowAddresses = p_Show;
}

void SetGameClock(double (*p_Now)()) {
    g_GameClock = p_Now;
}

void SetMirror(std::function<void(const std::string&)> p_Mirror) {
    std::lock_guard s_Lock(g_Mutex);
    g_Mirror = std::move(p_Mirror);
}

void Write(coop::LogLevel p_Level, std::string_view p_Category, std::string_view p_Text) {
    if (static_cast<int>(p_Level) > g_Level.load())
        return;

    char s_Head[96];
    std::snprintf(s_Head, sizeof(s_Head), "  %-5s %-8.*s| ", coop::LogLevelName(p_Level), static_cast<int>(std::min<size_t>(p_Category.size(), 8)),
        p_Category.data());
    std::lock_guard s_Lock(g_Mutex);
    std::string s_Line = Stamp() + s_Head + MaskAddresses(p_Text);

    // Control characters (an NPC name with a newline) would break the one-line format.
    for (char& c : s_Line)
        if (static_cast<unsigned char>(c) < 0x20)
            c = ' ';

    Emit(s_Line);
}

void Tagged(coop::LogLevel p_Level, std::string_view p_Line) {
    const size_t s_Colon = p_Line.find(": ");

    // A category is one short word ("partner", "held item"): anything else is all text.
    if (s_Colon != std::string_view::npos && s_Colon > 0 && s_Colon <= 12) {
        std::string s_Category(p_Line.substr(0, s_Colon));

        for (char& c : s_Category)
            if (c == ' ')
                c = '-';

        Write(p_Level, s_Category, p_Line.substr(s_Colon + 2));
        return;
    }

    Write(p_Level, "mod", p_Line);
}

} // namespace CoopLog
