#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <list>
#include <map>
#include <set>
#include <string>

#include "IPluginInterface.h"
#include "Glacier/ZResource.h"

class ZTemplateEntityFactory;

#include "Hotkeys.h"
#include "SdkWorld.h"
#include "TestChannel.h"
#include "coop/GameEvents.h"
#include "coop/RelaySocket.h"
#include "coop/Session.h"

class ZAchievementManagerSimple;
class ZDynamicObject;
class IBaseCharacter;
class ZKnowledge;
class ZApplicationEngineWin32;
struct SHitInfo;

// Two-player co-op for HITMAN World of Assassination. One player hosts from the in-game SDK
// menu and gets a join code from the co-op relay server (server/relay.py); the other joins with
// the code, or directly by address. No IOI, Peacock or store servers are involved.
class HitmanCoop : public IPluginInterface {
public:
    HitmanCoop();
    ~HitmanCoop() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    static std::string ModsFolder();
    void AppendLog(const std::string& p_Line);
    std::string m_LogPath;
    size_t m_LogBytes = 0;
    std::mutex m_LogMutex;

    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void ProcessCommands(double p_Now);
    void PublishStatus();
    void LoadSettings();
    void SaveSettings();
    static double Now();
    static std::string DetectGameVersion();
    // The engine parts this mod can't work without that the SDK couldn't find in this game build
    // (empty: all there). Co-op stays off with a message instead of crashing the game.
    static std::string MissingEngineParts();
    std::string m_Unsupported;

    DECLARE_PLUGIN_DETOUR(HitmanCoop, bool, OnLoadScene, ZEntitySceneContext*, SSceneInitParameters&);
    DECLARE_PLUGIN_DETOUR(HitmanCoop, void, OnClearScene, ZEntitySceneContext*, bool p_FullyUnloadScene);
    // Our mission restarted (or a save of it loaded): the partner's restarts too.
    void OnMissionRestarted(const std::string& p_Scene);
    std::string m_ClearedMission; // the mission a full clear left, until the next scene
    bool m_RestartTold = false;   // this restart was seen by its event, before the clear
    DECLARE_PLUGIN_DETOUR(HitmanCoop, bool, OnActorHit, IBaseCharacter* th, const SHitInfo& p_HitInfo);
    // The stand-in's alert level (its own, per NPC) is held at ambient: it never gets agitated,
    // searches, flees or fights, whatever it might still notice.
    DECLARE_PLUGIN_DETOUR(HitmanCoop, void, OnSetGameTension, ZKnowledge* th, EGameTension p_Tension);
    DECLARE_PLUGIN_DETOUR(
        HitmanCoop, void, OnEventSent, ZAchievementManagerSimple* th, uint32_t p_EventIndex, const ZDynamicObject& p_Event
    );
    // Test channel only (not hooked for players): logs and presses the game's input actions, so a
    // test can get through menus without a person. Windows-level injected input doesn't reach them.
    DECLARE_PLUGIN_DETOUR(HitmanCoop, bool, OnInputDigital, ZInputAction* th, int32_t p_ControllerId);
    DECLARE_PLUGIN_DETOUR(HitmanCoop, float, OnInputAnalog, ZInputAction* th, int32_t p_ControllerId);
    // The level's own state as the game moves it (loading, preparing, activating, playing): what a
    // loading partner is told, and the moment the mission is playable.
    DECLARE_PLUGIN_DETOUR(HitmanCoop, void, OnSetGameState, ZLevelManager* th, ZLevelManager::EGameState p_State);
    // The game menus our player opens (the map, the notebook...): the partner sees which.
    DECLARE_PLUGIN_DETOUR(HitmanCoop, bool, OnTryOpenMenu, ZGameUIManagerEntity* th, EGameUIMenu p_Menu, bool p_Force);
    std::atomic<int> m_LevelState { -1 };
    int m_LevelStateTold = -1;
    std::atomic<int> m_LastMenu { -1 };
    void UpdateLoadStage(double p_Now);
    // Our game's window in front or not (the game's own notice): the partner sees us as away.
    DECLARE_PLUGIN_DETOUR(HitmanCoop, void, OnWindowActivated, ZApplicationEngineWin32* th, bool p_Activated);
    std::atomic<bool> m_WindowActive { true };
    // Shots that hit the partner's stand-in since the last frame (no harm: their game decides).
    std::atomic<int> m_FriendlyHits { 0 };
    double m_FriendlyToldAt = -100.0;

