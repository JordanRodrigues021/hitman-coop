#pragma once

#include <string>

// Test builds only (installed when the test channel is on): writes the first access violations
// in the game to a file, each with its module-relative address and a stack walk, so a crash can
// be located without a debugger attached. Players never get this.
namespace CrashTrace {
void Install(const std::string& p_Path);
}
