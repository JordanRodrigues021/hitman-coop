#include "RigScan.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <set>

namespace {
// Only plainly readable, committed pages. Reading a guard page this way silently clears its
// guard, and the game crashed later (in code that walks an actor's entity list).
bool Readable(uintptr_t p_At, size_t p_Size) {
    for (uintptr_t s_Page = p_At & ~uintptr_t(0xFFF); s_Page < p_At + p_Size; s_Page += 0x1000) {
        MEMORY_BASIC_INFORMATION s_Info {};

        if (!VirtualQuery(reinterpret_cast<const void*>(s_Page), &s_Info, sizeof(s_Info)) || s_Info.State != MEM_COMMIT ||
            (s_Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(s_Info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            return false;
    }

    return true;
}

bool Read(uintptr_t p_At, void* p_Out, size_t p_Size) {
    if (!Readable(p_At, p_Size))
        return false;

    std::memcpy(p_Out, reinterpret_cast<const void*>(p_At), p_Size);
    return true;
}

bool LooksLikePointer(uint64_t p_Value) {
    return p_Value > 0x10000 && p_Value < 0x00007FFFFFFFFFFFull && (p_Value & 7) == 0;
}

template <class T>
bool IsParentTable(const std::vector<uint8_t>& p_Data, size_t p_Bones, std::vector<int32_t>& p_Out) {
    if (p_Data.size() < p_Bones * sizeof(T))
        return false;

    const T* s_Table = reinterpret_cast<const T*>(p_Data.data());

    if (static_cast<int64_t>(s_Table[0]) != -1)
        return false;

    p_Out.assign(p_Bones, -1);
    int s_Distinct = 0;

    for (size_t i = 1; i < p_Bones; ++i) {
        const int64_t s_Parent = static_cast<int64_t>(s_Table[i]);

        if (s_Parent < 0 || s_Parent >= static_cast<int64_t>(i))
            return false;

        p_Out[i] = static_cast<int32_t>(s_Parent);
        s_Distinct += s_Parent != static_cast<int64_t>(i) - 1;
    }

    // A plain 0,1,2,3... chain says nothing; a skeleton branches.
    return s_Distinct > 10;
}
}

std::vector<RigScan::Found> RigScan::FindParents(const void* p_Animator, size_t p_Bones, int p_Depth) {
    std::vector<Found> s_Found;

    // Where it is in 3.280.0 (animator +0x50, then +0x70, int32): checked, then used without a search.
    {
        uint64_t s_Rig = 0, s_Table = 0;
        std::vector<uint8_t> s_Data(p_Bones * 4);
        Found s_Hit;

        if (Read(reinterpret_cast<uintptr_t>(p_Animator) + 0x50, &s_Rig, 8) && LooksLikePointer(s_Rig) && Read(s_Rig + 0x70, &s_Table, 8) &&
            LooksLikePointer(s_Table) && Read(s_Table, s_Data.data(), s_Data.size()) && IsParentTable<int32_t>(s_Data, p_Bones, s_Hit.parents)) {
            s_Hit.path = "+0x50 > +0x70";
            s_Hit.width = 4;
            s_Found.push_back(std::move(s_Hit));
            return s_Found;
        }
    }

    std::set<uintptr_t> s_Seen;
    constexpr size_t c_Span = 0x300; // bytes of each object looked through for pointers

    struct Node {
        uintptr_t at;
        std::string path;
        int depth;
    };

    std::vector<Node> s_Queue { { reinterpret_cast<uintptr_t>(p_Animator), "", 0 } };

    for (size_t q = 0; q < s_Queue.size() && q < 20000 && s_Found.size() < 8; ++q) {
        const Node s_Node = s_Queue[q];

        if (!s_Seen.insert(s_Node.at).second)
            continue;

        // The object itself as a table (int16 or int32).
        std::vector<uint8_t> s_Data(p_Bones * 4);

        if (Read(s_Node.at, s_Data.data(), s_Data.size())) {
            Found s_Hit;

            if (IsParentTable<int16_t>(s_Data, p_Bones, s_Hit.parents))
                s_Hit.width = 2;
            else if (IsParentTable<int32_t>(s_Data, p_Bones, s_Hit.parents))
                s_Hit.width = 4;

            if (s_Hit.width) {
                s_Hit.path = s_Node.path.empty() ? "animator" : s_Node.path;
                s_Found.push_back(std::move(s_Hit));
                continue;
            }
        }

        if (s_Node.depth >= p_Depth)
            continue;

        std::vector<uint64_t> s_Words(c_Span / 8);

        if (!Read(s_Node.at, s_Words.data(), c_Span))
            continue;

        for (size_t i = 0; i < s_Words.size(); ++i) {
            if (!LooksLikePointer(s_Words[i]))
                continue;

            char s_Step[24];
            std::snprintf(s_Step, sizeof(s_Step), "%s+0x%zx", s_Node.path.empty() ? "" : " > ", i * 8);
            s_Queue.push_back({ static_cast<uintptr_t>(s_Words[i]), s_Node.path + s_Step, s_Node.depth + 1 });
        }
    }

    return s_Found;
}

std::string RigScan::Words(const void* p_At, size_t p_Size) {
    std::string s_Out;
    char s_Line[48];

    for (size_t i = 0; i + 8 <= p_Size; i += 8) {
        uint64_t s_Word = 0;

        if (!Read(reinterpret_cast<uintptr_t>(p_At) + i, &s_Word, sizeof(s_Word)))
            std::snprintf(s_Line, sizeof(s_Line), " +0x%zx ?", i);
        else
            std::snprintf(s_Line, sizeof(s_Line), " +0x%zx %016llx", i, static_cast<unsigned long long>(s_Word));

        s_Out += s_Line;
    }

    return s_Out;
}
