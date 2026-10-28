#pragma once

#include <string>

// Lets an automated test drive the mod in the real game without touching the UI: commands are
// read from a text file and the mod's state is written next to it as JSON. Off unless the
// `test_control` setting (mods/HitmanCoop.ini, [general]) names a file; players never need it.
//
//   <file>          one command per write: host | join <code or address> | leave | relay <host[:port]>
//                   | npcs (writes the actors that are not alive to the trace)
//                   | inputlog (traces the input actions the game asks about for 3 s)
//                   | press <action> (that input action reads as pressed for one frame)
//                   | actorai (traces the stand-in's AI setup next to a few NPCs')
//                   | partner on|off (off: never spawn the stand-in)
//                   | types <words> (writes matching engine types, properties and pins to <file>.types)
//   <file>.status   rewritten twice a second
//   <file>.trace    lifecycle lines (init, engine up, first frame), appended as they happen; the
//                   SDK's own log is only written when the game exits cleanly
class TestChannel {
public:
    void SetPath(const std::string& p_Path) { m_Path = p_Path; }
    bool Enabled() const { return !m_Path.empty(); }
    const std::string& Path() const { return m_Path; }

    // Returns the next command, or empty. Call often; it only touches the disk every 250 ms.
    std::string Poll(double p_Now);

    bool StatusDue(double p_Now);
    void WriteStatus(const std::string& p_Json);
    void Trace(const std::string& p_Line);

    static std::string Escape(const std::string& p_Text);

private:
    std::string m_Path;
    double m_NextPoll = 0.0;
    double m_NextStatus = 0.0;
};
