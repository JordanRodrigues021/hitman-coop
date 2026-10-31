#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Connection.h"
#include "Interpolation.h"
#include "Protocol.h"
#include "Socket.h"

namespace coop {

// Everything the session needs from the game. The real implementation talks to ZHMModSDK;
// the tests use a fake world, so the sync logic runs and is tested without the game.
class IGameWorld {
public:
    virtual ~IGameWorld() = default;

    // True once the local 47 exists in a loaded mission scene.
    virtual bool IsInScene() const = 0;
    virtual SceneDesc CurrentScene() const = 0;

    virtual bool GetLocalAvatar(Transform& p_Out) const = 0;
    virtual bool GetLocalOutfit(OutfitDesc& p_Out) const = 0;

    // Spawns the partner's stand-in on first call, then moves it.
    virtual void ShowRemoteAvatar(const Transform& p_Xf) = 0;
    virtual void HideRemoteAvatar() = 0;
    virtual void SetRemoteOutfit(const OutfitDesc& p_Outfit) = 0;

    // Skeleton pose of the local 47 / for the partner's stand-in. Optional: a world without
    // skeletons (the connection test) keeps the defaults.
    virtual bool GetLocalPose(Pose& /*p_Out*/) { return false; }
    // Our 47's skeleton (each bone's parent), the layout GetLocalPose is in; false if not known.
    virtual bool GetLocalSkeleton(std::vector<int16_t>& /*p_Out*/) { return false; }
    // The partner's, whenever it changes (empty: not known).
    virtual void SetRemoteSkeleton(const std::vector<int16_t>& /*p_Parents*/) {}
    virtual void ShowRemotePose(const Pose& /*p_Pose*/) {}

    // An NPC the local 47 is choking, killing or dragging (flags are NpcFlag_*), and the partner's
    // one shown here. Release comes once the partner's stream stops.
    virtual bool GetLocalVictim(uint64_t& /*p_Id*/, Transform& /*p_Xf*/, Pose& /*p_Pose*/, uint8_t& /*p_Flags*/) { return false; }
    virtual void ShowRemoteVictim(uint64_t /*p_Id*/, const Transform& /*p_Xf*/, const Pose& /*p_Pose*/, uint8_t /*p_Flags*/) {}
    virtual void ReleaseRemoteVictim() {}

    // Mission NPCs only; the partner's stand-in must not be listed.
    virtual void ListNpcs(std::vector<NpcState>& p_Out) const = 0;
    virtual void SetNpcTransform(uint64_t p_Id, const Transform& p_Xf) = 0;
    virtual void KillNpc(uint64_t p_Id) = 0;

    // Shared NPCs. A puppet is an NPC the partner's game runs: its AI is off here and it is
    // placed and posed from their stream; released, its AI runs here again (we own it now).
    virtual void SetNpcPuppet(uint64_t /*p_Id*/, bool /*p_Puppet*/) {}
    virtual void ShowNpcPose(uint64_t /*p_Id*/, const Pose& /*p_Pose*/) {}
    virtual void ClearNpcPose(uint64_t /*p_Id*/) {}
    // NPCs we own whose skeleton poses we send (near the partner): recorded from now on, and
    // their latest pose read back. Optional.
    virtual void WatchNpcPoses(const std::vector<uint64_t>& /*p_Ids*/) {}
    virtual bool GetNpcPose(uint64_t /*p_Id*/, Pose& /*p_Out*/) { return false; }

    virtual void OnPeerMissionEvent(const std::string& p_Name, const std::string& p_Json) = 0;

    // Scene items our player took or put back since the last call (see ItemChange).
    virtual void CollectItemChanges(std::vector<ItemChange>& p_Out) { (void)p_Out; }
    // The partner took one (hide it here) or put it back (show it there).
    virtual void ApplyPeerItem(const ItemChange& p_Change) { (void)p_Change; }
    // Out of sync (the partner left, restarted, went elsewhere): what they held is back here, and
    // what we hold gets reported again next time.
    virtual void ResetItems() {}
    // NPCs alert in our game by its own doing (not alerts we copied from the partner).
    virtual void CollectAlerts(std::vector<NpcAlert>& p_Out) { (void)p_Out; }
    // The partner's: the full set, replacing the last (empty: none, or the link is gone).
    virtual void ApplyPeerAlerts(const std::vector<NpcAlert>& p_Alerts) { (void)p_Alerts; }
    virtual void Notify(const std::string& p_Text) = 0;

