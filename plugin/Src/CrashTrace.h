#pragma once

#include <string>

// Writes the first faults in the game (access violations, illegal instructions, stack overflows,
// heap corruption, and anything left unhandled) to a file, so a crash can be located without a
// debugger attached. Each report has: the module-relative address, the registers and the bytes of
// the faulting instruction, a stack walk (the mod's own frames by function name, from
// HitmanCoop.sym; the game's with the start of the function they are in), what the mod was doing,
// and the tail of the flight recorder (BlackBox). All of the flight recorder goes to
// p_BlackBoxPath, and a minidump to p_DumpPath (it holds the game's memory: kept on the player's
// PC, never put in a bug report by itself). p_Header (the mod and game versions) heads each report.
//
// A fault inside a BlackBox::Guarded scope is one the mod catches: reported in short, no dump.
namespace CrashTrace {
void Install(const std::string& p_Path, const std::string& p_Header = {}, const std::string& p_BlackBoxPath = {},
    const std::string& p_DumpPath = {}, bool p_WriteDump = true);
// How many of the mod's functions can be named in a report (0: HitmanCoop.sym is missing).
size_t SymbolCount();
int Reports();
int CaughtFaults();
}