    // Teamwork: ready checks, the 3-2-1 countdown, signals and "come to me" (as mission events).
    bool OnTeamEvent(const std::string& p_Name, const std::string& p_Json);
    void ReadyCheck(double p_Now);
    void StartCountdown(double p_Now);
    void SendSignal(double p_Now, int p_Signal);
    double m_ReadyAskedAt = -100.0;        // we asked
    double m_PartnerAskedReadyAt = -100.0; // they asked, not answered yet
    double m_CountdownEnd = -1.0;          // Now() clock; the UI draws it
    double m_LastCountdown = -100.0;
    double m_LastSignal = -100.0;
    double m_LastComeHere = -100.0;
    // The partner standing still: how long (they may have stepped away).
    coop::Vec3 m_PartnerLastPos {};
    double m_PartnerMovedAt = 0.0;

    // Co-op record, kept across sessions (under m_StatusMutex; saved by the UI thread).
    struct Record {
        int missions = 0;
        int silentAssassins = 0;
        int kills = 0;
        int targets = 0;
        int knockouts = 0;
        double seconds = 0.0;
    } m_Record;
    std::atomic<bool> m_SaveRequested { false };
    double m_RecordSavedAt = 0.0;

    // The end-of-mission summary, shown for a while (under m_StatusMutex).
    struct Summary {
        double at = -100.0;
        double seconds = 0.0;
        coop::MsgPlayerState ours, theirs;
        bool theirsKnown = false;
        std::string partner, silentAssassin;
    } m_Summary;

    // The session's status, taken once a frame (it walks every NPC's owner).
    coop::SessionStatus m_FrameStatus;

    // Our mission so far (from our game's events), shared with the partner along with what we are up to.
    coop::GameEvents::Tally m_Tally;
    double m_MissionStart = 0.0;
    double m_SyncedSince = -1.0;
    void UpdatePlayerState();
    void ShowDebrief();

    // Start together: our mission held paused while the partner's game loads into it.
    void UpdateWaitForPartner(double p_Now, const coop::SessionStatus& p_Status);
    double m_WaitEligibleUntil = 0.0;
    bool m_Waiting = false;
    double m_WaitStart = 0.0;
    double m_PeerBusySeenAt = -100.0;
    std::atomic<bool> m_SkipWait { false };
    static constexpr double c_WaitMax = 90.0;

    // Hotkeys (see Hotkeys.h, bound in the Hotkeys tab).
    void UpdateHotkeys(double p_Now);
    std::array<bool, Hotkeys::c_Count> m_BindDown {};
    double m_LastPing = -100.0;
    // The quick messages, as the player wrote them (under m_QuickMutex: the UI edits, hotkeys read).
    std::mutex m_QuickMutex;
    std::array<std::string, 8> m_QuickTexts;
    std::string QuickText(int p_Index);

    // The mod's own cost, and the game's frame rate: on a slow PC the mod scans NPCs less often.
    double m_FrameEma = 1.0 / 60.0;
    double m_SlowSince = -1.0;
    bool m_SlowMode = false;
    std::atomic<float> m_ModCostMs { 0.f };

    // Our previous session's crash report and log, kept for a bug report.
    std::string m_PreviousCrash;

    // Game thread state. Hooks may run on other threads, so session access is serialized.
    std::recursive_mutex m_SessionMutex;
    SdkWorld m_World;
    std::unique_ptr<coop::Session> m_Session;
    std::string m_GameVersion;
    std::string m_LanAddress;
    std::string m_TailscaleAddress;
    bool m_FrameUpdateRegistered = false;
    std::shared_ptr<const coop::RelayState> m_Relay; // the current session's relay, if any
    bool m_RelayCodeShown = false;
    bool m_WelcomeShown = false;
    bool m_UiTraced = false;
    double m_NextHeartbeat = 0.0;

    struct ClassProbe {
        std::string name;
        TResourcePtr<ZTemplateEntityFactory> factory;
    };
    std::list<ClassProbe> m_ClassProbes; // a list: TResourcePtr must never be moved or reassigned
    double m_ClassReportAt = 0.0;

    std::mutex m_InputMutex;
    double m_InputLogUntil = 0.0;
    std::set<std::string> m_InputSeen;
    std::string m_PressAction;
    bool m_PressPending = false;
    int64_t m_PressFrame = -1;
    int64_t m_InputFrame = 0;
    double m_PressExpires = 0.0;
    std::set<std::string> m_AnalogSeen;
    std::map<std::string, std::pair<float, double>> m_AnalogHolds; // action -> value, until
    bool m_RelayFailShown = false;