    // Our player has paused (a pause menu is open); the partner has, so hold ours paused too.
    virtual bool IsLocallyPaused() const { return false; }
    virtual void SetPartnerPaused(bool /*p_Paused*/) {}
    // The partner restarted the mission we are both in: restart ours the way the pause menu does.
    virtual void RestartMission() {}

    // An NPC knocked out in the partner's game (by any means): knock ours out too.
    virtual void KnockOutNpc(uint64_t /*p_Id*/) {}
    // The partner's chat line or quick message, and an NPC (id) or place (id 0) they marked
    // (p_Kind: MarkKind_*, MarkKind_Remove when they took it back).
    virtual void OnPeerChat(const std::string& /*p_Text*/, bool /*p_Quick*/) {}
    virtual void OnPeerMark(uint64_t /*p_Id*/, const Vec3& /*p_Pos*/, uint8_t /*p_Kind*/) {}
};

struct SessionConfig {
    std::string playerName = "Agent";
    std::string modVersion = "0.13.0";
    std::string gameVersion = "unknown";

    double connectTimeout = 10.0;
    double timeout = 10.0;
    double avatarInterval = 1.0 / 30.0;
    double alertInterval = 0.2;
    double npcInterval = 1.0 / 15.0;
    double ownerCheckInterval = 0.5;
    float ownerHysteresis = 4.f;     // metres nearer the other player before an NPC changes hands
    double npcPoseInterval = 1.0 / 12.0;
    float npcPoseRange = 35.f;       // poses are sent for owned NPCs this near the other player
    size_t npcPoseMax = 10;          // the nearest this many
    float puppetNearRange = 60.f;    // puppets nearer the local player are placed every frame
    double npcScanRate = 30.0;      // local NPC scans per second (deaths, drift correction)
    double interpolationDelay = 0.2; // 0.1 froze and jumped over the internet (Tailscale) whenever packets bunched up
    // Playback delay follows the link between these: about 70 ms on a LAN, more when packets bunch up.
    bool adaptiveDelay = true;
    double interpolationMin = 0.08;
    double interpolationMax = 0.35;
    float npcFarRange = 60.f;        // owned NPCs farther than this from the partner are sent at a quarter of the rate
    size_t npcPoseFullRate = 4;      // the nearest this many poses at the full rate, the rest at half
    // Waiting this long, at most, for a partner whose game said it is loading.
    double busyGraceMax = 180.0;

    // Different game builds can still play together; NPCs whose ids differ simply don't sync.
    bool allowGameVersionMismatch = true;

    NpcCorrectionPolicy npcPolicy;
};

enum class SessionRole { None, Host, Client };
enum class SessionState { Idle, Listening, Connecting, Connected, Failed };

const char* SessionStateName(SessionState p_State);

struct SessionStatus {
    SessionRole role = SessionRole::None;
    SessionState state = SessionState::Idle;
    std::string message;
    std::string peerName;
    std::string peerGameVersion;
    std::string peerModVersion;
    std::string peerAddress;
    bool synced = false;
    uint32_t epoch = 0;
    SceneDesc hostScene;
    SceneDesc peerScene; // host: the mission the client last said it is in, if it isn't ours
    size_t npcsTracked = 0;
    uint64_t posesReceived = 0;
    // Shared NPCs: how many each game runs, and the last ownership check (host).
    size_t npcsPuppeted = 0;
    size_t npcsOwnedByClient = 0;
    std::string ownerCheck;
    ConnectionStats stats;
    bool peerBusy = false;       // the partner's game is loading
    bool peerPaused = false;     // the partner has their pause menu open
    bool congested = false;      // the link is struggling: NPC detail is sent less often
    bool lostConnection = false; // failed by a timeout or overload (worth reconnecting), not refused or ended
    double playbackDelayMs = 0.0;
    double sendKbps = 0.0;
    // What the partner's loading is at (LoadStage_*), and the scene, while peerBusy.
    uint8_t peerBusyStage = LoadStage_None;
    std::string peerBusyScene;
    // What the partner is up to (their alert level, menu, mission stats), if heard lately.
    bool peerStateKnown = false;
    MsgPlayerState peerState;
    // Seconds since the partner was last heard from (connected only).
    double peerSilence = 0.0;
    uint32_t floodDropped = 0; // chat lines, marks and events of theirs dropped as a flood
};

class Session {
public:
    Session(IGameWorld& p_World, SessionConfig p_Config);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Host on an already-bound socket.
    void Host(std::unique_ptr<IDatagramSocket> p_Socket);
    void Join(std::unique_ptr<IDatagramSocket> p_Socket, const Endpoint& p_Host, double p_Now);
    void Leave(const std::string& p_Reason, double p_Now);
    // Host: sends the partner away (they are told why) and waits for someone else.
    void KickPeer(const std::string& p_Reason, double p_Now);

