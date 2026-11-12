#include "OnlineWatch.h"

#include <Windows.h>

#include <cstdio>
#include <mutex>

#include "Globals.h"

namespace {
    using GetIface = void* (*)();
    using GetU64 = uint64_t (*)(void*);
    using GetBool = bool (*)(void*);

    bool g_Started = false;
    GetIface g_User = nullptr, g_Apps = nullptr;
    GetU64 g_SteamId = nullptr, g_AppOwner = nullptr;
    GetBool g_LoggedOn = nullptr, g_FamilyShared = nullptr;

    std::mutex g_Mutex;
    double g_NextCheck = 0.0;
    int32_t g_Online = -1;        // ZOnlineManager state, -1 unknown
    int g_Steam = -1;             // 1 signed in, 0 not, -1 unknown / not Steam
    bool g_Family = false;
    uint64_t g_Id = 0;
    double g_LastWarn = -1e9;
    uint32_t g_Drops = 0, g_SteamDrops = 0;
    std::string g_History;        // the last few changes, for bug reports

    template <class T>
    T Find(HMODULE p_Dll, const char* p_Prefix, int p_From, int p_To) {
        // The accessors carry the interface version in their name (SteamAPI_SteamUser_v023): the
        // newest the game's DLL has.
        for (int v = p_To; v >= p_From; --v) {
            char s_Name[96];
            std::snprintf(s_Name, sizeof(s_Name), "%s%03d", p_Prefix, v);
            if (const auto s_Fn = GetProcAddress(p_Dll, s_Name))
                return reinterpret_cast<T>(s_Fn);
        }
        return nullptr;
    }

