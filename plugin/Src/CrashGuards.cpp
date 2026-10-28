#include "CrashGuards.h"

#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
    enum class Kind {
        Return,     // at a function's entry (actor in rcx): return 0 at once
        SkipCall,   // mov rcx,[rdi+<anim>]; mov dl,1; mov rcx,[rcx+..]; call: skip it
        MuteSpeech, // in ZActor's ICharacterSpeakController::Speak: take its "no voice" path
        SkipUnit,   // at a function's entry (an actor's AI record in rcx): return at once
        NullGuard,  // a call whose result is read through: return instead when it is null
    };

    // Each place is found by its code pattern (IDA style, ?? any byte), wherever a game build has
    // it, and must match exactly once; struct offsets and call targets in it are read from the
    // match, never assumed. The patterns were taken from 3.280.0 with every offset, displacement
    // and call target left open, so builds that moved things around still match.
    struct Patch {
        const char* name;
        const char* pattern;
        size_t length; // bytes replaced (whole instructions)
        Kind kind;
        uintptr_t at = 0;
        std::vector<uint8_t> original; // what was there, and what Uninstall puts back
        bool applied = false;
    };

    Patch g_Patches[] = {
        // push rbx; sub rsp,20h; mov rbx,rcx; mov rcx,[rcx+<anim>] (3.280.0: +0x177ac0)
        { "restart guard 1", "40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? 00 00 48 8B 49 ?? E8 ?? ?? ?? ?? 80 A3 ?? ?? 00 00", 16, Kind::Return },
        // push rbx; push rsi; push rdi; sub rsp,..; mov rdi,rcx (+0x1689b0)
        { "restart guard 2", "40 53 56 57 48 81 EC ?? ?? 00 00 48 8B F9 E8 ?? ?? ?? ?? 8B 87 ?? ?? 00 00 48 8D 1D", 14, Kind::Return },
        // mov rcx,[rdi+<anim>]; mov dl,1; mov rcx,[rcx+..]; call .. (+0x16acb9)
        { "restart guard 3", "48 8B 8F ?? ?? 00 00 B2 01 48 8B 49 ?? E8 ?? ?? ?? ?? 8B 97 ?? ?? 00 00 48 8D 0D", 18, Kind::SkipCall },
        // In Speak (ZActor's ICharacterSpeakController vtable slot 6, the only caller of the
        // speech queue): cmp qword ptr [rcx+<controller>],0; mov r15,r9; mov rsi,r8; mov r14,rdx;
        // mov rbp,rcx; je <no controller: release the line, return -1> (+0x171f78)
        { "speech", "48 83 B9 ?? ?? 00 00 00 4D 8B F9 49 8B F0 4C 8B F2 48 8B E9 0F 84 ?? ?? ?? ?? 48 8B 81 ?? ?? FF FF 48 81 C1", 26, Kind::MuteSpeech },
        // The AI's per-actor reasoning, run each frame for every alive actor: reactions, searches,
        // investigations, the alert level they set. mov [rsp+10h],rbx; mov [rsp+18h],rbp; push
        // rsi; push rdi; push r14 (+0x448170)
        { "reasoning", "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 B8 ?? ?? ?? 00 E8 ?? ?? ?? ?? 48 2B E0 8B 41 ?? 48 8D 79", 14, Kind::SkipUnit },
        // ZActorAIPerceptibilityDisable's frame update (the stand-in's "unseen by NPCs"): its actor's
        // interface call returns null once a restart has torn the actor down, and it read through
        // that (crash at +0x322f55, 3.280.0). cmp byte ptr [rcx+..],1; jne ret; lea rcx,[rax+..];
        // mov rax,[rcx]; call [rax+..]; xor edx,edx; mov rcx,[rax+..]; add rsp,28h; jmp .. (+0x322f3d)
        { "perceptibility guard", "80 79 ?? 01 75 ?? 48 8D 88 ?? ?? 00 00 48 8B 01 FF 90 ?? ?? 00 00 33 D2 48 8B 48 ?? 48 83 C4 28 E9", 37, Kind::NullGuard },
    };

    // Where the reasoning's per-actor AI records are: the actor manager passes their table to the
    // loop that calls it (lea rcx,[table]; ...; call loop), and the loop indexes it by runtime id
    // (movsxd rax,[ids]; imul rcx,rax,<size>; add rcx,<skip>).
    constexpr const char* c_UnitsCaller = "48 8D 0D ?? ?? ?? ?? 48 89 54 24 38 48 8D 54 24 30 E8 ?? ?? ?? ?? 8B 87 ?? ?? 00 00 48 8D";
    constexpr const char* c_UnitsIndex = "4A 63 04 0F 48 69 C8 ?? ?? 00 00 48 83 C1 ?? 49";
    // ZKnowledge_SetGameTension: mov ebp,edx; mov rdi,rcx; cmp edx,[rcx+<tension>]; je, just past
    // its first instructions (the SDK hooks it and overwrites those), and later the actor's
    // runtime id (mov rax,[rdi+8]; movsxd rcx,[rax+<id>]; imul rax,rcx,..).
    constexpr const char* c_SetTension = "8B EA 48 8B F9 3B 91 ?? ?? 00 00 0F 84 ?? ?? ?? ?? 4C 89 74 24";
    constexpr const char* c_RuntimeId = "48 8B 47 08 48 63 88 ?? ?? 00 00 48 69 C1 ?? ?? 00 00";

    uintptr_t g_Units = 0;           // the first record (table + skip), 0 if not found
    size_t g_UnitSize = 0;
    uint32_t g_RuntimeIdOffset = 0;
    uint32_t g_TensionOffset = 0;    // in ZKnowledge, 0 if not found
    uint32_t g_AnimOffset = 0;       // ZActor's m_pAnimatedActor, from the first guard
    uint32_t g_ControllerOffset = 0; // the speech controller, in ICharacterSpeakController
    uint32_t g_SpeakerOffset = 0;    // ICharacterSpeakController within ZActor
    uintptr_t g_NoVoicePath = 0;

    // Only this actor is guarded. Other actors legitimately run these with no animated actor yet
    // (while spawning), and skipping them for every actor broke mission loading.
    std::atomic<void*> g_Actor = nullptr;
    std::atomic<void*> g_Speaker = nullptr; // g_Actor's ICharacterSpeakController
    // Every stand-in spawned, for the restart guards: Restart Mission rebuilds the mission without
    // freeing it at once, and the game tears an earlier stand-in down a restart later (crash at
    // +0x177ad0 on the second restart, 3.280.0). Null-terminated; written by the game thread only.
    constexpr size_t c_MaxGuarded = 16;
    std::atomic<void*> g_Guarded[c_MaxGuarded + 1] {};
    size_t g_GuardedNext = 0;
    std::atomic<void*> g_Knowledge = nullptr;
    // The AI records whose reasoning is skipped: the stand-in's and the partner's NPCs here
    // (puppets). Null-terminated, rebuilt into the spare buffer and swapped in, so the stub (on
    // AI threads) always reads a whole list.
    constexpr size_t c_MaxSkipped = 1024; // a whole mission's NPCs (Paris has 300)
    void* g_SkipBuffers[2][c_MaxSkipped + 1] {};
    std::atomic<void**> g_SkipList = g_SkipBuffers[0];
    int g_SkipSpare = 1;
    std::mutex g_SkipMutex;
    void* g_StandInUnit = nullptr;
    std::unordered_set<void*> g_PuppetActors;
    std::atomic<uint32_t> g_SkippedThinks = 0;
    std::atomic<uint32_t> g_MutedLines = 0;   // counted by the stub
    std::atomic<uint32_t> g_AllLines = 0;     // every actor's, for comparison
    std::atomic<uint32_t> g_Calmed = 0;
    std::atomic<int32_t> g_HighestWanted = 0;

    uint8_t* g_Stubs = nullptr;
    int g_Applied = 0;

    int32_t I32(uintptr_t p_At) { int32_t v; std::memcpy(&v, reinterpret_cast<const void*>(p_At), 4); return v; }
    uint32_t U32(uintptr_t p_At) { uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(p_At), 4); return v; }
    uintptr_t RelTarget(uintptr_t p_Rel32At) { return p_Rel32At + 4 + I32(p_Rel32At); }

    // The executable's own code section.
    bool TextRange(uintptr_t& p_From, uintptr_t& p_To) {
        const auto s_Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* s_Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(s_Base);
        const auto* s_Nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(s_Base + s_Dos->e_lfanew);
        const auto* s_Section = IMAGE_FIRST_SECTION(s_Nt);

        for (WORD i = 0; i < s_Nt->FileHeader.NumberOfSections; ++i, ++s_Section) {
            if (std::memcmp(s_Section->Name, ".text", 5) == 0) {
                p_From = s_Base + s_Section->VirtualAddress;
                p_To = p_From + s_Section->Misc.VirtualSize;
                return true;
            }
        }

        return false;
    }

    // The first place p_Pattern matches in [p_From, p_To), or 0.
    uintptr_t FindFirst(const char* p_Pattern, uintptr_t p_From, uintptr_t p_To);

    // The one place p_Pattern matches in [p_From, p_To), or 0 (none, or more than one).
    uintptr_t FindUnique(const char* p_Pattern, uintptr_t p_From, uintptr_t p_To) {
        const uintptr_t s_First = FindFirst(p_Pattern, p_From, p_To);
        return s_First && !FindFirst(p_Pattern, s_First + 1, p_To) ? s_First : 0;
    }

    uintptr_t FindFirst(const char* p_Pattern, uintptr_t p_From, uintptr_t p_To) {
        std::vector<int> s_Bytes;

        for (const char* c = p_Pattern; *c;) {
            if (*c == ' ') {
                ++c;
                continue;
            }

            s_Bytes.push_back(c[0] == '?' ? -1 : static_cast<int>(std::strtoul(std::string(c, 2).c_str(), nullptr, 16)));
            c += 2;
        }

        if (s_Bytes.empty() || p_To - p_From < s_Bytes.size())
            return 0;

        uintptr_t s_Found = 0;
        const auto* s_Mem = reinterpret_cast<const uint8_t*>(p_From);
        const size_t s_End = (p_To - p_From) - s_Bytes.size();
        const int s_First = s_Bytes[0];

        for (size_t i = 0; i <= s_End; ++i) {
            if (s_First >= 0 && s_Mem[i] != s_First)
                continue;

            size_t k = 1;

            while (k < s_Bytes.size() && (s_Bytes[k] < 0 || s_Mem[i + k] == s_Bytes[k]))
                ++k;

            if (k == s_Bytes.size())
                return p_From + i;
        }

        return s_Found;
    }

    void Emit(std::vector<uint8_t>& p_Out, std::initializer_list<uint8_t> p_Bytes) { p_Out.insert(p_Out.end(), p_Bytes); }

    void Emit32(std::vector<uint8_t>& p_Out, uint32_t p_Value) {
        for (int i = 0; i < 4; ++i)
            p_Out.push_back(static_cast<uint8_t>(p_Value >> (i * 8)));
    }

    void Emit64(std::vector<uint8_t>& p_Out, uint64_t p_Value) {
        for (int i = 0; i < 8; ++i)
            p_Out.push_back(static_cast<uint8_t>(p_Value >> (i * 8)));
    }

    // jmp qword ptr [rip+0] followed by the address: 14 bytes, reaches anywhere.
    void EmitJump(std::vector<uint8_t>& p_Out, uint64_t p_To) {
        Emit(p_Out, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });
        Emit64(p_Out, p_To);
    }

    // Leaves ZF set when "this" (rcx, or rdi) is the guarded actor and its animated actor is
    // null. Uses rax, which both places are free to clobber.
    void EmitIsGuardedAndDead(std::vector<uint8_t>& p_Out, bool p_Rdi) {
        Emit(p_Out, { 0x48, 0xB8 }); // mov rax,&g_Guarded
        Emit64(p_Out, reinterpret_cast<uint64_t>(&g_Guarded[0]));
        // next:
        Emit(p_Out, { 0x48, 0x83, 0x38, 0x00 });                                 // cmp qword ptr [rax],0
        Emit(p_Out, { 0x74, 0x18 });                                             // je none (3+2+4+2+3+8+2 bytes on)
        Emit(p_Out, { 0x48, 0x39, static_cast<uint8_t>(p_Rdi ? 0x38 : 0x08) }); // cmp [rax],rdi / rcx
        Emit(p_Out, { 0x74, 0x06 });                                             // je found (4+2 bytes on)
        Emit(p_Out, { 0x48, 0x83, 0xC0, 0x08 });                                 // add rax,8
        Emit(p_Out, { 0xEB, 0xEF });                                             // jmp next (back 17 bytes)
        // found: ZF from its animated actor being null
        Emit(p_Out, { 0x48, 0x8B, 0x00 });                                       // mov rax,[rax]
        Emit(p_Out, { 0x48, 0x83, 0xB8 });                                       // cmp qword ptr [rax+<anim>],0
        Emit32(p_Out, g_AnimOffset);
        Emit(p_Out, { 0x00 });
        Emit(p_Out, { 0xEB, 0x03 });                                             // jmp done
        // none: rax points at the terminator slot, never null, so ZF is cleared
        Emit(p_Out, { 0x48, 0x85, 0xC0 });                                       // test rax,rax
        // done:
    }

    // The original moves, then the "no voice" path for the stand-in or an actor without a speech
    // controller, else back to the original. rax is free here (the next instruction loads it).
    void EmitMuteSpeech(std::vector<uint8_t>& p_Out, uint64_t p_Resume) {
        Emit(p_Out, { 0x4D, 0x8B, 0xF9, 0x49, 0x8B, 0xF0, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xE9 }); // the four movs
        Emit(p_Out, { 0x48, 0xB8 });                                                             // mov rax,&g_AllLines
        Emit64(p_Out, reinterpret_cast<uint64_t>(&g_AllLines));
        Emit(p_Out, { 0xF0, 0xFF, 0x00 });                                                       // lock inc dword ptr [rax]
        Emit(p_Out, { 0x48, 0xB8 });                                                             // mov rax,&g_Speaker
        Emit64(p_Out, reinterpret_cast<uint64_t>(&g_Speaker));
        Emit(p_Out, { 0x48, 0x8B, 0x00 });                         // mov rax,[rax]
        Emit(p_Out, { 0x48, 0x39, 0xC8 });                         // cmp rax,rcx
        Emit(p_Out, { 0x74, 0x18 });                               // je muted (8 + 2 + 14 bytes on)
        Emit(p_Out, { 0x48, 0x83, 0xB9 });                         // cmp qword ptr [rcx+<controller>],0
        Emit32(p_Out, g_ControllerOffset);
        Emit(p_Out, { 0x00 });
        Emit(p_Out, { 0x74, 0x1B });                               // je silent (14 + 10 + 3 bytes on)
        EmitJump(p_Out, p_Resume);
        // muted: count it (lock inc dword ptr [rax]) and fall into silent
        Emit(p_Out, { 0x48, 0xB8 });                               // mov rax,&g_MutedLines
        Emit64(p_Out, reinterpret_cast<uint64_t>(&g_MutedLines));
        Emit(p_Out, { 0xF0, 0xFF, 0x00 });                         // lock inc dword ptr [rax]
        // silent:
        EmitJump(p_Out, g_NoVoicePath);
    }

    bool Write(uintptr_t p_At, const std::vector<uint8_t>& p_Bytes) {
        DWORD s_Old = 0;

        if (!VirtualProtect(reinterpret_cast<void*>(p_At), p_Bytes.size(), PAGE_EXECUTE_READWRITE, &s_Old))
            return false;

        std::memcpy(reinterpret_cast<void*>(p_At), p_Bytes.data(), p_Bytes.size());
        VirtualProtect(reinterpret_cast<void*>(p_At), p_Bytes.size(), s_Old, &s_Old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(p_At), p_Bytes.size());
        return true;
    }

    std::mutex g_FloorMutex;
    std::unordered_map<const void*, std::pair<int32_t, uint64_t>> g_Floors; // knowledge -> (tension, until GetTickCount64)
}

