#include "Updater.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace {
constexpr const char* c_WhatsNewFile = "mods\\HitmanCoop-whatsnew.txt";

std::wstring Wide(const std::string& p_Text) {
    if (p_Text.empty())
        return {};

    const int s_Size = MultiByteToWideChar(CP_UTF8, 0, p_Text.data(), static_cast<int>(p_Text.size()), nullptr, 0);
    std::wstring s_Out(static_cast<size_t>(s_Size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p_Text.data(), static_cast<int>(p_Text.size()), s_Out.data(), s_Size);
    return s_Out;
}

std::string Narrow(const std::wstring& p_Text) {
    if (p_Text.empty())
        return {};

    const int s_Size = WideCharToMultiByte(CP_UTF8, 0, p_Text.data(), static_cast<int>(p_Text.size()), nullptr, 0, nullptr, nullptr);
    std::string s_Out(static_cast<size_t>(s_Size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p_Text.data(), static_cast<int>(p_Text.size()), s_Out.data(), s_Size, nullptr, nullptr);
    return s_Out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET p_H) : h(p_H) {}
    ~Handle() {
        if (h)
            WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// One GET over HTTPS (plain HTTP only to this PC, for tests), redirects followed (GitHub's release
// downloads redirect to its file servers), at most p_Max bytes. p_Retry: it failed on the way
// (no connection, broke off), not with an answer.
bool HttpGetOnce(const std::string& p_Url, std::string& p_Body, size_t p_Max, const std::atomic<bool>& p_Stop, std::string& p_Error, bool& p_Retry) {
    p_Retry = false;

    const std::wstring s_Url = Wide(p_Url);
    URL_COMPONENTS s_Parts {};
    s_Parts.dwStructSize = sizeof(s_Parts);
    s_Parts.dwHostNameLength = static_cast<DWORD>(-1);
    s_Parts.dwUrlPathLength = static_cast<DWORD>(-1);
    s_Parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(s_Url.c_str(), 0, 0, &s_Parts)) {
        p_Error = "bad address " + p_Url;
        return false;
    }

    const std::wstring s_Host(s_Parts.lpszHostName, s_Parts.dwHostNameLength);
    const bool s_Secure = s_Parts.nScheme == INTERNET_SCHEME_HTTPS;

    if (!s_Secure && s_Host != L"127.0.0.1" && s_Host != L"localhost") {
        p_Error = "not https: " + p_Url;
        return false;
    }

    std::wstring s_Path(s_Parts.lpszUrlPath, s_Parts.dwUrlPathLength);
    s_Path.append(s_Parts.lpszExtraInfo, s_Parts.dwExtraInfoLength);

    Handle s_Session(WinHttpOpen(L"HitmanCoop-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));

    if (!s_Session.h) {
        p_Error = "no internet access (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    WinHttpSetTimeouts(s_Session.h, 10000, 5000, 15000, 30000);
    Handle s_Connect(WinHttpConnect(s_Session.h, s_Host.c_str(), s_Parts.nPort, 0));
    Handle s_Request(s_Connect.h ? WinHttpOpenRequest(s_Connect.h, L"GET", s_Path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, s_Secure ? WINHTTP_FLAG_SECURE : 0)
                                 : nullptr);

    if (!s_Request.h || !WinHttpSendRequest(s_Request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(s_Request.h, nullptr)) {
        p_Error = "can't reach " + Narrow(s_Host) + " (" + std::to_string(GetLastError()) + ")";
        p_Retry = true;
        return false;
    }

    DWORD s_Code = 0, s_CodeSize = sizeof(s_Code);
    WinHttpQueryHeaders(s_Request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &s_Code,
        &s_CodeSize, WINHTTP_NO_HEADER_INDEX);

    if (s_Code != 200) {
        p_Error = "HTTP " + std::to_string(s_Code) + " for " + p_Url;
        return false;
    }

    p_Body.clear();

    for (;;) {
        if (p_Stop) {
            p_Error = "stopped";
            return false;
        }

        DWORD s_Available = 0;

        if (!WinHttpQueryDataAvailable(s_Request.h, &s_Available)) {
            p_Error = "download broke off (" + std::to_string(GetLastError()) + ")";
            p_Retry = true;
            return false;
        }

        if (s_Available == 0)
            return true;

        if (p_Body.size() + s_Available > p_Max) {
            p_Error = "too big: " + p_Url;
            return false;
        }

        const size_t s_At = p_Body.size();
        p_Body.resize(s_At + s_Available);
        DWORD s_Read = 0;

        if (!WinHttpReadData(s_Request.h, p_Body.data() + s_At, s_Available, &s_Read)) {
            p_Error = "download broke off (" + std::to_string(GetLastError()) + ")";
            p_Retry = true;
            return false;
        }

        p_Body.resize(s_At + s_Read);
    }
}

// A few tries: a host's addresses come in another order on each try, and players' connections
// can't reach every one (one of GitHub's file server addresses timed out on a player's PC while the
// others answered; Windows' HTTP waits out the whole connect timeout on it).
bool HttpGet(const std::string& p_Url, std::string& p_Body, size_t p_Max, const std::atomic<bool>& p_Stop, std::string& p_Error) {
    for (int i = 0; i < 4 && !p_Stop; ++i) {
        bool s_Retry = false;

        if (HttpGetOnce(p_Url, p_Body, p_Max, p_Stop, p_Error, s_Retry))
            return true;

        if (!s_Retry)
            return false;

        Sleep(500);
    }

    return false;
}

bool ReadAll(const fs::path& p_Path, std::string& p_Out) {
    std::ifstream s_In(p_Path, std::ios::binary);

    if (!s_In)
        return false;

    std::ostringstream s_Buffer;
    s_Buffer << s_In.rdbuf();
    p_Out = s_Buffer.str();
    return true;
}

bool WriteAll(const fs::path& p_Path, const std::string& p_Data) {
    std::ofstream s_Out(p_Path, std::ios::binary | std::ios::trunc);
    s_Out.write(p_Data.data(), static_cast<std::streamsize>(p_Data.size()));
    return static_cast<bool>(s_Out);
}

// A path in update.txt stays inside the game folder: relative, no "..", no drive.
bool SafePath(const std::string& p_Path) {
    if (p_Path.empty() || p_Path.size() > 120 || p_Path.front() == '/' || p_Path.front() == '\\' || p_Path.find(':') != std::string::npos ||
        p_Path.find("..") != std::string::npos)
        return false;

    return std::all_of(p_Path.begin(), p_Path.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
    });
}

std::string FileName(const std::string& p_Path) {
    const size_t s_Slash = p_Path.find_last_of('/');
    return s_Slash == std::string::npos ? p_Path : p_Path.substr(s_Slash + 1);
}

fs::path Local(const std::string& p_GameDir, const std::string& p_Path) {
    std::string s_Path = p_Path;
    std::replace(s_Path.begin(), s_Path.end(), '/', '\\');
    return fs::path(p_GameDir) / s_Path;
}

fs::path WithSuffix(const fs::path& p_Path, const char* p_Suffix) {
    fs::path s_Out = p_Path;
    s_Out += p_Suffix;
    return s_Out;
}
}

Updater::~Updater() {
    m_Stop = true;

    if (m_Thread.joinable())
        m_Thread.join();
}

std::string Updater::Sha256(const std::string& p_Data) {
    BCRYPT_ALG_HANDLE s_Alg = nullptr;
    BCRYPT_HASH_HANDLE s_Hash = nullptr;
    unsigned char s_Digest[32] {};
    std::string s_Hex;

    if (BCryptOpenAlgorithmProvider(&s_Alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(s_Alg, &s_Hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(s_Hash, reinterpret_cast<PUCHAR>(const_cast<char*>(p_Data.data())), static_cast<ULONG>(p_Data.size()), 0) == 0 &&
                BCryptFinishHash(s_Hash, s_Digest, sizeof(s_Digest), 0) == 0) {
                char s_Byte[3];

                for (unsigned char c : s_Digest) {
                    std::snprintf(s_Byte, sizeof(s_Byte), "%02x", c);
                    s_Hex += s_Byte;
                }
            }

            BCryptDestroyHash(s_Hash);
        }

        BCryptCloseAlgorithmProvider(s_Alg, 0);
    }

    return s_Hex;
}

int Updater::CompareVersions(const std::string& p_A, const std::string& p_B) {
    std::istringstream s_A(p_A), s_B(p_B);

    for (int i = 0; i < 4; ++i) {
        int a = 0, b = 0;
        char s_Dot;
        s_A >> a;
        s_B >> b;
        s_A >> s_Dot;
        s_B >> s_Dot;

        if (a != b)
            return a < b ? -1 : 1;
    }

    return 0;
}

bool Updater::Parse(const std::string& p_Text, Manifest& p_Out) {
    std::istringstream s_In(p_Text);
    std::string s_Line;
    bool s_Notes = false;
    p_Out = {};

    while (std::getline(s_In, s_Line)) {
        if (!s_Line.empty() && s_Line.back() == '\r')
            s_Line.pop_back();

        if (s_Notes) {
            p_Out.notes += s_Line + "\n";
            continue;
        }

        std::istringstream s_Words(s_Line);
        std::string s_Key;
        s_Words >> s_Key;

        if (s_Key == "version")
            s_Words >> p_Out.version;
        else if (s_Key == "base")
            s_Words >> p_Out.base;
        else if (s_Key == "file") {
            File s_File;

            if (!(s_Words >> s_File.path >> s_File.size >> s_File.sha256) || !SafePath(s_File.path) || s_File.sha256.size() != 64)
                return false;

            p_Out.files.push_back(s_File);
        }
        else if (s_Key == "notes")
            s_Notes = true;
    }

    while (!p_Out.notes.empty() && (p_Out.notes.back() == '\n' || p_Out.notes.back() == ' '))
        p_Out.notes.pop_back();

    return !p_Out.version.empty() && !p_Out.base.empty() && !p_Out.files.empty();
}

void Updater::CleanUp(const std::string& p_GameDir) {
    std::error_code s_Error;

    for (const fs::path& s_Dir : { fs::path(p_GameDir), fs::path(p_GameDir) / "mods" }) {
        for (const auto& s_Entry : fs::directory_iterator(s_Dir, s_Error)) {
            const auto s_Ext = s_Entry.path().extension();

            if (s_Ext == ".hcold" || s_Ext == ".hcnew")
                fs::remove(s_Entry.path(), s_Error);
        }
    }
}

bool Updater::TakeWhatsNew(const std::string& p_GameDir, const std::string& p_Version, std::string& p_Notes) {
    const fs::path s_Path = fs::path(p_GameDir) / c_WhatsNewFile;
    std::string s_Text;

    if (!ReadAll(s_Path, s_Text))
        return false;

    std::istringstream s_In(s_Text);
    std::string s_Word, s_Version;
    s_In >> s_Word >> s_Version;

    // Not the version running (the update didn't load): kept until it does, unless it's older.
    if (s_Word != "version" || s_Version != p_Version) {
        if (s_Word != "version" || CompareVersions(s_Version, p_Version) < 0) {
            std::error_code s_Error;
            fs::remove(s_Path, s_Error);
        }

        return false;
    }

    std::getline(s_In, s_Word);
    p_Notes.assign(std::istreambuf_iterator<char>(s_In), {});
    std::error_code s_Error;
    fs::remove(s_Path, s_Error);
    return true;
}

void Updater::Start(const std::string& p_GameDir, const std::string& p_Version, const std::string& p_ManifestUrl, const std::string& p_FallbackUrl) {
    if (m_Thread.joinable())
        return;

    Set(State::Checking);
    m_Thread = std::thread(&Updater::Run, this, p_GameDir, p_Version, p_ManifestUrl, p_FallbackUrl);
}

Updater::Status Updater::Get() const {
    std::lock_guard s_Lock(m_Mutex);
    return m_Status;
}

void Updater::Set(State p_State, const std::string& p_Version, const std::string& p_Error) {
    std::lock_guard s_Lock(m_Mutex);
    m_Status = { p_State, p_Version, p_Error };
}

void Updater::Run(std::string p_GameDir, std::string p_Version, std::string p_ManifestUrl, std::string p_FallbackUrl) {
    std::string s_Text, s_Error;
    Manifest s_Manifest;

    if (!HttpGet(p_ManifestUrl, s_Text, 256 * 1024, m_Stop, s_Error) &&
        (p_FallbackUrl.empty() || !HttpGet(p_FallbackUrl, s_Text, 256 * 1024, m_Stop, s_Error)))
        return Set(State::Failed, {}, s_Error);

    if (!Parse(s_Text, s_Manifest))
        return Set(State::Failed, {}, "update.txt isn't readable");

    if (CompareVersions(s_Manifest.version, p_Version) <= 0)
        return Set(State::UpToDate, s_Manifest.version);

    // Every file that differs, downloaded and checked first: nothing is swapped unless all are.
    struct Staged {
        fs::path target;
        fs::path fresh;
    };
    std::vector<Staged> s_Staged;

    const auto s_Discard = [&] {
        std::error_code s_Ignored;

        for (const auto& s_File : s_Staged)
            fs::remove(s_File.fresh, s_Ignored);
    };

    for (const auto& s_File : s_Manifest.files) {
        const fs::path s_Target = Local(p_GameDir, s_File.path);
        std::string s_Have;

        if (ReadAll(s_Target, s_Have) && s_Have.size() == s_File.size && Sha256(s_Have) == s_File.sha256)
            continue;

        std::string s_Data;

        if (!HttpGet(s_Manifest.base + FileName(s_File.path), s_Data, 128ull * 1024 * 1024, m_Stop, s_Error)) {
            s_Discard();
            return Set(State::Failed, s_Manifest.version, s_Error);
        }

        if (s_Data.size() != s_File.size || Sha256(s_Data) != s_File.sha256) {
            s_Discard();
            return Set(State::Failed, s_Manifest.version, FileName(s_File.path) + " didn't match its SHA-256");
        }

        const fs::path s_Fresh = WithSuffix(s_Target, ".hcnew");

        if (!WriteAll(s_Fresh, s_Data)) {
            s_Discard();
            return Set(State::Failed, s_Manifest.version, "can't write in the game folder (" + s_Fresh.parent_path().string() + ")");
        }

        s_Staged.push_back({ s_Target, s_Fresh });
    }

    // Swapped in: the running file renamed out of the way (Windows allows that for a loaded DLL),
    // the new one renamed in. Undone if any of them can't be.
    std::vector<Staged> s_Swapped;
    bool s_Ok = true;

    for (const auto& s_File : s_Staged) {
        const fs::path s_Old = WithSuffix(s_File.target, ".hcold");
        const std::wstring s_OldW = s_Old.wstring(), s_TargetW = s_File.target.wstring(), s_FreshW = s_File.fresh.wstring();
        DeleteFileW(s_OldW.c_str());
        const bool s_Had = GetFileAttributesW(s_TargetW.c_str()) != INVALID_FILE_ATTRIBUTES;

        if (s_Had && !MoveFileExW(s_TargetW.c_str(), s_OldW.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            s_Ok = false;
            break;
        }

        if (!MoveFileExW(s_FreshW.c_str(), s_TargetW.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (s_Had)
                MoveFileExW(s_OldW.c_str(), s_TargetW.c_str(), MOVEFILE_REPLACE_EXISTING);

            s_Ok = false;
            break;
        }

        s_Swapped.push_back(s_File);
    }

    if (!s_Ok) {
        for (const auto& s_File : s_Swapped) {
            const std::wstring s_OldW = WithSuffix(s_File.target, ".hcold").wstring(), s_TargetW = s_File.target.wstring();
            DeleteFileW(s_TargetW.c_str());
            MoveFileExW(s_OldW.c_str(), s_TargetW.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        s_Discard();
        return Set(State::Failed, s_Manifest.version, "can't replace the mod's files (" + std::to_string(GetLastError()) + ")");
    }

    WriteAll(fs::path(p_GameDir) / c_WhatsNewFile, "version " + s_Manifest.version + "\n" + s_Manifest.notes + "\n");
    Set(State::Staged, s_Manifest.version);
}
