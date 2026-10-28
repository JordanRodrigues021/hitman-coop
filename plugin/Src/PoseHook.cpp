#include "PoseHook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {
// The pose update entry's arguments: (this, input pose, output pose, world matrix, then up to
// three more that are passed through untouched).
using UpdateFn = void (*)(void*, void*, uint8_t*, const void*, uint64_t, uint64_t, uint64_t);

constexpr size_t c_UpdateSlot = 1;

std::mutex g_Mutex;
void** g_Table = nullptr;
UpdateFn g_Original = nullptr;

constexpr size_t c_Channels = 2;
constexpr size_t c_Extras = 32; // the partner's victim and their NPCs near us (puppets)
constexpr size_t c_Watched = 16; // our NPCs near the partner, whose poses we send

std::atomic<void*> g_Sources[c_Channels] {};
std::atomic<void*> g_Target { nullptr };
std::atomic<void*> g_Extras[c_Extras] {};
std::vector<PoseHook::Bone> g_ExtraPoses[c_Extras];
std::vector<PoseHook::Bone> g_ExtraNative[c_Extras]; // the target's own first bones, before ours
uint32_t g_ExtraNativeCount[c_Extras] {};
std::atomic<void*> g_Watched[c_Watched] {};
std::vector<PoseHook::Bone> g_WatchedPoses[c_Watched];
bool g_WatchedFresh[c_Watched] {};
std::atomic<uint64_t> g_Calls { 0 };
std::atomic<uint64_t> g_TargetCalls { 0 }; // the target's own pose updates (is it still animating?)
std::atomic<uint32_t> g_SourceCount { 0 };
std::atomic<uint32_t> g_TargetCount { 0 };

std::vector<PoseHook::Bone> g_SourcePoses[c_Channels];
bool g_SourceFresh[c_Channels] {};
std::vector<PoseHook::Bone> g_TargetPose;
bool g_HasTargetPose = false;
bool g_AllPositions = false;
std::vector<PoseHook::Bone> g_TargetNative; // the target's own pose, before ours
std::vector<uint8_t> g_TargetMask;          // bones of ours to use (empty: all)
std::vector<int32_t> g_TargetMap; // per target bone, the source bone it copies (-1: its own; empty: same index)

// Writes p_Rot over p_Bone's rotation, on the same side of the 4D sphere as the rotation already
// there. q and -q are the same rotation, but the engine blends with the bone's own value, and our
// sign-normalised copies (smallest-three packing) flipped an NPC upside down.
void WriteRotation(PoseHook::Bone& p_Bone, const float* p_Rot) {
    const float s_Dot = p_Bone.rot[0] * p_Rot[0] + p_Bone.rot[1] * p_Rot[1] + p_Bone.rot[2] * p_Rot[2] + p_Bone.rot[3] * p_Rot[3];
    const float s_Sign = s_Dot < 0.f ? -1.f : 1.f;

    for (int i = 0; i < 4; ++i)
        p_Bone.rot[i] = p_Rot[i] * s_Sign;
}

bool ReadablePose(const uint8_t* p_Out, PoseHook::Bone*& p_Bones, uint32_t& p_Count) {
    if (!p_Out)
        return false;

    std::memcpy(&p_Bones, p_Out + 0x08, sizeof(p_Bones));
    std::memcpy(&p_Count, p_Out + 0x10, sizeof(p_Count));
    return p_Bones && p_Count > 0 && p_Count < 1024;
}