    void Tick(double p_Now, float p_Dt);

    // The game calls these when a mission finishes loading and when it is torn down.
    void OnLocalSceneLoaded();
    void OnLocalSceneUnloaded();

    void SendMissionEvent(const std::string& p_Name, const std::string& p_Json);
    void SendChat(const std::string& p_Text, bool p_Quick = false);
    // An NPC (id) or a place (id 0) our player marked for the partner (or took back: MarkKind_Remove).
    void SendMark(uint64_t p_Id, const Vec3& p_Pos, uint8_t p_Kind = MarkKind_Normal);
    // Our game is about to stall for up to p_Seconds (a load), or is done (0): the partner waits.
    // With what the load is at, and the scene it loads if known.
    void NotifyBusy(double p_Now, uint16_t p_Seconds, uint8_t p_Stage = LoadStage_Loading, const std::string& p_Scene = {});
    // What our player is up to, sent to the partner a few times a second (and soon after it changes).
    void SetLocalPlayerState(const MsgPlayerState& p_State) { m_LocalState = p_State; }
    // Our player restarted p_Scene (not one restarted for the partner): theirs restarts too.
    void OnLocalRestart(const std::string& p_Scene);

    // Client only: the mission the host is in.
    bool GetHostScene(SceneDesc& p_Out) const;

    // Client only: when the client starts p_Loading, should it load the host's mission instead?
    // Only within the same location: players can each own different locations (for example
    // one has the full game and the other only Part One), and nobody is sent somewhere they
    // didn't start themselves.
    bool RedirectFor(const SceneDesc& p_Loading, SceneDesc& p_Out) const;

    SessionStatus Status() const;

    // Client only: the host's NPCs as last received (for tools such as the test bot).
    void GetHostNpcs(std::vector<NpcState>& p_Out) const;
    bool Active() const { return m_Role != SessionRole::None; }
    bool Synced() const { return IsSynced(); }

    SessionConfig& Config() { return m_Config; }

private:
    void ReceivePackets(double p_Now);
    void HandleMessage(const Message& p_Msg, double p_Now);
    void HandleHello(const MsgHello& p_Hello, double p_Now);
    void RejectStranger(const Endpoint& p_From, const std::string& p_Reason);

    void UpdateScene();
    void UpdateAvatar(double p_Now);
    void UpdateOutfit();
    void UpdateSkeleton(double p_Now);
    void UpdateNpcsHost(double p_Now);
    void UpdateNpcsClient(float p_Dt);
    void UpdateItems();
    // Shared NPCs: ownership (host), sending ours, playing the partner's.
    void UpdateSharedNpcs(double p_Now);
    void SetOwners(const std::vector<NpcOwnership>& p_Changes);
    void ReconcilePuppets();
    void ReleasePuppets();
    void UpdatePuppets(double p_Now);
    uint8_t MyOwner() const { return m_Role == SessionRole::Client ? NpcOwner_Client : NpcOwner_Host; }
    uint8_t OwnerOf(uint64_t p_Id) const {
        const auto s_It = m_NpcOwner.find(p_Id);
        return s_It == m_NpcOwner.end() ? static_cast<uint8_t>(NpcOwner_Host) : s_It->second;
    }
    void UpdateAlerts(double p_Now);
    void UpdatePause();
    bool m_SentPause = false;
    bool m_PartnerPaused = false;

