#pragma once

#include <cstdint>
#include <array>
#include <vector>

// Copies skeleton poses between characters, so the partner's stand-in moves exactly like the
// partner's 47: walking, running, crouching, subduing, killing, climbing, anything.
//
// Every character's animation network (ZMorphemeEntity) is an IBoneAnimator, and they all share
// one function table. Its second entry computes the frame's pose into an output buffer: bone
// array at +0x08, bone count at +0x10, 32 bytes per bone (rotation quaternion, then position;
// parent-relative). This replaces that table entry with a wrapper that runs the game's own
// function and then either copies the result out (for the local 47) or overwrites it (for the
// stand-in). Poses are computed on the game's worker threads, hence the lock.
namespace PoseHook {
struct Bone {
    float rot[4];
    float pos[4];
};

// Patches the function table found on this animator interface (an IBoneAnimator* of any
// character). Safe to call repeatedly; false if the table couldn't be patched.
bool Install(void* p_AnyMorphemeAnimator);
void Uninstall();
bool Installed();

// Which animators to read from and write to (IBoneAnimator interface pointers), or nullptr.
// Source channel 0 is the local 47, channel 1 the NPC he is taking down.
void SetSource(void* p_Animator, size_t p_Channel = 0);
void SetTarget(void* p_Animator);

// The source's latest pose. False if none has been captured since the last call.
bool TakeSourcePose(std::vector<Bone>& p_Out, size_t p_Channel = 0);
// The pose the target is given every frame until cleared. Bones beyond the target's count are
// ignored; translations are only applied to bone 0 and 1 (root and pelvis) unless p_AllPositions,
// so the stand-in keeps its own limb lengths.
void SetTargetPose(const std::vector<Bone>& p_Pose, bool p_AllPositions);
void ClearTargetPose();
// The target's own pose (as its animation made it, before ours), from its last update.
bool TargetNativePose(std::vector<Bone>& p_Out);
// Only these bones of the target pose are used (empty: all). For a skeleton that only partly fits.
void SetTargetMask(const std::vector<uint8_t>& p_Mask);
// Target bone i copies source bone p_Map[i] (-1: keeps its own). Empty: the same index.
void SetTargetMap(const std::vector<int32_t>& p_Map);

// Extra targets (the partner's takedown victims): each animator is given its pose every frame
// until cleared. False when all slots are in use.
bool SetPoseFor(void* p_Animator, const std::vector<Bone>& p_Pose);
void ClearPoseFor(void* p_Animator);
// Animators whose own pose is recorded (our NPCs near the partner, sent to them), and the latest.
void SetWatched(const std::vector<void*>& p_Animators);
bool WatchedPose(void* p_Animator, std::vector<Bone>& p_Out);
// Diagnostics: an extra target's own first bones (before ours) and its bone count.
    uint32_t NativePoseOf(void* p_Animator, std::vector<Bone>& p_Out);
    void ClearExtraPoses();

// World pins. A character the partner's game runs (a puppet) is put where the partner's game has
// it every frame, but its own movement code still runs here on its last orders and turns or
// shifts the body afterwards: players saw NPCs walk north facing south. A pin says where the
// skeleton must stand in the world (rot: x y z w, pos: x y z); the hook then moves the root bone
// by whatever the character's own transform is off, so it is drawn exactly there, posed by the
// partner or by its own animation.
struct Pin {
    void* animator;
    float rot[4];
    float pos[3];
};
// All pins at once, every frame (sorted in place; the vector is left holding the old pins).
void SetPins(std::vector<Pin>& p_Pins);
void SetPinsOn(bool p_On);
bool PinsOn();
struct PinStats {
    uint32_t pinned = 0;    // characters pinned right now
    int layout = 0;         // the engine's world matrix: 0 not seen yet, 1: 4x4, 2: 4x3
    uint32_t corrected = 0; // pose updates since the last call where the skeleton had to be moved
    uint32_t aligned = 0;   // ... where it stood right already
    uint32_t unusable = 0;  // ... where the world matrix wasn't one (nothing moved)
    float maxAngle = 0.f;   // the largest turn corrected, degrees
    uint32_t angles[5] {};  // corrections by angle: <10, 10-45, 45-80, 80-100, 100+ degrees
};
PinStats TakePinStats();

// Diagnostics for the test channel.
uint32_t SourceBoneCount();
uint32_t TargetBoneCount();
uint64_t Calls();
uint64_t TargetCalls();
}