void Update(void* p_This, void* p_In, uint8_t* p_Out, const void* p_World, uint64_t p_A5, uint64_t p_A6, uint64_t p_A7) {
    g_Original(p_This, p_In, p_Out, p_World, p_A5, p_A6, p_A7);
    g_Calls.fetch_add(1, std::memory_order_relaxed);

    int s_Source = -1;
    int s_Extra = -1;

    for (size_t i = 0; i < c_Channels; ++i)
        if (p_This == g_Sources[i].load(std::memory_order_relaxed))
            s_Source = static_cast<int>(i);

    for (size_t i = 0; i < c_Extras; ++i)
        if (p_This == g_Extras[i].load(std::memory_order_relaxed))
            s_Extra = static_cast<int>(i);

    const bool s_IsTarget = p_This == g_Target.load(std::memory_order_relaxed);
    int s_Watched = -1;

    for (size_t i = 0; i < c_Watched; ++i)
        if (p_This == g_Watched[i].load(std::memory_order_relaxed))
            s_Watched = static_cast<int>(i);

    if (s_Source < 0 && s_Extra < 0 && !s_IsTarget && s_Watched < 0)
        return;

    PoseHook::Bone* s_Bones = nullptr;
    uint32_t s_Count = 0;

    if (!ReadablePose(p_Out, s_Bones, s_Count))
        return;

    std::lock_guard s_Lock(g_Mutex);

    if (s_Watched >= 0) {
        g_WatchedPoses[s_Watched].assign(s_Bones, s_Bones + s_Count);
        g_WatchedFresh[s_Watched] = true;
    }

    if (s_Source >= 0) {
        if (s_Source == 0)
            g_SourceCount = s_Count;

        g_SourcePoses[s_Source].assign(s_Bones, s_Bones + s_Count);
        g_SourceFresh[s_Source] = true;
    }

    if (s_Extra >= 0 && p_This == g_Extras[s_Extra].load(std::memory_order_relaxed)) {
        const auto& s_Pose = g_ExtraPoses[s_Extra];
        const uint32_t s_Copy = std::min<uint32_t>(s_Count, static_cast<uint32_t>(s_Pose.size()));
        g_ExtraNative[s_Extra].assign(s_Bones, s_Bones + s_Count);
        g_ExtraNativeCount[s_Extra] = s_Count;

        for (uint32_t i = 0; i < s_Copy; ++i) {
            WriteRotation(s_Bones[i], s_Pose[i].rot);

            if (i < 2)
                std::memcpy(s_Bones[i].pos, s_Pose[i].pos, sizeof(s_Bones[i].pos));
        }
    }

    if (s_IsTarget) {
        g_TargetCalls.fetch_add(1, std::memory_order_relaxed);
        g_TargetCount = s_Count;
        g_TargetNative.assign(s_Bones, s_Bones + s_Count);

        if (!g_HasTargetPose)
            return;

        const uint32_t s_Copy = g_TargetMap.empty() ? std::min<uint32_t>(s_Count, static_cast<uint32_t>(g_TargetPose.size()))
                                                    : std::min<uint32_t>(s_Count, static_cast<uint32_t>(g_TargetMap.size()));

        for (uint32_t i = 0; i < s_Copy; ++i) {
            if (i < g_TargetMask.size() && !g_TargetMask[i])
                continue;

            const int32_t s_From = g_TargetMap.empty() ? static_cast<int32_t>(i) : g_TargetMap[i];

            if (s_From < 0 || static_cast<size_t>(s_From) >= g_TargetPose.size())
                continue;

            const PoseHook::Bone& s_Source = g_TargetPose[s_From];

            WriteRotation(s_Bones[i], s_Source.rot);

            if (g_AllPositions || i < 2)
                std::memcpy(s_Bones[i].pos, s_Source.pos, sizeof(s_Bones[i].pos));
        }
    }
}

bool Patch(void** p_Slot, void* p_Value) {
    DWORD s_Old = 0;

    if (!VirtualProtect(p_Slot, sizeof(void*), PAGE_READWRITE, &s_Old))
        return false;

    InterlockedExchangePointer(p_Slot, p_Value);
    VirtualProtect(p_Slot, sizeof(void*), s_Old, &s_Old);
    return true;
}
}

bool PoseHook::Install(void* p_AnyMorphemeAnimator) {
    std::lock_guard s_Lock(g_Mutex);

    if (g_Table)
        return true;

    if (!p_AnyMorphemeAnimator)
        return false;

    void** s_Table = *static_cast<void***>(p_AnyMorphemeAnimator);

    if (!s_Table || s_Table[c_UpdateSlot] == reinterpret_cast<void*>(&Update))
        return false;

    g_Original = reinterpret_cast<UpdateFn>(s_Table[c_UpdateSlot]);

    if (!Patch(&s_Table[c_UpdateSlot], reinterpret_cast<void*>(&Update)))
        return false;

    g_Table = s_Table;
    return true;
}

void PoseHook::Uninstall() {
    std::lock_guard s_Lock(g_Mutex);

    if (g_Table && g_Original)
        Patch(&g_Table[c_UpdateSlot], reinterpret_cast<void*>(g_Original));

    g_Table = nullptr;

    for (auto& s_Source : g_Sources)
        s_Source = nullptr;

    for (auto& s_Extra : g_Extras)
        s_Extra = nullptr;

    for (auto& s_Watched : g_Watched)
        s_Watched = nullptr;

    g_Target = nullptr;
    g_HasTargetPose = false;
}

