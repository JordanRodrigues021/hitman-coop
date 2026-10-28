#pragma once

// The game's pause, the way its menus do it: a list of named pause requests in the game loop
// manager (the pause menu's is "UI_Menus"); the game runs while the list is empty. Found by the
// menus' own code, in whatever game build is running.
namespace GamePause {
    bool Available();
    // Our player has the pause menu (or another menu that pauses) open.
    bool MenuOpen();
    // The partner's: our game is held paused under our own name while they are.
    void SetPartnerPause(bool p_Paused);
    // Held paused until the partner's game is in the mission too ("start together").
    void SetWaitPause(bool p_Paused);
    bool Holding(); // either hold is on
    void Forget(); // a new scene: our hold is gone with the old one

    // The game's "RestartLevel" (what the pause menu's Restart calls), found by its registration
    // under that name; false if this game build has none.
    bool CanRestart();
    bool Restart();
}
