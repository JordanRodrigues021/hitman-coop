#pragma once

#include <string>

// Test channel only: writes the game's reflected types whose names contain any of the given
// words (space separated, case-insensitive) to a file, with each class's properties and input
// pin ids. Used to find engine classes the mod can use, such as NPC movement behaviours.
namespace TypeDump {
void Write(const std::string& p_Filter, const std::string& p_Path);
}