namespace {
    void* UnitOf(void* p_Actor) {
        if (!p_Actor || !g_Units || !g_RuntimeIdOffset)
            return nullptr;

        int32_t s_Id = -1;
        std::memcpy(&s_Id, static_cast<uint8_t*>(p_Actor) + g_RuntimeIdOffset, sizeof(s_Id));
        return s_Id >= 0 && s_Id < 500 ? reinterpret_cast<void*>(g_Units + static_cast<size_t>(s_Id) * g_UnitSize) : nullptr;
    }

    // Under g_SkipMutex.
    void RebuildSkipList() {
        void** s_List = g_SkipBuffers[g_SkipSpare];
        size_t n = 0;

        if (g_StandInUnit)
            s_List[n++] = g_StandInUnit;

        for (void* s_Actor : g_PuppetActors)
            if (void* s_Unit = UnitOf(s_Actor); s_Unit && n < c_MaxSkipped)
                s_List[n++] = s_Unit;

        s_List[n] = nullptr;
        g_SkipList = s_List;
        g_SkipSpare ^= 1;
    }
}

void CrashGuards::SetPuppet(void* p_Actor, bool p_Puppet) {
    std::lock_guard s_Lock(g_SkipMutex);

    if (p_Puppet ? g_PuppetActors.insert(p_Actor).second : g_PuppetActors.erase(p_Actor) > 0)
        RebuildSkipList();
}

