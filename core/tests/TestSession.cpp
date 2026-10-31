#include <algorithm>
#include <cmath>

#include "FakeWorld.h"
#include "SimNetwork.h"
#include "TestFramework.h"
#include "coop/Session.h"

using namespace coop;

static const SceneDesc c_Miami { "assembly:/_pro/scenes/missions/miami/scene_flamingo.entity",
    { "assembly:/_pro/scenes/missions/miami/mission_flamingo.brick" }, "flamingo" };

static const SceneDesc c_Paris { "assembly:/_pro/scenes/missions/paris/_scene_paris.entity",
    { "assembly:/_pro/scenes/missions/paris/mission_peacock.brick" }, "peacock" };

// Another mission at the same location, and a Part One location in the game's own capitalisation.
static const SceneDesc c_ParisOther { "assembly:/_pro/scenes/missions/paris/_scene_paris.entity",
    { "assembly:/_pro/scenes/missions/paris/mission_other.brick" }, "other" };

static const SceneDesc c_Sapienza { "assembly:/_PRO/Scenes/Missions/CoastalTown/_scene_mission_coastaltown.entity",
    { "assembly:/_PRO/Scenes/Missions/CoastalTown/mission_octopus.brick" }, "octopus" };

static const Endpoint c_HostEp { 0x0A000001, kDefaultPort };
static const Endpoint c_ClientEp { 0x0A000002, 50000 };

static bool HasNote(const FakeWorld& w, const std::string& needle) {
    return std::any_of(w.notes.begin(), w.notes.end(), [&](const std::string& n) { return n.find(needle) != std::string::npos; });
}

// Two players on a simulated network, stepped at 60 Hz.
struct Duo {
    explicit Duo(uint32_t p_Seed, SessionConfig p_HostCfg = {}, SessionConfig p_ClientCfg = {}) :
        net(SimNetwork::Create(p_Seed)),
        hostWorld("host"),
        clientWorld("client"),
        host(hostWorld, WithName(p_HostCfg, "Host")),
        client(clientWorld, WithName(p_ClientCfg, "Client")) {}

    static SessionConfig WithName(SessionConfig c, const char* n) {
        if (c.playerName == "Agent")
            c.playerName = n;

        return c;
    }

    void Start() {
        host.Host(net->Open(c_HostEp));
        client.Join(net->Open(c_ClientEp), c_HostEp, t);
    }

    void Step(double p_Seconds, bool p_MoveNpcs = true) {
        const double s_End = t + p_Seconds;

        while (t < s_End) {
            t += dt;
            net->SetTime(t);

            if (p_MoveNpcs) {
                hostWorld.StepNpcs(static_cast<float>(dt));
                clientWorld.StepNpcs(static_cast<float>(dt));
            }

            if (onStep) onStep(t);

            host.Tick(t, static_cast<float>(dt));
            client.Tick(t, static_cast<float>(dt));
        }
    }

    // Runs until both sides report sync, up to p_Max seconds.
    bool StepUntilSynced(double p_Max) {
        const double s_End = t + p_Max;

        while (t < s_End) {
            Step(dt);

            if (host.Synced() && client.Synced())
                return true;
        }

        return false;
    }

    void HostLoads(const SceneDesc& s, int npcs) {
        hostWorld.LoadScene(s, npcs, 0.f);
        host.OnLocalSceneLoaded();
    }

    void ClientLoads(const SceneDesc& s, int npcs, float phase) {
        clientWorld.LoadScene(s, npcs, phase);
        client.OnLocalSceneLoaded();
    }

    std::shared_ptr<SimNetwork> net;
    FakeWorld hostWorld, clientWorld;
    Session host, client;
    double t = 0.0;
    double dt = 1.0 / 60.0;
    std::function<void(double)> onStep;
};

TEST(Session_HandshakeAndFollowHostMission) {
    Duo d(10);
    d.HostLoads(c_Miami, 10);
    d.Start();
    d.Step(1.0);

    CHECK(d.host.Status().state == SessionState::Connected);
    CHECK(d.client.Status().state == SessionState::Connected);
    CHECK(d.client.Status().peerName == "Host");
    CHECK(d.host.Status().peerName == "Client");

    // Client is still in the menus: it knows where to go, nobody is synced yet.
    SceneDesc s_HostScene;
    CHECK(d.client.GetHostScene(s_HostScene) && s_HostScene.SameAs(c_Miami));
    CHECK(!d.host.Synced() && !d.client.Synced());

    d.ClientLoads(c_Miami, 10, 0.f);
    CHECK(d.StepUntilSynced(2.0));

    d.Step(0.5);
    CHECK(d.hostWorld.remoteShown && d.clientWorld.remoteShown);
    CHECK(d.hostWorld.remoteSpawns == 1 && d.clientWorld.remoteSpawns == 1);
    CHECK(HasNote(d.clientWorld, "Co-op active"));
}

TEST(Session_WrongMissionIsNotSynced) {
    Duo d(11);
    d.HostLoads(c_Paris, 4);
    d.Start();
    d.ClientLoads(c_ParisOther, 4, 0.f);
    d.Step(2.0);

    CHECK(!d.host.Synced() && !d.client.Synced());
    CHECK(!d.hostWorld.remoteShown && !d.clientWorld.remoteShown);
    CHECK(HasNote(d.clientWorld, "different mission"));
    CHECK(HasNote(d.clientWorld, "any mission in Paris"));
}

// The same mission, each PC with its own simulation quality brick (a 6-core and an 8-core CPU).
TEST(Session_MachineBricksStillSync) {
    const std::string s_Dir = "assembly:/_pro/scenes/missions/skunk/";
    const SceneDesc s_Host { s_Dir + "scene_skunk.entity", { s_Dir + "mission_skunk.brick", s_Dir + "intel_8core_skunk.brick" }, "Skunk" };
    const SceneDesc s_Client { s_Dir + "scene_skunk.entity", { s_Dir + "intel_6core_skunk.brick", s_Dir + "mission_skunk.brick" }, "Skunk" };
    Duo d(18);
    d.HostLoads(s_Host, 4);
    d.Start();
    d.ClientLoads(s_Client, 4, 0.f);
    CHECK(d.StepUntilSynced(2.0));
    CHECK(!HasNote(d.clientWorld, "different mission"));

    // Already the host's mission: a client starting it isn't sent anywhere.
    SceneDesc s_Out;
    CHECK(!d.client.RedirectFor(s_Client, s_Out));
    CHECK(LocationDisplayName(s_Host.Location()) == "Whittleton Creek");
}

TEST(Session_DifferentDifficultyIsTold) {
    SceneDesc s_Master = c_Paris;
    s_Master.bricks.push_back("assembly:/_pro/scenes/missions/paris/difficulty_pro1_paris_fashionshow.brick");
    Duo d(19);
    d.HostLoads(c_Paris, 4);
    d.Start();
    d.ClientLoads(s_Master, 4, 0.f);
    d.Step(2.0);

    CHECK(!d.host.Synced() && !d.client.Synced());
    CHECK(HasNote(d.clientWorld, "different difficulties"));
    CHECK(HasNote(d.hostWorld, "different difficulty"));
}

