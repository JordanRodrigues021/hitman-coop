#pragma once

#include <string>

#include "Glacier/ZEntity.h"

class ZHM5BaseCharacter;

// Test channel only: dumps a character's animation data (morpheme entity, bone data and what
// they point to) as hex and floats, so the pose layout can be found. Reads go through
// ReadProcessMemory, so a wrong guess can't crash the game.
namespace BoneProbe {
std::string Dump(const char* p_Label, ZHM5BaseCharacter* p_Character, size_t p_Bytes);
// The IBoneAnimator function table of an entity, as HITMAN3.exe offsets.
std::string AnimatorTable(const char* p_Label, ZEntityRef p_Entity);
}