void CrashGuards::ClearPuppets() {
    std::lock_guard s_Lock(g_SkipMutex);
    g_PuppetActors.clear();
    RebuildSkipList();
}

size_t CrashGuards::PuppetCount() {
    std::lock_guard s_Lock(g_SkipMutex);
    return g_PuppetActors.size();
}

void CrashGuards::SetActor(void* p_Actor) {
    g_Actor = p_Actor;

    if (p_Actor) {
        bool s_Known = false;

        for (size_t i = 0; i < c_MaxGuarded && g_Guarded[i]; ++i)
            s_Known = s_Known || g_Guarded[i] == p_Actor;

        // The oldest is replaced once all are taken: long gone by then.
        if (!s_Known) {
            g_Guarded[g_GuardedNext % c_MaxGuarded] = p_Actor;
            ++g_GuardedNext;
        }
    }

    g_Speaker = p_Actor && g_SpeakerOffset ? static_cast<uint8_t*>(p_Actor) + g_SpeakerOffset : nullptr;
}

void CrashGuards::SetKnowledge(void* p_Knowledge, bool p_Think) {
    g_Knowledge = p_Knowledge;

    // The AI record follows with it (both cleared when a scene is cleared): looked up again each
    // time, since the runtime id is only given once the actor is activated.
    void* s_Unit = p_Knowledge && !p_Think ? UnitOf(g_Actor) : nullptr;
    std::lock_guard s_Lock(g_SkipMutex);

    if (s_Unit != g_StandInUnit) {
        g_StandInUnit = s_Unit;
        RebuildSkipList();
    }
}

