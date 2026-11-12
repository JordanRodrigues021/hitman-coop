#include "PoseHook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include <DirectXMath.h>

namespace {
// The pose update entry's arguments: (this, input pose, output pose, world matrix, then up to
// three more that are passed through untouched).
using UpdateFn = void (*)(void*, void*, uint8_t*, const void*, uint64_t, uint64_t, uint64_t);

constexpr size_t c_UpdateSlot = 1;

std::mutex g_Mutex;
void** g_Table = nullptr;
UpdateFn g_Original = nullptr;

constexpr size_t c_Channels = 2;
constexpr size_t c_Extras = 64;  // the partner's victim and their NPCs near us (puppets)
constexpr size_t c_Watched = 40; // our NPCs near the partner, whose poses we send (Session's npcPoseMax and some)

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

// World pins (see PoseHook.h): where each pinned character's skeleton must be, sorted by animator.
std::shared_mutex g_PinMutex;
std::vector<PoseHook::Pin> g_Pins;
std::atomic<uint32_t> g_PinCount { 0 };
std::atomic<bool> g_PinsOn { true };
std::atomic<int> g_PinLayout { 0 }; // the world matrix argument: 0 not known yet, 1: 4x4 floats, 2: 4x3 floats
std::atomic<uint32_t> g_PinCorrected { 0 }, g_PinAligned { 0 }, g_PinUnusable { 0 };
std::atomic<uint32_t> g_PinMaxAngle { 0 }; // hundredths of a degree, since the stats were last read
std::atomic<uint32_t> g_PinAngles[5] {};     // corrections by angle: <10, 10-45, 45-80, 80-100, 100+ degrees

// Writes p_Rot over p_Bone's rotation, on the same side of the 4D sphere as the rotation already
// there. q and -q are the same rotation, but the engine blends with the bone's own value, and our
// sign-normalised copies (smallest-three packing) flipped an NPC upside down.
void WriteRotation(PoseHook::Bone& p_Bone, const float* p_Rot) {
    const float s_Dot = p_Bone.rot[0] * p_Rot[0] + p_Bone.rot[1] * p_Rot[1] + p_Bone.rot[2] * p_Rot[2] + p_Bone.rot[3] * p_Rot[3];
    const float s_Sign = s_Dot < 0.f ? -1.f : 1.f;

    for (int i = 0; i < 4; ++i)
        p_Bone.rot[i] = p_Rot[i] * s_Sign;
}

bool FindPin(void* p_Animator, PoseHook::Pin& p_Out) {
    std::shared_lock s_Lock(g_PinMutex);
    const auto s_It = std::lower_bound(g_Pins.begin(), g_Pins.end(), p_Animator,
        [](const PoseHook::Pin& p_Pin, void* p_Key) { return p_Pin.animator < p_Key; });

    if (s_It == g_Pins.end() || s_It->animator != p_Animator)
        return false;

    p_Out = *s_It;
    return true;
}

// The first 16 floats at the world matrix argument, under SEH (it is the engine's pointer).
bool PeekWorld(const void* p_World, float* p_Out) noexcept {
    __try {
        std::memcpy(p_Out, p_World, 16 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Three unit axes at right angles and a finite position: a character's world matrix.
bool Rigid(const float p_Rows[4][3]) {
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(p_Rows[i][k]) || std::fabs(p_Rows[i][k]) > 1e6f)
                return false;

    const auto s_Dot = [&](int a, int b) { return p_Rows[a][0] * p_Rows[b][0] + p_Rows[a][1] * p_Rows[b][1] + p_Rows[a][2] * p_Rows[b][2]; };

    for (int i = 0; i < 3; ++i)
        if (std::fabs(s_Dot(i, i) - 1.f) > 0.05f)
            return false;

    return std::fabs(s_Dot(0, 1)) < 0.05f && std::fabs(s_Dot(0, 2)) < 0.05f && std::fabs(s_Dot(1, 2)) < 0.05f;
}

// The world matrix argument as rows (x, y, z axes, position), in the layout it has. Which layout
// that is is found from the data itself, once, and kept: only a matrix that is rigid and within a
// few metres of where the character was put counts, so a build that passes something else here
// never moves a bone.
bool WorldRows(const void* p_World, const PoseHook::Pin& p_Pin, float p_Rows[4][3]) {
    float s_Raw[16];

    if (!p_World || !PeekWorld(p_World, s_Raw))
        return false;

    const auto s_Try = [&](int p_Layout) {
        const int s_Stride = p_Layout == 1 ? 4 : 3;

        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 3; ++k)
                p_Rows[i][k] = s_Raw[i * s_Stride + k];

        if (p_Layout == 1 && (std::fabs(s_Raw[3]) > 0.01f || std::fabs(s_Raw[7]) > 0.01f || std::fabs(s_Raw[11]) > 0.01f))
            return false;

        const float s_X = p_Rows[3][0] - p_Pin.pos[0], s_Y = p_Rows[3][1] - p_Pin.pos[1], s_Z = p_Rows[3][2] - p_Pin.pos[2];
        return Rigid(p_Rows) && s_X * s_X + s_Y * s_Y + s_Z * s_Z < 36.f;
    };

    if (const int s_Known = g_PinLayout.load(std::memory_order_relaxed))
        return s_Try(s_Known);

    for (const int s_Layout : { 1, 2 }) {
        if (s_Try(s_Layout)) {
            g_PinLayout.store(s_Layout, std::memory_order_relaxed);
            return true;
        }
    }

    return false;
}

// Moves the root bone so the skeleton stands where the pin says, whatever the character's own
// transform is this frame: root' = root * wanted * actual^-1 (row vectors, as the engine's).
void ApplyPin(PoseHook::Bone& p_Root, const PoseHook::Pin& p_Pin, const void* p_World) {
    using namespace DirectX;
    float s_Rows[4][3];

    if (!WorldRows(p_World, p_Pin, s_Rows)) {
        g_PinUnusable.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const XMMATRIX s_Actual = XMMatrixSet(s_Rows[0][0], s_Rows[0][1], s_Rows[0][2], 0.f, s_Rows[1][0], s_Rows[1][1], s_Rows[1][2], 0.f,
        s_Rows[2][0], s_Rows[2][1], s_Rows[2][2], 0.f, s_Rows[3][0], s_Rows[3][1], s_Rows[3][2], 1.f);
    XMMATRIX s_Wanted = XMMatrixRotationQuaternion(XMQuaternionNormalize(XMVectorSet(p_Pin.rot[0], p_Pin.rot[1], p_Pin.rot[2], p_Pin.rot[3])));
    s_Wanted.r[3] = XMVectorSet(p_Pin.pos[0], p_Pin.pos[1], p_Pin.pos[2], 1.f);
    XMVECTOR s_Det;
    const XMMATRIX s_Fix = XMMatrixMultiply(s_Wanted, XMMatrixInverse(&s_Det, s_Actual));

    // How far off the character's own transform is: nothing to do when it is where it should be.
    const XMVECTOR s_FixRot = XMQuaternionNormalize(XMQuaternionRotationMatrix(s_Fix));
    const float s_Angle = 2.f * std::acos(std::min(1.f, std::fabs(XMVectorGetW(s_FixRot))));
    const float s_Off = XMVectorGetX(XMVector3Length(s_Fix.r[3]));

    if (!std::isfinite(s_Angle) || !std::isfinite(s_Off))
        return;

    if (s_Angle < 0.004f && s_Off < 0.004f) {
        g_PinAligned.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    XMMATRIX s_Bone = XMMatrixRotationQuaternion(XMQuaternionNormalize(XMVectorSet(p_Root.rot[0], p_Root.rot[1], p_Root.rot[2], p_Root.rot[3])));
    s_Bone.r[3] = XMVectorSet(p_Root.pos[0], p_Root.pos[1], p_Root.pos[2], 1.f);
    const XMMATRIX s_New = XMMatrixMultiply(s_Bone, s_Fix);
    XMFLOAT4 s_Rot;
    XMStoreFloat4(&s_Rot, XMQuaternionNormalize(XMQuaternionRotationMatrix(s_New)));
    const float s_Q[4] = { s_Rot.x, s_Rot.y, s_Rot.z, s_Rot.w };
    WriteRotation(p_Root, s_Q);
    p_Root.pos[0] = XMVectorGetX(s_New.r[3]);
    p_Root.pos[1] = XMVectorGetY(s_New.r[3]);
    p_Root.pos[2] = XMVectorGetZ(s_New.r[3]);
    g_PinCorrected.fetch_add(1, std::memory_order_relaxed);

    const float s_Degrees = s_Angle * 57.29578f;
    g_PinAngles[s_Degrees < 10.f ? 0 : s_Degrees < 45.f ? 1 : s_Degrees < 80.f ? 2 : s_Degrees < 100.f ? 3 : 4].fetch_add(1, std::memory_order_relaxed);
    const uint32_t s_Hundredths = static_cast<uint32_t>(s_Angle * 5729.578f);
    uint32_t s_Max = g_PinMaxAngle.load(std::memory_order_relaxed);

    while (s_Hundredths > s_Max && !g_PinMaxAngle.compare_exchange_weak(s_Max, s_Hundredths, std::memory_order_relaxed)) {}
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

    PoseHook::Pin s_Pin;
    const bool s_Pinned = !s_IsTarget && g_PinCount.load(std::memory_order_relaxed) && g_PinsOn.load(std::memory_order_relaxed) &&
        FindPin(p_This, s_Pin);

    if (s_Source < 0 && s_Extra < 0 && !s_IsTarget && s_Watched < 0 && !s_Pinned)
        return;

    PoseHook::Bone* s_Bones = nullptr;
    uint32_t s_Count = 0;

    if (!ReadablePose(p_Out, s_Bones, s_Count))
        return;

    // Pinned and nothing else: its own animation, stood where the pin says. No shared state.
    if (s_Source < 0 && s_Extra < 0 && s_Watched < 0) {
        ApplyPin(s_Bones[0], s_Pin, p_World);
        return;
    }

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

    // After the partner's pose is in: the whole skeleton as it stands in the partner's game.
    if (s_Pinned)
        ApplyPin(s_Bones[0], s_Pin, p_World);

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

    // Each keeps its slot, and the pose recorded there, while it stays watched. The list comes
    // sorted by distance, and slots were given out in that order: every time two NPCs swapped
    // places, all after them moved slot, their poses were dropped as stale and not sent that round
    // (an NPC walking towards the partner kept losing its pose, and walked on its own legs there).
    for (size_t i = 0; i < c_Watched; ++i) {
        if (g_Watched[i] && std::find(p_Animators.begin(), p_Animators.end(), g_Watched[i].load()) == p_Animators.end()) {
            g_Watched[i] = nullptr;
            g_WatchedFresh[i] = false;
        }
    }

    for (void* s_Want : p_Animators) {
        if (!s_Want)
            continue;

        int s_Free = -1;
        bool s_Have = false;

        for (size_t i = 0; i < c_Watched && !s_Have; ++i) {
            if (g_Watched[i] == s_Want)
                s_Have = true;
            else if (!g_Watched[i] && s_Free < 0)
                s_Free = static_cast<int>(i);
        }

        if (!s_Have && s_Free >= 0) {
            g_WatchedFresh[s_Free] = false;
            g_Watched[s_Free] = s_Want;
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

void PoseHook::SetPins(std::vector<Pin>& p_Pins) {
    std::sort(p_Pins.begin(), p_Pins.end(), [](const Pin& a, const Pin& b) { return a.animator < b.animator; });
    std::unique_lock s_Lock(g_PinMutex);
    g_Pins.swap(p_Pins);
    g_PinCount.store(static_cast<uint32_t>(g_Pins.size()), std::memory_order_relaxed);
}

void PoseHook::SetPinsOn(bool p_On) {
    g_PinsOn = p_On;
}

bool PoseHook::PinsOn() {
    return g_PinsOn;
}

PoseHook::PinStats PoseHook::TakePinStats() {
    PinStats s_Stats;
    s_Stats.pinned = g_PinCount.load();
    s_Stats.layout = g_PinLayout.load();
    s_Stats.corrected = g_PinCorrected.exchange(0);
    s_Stats.aligned = g_PinAligned.exchange(0);
    s_Stats.unusable = g_PinUnusable.exchange(0);
    s_Stats.maxAngle = static_cast<float>(g_PinMaxAngle.exchange(0)) / 100.f;

    for (int i = 0; i < 5; ++i)
        s_Stats.angles[i] = g_PinAngles[i].exchange(0);
    return s_Stats;
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
