#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Scripted interactions, shared between the two games.
//
// A mission's own logic runs in each game by itself. When a player uses something that only the
// mission script cares about (signs a paper, sabotages a machine, pours a drink), the level's
// interaction entity (ZHM5Action) signals an output pin and the script takes it from there:
// objectives advance, an NPC is sent to talk. In the partner's game nobody pressed anything, so
// its script stayed where it was, and whichever game ran the NPC, the mission was stuck (a target
// who never spoke again after the NDA was signed, 0.13.95).
//
// This watches the pins of the level's interactions. One fired while our 47 stands at it is told
// to the partner, whose game signals the same pin on the same entity (found by its place in the
// scene, the same in both games): the script there goes on as if their player had done it.
// Doors, items, outfits, bodies, containers and hiding places are left out: they have their own
// sync, and firing their pins a second time would undo it.
namespace WorldScript {

struct Watched {
    void* object = nullptr;  // the entity (ZEntityRef::m_pObj)
    uint64_t key = 0;        // its place in the scene: the same in both games
    int32_t type = 0;        // EActionType
    bool shared = false;     // of a kind that is told to the partner
    bool hasPos = false;
    float pos[3] {};
    char name[40] {};
};

// The level's interactions, set when a mission is ready (replaces the last list).
void SetWatched(std::vector<Watched> p_List);
void Clear();
// Where our 47 stands (each frame).
void SetHero(float p_X, float p_Y, float p_Z, bool p_Valid);
// [sync] scripted_actions: told and applied, or only watched (the flight recorder still gets them).
void SetShared(bool p_On);
bool Shared();

// What a pin carries: nothing, or a simple value that can be sent (anything else is never shared).
enum ValueKind : int { Value_None = 0, Value_Int32 = 1, Value_Bool = 2, Value_Float = 3, Value_UInt32 = 4, Value_Other = -1 };
ValueKind KindOf(const char* p_TypeName);

// From the output pin hook, on whatever thread signals it. Cheap for anything not watched.
void OnOutputPin(void* p_Object, uint32_t p_Pin, ValueKind p_Kind, uint32_t p_Bits, const char* p_TypeName);

struct Fired {
    uint64_t key;
    uint32_t pin;
    int kind = Value_None;
    uint32_t bits = 0; // the value's bytes (int32, bool, float32, uint32)
};
// What our player set off since the last call (the game's main thread).
bool Take(std::vector<Fired>& p_Out);
// The partner's: the entity to signal, or nullptr when this game doesn't have it. Marks it, so
// the pin we then signal isn't told back.
void* BeginReplay(uint64_t p_Key, uint32_t p_Pin, std::string& p_Name);
void EndReplay();

struct Counts {
    size_t watched = 0, shared = 0;
    uint32_t seen = 0, told = 0, applied = 0, unknown = 0, scripted = 0, withData = 0, repeating = 0;
};
Counts Stats();
std::string Describe();
// A copy of what is watched (the test channel's list).
std::vector<Watched> List();
// A name for a pin id when it is one of the usual ones (for the logs), else its number.
std::string PinName(uint32_t p_Pin);

} // namespace WorldScript
