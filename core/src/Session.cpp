#include "coop/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <limits>

namespace coop {

// NPCs that haven't moved are still resent this often, so a lost snapshot can't leave one stale forever.
static constexpr double c_NpcRefreshInterval = 2.0;
static constexpr float c_NpcMoveThreshold = 0.05f;
static constexpr float c_NpcTurnThreshold = 0.035f; // about 2 degrees
static constexpr double c_HandshakeTimeout = 5.0;
// A newcomer may take the place of a partner silent this long (they are likely the same player
// reconnecting, whose old address the host still holds).
static constexpr double c_ReplaceSilence = 2.5;
// A frame gap this long is our own stall: the partner isn't timed out for what we didn't read.
static constexpr double c_StallGap = 1.5;
static constexpr double c_PoseHold = 1.5; // a puppet keeps its last pose this long without a new one

// Two poses alike to the eye (each bone within about half a degree, root and pelvis within 1 cm).
static bool PosesAlike(const Pose& p_A, const Pose& p_B) {
    if (p_A.rotations.size() != p_B.rotations.size() || Distance(p_A.rootPos, p_B.rootPos) > 0.01f ||
        Distance(p_A.pelvisPos, p_B.pelvisPos) > 0.01f)
        return false;

    for (size_t i = 0; i < p_A.rotations.size(); ++i)
        if (std::fabs(Dot(p_A.rotations[i], p_B.rotations[i])) < 0.99999f)
            return false;

    return true;
}

// Tools (the connection test, the bot) send no game version, so they never trigger the warning.
static bool VersionsDiffer(const std::string& p_A, const std::string& p_B) {
    return !p_A.empty() && !p_B.empty() && p_A != p_B;
}

const char* SessionStateName(SessionState p_State) {
    switch (p_State) {
        case SessionState::Idle: return "Idle";
        case SessionState::Listening: return "Hosting, waiting for a friend";
        case SessionState::Connecting: return "Connecting";
        case SessionState::Connected: return "Connected";
        case SessionState::Failed: return "Failed";
        default: return "Unknown";
    }
}

Session::Session(IGameWorld& p_World, SessionConfig p_Config) :
    m_World(p_World),
    m_Config(std::move(p_Config)),
    m_RemoteAvatar(m_Config.interpolationDelay),
    m_RemotePose(m_Config.interpolationDelay, 16),
    m_VictimXf(m_Config.interpolationDelay),
    m_VictimPose(m_Config.interpolationDelay, 16) {}

void Session::ResetVictim() {
    if (m_VictimShown)
        m_World.ReleaseRemoteVictim();

    m_VictimShown = false;
    m_VictimId = 0;
    m_VictimXf.Clear();
    m_VictimPose.Clear();
}

Session::~Session() = default;

void Session::Host(std::unique_ptr<IDatagramSocket> p_Socket) {
    m_Socket = std::move(p_Socket);
    m_Conn.reset();
    m_Role = SessionRole::Host;
    m_State = SessionState::Listening;
    m_Message = "Hosting. Waiting for a friend to join.";
    m_PeerHandshaken = false;
    m_PeerReadyEpoch = 0;

    // The host may already be in a mission when it starts hosting.
    m_LocalInScene = m_World.IsInScene();

    if (m_LocalInScene && ++m_Epoch == 0)
        ++m_Epoch;
}

void Session::Join(std::unique_ptr<IDatagramSocket> p_Socket, const Endpoint& p_Host, double p_Now) {
    m_Socket = std::move(p_Socket);
    m_Conn = std::make_unique<Connection>(p_Host);
    m_Role = SessionRole::Client;
    m_State = SessionState::Connecting;
    m_Message = "Connecting to " + p_Host.ToString() + "...";
    m_ConnectStart = p_Now;
    m_LostConnection = false;
    m_PeerStateAt = -1.0;
    m_PeerBusyUntil = 0.0;
    m_Epoch = 0;
    m_HostScene = {};
    m_LocalReadyEpoch = 0;
    m_LocalInScene = m_World.IsInScene();

    MsgHello s_Hello;
    s_Hello.modVersion = m_Config.modVersion;
    s_Hello.gameVersion = m_Config.gameVersion;
    s_Hello.playerName = m_Config.playerName;
    m_Conn->SendReliable(MsgType::Hello, Encode(s_Hello));

    FlushTo(p_Now);
}

void Session::Leave(const std::string& p_Reason, double p_Now) {
    if (m_Conn) {
        MsgReject s_Bye { p_Reason };
        m_Conn->SendReliable(MsgType::Disconnect, Encode(s_Bye));
        FlushTo(p_Now);
    }

    ResetSceneState();
    m_Conn.reset();
    m_Socket.reset();
    m_Role = SessionRole::None;
    m_State = SessionState::Idle;
    m_Message = p_Reason;
    m_LostConnection = false;
    m_PeerStateAt = -1.0;
    m_PeerName.clear();
    m_PeerGameVersion.clear();
    m_PeerHandshaken = false;
    m_Epoch = 0;
    m_HostScene = {};
    m_PeerReadyEpoch = 0;
    m_LocalReadyEpoch = 0;
    m_HasRemoteOutfit = false;
}

void Session::KickPeer(const std::string& p_Reason, double p_Now) {
    if (m_Role != SessionRole::Host || !m_Conn || !m_PeerHandshaken)
        return;

    const std::string s_Name = m_PeerName;
    m_Conn->SendReliable(MsgType::Disconnect, Encode(MsgReject { p_Reason }));
    FlushTo(p_Now);
    DropPeer("You removed " + s_Name + ".");
}

void Session::Fail(const std::string& p_Reason) {
    ResetSceneState();
    m_Conn.reset();
    m_Socket.reset();
    m_Role = SessionRole::None;
    m_State = SessionState::Failed;
    m_Message = p_Reason;
    m_PeerStateAt = -1.0;
    m_PeerHandshaken = false;
    m_HasRemoteOutfit = false;
    m_World.Notify(p_Reason);
}

void Session::DropPeer(const std::string& p_Reason) {
    ResetSceneState();
    m_PeerScene = {};
    m_Conn.reset();
    m_State = SessionState::Listening;
    m_Message = p_Reason + " Waiting for a friend to join.";
    m_PeerStateAt = -1.0;
    m_PeerBusyUntil = 0.0;
    m_PeerName.clear();
    m_PeerGameVersion.clear();
    m_PeerHandshaken = false;
    m_PeerReadyEpoch = 0;
    m_HasRemoteOutfit = false;
    m_World.Notify(p_Reason);
}

void Session::ResetSceneState() {
    if (m_WasSynced)
        OnSyncChanged(false);

    m_WasSynced = false;
    m_RemoteAvatar.Clear();
    m_RemotePose.Clear();
    ResetVictim();
    m_HostNpcAlive.clear();
    m_HostNpcs.clear();
    m_HostDead.clear();
    m_ClientNpcAlive.clear();
    m_KillRequested.clear();
    m_KillApplied.clear();
    m_NpcSent.clear();
    m_KnownDown.clear();
    m_PoseSent.clear();
    m_NpcCacheValid = false;
    m_ToldAboutMismatch = false;
}

void Session::OnLocalSceneLoaded() {
    ResetSceneState();
    m_LocalInScene = true;

    if (m_Role == SessionRole::Host) {
        ++m_Epoch;

        if (m_Epoch == 0)
            ++m_Epoch;

        SendSceneInfo();

        if (!m_PeerScene.Empty() && !m_PeerScene.SameLocationAs(m_World.CurrentScene())) {
            m_World.Notify(m_PeerName + " is in " + LocationDisplayName(m_PeerScene.Location()) +
                ", a different location. Pick a location you both own.");
        }
    }
    else if (m_Role == SessionRole::Client) {
        m_LocalReadyEpoch = 0;
    }

    // Our load is done: the partner stops waiting.
    if (m_Conn && m_State == SessionState::Connected)
        m_Conn->SendReliable(MsgType::Busy, Encode(MsgBusy { 0 }));
}

void Session::OnLocalSceneUnloaded() {
    ResetSceneState();
    m_LocalInScene = false;

    if (m_Role == SessionRole::Host) {
        SendSceneInfo();
    }
    else if (m_Role == SessionRole::Client) {
        if (m_LocalReadyEpoch != 0) {
            SendSceneReady(0);
            m_LocalReadyEpoch = 0;
        }

        SendClientScene(); // back in the menus
    }
}

void Session::SendMissionEvent(const std::string& p_Name, const std::string& p_Json) {
    if (m_State != SessionState::Connected || !m_Conn)
        return;

    MsgMissionEvent s_Event { p_Name, p_Json.size() > kMaxEventJson ? p_Json.substr(0, kMaxEventJson) : p_Json };
    m_Conn->SendReliable(MsgType::MissionEvent, Encode(s_Event));
}

void Session::SendChat(const std::string& p_Text, bool p_Quick) {
    if (m_State != SessionState::Connected || !m_Conn || p_Text.empty())
        return;

    m_Conn->SendReliable(MsgType::Chat, Encode(MsgChat { static_cast<uint8_t>(p_Quick ? 1 : 0), p_Text.substr(0, kMaxChatLen) }));
}

void Session::SendMark(uint64_t p_Id, const Vec3& p_Pos, uint8_t p_Kind) {
    if (IsSynced())
        m_Conn->SendReliable(MsgType::Mark, Encode(MsgMark { m_Epoch, p_Id, p_Pos, p_Kind }));
}

void Session::NotifyBusy(double p_Now, uint16_t p_Seconds, uint8_t p_Stage, const std::string& p_Scene) {
    if (m_State != SessionState::Connected || !m_Conn)
        return;

    MsgBusy s_Busy { p_Seconds, p_Seconds == 0 ? static_cast<uint8_t>(LoadStage_None) : p_Stage, p_Seconds == 0 ? std::string() : p_Scene.substr(0, 256) };
    m_Conn->SendReliable(MsgType::Busy, Encode(s_Busy));
    FlushTo(p_Now); // now: the stall may come before the next tick
}

bool Session::Admit(RateGate& p_Gate, double p_Now, double p_PerSecond, double p_Burst) {
    if (p_Gate.Take(p_Now, p_PerSecond, p_Burst))
        return true;

    ++m_FloodDropped;

    if (p_Now - m_FloodToldAt > 30.0) {
        m_FloodToldAt = p_Now;
        m_World.Notify("Too many messages from " + (m_PeerName.empty() ? std::string("your partner") : m_PeerName) +
            " at once: some were left out.");
    }

    return false;
}

void Session::UpdatePlayerState(double p_Now) {
    if (!m_Conn)
        return;

    // Twice a second, and soon after a change (unreliable: a lost one is replaced by the next).
    const bool s_Changed = !(m_LocalState == m_LocalStateSent);

    if (p_Now < m_NextStateSend && !(s_Changed && p_Now - m_LastStateSend > 0.15))
        return;

    m_Conn->SendUnreliable(MsgType::PlayerState, Encode(m_LocalState));
    m_LocalStateSent = m_LocalState;
    m_LastStateSend = p_Now;
    m_NextStateSend = p_Now + 0.5;
}

bool Session::GetHostScene(SceneDesc& p_Out) const {
    if (m_Role != SessionRole::Client || m_State != SessionState::Connected || m_HostScene.Empty())
        return false;

    p_Out = m_HostScene;
    return true;
}

void Session::GetHostNpcs(std::vector<NpcState>& p_Out) const {
    for (const auto& [s_Id, s_Npc] : m_HostNpcs) {
        NpcState s_State = s_Npc;

        if (m_HostDead.count(s_Id))
            s_State.flags &= ~NpcFlag_Alive;

        p_Out.push_back(s_State);
    }
}

bool Session::RedirectFor(const SceneDesc& p_Loading, SceneDesc& p_Out) const {
    SceneDesc s_Host;

    if (!GetHostScene(s_Host) || s_Host.SameAs(p_Loading) || !s_Host.SameLocationAs(p_Loading))
        return false;

    p_Out = s_Host;
    return true;
}

SessionStatus Session::Status() const {
    SessionStatus s_Status;
    s_Status.role = m_Role;
    s_Status.state = m_State;
    s_Status.message = m_Message;
    s_Status.peerName = m_PeerName;
    s_Status.peerGameVersion = m_PeerGameVersion;
    s_Status.peerModVersion = m_PeerModVersion;
    s_Status.synced = IsSynced();
    s_Status.epoch = m_Epoch;
    s_Status.hostScene = m_Role == SessionRole::Client ? m_HostScene : SceneDesc {};
    s_Status.peerScene = m_Role == SessionRole::Host ? m_PeerScene : SceneDesc {};
    s_Status.npcsTracked = m_Role == SessionRole::Host ? m_HostNpcAlive.size() : m_HostNpcs.size();
    s_Status.posesReceived = m_PosesReceived;

    if (m_Conn) {
        s_Status.stats = m_Conn->Stats();
        s_Status.peerAddress = m_Conn->Remote().ToString();
    }

    s_Status.npcsPuppeted = m_Puppeted.size();
    s_Status.ownerCheck = m_OwnerCheck;

    for (const auto& [s_Id, s_Owner] : m_NpcOwner)
        s_Status.npcsOwnedByClient += s_Owner == NpcOwner_Client ? 1 : 0;

    s_Status.peerBusy = m_PeerBusyUntil > m_LastTick;
    s_Status.peerPaused = m_PartnerPaused;
    s_Status.congested = m_Congested;
    s_Status.lostConnection = m_LostConnection;
    s_Status.playbackDelayMs = m_RemoteAvatar.Delay() * 1000.0;
    s_Status.sendKbps = m_SendKbps;

    if (s_Status.peerBusy) {
        s_Status.peerBusyStage = m_PeerBusyStage;
        s_Status.peerBusyScene = m_PeerBusyScene;
    }

    s_Status.peerStateKnown = m_State == SessionState::Connected && m_PeerStateAt >= 0.0 && m_LastTick - m_PeerStateAt < 3.0;

    if (s_Status.peerStateKnown)
        s_Status.peerState = m_PeerState;

    if (m_Conn && m_State == SessionState::Connected && m_Conn->HasReceived())
        s_Status.peerSilence = std::max(0.0, m_LastTick - m_Conn->LastReceiveTime());

    s_Status.floodDropped = m_FloodDropped;
    return s_Status;
}

bool Session::IsSynced() const {
    if (m_State != SessionState::Connected || !m_LocalInScene || m_Epoch == 0 || !m_World.IsInScene())
        return false;

    if (m_Role == SessionRole::Host)
        return m_PeerReadyEpoch == m_Epoch;

    return !m_HostScene.Empty() && m_LocalReadyEpoch == m_Epoch;
}

uint32_t Session::SyncedEpoch() const {
    return IsSynced() ? m_Epoch : 0;
}

void Session::Tick(double p_Now, float p_Dt) {
    if (m_Role == SessionRole::None)
        return;

    // A long gap since the last tick is our own stall (a load): what the partner sent meanwhile is
    // only being read now, so their silence isn't counted until it has been.
    if (m_LastTick >= 0.0 && p_Now - m_LastTick > c_StallGap)
        m_StallGraceUntil = p_Now + 3.0;

    m_LastTick = p_Now;
    m_NpcCacheValid = false;
    ReceivePackets(p_Now);

    if (m_Role == SessionRole::None)
        return;

    if (m_Role == SessionRole::Client && m_State == SessionState::Connecting &&
        p_Now - m_ConnectStart > m_Config.connectTimeout) {
        if (m_Conn && m_Conn->Remote().IsRelay())
            Fail("No answer from the host through the co-op server. Check the join code, and that the host's game is still hosting.");
        else
            Fail("No answer from the host. Check the address, and that the host forwarded UDP port " +
                std::to_string(m_Conn ? m_Conn->Remote().port : kDefaultPort) + " or that you share a VPN like Tailscale.");

        m_LostConnection = true; // no answer, not a refusal: worth another try if this host was reachable before
        return;
    }

    if (m_Conn) {
        const bool s_Handshaken = m_Role == SessionRole::Client || m_PeerHandshaken;
        const double s_Silence = p_Now - m_Conn->LastReceiveTime();

        if (m_Role == SessionRole::Host && !m_PeerHandshaken && p_Now - m_PeerAcceptedAt > c_HandshakeTimeout) {
            DropPeer("A connection attempt did not complete.");
        }
        else if (m_Conn->Overflowed()) {
            if (m_Role == SessionRole::Host)
                DropPeer("Connection to your friend was overloaded.");
            else {
                Fail("Connection to the host was overloaded.");
                m_LostConnection = true;
            }

            return;
        }
        else if (s_Handshaken && m_State == SessionState::Connected && m_Conn->HasReceived() &&
            s_Silence > m_Config.timeout && p_Now >= m_StallGraceUntil && p_Now >= m_PeerBusyUntil) {
            if (m_Role == SessionRole::Host)
                DropPeer(m_PeerName + " timed out.");
            else {
                Fail("Lost connection to the host.");
                m_LostConnection = true;
            }

            return;
        }
    }

    if (m_State == SessionState::Connected) {
        UpdateLink(p_Now);
        UpdatePlayerState(p_Now);
        UpdateScene();
        UpdateOutfit();
        UpdateSkeleton(p_Now);
        UpdateAvatar(p_Now);
        UpdateItems();
        UpdateAlerts(p_Now);
        UpdateSharedNpcs(p_Now);
        UpdatePause();

        // Walking every actor in the level is the most expensive thing the mod does, so it runs at
        // npcScanRate instead of every frame. The client's blend uses the time since the last scan.
        m_NpcScanDt += p_Dt;

        if (p_Now >= m_NextNpcScan) {
            m_NextNpcScan = std::max(m_NextNpcScan + 1.0 / m_Config.npcScanRate, p_Now);

            if (m_Role == SessionRole::Host)
                UpdateNpcsHost(p_Now);
            else
                UpdateNpcsClient(std::min(m_NpcScanDt, 0.25f));

            m_NpcScanDt = 0.f;
        }
    }

    FlushTo(p_Now);
}

void Session::FlushTo(double p_Now) {
    if (!m_Conn || !m_Socket)
        return;

    m_OutScratch.clear();
    m_Conn->Flush(p_Now, m_OutScratch);

    for (const auto& s_Datagram : m_OutScratch)
        m_Socket->SendTo(m_Conn->Remote(), s_Datagram.data(), s_Datagram.size());
}

void Session::ReceivePackets(double p_Now) {
    uint8_t s_Buffer[2048];
    Endpoint s_From;

    for (int s_Guard = 0; s_Guard < 512 && m_Socket; ++s_Guard) {
        const int s_Size = m_Socket->RecvFrom(s_From, s_Buffer, sizeof(s_Buffer));

        if (s_Size < 0)
            break;

        if (!Connection::IsPacket(s_Buffer, static_cast<size_t>(s_Size)))
            continue;

        if (m_Role == SessionRole::Host) {
            if (!m_Conn) {
                m_Conn = std::make_unique<Connection>(s_From);
                m_PeerAcceptedAt = p_Now;
                m_PeerHandshaken = false;
            }
            else if (s_From != m_Conn->Remote()) {
                // Our partner, silent a while, is most likely this newcomer reconnecting from a new
                // address: make room (a partner who said they are loading keeps their place).
                if (p_Now - m_Conn->LastReceiveTime() > c_ReplaceSilence && p_Now >= m_PeerBusyUntil) {
                    DropPeer((m_PeerName.empty() ? std::string("Your friend") : m_PeerName) + " lost connection.");
                    m_Conn = std::make_unique<Connection>(s_From);
                    m_PeerAcceptedAt = p_Now;
                    m_PeerHandshaken = false;
                }
                else {
                    RejectStranger(s_From, "This session already has two players.");
                    continue;
                }
            }
        }
        else if (!m_Conn || s_From != m_Conn->Remote()) {
            continue;
        }

        if (!m_Conn->Receive(s_Buffer, static_cast<size_t>(s_Size), p_Now))
            continue;

        Message s_Msg;

        while (m_Conn && m_Conn->PopMessage(s_Msg))
            HandleMessage(s_Msg, p_Now);
    }
}

void Session::RejectStranger(const Endpoint& p_From, const std::string& p_Reason) {
    Connection s_Temp(p_From);
    s_Temp.SendReliable(MsgType::Reject, Encode(MsgReject { p_Reason }));

    std::vector<std::vector<uint8_t>> s_Out;
    s_Temp.Flush(0.0, s_Out);

    for (const auto& s_Datagram : s_Out)
        m_Socket->SendTo(p_From, s_Datagram.data(), s_Datagram.size());
}

void Session::HandleMessage(const Message& p_Msg, double p_Now) {
    // Until the handshake is done, only handshake messages count.
    if (m_Role == SessionRole::Host && !m_PeerHandshaken && p_Msg.type != MsgType::Hello)
        return;

    if (m_Role == SessionRole::Client && m_State == SessionState::Connecting && p_Msg.type != MsgType::Welcome &&
        p_Msg.type != MsgType::Reject)
        return;

    switch (p_Msg.type) {
        case MsgType::Hello: {
            MsgHello s_Hello;

            if (m_Role == SessionRole::Host && Decode(p_Msg.payload, s_Hello))
                HandleHello(s_Hello, p_Now);

            break;
        }

        case MsgType::Welcome: {
            MsgWelcome s_Welcome;

            if (m_Role != SessionRole::Client || m_State != SessionState::Connecting || !Decode(p_Msg.payload, s_Welcome))
                break;

            m_State = SessionState::Connected;
            m_PeerName = s_Welcome.hostName;
            m_PeerGameVersion = s_Welcome.gameVersion;
            m_PeerModVersion = s_Welcome.modVersion;
            m_Message = "Connected to " + m_PeerName + ".";
            m_World.Notify("Connected to " + m_PeerName + ".");

            if (VersionsDiffer(s_Welcome.gameVersion, m_Config.gameVersion)) {
                m_World.Notify("Game versions differ (host " + s_Welcome.gameVersion + ", you " + m_Config.gameVersion +
                    "). Some NPCs may not sync.");
            }

            break;
        }

        case MsgType::Reject: {
            MsgReject s_Reject;

            if (m_Role == SessionRole::Client && Decode(p_Msg.payload, s_Reject))
                Fail("The host refused the connection: " + s_Reject.reason);

            break;
        }

        case MsgType::Disconnect: {
            MsgReject s_Bye;
            Decode(p_Msg.payload, s_Bye);

            if (m_Role == SessionRole::Host)
                DropPeer(m_PeerName + " left (" + s_Bye.reason + ").");
            else
                Fail("The host ended the session (" + s_Bye.reason + ").");

            break;
        }

        case MsgType::SceneInfo: {
            MsgSceneInfo s_Info;

            if (!Decode(p_Msg.payload, s_Info))
                break;

            if (m_Role == SessionRole::Host) {
                // The client is somewhere other than our mission.
                const bool s_Changed = !s_Info.scene.SameAs(m_PeerScene);
                m_PeerScene = s_Info.scene;

                if (s_Changed && !m_PeerScene.Empty() && m_LocalInScene) {
                    const SceneDesc s_Mine = m_World.CurrentScene();

                    if (!m_PeerScene.SameLocationAs(s_Mine)) {
                        m_World.Notify(m_PeerName + " is in " + LocationDisplayName(m_PeerScene.Location()) + " and you are in " +
                            LocationDisplayName(s_Mine.Location()) + ". Pick a location you both own.");
                    }
                    else if (m_PeerScene.DiffersInDifficulty(s_Mine)) {
                        m_World.Notify(m_PeerName + " started this mission on a different difficulty. They have to start it again on yours.");
                    }
                }

                break;
            }

            if (m_Role != SessionRole::Client)
                break;

            if (s_Info.epoch != m_Epoch || !s_Info.scene.SameAs(m_HostScene)) {
                ResetSceneState();
                m_Epoch = s_Info.epoch;
                m_HostScene = s_Info.scene;

                if (m_LocalReadyEpoch != 0) {
                    SendSceneReady(0);
                    m_LocalReadyEpoch = 0;
                }

                if (m_HostScene.Empty()) {
                    m_World.Notify(m_PeerName + " is in the menus.");
                }
                else {
                    const std::string s_Where = LocationDisplayName(m_HostScene.Location());
                    m_World.Notify(m_PeerName + " started a mission in " + s_Where + ". Start any mission in " +
                        s_Where + " to join them.");
                }
            }

            break;
        }

        case MsgType::SceneReady: {
            MsgSceneReady s_Ready;

            if (m_Role == SessionRole::Host && Decode(p_Msg.payload, s_Ready)) {
                // Not while ours is reloading too (a restart takes both out of the mission a moment).
                if (s_Ready.epoch == 0 && m_PeerReadyEpoch != 0 && m_PeerReadyEpoch == m_Epoch && m_LocalInScene && m_World.IsInScene())
                    m_World.Notify(m_PeerName + " left the mission.");

                m_PeerReadyEpoch = s_Ready.epoch;

                if (s_Ready.epoch != 0)
                    m_PeerScene = {};
            }

            break;
        }

        case MsgType::AvatarState: {
            MsgAvatarState s_State;

            if (Decode(p_Msg.payload, s_State) && IsSynced() && s_State.epoch == m_Epoch) {
                m_RemoteAvatar.Push(p_Now, s_State.timeMs, s_State.xf);
                m_RemoteAvatarLast = s_State.xf;
                m_HaveRemoteAvatar = true;
            }

            break;
        }

        case MsgType::Pose: {
            MsgPose s_Pose;

            if (Decode(p_Msg.payload, s_Pose) && IsSynced() && s_Pose.epoch == m_Epoch) {
                m_RemotePose.Push(p_Now, s_Pose.timeMs, s_Pose.pose);
                ++m_PosesReceived;
            }

            break;
        }

        case MsgType::VictimPose: {
            MsgVictimPose s_Victim;

            if (Decode(p_Msg.payload, s_Victim) && IsSynced() && s_Victim.body.epoch == m_Epoch) {
                // A different NPC: start its playback afresh rather than blending between two.
                if (s_Victim.id != m_VictimId) {
                    ResetVictim();
                    m_VictimId = s_Victim.id;
                }

                m_VictimXf.Push(p_Now, s_Victim.body.timeMs, s_Victim.xf);
                m_VictimPose.Push(p_Now, s_Victim.body.timeMs, s_Victim.body.pose);
                m_VictimFlags = s_Victim.flags;
                m_VictimLastReceive = p_Now;
            }

            break;
        }

        case MsgType::Skeleton: {
            MsgSkeleton s_Skeleton;

            if (Decode(p_Msg.payload, s_Skeleton))
                m_World.SetRemoteSkeleton(s_Skeleton.parents);

            break;
        }

        case MsgType::Outfit: {
            MsgOutfit s_Outfit;

            if (!Decode(p_Msg.payload, s_Outfit))
                break;

            m_RemoteOutfit = s_Outfit.outfit;
            m_HasRemoteOutfit = true;

            if (IsSynced())
                m_World.SetRemoteOutfit(m_RemoteOutfit);

            break;
        }

        case MsgType::NpcSnapshot: {
            MsgNpcSnapshot s_Snapshot;

            if (!Decode(p_Msg.payload, s_Snapshot) || s_Snapshot.epoch != m_Epoch || !IsSynced())
                break;

            for (const auto& s_Npc : s_Snapshot.npcs) {
                if (m_Role == SessionRole::Client)
                    m_HostNpcs[s_Npc.id] = s_Npc;

                // Only NPCs the sender runs are in it; one we took over since is ours to move.
                if (OwnerOf(s_Npc.id) != MyOwner())
                    m_Puppets[s_Npc.id].xf.Push(p_Now, s_Snapshot.timeMs, s_Npc.xf);
            }

            break;
        }

        case MsgType::Restart: {
            MsgRestart s_Restart;

            // Only into the same mission: the partner restarting something else changes nothing here.
            if (Decode(p_Msg.payload, s_Restart) && m_World.IsInScene() && m_World.CurrentScene().scene == s_Restart.scene) {
                m_World.Notify(m_PeerName + " restarted the mission.");
                m_World.RestartMission();
            }

            break;
        }

        case MsgType::Busy: {
            MsgBusy s_Busy;

            if (Decode(p_Msg.payload, s_Busy)) {
                m_PeerBusyUntil = s_Busy.seconds == 0 ? 0.0 : p_Now + std::min<double>(s_Busy.seconds, m_Config.busyGraceMax);
                m_PeerBusyStage = s_Busy.seconds == 0 ? static_cast<uint8_t>(LoadStage_None) : s_Busy.stage;
                m_PeerBusyScene = s_Busy.seconds == 0 ? std::string() : s_Busy.scene;
            }

            break;
        }

        case MsgType::Chat: {
            MsgChat s_Chat;

            // At most 8 at once, then one a second.
            if (Decode(p_Msg.payload, s_Chat) && !s_Chat.text.empty() && Admit(m_ChatGate, p_Now, 1.0, 8.0))
                m_World.OnPeerChat(s_Chat.text, s_Chat.quick != 0);

            break;
        }

        case MsgType::Mark: {
            MsgMark s_Mark;

            if (Decode(p_Msg.payload, s_Mark) && s_Mark.epoch == m_Epoch && IsSynced() &&
                (s_Mark.kind == MarkKind_Remove || Admit(m_MarkGate, p_Now, 1.5, 5.0)))
                m_World.OnPeerMark(s_Mark.id, s_Mark.pos, s_Mark.kind);

            break;
        }

        case MsgType::NpcKnockedOut: {
            MsgNpcDied s_Down;

            if (!Decode(p_Msg.payload, s_Down) || s_Down.epoch != m_Epoch || !IsSynced() || !m_KnownDown.insert(s_Down.id).second)
                break;

            // A body isn't a puppet: it lies where it fell here (a drag comes as the partner's victim).
            if (m_Puppeted.erase(s_Down.id)) {
                m_World.ClearNpcPose(s_Down.id);
                m_World.SetNpcPuppet(s_Down.id, false);
                m_Puppets.erase(s_Down.id);
            }

            m_World.KnockOutNpc(s_Down.id);
            break;
        }

        case MsgType::PlayerState: {
            MsgPlayerState s_State;

            if (Decode(p_Msg.payload, s_State)) {
                m_PeerState = s_State;
                m_PeerStateAt = p_Now;
            }

            break;
        }

        case MsgType::Pause: {
            MsgPause s_Pause;

            if (Decode(p_Msg.payload, s_Pause) && IsSynced() && (s_Pause.paused != 0) != m_PartnerPaused) {
                m_PartnerPaused = s_Pause.paused != 0;
                m_World.SetPartnerPaused(m_PartnerPaused);
                m_World.Notify(m_PartnerPaused ? m_PeerName + " paused the game." : m_PeerName + " is back.");
            }

            break;
        }

        case MsgType::NpcOwners: {
            MsgNpcOwners s_Owners;

            if (m_Role == SessionRole::Client && Decode(p_Msg.payload, s_Owners) && s_Owners.epoch == m_Epoch && IsSynced())
                SetOwners(s_Owners.owners);

            break;
        }

        case MsgType::NpcPose: {
            MsgVictimPose s_Pose;

            if (!Decode(p_Msg.payload, s_Pose) || s_Pose.body.epoch != m_Epoch || !IsSynced() || OwnerOf(s_Pose.id) == MyOwner())
                break;

            auto& s_Puppet = m_Puppets[s_Pose.id];
            s_Puppet.pose.Push(p_Now, s_Pose.body.timeMs, s_Pose.body.pose);
            s_Puppet.lastPose = p_Now;
            break;
        }

        case MsgType::NpcDied: {
            MsgNpcDied s_Died;

            if (m_Role == SessionRole::Client && Decode(p_Msg.payload, s_Died) && s_Died.epoch == m_Epoch)
                m_HostDead.insert(s_Died.id);

            break;
        }

        case MsgType::NpcKillRequest: {
            MsgNpcDied s_Request;

            if (m_Role != SessionRole::Host || !Decode(p_Msg.payload, s_Request) || s_Request.epoch != m_Epoch ||
                !IsSynced())
                break;

            // Only kill NPCs we know are alive; the death is then announced back by UpdateNpcsHost.
            if (const auto s_It = m_HostNpcAlive.find(s_Request.id); s_It != m_HostNpcAlive.end() && s_It->second)
                m_World.KillNpc(s_Request.id);

            break;
        }

        case MsgType::Alerts: {
            MsgAlerts s_Alerts;

            if (Decode(p_Msg.payload, s_Alerts) && s_Alerts.epoch == m_Epoch && IsSynced())
                m_World.ApplyPeerAlerts(s_Alerts.alerts);

            break;
        }

        case MsgType::Item: {
            MsgItem s_Item;

            if (Decode(p_Msg.payload, s_Item) && s_Item.epoch == m_Epoch && IsSynced())
                m_World.ApplyPeerItem(s_Item.change);

            break;
        }

        case MsgType::MissionEvent: {
            MsgMissionEvent s_Event;

            // A big explosion is a burst of kills: 60 at once, then 15 a second.
            if (Decode(p_Msg.payload, s_Event) && Admit(m_EventGate, p_Now, 15.0, 60.0))
                m_World.OnPeerMissionEvent(s_Event.name, s_Event.json);

            break;
        }

        default:
            break;
    }
}

void Session::HandleHello(const MsgHello& p_Hello, double p_Now) {
    if (m_PeerHandshaken)
        return;

    std::string s_Reason;

    if (p_Hello.protocol != kProtocolVersion) {
        // The player's words first (versions as they see them), the protocol numbers after.
        s_Reason = "Different HITMAN Co-op versions: the host has " + m_Config.modVersion + ", you have " +
            (p_Hello.modVersion.empty() ? std::string("an older one") : p_Hello.modVersion) +
            ". Both players need the same version (mod protocol " + std::to_string(kProtocolVersion) + " vs " +
            std::to_string(p_Hello.protocol) + ").";
    }
    else if (!m_Config.allowGameVersionMismatch && p_Hello.gameVersion != m_Config.gameVersion) {
        s_Reason = "Game version mismatch (host " + m_Config.gameVersion + ", you " + p_Hello.gameVersion + ").";
    }

    if (!s_Reason.empty()) {
        m_Conn->SendReliable(MsgType::Reject, Encode(MsgReject { s_Reason }));
        FlushTo(p_Now);
        DropPeer("Refused " + p_Hello.playerName + ": " + s_Reason);
        return;
    }

    MsgWelcome s_Welcome;
    s_Welcome.hostName = m_Config.playerName;
    s_Welcome.gameVersion = m_Config.gameVersion;
    s_Welcome.modVersion = m_Config.modVersion;
    m_Conn->SendReliable(MsgType::Welcome, Encode(s_Welcome));

    m_PeerHandshaken = true;
    m_PeerName = p_Hello.playerName.empty() ? "Your friend" : p_Hello.playerName;
    m_PeerGameVersion = p_Hello.gameVersion;
    m_PeerModVersion = p_Hello.modVersion;
    m_PeerReadyEpoch = 0;
    m_State = SessionState::Connected;
    m_Message = m_PeerName + " joined.";
    m_World.Notify(m_PeerName + " joined.");

    if (VersionsDiffer(p_Hello.gameVersion, m_Config.gameVersion)) {
        m_World.Notify("Game versions differ (you " + m_Config.gameVersion + ", " + m_PeerName + " " +
            p_Hello.gameVersion + "). Some NPCs may not sync.");
    }

    SendSceneInfo();
}

void Session::SendSceneInfo() {
    if (m_Role != SessionRole::Host || m_State != SessionState::Connected || !m_Conn || !m_PeerHandshaken)
        return;

    MsgSceneInfo s_Info;
    s_Info.epoch = m_Epoch;

    if (m_LocalInScene)
        s_Info.scene = m_World.CurrentScene();

    m_Conn->SendReliable(MsgType::SceneInfo, Encode(s_Info));
}

// Client to host: "I'm in this mission, not yours", so the host can see why nobody is syncing.
void Session::SendClientScene() {
    if (m_Role != SessionRole::Client || m_State != SessionState::Connected || !m_Conn)
        return;

    MsgSceneInfo s_Info;
    s_Info.epoch = 0;

    if (m_LocalInScene && m_World.IsInScene())
        s_Info.scene = m_World.CurrentScene();

    m_Conn->SendReliable(MsgType::SceneInfo, Encode(s_Info));
}

void Session::SendSceneReady(uint32_t p_Epoch) {
    if (m_Conn && m_State == SessionState::Connected)
        m_Conn->SendReliable(MsgType::SceneReady, Encode(MsgSceneReady { p_Epoch }));
}

void Session::UpdateScene() {
    if (m_Role == SessionRole::Client && m_LocalInScene && m_World.IsInScene() && !m_HostScene.Empty()) {
        if (m_World.CurrentScene().SameAs(m_HostScene)) {
            if (m_LocalReadyEpoch != m_Epoch) {
                m_LocalReadyEpoch = m_Epoch;
                SendSceneReady(m_Epoch);
            }
        }
        else if (!m_ToldAboutMismatch) {
            m_ToldAboutMismatch = true;
            const SceneDesc s_Mine = m_World.CurrentScene();
            const std::string s_There = LocationDisplayName(m_HostScene.Location());

            if (s_Mine.DiffersInDifficulty(m_HostScene)) {
                m_World.Notify("You and " + m_PeerName + " picked different difficulties. Go back to the menu and start the mission "
                    "again on the same difficulty as them.");
            }
            else if (s_Mine.SameLocationAs(m_HostScene)) {
                m_World.Notify("You are in a different mission from " + m_PeerName + ". Go back to the menu and start any mission in " +
                    s_There + " to follow them.");
            }
            else {
                m_World.Notify("You are in " + LocationDisplayName(s_Mine.Location()) + " and " + m_PeerName + " is in " + s_There +
                    ". You can only play together at a location you both own.");
            }

            SendClientScene();
        }
    }

    const bool s_Synced = IsSynced();

    if (s_Synced != m_WasSynced) {
        m_WasSynced = s_Synced;
        OnSyncChanged(s_Synced);
    }
}

void Session::OnSyncChanged(bool p_Synced) {
    if (p_Synced) {
        m_HasLocalOutfitSent = false;
        m_HasLocalSkeletonSent = false;
        m_NextAvatarSend = 0.0;
        m_NextNpcSend = 0.0;

        if (m_HasRemoteOutfit)
            m_World.SetRemoteOutfit(m_RemoteOutfit);

        m_World.Notify("Co-op active: " + m_PeerName + " is in the mission with you.");
        return;
    }

    if (m_RemoteAvatarShown) {
        m_World.HideRemoteAvatar();
        m_RemoteAvatarShown = false;
    }

    m_World.ResetItems();
    m_World.ApplyPeerAlerts({});
    ReleasePuppets();

    // Never left paused by a partner who is gone; their next pause is told again.
    if (m_PartnerPaused)
        m_World.SetPartnerPaused(false);

    m_PartnerPaused = false;
    m_SentPause = false;

    m_RemoteAvatar.Clear();
    m_RemotePose.Clear();
    ResetVictim();
    m_HostNpcs.clear();
    m_NpcSent.clear();
}

void Session::UpdateOutfit() {
    if (!IsSynced())
        return;

    OutfitDesc s_Outfit;

    if (!m_World.GetLocalOutfit(s_Outfit))
        return;

    if (!m_HasLocalOutfitSent || s_Outfit != m_LastSentOutfit) {
        m_Conn->SendReliable(MsgType::Outfit, Encode(MsgOutfit { s_Outfit }));
        m_LastSentOutfit = s_Outfit;
        m_HasLocalOutfitSent = true;
    }
}

void Session::UpdateSkeleton(double p_Now) {
    // A few times a second: an outfit change can bring another skeleton a moment after the outfit.
    if (!IsSynced() || p_Now < m_NextSkeletonCheck)
        return;

    m_NextSkeletonCheck = p_Now + 0.25;
    m_SkeletonScratch.clear();

    if (!m_World.GetLocalSkeleton(m_SkeletonScratch) || m_SkeletonScratch.empty())
        return;

    if (!m_HasLocalSkeletonSent || m_SkeletonScratch != m_LastSentSkeleton) {
        m_Conn->SendReliable(MsgType::Skeleton, Encode(MsgSkeleton { m_SkeletonScratch }));
        m_LastSentSkeleton = m_SkeletonScratch;
        m_HasLocalSkeletonSent = true;
    }
}

void Session::UpdateAvatar(double p_Now) {
    if (!IsSynced())
        return;

    if (p_Now >= m_NextAvatarSend) {
        MsgAvatarState s_State;
        s_State.epoch = m_Epoch;
        s_State.timeMs = static_cast<uint32_t>(static_cast<uint64_t>(p_Now * 1000.0));

        if (m_World.GetLocalAvatar(s_State.xf)) {
            m_Conn->SendUnreliable(MsgType::AvatarState, Encode(s_State));

            // The pose goes with the same timestamp, so both play back in step.
            MsgPose s_Pose;
            s_Pose.epoch = m_Epoch;
            s_Pose.timeMs = s_State.timeMs;

            if (m_World.GetLocalPose(s_Pose.pose))
                m_Conn->SendUnreliable(MsgType::Pose, Encode(s_Pose));

            m_VictimSend.body.epoch = m_Epoch;
            m_VictimSend.body.timeMs = s_State.timeMs;

            if (m_World.GetLocalVictim(m_VictimSend.id, m_VictimSend.xf, m_VictimSend.body.pose, m_VictimSend.flags))
                m_Conn->SendUnreliable(MsgType::VictimPose, Encode(m_VictimSend));
        }

        m_NextAvatarSend = std::max(m_NextAvatarSend + m_Config.avatarInterval, p_Now);
    }

    PlayPair(m_RemoteAvatar, m_RemotePose);
    PlayPair(m_VictimXf, m_VictimPose);
    Transform s_Xf;

    if (m_RemoteAvatar.Sample(p_Now, s_Xf)) {
        m_World.ShowRemoteAvatar(s_Xf);
        m_RemoteAvatarShown = true;

        if (m_RemotePose.Sample(p_Now, m_PoseScratch))
            m_World.ShowRemotePose(m_PoseScratch);
    }

    // The partner's takedown victim, played back on the same delay as the partner.
    if (m_VictimId != 0 && p_Now - m_VictimLastReceive > 0.6)
        ResetVictim();
    else if (m_VictimId != 0 && m_VictimXf.Sample(p_Now, m_VictimXfScratch) &&
             m_VictimPose.Sample(p_Now, m_VictimPoseScratch)) {
        m_World.ShowRemoteVictim(m_VictimId, m_VictimXfScratch, m_VictimPoseScratch, m_VictimFlags);
        m_VictimShown = true;
    }
}

void Session::UpdateItems() {
    // Only collected while synced: the first collection then reports what our player already
    // holds, so a partner who joins late still sees those items gone.
    if (!IsSynced())
        return;

    m_ItemScratch.clear();
    m_World.CollectItemChanges(m_ItemScratch);

    for (const auto& s_Change : m_ItemScratch)
        m_Conn->SendReliable(MsgType::Item, Encode(MsgItem { m_Epoch, s_Change }));
}

void Session::UpdateAlerts(double p_Now) {
    if (!IsSynced() || p_Now < m_NextAlertSend)
        return;

    m_NextAlertSend = p_Now + m_Config.alertInterval;
    m_AlertSend.epoch = m_Epoch;
    m_AlertSend.alerts.clear();
    m_World.CollectAlerts(m_AlertSend.alerts);
    m_Conn->SendUnreliable(MsgType::Alerts, Encode(m_AlertSend));
}

const std::vector<NpcState>& Session::Npcs() {
    if (!m_NpcCacheValid) {
        m_NpcCache.clear();
        m_World.ListNpcs(m_NpcCache);
        m_NpcCacheValid = true;
    }

    return m_NpcCache;
}

// Knocked out here by any means (a takedown, a thrown object, a sedative): the partner's copy
// goes down too. Either game announces it; the other only applies it.
static bool KnockedOut(const NpcState& p_Npc) { return p_Npc.Alive() && (p_Npc.flags & NpcFlag_Pacified) != 0; }

void Session::UpdateNpcsHost(double p_Now) {
    if (!IsSynced())
        return;

    (void)p_Now;

    for (const auto& s_Npc : Npcs()) {
        const bool s_Alive = s_Npc.Alive();
        auto [s_It, s_Inserted] = m_HostNpcAlive.try_emplace(s_Npc.id, s_Alive);

        if (!s_Inserted && s_It->second && !s_Alive)
            m_Conn->SendReliable(MsgType::NpcDied, Encode(MsgNpcDied { m_Epoch, s_Npc.id }));

        // An NPC that was already dead when we first saw it still needs announcing.
        if (s_Inserted && !s_Alive)
            m_Conn->SendReliable(MsgType::NpcDied, Encode(MsgNpcDied { m_Epoch, s_Npc.id }));

        s_It->second = s_Alive;

        if (KnockedOut(s_Npc) && m_KnownDown.insert(s_Npc.id).second)
            m_Conn->SendReliable(MsgType::NpcKnockedOut, Encode(MsgNpcDied { m_Epoch, s_Npc.id }));
    }
}

void Session::UpdateNpcsClient(float p_Dt) {
    if (!IsSynced())
        return;

    (void)p_Dt;

    for (const auto& s_Npc : Npcs()) {
        const bool s_Alive = s_Npc.Alive();
        const bool s_HostDead = m_HostDead.count(s_Npc.id) != 0;

        if (s_HostDead) {
            if (s_Alive && m_KillApplied.insert(s_Npc.id).second)
                m_World.KillNpc(s_Npc.id);
        }
        else {
            const auto s_Prev = m_ClientNpcAlive.find(s_Npc.id);

            // We killed it locally: ask the host, who owns NPCs, to make it official.
            if (s_Prev != m_ClientNpcAlive.end() && s_Prev->second && !s_Alive &&
                m_KillRequested.insert(s_Npc.id).second)
                m_Conn->SendReliable(MsgType::NpcKillRequest, Encode(MsgNpcDied { m_Epoch, s_Npc.id }));
        }

        m_ClientNpcAlive[s_Npc.id] = s_Alive;

        if (KnockedOut(s_Npc) && m_KnownDown.insert(s_Npc.id).second)
            m_Conn->SendReliable(MsgType::NpcKnockedOut, Encode(MsgNpcDied { m_Epoch, s_Npc.id }));
    }
}

void Session::UpdateLink(double p_Now) {
    if (!m_Conn || p_Now < m_NextLinkCheck)
        return;

    m_NextLinkCheck = p_Now + 0.5;
    const ConnectionStats s_Stats = m_Conn->Stats();

    if (s_Stats.rttMs > 0.0)
        m_MinRtt = std::min(m_MinRtt, s_Stats.rttMs);

    // Reliable messages piling up, or the round trip well above its best, for 1.5 s on end: the
    // link can't carry what we send (a mission's first moments send a burst, and a hitch delays
    // one ack: neither counts). NPC detail goes at half the rate until it has been fine for 4 s.
    const bool s_Struggling = s_Stats.reliablePending > 48 ||
        (s_Stats.rttMs > 0.0 && m_MinRtt < 1e8 && s_Stats.rttMs > m_MinRtt * 2.0 + 120.0);
    m_StruggleChecks = s_Struggling ? m_StruggleChecks + 1 : 0;

    if (m_StruggleChecks >= 3) {
        m_CongestedSeen = p_Now;

        if (!m_Congested && IsSynced() && p_Now - m_CongestionToldAt > 60.0) {
            m_CongestionToldAt = p_Now;
            m_World.Notify("The connection is struggling: sending less NPC detail until it recovers.");
        }

        m_Congested = true;
    }
    else if (m_Congested && p_Now - m_CongestedSeen > 4.0)
        m_Congested = false;

    if (p_Now - m_LinkBytesAt >= 1.0) {
        if (m_LinkBytesAt > 0.0)
            m_SendKbps = static_cast<double>(s_Stats.bytesSent - m_LinkBytes) * 8.0 / 1000.0 / (p_Now - m_LinkBytesAt);

        m_LinkBytes = s_Stats.bytesSent;
        m_LinkBytesAt = p_Now;
    }
}

void Session::OnLocalRestart(const std::string& p_Scene) {
    if (m_Conn && m_State == SessionState::Connected)
        m_Conn->SendReliable(MsgType::Restart, Encode(MsgRestart { p_Scene }));
}

void Session::UpdatePause() {
    if (!IsSynced())
        return;

    if (const bool s_Paused = m_World.IsLocallyPaused(); s_Paused != m_SentPause) {
        m_SentPause = s_Paused;
        m_Conn->SendReliable(MsgType::Pause, Encode(MsgPause { static_cast<uint8_t>(s_Paused ? 1 : 0) }));
    }
}

void Session::SetOwners(const std::vector<NpcOwnership>& p_Changes) {
    for (const auto& s_Change : p_Changes) {
        m_NpcOwner[s_Change.id] = s_Change.owner;

        // Ours now: what the partner streamed is stale.
        if (s_Change.owner == MyOwner())
            m_Puppets.erase(s_Change.id);
    }

    ReconcilePuppets();
}

// Every listed NPC that is alive and standing is a puppet exactly when the partner runs it. A
// knocked out one lies where it fell in each game.
void Session::ReconcilePuppets() {
    for (const auto& s_Npc : Npcs()) {
        const bool s_Want = s_Npc.Alive() && !KnockedOut(s_Npc) && OwnerOf(s_Npc.id) != MyOwner();
        const bool s_Is = m_Puppeted.count(s_Npc.id) != 0;

        if (s_Want == s_Is)
            continue;

        if (s_Want)
            m_Puppeted.insert(s_Npc.id);
        else {
            m_Puppeted.erase(s_Npc.id);
            m_World.ClearNpcPose(s_Npc.id);
            m_Puppets.erase(s_Npc.id);
        }

        m_World.SetNpcPuppet(s_Npc.id, s_Want);
    }
}

void Session::ReleasePuppets() {
    for (const uint64_t s_Id : m_Puppeted) {
        m_World.ClearNpcPose(s_Id);
        m_World.SetNpcPuppet(s_Id, false);
    }

    m_Puppeted.clear();
    m_Puppets.clear();
    m_NpcOwner.clear();
    m_NpcSent.clear();
    m_PoseSent.clear();
    m_HaveRemoteAvatar = false;
    m_World.WatchNpcPoses({});
}

void Session::UpdateSharedNpcs(double p_Now) {
    if (!IsSynced())
        return;

    // Host: who runs what. An NPC goes to the client once the client is clearly nearer (some
    // slack, so NPCs don't flip back and forth) and comes back once it is only a little nearer, so
    // players standing together leave everything with the host. An alert NPC stays with the game
    // it is alert in (its search or fight goes on there), and a body with the game it fell in.
    if (p_Now >= m_NextOwnerCheck) {
        m_NextOwnerCheck = p_Now + m_Config.ownerCheckInterval;
        Transform s_Local;

        if (m_Role == SessionRole::Host && m_HaveRemoteAvatar && m_World.GetLocalAvatar(s_Local)) {
            const auto& s_Npcs = Npcs();
            std::vector<NpcOwnership> s_Changes;

            for (const auto& s_Npc : s_Npcs) {
                if (!s_Npc.Alive() || KnockedOut(s_Npc) || s_Npc.tension >= 4)
                    continue;

                const float s_ToHost = Distance(s_Local.pos, s_Npc.xf.pos);
                const float s_ToClient = Distance(m_RemoteAvatarLast.pos, s_Npc.xf.pos);
                const uint8_t s_Owner = OwnerOf(s_Npc.id);

                if (s_Owner == NpcOwner_Host && s_ToClient + m_Config.ownerHysteresis < s_ToHost)
                    s_Changes.push_back({ s_Npc.id, NpcOwner_Client });
                else if (s_Owner == NpcOwner_Client && s_ToHost < s_ToClient + m_Config.ownerHysteresis * 0.5f)
                    s_Changes.push_back({ s_Npc.id, NpcOwner_Host });
            }

            {
                float s_Nearest = 1e9f;
                size_t s_Alert = 0;
                Vec3 s_Spot {};
                float s_SpotFrom = 1e9f;
                for (const auto& s_Npc : s_Npcs) {
                    s_Nearest = std::min(s_Nearest, Distance(m_RemoteAvatarLast.pos, s_Npc.xf.pos));
                    s_Alert += s_Npc.tension >= 4 ? 1 : 0;

                    // A calm NPC some way from our player (where a partner would take it over).
                    if (const float s_D = Distance(s_Local.pos, s_Npc.xf.pos); s_Npc.Alive() && s_Npc.tension < 4 && s_D > 15.f && std::abs(s_D - 25.f) < std::abs(s_SpotFrom - 25.f)) {
                        s_Spot = s_Npc.xf.pos;
                        s_SpotFrom = s_D;
                    }
                }
                char s_Text[220];
                std::snprintf(s_Text, sizeof(s_Text), "%zu npcs, %zu alert, nearest to partner %.1f m, partner at %.1f %.1f %.1f, %zu changes, spot %.1f %.1f %.1f",
                    s_Npcs.size(), s_Alert, s_Nearest, m_RemoteAvatarLast.pos.x, m_RemoteAvatarLast.pos.y, m_RemoteAvatarLast.pos.z,
                    s_Changes.size(), s_Spot.x, s_Spot.y, s_Spot.z);
                m_OwnerCheck = s_Text;
            }

            for (size_t i = 0; i < s_Changes.size(); i += kMaxOwnershipsPerMsg) {
                MsgNpcOwners s_Msg;
                s_Msg.epoch = m_Epoch;
                s_Msg.owners.assign(s_Changes.begin() + i, s_Changes.begin() + std::min(s_Changes.size(), i + kMaxOwnershipsPerMsg));
                m_Conn->SendReliable(MsgType::NpcOwners, Encode(s_Msg));
            }

            SetOwners(s_Changes);
        }
        else
            ReconcilePuppets(); // NPCs that appeared or died since
    }

    // The NPCs we run, to the partner: those that moved, and every one now and then. Those far
    // from the partner (who only sees them from afar) a quarter as often.
    if (p_Now >= m_NextNpcSend) {
        m_NextNpcSend = std::max(m_NextNpcSend + m_Config.npcInterval * RateScale(), p_Now);

        MsgNpcSnapshot s_Snapshot;
        s_Snapshot.epoch = m_Epoch;
        s_Snapshot.tick = ++m_NpcTick;
        s_Snapshot.timeMs = static_cast<uint32_t>(static_cast<uint64_t>(p_Now * 1000.0));

        const auto s_SendChunk = [&] {
            if (!s_Snapshot.npcs.empty())
                m_Conn->SendUnreliable(MsgType::NpcSnapshot, Encode(s_Snapshot));

            s_Snapshot.npcs.clear();
        };

        for (const auto& s_Npc : Npcs()) {
            if (!s_Npc.Alive() || KnockedOut(s_Npc) || OwnerOf(s_Npc.id) != MyOwner())
                continue;

            auto s_Sent = m_NpcSent.find(s_Npc.id);
            const bool s_RefreshDue = s_Sent == m_NpcSent.end() || p_Now - s_Sent->second.time >= c_NpcRefreshInterval;

            if (!s_RefreshDue && Distance(s_Sent->second.xf.pos, s_Npc.xf.pos) < c_NpcMoveThreshold &&
                AngleBetween(s_Sent->second.xf.rot, s_Npc.xf.rot) < c_NpcTurnThreshold)
                continue;

            if (!s_RefreshDue && m_HaveRemoteAvatar && Distance(m_RemoteAvatarLast.pos, s_Npc.xf.pos) > m_Config.npcFarRange &&
                (m_NpcTick + static_cast<uint32_t>(s_Npc.id)) % 4 != 0)
                continue;

            m_NpcSent[s_Npc.id] = { s_Npc.xf, p_Now };
            s_Snapshot.npcs.push_back(s_Npc);

            if (s_Snapshot.npcs.size() == kMaxNpcsPerSnapshot)
                s_SendChunk();
        }

        s_SendChunk();
    }

    // Skeleton poses of the NPCs we run that are near the partner, so they animate there too: the
    // nearest few at the full rate, the rest at half, and one standing still not again until it
    // moves (or its last is getting old).
    if (p_Now >= m_NextNpcPoseSend && m_HaveRemoteAvatar) {
        m_NextNpcPoseSend = p_Now + m_Config.npcPoseInterval * RateScale();
        ++m_PoseRound;
        std::vector<std::pair<float, uint64_t>> s_Near;

        for (const auto& s_Npc : Npcs()) {
            if (!s_Npc.Alive() || KnockedOut(s_Npc) || OwnerOf(s_Npc.id) != MyOwner())
                continue;

            if (const float s_D = Distance(m_RemoteAvatarLast.pos, s_Npc.xf.pos); s_D < m_Config.npcPoseRange)
                s_Near.emplace_back(s_D, s_Npc.id);
        }

        std::sort(s_Near.begin(), s_Near.end());
        const size_t s_Max = m_Congested ? std::max<size_t>(1, m_Config.npcPoseMax / 2) : m_Config.npcPoseMax;

        if (s_Near.size() > s_Max)
            s_Near.resize(s_Max);

        m_PoseIds.clear();

        for (const auto& [s_D, s_Id] : s_Near)
            m_PoseIds.push_back(s_Id);

        m_World.WatchNpcPoses(m_PoseIds);
        m_NpcPoseSend.body.epoch = m_Epoch;
        m_NpcPoseSend.body.timeMs = static_cast<uint32_t>(static_cast<uint64_t>(p_Now * 1000.0));
        m_NpcPoseSend.flags = NpcFlag_Alive;

        for (size_t i = 0; i < m_PoseIds.size(); ++i) {
            const uint64_t s_Id = m_PoseIds[i];

            if (i >= m_Config.npcPoseFullRate && (m_PoseRound + static_cast<uint32_t>(s_Id)) % 2 != 0)
                continue;

            if (!m_World.GetNpcPose(s_Id, m_NpcPoseSend.body.pose))
                continue;

            auto& s_Last = m_PoseSent[s_Id];

            if (p_Now - s_Last.time < c_PoseHold * 0.3 && PosesAlike(s_Last.pose, m_NpcPoseSend.body.pose))
                continue;

            s_Last.pose = m_NpcPoseSend.body.pose;
            s_Last.time = p_Now;
            m_NpcPoseSend.id = s_Id;
            m_Conn->SendUnreliable(MsgType::NpcPose, Encode(m_NpcPoseSend));
        }
    }

    UpdatePuppets(p_Now);
}

void Session::PlayPair(SnapshotBuffer& p_Xf, PoseBuffer& p_Pose) {
    if (!m_Config.adaptiveDelay)
        return;

    const double s_Delay = std::max(p_Xf.Wanted(m_Config.interpolationMin, m_Config.interpolationMax),
        p_Pose.Empty() ? 0.0 : p_Pose.Wanted(m_Config.interpolationMin, m_Config.interpolationMax));
    p_Xf.Retarget(s_Delay);
    p_Pose.Retarget(s_Delay);
}

// Every frame: the partner's NPCs where and how they are there (a moment ago, played back smoothly).
void Session::UpdatePuppets(double p_Now) {
    Transform s_Local;
    const bool s_HaveLocal = m_World.GetLocalAvatar(s_Local);
    ++m_PuppetFrame;

    for (auto& [s_Id, s_Puppet] : m_Puppets) {
        // The partner's takedown victim is placed by that stream (see UpdateAvatar).
        if (s_Id == m_VictimId || !m_Puppeted.count(s_Id))
            continue;

        PlayPair(s_Puppet.xf, s_Puppet.pose);
        Transform s_Xf;

        if (!s_Puppet.xf.Sample(p_Now, s_Xf))
            continue;

        // Far ones less often: nobody sees them closely.
        const bool s_Near = s_HaveLocal && Distance(s_Local.pos, s_Xf.pos) < m_Config.puppetNearRange;

        if (s_Near || (m_PuppetFrame + static_cast<uint32_t>(s_Id)) % 8 == 0)
            m_World.SetNpcTransform(s_Id, s_Xf);

        if (p_Now - s_Puppet.lastPose < c_PoseHold && s_Puppet.pose.Sample(p_Now, m_NpcPoseScratch)) {
            m_World.ShowNpcPose(s_Id, m_NpcPoseScratch);
            s_Puppet.posed = true;
        }
        else if (s_Puppet.posed) {
            m_World.ClearNpcPose(s_Id);
            s_Puppet.posed = false;
        }
    }
}

} // namespace coop