int32_t CrashGuards::TensionOf(const void* p_Knowledge) {
    if (!p_Knowledge || !g_TensionOffset)
        return -1;

    int32_t s_Tension;
    std::memcpy(&s_Tension, static_cast<const uint8_t*>(p_Knowledge) + g_TensionOffset, sizeof(s_Tension));
    return s_Tension;
}

void CrashGuards::SetAlertFloors(const std::vector<std::pair<const void*, int32_t>>& p_Floors, uint32_t p_ForMs) {
    const uint64_t s_Until = GetTickCount64() + p_ForMs;
    std::lock_guard s_Lock(g_FloorMutex);
    g_Floors.clear();

    for (const auto& [s_Knowledge, s_Tension] : p_Floors)
        g_Floors[s_Knowledge] = { s_Tension, s_Until };
}

int32_t CrashGuards::AlertFloor(const void* p_Knowledge) {
    std::lock_guard s_Lock(g_FloorMutex);
    const auto s_It = g_Floors.find(p_Knowledge);
    return s_It != g_Floors.end() && GetTickCount64() < s_It->second.second ? s_It->second.first : 0;
}

uint32_t CrashGuards::SkippedThinks() { return g_SkippedThinks; }

bool CrashGuards::IsStandInKnowledge(const void* p_Knowledge) {
    return p_Knowledge && p_Knowledge == g_Knowledge.load(std::memory_order_relaxed);
}

