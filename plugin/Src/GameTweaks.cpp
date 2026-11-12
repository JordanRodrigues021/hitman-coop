#include "GameTweaks.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
const uint8_t c_Magic[16] = { 0x22, 0x3D, 0x6F, 0x9A, 0xB3, 0xF8, 0xFE, 0xB6, 0x61, 0xD9, 0xCC, 0x1C, 0x62, 0xDE, 0x83, 0x41 };
const uint32_t c_Key[4] = { 0x30F95282, 0x1F48C419, 0x295F8548, 0x2A78366D };
constexpr uint32_t c_Delta = 0x61C88647;

constexpr const char* c_Section = "application";
constexpr const char* c_Name = "OnlineDisableAutoReconnect";
constexpr const char* c_Value = "0";

std::string g_State = "not looked at";

uint32_t Crc32(const uint8_t* p_Data, size_t p_Size) {
    uint32_t s_Crc = 0xFFFFFFFF;

    for (size_t i = 0; i < p_Size; ++i) {
        s_Crc ^= p_Data[i];

        for (int k = 0; k < 8; ++k)
            s_Crc = (s_Crc >> 1) ^ (0xEDB88320 & (0 - (s_Crc & 1)));
    }

    return ~s_Crc;
}

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool ReadFile(const std::string& p_Path, std::vector<uint8_t>& p_Out) {
    std::ifstream s_In(p_Path, std::ios::binary);

    if (!s_In)
        return false;

    p_Out.assign(std::istreambuf_iterator<char>(s_In), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFile(const std::string& p_Path, const std::vector<uint8_t>& p_Data) {
    std::ofstream s_Out(p_Path, std::ios::binary | std::ios::trunc);

    if (!s_Out)
        return false;

    s_Out.write(reinterpret_cast<const char*>(p_Data.data()), static_cast<std::streamsize>(p_Data.size()));
    return static_cast<bool>(s_Out);
}

bool Decrypt(const std::vector<uint8_t>& p_Data, std::string& p_Text) {
    if (p_Data.size() < 28 || (p_Data.size() - 20) % 8 != 0 || std::memcmp(p_Data.data(), c_Magic, 16) != 0)
        return false;

    std::vector<uint8_t> s_Plain(p_Data.size() - 20);

    for (size_t i = 0; i < s_Plain.size(); i += 8) {
        uint32_t v0 = U32(&p_Data[20 + i]), v1 = U32(&p_Data[24 + i]), s_Sum = 0xC6EF3720;

        for (int r = 0; r < 32; ++r) {
            v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s_Sum + c_Key[(s_Sum >> 11) & 3]);
            s_Sum += c_Delta;
            v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s_Sum + c_Key[s_Sum & 3]);
        }

        std::memcpy(&s_Plain[i], &v0, 4);
        std::memcpy(&s_Plain[i + 4], &v1, 4);
    }

    size_t s_Length = s_Plain.size();

    while (s_Length > 0 && s_Plain[s_Length - 1] == 0)
        --s_Length;

    if (Crc32(s_Plain.data(), s_Length) != U32(&p_Data[16]))
        return false;

    p_Text.assign(reinterpret_cast<const char*>(s_Plain.data()), s_Length);
    return true;
}

std::vector<uint8_t> Encrypt(const std::string& p_Text) {
    const size_t s_Padded = (p_Text.size() + 7) / 8 * 8;
    std::vector<uint8_t> s_Plain(s_Padded, 0);
    std::memcpy(s_Plain.data(), p_Text.data(), p_Text.size());
    std::vector<uint8_t> s_Data(20 + s_Padded);
    std::memcpy(s_Data.data(), c_Magic, 16);
    const uint32_t s_Crc = Crc32(reinterpret_cast<const uint8_t*>(p_Text.data()), p_Text.size());
    std::memcpy(&s_Data[16], &s_Crc, 4);

    for (size_t i = 0; i < s_Padded; i += 8) {
        uint32_t v0 = U32(&s_Plain[i]), v1 = U32(&s_Plain[i + 4]), s_Sum = 0;

        for (int r = 0; r < 32; ++r) {
            v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s_Sum + c_Key[s_Sum & 3]);
            s_Sum -= c_Delta;
            v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s_Sum + c_Key[(s_Sum >> 11) & 3]);
        }

        std::memcpy(&s_Data[20 + i], &v0, 4);
        std::memcpy(&s_Data[24 + i], &v1, 4);
    }

    return s_Data;
}

std::string Trim(const std::string& p_Text) {
    const size_t s_From = p_Text.find_first_not_of(" \t\r\n");
    const size_t s_To = p_Text.find_last_not_of(" \t\r\n");
    return s_From == std::string::npos ? std::string() : p_Text.substr(s_From, s_To - s_From + 1);
}

