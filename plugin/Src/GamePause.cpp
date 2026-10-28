#include "GamePause.h"

#include "CrashGuards.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "Globals.h"
#include "Hooks.h"

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZString.h>

namespace {
    // A menu asking for a pause (3.280.0: inlined at four places, all alike):
    //   lea rdx,[rsp+20h]; lea rcx,[requests]; call Find; cmp eax,-1; jne
    //   lea rdx,[rsp+20h]; lea rcx,[requests]; call PushBack; mov byte ptr [changed],1
    constexpr const char* c_Request =
        "48 8D 54 24 20 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 F8 FF 75 ?? 48 8D 54 24 20 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01";

    using FindFn = int32_t (*)(void* p_Requests, const ZString* p_Name);
    using PushFn = void (*)(void* p_Requests, const ZString* p_Name);

    bool g_Looked = false;
    void* g_Requests = nullptr;
    uint8_t* g_Changed = nullptr;
    FindFn g_Find = nullptr;
    PushFn g_Push = nullptr;
    bool g_Holding = false;
    bool g_Waiting = false;

    // Static strings: the game only frees ones it allocated.
    constexpr char c_Ours[] = "HitmanCoopPartner";
    constexpr char c_Wait[] = "HitmanCoopWait";
    constexpr char c_Menus[] = "UI_Menus";

    uintptr_t Rel(uintptr_t p_Rel32At) {
        int32_t s_Rel;
        std::memcpy(&s_Rel, reinterpret_cast<const void*>(p_Rel32At), 4);
        return p_Rel32At + 4 + s_Rel;
    }

    void Look() {
        if (g_Looked)
            return;

        g_Looked = true;
        const auto s_Sites = CrashGuards::FindAll(c_Request);

        if (s_Sites.empty())
            return;

        // Every site must agree on the list, the functions and the flag.
        for (const uintptr_t s_At : s_Sites) {
            void* s_Requests = reinterpret_cast<void*>(Rel(s_At + 8));
            auto s_Find = reinterpret_cast<FindFn>(Rel(s_At + 13));
            auto s_Push = reinterpret_cast<PushFn>(Rel(s_At + 35));
            auto* s_Changed = reinterpret_cast<uint8_t*>(Rel(s_At + 41) + 1); // the imm8 follows the disp32

            if (s_At != s_Sites[0] && (s_Requests != g_Requests || s_Find != g_Find || s_Push != g_Push || s_Changed != g_Changed)) {
                g_Requests = nullptr;
                return;
            }

            g_Requests = s_Requests;
            g_Find = s_Find;
            g_Push = s_Push;
            g_Changed = s_Changed;
        }

        // The list belongs to the game loop manager the SDK knows (it releases from it).
        const auto s_Manager = reinterpret_cast<uintptr_t>(Globals::GameLoopManager);

        if (!s_Manager || reinterpret_cast<uintptr_t>(g_Requests) - s_Manager > 0x400)
            g_Requests = nullptr;
    }

    ZString Name(const char* p_Text, size_t p_Length) { return ZString(std::string_view(p_Text, p_Length)); }
}

namespace {
    // Registered under the hash of its name: lea rax,[RestartLevel]; mov dword ptr [..],CRC32("RestartLevel")
    constexpr const char* c_RestartRegistration = "48 8D 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 3E AE 2B 50";
    using RestartFn = void (*)();
    RestartFn g_Restart = nullptr;
    bool g_RestartLooked = false;

    void LookRestart() {
        if (g_RestartLooked)
            return;

        g_RestartLooked = true;
        const auto s_Sites = CrashGuards::FindAll(c_RestartRegistration);

        if (s_Sites.size() == 1)
            g_Restart = reinterpret_cast<RestartFn>(Rel(s_Sites[0] + 3));
    }
}

bool GamePause::CanRestart() {
    LookRestart();
    return g_Restart != nullptr;
}

bool GamePause::Restart() {
    if (!CanRestart())
        return false;

    g_Restart();
    return true;
}

void GamePause::Forget() { g_Holding = g_Waiting = false; }

bool GamePause::Holding() { return g_Holding || g_Waiting; }

bool GamePause::Available() {
    Look();
    return g_Requests != nullptr;
}

bool GamePause::MenuOpen() {
    if (!Available())
        return false;

    const ZString s_Menus = Name(c_Menus, sizeof(c_Menus) - 1);
    return g_Find(g_Requests, &s_Menus) >= 0;
}

namespace {
    // A pause request of ours, by its name, pushed once and released once.
    void SetHold(const char* p_Name, size_t p_Length, bool p_Paused, bool& p_Holding) {
        if (!GamePause::Available() || p_Paused == p_Holding)
            return;

        const ZString s_Name = Name(p_Name, p_Length);

        if (p_Paused) {
            if (g_Find(g_Requests, &s_Name) < 0) {
                g_Push(g_Requests, &s_Name);
                *g_Changed = 1;
            }
        }
        else
            Hooks::ZGameLoopManager_ReleasePause->Call(Globals::GameLoopManager, s_Name);

        p_Holding = p_Paused;
    }
}

void GamePause::SetPartnerPause(bool p_Paused) { SetHold(c_Ours, sizeof(c_Ours) - 1, p_Paused, g_Holding); }

void GamePause::SetWaitPause(bool p_Paused) { SetHold(c_Wait, sizeof(c_Wait) - 1, p_Paused, g_Waiting); }