uint32_t CrashGuards::MutedLines() { return g_MutedLines; }

uint32_t CrashGuards::AllLines() { return g_AllLines; }

void CrashGuards::NoteCalmed(int32_t p_Wanted) {
    ++g_Calmed;

    for (int32_t s_Was = g_HighestWanted; p_Wanted > s_Was && !g_HighestWanted.compare_exchange_weak(s_Was, p_Wanted);) {}
}

uint32_t CrashGuards::Calmed() { return g_Calmed; }

namespace {
    std::mutex g_FromMutex;
    std::map<std::string, uint32_t> g_From;
}

void CrashGuards::NoteCalmedFrom(const void* const* p_Frames, int p_Count) {
    const auto s_Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string s_Chain;

    for (int i = 0; i < p_Count && s_Chain.size() < 120; ++i) {
        const auto s_At = reinterpret_cast<uintptr_t>(p_Frames[i]);

        // Only the game's own frames (the SDK's hook trampolines and this DLL are skipped).
        if (s_At > s_Base && s_At < s_Base + 0x3000000) {
            char s_Text[24];
            std::snprintf(s_Text, sizeof(s_Text), "%s%llx", s_Chain.empty() ? "" : "<", static_cast<unsigned long long>(s_At - s_Base));
            s_Chain += s_Text;
        }
    }

    std::lock_guard s_Lock(g_FromMutex);
    ++g_From[s_Chain];
}