bool SameName(const std::string& a, const char* b) {
    return _stricmp(a.c_str(), b) == 0;
}

// The line "Name=..." in [section]: where it starts and ends in p_Text (without its line end), and
// where the section's heading ends. False when the section isn't there.
struct Found {
    bool section = false, line = false;
    size_t lineFrom = 0, lineTo = 0, afterHeading = 0;
    std::string value, lineEnd = "\n";
};

Found Find(const std::string& p_Text) {
    Found s_Found;
    std::string s_Current;
    size_t s_At = 0;

    while (s_At <= p_Text.size()) {
        size_t s_End = p_Text.find('\n', s_At);
        const bool s_Last = s_End == std::string::npos;

        if (s_Last)
            s_End = p_Text.size();

        const std::string s_Raw = p_Text.substr(s_At, s_End - s_At);
        const std::string s_Line = Trim(s_Raw);

        if (s_Line.size() >= 2 && s_Line.front() == '[' && s_Line.back() == ']') {
            s_Current = s_Line.substr(1, s_Line.size() - 2);

            if (SameName(s_Current, c_Section)) {
                s_Found.section = true;
                s_Found.afterHeading = s_Last ? s_End : s_End + 1;
                s_Found.lineEnd = !s_Raw.empty() && s_Raw.back() == '\r' ? "\r\n" : "\n";
            }
        }
        else if (SameName(s_Current, c_Section) && !s_Line.empty() && s_Line[0] != '#') {
            if (const size_t s_Eq = s_Line.find('='); s_Eq != std::string::npos && SameName(Trim(s_Line.substr(0, s_Eq)), c_Name)) {
                s_Found.line = true;
                s_Found.lineFrom = s_At;
                s_Found.lineTo = !s_Raw.empty() && s_Raw.back() == '\r' ? s_End - 1 : s_End;
                s_Found.value = Trim(s_Line.substr(s_Eq + 1));
                return s_Found;
            }
        }

        if (s_Last)
            break;

        s_At = s_End + 1;
    }

    return s_Found;
}

// Whether the setup noted this change in the install file (so it is ours to make again).
bool NotedByTheSetup(const std::string& p_Retail) {
    std::vector<uint8_t> s_Json;

    if (!ReadFile(p_Retail + "HitmanCoop-install.json", s_Json))
        return false;

    const std::string s_Text(s_Json.begin(), s_Json.end());
    const size_t s_Tweaks = s_Text.find("\"gametweaks\"");
    return s_Tweaks != std::string::npos && s_Text.find(std::string(c_Section) + "/" + c_Name, s_Tweaks) != std::string::npos;
}
} // namespace

std::string GameTweaks::ReadPlain(const std::string& p_Path) {
    std::vector<uint8_t> s_Data;
    std::string s_Text;
    return ReadFile(p_Path, s_Data) && Decrypt(s_Data, s_Text) ? s_Text : std::string();
}

const std::string& GameTweaks::State() {
    return g_State;
}

std::string GameTweaks::CheckAndHeal(const std::string& p_Retail) {
    const std::string s_Path = p_Retail + "thumbs.dat";
    std::vector<uint8_t> s_Data;
    std::string s_Text;

    if (!ReadFile(s_Path, s_Data))
        return g_State = "thumbs.dat not found";

    if (!Decrypt(s_Data, s_Text))
        return g_State = "thumbs.dat isn't in the format the mod knows (left alone)";

    const Found s_Found = Find(s_Text);

    if (s_Found.line && s_Found.value == c_Value)
        return g_State = "thumbs.dat tuned for co-op (the game reconnects to IOI's servers by itself)";

    if (!NotedByTheSetup(p_Retail))
        return g_State = "thumbs.dat as the game ships it (the setup's co-op tuning is off)";

    if (!s_Found.section)
        return g_State = "thumbs.dat has no [application] section (left alone)";

    // The setup made this change and a game update undid it: made again, the game's file kept.
    std::string s_New = s_Text;

    if (s_Found.line)
        s_New.replace(s_Found.lineFrom, s_Found.lineTo - s_Found.lineFrom, std::string(c_Name) + "=" + c_Value);
    else
        s_New.insert(s_Found.afterHeading, std::string(c_Name) + "=" + c_Value + s_Found.lineEnd);

    const std::vector<uint8_t> s_Out = Encrypt(s_New);
    std::string s_Check;

    if (!Decrypt(s_Out, s_Check) || s_Check != s_New)
        return g_State = "thumbs.dat: the tuned file didn't check out (left alone)";

    if (!WriteFile(p_Retail + "thumbs.dat.before-hitmancoop", s_Data) || !WriteFile(s_Path, s_Out))
        return g_State = "thumbs.dat couldn't be written (as the game ships it; run the setup again)";

    return g_State = "thumbs.dat tuned for co-op again after a game update (from the next start of the game)";
}
