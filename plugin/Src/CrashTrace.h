#pragma once

#include <string>

// Writes the first faults in the game (access violations, illegal instructions, stack overflows,
// heap corruption, and anything left unhandled) to a file, each with its module-relative address
// and a stack walk, so a crash can be located without a debugger attached. p_Header (the mod and
// game versions) heads each report.
namespace CrashTrace {
void Install(const std::string& p_Path, const std::string& p_Header = {});
}