TEST(Scene_LocationFromScenePath) {
    CHECK(c_Miami.Location() == "miami");
    CHECK(c_Sapienza.Location() == "coastaltown");
    CHECK(c_Paris.SameLocationAs(c_ParisOther));
    CHECK(!c_Paris.SameLocationAs(c_Sapienza));
    CHECK(LocationDisplayName(c_Sapienza.Location()) == "Sapienza");
    CHECK(LocationDisplayName("colorado_2") == "Colorado");
    CHECK(LocationDisplayName("somewhere") == "somewhere");

    // Menus and unknown layouts have no location, and never count as the same location.
    const SceneDesc s_Menu { "assembly:/_pro/scenes/frontend/mainmenu.entity", {}, "" };
    const SceneDesc s_Odd { "assembly:/_pro/scenes/missions/", {}, "" };
    CHECK(s_Menu.Location().empty() && s_Odd.Location().empty());
    CHECK(!s_Menu.SameLocationAs(s_Menu));
}

// A player with only some locations (Part One) is never sent to a location they didn't start.
TEST(Session_RedirectOnlyWithinTheSameLocation) {
    Duo d(16);
    d.HostLoads(c_Paris, 2);
    d.Start();
    d.Step(1.0);

    SceneDesc s_Out;
    CHECK(d.client.RedirectFor(c_ParisOther, s_Out) && s_Out.SameAs(c_Paris));
    CHECK(!d.client.RedirectFor(c_Sapienza, s_Out));
    CHECK(!d.client.RedirectFor(c_Miami, s_Out));
    CHECK(!d.client.RedirectFor(c_Paris, s_Out)); // already the host's mission
    CHECK(!d.host.RedirectFor(c_ParisOther, s_Out)); // host never redirects
    CHECK(HasNote(d.clientWorld, "Start any mission in Paris"));
}

TEST(Session_DifferentLocationsTellBothPlayers) {
    Duo d(17);
    d.HostLoads(c_Miami, 2);
    d.Start();
    d.ClientLoads(c_Sapienza, 2, 0.f);
    d.Step(1.5);

    CHECK(!d.host.Synced() && !d.client.Synced());
    CHECK(HasNote(d.clientWorld, "You are in Sapienza and Host is in Miami"));
    CHECK(HasNote(d.hostWorld, "Client is in Sapienza and you are in Miami"));
    CHECK(d.host.Status().peerScene.SameAs(c_Sapienza));

    // Host moves to the client's location: it is told, and the client can now follow.
    d.hostWorld.notes.clear();
    d.HostLoads(c_Sapienza, 2);
    d.Step(1.0);
    CHECK(!HasNote(d.hostWorld, "different location"));

    // Client back to the menus: the host forgets where it was.
    d.clientWorld.UnloadScene();
    d.client.OnLocalSceneUnloaded();
    d.Step(0.5);
    CHECK(d.host.Status().peerScene.Empty());

    d.ClientLoads(c_Sapienza, 2, 0.f);
    CHECK(d.StepUntilSynced(2.0));
    CHECK(d.host.Status().peerScene.Empty());
}

TEST(Session_AvatarsTrackEachOther) {
    Duo d(12);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    // Host runs in a straight line at 6 m/s, client strafes.
    float s_MaxLag = 0.f;
    d.onStep = [&](double t) {
        d.hostWorld.player.pos = { static_cast<float>(6.0 * t), 0.f, 0.f };
        d.clientWorld.player.pos = { 0.f, 0.f, static_cast<float>(3.0 * t) };
        d.hostWorld.player.rot = Normalize({ 0.f, std::sin(static_cast<float>(t) * 0.5f), 0.f, std::cos(static_cast<float>(t) * 0.5f) });
    };

    for (int i = 0; i < 300; ++i) {
        d.Step(1.0 / 60.0);
        // Skip the first half second: the host teleports onto the path when the test starts moving it.
        if (i > 30 && d.clientWorld.remoteShown)
            s_MaxLag = std::max(s_MaxLag, Distance(d.clientWorld.remoteXf.pos, d.hostWorld.player.pos));
    }

    // 200 ms interpolation + 20 ms best-case latency + up to one 33 ms send interval at 6 m/s is about 1.5 m.
    CHECK_MSG(s_MaxLag < 1.7f, "max lag %.2f m", s_MaxLag);

    // Once everyone stops, the stand-ins land exactly.
    d.onStep = nullptr;
    d.Step(1.0);
    CHECK(Distance(d.clientWorld.remoteXf.pos, d.hostWorld.player.pos) < 0.01f);
    CHECK(Distance(d.hostWorld.remoteXf.pos, d.clientWorld.player.pos) < 0.01f);
    CHECK(AngleBetween(d.clientWorld.remoteXf.rot, d.hostWorld.player.rot) < 0.01f);
}

TEST(Session_PoseFollowsTheAvatar) {
    Duo d(21);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    // The host's 47 swings one bone back and forth (a walking leg, say) while moving.
    d.hostWorld.pose.rotations.assign(231, Quat {});
    d.onStep = [&](double t) {
        const float s_A = 0.6f * std::sin(static_cast<float>(t) * 6.f);
        d.hostWorld.pose.rotations[40] = Normalize({ std::sin(s_A * 0.5f), 0.f, 0.f, std::cos(s_A * 0.5f) });
        d.hostWorld.pose.pelvisPos = { 0.f, 0.f, 0.95f + 0.05f * std::sin(static_cast<float>(t) * 12.f) };
        d.hostWorld.player.pos = { static_cast<float>(1.5 * t), 0.f, 0.f };
    };

    for (int i = 0; i < 240; ++i)
        d.Step(1.0 / 60.0);

    CHECK(d.clientWorld.remotePoses > 100);
    CHECK(d.clientWorld.remotePose.rotations.size() == 231);

    // Once the host holds still, the client's copy lands on the same pose.
    d.onStep = nullptr;
    d.Step(1.0);
    CHECK(AngleBetween(d.clientWorld.remotePose.rotations[40], d.hostWorld.pose.rotations[40]) < 0.005f);
    CHECK(Distance(d.clientWorld.remotePose.pelvisPos, d.hostWorld.pose.pelvisPos) < 0.001f);

    // A world without skeletons (the client here) sends no poses.
    CHECK(d.hostWorld.remotePoses == 0);
    CHECK(d.hostWorld.remoteVictimPoses == 0);
}

TEST(Session_VictimStreamsAndReleases) {
    Duo d(22);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    // The client's 47 chokes an NPC: its pose and place reach the host while it lasts.
    d.clientWorld.victim.id = 77;
    d.clientWorld.victim.pose.rotations.assign(231, Quat {});
    d.clientWorld.victim.xf.pos = { 2.f, 1.f, 0.f };
    d.clientWorld.victim.flags = NpcFlag_Alive;

    for (int i = 0; i < 120; ++i)
        d.Step(1.0 / 60.0);

    CHECK(d.hostWorld.remoteVictimPoses > 30);
    CHECK(d.hostWorld.remoteVictim.id == 77);
    CHECK(Distance(d.hostWorld.remoteVictim.xf.pos, d.clientWorld.victim.xf.pos) < 0.001f);

    // Knocked out, then let go: the host is told to release it once.
    d.clientWorld.victim.flags = NpcFlag_Alive | NpcFlag_Pacified;
    d.Step(0.5);
    CHECK(d.hostWorld.remoteVictim.flags == (NpcFlag_Alive | NpcFlag_Pacified));
    d.clientWorld.victim.id = 0;
    d.Step(1.0);
    CHECK(d.hostWorld.remoteVictim.id == 0);
    CHECK(d.hostWorld.remoteVictimReleases == 1);
}

