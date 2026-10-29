// HitmanCoop-NetTest: checks that two PCs can reach each other before anyone starts the game.
// It runs the mod's real session and networking code against a fake mission (the same one the
// core tests use): the host's 47 walks a circle, NPCs walk theirs, and the friend kills one NPC.
//
//   HitmanCoop-NetTest.exe                 asks whether to host or join
//   HitmanCoop-NetTest.exe host [port]
//   HitmanCoop-NetTest.exe join <address[:port]>
//   HitmanCoop-NetTest.exe bot <address[:port]> [command file]   fake friend for in-game testing
// A 6-character join code works wherever an address does; it goes through the co-op server.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <cctype>
#include <deque>
#include <unordered_set>
#include <vector>

#include <Windows.h>
#include <conio.h>
#include <timeapi.h>

#include "../tests/FakeWorld.h"
#include "coop/RelaySocket.h"

using namespace coop;

namespace {

constexpr double c_TestLength = 20.0;
constexpr double c_KillAt = 5.0;
constexpr uint64_t c_KillTarget = 0x1003;
constexpr int c_NpcCount = 8;

const SceneDesc c_TestScene { "[assembly:/_hitmancoop/nettest.entity].pc_entitytype", {}, "nettest" };

double Now() {
    using namespace std::chrono;
    static const auto s_Start = steady_clock::now();
    return duration<double>(steady_clock::now() - s_Start).count();
}

// Sleeps about p_Ms milliseconds. A high-resolution waitable timer is used because Windows 11
// ignores timeBeginPeriod for processes without a visible window, rounding every sleep up to
// 15.6 ms, which would show up as extra ping.
void ShortSleep(int p_Ms) {
    static HANDLE s_Timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    LARGE_INTEGER s_Due;
    s_Due.QuadPart = -10000LL * p_Ms;

    if (s_Timer && SetWaitableTimer(s_Timer, &s_Due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(s_Timer, INFINITE);
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(p_Ms));
}

void PrintNotes(FakeWorld& p_World) {
    for (const auto& s_Note : p_World.notes)
        std::printf("  [mod] %s\n", s_Note.c_str());

    p_World.notes.clear();
}

void WaitForEnter() {
    std::printf("\nPress Enter to close.\n");
    std::string s_Line;
    std::getline(std::cin, s_Line);
}

// The co-op relay server to use, from --relay or the build's default. Empty: direct only.
std::string g_RelayServer = COOP_DEFAULT_RELAY;

struct Link {
    std::unique_ptr<IDatagramSocket> socket;
    std::shared_ptr<const RelayState> relay; // outlives the socket, which the session owns
    Endpoint host;                // join only
    bool codeShown = false;
    bool relayFailShown = false;
};

// Host: a direct UDP port, plus a join code from the relay server when one is configured.
bool OpenHost(uint16_t p_Port, Link& p_Link, std::string& p_Error) {
    auto s_Udp = UdpSocket::Open(p_Port, p_Error);

    if (!s_Udp)
        return false;

    std::unique_ptr<RelaySocket> s_Relay;

    if (!g_RelayServer.empty()) {
        Endpoint s_Server;
        std::string s_RelayError;

        if (ResolveEndpoint(g_RelayServer, kDefaultRelayPort, s_Server, s_RelayError))
            s_Relay = RelaySocket::Host(s_Server, s_RelayError);

        if (!s_Relay)
            std::printf("Co-op server %s unavailable (%s); only direct connections will work.\n", g_RelayServer.c_str(),
                        s_RelayError.c_str());
    }

    if (s_Relay)
        p_Link.relay = s_Relay->State();

    p_Link.socket = std::make_unique<MultiSocket>(std::move(s_Udp), std::move(s_Relay));
    return true;
}

// Join: a join code goes through the relay server, anything else is an address.
bool OpenJoin(const std::string& p_Target, Link& p_Link, std::string& p_Error) {
    std::string s_Code;

    if (RelaySocket::NormalizeCode(p_Target, s_Code)) {
        Endpoint s_Server;

        if (g_RelayServer.empty()) {
            p_Error = "This build has no co-op server set, so join codes can't be used (pass --relay).";
            return false;
        }

        if (!ResolveEndpoint(g_RelayServer, kDefaultRelayPort, s_Server, p_Error))
            return false;

        auto s_Relay = RelaySocket::Join(s_Server, s_Code, p_Error);

        if (!s_Relay)
            return false;

        p_Link.relay = s_Relay->State();
        p_Link.socket = std::move(s_Relay);
        p_Link.host = Endpoint::Relay(kRelayHostPeer);
        std::printf("Joining with code %s through the co-op server %s...\n", s_Code.c_str(), g_RelayServer.c_str());
        return true;
    }

    if (!ResolveEndpoint(p_Target, kDefaultPort, p_Link.host, p_Error))
        return false;

    auto s_Udp = UdpSocket::Open(0, p_Error);

    if (!s_Udp)
        return false;

    p_Link.socket = std::move(s_Udp);
    std::printf("Joining %s directly...\n", p_Link.host.ToString().c_str());
    return true;
}

// Prints the join code once it arrives. Returns false if the relay failed and that is fatal
// (for a client; a host can still take direct connections).
bool WatchRelay(Link& p_Link, bool p_Host) {
    if (!p_Link.relay)
        return true;

    const RelayState& s_State = *p_Link.relay;

    if (s_State.phase == RelayState::Phase::Ready && p_Host && !p_Link.codeShown) {
        p_Link.codeShown = true;
        std::printf("\n    JOIN CODE:  %s    (your friend types this to join)\n\n", s_State.code.c_str());
    }

    if (s_State.phase == RelayState::Phase::Failed && !p_Link.relayFailShown) {
        p_Link.relayFailShown = true;
        std::printf("Co-op server: %s\n", s_State.error.c_str());
        return p_Host;
    }

    return s_State.phase != RelayState::Phase::Failed || p_Host;
}

int Run(bool p_Host, const std::string& p_Address, uint16_t p_Port) {
    std::string s_Error;
    Link s_Link;

    if (!(p_Host ? OpenHost(p_Port, s_Link, s_Error) : OpenJoin(p_Address, s_Link, s_Error))) {
        std::printf("Can't start: %s\n", s_Error.c_str());
        return 2;
    }

    FakeWorld s_World(p_Host ? "host" : "friend");
    SessionConfig s_Config;
    s_Config.playerName = p_Host ? "NetTest host" : "NetTest friend";
    s_Config.gameVersion = "";   // not a game: no version warning on either side
    Session s_Session(s_World, s_Config);

    if (p_Host) {
        s_World.LoadScene(c_TestScene, c_NpcCount, 0.f);
        s_Session.Host(std::move(s_Link.socket));
        const std::string s_Lan = LocalLanAddress();
        std::printf("Hosting. Your friend can join with:\n");

        if (s_Link.relay)
            std::printf("  the join code (shown below once the co-op server answers), from anywhere\n");

        if (!s_Lan.empty())
            std::printf("  %s on the same network, or your public IP if UDP port %u is forwarded to it\n", s_Lan.c_str(), p_Port);

        std::printf("Waiting for your friend (Ctrl+C to stop)...\n");
    }
    else {
        s_Session.Join(std::move(s_Link.socket), s_Link.host, Now());
    }

    double s_SyncedAt = -1.0;
    double s_NextPrint = 0.0;
    double s_Last = Now();
    bool s_Killed = false;
    bool s_SawPartner = false;
    float s_PartnerTravel = 0.f;
    Vec3 s_LastPartnerPos {};
    double s_WorstRtt = 0.0;
    double s_LastRtt = 0.0;

    while (true) {
        const double s_Now = Now();
        const float s_Dt = static_cast<float>(s_Now - s_Last);
        s_Last = s_Now;

        // Each 47 walks a 3 m circle at about 1.5 m/s, the friend's offset from the host's.
        const float s_Angle = static_cast<float>(s_Now) * 0.5f;
        const float s_Offset = p_Host ? 0.f : 10.f;
        s_World.player.pos = { s_Offset + 3.f * std::cos(s_Angle), 0.f, 3.f * std::sin(s_Angle) };

        if (p_Host) {
            s_World.StepNpcs(s_Dt);
        }
        else {
            SceneDesc s_HostScene;

            // What the mod's mission redirect does in game: load the host's mission.
            if (!s_World.inScene && s_Session.GetHostScene(s_HostScene)) {
                std::printf("Host is in mission '%s', loading it.\n", s_HostScene.codename.c_str());
                s_World.LoadScene(s_HostScene, c_NpcCount, 0.7f);
                s_Session.OnLocalSceneLoaded();
            }

            if (s_World.inScene)
                s_World.StepNpcs(s_Dt);
        }

        s_Session.Tick(s_Now, s_Dt);
        PrintNotes(s_World);

        if (!WatchRelay(s_Link, p_Host))
            return 1;

        const SessionStatus s_Status = s_Session.Status();

        if (s_Status.state == SessionState::Failed || s_Status.state == SessionState::Idle) {
            std::printf("\nFAIL: %s\n", s_Status.message.c_str());

            if (!p_Host && !s_Link.relay) {
                std::printf("Also check that the host has this test running and that Windows Firewall on\n"
                            "their PC allows it.\n"
                            "Joining the host's public IP from the host's own PC (or from the same home\n"
                            "network) usually fails even when the port is forwarded: most routers don't loop\n"
                            "it back. On the same network use the host's LAN address (it starts with 192.168.\n"
                            "or 10.); otherwise test with your friend or from a phone hotspot.\n");
            }

            return 1;
        }

        if (s_SyncedAt < 0.0 && s_Session.Synced()) {
            s_SyncedAt = s_Now;
            std::printf("Connected to %s (%s) and in the same mission. Testing for %.0f seconds...\n",
                        s_Status.peerName.c_str(), s_Status.peerAddress.c_str(), c_TestLength);
        }

        if (s_World.remoteShown) {
            if (s_SawPartner)
                s_PartnerTravel += Distance(s_World.remoteXf.pos, s_LastPartnerPos);

            s_SawPartner = true;
            s_LastPartnerPos = s_World.remoteXf.pos;
        }

        if (s_SyncedAt >= 0.0) {
            if (s_Status.state == SessionState::Connected) {
                s_LastRtt = s_Status.stats.rttMs;
                s_WorstRtt = std::max(s_WorstRtt, s_LastRtt);
            }

            if (!p_Host && !s_Killed && s_Now - s_SyncedAt > c_KillAt) {
                std::printf("Killing NPC %llx on this side; the host should confirm it.\n",
                            static_cast<unsigned long long>(c_KillTarget));
                s_World.KillNpc(c_KillTarget);
                s_Killed = true;
            }
        }

        if (s_Now >= s_NextPrint) {
            s_NextPrint = s_Now + 1.0;

            if (s_Status.state == SessionState::Connected) {
                std::printf("  %-9s ping %4.0f ms  sent %llu  recv %llu  resent %llu  NPCs %zu  partner %s\n",
                            s_Session.Synced() ? "synced" : "syncing", s_Status.stats.rttMs,
                            static_cast<unsigned long long>(s_Status.stats.packetsSent),
                            static_cast<unsigned long long>(s_Status.stats.packetsReceived),
                            static_cast<unsigned long long>(s_Status.stats.resends), s_Status.npcsTracked,
                            s_World.remoteShown ? "visible" : "-");
            }
        }

        if (s_SyncedAt >= 0.0 && s_Now - s_SyncedAt > c_TestLength)
            break;

        ShortSleep(4);
    }

    const SessionStatus s_Status = s_Session.Status();
    const auto& s_Npc = s_World.npcs[c_KillTarget];
    const bool s_KillOk = !s_Npc.Alive() && (p_Host ? s_World.killCalls[c_KillTarget] == 1 : true);
    const bool s_PartnerOk = s_SawPartner && s_PartnerTravel > 3.f;
    const bool s_NpcsOk = p_Host || s_Status.npcsTracked == c_NpcCount;
    const double s_Loss = s_Status.stats.packetsSent ? 100.0 * s_Status.stats.resends / s_Status.stats.packetsSent : 0.0;

    std::printf("\nResults\n");
    std::printf("  %s  connection and mission sync\n", "PASS");
    std::printf("  %s  partner position received and moving (%.0f m walked)\n", s_PartnerOk ? "PASS" : "FAIL",
                s_PartnerTravel);
    std::printf("  %s  NPCs tracked from the host (%zu of %d)\n", s_NpcsOk ? "PASS" : "FAIL",
                p_Host ? static_cast<size_t>(c_NpcCount) : s_Status.npcsTracked, c_NpcCount);
    std::printf("  %s  friend's kill made official by the host\n", s_KillOk ? "PASS" : "FAIL");
    std::printf("  ping %.0f ms (worst %.0f), resent %.1f%% of packets\n", s_LastRtt, s_WorstRtt, s_Loss);

    if (s_WorstRtt > 250.0)
        std::printf("  Note: ping above 250 ms will make the partner and NPCs lag behind.\n");

    const bool s_Ok = s_PartnerOk && s_NpcsOk && s_KillOk;
    std::printf("\n%s\n", s_Ok ? "Everything works. You can play together." : "Some checks failed (see above).");

    s_Session.Leave("Test finished", Now());

    // Give the goodbye a moment to reach the other side.
    for (int i = 0; i < 10; ++i) {
        s_Session.Tick(Now(), 0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return s_Ok ? 0 : 1;
}

// Bot: a stand-in friend for testing the mod in the real game without a second PC. It joins
// the host, follows the host's mission, walks circles around the host's 47 wearing the host's
// disguise, mirrors the host's NPCs, and logs everything the host sends.
// Commands, typed as keys or written as a line to the command file:
//   kill   ask the host to kill the alive NPC nearest the host's 47
//   stand  stop walking (stay put)       orbit  walk circles again
//   front [degrees]  stand 3 m in front of the host's 47, facing it (degrees turns the spot
//                    around the host, for checking which way the game's actors face)
//   at <x> <y> <z>   stand at that world position, facing the host's 47
//   line <speed> <length>  walk back and forth on a line in front of the host's 47 (m/s, m)
//   beside [metres]  stay at the host's side (+ right, - left), facing the way the host faces
//   echo [seconds] [metres]  replay the host's own path and skeleton pose that many seconds
//                    later, offset to the side: the host sees their own moves copied
//   ko     knock out the alive NPC nearest the host's 47 (the host's game should knock it out)
//   chat <text>  send a chat line         mark  mark the NPC nearest the host's 47 for the host
//   quit   leave the session
int RunBot(const std::string& p_Address, const std::string& p_CommandFile) {
    std::string s_Error;
    Link s_Link;

    if (!OpenJoin(p_Address, s_Link, s_Error)) {
        std::printf("Can't start the bot: %s\n", s_Error.c_str());
        return 2;
    }

    const Endpoint s_HostEp = s_Link.host;
    FakeWorld s_World("bot");
    SessionConfig s_Config;
    s_Config.playerName = "Test bot";
    s_Config.gameVersion = "";   // not a game: no version warning on either side
    Session s_Session(s_World, s_Config);
    s_Session.Join(std::move(s_Link.socket), s_HostEp, Now());

    auto Log = [](const char* p_Fmt, auto... p_Args) {
        std::printf("[%8.2f] ", Now());
        std::printf(p_Fmt, p_Args...);
        std::printf("\n");
    };

    Log("Bot joining %s. Keys: K kill nearest NPC, S stand, O orbit, Q quit.", p_Address.c_str());

    bool s_Orbit = true;
    bool s_Front = false;
    float s_FrontOffset = 0.f;
    bool s_At = false;
    Vec3 s_AtPos {};
    bool s_Line = false;
    float s_LineSpeed = 1.5f, s_LineLength = 8.f, s_LinePos = 0.f, s_LineDir = 1.f;
    Vec3 s_LineStart {}, s_LineAxis {};
    bool s_LineStarted = false;
    bool s_Beside = false;
    float s_BesideOffset = 1.4f;
    bool s_Echo = false;
    double s_EchoDelay = 1.0;
    float s_EchoOffset = 1.5f;
    struct EchoFrame { double time; Transform xf; Pose pose; FakeWorld::Victim victim; std::vector<int16_t> skeleton; };
    std::deque<EchoFrame> s_EchoFrames;
    bool s_Quit = false;
    bool s_WasSynced = false;
    float s_Angle = 0.f;
    double s_Last = Now();
    double s_NextReport = 0.0;
    int s_OutfitChanges = 0;
    size_t s_EventsSeen = 0;
    std::vector<NpcState> s_HostNpcs;
    std::unordered_set<uint64_t> s_KilledHere;
    std::unordered_set<uint64_t> s_KnockedHere;
    size_t s_ChatSeen = 0, s_MarksSeen = 0;
    bool s_LastBusy = false;
    MsgPlayerState s_LastHostState;
    uint8_t s_LastBusyStage = LoadStage_None;
    std::unordered_set<uint64_t> s_DeathsLogged;
    size_t s_LastPeerAlerts = 0;
    size_t s_LastPuppets = 0;
    bool s_LastHeld = false;

    double s_LoadUntil = 0.0; // "loadin": the bot's game is loading into the host's mission until then

    auto Command = [&](const std::string& p_Command) {
        if (p_Command == "stand") {
            s_Orbit = false;
            s_Front = false;
            s_At = false;
            s_Beside = s_Echo = false;
            s_Line = false;
            Log("Standing still.");
        }
        else if (p_Command == "orbit") {
            s_Orbit = true;
            s_Front = false;
            s_At = false;
            s_Beside = s_Echo = false;
            s_Line = false;
            Log("Walking circles around the host.");
        }
        else if (p_Command.rfind("echo", 0) == 0) {
            float s_Delay = 1.f, s_Offset = 1.5f;
            sscanf_s(p_Command.c_str() + 4, "%f %f", &s_Delay, &s_Offset);
            s_EchoDelay = s_Delay;
            s_EchoOffset = s_Offset;
            s_Orbit = s_Front = s_At = s_Line = s_Beside = s_Echo = false;
            s_Echo = true;
            s_EchoFrames.clear();
            Log("Echoing the host %.1f s later, %.1f m to the side.", s_Delay, s_Offset);
        }
        else if (p_Command.rfind("beside", 0) == 0) {
            if (p_Command.size() > 7)
                s_BesideOffset = std::strtof(p_Command.c_str() + 7, nullptr);

            s_Orbit = s_Front = s_At = s_Line = false;
            s_Beside = true;
            Log("Standing %.1f m to the host's side.", s_BesideOffset);
        }
        else if (p_Command.rfind("line", 0) == 0) {
            sscanf_s(p_Command.c_str() + 4, "%f %f", &s_LineSpeed, &s_LineLength);
            s_Orbit = s_Front = s_At = s_Beside = s_Echo = false;
            s_Line = true;
            s_LineStarted = false;
            Log("Walking a %.1f m line at %.1f m/s.", s_LineLength, s_LineSpeed);
        }
        else if (p_Command.rfind("at ", 0) == 0) {
            if (sscanf_s(p_Command.c_str() + 3, "%f %f %f", &s_AtPos.x, &s_AtPos.y, &s_AtPos.z) == 3) {
                s_Orbit = false;
                s_Front = false;
                s_Beside = s_Echo = false;
                s_At = true;
                s_Line = false;
            Log("Standing at (%.1f, %.1f, %.1f).", s_AtPos.x, s_AtPos.y, s_AtPos.z);
            }
        }
        else if (p_Command == "front" || p_Command.rfind("front ", 0) == 0) {
            s_Orbit = false;
            s_At = false;
            s_Beside = s_Echo = false;
            s_Front = true;
            s_FrontOffset = p_Command.size() > 6 ? std::strtof(p_Command.c_str() + 6, nullptr) * 0.017453292f : 0.f;
            s_Line = false;
            Log("Standing in front of the host's 47 (offset %.0f degrees).", s_FrontOffset * 57.29578f);
        }
        else if (p_Command.rfind("alert", 0) == 0) {
            // alert <tension>: the 6 NPCs nearest the host are alert that much in "our" game
            // (EGameTension: 2 agitated .. 8 combat); alert 0 calms them.
            const int s_Tension = std::atoi(p_Command.c_str() + 5);
            s_World.alerts.clear();

            if (s_Tension > 1) {
                std::vector<std::pair<float, uint64_t>> s_Near;

                for (const auto& [s_Id, s_Npc] : s_World.npcs)
                    if (s_Npc.Alive())
                        s_Near.emplace_back(Distance(s_Npc.xf.pos, s_World.remoteXf.pos), s_Id);

                std::sort(s_Near.begin(), s_Near.end());

                for (size_t i = 0; i < s_Near.size() && i < 6; ++i)
                    s_World.alerts.push_back({ s_Near[i].second, static_cast<uint8_t>(s_Tension) });
            }

            Log("Alert %d on %zu NPCs near the host.", s_Tension, s_World.alerts.size());
        }
        else if (p_Command == "restart") {
            // As if the bot restarted the mission it shares with the host.
            if (s_World.inScene) {
                s_Session.OnLocalRestart(s_World.scene.scene);
                Log("Restarted the mission (the host's game should restart too).");
            }
        }
        else if (p_Command == "pause on" || p_Command == "pause off") {
            s_World.paused = p_Command == "pause on";
            Log(s_World.paused ? "Pause menu open (the host's game should pause)." : "Pause menu closed.");
        }
        else if (p_Command == "quit") {
            s_Quit = true;
        }
        else if (p_Command.rfind("chat ", 0) == 0) {
            s_Session.SendChat(p_Command.substr(5));
            Log("Said '%s'.", p_Command.substr(5).c_str());
        }
        else if (p_Command == "hunted" || p_Command == "calm") {
            // As if the bot's 47 were hunted while trespassing, after a target kill and a knock-out.
            MsgPlayerState s_State;

            if (p_Command == "hunted") {
                s_State.flags = PlayerFlag_Trespassing;
                s_State.threat = Threat_Hunted;
                s_State.stats.kills = s_State.stats.targets = 1;
                s_State.stats.knockouts = 1;
            }

            s_Session.SetLocalPlayerState(s_State);
            Log(p_Command == "hunted" ? "Hunted and trespassing (the host's HUD should say so)." : "Calm again.");
        }
        else if (p_Command == "feed") {
            // A kill feed line and a challenge, as the mod sends them.
            s_Session.SendMissionEvent("Coop_Kill", "TH|Viktor Novikov");
            s_Session.SendMissionEvent("Coop_Challenge", "Fresh Disguise");
            Log("Sent a kill and a challenge.");
        }
        else if (p_Command.rfind("loadin ", 0) == 0) {
            // As if the bot's game left and loaded back into the host's mission, taking that long.
            const double s_Seconds = std::clamp(std::atof(p_Command.c_str() + 7), 1.0, 120.0);
            SceneDesc s_Host;

            if (s_World.inScene) {
                s_World.UnloadScene();
                s_Session.OnLocalSceneUnloaded();
            }

            s_Session.GetHostScene(s_Host);
            s_Session.NotifyBusy(Now(), static_cast<uint16_t>(s_Seconds + 10.0), LoadStage_Loading, s_Host.scene);
            s_LoadUntil = Now() + s_Seconds;
            Log("Loading into the host's mission for %.0f s.", s_Seconds);
        }
        else if (p_Command.rfind("event ", 0) == 0) {
            // event <name> [text]: a mission event as the mod sends them (Coop_Signal help, Coop_ReadyCheck, ...).
            const std::string s_Rest = p_Command.substr(6);
            const size_t s_Space = s_Rest.find(' ');
            const std::string s_Name = s_Rest.substr(0, s_Space);
            const std::string s_Text = s_Space == std::string::npos ? std::string() : s_Rest.substr(s_Space + 1);
            s_Session.SendMissionEvent(s_Name, s_Text);
            Log("Sent event %s '%s'.", s_Name.c_str(), s_Text.c_str());
        }
        else if (p_Command == "away") {
            // Tabbed out, in green.
            MsgPlayerState s_State;
            s_State.flags = PlayerFlag_Away;
            s_State.colour = 0x30FF80;
            s_Session.SetLocalPlayerState(s_State);
            Log("Away (tabbed out), colour green.");
        }
        else if (p_Command.rfind("hold", 0) == 0) {
            // hold <repository id>: the bot's 47 has that item in hand (the host's stand-in should too).
            MsgPlayerState s_State;
            s_State.heldItem = p_Command.size() > 5 ? p_Command.substr(5) : std::string();
            s_Session.SetLocalPlayerState(s_State);
            Log("Holding '%s'.", s_State.heldItem.c_str());
        }
        else if (p_Command == "ping") {
            s_Session.SendMissionEvent("Coop_Ping", "");
            Log("Asked where the host is.");
        }
        else if (p_Command == "ko" || p_Command == "mark" || p_Command == "danger" || p_Command == "unmark") {
            uint64_t s_Best = 0;
            float s_BestDist = 1e9f;
            Vec3 s_BestPos {};

            for (const auto& [s_Id, s_Npc] : s_World.npcs) {
                const float s_Dist = Distance(s_Npc.xf.pos, s_World.remoteXf.pos);

                if (s_Npc.Alive() && !(s_Npc.flags & NpcFlag_Pacified) && s_Dist < s_BestDist) {
                    s_Best = s_Id;
                    s_BestDist = s_Dist;
                    s_BestPos = s_Npc.xf.pos;
                }
            }

            if (!s_Session.Synced() || s_BestDist > 50.f) {
                Log("No standing NPC within 50 m of the host (or not in the host's mission).");
                return;
            }

            if (p_Command != "ko") {
                const uint8_t s_Kind = p_Command == "danger" ? MarkKind_Danger : p_Command == "unmark" ? MarkKind_Remove : MarkKind_Normal;
                s_Session.SendMark(s_Best, s_BestPos, s_Kind);
                Log("Marked NPC %016llx (%s), %.1f m from the host.", static_cast<unsigned long long>(s_Best), p_Command.c_str(), s_BestDist);
            }
            else {
                s_KnockedHere.insert(s_Best);
                Log("Knocking out NPC %016llx, %.1f m from the host. The host's game should knock it out.",
                    static_cast<unsigned long long>(s_Best), s_BestDist);
            }
        }
        else if (p_Command == "kill") {
            if (!s_Session.Synced() || !s_World.remoteShown) {
                Log("Can't kill yet: not in the host's mission.");
                return;
            }

            uint64_t s_Best = 0;
            float s_BestDist = 1e9f;

            for (const auto& [s_Id, s_Npc] : s_World.npcs) {
                const float s_Dist = Distance(s_Npc.xf.pos, s_World.remoteXf.pos);

                if (s_Npc.Alive() && s_Dist < s_BestDist) {
                    s_Best = s_Id;
                    s_BestDist = s_Dist;
                }
            }

            if (s_BestDist > 50.f) {
                Log("No alive NPC within 50 m of the host.");
                return;
            }

            Log("Killing NPC %016llx, %.1f m from the host. The host's game should kill it.",
                static_cast<unsigned long long>(s_Best), s_BestDist);
            s_KilledHere.insert(s_Best);
            s_World.KillNpc(s_Best);
        }
        else if (!p_Command.empty()) {
            Log("Unknown command '%s'.", p_Command.c_str());
        }
    };

    size_t s_PeerItemsSeen = 0;

    while (!s_Quit) {
        const double s_Now = Now();
        const float s_Dt = static_cast<float>(s_Now - s_Last);
        s_Last = s_Now;

        while (_kbhit()) {
            switch (std::tolower(_getch())) {
                case 'k': Command("kill"); break;
                case 's': Command("stand"); break;
                case 'o': Command("orbit"); break;
                case 'q': Command("quit"); break;
                default: break;
            }
        }

        if (!p_CommandFile.empty()) {
            FILE* s_File = nullptr;

            if (fopen_s(&s_File, p_CommandFile.c_str(), "r") == 0 && s_File) {
                char s_Text[64] {};
                const bool s_Got = std::fgets(s_Text, sizeof(s_Text), s_File) != nullptr;
                std::fclose(s_File);
                std::remove(p_CommandFile.c_str());

                if (s_Got) {
                    std::string s_Cmd(s_Text);
                    while (!s_Cmd.empty() && std::isspace(static_cast<unsigned char>(s_Cmd.back())))
                        s_Cmd.pop_back();
                    Command(s_Cmd);
                }
            }
        }

        // Bodies the host moved or put away.
        for (; s_PeerItemsSeen < s_World.peerItems.size(); ++s_PeerItemsSeen) {
            const auto& s_Item = s_World.peerItems[s_PeerItemsSeen];

            if (s_Item.flags & ItemFlag_Body)
                Log("Host's body %016llx %s at (%.2f, %.2f, %.2f).", static_cast<unsigned long long>(s_Item.id),
                    (s_Item.flags & ItemFlag_BodyHidden) ? "put away" : "moved", s_Item.xf.pos.x, s_Item.xf.pos.y, s_Item.xf.pos.z);
        }

        // What the host's game reports alert (its own NPCs' alert levels).
        if (s_World.peerAlerts.size() != s_LastPeerAlerts) {
            s_LastPeerAlerts = s_World.peerAlerts.size();
            int s_Highest = 0;

            for (const auto& s_Alert : s_World.peerAlerts)
                s_Highest = std::max<int>(s_Highest, s_Alert.tension);

            Log("Host reports %zu alert NPCs (highest level %d).", s_LastPeerAlerts, s_Highest);
        }

        if (s_World.puppets.size() != s_LastPuppets || s_World.heldPaused != s_LastHeld) {
            s_LastPuppets = s_World.puppets.size();
            s_LastHeld = s_World.heldPaused;
            Log("Shared NPCs: %zu run by the host (puppets here), %zu by us; held paused: %s.", s_LastPuppets,
                s_World.npcs.size() - std::min(s_World.npcs.size(), s_LastPuppets), s_LastHeld ? "yes" : "no");
        }

        // Follow the host: load its mission, leave when it leaves.
        SceneDesc s_HostScene;

        if (s_Now < s_LoadUntil) {
            // still "loading"
        }
        else if (s_Session.GetHostScene(s_HostScene)) {
            if (!s_World.inScene || !s_World.scene.SameAs(s_HostScene)) {
                Log("Host is in %s (%s, codename '%s'). Loading it.", LocationDisplayName(s_HostScene.Location()).c_str(),
                    s_HostScene.scene.c_str(), s_HostScene.codename.c_str());

                for (const auto& s_Brick : s_HostScene.bricks)
                    Log("  brick %s", s_Brick.c_str());

                s_World.LoadScene(s_HostScene, 0, 0.f);
                s_KilledHere.clear();
                s_KnockedHere.clear();
                s_DeathsLogged.clear();
                s_Session.OnLocalSceneLoaded();
            }
        }
        else if (s_World.inScene && s_Session.Status().state == SessionState::Connected) {
            Log("Host left the mission.");
            s_World.UnloadScene();
            s_Session.OnLocalSceneUnloaded();
        }

        // Mirror the host's NPCs exactly, so nothing drifts and kills refer to real NPCs.
        if (s_World.inScene) {
            s_HostNpcs.clear();
            s_Session.GetHostNpcs(s_HostNpcs);

            for (const auto& s_Npc : s_HostNpcs) {
                auto& s_Mine = s_World.npcs[s_Npc.id];
                const bool s_KilledByUs = s_KilledHere.count(s_Npc.id) != 0;
                s_Mine = s_Npc;

                if (s_KilledByUs)
                    s_Mine.flags &= ~NpcFlag_Alive;

                if (s_KnockedHere.count(s_Npc.id))
                    s_Mine.flags |= NpcFlag_Pacified;

                if (!s_Npc.Alive() && s_DeathsLogged.insert(s_Npc.id).second)
                    Log("Host reports NPC %016llx dead%s.", static_cast<unsigned long long>(s_Npc.id),
                        s_KilledByUs ? " (our kill request worked)" : "");
            }
        }

        // Walk a 3 m circle around the host's 47, at its height, facing the way we walk.
        if (s_World.inScene && s_World.remoteShown) {
            if (s_Orbit)
                s_Angle += s_Dt * 0.5f;

            const Vec3 s_Center = s_World.remoteXf.pos;

            if (s_Front) {
                // The host's yaw around Z (Glacier is Z-up); characters face their local -Y.
                const auto& s_Q = s_World.remoteXf.rot;
                s_Angle = 2.f * std::atan2(s_Q.z, s_Q.w) - 1.5707964f + s_FrontOffset;
            }

            if (s_At)
                s_Angle = std::atan2(s_AtPos.y - s_Center.y, s_AtPos.x - s_Center.x);

            if (s_Echo) {
                s_EchoFrames.push_back({ s_Now, s_World.remoteXf, s_World.remotePose, s_World.remoteVictim, s_World.remoteSkeleton });

                while (s_EchoFrames.size() > 2 && s_EchoFrames[1].time <= s_Now - s_EchoDelay)
                    s_EchoFrames.pop_front();

                const EchoFrame& s_Frame = s_EchoFrames.front();
                const auto& s_Q = s_Frame.xf.rot;
                const float s_Yaw = 2.f * std::atan2(s_Q.z, s_Q.w);
                s_World.player.pos = { s_Frame.xf.pos.x - std::cos(s_Yaw) * s_EchoOffset,
                    s_Frame.xf.pos.y - std::sin(s_Yaw) * s_EchoOffset, s_Frame.xf.pos.z };
                s_World.player.rot = s_Q;
                s_World.pose = s_Frame.pose;
                s_World.skeleton = s_Frame.skeleton; // the layout of that pose, as late as it

                // The host's takedown victim too, moved by the same offset (the host picks one of
                // its own NPCs to play it, see the plugin's "victimtest").
                s_World.victim = s_Frame.victim;
                s_World.victim.xf.pos.x -= std::cos(s_Yaw) * s_EchoOffset;
                s_World.victim.xf.pos.y -= std::sin(s_Yaw) * s_EchoOffset;
            }
            else if (s_Beside) {
                // Facing -Y at yaw 0, the host's right is -X rotated by its yaw.
                const auto& s_Q = s_World.remoteXf.rot;
                const float s_Yaw = 2.f * std::atan2(s_Q.z, s_Q.w);
                s_World.player.pos = { s_Center.x - std::cos(s_Yaw) * s_BesideOffset, s_Center.y - std::sin(s_Yaw) * s_BesideOffset, s_Center.z };
                s_World.player.rot = s_Q;
            }
            else if (s_Line) {
                if (!s_LineStarted) {
                    // Start 3 m to the host's side and walk along the host's facing.
                    const auto& s_Q = s_World.remoteXf.rot;
                    const float s_Yaw = 2.f * std::atan2(s_Q.z, s_Q.w);
                    s_LineAxis = { std::sin(s_Yaw), -std::cos(s_Yaw), 0.f };
                    s_LineStart = { s_Center.x + 3.f * std::cos(s_Yaw), s_Center.y + 3.f * std::sin(s_Yaw), s_Center.z };
                    s_LinePos = 0.f;
                    s_LineDir = 1.f;
                    s_LineStarted = true;
                }

                s_LinePos += s_LineDir * s_LineSpeed * s_Dt;

                if (s_LinePos > s_LineLength) { s_LinePos = s_LineLength; s_LineDir = -1.f; }
                if (s_LinePos < 0.f) { s_LinePos = 0.f; s_LineDir = 1.f; }

                s_World.player.pos = { s_LineStart.x + s_LineAxis.x * s_LinePos, s_LineStart.y + s_LineAxis.y * s_LinePos, s_LineStart.z };
                // Face the way we walk: a yaw of 0 faces -Y.
                const float s_Heading = std::atan2(s_LineAxis.y * s_LineDir, s_LineAxis.x * s_LineDir);
                const float s_Yaw = s_Heading + 1.5707964f;
                s_World.player.rot = { 0.f, 0.f, std::sin(s_Yaw * 0.5f), std::cos(s_Yaw * 0.5f) };
            }
            else {

            s_World.player.pos = s_At ? s_AtPos
                : Vec3 { s_Center.x + 3.f * std::cos(s_Angle), s_Center.y + 3.f * std::sin(s_Angle), s_Center.z };

            // Glacier is Z-up: yaw around Z, and a yaw of 0 faces -Y (checked in game: the stand-in
            // turned its back to the host before). Walking: face along the circle; else face the host.
            const float s_Yaw = (s_Front || s_At) ? s_Angle - 1.5707964f : s_Angle + 3.1415927f;
            s_World.player.rot = { 0.f, 0.f, std::sin(s_Yaw * 0.5f), std::cos(s_Yaw * 0.5f) };
            }
        }

        // Wear what the host wears.
        if (s_World.remoteOutfitChanges != s_OutfitChanges) {
            s_OutfitChanges = s_World.remoteOutfitChanges;
            s_World.outfit = s_World.remoteOutfit;
            Log("Host outfit %s (charset %d, variation %d); wearing it too.", s_World.outfit.repositoryId.c_str(),
                s_World.outfit.charset, s_World.outfit.variation);
        }

        s_Session.Tick(s_Now, s_Dt);

        if (!WatchRelay(s_Link, false))
            return 1;

        for (const auto& s_Note : s_World.notes)
            Log("[mod] %s", s_Note.c_str());

        s_World.notes.clear();

        for (; s_EventsSeen < s_World.peerEvents.size(); ++s_EventsSeen)
            Log("Host event %s", s_World.peerEvents[s_EventsSeen].substr(0, 300).c_str());

        if (const bool s_Busy = s_Session.Status().peerBusy; s_Busy != s_LastBusy) {
            s_LastBusy = s_Busy;
            Log(s_Busy ? "Host is loading (waiting for it)." : "Host is done loading.");
        }

        {
            // What the host is up to, whenever it changes (its alert level, menu, mission stats), and
            // what its loading is at.
            const auto s_Status = s_Session.Status();

            if (s_Status.peerStateKnown && !(s_Status.peerState == s_LastHostState)) {
                s_LastHostState = s_Status.peerState;
                const auto& s_S = s_LastHostState;
                Log("Host state: threat %u, menu %u, trespassing %d, kills %u (targets %u), knocked out %u, spotted %u, bodies %u, challenges %u",
                    s_S.threat, s_S.menu, (s_S.flags & PlayerFlag_Trespassing) ? 1 : 0, s_S.stats.kills, s_S.stats.targets,
                    s_S.stats.knockouts, s_S.stats.spotted, s_S.stats.bodiesFound, s_S.stats.challenges);
            }

            const uint8_t s_Stage = s_Status.peerBusy ? s_Status.peerBusyStage : static_cast<uint8_t>(LoadStage_None);

            if (s_Stage != s_LastBusyStage) {
                s_LastBusyStage = s_Stage;

                if (s_Stage != LoadStage_None)
                    Log("Host is %s (%s).", LoadStageName(s_Stage), s_Status.peerBusyScene.c_str());
            }
        }

        for (; s_ChatSeen < s_World.chat.size(); ++s_ChatSeen)
            Log("Host says '%s'", s_World.chat[s_ChatSeen].c_str());

        for (; s_MarksSeen < s_World.marks.size(); ++s_MarksSeen)
            Log("Host marked %016llx at %.1f %.1f %.1f (kind %u)", static_cast<unsigned long long>(s_World.marks[s_MarksSeen].first),
                s_World.marks[s_MarksSeen].second.x, s_World.marks[s_MarksSeen].second.y, s_World.marks[s_MarksSeen].second.z,
                s_World.markKinds[s_MarksSeen]);

        const SessionStatus s_Status = s_Session.Status();

        if (s_Status.state == SessionState::Failed || s_Status.state == SessionState::Idle) {
            Log("Session ended: %s", s_Status.message.c_str());
            return 1;
        }

        if (s_Session.Synced() != s_WasSynced) {
            s_WasSynced = s_Session.Synced();
            Log(s_WasSynced ? "SYNCED with the host's mission." : "No longer synced.");
        }

        if (s_Now >= s_NextReport) {
            s_NextReport = s_Now + 5.0;

            if (s_Status.state == SessionState::Connected) {
                size_t s_Alive = 0;

                for (const auto& [s_Id, s_Npc] : s_World.npcs)
                    s_Alive += s_Npc.Alive() ? 1 : 0;

                Log("ping %.0f ms, %s, NPCs %zu (%zu alive), host 47 at (%.1f, %.1f, %.1f)", s_Status.stats.rttMs,
                    s_Session.Synced() ? "synced" : "not synced", s_World.npcs.size(), s_Alive, s_World.remoteXf.pos.x,
                    s_World.remoteXf.pos.y, s_World.remoteXf.pos.z);
            }
        }

        ShortSleep(4);
    }

    s_Session.Leave("Bot stopped", Now());

    for (int i = 0; i < 10; ++i) {
        s_Session.Tick(Now(), 0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    Log("Bot stopped.");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 1 ms sleeps where Windows honours it (see ShortSleep for where it doesn't).
    timeBeginPeriod(1);
    std::printf("==============================================\n"
                "  HITMAN CO-OP  -  connection test\n"
                "  created by Jordan Rodrigues\n"
                "==============================================\n\n");

    // "--relay host[:port]" anywhere picks the co-op server; "--relay none" turns it off.
    std::vector<std::string> s_Args;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--relay" && i + 1 < argc)
            g_RelayServer = std::string(argv[++i]) == "none" ? "" : argv[i];
        else
            s_Args.emplace_back(argv[i]);
    }

    std::string s_Mode = s_Args.size() > 0 ? s_Args[0] : "";
    std::string s_Arg = s_Args.size() > 1 ? s_Args[1] : "";
    const bool s_Interactive = s_Mode.empty();

    if (s_Interactive) {
        std::printf("Type H to host or J to join, then press Enter: ");
        std::getline(std::cin, s_Mode);
        s_Mode = !s_Mode.empty() && (s_Mode[0] == 'j' || s_Mode[0] == 'J') ? "join" : "host";

        if (s_Mode == "join") {
            std::printf("Host's join code (or IP address): ");
            std::getline(std::cin, s_Arg);
        }
    }

    int s_Result = 2;

    if (s_Mode == "host") {
        const int s_Port = s_Arg.empty() ? kDefaultPort : std::atoi(s_Arg.c_str());

        if (s_Port <= 0 || s_Port > 65535)
            std::printf("Bad port '%s'.\n", s_Arg.c_str());
        else
            s_Result = Run(true, "", static_cast<uint16_t>(s_Port));
    }
    else if (s_Mode == "join" && !s_Arg.empty()) {
        s_Result = Run(false, s_Arg, 0);
    }
    else if (s_Mode == "bot" && !s_Arg.empty()) {
        s_Result = RunBot(s_Arg, s_Args.size() > 2 ? s_Args[2] : "");
    }
    else {
        std::printf("Usage: HitmanCoop-NetTest host [port] | join <code or address[:port]> | bot <code or address[:port]> [command file]\n"
                    "       add --relay host[:port] to use a different co-op server, or --relay none\n");
    }

    if (s_Interactive)
        WaitForEnter();

    return s_Result;
}