    // Every NPC, listed once a tick at most (each list walks every actor in the level).
    const std::vector<NpcState>& Npcs();
    std::vector<NpcState> m_NpcCache;
    bool m_NpcCacheValid = false;

    // The link: congestion (less NPC detail while it lasts) and the bandwidth we use.
    void UpdateLink(double p_Now);
    double RateScale() const { return m_Congested ? 2.0 : 1.0; }
    bool m_Congested = false;
    int m_StruggleChecks = 0;
    double m_CongestedSeen = -100.0;
    double m_NextLinkCheck = 0.0;
    double m_MinRtt = 1e9;
    uint64_t m_LinkBytes = 0;
    double m_LinkBytesAt = 0.0;
    double m_SendKbps = 0.0;
    double m_CongestionToldAt = -1000.0;

    // Our player's state for the partner, and theirs.
    void UpdatePlayerState(double p_Now);
    MsgPlayerState m_LocalState;
    MsgPlayerState m_LocalStateSent;
    double m_NextStateSend = 0.0;
    double m_LastStateSend = -100.0;
    MsgPlayerState m_PeerState;
    double m_PeerStateAt = -1.0;

    // A partner (or a stranger's game) flooding us: chat, marks and events past a sane rate are dropped.
    struct RateGate {
        double tokens = -1.0;
        double last = 0.0;

        bool Take(double p_Now, double p_PerSecond, double p_Burst) {
            if (tokens < 0.0)
                tokens = p_Burst;

            tokens = std::min(p_Burst, tokens + std::max(0.0, p_Now - last) * p_PerSecond);
            last = p_Now;

            if (tokens < 1.0)
                return false;

            tokens -= 1.0;
            return true;
        }
    };
    bool Admit(RateGate& p_Gate, double p_Now, double p_PerSecond, double p_Burst);
    RateGate m_ChatGate, m_MarkGate, m_EventGate;
    uint32_t m_FloodDropped = 0;
    double m_FloodToldAt = -100.0;

    // Stalls: the partner's announced load, and our own frame gaps (packets waited unread meanwhile).
    double m_PeerBusyUntil = 0.0;
    uint8_t m_PeerBusyStage = LoadStage_None;
    std::string m_PeerBusyScene;
    double m_LastTick = -1.0;
    double m_StallGraceUntil = 0.0;
    bool m_LostConnection = false;

    // NPCs known knocked out in either game (announced once).
    std::unordered_set<uint64_t> m_KnownDown;
    struct SentPose {
        Pose pose;
        double time = 0.0;
    };
    std::unordered_map<uint64_t, SentPose> m_PoseSent; // the last pose sent of each NPC we run
    uint32_t m_PoseRound = 0;
    // A transform stream and its pose stream played back on one delay, the one the worse needs.
    void PlayPair(SnapshotBuffer& p_Xf, PoseBuffer& p_Pose);

    void SendSceneInfo();
    void SendSceneReady(uint32_t p_Epoch);
    void SendClientScene();
    void OnSyncChanged(bool p_Synced);

    void DropPeer(const std::string& p_Reason);
    void Fail(const std::string& p_Reason);
    void ResetSceneState();
    void FlushTo(double p_Now);

    bool IsSynced() const;
    uint32_t SyncedEpoch() const;

    IGameWorld& m_World;
    SessionConfig m_Config;

    std::unique_ptr<IDatagramSocket> m_Socket;
    std::unique_ptr<Connection> m_Conn;

    SessionRole m_Role = SessionRole::None;
    SessionState m_State = SessionState::Idle;
    std::string m_Message;
    std::string m_PeerName;
    std::string m_PeerGameVersion;
    std::string m_PeerModVersion;
    double m_ConnectStart = 0.0;
    double m_PeerAcceptedAt = 0.0;
    bool m_PeerHandshaken = false;

