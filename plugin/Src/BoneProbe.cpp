#include "BoneProbe.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include <Glacier/ZHM5BaseCharacter.h>
#include <Glacier/ZMorpheme.h>
#include <Glacier/ZPhysics.h>

namespace {
bool SafeRead(const void* p_Address, void* p_Out, size_t p_Size) {
    SIZE_T s_Read = 0;
    return p_Address && ReadProcessMemory(GetCurrentProcess(), p_Address, p_Out, p_Size, &s_Read) && s_Read == p_Size;
}

void HexDump(std::string& p_Out, const char* p_Label, const void* p_Address, size_t p_Bytes) {
    std::vector<uint8_t> s_Data(p_Bytes);
    char s_Line[256];

    if (!SafeRead(p_Address, s_Data.data(), p_Bytes)) {
        std::snprintf(s_Line, sizeof(s_Line), "%s @ %p: unreadable\n", p_Label, p_Address);
        p_Out += s_Line;
        return;
    }

    std::snprintf(s_Line, sizeof(s_Line), "%s @ %p (%zu bytes)\n", p_Label, p_Address, p_Bytes);
    p_Out += s_Line;

    for (size_t i = 0; i < p_Bytes; i += 16) {
        float s_F[4];
        uint64_t s_Q[2];
        std::memcpy(s_F, &s_Data[i], 16);
        std::memcpy(s_Q, &s_Data[i], 16);
        std::snprintf(s_Line, sizeof(s_Line), "  +%04zx  %016llx %016llx  | %10.4f %10.4f %10.4f %10.4f\n", i,
            static_cast<unsigned long long>(s_Q[0]), static_cast<unsigned long long>(s_Q[1]), s_F[0], s_F[1], s_F[2], s_F[3]);
        p_Out += s_Line;
    }
}

bool LooksLikeHeap(uint64_t p_Value) {
    return p_Value > 0x10000000ull && p_Value < 0x7ff000000000ull && (p_Value & 7) == 0;
}
}

std::string BoneProbe::AnimatorTable(const char* p_Label, ZEntityRef p_Entity) {
    char s_Line[256];
    std::string s_Out = std::string("=== ") + p_Label + "\n";
    auto* s_Animator = p_Entity.QueryInterface<IBoneAnimator>();
    const auto s_Exe = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
    void* s_Root = p_Entity.QueryInterface<void>();
    std::snprintf(s_Line, sizeof(s_Line), "entity %p, IBoneAnimator at %p (+0x%llx)\n", s_Root, static_cast<void*>(s_Animator),
        s_Animator && s_Root ? static_cast<unsigned long long>(reinterpret_cast<uint64_t>(s_Animator) - reinterpret_cast<uint64_t>(s_Root)) : 0ull);
    s_Out += s_Line;

    uint64_t s_Table = 0;

    if (!s_Animator || !SafeRead(s_Animator, &s_Table, 8))
        return s_Out;

    uint64_t s_Entries[8] {};
    SafeRead(reinterpret_cast<const void*>(s_Table), s_Entries, sizeof(s_Entries));

    for (int i = 0; i < 8; ++i) {
        std::snprintf(s_Line, sizeof(s_Line), "  [%d] HITMAN3.exe+0x%llx\n", i, static_cast<unsigned long long>(s_Entries[i] - s_Exe));
        s_Out += s_Line;
    }

    return s_Out;
}

std::string BoneProbe::Dump(const char* p_Label, ZHM5BaseCharacter* p_Character, size_t p_Bytes) {
    std::string s_Out = std::string("=== ") + p_Label + "\n";

    if (!p_Character)
        return s_Out + "no character\n";

    auto* s_Morpheme = p_Character->m_pMorphemeEntity.m_entityRef.QueryInterface<ZMorphemeEntity>();
    char s_Line[256];
    std::snprintf(s_Line, sizeof(s_Line), "character %p morpheme entity %p\n", static_cast<void*>(p_Character),
        static_cast<void*>(s_Morpheme));
    s_Out += s_Line;

    if (!s_Morpheme)
        return s_Out;

    HexDump(s_Out, "morpheme entity", s_Morpheme, 0x160);

    ZAnimationBoneData* s_BoneData = nullptr;
    SafeRead(&s_Morpheme->m_BoneAnimatorInfo.m_pBoneData, &s_BoneData, sizeof(s_BoneData));
    HexDump(s_Out, "bone data", s_BoneData, p_Bytes);

    // One level down: whatever the bone data points to.
    std::vector<uint64_t> s_Words(p_Bytes / 8);

    if (SafeRead(s_BoneData, s_Words.data(), s_Words.size() * 8)) {
        int s_Followed = 0;

        for (size_t i = 0; i < s_Words.size() && s_Followed < 8; ++i) {
            if (!LooksLikeHeap(s_Words[i]))
                continue;

            std::snprintf(s_Line, sizeof(s_Line), "bone data +%04zx ->", i * 8);
            HexDump(s_Out, s_Line, reinterpret_cast<const void*>(s_Words[i]), 0x100);
            ++s_Followed;
        }
    }

    return s_Out;
}