    void WatchRelay(double p_Now);
    void RunTestChannel(double p_Now);

    // Co-op extras on the game thread: marks, reconnecting, telling the partner when a load is over.
    void MarkAimed(double p_Now); // marks what our player aims at, for both screens (twice quickly: danger; again: taken back)
    bool KeyAllowed() const;      // our hotkeys count: the game has the focus, nobody types, no menu is open
    void UpdateReconnect(double p_Now);
    void UpdateBusy(double p_Now, double p_FrameGap);
    double m_LastMarkHint = -100.0;
    bool m_BusyTold = false;
    double m_BusyToldAt = 0.0;
    double m_LastFrame = 0.0;
    double m_SmoothSince = 0.0;
    std::map<std::string, double> m_AwareSentAt;
    double m_LastRegroup = -100.0;
    // The last join, retried when the connection is lost (not when refused or left).
    struct JoinMemory {
        std::string address, name, relay;
    };
    std::optional<JoinMemory> m_LastJoin;
    bool m_JoinedOnce = false; // connected with m_LastJoin at least once: its failures are worth retrying
    int m_ReconnectAttempt = 0;
    double m_ReconnectAt = 0.0;
    bool m_Reconnecting = false;

    // Render thread: the HUD, the partner's name tag and marks drawn over the game.
    void DrawHud(const coop::SessionStatus& p_Status, bool p_InMission, const SdkWorld::Overlay& p_Overlay, bool p_Reconnecting);
    void DrawOverlay(const SdkWorld::Overlay& p_Overlay, const coop::SessionStatus& p_Status);
    void DrawRadar(const SdkWorld::Overlay& p_Overlay, const coop::SessionStatus& p_Status, float p_Top);
    void DrawCompass(const SdkWorld::Overlay& p_Overlay, const coop::SessionStatus& p_Status);
    void DrawCountdown();
    void DrawSummary();
    void DrawToasts();
    void DrawWindow(const coop::SessionStatus& p_Status, bool p_InMission, const std::optional<coop::RelayState>& p_Relay);
    void DrawHeader(const coop::SessionStatus& p_Status, bool p_InMission);
    void DrawPlayTab(const coop::SessionStatus& p_Status, bool p_InMission, const std::optional<coop::RelayState>& p_Relay);
    void DrawChatTab(const coop::SessionStatus& p_Status);
    void DrawMissionTab(const coop::SessionStatus& p_Status);
    void DrawSettingsTab();
    void DrawHotkeysTab();
    void DrawAboutTab(const coop::SessionStatus& p_Status);
    void DrawHistory();
    std::string Diagnostics(const coop::SessionStatus& p_Status);
    std::string Hide(const std::string& p_Secret) const; // "••••••" in streamer mode
    void ApplySettings();                                // UI values to the game thread's copies
    size_t m_ChatSeen = 0;      // render thread: chat lines read (the Chat tab was open)
    int m_Capturing = -1;       // render thread: the action whose key is being bound
    std::string m_CopiedCode;   // render thread: the join code put on the clipboard
    std::atomic<bool> m_CodeCopied { false };
    SdkWorld::Overlay m_Overlay; // under m_StatusMutex
    bool m_OverlayReconnecting = false;
    // Also under m_StatusMutex, for the HUD: our mission stats, the start-together wait, the timer.
    coop::MsgPlayerState m_OverlayOurs;
    bool m_OverlayWaiting = false;
    double m_OverlayWaitLeft = 0.0;
    double m_OverlayTogether = -1.0; // seconds in the mission together, -1: not together
    double m_OverlayCountdownEnd = -1.0;
    int m_OverlayReady = 0;             // 1: we asked, waiting; 2: the partner asked us
    double m_OverlayPartnerStill = 0.0; // seconds the partner hasn't moved
    bool m_OverlayCanReconnect = false;
    float m_HudBottom = 0.f;          // render thread: where the HUD ended this frame
    std::deque<float> m_PingHistory;  // render thread
    double m_PingSampleAt = 0.0;
    std::atomic<float> m_ViewWidth { 0.f };
    std::atomic<float> m_ViewHeight { 0.f };
    std::atomic<bool> m_WantText { false };
    std::atomic<bool> m_OverlayOpen { false }; // the mod menu is open (its clicks aren't marks)
    char m_ChatBuffer[coop::kMaxChatLen] = "";

