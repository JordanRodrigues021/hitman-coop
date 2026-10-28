#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Patches to HITMAN3.exe's code, made in memory while the game runs and put back on unload. Each
// place is found by its code pattern, in whatever build is running (taken from 3.280.0 with every
// offset and address left open), and a build that doesn't have one leaves only that one out. Each
// acts only on the partner's stand-in:
// - three places that read through an actor's m_pAnimatedActor without checking it. On Restart
//   Mission the stand-in's animated actor is freed before the actor itself, and the game crashed.
// - its speech: every line an NPC says goes through ZActor's ICharacterSpeakController::Speak,
//   which already handles an actor without a voice. The stand-in is sent down that path, so it
//   never says a word (your partner's 47 doesn't talk either).
// - its AI reasoning: the per-actor step that runs reactions, searches and investigations (and
//   sets the alert level they need) is skipped for it, so it never joins a search or reacts to
//   anything. Its body, animation and our pose are untouched.
namespace CrashGuards {
    // Returns how many patches were applied (of Count()).
    int Install();
    int Count();
    // Every place (up to 16) a code pattern (IDA style) matches in HITMAN3.exe.
    std::vector<uintptr_t> FindAll(const char* p_Pattern);
    std::string Report(); // which were found
    // ZKnowledge's own alert level (EGameTension), -1 if not known in this build.
    int32_t TensionOf(const void* p_Knowledge);
    // Alert levels the partner's game has for NPCs (by their ZKnowledge): ours are kept at least
    // that alert until p_ForMs from now, when the next set replaces them.
    void SetAlertFloors(const std::vector<std::pair<const void*, int32_t>>& p_Floors, uint32_t p_ForMs);
    int32_t AlertFloor(const void* p_Knowledge);
    // The stand-in. Kept until the next one: it must still be set while the scene tears it down.
    // The restart guards cover every stand-in set (the last 16).
    void SetActor(void* p_Actor);
    // Its AI knowledge, whose alert level is held at ambient (see HitmanCoop's tension detour).
    // Cleared when a scene is cleared, so a later NPC reusing the address is left alone.
    // p_Think: its AI reasoning still runs. It must run for its first moments: never run, its AI
    // record was never set up and other NPCs saw it as a murdered body (3.280.0 test).
    void SetKnowledge(void* p_Knowledge, bool p_Think = true);
    bool IsStandInKnowledge(const void* p_Knowledge);
    // The partner's NPCs here (puppets): their AI reasoning is skipped too.
    void SetPuppet(void* p_Actor, bool p_Puppet);
    void ClearPuppets();
    size_t PuppetCount();
    // Lines the stand-in would have said, and alert levels it would have reached, since the start.
    uint32_t MutedLines();
    uint32_t AllLines(); // lines spoken by any actor (test)
    uint32_t SkippedThinks();
    void NoteCalmed(int32_t p_Wanted);
    // Test: where in HITMAN3.exe the calmed requests came from (return addresses, most frequent first).
    void NoteCalmedFrom(const void* const* p_Frames, int p_Count);
    std::string CalmedFrom();
    uint32_t Calmed();
    int32_t HighestWanted();
    void Uninstall();
}
