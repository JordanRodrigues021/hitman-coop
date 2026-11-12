#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Watches the two online connections the game has besides co-op's, so a player's report of "Steam
// says my account signed in on another computer" can be explained:
// - Steam: is this PC signed in to Steam, is this copy of the game borrowed (Family Sharing: the
//   owner starting any game signs the borrower out), and is the partner on the same Steam account
//   (two PCs on one account: Steam signs one out, at random moments). Read from the game's own
//   steam_api64.dll (only queries; nothing is changed), Steam copies only.
// - IOI's servers: the game's online state machine (ZOnlineManager), whose drops and re-sign-ins
//   are logged with the time, and told to the player when they happen during co-op.
// Co-op never uses either: its own connection goes on through all of this.
namespace OnlineWatch {
    void Start();

    // Each frame: checks once a second. p_Log gets lines for the log, p_Warn what the player should
    // hear about (only while p_InSession).
    void Update(double p_Now, bool p_InSession, const std::function<void(const std::string&)>& p_Log,
        const std::function<void(const std::string&)>& p_Warn);

    // "<tag> <family>": what the partner compares (see Coop_Account). The tag is a salted hash of
    // this PC's Steam account (not the account id). Empty without Steam.
    std::string AccountMessage();
    // The partner's AccountMessage: a warning for both players if they share one account (or a
    // borrowed copy), empty if all is well.
    std::string CheckPartnerAccount(const std::string& p_Message, const std::string& p_PartnerName);

    // One line for diagnostics and bug reports.
    std::string Describe();
    const char* OnlineStateName(int32_t p_State);
}