    TestChannel m_Test;

    // Requests from the UI (render thread), handled on the game thread.
    struct Command {
        enum class Kind { Host, Join, Leave, Chat, QuickChat, Regroup, Ping, ComeHere, ReadyCheck, Countdown, Signal, Kick, Reconnect } kind;
        std::string address;   // join: code or address
        uint16_t port = 0;
        std::string name;
        std::string relay;     // co-op server, empty for none
        int value = 0;         // signal: which
    };

    std::mutex m_CommandMutex;
    std::vector<Command> m_Commands;
    void PushCommand(const Command& p_Command);

    // Status for the UI, written on the game thread.
    std::mutex m_StatusMutex;
    coop::SessionStatus m_Status;
    bool m_InMission = false;
    std::optional<coop::RelayState> m_RelayStatus;

    // UI state (render thread).
    bool m_ShowWindow = false;
    char m_NameBuffer[64] = "Agent";
    char m_AddressBuffer[128] = "";
    char m_RelayBuffer[128] = "";
    int m_Port = coop::kDefaultPort;
    bool m_FollowHost = true;
    bool m_CorrectNpcs = true;
    float m_LocalBubble = 8.f;
    bool m_ShowToasts = true;
    bool m_ShowHud = true;
    bool m_ShowNameTag = true;
    bool m_ShowMarks = true;
    bool m_PauseTogether = true;
    bool m_AutoReconnect = true;
    bool m_ShowRadar = true;
    bool m_ShowCompass = false;
    bool m_TargetAssist = false;
    bool m_Streamer = false;
    bool m_AutoCopyCode = true;
    bool m_Feet = false;
    bool m_ColourBlind = false;
    bool m_MapNoPause = false;
    bool m_ToastKillFeed = true, m_ToastChallenges = true, m_ToastAlerts = true, m_ToastChat = true;
    float m_ToastSeconds = 8.f;
    float m_OverlayScale = 1.f;
    float m_WindowScale = 1.f;
    int m_RadarRange = 1;     // 25, 50 or 100 m
    float m_RadarSize = 1.f;
    int m_MaxMarks = 1;
    int m_MarkSeconds = 20;
    int m_SyncQuality = 1;    // low, normal, high
    float m_Colour[3] = { 0.43f, 0.78f, 1.f };
    char m_QuickBuffers[8][64] {};
    bool m_Sounds = true;
    bool m_Hotkeys = true;
    bool m_WaitForPartner = true;
    int m_HudCorner = 0;      // 0 top right, 1 top left, 2 bottom right, 3 bottom left
    float m_HudScale = 1.f;
    float m_HudOpacity = 0.45f;
    std::vector<std::string> m_RecentJoins; // the last few codes or addresses joined, newest first
    void RememberJoin(const std::string& p_Address);

    // Settings shared with the game thread.
    std::atomic<bool> m_PauseTogetherSetting { true };
    std::atomic<bool> m_SoundsSetting { true };
    std::atomic<bool> m_StreamerSetting { false };
    std::atomic<bool> m_MapNoPauseSetting { false };
    std::atomic<bool> m_TargetAssistSetting { false };
    std::atomic<bool> m_FeetSetting { false };
    std::atomic<int> m_SyncQualitySetting { 1 };
    std::atomic<int> m_MaxMarksSetting { 1 };
    std::atomic<int> m_MarkSecondsSetting { 20 };
    std::atomic<uint32_t> m_ColourSetting { 0 };
    std::atomic<uint32_t> m_ToastFilterSetting { 0xF };
    // Display toggles asked for by hotkeys (the UI owns those settings).
    std::atomic<bool> m_ToggleHudAsked { false };
    std::atomic<bool> m_ToggleRadarAsked { false };
    std::atomic<bool> m_HideOverlay { false };
    std::atomic<bool> m_HotkeysSetting { true };
    std::atomic<bool> m_WaitForPartnerSetting { true };
    std::atomic<bool> m_AutoReconnectSetting { true };
    std::atomic<bool> m_FollowHostSetting { true };
    std::atomic<bool> m_CorrectNpcsSetting { true };
    std::atomic<float> m_LocalBubbleSetting { 8.f };
};

DECLARE_ZHM_PLUGIN(HitmanCoop)