    // The game's online state, read without trusting the pointer (a build with another layout, or
    // the manager not made yet).
    int32_t ReadOnlineState() {
        const auto* s_Manager = reinterpret_cast<const uint8_t*>(Globals::OnlineManager);
        if (!s_Manager)
            return -1;

        int32_t s_State = -1;
        __try {
            s_State = *reinterpret_cast<const int32_t*>(s_Manager + 0x18);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        return s_State >= 1 && s_State <= 46 ? s_State : -1;
    }

    bool SteamCall(GetBool p_Fn, void* p_Iface, bool& p_Out) {
        __try {
            p_Out = p_Fn(p_Iface);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SteamCallU64(GetU64 p_Fn, void* p_Iface, uint64_t& p_Out) {
        __try {
            p_Out = p_Fn(p_Iface);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* Iface(GetIface p_Fn) {
        __try {
            return p_Fn ? p_Fn() : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    std::string Tag(uint64_t p_Id) {
        // FNV-1a over a salted text: the partner can compare it with theirs, not read the id off it.
        uint64_t h = 1469598103934665603ull;
        char s_Text[64];
        std::snprintf(s_Text, sizeof(s_Text), "hitman-coop/account/%llu", static_cast<unsigned long long>(p_Id));
        for (const char* c = s_Text; *c; ++c) {
            h ^= static_cast<uint8_t>(*c);
            h *= 1099511628211ull;
        }
        char s_Hex[20];
        std::snprintf(s_Hex, sizeof(s_Hex), "%016llx", static_cast<unsigned long long>(h));
        return s_Hex;
    }

    void Remember(double p_Now, const std::string& p_What) {
        char s_At[24];
        std::snprintf(s_At, sizeof(s_At), "[%.0f] ", p_Now);
        g_History += s_At + p_What + "; ";
        if (g_History.size() > 600)
            g_History.erase(0, g_History.size() - 600);
    }
}

const char* OnlineWatch::OnlineStateName(int32_t p_State) {
    static constexpr const char* c_Names[] = { "?", "trigger flow", "offline", "suspended", "disconnected", "starting to connect", "pre-authenticating",
        "fetching API version", "fetching API version", "API version received", "fetching configuration", "configuration received", "online consent",
        "syncing entitlements", "entitlements synced", "authenticating", "authenticated", "syncing entitlements", "entitlements synced",
        "authenticating game service", "game service authenticated", "IOI account sign-up", "fetching offline cache", "offline cache received",
        "fetching resources", "resources mounted", "syncing achievements", "updating friends", "checking DLC", "fetching platform user",
        "platform user received", "fetching user configuration", "user configuration received", "fetching profile", "profile received",
        "syncing profile", "profile synced", "retrieving events", "events retrieved", "waiting to sync events", "connected",
        "connecting to game session", "connecting to game session", "retrying to save events", "retrying to save events", "re-authenticating",
        "platform session reconnecting" };
    return p_State >= 1 && p_State <= 46 ? c_Names[p_State] : "unknown";
}

void OnlineWatch::Start() {
    std::lock_guard s_Lock(g_Mutex);
    if (g_Started)
        return;
    g_Started = true;

    // Steam copies only; Epic's has no steam_api64.dll loaded.
    const HMODULE s_Steam = GetModuleHandleA("steam_api64.dll");
    if (!s_Steam)
        return;

    g_User = Find<GetIface>(s_Steam, "SteamAPI_SteamUser_v", 19, 30);
    g_Apps = Find<GetIface>(s_Steam, "SteamAPI_SteamApps_v", 6, 12);
    g_SteamId = reinterpret_cast<GetU64>(GetProcAddress(s_Steam, "SteamAPI_ISteamUser_GetSteamID"));
    g_LoggedOn = reinterpret_cast<GetBool>(GetProcAddress(s_Steam, "SteamAPI_ISteamUser_BLoggedOn"));
    g_FamilyShared = reinterpret_cast<GetBool>(GetProcAddress(s_Steam, "SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing"));
    g_AppOwner = reinterpret_cast<GetU64>(GetProcAddress(s_Steam, "SteamAPI_ISteamApps_GetAppOwner"));
}

void OnlineWatch::Update(double p_Now, bool p_InSession, const std::function<void(const std::string&)>& p_Log,
    const std::function<void(const std::string&)>& p_Warn) {
    std::string s_Log, s_Warn;
    {
        std::lock_guard s_Lock(g_Mutex);
        if (p_Now < g_NextCheck)
            return;
        g_NextCheck = p_Now + 1.0;

        // IOI's servers.
        if (const int32_t s_State = ReadOnlineState(); s_State != g_Online && s_State != -1) {
            const int32_t s_Was = g_Online;
            g_Online = s_State;
            s_Log = std::string("online: the game's connection to IOI's servers: ") + OnlineStateName(s_State) + " (" + std::to_string(s_State) + ")";
            Remember(p_Now, OnlineStateName(s_State));

            // From connected to lost or signing in again: what players see as a sudden error.
            const bool s_Lost = s_Was == 40 && (s_State == 2 || s_State == 4 || s_State == 43 || s_State == 45 || s_State == 46);
            if (s_Lost) {
                ++g_Drops;
                if (p_InSession && p_Now - g_LastWarn > 60.0) {
                    g_LastWarn = p_Now;
                    s_Warn = s_State == 46 ? "Steam's session for HITMAN dropped and is reconnecting (not co-op's connection: co-op carries on). If Steam says your account "
                                             "signed in on another computer, the same Steam account is in use on another PC."
                                           : "HITMAN lost its connection to IOI's servers (not co-op's connection: co-op carries on). The game may sign in again by itself.";
                }
            }
        }

        // Steam.
        if (g_User && g_LoggedOn) {
            if (void* s_User = Iface(g_User)) {
                bool s_On = false;
                if (SteamCall(g_LoggedOn, s_User, s_On)) {
                    if (g_Steam == 1 && !s_On) {
                        ++g_SteamDrops;
                        Remember(p_Now, "Steam signed out");
                        s_Log += std::string(s_Log.empty() ? "" : "\n") + "online: Steam isn't signed in on this PC any more";
                        if (p_InSession && p_Now - g_LastWarn > 60.0) {
                            g_LastWarn = p_Now;
                            s_Warn = "Steam signed this PC out (co-op carries on). If Steam says your account signed in on another computer, "
                                     "your Steam account is in use on another PC: each player needs their own.";
                        }
                    }
                    else if (g_Steam == 0 && s_On) {
                        Remember(p_Now, "Steam signed in again");
                        s_Log += std::string(s_Log.empty() ? "" : "\n") + "online: Steam is signed in again";
                    }
                    g_Steam = s_On ? 1 : 0;
                }

                if (g_Id == 0 && g_SteamId)
                    SteamCallU64(g_SteamId, s_User, g_Id);
            }
        }

        if (g_Apps && g_FamilyShared && !g_Family && g_Id != 0) {
            if (void* s_Apps = Iface(g_Apps)) {
                bool s_Shared = false;
                uint64_t s_Owner = 0;
                const bool s_Borrowed = (SteamCall(g_FamilyShared, s_Apps, s_Shared) && s_Shared) ||
                    (g_AppOwner && SteamCallU64(g_AppOwner, s_Apps, s_Owner) && s_Owner != 0 && s_Owner != g_Id);
                if (s_Borrowed) {
                    g_Family = true;
                    s_Log += std::string(s_Log.empty() ? "" : "\n") + "online: this copy of HITMAN is borrowed through Steam Family Sharing";
                    s_Warn = "This copy of HITMAN is borrowed through Steam Family Sharing: whenever its owner starts a game, Steam signs you out "
                             "(\"signed in on another computer\"). For co-op, each player needs their own copy.";
                }
            }
        }
    }

    if (!s_Log.empty())
        p_Log(s_Log);
    if (!s_Warn.empty())
        p_Warn(s_Warn);
}

std::string OnlineWatch::AccountMessage() {
    std::lock_guard s_Lock(g_Mutex);
    return g_Id == 0 ? std::string() : Tag(g_Id) + (g_Family ? " 1" : " 0");
}

std::string OnlineWatch::CheckPartnerAccount(const std::string& p_Message, const std::string& p_PartnerName) {
    const std::string s_Theirs = p_Message.substr(0, p_Message.find(' '));
    const bool s_TheyBorrow = p_Message.size() > 17 && p_Message[17] == '1';
    const std::string s_Ours = AccountMessage();

    if (!s_Ours.empty() && s_Theirs.size() == 16 && s_Ours.compare(0, 16, s_Theirs) == 0)
        return "You and " + p_PartnerName + " are signed in to the same Steam account. Steam lets one PC use an account at a time and will sign one of you "
               "out (\"signed in on another computer\"). Each player needs their own Steam account with HITMAN.";

    if (s_TheyBorrow)
        return p_PartnerName + " plays a copy of HITMAN borrowed through Steam Family Sharing: Steam signs them out whenever the owner starts a game.";

    return {};
}

std::string OnlineWatch::Describe() {
    std::lock_guard s_Lock(g_Mutex);
    char s_Text[160];
    std::snprintf(s_Text, sizeof(s_Text), "IOI servers: %s, %u drops | Steam: %s%s, %u sign-outs", g_Online < 0 ? "unknown" : OnlineStateName(g_Online), g_Drops,
        g_Steam < 0 ? "not used" : g_Steam ? "signed in" : "NOT signed in", g_Family ? ", borrowed copy (Family Sharing)" : "", g_SteamDrops);
    return std::string(s_Text) + (g_History.empty() ? "" : " | changes: " + g_History);
}
