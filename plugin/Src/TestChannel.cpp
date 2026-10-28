#include "TestChannel.h"

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>

std::string TestChannel::Poll(double p_Now) {
    if (!Enabled() || p_Now < m_NextPoll)
        return {};

    m_NextPoll = p_Now + 0.25;

    std::ifstream s_File(m_Path);

    if (!s_File)
        return {};

    std::string s_Line;
    std::getline(s_File, s_Line);
    s_File.close();
    std::remove(m_Path.c_str());

    while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == ' '))
        s_Line.pop_back();

    return s_Line;
}

bool TestChannel::StatusDue(double p_Now) {
    if (!Enabled() || p_Now < m_NextStatus)
        return false;

    m_NextStatus = p_Now + 0.5;
    return true;
}

void TestChannel::WriteStatus(const std::string& p_Json) {
    // Write then rename, so a reader never sees half a file.
    const std::string s_Final = m_Path + ".status";
    const std::string s_Temp = s_Final + ".tmp";

    {
        std::ofstream s_File(s_Temp, std::ios::trunc);

        if (!s_File)
            return;

        s_File << p_Json;
    }

    std::remove(s_Final.c_str());
    std::rename(s_Temp.c_str(), s_Final.c_str());
}

void TestChannel::Trace(const std::string& p_Line) {
    if (!Enabled())
        return;

    std::ofstream s_File(m_Path + ".trace", std::ios::app);

    // Milliseconds since start, to line trace lines up with a video.
    if (s_File)
        s_File << '[' << GetTickCount64() % 10000000 << "] " << p_Line << '\n';
}

std::string TestChannel::Escape(const std::string& p_Text) {
    std::string s_Out;

    for (const char c : p_Text) {
        if (c == '"' || c == '\\') {
            s_Out += '\\';
            s_Out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20) {
            s_Out += ' ';
        }
        else {
            s_Out += c;
        }
    }

    return s_Out;
}