bool PoseHook::Installed() {
    return g_Table != nullptr;
}

void PoseHook::SetSource(void* p_Animator, size_t p_Channel) {
    if (p_Channel < c_Channels)
        g_Sources[p_Channel] = p_Animator;
}

void PoseHook::SetTarget(void* p_Animator) {
    g_Target = p_Animator;
}

bool PoseHook::TakeSourcePose(std::vector<Bone>& p_Out, size_t p_Channel) {
    std::lock_guard s_Lock(g_Mutex);

    if (p_Channel >= c_Channels || !g_SourceFresh[p_Channel])
        return false;

    p_Out = g_SourcePoses[p_Channel];
    g_SourceFresh[p_Channel] = false;
    return true;
}

bool PoseHook::SetPoseFor(void* p_Animator, const std::vector<Bone>& p_Pose) {
    if (!p_Animator)
        return false;

    std::lock_guard s_Lock(g_Mutex);
    int s_Free = -1;

    for (size_t i = 0; i < c_Extras; ++i) {
        if (g_Extras[i] == p_Animator) {
            g_ExtraPoses[i] = p_Pose;
            return true;
        }

        if (!g_Extras[i] && s_Free < 0)
            s_Free = static_cast<int>(i);
    }

    if (s_Free < 0)
        return false;

    g_ExtraPoses[s_Free] = p_Pose;
    g_Extras[s_Free] = p_Animator;
    return true;
}

void PoseHook::ClearPoseFor(void* p_Animator) {
    std::lock_guard s_Lock(g_Mutex);

    for (auto& s_Extra : g_Extras)
        if (s_Extra == p_Animator)
            s_Extra = nullptr;
}

void PoseHook::SetWatched(const std::vector<void*>& p_Animators) {
    std::lock_guard s_Lock(g_Mutex);

    for (size_t i = 0; i < c_Watched; ++i) {
        void* s_Want = i < p_Animators.size() ? p_Animators[i] : nullptr;

        if (g_Watched[i] != s_Want) {
            g_Watched[i] = s_Want;
            g_WatchedFresh[i] = false;
        }
    }
}

bool PoseHook::WatchedPose(void* p_Animator, std::vector<Bone>& p_Out) {
    std::lock_guard s_Lock(g_Mutex);

    for (size_t i = 0; i < c_Watched; ++i) {
        if (p_Animator && g_Watched[i] == p_Animator && g_WatchedFresh[i]) {
            p_Out = g_WatchedPoses[i];
            return !p_Out.empty();
        }
    }

    return false;
}

void PoseHook::ClearExtraPoses() {
    std::lock_guard s_Lock(g_Mutex);

    for (auto& s_Extra : g_Extras)
        s_Extra = nullptr;
}

void PoseHook::SetTargetPose(const std::vector<Bone>& p_Pose, bool p_AllPositions) {
    std::lock_guard s_Lock(g_Mutex);
    g_TargetPose = p_Pose;
    g_AllPositions = p_AllPositions;
    g_HasTargetPose = !p_Pose.empty();
}

bool PoseHook::TargetNativePose(std::vector<Bone>& p_Out) {
    std::lock_guard s_Lock(g_Mutex);
    p_Out = g_TargetNative;
    return !p_Out.empty();
}

void PoseHook::SetTargetMap(const std::vector<int32_t>& p_Map) {
    std::lock_guard s_Lock(g_Mutex);
    g_TargetMap = p_Map;
}

void PoseHook::SetTargetMask(const std::vector<uint8_t>& p_Mask) {
    std::lock_guard s_Lock(g_Mutex);
    g_TargetMask = p_Mask;
}

void PoseHook::ClearTargetPose() {
    std::lock_guard s_Lock(g_Mutex);
    g_HasTargetPose = false;
}

uint32_t PoseHook::SourceBoneCount() {
    return g_SourceCount;
}

uint32_t PoseHook::TargetBoneCount() {
    return g_TargetCount;
}

uint64_t PoseHook::Calls() {
    return g_Calls;
}

uint64_t PoseHook::TargetCalls() {
    return g_TargetCalls;
}

uint32_t PoseHook::NativePoseOf(void* p_Animator, std::vector<Bone>& p_Out) {
    std::lock_guard s_Lock(g_Mutex);

    for (size_t i = 0; i < c_Extras; ++i) {
        if (g_Extras[i] == p_Animator) {
            p_Out = g_ExtraNative[i];
            return g_ExtraNativeCount[i];
        }
    }

    return 0;
}