std::string CrashGuards::CalmedFrom() {
    std::lock_guard s_Lock(g_FromMutex);
    std::vector<std::pair<uint32_t, std::string>> s_Sorted;

    for (const auto& [s_Chain, s_Count] : g_From)
        s_Sorted.emplace_back(s_Count, s_Chain);

    std::sort(s_Sorted.rbegin(), s_Sorted.rend());
    std::string s_Out;

    for (size_t i = 0; i < s_Sorted.size() && i < 6; ++i)
        s_Out += std::to_string(s_Sorted[i].first) + "x " + s_Sorted[i].second + " | ";

    return s_Out;
}

int32_t CrashGuards::HighestWanted() { return g_HighestWanted; }

int CrashGuards::Count() { return static_cast<int>(std::size(g_Patches)); }

std::vector<uintptr_t> CrashGuards::FindAll(const char* p_Pattern) {
    std::vector<uintptr_t> s_Found;
    uintptr_t s_From = 0, s_To = 0;

    if (!TextRange(s_From, s_To))
        return s_Found;

    for (uintptr_t s_At = s_From; s_Found.size() < 16;) {
        s_At = FindFirst(p_Pattern, s_At, s_To);

        if (!s_At)
            break;

        s_Found.push_back(s_At);
        ++s_At;
    }

    return s_Found;
}

std::string CrashGuards::Report() {
    std::string s_Out;

    for (const auto& s_Patch : g_Patches)
        s_Out += std::string(s_Out.empty() ? "" : ", ") + s_Patch.name + (s_Patch.applied ? " on" : " not found");

    s_Out += g_TensionOffset ? ", alert levels on" : ", alert levels not found";
    s_Out += g_Units && g_RuntimeIdOffset ? ", AI records on" : ", AI records not found";
    return s_Out;
}