TEST(Session_OutfitChangesPropagate) {
    Duo d(13);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(0.5);

    CHECK(d.clientWorld.remoteOutfit == d.hostWorld.outfit);

    d.hostWorld.outfit = { "0a4ea2b1-4b4b-4e28-a5ad-d1a0c2dd2c7b", 1, 2 };
    d.Step(0.5);
    CHECK(d.clientWorld.remoteOutfit == d.hostWorld.outfit);

    d.clientWorld.outfit = { "11111111-2222-3333-4444-555555555555", 0, 3 };
    d.Step(0.5);
    CHECK(d.hostWorld.remoteOutfit == d.clientWorld.outfit);
}

TEST(Session_HostKillReplicatesToClient) {
    Duo d(14);
    d.HostLoads(c_Miami, 20);
    d.ClientLoads(c_Miami, 20, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(0.5);

    d.hostWorld.KillNpc(0x1005);
    d.Step(1.0);

    CHECK(!d.clientWorld.npcs[0x1005].Alive());
    CHECK(d.clientWorld.killCalls[0x1005] == 1);
    CHECK(d.clientWorld.npcs[0x1006].Alive());
}

TEST(Session_ClientKillIsMadeOfficialByHost) {
    Duo d(15);
    d.HostLoads(c_Miami, 20);
    d.ClientLoads(c_Miami, 20, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(0.5);

    d.clientWorld.npcs[0x1007].flags &= ~NpcFlag_Alive; // client's 47 killed it
    d.Step(1.5);

    CHECK(!d.hostWorld.npcs[0x1007].Alive());
    CHECK(d.hostWorld.killCalls[0x1007] == 1);
    CHECK(d.clientWorld.killCalls[0x1007] == 0); // already dead locally, no double kill
}

TEST(Session_NpcsAreSharedByOwner) {
    Duo d(16);
    d.HostLoads(c_Miami, 10);
    d.ClientLoads(c_Miami, 10, 2.5f); // client's NPCs start far round their circles
    d.clientWorld.player.pos = FakeWorld::Center(0x1000); // client stands in the middle of NPC 0's circle
    d.hostWorld.player.pos = { 1000.f, 0.f, 1000.f };     // host far from every NPC... but NPC 0 is nearest the client
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(5.0);

    // NPC 0 is the client's to run; every other NPC is nearer... the host is far from all of
    // them too, so ownership goes by who is nearer: the client, for all.
    CHECK(!d.clientWorld.puppets.count(0x1000));
    CHECK(d.hostWorld.puppets.count(0x1000));

    // Both games agree on every NPC, whoever runs it.
    float s_Worst = 0.f;
    for (const auto& [id, npc] : d.hostWorld.npcs)
        s_Worst = std::max(s_Worst, Distance(npc.xf.pos, d.clientWorld.npcs[id].xf.pos));

    CHECK_MSG(s_Worst < 1.0f, "worst disagreement %.2f m", s_Worst);
}

TEST(Session_NpcOwnershipFollowsThePlayers) {
    Duo d(33);
    d.HostLoads(c_Miami, 10);
    d.ClientLoads(c_Miami, 10, 0.f);
    d.hostWorld.player.pos = FakeWorld::Center(0x1000);
    d.clientWorld.player.pos = FakeWorld::Center(0x1009);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(3.0);

    // Each runs the NPCs near them; the other plays them as puppets.
    CHECK(!d.hostWorld.puppets.count(0x1000) && d.clientWorld.puppets.count(0x1000));
    CHECK(!d.clientWorld.puppets.count(0x1009) && d.hostWorld.puppets.count(0x1009));

    // The client walks over to NPC 0: it changes hands.
    d.clientWorld.player.pos = FakeWorld::Center(0x1000);
    d.hostWorld.player.pos = FakeWorld::Center(0x1009);
    d.Step(3.0);
    CHECK(d.hostWorld.puppets.count(0x1000) && !d.clientWorld.puppets.count(0x1000));
    CHECK(d.clientWorld.puppets.count(0x1009) && !d.hostWorld.puppets.count(0x1009));

    float s_Worst = 0.f;
    for (const auto& [id, npc] : d.hostWorld.npcs)
        s_Worst = std::max(s_Worst, Distance(npc.xf.pos, d.clientWorld.npcs[id].xf.pos));

    CHECK_MSG(s_Worst < 1.0f, "worst disagreement %.2f m", s_Worst);

    // Out of sync: nobody's NPCs are puppets any more.
    d.client.Leave("bye", d.t);
    d.Step(1.0);
    CHECK(d.hostWorld.puppets.empty());
}

TEST(Session_NpcsComeBackWhenThePlayersMeet) {
    Duo d(37);
    d.HostLoads(c_Miami, 10);
    d.ClientLoads(c_Miami, 10, 0.f);
    d.hostWorld.player.pos = FakeWorld::Center(0x1000);
    d.clientWorld.player.pos = FakeWorld::Center(0x1009);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(3.0);
    REQUIRE(d.hostWorld.puppets.count(0x1009));

    // The client comes over and stands beside the host: the host runs every NPC again.
    d.clientWorld.player.pos = d.hostWorld.player.pos + Vec3 { 1.f, 0.f, 0.f };
    d.Step(3.0);
    CHECK_MSG(d.hostWorld.puppets.empty(), "%zu puppets left on the host", d.hostWorld.puppets.size());
    CHECK(d.clientWorld.puppets.size() == d.clientWorld.npcs.size());
}

TEST(Session_NpcChasingTheClientGoesToTheClient) {
    Duo d(39);
    d.HostLoads(c_Miami, 10);
    d.ClientLoads(c_Miami, 10, 0.f);
    d.hostWorld.player.pos = FakeWorld::Center(0x1000);
    d.clientWorld.player.pos = d.hostWorld.player.pos + Vec3 { 1.f, 0.f, 0.f };
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(3.0);

    // Standing together: the host runs everything, the client plays puppets.
    REQUIRE(d.hostWorld.puppets.empty());
    REQUIRE(d.clientWorld.puppets.count(0x1001));

    // A guard arrests the client's 47 in the client's game: the client runs it from then on.
    d.clientWorld.alerts = { { 0x1001, 7 } };
    d.Step(2.0);
    CHECK(d.hostWorld.puppets.count(0x1001) && !d.clientWorld.puppets.count(0x1001));
    CHECK_MSG(d.hostWorld.puppets.size() == 1, "%zu puppets on the host", d.hostWorld.puppets.size());

    // Only looking around (alerted, not after them): no change for the others.
    d.clientWorld.alerts = { { 0x1001, 7 }, { 0x1002, 4 } };
    d.Step(2.0);
    CHECK(!d.hostWorld.puppets.count(0x1002));

    // It calms down: with the players together, it goes back to the host.
    d.clientWorld.alerts.clear();
    d.Step(5.0);
    CHECK(!d.hostWorld.puppets.count(0x1001) && d.clientWorld.puppets.count(0x1001));
}

TEST(Session_PauseHoldsBothGames) {
    Duo d(35);
    d.HostLoads(c_Miami, 2);
    d.ClientLoads(c_Miami, 2, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.clientWorld.paused = true;
    d.Step(0.5);
    CHECK(d.hostWorld.heldPaused);
    d.clientWorld.paused = false;
    d.Step(0.5);
    CHECK(!d.hostWorld.heldPaused);

    // Paused when the partner leaves: let go.
    d.hostWorld.paused = true;
    d.Step(0.5);
    CHECK(d.clientWorld.heldPaused);
    d.host.Leave("bye", d.t);
    d.Step(1.0);
    CHECK(!d.clientWorld.heldPaused);
}

TEST(Session_RestartReachesThePartnerInTheSameMission) {
    Duo d(36);
    d.HostLoads(c_Miami, 2);
    d.ClientLoads(c_Miami, 2, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.client.OnLocalRestart(c_Miami.scene);
    d.Step(0.5);
    CHECK(d.hostWorld.restarts == 1);

    // A restart of some other mission does nothing here.
    d.host.OnLocalRestart("assembly:/_pro/scenes/missions/paris/_scene_paris.entity");
    d.Step(0.5);
    CHECK(d.clientWorld.restarts == 0);
}

TEST(Session_NpcPosesReachTheOtherGame) {
    Duo d(34);
    d.HostLoads(c_Miami, 5);
    d.ClientLoads(c_Miami, 5, 0.f);
    d.hostWorld.player.pos = { 2.f, 0.f, 0.f };
    d.clientWorld.player.pos = { 3.f, 0.f, 0.f }; // the host is nearer NPC 0, and the client near too
    Pose s_Pose;
    s_Pose.rotations.assign(5, Quat { 0.f, 0.f, 0.f, 1.f });
    s_Pose.rotations[2] = Quat { 0.f, 0.7071f, 0.f, 0.7071f };
    d.hostWorld.npcPoses[0x1000] = s_Pose;
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(2.0);

    CHECK(std::find(d.hostWorld.watchedNpcs.begin(), d.hostWorld.watchedNpcs.end(), 0x1000) != d.hostWorld.watchedNpcs.end());
    REQUIRE(d.clientWorld.shownNpcPoses.count(0x1000));
    CHECK(std::fabs(d.clientWorld.shownNpcPoses[0x1000].rotations[2].y - 0.7071f) < 0.01f);
}

// Puppets near the partner follow closely even over a laggy, lossy link.
TEST(Session_NearNpcsStayCloseUnderLag) {
    Duo d(41);
    d.net->config = { 0.03, 0.12, 0.05, 0.0, false };
    d.HostLoads(c_Miami, 25);
    d.ClientLoads(c_Miami, 25, 0.f);
    d.hostWorld.player.pos = FakeWorld::Center(0x1006);
    d.clientWorld.player.pos = FakeWorld::Center(0x1006) + Vec3 { 1.f, 0.f, 0.f }; // together: the host runs all
    d.Start();
    REQUIRE(d.StepUntilSynced(3.0));
    d.Step(10.0);

    float s_Worst = 0.f;
    int s_Near = 0;

    for (const auto& [id, npc] : d.hostWorld.npcs) {
        if (Distance(npc.xf.pos, d.clientWorld.player.pos) > 60.f)
            continue;

        ++s_Near;
        s_Worst = std::max(s_Worst, Distance(npc.xf.pos, d.clientWorld.npcs[id].xf.pos));
    }

    CHECK(s_Near >= 5);
    // NPCs walk 1.2 m/s, played back about 0.2 s behind.
    CHECK_MSG(s_Worst < 0.6f, "worst near NPC %.2f m behind", s_Worst);
}

// NPCs far from the partner cost a fraction of what they did at the full rate.
TEST(Session_FarNpcsAreSentLessOften) {
    const auto s_Upload = [](float p_FarRange) -> double {
        SessionConfig s_Config;
        s_Config.npcFarRange = p_FarRange;
        Duo d(42, s_Config);
        d.HostLoads(c_Miami, 100);
        d.ClientLoads(c_Miami, 100, 0.f);
        d.hostWorld.player.pos = { 80.f, 0.f, 400.f };    // among the NPCs: the host runs them all
        d.clientWorld.player.pos = { 80.f, 0.f, 1400.f }; // 1 km off
        d.Start();

        if (!d.StepUntilSynced(3.0))
            return -1.0;

        d.Step(2.0);
        const auto s_Before = d.host.Status().stats.bytesSent;
        d.Step(5.0);
        return (d.host.Status().stats.bytesSent - s_Before) / 5.0;
    };

    const double s_Full = s_Upload(1e9f);
    const double s_Default = s_Upload(SessionConfig {}.npcFarRange);
    CHECK_MSG(s_Default > 0.0 && s_Default < s_Full * 0.45, "far NPCs %.0f B/s, at the full rate %.0f B/s", s_Default, s_Full);
}

TEST(Session_KnockOutsReachThePartnerOnce) {
    Duo d(43);
    d.HostLoads(c_Miami, 4);
    d.ClientLoads(c_Miami, 4, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(2.0);
    REQUIRE(d.clientWorld.puppets.count(0x1001));

    // The host's guard is knocked out (a thrown can, say): down on the client too, no longer a puppet.
    d.hostWorld.npcs[0x1001].flags |= NpcFlag_Pacified;
    d.Step(1.0);
    CHECK(d.clientWorld.knockOuts[0x1001] == 1);
    CHECK(!d.clientWorld.puppets.count(0x1001));

    // The client knocks one out itself: the host follows, and nothing echoes back.
    d.clientWorld.npcs[0x1002].flags |= NpcFlag_Pacified;
    d.Step(1.0);
    CHECK(d.hostWorld.knockOuts[0x1002] == 1);
    CHECK(d.hostWorld.knockOuts.count(0x1001) == 0);
    CHECK(d.clientWorld.knockOuts.count(0x1002) == 0);
    d.Step(2.0);
    CHECK(d.clientWorld.knockOuts[0x1001] == 1 && d.hostWorld.knockOuts[0x1002] == 1);
}

TEST(Session_ChatAndMarksReachThePartner) {
    Duo d(44);
    d.HostLoads(c_Miami, 3);
    d.ClientLoads(c_Miami, 3, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.client.SendChat("guard at the stairs");
    d.host.SendChat("Wait for me", true);
    d.host.SendMark(0x1002, { 1.f, 2.f, 3.f });
    d.Step(0.5);

    REQUIRE(d.hostWorld.chat.size() == 1 && d.clientWorld.chat.size() == 1);
    CHECK(d.hostWorld.chat[0] == "guard at the stairs");
    CHECK(d.clientWorld.chat[0] == "!Wait for me");
    REQUIRE(d.clientWorld.marks.size() == 1);
    CHECK(d.clientWorld.marks[0].first == 0x1002 && std::fabs(d.clientWorld.marks[0].second.z - 3.f) < 1e-4f);
}

// Both learn the other's mod version; the host can send the partner away, who isn't left reconnecting.
TEST(Session_VersionsKnownAndKick) {
    SessionConfig s_Cfg;
    s_Cfg.modVersion = "9.9.9";
    Duo d(51, s_Cfg, s_Cfg);
    d.HostLoads(c_Miami, 2);
    d.ClientLoads(c_Miami, 2, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    CHECK(d.client.Status().peerModVersion == "9.9.9" && d.host.Status().peerModVersion == "9.9.9");

    d.host.KickPeer("the host wants to play alone", d.t);
    d.Step(1.0);
    CHECK(d.host.Status().state == SessionState::Listening);
    CHECK(d.client.Status().state == SessionState::Failed && !d.client.Status().lostConnection);
    CHECK(HasNote(d.clientWorld, "the host wants to play alone"));
    CHECK(HasNote(d.hostWorld, "You removed Client"));
}

// What a player is up to reaches the other soon after it changes, and is forgotten once unheard.
TEST(Session_PlayerStateReachesThePartner) {
    Duo d(46);
    d.HostLoads(c_Miami, 2);
    d.Start();
    d.Step(1.0); // connected, the client still in the menus: the state goes anyway

    MsgPlayerState s_State;
    s_State.flags = PlayerFlag_Trespassing;
    s_State.threat = Threat_Hunted;
    s_State.menu = 4;
    s_State.stats.kills = 2;
    s_State.stats.targets = 1;
    d.host.SetLocalPlayerState(s_State);
    d.Step(0.3);

    REQUIRE(d.client.Status().peerStateKnown);
    CHECK(d.client.Status().peerState == s_State);
    CHECK(d.client.Status().peerSilence < 0.2);

    s_State.threat = Threat_Calm;
    d.host.SetLocalPlayerState(s_State);
    d.Step(0.3);
    CHECK(d.client.Status().peerState.threat == Threat_Calm);

    // Unheard for a few seconds: not known any more (the connection itself lasts longer).
    d.net->config.cut = true;
    d.Step(4.0);
    CHECK(!d.client.Status().peerStateKnown);
    CHECK(d.client.Status().peerSilence > 3.5);
    CHECK(d.client.Status().state == SessionState::Connected);
}

// A partner's game sending chat (or marks) in a flood gets a few through, then one a second.
TEST(Session_FloodsAreCapped) {
    Duo d(47);
    d.HostLoads(c_Miami, 2);
    d.ClientLoads(c_Miami, 2, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    for (int i = 0; i < 100; ++i)
        d.client.SendChat("spam " + std::to_string(i));

    d.Step(2.0);
    CHECK(d.hostWorld.chat.size() >= 8 && d.hostWorld.chat.size() <= 12);
    CHECK(HasNote(d.hostWorld, "Too many messages"));
    CHECK(d.host.Status().floodDropped >= 85);

    // A while later, a line comes through as usual.
    d.Step(10.0);
    const size_t s_Before = d.hostWorld.chat.size();
    d.client.SendChat("are you there?");
    d.Step(0.5);
    REQUIRE(d.hostWorld.chat.size() == s_Before + 1);
    CHECK(d.hostWorld.chat.back() == "are you there?");
}

TEST(Session_MarkKindsReachThePartner) {
    Duo d(48);
    d.HostLoads(c_Miami, 3);
    d.ClientLoads(c_Miami, 3, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.host.SendMark(0x1001, { 1.f, 2.f, 3.f }, MarkKind_Danger);
    d.Step(0.3);
    d.host.SendMark(0x1001, { 1.f, 2.f, 3.f }, MarkKind_Remove);
    d.Step(0.3);

    REQUIRE(d.clientWorld.markKinds.size() == 2);
    CHECK(d.clientWorld.markKinds[0] == MarkKind_Danger && d.clientWorld.markKinds[1] == MarkKind_Remove);

    // Taking marks back is never held back as a flood.
    for (int i = 0; i < 20; ++i)
        d.host.SendMark(0x1002, { 0.f, 0.f, 0.f }, MarkKind_Remove);

    d.Step(0.5);
    CHECK(d.clientWorld.markKinds.size() == 22);
}

TEST(Session_LoadingStageReachesThePartner) {
    Duo d(49);
    d.HostLoads(c_Paris, 2);
    d.ClientLoads(c_Paris, 2, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.client.NotifyBusy(d.t, 60, LoadStage_Preparing, c_Paris.scene);
    d.Step(0.3);
    auto s_Status = d.host.Status();
    CHECK(s_Status.peerBusy && s_Status.peerBusyStage == LoadStage_Preparing && s_Status.peerBusyScene == c_Paris.scene);

    d.client.NotifyBusy(d.t, 0);
    d.Step(0.3);
    s_Status = d.host.Status();
    CHECK(!s_Status.peerBusy && s_Status.peerBusyStage == LoadStage_None && s_Status.peerBusyScene.empty());
}

// Packet loss is measured from the partner's packets that never came.
TEST(Session_LossIsMeasured) {
    Duo d(50);
    d.HostLoads(c_Miami, 4);
    d.ClientLoads(c_Miami, 4, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(5.0);
    CHECK(d.client.Status().stats.lossPercent < 1.0);

    d.net->config.loss = 0.2;
    d.Step(15.0);
    const double s_Loss = d.client.Status().stats.lossPercent;
    CHECK(s_Loss > 12.0 && s_Loss < 28.0);
    CHECK(d.client.Status().stats.jitterMs >= 0.0);
}

// A partner whose game said it is loading is waited for, past the usual timeout.
TEST(Session_LoadingPartnerIsWaitedFor) {
    Duo d(45);
    d.HostLoads(c_Miami, 2);
    d.ClientLoads(c_Miami, 2, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.client.NotifyBusy(d.t, 60);
    d.Step(0.3);
    CHECK(d.host.Status().peerBusy);
    // The client's game stalls 20 s (no ticks at all), well past the 10 s timeout.
    for (const double s_End = d.t + 20.0; d.t < s_End;) {
        d.t += d.dt;
        d.net->SetTime(d.t);
        d.host.Tick(d.t, static_cast<float>(d.dt));
    }

    CHECK(d.host.Status().state == SessionState::Connected);
    d.client.OnLocalSceneLoaded(); // done: busy no more
    d.Step(2.0);
    CHECK(d.host.Status().state == SessionState::Connected && !d.host.Status().peerBusy);

    // Without a word, the same silence is a timeout.
    d.net->config.cut = true;
    d.Step(12.0);
    CHECK(d.host.Status().state == SessionState::Listening);
    CHECK(d.client.Status().lostConnection);
}

// The client's connection drops and it joins again from a new address: the host takes it back
// without waiting out the old one's timeout.
TEST(Session_ReconnectTakesTheOldPlace) {
    Duo d(46);
    d.HostLoads(c_Miami, 2);
    d.Start();
    d.Step(1.0);
    REQUIRE(d.host.Status().state == SessionState::Connected);

    d.net->config.cut = true;
    d.Step(4.0);
    d.net->config.cut = false;
    REQUIRE(d.host.Status().state == SessionState::Connected); // still holding the old address

    FakeWorld s_World("again");
    Session s_Again(s_World, Duo::WithName({}, "Client"));
    s_Again.Join(d.net->Open({ 0x0A000002, 50001 }), c_HostEp, d.t);

    for (int i = 0; i < 120; ++i) {
        d.Step(d.dt);
        s_Again.Tick(d.t, static_cast<float>(d.dt));
    }

    CHECK(s_Again.Status().state == SessionState::Connected);
    CHECK(d.host.Status().state == SessionState::Connected);
    CHECK(HasNote(d.hostWorld, "lost connection"));
}

// Playback lag follows the link: small on a steady one, larger when packets come unevenly.
TEST(Session_PlaybackDelayFollowsTheLink) {
    const auto s_Delay = [](double p_Min, double p_Max) -> double {
        Duo d(47);
        d.net->config = { p_Min, p_Max, 0.0, 0.0, false };
        d.HostLoads(c_Miami, 1);
        d.ClientLoads(c_Miami, 1, 0.f);
        d.Start();

        if (!d.StepUntilSynced(3.0))
            return -1.0;

        for (int i = 0; i < 600; ++i) {
            d.hostWorld.player.pos.x += 0.05f;
            d.Step(d.dt);
        }

        return d.client.Status().playbackDelayMs;
    };

    const double s_Steady = s_Delay(0.02, 0.025);
    const double s_Jittery = s_Delay(0.03, 0.2);
    CHECK_MSG(s_Steady < 100.0, "steady link: %.0f ms", s_Steady);
    CHECK_MSG(s_Jittery > 150.0, "jittery link: %.0f ms", s_Jittery);
}

TEST(Session_SlowLinkSendsLessNpcDetail) {
    Duo d(48);
    d.HostLoads(c_Miami, 3);
    d.ClientLoads(c_Miami, 3, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(3.0);
    CHECK(!d.host.Status().congested);

    d.net->config.minLatency = d.net->config.maxLatency = 0.4; // the round trip jumps from ~80 to 800 ms
    d.Step(4.0);
    CHECK(d.host.Status().congested);
    CHECK(HasNote(d.hostWorld, "struggling"));

    d.net->config.minLatency = 0.02;
    d.net->config.maxLatency = 0.06;
    d.Step(12.0);
    CHECK(!d.host.Status().congested);
}

// A burst (a mission's first moments send many reliable messages at once) isn't a struggling link.
TEST(Session_BurstIsNotCongestion) {
    Duo d(50);
    d.HostLoads(c_Miami, 3);
    d.ClientLoads(c_Miami, 3, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(2.0);

    // (Taken-back marks: reliable, and never held back as a flood, unlike chat.)
    for (int i = 0; i < 150; ++i)
        d.host.SendMark(0x1000 + i, { 0.f, 0.f, 0.f }, MarkKind_Remove);

    // And the partner's game hitches for 0.6 s (spawning, streaming in textures): one late ack.
    for (const double s_End = d.t + 0.6; d.t < s_End;) {
        d.t += d.dt;
        d.net->SetTime(d.t);
        d.host.Tick(d.t, static_cast<float>(d.dt));
        CHECK(!d.host.Status().congested);
    }

    for (int i = 0; i < 240; ++i) {
        d.Step(d.dt);
        CHECK(!d.host.Status().congested);
    }

    CHECK(!HasNote(d.hostWorld, "struggling"));
    CHECK(d.clientWorld.markKinds.size() == 150);
}

// An NPC standing still isn't streamed over and over, and still keeps its pose there.
TEST(Session_StillNpcPosesAreNotResent) {
    Duo d(49);
    d.HostLoads(c_Miami, 1);
    d.ClientLoads(c_Miami, 1, 0.f);
    d.hostWorld.player.pos = FakeWorld::Center(0x1000);
    d.clientWorld.player.pos = FakeWorld::Center(0x1000) + Vec3 { 1.f, 0.f, 0.f };
    Pose s_Pose;
    s_Pose.rotations.assign(200, Quat {});
    d.hostWorld.npcPoses[0x1000] = s_Pose;
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(2.0, false);
    const auto s_Before = d.host.Status().stats.bytesSent;
    d.Step(6.0, false);
    const double s_Rate = (d.host.Status().stats.bytesSent - s_Before) / 6.0;

    // 12 a second would be ~10 KB/s; one every ~0.45 s is about 2 KB/s.
    CHECK_MSG(s_Rate < 4000.0, "host upload %.0f B/s", s_Rate);
    CHECK(d.clientWorld.shownNpcPoses.count(0x1000));
}

TEST(Session_IdleNpcsCostLittleBandwidth) {
    Duo d(17);
    d.HostLoads(c_Miami, 200);
    d.ClientLoads(c_Miami, 200, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.Step(3.0, false); // nobody moves
    const auto s_Before = d.host.Status().stats.bytesSent;
    d.Step(10.0, false);
    const double s_Rate = (d.host.Status().stats.bytesSent - s_Before) / 10.0;

    // Only the 2-second refresh: ~200 * 29 B / 2 s + overhead.
    CHECK_MSG(s_Rate < 6000.0, "idle host upload %.0f B/s", s_Rate);
}

TEST(Session_NewMissionStartsNewEpoch) {
    Duo d(18);
    d.HostLoads(c_Miami, 5);
    d.ClientLoads(c_Miami, 5, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    const uint32_t s_FirstEpoch = d.host.Status().epoch;

    d.hostWorld.UnloadScene();
    d.host.OnLocalSceneUnloaded();
    d.Step(0.5);
    CHECK(!d.client.Synced());
    CHECK(!d.clientWorld.remoteShown);

    d.HostLoads(c_Paris, 5);
    d.Step(0.5);
    SceneDesc s_HostScene;
    CHECK(d.client.GetHostScene(s_HostScene) && s_HostScene.SameAs(c_Paris));
    CHECK(!d.client.Synced()); // client is still in Miami

    d.clientWorld.UnloadScene();
    d.client.OnLocalSceneUnloaded();
    d.ClientLoads(c_Paris, 5, 0.f);
    CHECK(d.StepUntilSynced(2.0));
    CHECK(d.host.Status().epoch == s_FirstEpoch + 1);
    CHECK(d.client.Status().epoch == s_FirstEpoch + 1);

    d.Step(0.5);
    CHECK(d.clientWorld.remoteShown && d.hostWorld.remoteShown);
}

TEST(Session_RestartSameMissionResyncs) {
    Duo d(19);
    d.HostLoads(c_Miami, 5);
    d.ClientLoads(c_Miami, 5, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.hostWorld.KillNpc(0x1001);
    d.Step(1.0);
    CHECK(!d.clientWorld.npcs[0x1001].Alive());

    // Both restart the mission; deaths from the old run must not leak into the new one.
    d.HostLoads(c_Miami, 5);
    d.ClientLoads(c_Miami, 5, 0.f);
    CHECK(d.StepUntilSynced(3.0));
    d.Step(1.0);
    CHECK(d.clientWorld.npcs[0x1001].Alive());
    CHECK(d.hostWorld.npcs[0x1001].Alive());
}

TEST(Session_LeaveIsSeenByHost) {
    Duo d(20);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));
    d.Step(0.5);
    CHECK(d.hostWorld.remoteShown);

    d.client.Leave("Quit to menu", d.t);
    d.Step(1.0);

    CHECK(d.host.Status().state == SessionState::Listening);
    CHECK(!d.hostWorld.remoteShown);
    CHECK(HasNote(d.hostWorld, "left"));

    // A friend can join again afterwards.
    d.client.Join(d.net->Open({ 0x0A000002, 50001 }), c_HostEp, d.t);
    CHECK(d.StepUntilSynced(3.0));
}

TEST(Session_HostQuitEndsClientSession) {
    Duo d(21);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.host.Leave("Host quit", d.t);
    d.Step(1.0);
    CHECK(d.client.Status().state == SessionState::Failed);
    CHECK(!d.clientWorld.remoteShown);
    CHECK(HasNote(d.clientWorld, "ended the session"));
}

TEST(Session_TimeoutWhenLinkDies) {
    Duo d(22);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 0.f);
    d.Start();
    REQUIRE(d.StepUntilSynced(2.0));

    d.net->config.cut = true;
    d.Step(d.host.Config().timeout + 1.0);

    CHECK(d.host.Status().state == SessionState::Listening);
    CHECK(d.client.Status().state == SessionState::Failed);
    CHECK(!d.hostWorld.remoteShown && !d.clientWorld.remoteShown);
}

TEST(Session_ConnectTimeoutHasHelpfulMessage) {
    Duo d(23);
    d.client.Join(d.net->Open(c_ClientEp), c_HostEp, d.t); // nobody hosting
    d.Step(d.client.Config().connectTimeout + 1.0);
    CHECK(d.client.Status().state == SessionState::Failed);
    CHECK(d.client.Status().message.find("forwarded UDP port") != std::string::npos);
}

TEST(Session_ThirdPlayerIsTurnedAway) {
    Duo d(24);
    d.HostLoads(c_Miami, 0);
    d.Start();
    d.Step(1.0);

    FakeWorld s_ThirdWorld("third");
    Session s_Third(s_ThirdWorld, SessionConfig {});
    s_Third.Join(d.net->Open({ 0x0A000003, 50000 }), c_HostEp, d.t);

    for (int i = 0; i < 120; ++i) {
        d.Step(1.0 / 60.0);
        s_Third.Tick(d.t, 1.f / 60.f);
    }

    CHECK(s_Third.Status().state == SessionState::Failed);
    CHECK(s_Third.Status().message.find("two players") != std::string::npos);
    CHECK(d.host.Status().peerName == "Client");
}

// The host listens on IP and the relay at once; a friend through the relay plays exactly like one over IP.
TEST(Session_HostOverIpAndRelayAtOnce) {
    auto s_Net = SimNetwork::Create(28);
    const Endpoint s_HostRelay = Endpoint::Relay(1);
    const Endpoint s_FriendRelay = Endpoint::Relay(2);

    FakeWorld s_HostWorld("host"), s_FriendWorld("friend"), s_ThirdWorld("third");
    Session s_Host(s_HostWorld, SessionConfig {}), s_Friend(s_FriendWorld, SessionConfig {}), s_Third(s_ThirdWorld, SessionConfig {});

    s_HostWorld.LoadScene(c_Paris, 3, 0.f);
    s_Host.Host(std::make_unique<MultiSocket>(s_Net->Open(c_HostEp), s_Net->Open(s_HostRelay)));
    s_Friend.Join(s_Net->Open(s_FriendRelay), s_HostRelay, 0.0);

    double t = 0.0;
    auto Step = [&](double p_Seconds) {
        for (const double s_End = t + p_Seconds; t < s_End;) {
            t += 1.0 / 60.0;
            s_Net->SetTime(t);
            s_Host.Tick(t, 1.f / 60.f);
            s_Friend.Tick(t, 1.f / 60.f);
            s_Third.Tick(t, 1.f / 60.f);
        }
    };

    Step(1.0);
    SceneDesc s_Scene;
    REQUIRE(s_Friend.GetHostScene(s_Scene));
    s_FriendWorld.LoadScene(s_Scene, 3, 0.f);
    s_Friend.OnLocalSceneLoaded();
    Step(1.0);

    CHECK(s_Host.Synced() && s_Friend.Synced());
    CHECK(s_Host.Status().peerAddress == s_FriendRelay.ToString());
    CHECK(s_HostWorld.remoteShown && s_FriendWorld.remoteShown);

    // Someone joining by IP while the relayed friend is in is turned away.
    s_Third.Join(s_Net->Open(c_ClientEp), c_HostEp, t);
    Step(2.0);
    CHECK(s_Third.Status().message.find("two players") != std::string::npos);
    CHECK(s_Host.Synced());
}

TEST(Session_ProtocolMismatchIsRejected) {
    auto s_Net = SimNetwork::Create(25);
    FakeWorld s_HostWorld("host");
    Session s_Host(s_HostWorld, SessionConfig {});
    s_Host.Host(s_Net->Open(c_HostEp));

    // A future client speaking protocol 999.
    auto s_Sock = s_Net->Open(c_ClientEp);
    Connection s_Conn(c_HostEp);
    MsgHello s_Hello;
    s_Hello.protocol = 999;
    s_Hello.playerName = "Future";
    s_Conn.SendReliable(MsgType::Hello, Encode(s_Hello));

    bool s_GotReject = false;

    for (double t = 0; t < 2.0 && !s_GotReject; t += 1.0 / 60.0) {
        s_Net->SetTime(t);
        std::vector<std::vector<uint8_t>> s_Out;
        s_Conn.Flush(t, s_Out);
        for (auto& p : s_Out) s_Sock->SendTo(c_HostEp, p.data(), p.size());

        s_Host.Tick(t, 1.f / 60.f);

        uint8_t s_Buf[2048];
        Endpoint s_From;
        int n;
        while ((n = s_Sock->RecvFrom(s_From, s_Buf, sizeof(s_Buf))) >= 0) {
            s_Conn.Receive(s_Buf, static_cast<size_t>(n), t);
            Message m;
            while (s_Conn.PopMessage(m)) {
                MsgReject r;
                if (m.type == MsgType::Reject && Decode(m.payload, r) && r.reason.find("protocol") != std::string::npos)
                    s_GotReject = true;
            }
        }
    }

    CHECK(s_GotReject);
    CHECK(s_Host.Status().state == SessionState::Listening);
}

TEST(Session_GameVersionPolicy) {
    {
        SessionConfig h, c;
        h.gameVersion = "3.240.1";
        c.gameVersion = "3.230.0";
        Duo d(26, h, c);
        d.HostLoads(c_Miami, 0);
        d.Start();
        d.Step(1.0);
        CHECK(d.client.Status().state == SessionState::Connected); // allowed by default, with a warning
        CHECK(HasNote(d.clientWorld, "Game versions differ"));
    }
    {
        SessionConfig h, c;
        h.gameVersion = "3.240.1";
        h.allowGameVersionMismatch = false;
        c.gameVersion = "3.230.0";
        Duo d(27, h, c);
        d.Start();
        d.Step(1.0);
        CHECK(d.client.Status().state == SessionState::Failed);
        CHECK(d.client.Status().message.find("Game version mismatch") != std::string::npos);
    }
}

TEST(Session_MissionEventsReachPeer) {
    Duo d(28);
    d.HostLoads(c_Miami, 0);
    d.Start();
    d.Step(1.0);
    d.host.SendMissionEvent("ContractEnd", "{\"Value\":{}}");
    d.Step(0.5);
    REQUIRE(!d.clientWorld.peerEvents.empty());
    CHECK(d.clientWorld.peerEvents.back() == "ContractEnd:{\"Value\":{}}");
}

TEST(Session_ItemChangesReachPeer) {
    Duo d(30);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 1.0f);
    d.Start();
    REQUIRE(d.StepUntilSynced(5.0));

    d.clientWorld.itemChanges.push_back({ 0x42, ItemFlag_Taken, {} });
    d.Step(0.5);
    REQUIRE(d.hostWorld.peerItems.size() == 1);
    CHECK(d.hostWorld.peerItems[0].id == 0x42 && d.hostWorld.peerItems[0].Taken());

    ItemChange s_Dropped { 0x42, 0, {} };
    s_Dropped.xf.pos = { 3.f, 4.f, 0.5f };
    d.hostWorld.itemChanges.push_back(s_Dropped);
    d.Step(0.5);
    REQUIRE(d.clientWorld.peerItems.size() == 1);
    CHECK(!d.clientWorld.peerItems[0].Taken());
    CHECK(Distance(d.clientWorld.peerItems[0].xf.pos, s_Dropped.xf.pos) < 0.01f);
}

TEST(Session_SkeletonSentOnceAndOnChange) {
    Duo d(32);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 1.0f);
    d.clientWorld.skeleton = { -1, 0, 1, 1, 0 };
    d.Start();
    REQUIRE(d.StepUntilSynced(5.0));
    d.Step(1.0);
    CHECK(d.hostWorld.remoteSkeleton == d.clientWorld.skeleton);
    CHECK(d.hostWorld.remoteSkeletons == 1); // not again while it is the same

    // A disguise with another skeleton.
    d.clientWorld.skeleton = { -1, 0, 1, 2, 0, 4 };
    d.Step(1.0);
    CHECK(d.hostWorld.remoteSkeleton == d.clientWorld.skeleton);
    CHECK(d.hostWorld.remoteSkeletons == 2);
}

TEST(Session_AlertsReachPeerBothWays) {
    Duo d(31);
    d.HostLoads(c_Miami, 0);
    d.ClientLoads(c_Miami, 0, 1.0f);
    d.Start();
    REQUIRE(d.StepUntilSynced(5.0));

    d.clientWorld.alerts = { { 0x77, 5 }, { 0x78, 3 } };
    d.hostWorld.alerts = { { 0x91, 8 } };
    d.Step(0.6);
    REQUIRE(d.hostWorld.peerAlerts.size() == 2);
    CHECK(d.hostWorld.peerAlerts[0].id == 0x77 && d.hostWorld.peerAlerts[0].tension == 5);
    REQUIRE(d.clientWorld.peerAlerts.size() == 1);
    CHECK(d.clientWorld.peerAlerts[0].tension == 8);

    // Calmed down there: the next set is empty, and so is ours of theirs.
    d.clientWorld.alerts.clear();
    d.Step(0.6);
    CHECK(d.hostWorld.peerAlerts.empty());
}

// Long run over a bad connection: everything must still converge.
TEST(Session_ChaosSoak) {
    Duo d(29);
    d.net->config = { 0.03, 0.25, 0.2, 0.05, false };
    d.HostLoads(c_Miami, 60);
    d.ClientLoads(c_Miami, 60, 1.0f);
    d.clientWorld.player.pos = { -500.f, 0.f, -500.f }; // away from every NPC
    d.Start();
    REQUIRE(d.StepUntilSynced(5.0));

    // Host and client each kill a batch of NPCs over a minute.
    for (int i = 0; i < 30; ++i) {
        d.Step(2.0);
        const uint64_t s_Id = 0x1000 + static_cast<uint64_t>(i * 2);

        if (i % 2 == 0)
            d.hostWorld.KillNpc(s_Id);
        else
            d.clientWorld.npcs[s_Id].flags &= ~NpcFlag_Alive;

        d.hostWorld.player.pos = { static_cast<float>(i), 0.f, 0.f };
    }

    d.Step(5.0);

    int s_Mismatched = 0;
    float s_WorstDrift = 0.f;

    for (const auto& [id, npc] : d.hostWorld.npcs) {
        const auto& s_Client = d.clientWorld.npcs[id];

        if (npc.Alive() != s_Client.Alive())
            ++s_Mismatched;
        else if (npc.Alive())
            s_WorstDrift = std::max(s_WorstDrift, Distance(npc.xf.pos, s_Client.xf.pos));
    }

    CHECK_MSG(s_Mismatched == 0, "%d NPCs disagree on alive/dead", s_Mismatched);
    // Every NPC is over 60 m from the client, so sent at a quarter of the rate (see the near test).
    CHECK_MSG(s_WorstDrift < 2.5f, "worst NPC drift %.2f m", s_WorstDrift);
    CHECK(Distance(d.clientWorld.remoteXf.pos, d.hostWorld.player.pos) < 0.05f);
    CHECK(d.host.Synced() && d.client.Synced());

    for (const auto& [id, n] : d.hostWorld.killCalls)
        CHECK_MSG(n == 1, "host killed %llx %d times", static_cast<unsigned long long>(id), n);

    for (const auto& [id, n] : d.clientWorld.killCalls)
        CHECK_MSG(n == 1, "client killed %llx %d times", static_cast<unsigned long long>(id), n);
}

// Same flow over real Winsock UDP sockets on 127.0.0.1.
TEST(Session_RealUdpLoopback) {
    std::string s_Error;
    auto s_HostSock = UdpSocket::Open(0, s_Error);
    REQUIRE(s_HostSock);
    const uint16_t s_Port = s_HostSock->LocalPort();

    auto s_ClientSock = UdpSocket::Open(0, s_Error);
    REQUIRE(s_ClientSock);

    Endpoint s_HostEp;
    REQUIRE(ResolveEndpoint("127.0.0.1:" + std::to_string(s_Port), kDefaultPort, s_HostEp, s_Error));
    CHECK(s_HostEp.ip == 0x7F000001 && s_HostEp.port == s_Port);

    FakeWorld s_HostWorld("host"), s_ClientWorld("client");
    Session s_Host(s_HostWorld, SessionConfig {}), s_Client(s_ClientWorld, SessionConfig {});

    s_HostWorld.LoadScene(c_Miami, 5, 0.f);
    s_ClientWorld.LoadScene(c_Miami, 5, 0.f);
    s_Host.Host(std::move(s_HostSock));
    s_Client.Join(std::move(s_ClientSock), s_HostEp, 0.0);

    bool s_Synced = false;
    for (double t = 0.0; t < 3.0 && !s_Synced; t += 1.0 / 60.0) {
        s_Host.Tick(t, 1.f / 60.f);
        s_Client.Tick(t, 1.f / 60.f);
        s_Synced = s_Host.Synced() && s_Client.Synced();
    }

    CHECK(s_Synced);

    s_HostWorld.player.pos = { 12.f, 3.f, -4.f };
    for (double t = 3.0; t < 4.0; t += 1.0 / 60.0) {
        s_Host.Tick(t, 1.f / 60.f);
        s_Client.Tick(t, 1.f / 60.f);
    }

    CHECK(s_ClientWorld.remoteShown && Distance(s_ClientWorld.remoteXf.pos, s_HostWorld.player.pos) < 0.01f);
}

TEST(Socket_ResolveRejectsBadInput) {
    Endpoint e;
    std::string err;
    CHECK(!ResolveEndpoint("", kDefaultPort, e, err));
    CHECK(!ResolveEndpoint("1.2.3.4:99999", kDefaultPort, e, err));
    CHECK(!ResolveEndpoint("1.2.3.4:abc", kDefaultPort, e, err));
    CHECK(ResolveEndpoint(" 10.0.0.5 ", kDefaultPort, e, err) && e.ip == 0x0A000005 && e.port == kDefaultPort);
}