    // Scene. The host owns the epoch counter; every mission load gets a new epoch so
    // late packets from a previous mission can never touch the current one.
    bool m_LocalInScene = false;
    uint32_t m_Epoch = 0;           // host: current scene epoch; client: host's epoch
    SceneDesc m_HostScene;          // client: host's mission (empty when host is in menus)
    uint32_t m_PeerReadyEpoch = 0;  // host: epoch the client reported ready for
    uint32_t m_LocalReadyEpoch = 0; // client: epoch we told the host we are ready for
    bool m_WasSynced = false;
    bool m_ToldAboutMismatch = false;
    SceneDesc m_PeerScene;          // host: where the client says it is when that isn't our mission

    // Avatar
    double m_NextAvatarSend = 0.0;
    SnapshotBuffer m_RemoteAvatar;
    PoseBuffer m_RemotePose;
    Pose m_PoseScratch;
    uint64_t m_PosesReceived = 0;
    SnapshotBuffer m_VictimXf;
    PoseBuffer m_VictimPose;
    uint64_t m_VictimId = 0;
    uint8_t m_VictimFlags = 0;
    double m_VictimLastReceive = -100.0;
    bool m_VictimShown = false;
    Transform m_VictimXfScratch;
    Pose m_VictimPoseScratch;
    MsgVictimPose m_VictimSend;

    void ResetVictim();
    bool m_RemoteAvatarShown = false;
    bool m_HasLocalOutfitSent = false;
    OutfitDesc m_LastSentOutfit;
    bool m_HasRemoteOutfit = false;
    std::vector<int16_t> m_LastSentSkeleton;
    bool m_HasLocalSkeletonSent = false;
    double m_NextSkeletonCheck = 0.0;
    std::vector<int16_t> m_SkeletonScratch;
    OutfitDesc m_RemoteOutfit;

    // NPCs
    uint32_t m_NpcTick = 0;
    double m_NextNpcSend = 0.0;
    double m_NextNpcScan = 0.0;
    float m_NpcScanDt = 0.f;
    std::unordered_map<uint64_t, bool> m_HostNpcAlive;      // host: last seen alive state
    std::unordered_map<uint64_t, NpcState> m_HostNpcs;      // client: latest host state
    std::unordered_set<uint64_t> m_HostDead;                // client: deaths announced by the host
    std::unordered_map<uint64_t, bool> m_ClientNpcAlive;    // client: last seen local alive state
    std::unordered_set<uint64_t> m_KillRequested;           // client: kill requests already sent
    std::unordered_set<uint64_t> m_KillApplied;             // client: host deaths already applied locally

    struct SentNpc {
        Transform xf;
        double time;
    };

    std::unordered_map<uint64_t, SentNpc> m_NpcSent;        // what the partner last got of the NPCs we run

    std::vector<NpcState> m_NpcScratch;

    // Shared NPCs
    std::unordered_map<uint64_t, uint8_t> m_NpcOwner; // both: who runs each NPC (absent: the host)
    struct Puppet {
        SnapshotBuffer xf { 0.2 };
        PoseBuffer pose { 0.2 };
        double lastPose = -100.0;
        bool posed = false;
    };
    std::unordered_map<uint64_t, Puppet> m_Puppets;   // NPCs the partner runs, as they stream them
    std::unordered_set<uint64_t> m_Puppeted;          // NPCs the world has as puppets now
    Transform m_RemoteAvatarLast;
    bool m_HaveRemoteAvatar = false;
    double m_NextOwnerCheck = 0.0;
    double m_NextNpcPoseSend = 0.0;
    uint32_t m_PuppetFrame = 0;
    std::vector<uint64_t> m_PoseIds;
    MsgVictimPose m_NpcPoseSend;
    Pose m_NpcPoseScratch;
    std::vector<NpcState> m_SharedScratch;
    std::string m_OwnerCheck;
    std::vector<ItemChange> m_ItemScratch;
    MsgAlerts m_AlertSend;
    double m_NextAlertSend = 0.0;
    std::vector<std::vector<uint8_t>> m_OutScratch;
};

} // namespace coop