int CrashGuards::Install() {
    uintptr_t s_From = 0, s_To = 0;

    if (!TextRange(s_From, s_To))
        return 0;

    if (!g_Stubs)
        g_Stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

    if (!g_Stubs)
        return 0;

    // Offsets the patches need, from the code that uses them.
    if (const uintptr_t s_Tension = FindUnique(c_SetTension, s_From, s_To)) {
        g_TensionOffset = U32(s_Tension + 7);

        if (const uintptr_t s_Id = FindUnique(c_RuntimeId, s_Tension, s_Tension + 0x180))
            g_RuntimeIdOffset = U32(s_Id + 7);
    }

    if (const uintptr_t s_Caller = FindUnique(c_UnitsCaller, s_From, s_To)) {
        const uintptr_t s_Table = RelTarget(s_Caller + 3);
        const uintptr_t s_Loop = RelTarget(s_Caller + 18);

        if (s_Loop > s_From && s_Loop + 0x100 < s_To) {
            if (const uintptr_t s_Index = FindUnique(c_UnitsIndex, s_Loop, s_Loop + 0x100)) {
                g_UnitSize = U32(s_Index + 7);
                g_Units = s_Table + *reinterpret_cast<const uint8_t*>(s_Index + 14);
            }
        }
    }

    size_t s_Offset = 0;

    for (auto& s_Patch : g_Patches) {
        if (s_Patch.applied)
            continue;

        // Each on its own: a place a build doesn't have (or has twice) leaves only that one out.
        const uintptr_t s_At = FindUnique(s_Patch.pattern, s_From, s_To);

        if (!s_At)
            continue;

        s_Patch.at = s_At;
        s_Patch.original.assign(reinterpret_cast<const uint8_t*>(s_At), reinterpret_cast<const uint8_t*>(s_At) + s_Patch.length);
        const uint64_t s_Resume = s_At + s_Patch.length;
        std::vector<uint8_t> s_Stub;

        if (s_Patch.kind == Kind::Return) {
            // The first guard's own instruction tells where the animated actor is.
            if (!g_AnimOffset && s_Patch.length == 16)
                g_AnimOffset = U32(s_At + 12);

            if (!g_AnimOffset)
                continue;

            EmitIsGuardedAndDead(s_Stub, false);
            Emit(s_Stub, { 0x75, 0x03 });       // jne: run the original
            Emit(s_Stub, { 0x31, 0xC0, 0xC3 }); // xor eax,eax; ret
            s_Stub.insert(s_Stub.end(), s_Patch.original.begin(), s_Patch.original.end());
            EmitJump(s_Stub, s_Resume);
        }
        else if (s_Patch.kind == Kind::SkipCall) {
            if (!g_AnimOffset || U32(s_At + 3) != g_AnimOffset)
                continue;

            const uint64_t s_Callee = RelTarget(s_At + 14);
            EmitIsGuardedAndDead(s_Stub, true);
            Emit(s_Stub, { 0x74, 0x19 }); // je: past the call (13 + 10 + 2 bytes)
            s_Stub.insert(s_Stub.end(), s_Patch.original.begin(), s_Patch.original.begin() + 13); // the three movs
            Emit(s_Stub, { 0x48, 0xB8 }); // mov rax,callee
            Emit64(s_Stub, s_Callee);
            Emit(s_Stub, { 0xFF, 0xD0 }); // call rax
            EmitJump(s_Stub, s_Resume);
        }
        else if (s_Patch.kind == Kind::SkipUnit) {
            if (!g_Units || !g_UnitSize || !g_RuntimeIdOffset)
                continue;

            Emit(s_Stub, { 0x48, 0xB8 }); // mov rax,&g_SkipList
            Emit64(s_Stub, reinterpret_cast<uint64_t>(&g_SkipList));
            Emit(s_Stub, { 0x48, 0x8B, 0x00 });             // mov rax,[rax]: the list
            // next:
            Emit(s_Stub, { 0x48, 0x83, 0x38, 0x00 });       // cmp qword ptr [rax],0
            Emit(s_Stub, { 0x74, 0x19 });                   // je original (3 + 2 + 4 + 2 + 14 bytes on)
            Emit(s_Stub, { 0x48, 0x39, 0x08 });             // cmp [rax],rcx
            Emit(s_Stub, { 0x74, 0x06 });                   // je skip (4 + 2 bytes on)
            Emit(s_Stub, { 0x48, 0x83, 0xC0, 0x08 });       // add rax,8
            Emit(s_Stub, { 0xEB, 0xEF });                   // jmp next (back 17 bytes)
            // skip:
            Emit(s_Stub, { 0x48, 0xB8 });       // mov rax,&g_SkippedThinks
            Emit64(s_Stub, reinterpret_cast<uint64_t>(&g_SkippedThinks));
            Emit(s_Stub, { 0xF0, 0xFF, 0x00 }); // lock inc dword ptr [rax]
            Emit(s_Stub, { 0xC3 });             // ret
            // original:
            s_Stub.insert(s_Stub.end(), s_Patch.original.begin(), s_Patch.original.end());
            EmitJump(s_Stub, s_Resume);
        }
        else if (s_Patch.kind == Kind::NullGuard) {
            const uint64_t s_Next = RelTarget(s_At + 33);
            const auto& o = s_Patch.original;
            s_Stub.insert(s_Stub.end(), o.begin(), o.begin() + 4);       // cmp byte ptr [rcx+..],1
            Emit(s_Stub, { 0x75, 0x2D });                                // jne ret (16+3+2+6+4+14 bytes on)
            s_Stub.insert(s_Stub.end(), o.begin() + 6, o.begin() + 22);  // lea; mov; call
            Emit(s_Stub, { 0x48, 0x85, 0xC0 });                          // test rax,rax
            Emit(s_Stub, { 0x74, 0x18 });                                // je ret (6+4+14 bytes on)
            s_Stub.insert(s_Stub.end(), o.begin() + 22, o.begin() + 32); // xor edx,edx; mov rcx,[rax+..]; add rsp,28h
            EmitJump(s_Stub, s_Next);
            // ret:
            Emit(s_Stub, { 0x48, 0x83, 0xC4, 0x28, 0xC3 });              // add rsp,28h; ret
        }
        else {
            g_ControllerOffset = U32(s_At + 3);
            g_NoVoicePath = RelTarget(s_At + 22);
            // mov rax,[rcx-<speaker>] right after: back from the interface to the ZActor.
            g_SpeakerOffset = static_cast<uint32_t>(-I32(s_At + 29));

            if (g_SpeakerOffset == 0 || g_SpeakerOffset > 0x2000 || g_NoVoicePath < s_From || g_NoVoicePath >= s_To)
                continue;

            if (void* s_Actor = g_Actor)
                g_Speaker = static_cast<uint8_t*>(s_Actor) + g_SpeakerOffset;

            EmitMuteSpeech(s_Stub, s_Resume);
        }

        if (s_Offset + s_Stub.size() > 4096)
            break;

        std::memcpy(g_Stubs + s_Offset, s_Stub.data(), s_Stub.size());

        std::vector<uint8_t> s_Jump;
        EmitJump(s_Jump, reinterpret_cast<uint64_t>(g_Stubs + s_Offset));

        while (s_Jump.size() < s_Patch.length)
            s_Jump.push_back(0x90);

        FlushInstructionCache(GetCurrentProcess(), g_Stubs + s_Offset, s_Stub.size());

        if (Write(s_At, s_Jump)) {
            s_Patch.applied = true;
            ++g_Applied;
        }

        s_Offset += (s_Stub.size() + 15) & ~size_t(15);
    }

    return g_Applied;
}

void CrashGuards::Uninstall() {
    for (auto& s_Patch : g_Patches) {
        if (s_Patch.applied && Write(s_Patch.at, s_Patch.original)) {
            s_Patch.applied = false;
            --g_Applied;
        }
    }

    // The stubs stay allocated: another thread may still be running through one.
}
