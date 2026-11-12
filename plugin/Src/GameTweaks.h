#pragma once

#include <string>

// The game's own settings file, Retail\thumbs.dat (encrypted text; see installer\setup\GameFiles.cs
// for the format and for what the setup changes in it, and why).
//
// The setup makes the change and notes it in Retail\HitmanCoop-install.json. A game update puts
// the game's own file back; the mod then makes the change again here, at start (it takes effect
// the next time the game starts), keeping the game's file beside it as the setup does. A player
// who unticked the change in the setup has no note in the install file, and nothing is touched.
namespace GameTweaks {

// Looks at the file, and makes the noted change again if it is gone. p_Retail ends with a slash.
// Returns one line for the log and the diagnostics.
std::string CheckAndHeal(const std::string& p_Retail);
// The last result (for the diagnostics).
const std::string& State();

// Exposed for the test channel: the file's plain text, or empty when it can't be read.
std::string ReadPlain(const std::string& p_Path);

} // namespace GameTweaks
