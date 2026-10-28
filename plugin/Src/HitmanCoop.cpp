#include "HitmanCoop.h"
#include "CoopText.h"
#include "CrashTrace.h"
#include "CrashGuards.h"
#include "GamePause.h"
#include "PoseHook.h"
#include "TypeDump.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Functions.h"
#include "Globals.h"
#include "Hooks.h"
#include "IconsMaterialDesign.h"
#include "Logging.h"

#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZInput.h>
#include <Glacier/ZActor.h>
#include <Glacier/ZApplicationEngineWin32.h>
#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZScene.h>

#ifndef COOP_DEFAULT_RELAY
#define COOP_DEFAULT_RELAY ""
#endif

// Events from the game's achievement manager passed on as they are (kills and challenges go in a
// compact form of their own, see OnEventSent).
static bool ShouldForwardEvent(const std::string& p_Name, const std::string& /*p_Json*/) {
    return p_Name == "ContractEnd" || p_Name == "ContractFailed";
}

// Pulls "Name" out of the event JSON without needing a JSON library.
static std::string EventName(const std::string& p_Json) {
    const auto s_Key = p_Json.find("\"Name\":\"");

    if (s_Key == std::string::npos)
        return {};

    const auto s_Start = s_Key + 8;
    const auto s_End = p_Json.find('"', s_Start);
    return s_End == std::string::npos ? std::string {} : p_Json.substr(s_Start, s_End - s_Start);
}

static std::string ToLowerCopy(std::string p_Value) {
    for (auto& c : p_Value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return p_Value;
}

HitmanCoop::HitmanCoop() = default;

std::string HitmanCoop::ModsFolder() {
    char s_Exe[MAX_PATH] {};
    GetModuleFileNameA(nullptr, s_Exe, MAX_PATH);
    std::string s_Dir(s_Exe);
    s_Dir.resize(s_Dir.find_last_of("\\/") + 1);
    return s_Dir + "mods\\";
}

void HitmanCoop::AppendLog(const std::string& p_Line) {
    std::lock_guard s_Lock(m_LogMutex);

    // Capped, so a long session can't fill the disk.
    if (m_LogPath.empty() || m_LogBytes > 4 * 1024 * 1024)
        return;

    char s_Time[32];
    std::snprintf(s_Time, sizeof(s_Time), "[%9.2f] ", Now());

    if (std::ofstream s_File(m_LogPath, std::ios::app); s_File) {
        s_File << s_Time << p_Line << '\n';
        m_LogBytes += p_Line.size() + 13;
    }
}

HitmanCoop::~HitmanCoop() {
    PoseHook::Uninstall();
    CrashGuards::Uninstall();

    if (m_FrameUpdateRegistered) {
        const ZMemberDelegate<HitmanCoop, void(const SGameUpdateEvent&)> s_Delegate(this, &HitmanCoop::OnFrameUpdate);
        Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    }

    std::lock_guard s_Lock(m_SessionMutex);

    if (m_Session && m_Session->Active())
        m_Session->Leave("Game closed", Now());
}

void HitmanCoop::Init() {
    m_GameVersion = DetectGameVersion();
    m_LanAddress = coop::LocalLanAddress();
    m_TailscaleAddress = coop::LocalTailscaleAddress();
    LoadSettings();
    const int s_Patches = CrashGuards::Install();
    Logger::Info("[HitmanCoop] Game patches applied: {} of {} ({}).", s_Patches, CrashGuards::Count(), CrashGuards::Report());

    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &HitmanCoop::OnLoadScene);
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &HitmanCoop::OnClearScene);
    Hooks::ZActor_YouGotHit->AddDetour(this, &HitmanCoop::OnActorHit);
    Hooks::ZKnowledge_SetGameTension->AddDetour(this, &HitmanCoop::OnSetGameTension);
    Hooks::ZAchievementManagerSimple_OnEventSent->AddDetour(this, &HitmanCoop::OnEventSent);
    Hooks::ZLevelManager_SetGameState->AddDetour(this, &HitmanCoop::OnSetGameState);
    Hooks::ZGameUIManagerEntity_TryOpenMenu->AddDetour(this, &HitmanCoop::OnTryOpenMenu);
    Hooks::ZApplicationEngineWin32_OnMainWindowActivated->AddDetour(this, &HitmanCoop::OnWindowActivated);
    m_World.m_EventHook = [this](const std::string& p_Name, const std::string& p_Json) { return OnTeamEvent(p_Name, p_Json); };

    if (m_Test.Enabled()) {
        Hooks::ZInputAction_Digital->AddDetour(this, &HitmanCoop::OnInputDigital);
        Hooks::ZInputAction_Analog->AddDetour(this, &HitmanCoop::OnInputAnalog);
    }

    Logger::Info("[HitmanCoop] HITMAN Co-op v{} by {} loaded (protocol {}), game version {}.", c_ModVersion, c_Author,
        coop::kProtocolVersion, m_GameVersion);
    m_Test.Trace("init: HITMAN Co-op " + std::string(c_ModVersion) + ", game " + m_GameVersion + ", game patches " +
        std::to_string(s_Patches) + " of " + std::to_string(CrashGuards::Count()) + " (" + CrashGuards::Report() + ")");

    if (m_Test.Enabled()) {
        m_World.SetTrace([this](const std::string& p_Line) { m_Test.Trace(p_Line); });
        CrashTrace::Install(m_Test.Path() + ".crash");
    }
    else {
        // For players: what the mod did (a few lines a minute) and any crash, next to the game's
        // mods folder, to send along with a bug report.
        const std::string s_Dir = ModsFolder();
        m_LogPath = s_Dir + "HitmanCoop.log";

        // The last session's log, and its crash report if it had one, are kept for a bug report. A
        // report older than that session's last log line is from an earlier session: kept, not told.
        std::error_code s_Error;
        const std::string s_Crash = s_Dir + "HitmanCoop-crash.txt";

        if (std::filesystem::exists(s_Crash, s_Error)) {
            const auto s_CrashTime = std::filesystem::last_write_time(s_Crash, s_Error);
            const auto s_LogTime = std::filesystem::last_write_time(m_LogPath, s_Error);
            const bool s_Recent = s_Error || s_CrashTime + std::chrono::seconds(2) >= s_LogTime;
            std::filesystem::rename(s_Crash, s_Dir + "HitmanCoop-crash-previous.txt", s_Error);

            if (!s_Error && s_Recent)
                m_PreviousCrash = "mods\\HitmanCoop-crash-previous.txt";
        }

        std::filesystem::rename(m_LogPath, s_Dir + "HitmanCoop-previous.log", s_Error);

        std::ofstream(m_LogPath, std::ios::trunc) << "HITMAN Co-op " << c_ModVersion << ", game " << m_GameVersion << '\n';
        m_World.SetTrace([this](const std::string& p_Line) { AppendLog(p_Line); });
        CrashTrace::Install(s_Dir + "HitmanCoop-crash.txt");
    }
}

void HitmanCoop::OnEngineInitialized() {
    // Checked once the engine is up: some of these (the type registry) only exist from then on.
    m_Unsupported = MissingEngineParts();

    if (!m_Unsupported.empty()) {
        Logger::Error("[HitmanCoop] This game build lacks what co-op needs ({}); co-op stays off.", m_Unsupported);
        m_World.Notify("HITMAN Co-op can't run on this game version (" + m_GameVersion + "). Update the game and the mod.");
        m_Test.Trace("engine check: MISSING " + m_Unsupported);
    }
    else
        m_Test.Trace("engine check: everything co-op needs is there");

    if (!Globals::GameLoopManager)
        return;

    // eUpdateAlways keeps the connection alive while the game is paused or in menus.
    const ZMemberDelegate<HitmanCoop, void(const SGameUpdateEvent&)> s_Delegate(this, &HitmanCoop::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    m_FrameUpdateRegistered = true;
    m_Test.Trace("engine initialized, frame update registered");
}

double HitmanCoop::Now() {
    static const auto s_Frequency = [] {
        LARGE_INTEGER s_Value;
        QueryPerformanceFrequency(&s_Value);
        return static_cast<double>(s_Value.QuadPart);
    }();

    LARGE_INTEGER s_Counter;
    QueryPerformanceCounter(&s_Counter);
    return static_cast<double>(s_Counter.QuadPart) / s_Frequency;
}

std::string HitmanCoop::MissingEngineParts() {
    std::string s_Missing;
    const auto Need = [&](bool p_Found, const char* p_Name) {
        if (!p_Found)
            s_Missing += std::string(s_Missing.empty() ? "" : ", ") + p_Name;
    };

    Need(Globals::GameLoopManager != nullptr, "game loop");
    Need(Globals::TypeRegistry && *Globals::TypeRegistry, "type registry");
    Need(Globals::Hitman5Module != nullptr, "game module");
    Need(Globals::ActorManager != nullptr, "actor manager");
    Need(Globals::EntityManager != nullptr, "entity manager");
    Need(Globals::ResourceManager != nullptr, "resource manager");
    Need(Globals::ResourceContainer && *Globals::ResourceContainer, "resource container");
    Need(Globals::ContentKitManager != nullptr, "content kits");
    Need(Functions::ZEntityManager_NewEntity->Exists(), "entity spawning");
    Need(Functions::ZActor_SetOutfit->Exists(), "outfits");
    Need(Functions::ZActor_KillActor->Exists(), "NPC deaths");
    Need(Functions::ZDynamicObject_ToString->Exists(), "mission events");
    return s_Missing;
}

std::string HitmanCoop::DetectGameVersion() {
    wchar_t s_Path[MAX_PATH] {};

    if (!GetModuleFileNameW(nullptr, s_Path, MAX_PATH))
        return "unknown";

    DWORD s_Handle = 0;
    const DWORD s_Size = GetFileVersionInfoSizeW(s_Path, &s_Handle);

    if (s_Size == 0)
        return "unknown";

    std::vector<uint8_t> s_Data(s_Size);
    VS_FIXEDFILEINFO* s_Info = nullptr;
    UINT s_InfoLen = 0;

    if (!GetFileVersionInfoW(s_Path, 0, s_Size, s_Data.data()) ||
        !VerQueryValueW(s_Data.data(), L"\\", reinterpret_cast<void**>(&s_Info), &s_InfoLen) || !s_Info)
        return "unknown";

    char s_Version[64];
    std::snprintf(s_Version, sizeof(s_Version), "%u.%u.%u", HIWORD(s_Info->dwFileVersionMS),
        LOWORD(s_Info->dwFileVersionMS), HIWORD(s_Info->dwFileVersionLS));
    return s_Version;
}

void HitmanCoop::LoadSettings() {
    const ZString s_Name = GetSetting("general", "player_name", "Agent");
    const ZString s_Address = GetSetting("general", "last_address", "");
    const ZString s_Relay = GetSetting("general", "coop_server", COOP_DEFAULT_RELAY);

    strncpy_s(m_NameBuffer, s_Name.c_str(), _TRUNCATE);
    strncpy_s(m_AddressBuffer, s_Address.c_str(), _TRUNCATE);
    strncpy_s(m_RelayBuffer, s_Relay.c_str(), _TRUNCATE);

    m_Port = static_cast<int>(GetSettingInt("general", "port", coop::kDefaultPort));
    m_FollowHost = GetSettingBool("general", "follow_host_mission", true);
    m_CorrectNpcs = GetSettingBool("npcs", "correct_drift", true);
    m_LocalBubble = static_cast<float>(GetSettingDouble("npcs", "local_bubble_metres", 25.0));

    // 8 m (the old default) let you watch NPCs 10-20 m away slide through walls towards the
    // host's copies: saved 8s move up to the new default.
    if (m_LocalBubble > 7.99f && m_LocalBubble < 8.01f)
        m_LocalBubble = 25.f;

    m_ShowToasts = GetSettingBool("general", "show_notifications", true);
    m_ShowHud = GetSettingBool("general", "show_hud", true);
    m_ShowNameTag = GetSettingBool("general", "show_name_tag", true);
    m_ShowMarks = GetSettingBool("general", "show_marks", true);
    m_PauseTogether = GetSettingBool("general", "pause_together", true);
    m_AutoReconnect = GetSettingBool("general", "auto_reconnect", true);
    m_ShowRadar = GetSettingBool("general", "show_radar", true);
    m_Sounds = GetSettingBool("general", "sounds", true);
    m_Hotkeys = GetSettingBool("general", "hotkeys", true);
    m_WaitForPartner = GetSettingBool("general", "start_together", true);
    m_HudCorner = std::clamp(static_cast<int>(GetSettingInt("general", "hud_corner", 0)), 0, 3);
    m_HudScale = std::clamp(static_cast<float>(GetSettingDouble("general", "hud_scale", 1.0)), 0.75f, 1.75f);
    m_HudOpacity = std::clamp(static_cast<float>(GetSettingDouble("general", "hud_opacity", 0.45)), 0.f, 1.f);
    m_ShowCompass = GetSettingBool("general", "show_compass", false);
    m_TargetAssist = GetSettingBool("general", "target_assist", false);
    m_Streamer = GetSettingBool("general", "streamer_mode", false);
    m_AutoCopyCode = GetSettingBool("general", "copy_join_code", true);
    m_Feet = GetSettingBool("general", "feet", false);
    m_ColourBlind = GetSettingBool("general", "colour_blind", false);
    m_MapNoPause = GetSettingBool("general", "partner_map_no_pause", false);
    m_ToastKillFeed = GetSettingBool("notifications", "kill_feed", true);
    m_ToastChallenges = GetSettingBool("notifications", "challenges", true);
    m_ToastAlerts = GetSettingBool("notifications", "partner_alerts", true);
    m_ToastChat = GetSettingBool("notifications", "chat", true);
    m_ToastSeconds = std::clamp(static_cast<float>(GetSettingDouble("notifications", "seconds", 8.0)), 3.f, 20.f);
    m_OverlayScale = std::clamp(static_cast<float>(GetSettingDouble("general", "overlay_scale", 1.0)), 0.75f, 2.f);
    m_WindowScale = std::clamp(static_cast<float>(GetSettingDouble("general", "window_scale", 1.0)), 0.8f, 1.6f);
    m_RadarRange = std::clamp(static_cast<int>(GetSettingInt("general", "radar_range", 1)), 0, 2);
    m_RadarSize = std::clamp(static_cast<float>(GetSettingDouble("general", "radar_size", 1.0)), 0.6f, 2.f);
    m_MaxMarks = std::clamp(static_cast<int>(GetSettingInt("marks", "per_player", 1)), 1, 3);
    m_MarkSeconds = std::clamp(static_cast<int>(GetSettingInt("marks", "seconds", 20)), 10, 120);
    m_SyncQuality = std::clamp(static_cast<int>(GetSettingInt("npcs", "sync_quality", 1)), 0, 2);

    const uint32_t s_Colour = static_cast<uint32_t>(GetSettingInt("general", "colour", 0x6EC8FF)) & 0xFFFFFF;
    m_Colour[0] = ((s_Colour >> 16) & 0xFF) / 255.f;
    m_Colour[1] = ((s_Colour >> 8) & 0xFF) / 255.f;
    m_Colour[2] = (s_Colour & 0xFF) / 255.f;

    for (int i = 0; i < 8; ++i) {
        const ZString s_Text = GetSetting("quick_messages", ZString(std::string("message" + std::to_string(i + 1))), ZString(std::string(c_DefaultQuickMessages[i])));
        strncpy_s(m_QuickBuffers[i], s_Text.c_str(), _TRUNCATE);
    }

    // Hotkeys: each action's binding, or its default. 0.10's mark_key still counts.
    const ZString s_OldMark = GetSetting("general", "mark_key", "");

    for (int i = 0; i < Hotkeys::c_Count; ++i) {
        const auto s_Action = static_cast<Hotkeys::Action>(i);
        const auto& s_Info = Hotkeys::Describe(s_Action);
        std::string s_Fallback = s_Info.fallback;

        if (s_Action == Hotkeys::Action::Mark && s_OldMark.size() > 0)
            s_Fallback = std::string(s_OldMark.c_str(), s_OldMark.size());

        const ZString s_Text = GetSetting("hotkeys", ZString(std::string(std::string("key_") + s_Info.key)), ZString(std::string(s_Fallback)));
        Hotkeys::Bind s_Bind;
        Hotkeys::Parse(std::string(s_Text.c_str(), s_Text.size()), s_Bind);
        Hotkeys::Set(s_Action, s_Bind);
    }

    {
        const ZString s_Recent = GetSetting("general", "recent_joins", "");
        std::istringstream s_List(std::string(s_Recent.c_str(), s_Recent.size()));

        for (std::string s_Item; std::getline(s_List, s_Item, ';') && m_RecentJoins.size() < 5;)
            if (!s_Item.empty())
                m_RecentJoins.push_back(s_Item);
    }

    m_Record.missions = static_cast<int>(GetSettingInt("record", "missions", 0));
    m_Record.silentAssassins = static_cast<int>(GetSettingInt("record", "silent_assassins", 0));
    m_Record.kills = static_cast<int>(GetSettingInt("record", "kills", 0));
    m_Record.targets = static_cast<int>(GetSettingInt("record", "targets", 0));
    m_Record.knockouts = static_cast<int>(GetSettingInt("record", "knockouts", 0));
    m_Record.seconds = GetSettingDouble("record", "seconds_together", 0.0);

    ApplySettings();

    const ZString s_TestControl = GetSetting("general", "test_control", "");
    m_Test.SetPath(std::string(s_TestControl.c_str(), s_TestControl.size()));

    if (m_Test.Enabled())
        Logger::Info("[HitmanCoop] Test channel on: {}", s_TestControl.c_str());
}

void HitmanCoop::ApplySettings() {
    m_PauseTogetherSetting = m_PauseTogether;
    m_AutoReconnectSetting = m_AutoReconnect;
    m_FollowHostSetting = m_FollowHost;
    m_CorrectNpcsSetting = m_CorrectNpcs;
    m_LocalBubbleSetting = m_LocalBubble;
    m_SoundsSetting = m_Sounds;
    m_HotkeysSetting = m_Hotkeys;
    m_WaitForPartnerSetting = m_WaitForPartner;
    m_StreamerSetting = m_Streamer;
    m_MapNoPauseSetting = m_MapNoPause;
    m_TargetAssistSetting = m_TargetAssist;
    m_FeetSetting = m_Feet;
    m_SyncQualitySetting = m_SyncQuality;
    m_MaxMarksSetting = m_MaxMarks;
    m_MarkSecondsSetting = m_MarkSeconds;
    m_ToastFilterSetting = (m_ToastKillFeed ? 1u : 0u) | (m_ToastChallenges ? 2u : 0u) | (m_ToastAlerts ? 4u : 0u) | (m_ToastChat ? 8u : 0u);

    const auto Byte = [](float p_Value) { return static_cast<uint32_t>(std::clamp(p_Value, 0.f, 1.f) * 255.f + 0.5f); };
    m_ColourSetting = (Byte(m_Colour[0]) << 16) | (Byte(m_Colour[1]) << 8) | Byte(m_Colour[2]);

    std::lock_guard s_Lock(m_QuickMutex);

    for (int i = 0; i < 8; ++i)
        m_QuickTexts[i] = m_QuickBuffers[i][0] ? m_QuickBuffers[i] : c_DefaultQuickMessages[i];
}

std::string HitmanCoop::QuickText(int p_Index) {
    std::lock_guard s_Lock(m_QuickMutex);
    return m_QuickTexts[std::clamp(p_Index, 0, 7)];
}

// UI thread only (it reads the UI's own values).
void HitmanCoop::SaveSettings() {
    SetSetting("general", "player_name", m_NameBuffer);
    SetSetting("general", "last_address", m_AddressBuffer);
    SetSetting("general", "coop_server", m_RelayBuffer);
    SetSettingInt("general", "port", m_Port);
    SetSettingBool("general", "follow_host_mission", m_FollowHost);
    SetSettingBool("npcs", "correct_drift", m_CorrectNpcs);
    SetSettingDouble("npcs", "local_bubble_metres", m_LocalBubble);
    SetSettingBool("general", "show_notifications", m_ShowToasts);
    SetSettingBool("general", "show_hud", m_ShowHud);
    SetSettingBool("general", "show_name_tag", m_ShowNameTag);
    SetSettingBool("general", "show_marks", m_ShowMarks);
    SetSettingBool("general", "pause_together", m_PauseTogether);
    SetSettingBool("general", "auto_reconnect", m_AutoReconnect);
    SetSettingBool("general", "show_radar", m_ShowRadar);
    SetSettingBool("general", "sounds", m_Sounds);
    SetSettingBool("general", "hotkeys", m_Hotkeys);
    SetSettingBool("general", "start_together", m_WaitForPartner);
    SetSettingInt("general", "hud_corner", m_HudCorner);
    SetSettingDouble("general", "hud_scale", m_HudScale);
    SetSettingDouble("general", "hud_opacity", m_HudOpacity);
    SetSettingBool("general", "show_compass", m_ShowCompass);
    SetSettingBool("general", "target_assist", m_TargetAssist);
    SetSettingBool("general", "streamer_mode", m_Streamer);
    SetSettingBool("general", "copy_join_code", m_AutoCopyCode);
    SetSettingBool("general", "feet", m_Feet);
    SetSettingBool("general", "colour_blind", m_ColourBlind);
    SetSettingBool("general", "partner_map_no_pause", m_MapNoPause);
    SetSettingBool("notifications", "kill_feed", m_ToastKillFeed);
    SetSettingBool("notifications", "challenges", m_ToastChallenges);
    SetSettingBool("notifications", "partner_alerts", m_ToastAlerts);
    SetSettingBool("notifications", "chat", m_ToastChat);
    SetSettingDouble("notifications", "seconds", m_ToastSeconds);
    SetSettingDouble("general", "overlay_scale", m_OverlayScale);
    SetSettingDouble("general", "window_scale", m_WindowScale);
    SetSettingInt("general", "radar_range", m_RadarRange);
    SetSettingDouble("general", "radar_size", m_RadarSize);
    SetSettingInt("marks", "per_player", m_MaxMarks);
    SetSettingInt("marks", "seconds", m_MarkSeconds);
    SetSettingInt("npcs", "sync_quality", m_SyncQuality);
    SetSettingInt("general", "colour", static_cast<int64_t>(m_ColourSetting.load()));

    for (int i = 0; i < 8; ++i)
        SetSetting("quick_messages", ZString(std::string("message" + std::to_string(i + 1))), ZString(std::string(m_QuickBuffers[i])));

    for (int i = 0; i < Hotkeys::c_Count; ++i) {
        const auto s_Action = static_cast<Hotkeys::Action>(i);
        const std::string s_Name = Hotkeys::Name(Hotkeys::Get(s_Action));
        SetSetting("hotkeys", ZString(std::string(std::string("key_") + Hotkeys::Describe(s_Action).key)), ZString(std::string(s_Name.empty() ? std::string("OFF") : s_Name)));
    }

    std::string s_Recent;

    for (const auto& s_Item : m_RecentJoins)
        s_Recent += (s_Recent.empty() ? "" : ";") + s_Item;

    SetSetting("general", "recent_joins", ZString(std::string(s_Recent)));

    Record s_Record;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Record = m_Record;
    }

    SetSettingInt("record", "missions", s_Record.missions);
    SetSettingInt("record", "silent_assassins", s_Record.silentAssassins);
    SetSettingInt("record", "kills", s_Record.kills);
    SetSettingInt("record", "targets", s_Record.targets);
    SetSettingInt("record", "knockouts", s_Record.knockouts);
    SetSettingDouble("record", "seconds_together", s_Record.seconds);
}

void HitmanCoop::RememberJoin(const std::string& p_Address) {
    std::string s_Clean;

    for (const char c : p_Address)
        if (static_cast<unsigned char>(c) > 0x20 && c != ';')
            s_Clean += c;

    if (s_Clean.empty())
        return;

    std::erase(m_RecentJoins, s_Clean);
    m_RecentJoins.insert(m_RecentJoins.begin(), s_Clean);

    if (m_RecentJoins.size() > 5)
        m_RecentJoins.resize(5);
}

bool HitmanCoop::KeyAllowed() const {
    // Only while the game has the focus, nobody is typing (a letter key in the chat box), and no
    // game menu is open (a click in the pause menu isn't a mark).
    DWORD s_Process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Process);
    return s_Process == GetCurrentProcessId() && !m_WantText && !m_OverlayOpen && !GamePause::MenuOpen();
}

void HitmanCoop::PushCommand(const Command& p_Command) {
    std::lock_guard s_Lock(m_CommandMutex);
    m_Commands.push_back(p_Command);
}

void HitmanCoop::UpdateHotkeys(double p_Now) {
    using Hotkeys::Action;
    const bool s_Allowed = m_HotkeysSetting && KeyAllowed();
    const bool s_InSession = m_FrameStatus.state == coop::SessionState::Connected;
    std::array<Hotkeys::Bind, Hotkeys::c_Count> s_Binds;
    std::array<bool, Hotkeys::c_Count> s_Down {};

    for (int i = 0; i < Hotkeys::c_Count; ++i) {
        s_Binds[i] = Hotkeys::Get(static_cast<Action>(i));
        s_Down[i] = s_Allowed && Hotkeys::Down(s_Binds[i]);
    }

    for (int i = 0; i < Hotkeys::c_Count; ++i) {
        const bool s_Pressed = s_Down[i] && !m_BindDown[i];
        m_BindDown[i] = s_Down[i];

        if (!s_Pressed)
            continue;

        // A key also bound with a modifier: with that modifier held, only that binding counts.
        bool s_Shadowed = false;

        for (int j = 0; j < Hotkeys::c_Count && s_Binds[i].mods == 0; ++j)
            s_Shadowed |= j != i && s_Binds[j].vk == s_Binds[i].vk && s_Binds[j].mods != 0 && s_Down[j];

        if (s_Shadowed)
            continue;

        const auto s_Action = static_cast<Action>(i);

        // Showing and hiding works any time; the rest needs a partner.
        if (s_Action == Action::ToggleHud)
            m_ToggleHudAsked = true;
        else if (s_Action == Action::ToggleRadar)
            m_ToggleRadarAsked = true;
        else if (s_Action == Action::HideOverlay) {
            m_HideOverlay = !m_HideOverlay;
            m_World.Notify(m_HideOverlay ? "Co-op overlay hidden (screenshot mode). Press the key again to bring it back." : "Co-op overlay back.");
        }
        else if (s_Action == Action::Mark)
            MarkAimed(p_Now);
        else if (!s_InSession)
            continue;
        else if (s_Action >= Action::Quick1 && s_Action <= Action::Quick8)
            PushCommand({ Command::Kind::QuickChat, QuickText(i - static_cast<int>(Action::Quick1)) });
        else if (s_Action == Action::Regroup)
            PushCommand({ Command::Kind::Regroup });
        else if (s_Action == Action::WhereAreYou)
            PushCommand({ Command::Kind::Ping });
        else if (s_Action == Action::ComeHere)
            PushCommand({ Command::Kind::ComeHere });
        else if (s_Action == Action::ReadyCheck)
            PushCommand({ Command::Kind::ReadyCheck });
        else if (s_Action == Action::Countdown)
            PushCommand({ Command::Kind::Countdown });
        else if (s_Action >= Action::SignalOk && s_Action <= Action::SignalLook) {
            Command s_Signal { Command::Kind::Signal };
            s_Signal.value = i - static_cast<int>(Action::SignalOk);
            PushCommand(s_Signal);
        }
    }
}

void HitmanCoop::MarkAimed(double p_Now) {
    if (!m_World.IsInScene())
        return;

    if (!m_Session || !m_Session->Synced()) {
        if (p_Now - m_LastMarkHint > 5.0) {
            m_LastMarkHint = p_Now;
            m_World.Notify("Marks are shared once your partner is in the mission with you.");
        }

        return;
    }

    uint64_t s_Id = 0;
    coop::Vec3 s_Pos;
    std::string s_Label;

    if (!m_World.PickAimed(s_Id, s_Pos, s_Label)) {
        m_World.Notify("Nothing to mark there: aim at someone, or at something nearby.");
        return;
    }

    // The same person (or spot) again: twice quickly warns of danger there, later takes it back.
    double s_Age = 0.0;
    bool s_Danger = false;
    const bool s_Same = m_World.OurMarkOn(s_Id, s_Pos, s_Age, s_Danger);

    if (s_Same && !s_Danger && s_Age < 0.6) {
        m_World.AddMark(s_Id, s_Pos, true, s_Id ? s_Label : "danger", true);
        m_Session->SendMark(s_Id, s_Pos, coop::MarkKind_Danger);
        m_World.Notify("You warned " + m_FrameStatus.peerName + ": danger, " + s_Label + ".");
        return;
    }

    if (s_Same) {
        m_World.RemoveMark(true, s_Id, s_Pos);
        m_Session->SendMark(s_Id, s_Pos, coop::MarkKind_Remove);
        m_World.Notify("Mark taken back.");
        return;
    }

    m_World.AddMark(s_Id, s_Pos, true, s_Label);
    m_Session->SendMark(s_Id, s_Pos);
    m_World.Notify("You marked " + s_Label + ". Mark again quickly to warn of danger.");
}

// The partner's teamwork events (see the Coop_* names below); false for any other event.
bool HitmanCoop::OnTeamEvent(const std::string& p_Name, const std::string& p_Json) {
    const double s_Now = Now();
    const std::string& s_Partner = m_FrameStatus.peerName.empty() ? std::string("Your partner") : m_FrameStatus.peerName;
    const std::string s_ReadyKey = Hotkeys::Name(Hotkeys::Get(Hotkeys::Action::ReadyCheck));
    const std::string s_RegroupKey = Hotkeys::Name(Hotkeys::Get(Hotkeys::Action::Regroup));

    if (p_Name == "Coop_ReadyCheck") {
        m_PartnerAskedReadyAt = s_Now;
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Partner + " asks: ready? Press " + (s_ReadyKey.empty() ? std::string("I'm ready") : s_ReadyKey) +
            " (or the button in CO-OP) when you are.");
        return true;
    }

    if (p_Name == "Coop_Ready") {
        const bool s_WeAsked = s_Now - m_ReadyAskedAt < 30.0;
        m_ReadyAskedAt = -100.0;
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_WeAsked ? s_Partner + " is ready. You're both ready: go!" : s_Partner + " is ready.");
        return true;
    }

    if (p_Name == "Coop_Countdown") {
        // Started when they sent it: half a round trip ago, so both reach zero together.
        const double s_Seconds = std::clamp(std::atof(p_Json.c_str()), 1.0, 10.0);
        m_CountdownEnd = s_Now + s_Seconds - std::clamp(m_FrameStatus.stats.rttMs / 2000.0, 0.0, 0.5);
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Partner + " started a countdown.");
        return true;
    }

    if (p_Name == "Coop_Signal") {
        for (int i = 0; i < Signal_Count; ++i) {
            if (p_Json == c_SignalKeys[i]) {
                m_World.m_PartnerSignal = i;
                m_World.m_PartnerSignalAt = s_Now;
                m_World.NotifyAs(i == Signal_Help ? SdkWorld::ToastKind::Warning : SdkWorld::ToastKind::Partner, s_Partner + ": " + c_SignalWords[i]);

                if (i == Signal_Help || i == Signal_Look)
                    m_World.Pulse();
            }
        }

        return true;
    }

    if (p_Name == "Coop_ComeHere") {
        m_World.Pulse();
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Partner + " asks you to come to them: follow the arrow, or press " +
            (s_RegroupKey.empty() ? std::string("Regroup in CO-OP") : s_RegroupKey) + " to regroup.");
        return true;
    }

    if (p_Name == "Coop_FriendlyFire") {
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Partner + " hit you by mistake (no harm done here).");
        return true;
    }

    return false;
}

void HitmanCoop::ReadyCheck(double p_Now) {
    if (!m_Session || m_FrameStatus.state != coop::SessionState::Connected)
        return;

    // Answering theirs, or asking (once every few seconds).
    if (p_Now - m_PartnerAskedReadyAt < 60.0) {
        m_PartnerAskedReadyAt = -100.0;
        m_Session->SendMissionEvent("Coop_Ready", "");
        m_World.Notify("You told " + m_FrameStatus.peerName + " you're ready.");
        return;
    }

    if (p_Now - m_ReadyAskedAt < 5.0)
        return;

    m_ReadyAskedAt = p_Now;
    m_Session->SendMissionEvent("Coop_ReadyCheck", "");
    m_World.Notify("Ready check sent to " + m_FrameStatus.peerName + ".");
}

void HitmanCoop::StartCountdown(double p_Now) {
    if (!m_Session || m_FrameStatus.state != coop::SessionState::Connected || p_Now - m_LastCountdown < 5.0)
        return;

    m_LastCountdown = p_Now;
    m_CountdownEnd = p_Now + 3.0;
    m_Session->SendMissionEvent("Coop_Countdown", "3");
}

void HitmanCoop::SendSignal(double p_Now, int p_Signal) {
    if (!m_Session || !m_Session->Synced() || p_Signal < 0 || p_Signal >= Signal_Count || p_Now - m_LastSignal < 1.0)
        return;

    m_LastSignal = p_Now;
    m_Session->SendMissionEvent("Coop_Signal", c_SignalKeys[p_Signal]);
    m_World.Notify(std::string("You signalled: ") + c_SignalWords[p_Signal]);
}

void HitmanCoop::UpdateReconnect(double p_Now) {
    if (!m_Session)
        return;

    const auto s_Status = m_Session->Status();

    if (s_Status.state == coop::SessionState::Connected) {
        if (m_Reconnecting)
            m_World.NotifyAs(SdkWorld::ToastKind::Partner, "Reconnected.");

        m_Reconnecting = false;
        m_ReconnectAttempt = 0;
        m_ReconnectAt = 0.0;
        m_JoinedOnce = m_LastJoin.has_value();
        return;
    }

    if (s_Status.state != coop::SessionState::Failed)
        return; // still connecting: wait for how it ends

    // A dropped connection to a host we were playing with (not a refusal, not leaving) comes back
    // by itself: up to 6 tries in about a minute.
    constexpr double c_Waits[] = { 2.0, 4.0, 6.0, 10.0, 15.0, 20.0 };
    constexpr int c_Tries = static_cast<int>(sizeof(c_Waits) / sizeof(c_Waits[0]));

    if (!s_Status.lostConnection || !m_LastJoin || !m_JoinedOnce || !m_AutoReconnectSetting || m_ReconnectAttempt >= c_Tries) {
        m_Reconnecting = false;
        return;
    }

    if (m_ReconnectAt <= 0.0) {
        m_Reconnecting = true;
        m_ReconnectAt = p_Now + c_Waits[m_ReconnectAttempt];
        m_World.NotifyAs(SdkWorld::ToastKind::Warning, "Connection lost. Reconnecting in " +
            std::to_string(static_cast<int>(c_Waits[m_ReconnectAttempt])) + " s (try " + std::to_string(m_ReconnectAttempt + 1) +
            " of " + std::to_string(c_Tries) + ")...");
        return;
    }

    if (p_Now >= m_ReconnectAt) {
        ++m_ReconnectAttempt;
        m_ReconnectAt = 0.0;
        PushCommand({ Command::Kind::Join, m_LastJoin->address, 0, m_LastJoin->name, m_LastJoin->relay });
    }
}

void HitmanCoop::UpdateBusy(double p_Now, double p_FrameGap) {
    if (p_FrameGap > 0.5)
        m_SmoothSince = p_Now;

    // A load we told the partner about is over once frames have come smoothly for 2 s in a loaded scene.
    if (m_BusyTold && p_Now - m_BusyToldAt > 2.0 && p_Now - m_SmoothSince > 2.0 && *Globals::ApplicationEngineWin32 &&
        (*Globals::ApplicationEngineWin32)->m_bSceneLoaded) {
        m_BusyTold = false;

        if (m_Session)
            m_Session->NotifyBusy(p_Now, 0);
    }
}

void HitmanCoop::UpdateLoadStage(double p_Now) {
    const int s_State = m_LevelState;

    if (s_State == m_LevelStateTold)
        return;

    m_LevelStateTold = s_State;
    using State = ZLevelManager::EGameState;
    uint8_t s_Stage = coop::LoadStage_None;

    switch (static_cast<State>(s_State)) {
        case State::EGS_PreloadAssets:
        case State::EGS_WaitingForLoadVideo:
        case State::EGS_Precaching:
        case State::EGS_WaitingForPrecache: s_Stage = coop::LoadStage_Loading; break;
        case State::EGS_Preparing:
        case State::EGS_LoadSaveGame: s_Stage = coop::LoadStage_Preparing; break;
        case State::EGS_Activating:
        case State::EGS_ActivatedStart:
        case State::EGS_Activated: s_Stage = coop::LoadStage_Activating; break;
        case State::EGS_Playing: s_Stage = coop::LoadStage_None; break;
        case State::EGS_Deactivating: s_Stage = coop::LoadStage_Clearing; break;
        default: return;
    }

    m_Test.Trace("level: state " + std::to_string(s_State));

    if (!m_Session)
        return;

    // Playing: the load is over, the partner stops waiting at once (not 2 s of smooth frames later).
    if (s_Stage == coop::LoadStage_None) {
        if (m_BusyTold) {
            m_BusyTold = false;
            m_Session->NotifyBusy(p_Now, 0);
        }

        return;
    }

    // Still loading: the partner keeps waiting, and sees how far along it is.
    m_Session->NotifyBusy(p_Now, 120, s_Stage, m_World.CurrentScene().scene);
    m_BusyTold = true;
    m_BusyToldAt = p_Now;
}

void HitmanCoop::UpdatePlayerState() {
    coop::MsgPlayerState s_State = m_Tally.State();

    // A menu that pauses the game (the map, the notebook, the pause menu): which one it is.
    if (m_World.IsInScene() && GamePause::MenuOpen()) {
        const int s_Menu = m_LastMenu;
        s_State.menu = static_cast<uint8_t>(s_Menu >= 0 && s_Menu < static_cast<int>(std::size(c_MenuNames)) ? s_Menu + 1 :
            static_cast<int>(EGameUIMenu::eUIMenu_PauseMenu) + 1);
    }

    // Tabbed out: the game's own notice, or another window in front.
    DWORD s_Process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Process);

    if (!m_WindowActive || s_Process != GetCurrentProcessId())
        s_State.flags |= coop::PlayerFlag_Away;

    s_State.colour = m_ColourSetting;
    m_Session->SetLocalPlayerState(s_State);
}

void HitmanCoop::ShowDebrief() {
    const auto& s_Ours = m_Tally.State().stats;
    const auto& s_Status = m_FrameStatus;

    // The summary panel, and the co-op record when it was a mission together.
    {
        const std::string s_Broken = coop::GameEvents::SilentAssassinBroken(m_Tally.State(), s_Status.peerStateKnown ? s_Status.peerState : coop::MsgPlayerState {});
        std::lock_guard s_Lock(m_StatusMutex);
        m_Summary.at = Now();
        m_Summary.seconds = Now() - m_MissionStart;
        m_Summary.ours = m_Tally.State();
        m_Summary.theirs = s_Status.peerState;
        m_Summary.theirsKnown = s_Status.peerStateKnown;
        m_Summary.partner = s_Status.peerName;
        m_Summary.silentAssassin = s_Broken;

        if (s_Status.synced) {
            ++m_Record.missions;
            m_Record.silentAssassins += s_Broken.empty() ? 1 : 0;
            m_Record.kills += s_Ours.kills;
            m_Record.targets += s_Ours.targets;
            m_Record.knockouts += s_Ours.knockouts;
            m_SaveRequested = true;
        }
    }

    char s_Line[320];
    std::snprintf(s_Line, sizeof(s_Line), "Mission complete in %s. You: %u kills (%u targets), %u knocked out, spotted %u times.",
        Clock(Now() - m_MissionStart).c_str(), s_Ours.kills, s_Ours.targets, s_Ours.knockouts, s_Ours.spotted);
    m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Line);

    if (s_Status.peerStateKnown) {
        const auto& s_Theirs = s_Status.peerState.stats;
        std::snprintf(s_Line, sizeof(s_Line), "%s: %u kills (%u targets), %u knocked out, spotted %u times.", s_Status.peerName.c_str(),
            s_Theirs.kills, s_Theirs.targets, s_Theirs.knockouts, s_Theirs.spotted);
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Line);

        const std::string s_Broken = coop::GameEvents::SilentAssassinBroken(m_Tally.State(), s_Status.peerState);
        m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Broken.empty() ? "Team Silent Assassin: kept, as far as co-op can tell." :
            "Team Silent Assassin: lost (" + s_Broken + ").");
    }
}

void HitmanCoop::UpdateWaitForPartner(double p_Now, const coop::SessionStatus& p_Status) {
    const bool s_Connected = p_Status.state == coop::SessionState::Connected;

    if (p_Status.peerBusy)
        m_PeerBusySeenAt = p_Now;

    // The partner's game is loading this same location (they are coming), as our mission begins.
    const coop::SceneDesc s_Theirs { p_Status.peerBusyScene, {}, {} };
    const bool s_Coming = s_Connected && p_Status.peerBusy && p_Status.peerBusyStage != coop::LoadStage_Clearing &&
        s_Theirs.SameLocationAs(m_World.CurrentScene());

    if (!m_Waiting && m_WaitForPartnerSetting && m_World.IsInScene() && !p_Status.synced && p_Now < m_WaitEligibleUntil && s_Coming) {
        m_Waiting = true;
        m_WaitStart = p_Now;
        m_SkipWait = false;
        m_World.Notify("Waiting for " + p_Status.peerName + " to load in, so you start together. Open CO-OP to start without them.");
    }

    if (m_Waiting) {
        std::string s_Why;

        if (p_Status.synced)
            s_Why = p_Status.peerName + " is in. Go!";
        else if (!s_Connected || !m_World.IsInScene())
            s_Why = " ";
        else if (m_SkipWait)
            s_Why = "Starting without " + p_Status.peerName + ".";
        else if (p_Now - m_WaitStart > c_WaitMax)
            s_Why = p_Status.peerName + " is taking long: starting without them.";
        else if (p_Now - m_PeerBusySeenAt > 8.0)
            s_Why = p_Status.peerName + " finished loading somewhere else: starting without them.";

        if (!s_Why.empty()) {
            m_Waiting = false;
            m_WaitEligibleUntil = 0.0;

            if (s_Why != " ")
                m_World.NotifyAs(SdkWorld::ToastKind::Partner, s_Why);
        }
    }

    GamePause::SetWaitPause(m_Waiting);
}

void HitmanCoop::ProcessCommands(double p_Now) {
    std::vector<Command> s_Commands;

    {
        std::lock_guard s_Lock(m_CommandMutex);
        s_Commands.swap(m_Commands);
    }

    for (const auto& s_Command : s_Commands) {
        if (s_Command.kind == Command::Kind::Chat || s_Command.kind == Command::Kind::QuickChat) {
            if (m_Session && m_Session->Status().state == coop::SessionState::Connected) {
                m_Session->SendChat(s_Command.address, s_Command.kind == Command::Kind::QuickChat);
                m_World.AddChat(s_Command.address, true, s_Command.kind == Command::Kind::QuickChat);
            }
            else
                m_World.Notify("Not connected to a partner: your message wasn't sent.");

            continue;
        }

        if (s_Command.kind == Command::Kind::Ping) {
            if (!m_Session || !m_Session->Synced())
                m_World.Notify("Your partner isn't in the mission with you yet.");
            else if (p_Now - m_LastPing > 5.0) {
                m_LastPing = p_Now;
                m_Session->SendMissionEvent("Coop_Ping", "");
                m_World.Pulse();
                m_World.Notify("You asked " + m_Session->Status().peerName + " where they are: follow the arrow.");
            }

            continue;
        }

        if (s_Command.kind == Command::Kind::Regroup) {
            std::string s_Why;

            if (!m_Session || !m_Session->Synced())
                m_World.Notify("Regroup works once your partner is in the mission with you.");
            else if (p_Now - m_LastRegroup < 10.0)
                m_World.Notify("Regroup again in a few seconds.");
            else if (!m_World.RegroupToPartner(s_Why))
                m_World.Notify("Can't regroup: " + s_Why + ".");
            else {
                m_LastRegroup = p_Now;
                m_Session->SendMissionEvent("Coop_Regroup", "");
                m_World.Notify("You regrouped with " + m_Session->Status().peerName + ".");
            }

            continue;
        }

        if (s_Command.kind == Command::Kind::ComeHere) {
            if (!m_Session || !m_Session->Synced())
                m_World.Notify("Your partner isn't in the mission with you yet.");
            else if (p_Now - m_LastComeHere > 5.0) {
                m_LastComeHere = p_Now;
                m_Session->SendMissionEvent("Coop_ComeHere", "");
                m_World.Notify("You asked " + m_FrameStatus.peerName + " to come to you.");
            }

            continue;
        }

        if (s_Command.kind == Command::Kind::ReadyCheck) {
            ReadyCheck(p_Now);
            continue;
        }

        if (s_Command.kind == Command::Kind::Countdown) {
            StartCountdown(p_Now);
            continue;
        }

        if (s_Command.kind == Command::Kind::Signal) {
            SendSignal(p_Now, s_Command.value);
            continue;
        }

        if (s_Command.kind == Command::Kind::Kick) {
            if (m_Session)
                m_Session->KickPeer("the host ended your session", p_Now);

            continue;
        }

        if (s_Command.kind == Command::Kind::Reconnect) {
            // Now, instead of waiting out the next try.
            if (m_LastJoin && (!m_Session || m_Session->Status().state == coop::SessionState::Failed)) {
                m_ReconnectAttempt = 0;
                m_ReconnectAt = 0.0;
                m_Reconnecting = true;
                PushCommand({ Command::Kind::Join, m_LastJoin->address, 0, m_LastJoin->name, m_LastJoin->relay });
            }

            continue;
        }

        if (!m_Unsupported.empty() && s_Command.kind != Command::Kind::Leave) {
            m_World.Notify("Co-op is off: this game version (" + m_GameVersion + ") lacks " + m_Unsupported + ". Update the game and the mod.");
            continue;
        }

        if (m_Session && m_Session->Active())
            m_Session->Leave(s_Command.kind == Command::Kind::Leave ? "Left the session" : "Starting a new session", p_Now);

        if (s_Command.kind == Command::Kind::Leave) {
            m_World.Notify("You left the co-op session.");
            m_LastJoin.reset();
            m_Reconnecting = false;
            m_ReconnectAt = 0.0;
            continue;
        }

        if (s_Command.kind == Command::Kind::Join) {
            // A join of our own starts its retries afresh; a retry keeps counting.
            if (!m_Reconnecting) {
                m_ReconnectAttempt = 0;
                m_JoinedOnce = false;
            }

            m_LastJoin = JoinMemory { s_Command.address, s_Command.name, s_Command.relay };
        }
        else {
            m_LastJoin.reset();
            m_Reconnecting = false;
            m_ReconnectAt = 0.0;
        }

        coop::SessionConfig s_Config;
        s_Config.playerName = s_Command.name.empty() ? "Agent" : s_Command.name;
        s_Config.modVersion = c_ModVersion;
        s_Config.gameVersion = m_GameVersion;

        m_Session = std::make_unique<coop::Session>(m_World, s_Config);
        m_Relay.reset();
        m_RelayCodeShown = m_RelayFailShown = false;
        std::string s_Error;

        // The co-op server, if one is set and resolves.
        coop::Endpoint s_Server;
        std::string s_ServerError;
        const bool s_HaveServer = !s_Command.relay.empty() &&
            coop::ResolveEndpoint(s_Command.relay, coop::kDefaultRelayPort, s_Server, s_ServerError);

        if (s_Command.kind == Command::Kind::Host) {
            // Direct connections on the UDP port, and a join code from the co-op server.
            auto s_Udp = coop::UdpSocket::Open(s_Command.port, s_Error);

            if (!s_Udp) {
                m_World.Notify("Could not host: " + s_Error);
                m_Session.reset();
                continue;
            }

            std::unique_ptr<coop::RelaySocket> s_Relay;

            if (s_HaveServer)
                s_Relay = coop::RelaySocket::Host(s_Server, s_ServerError);

            if (s_Relay) {
                m_Relay = s_Relay->State();
                m_World.Notify("Hosting. Getting a join code from the co-op server...");
            }
            else {
                m_World.Notify(s_Command.relay.empty() ? "Hosting. No co-op server is set, so friends join by IP address." :
                    "Hosting, but the co-op server isn't available (" + s_ServerError + "). Friends can still join by IP address.");
            }

            m_Session->Host(std::make_unique<coop::MultiSocket>(std::move(s_Udp), std::move(s_Relay)));
        }
        else {
            std::string s_Code;
            std::unique_ptr<coop::IDatagramSocket> s_Socket;
            coop::Endpoint s_Host;

            if (coop::RelaySocket::NormalizeCode(s_Command.address, s_Code)) {
                if (!s_HaveServer) {
                    m_World.Notify(s_Command.relay.empty() ? "Can't use a join code: no co-op server is set (Settings)." :
                        "Can't reach the co-op server: " + s_ServerError);
                    m_Session.reset();
                    continue;
                }

                auto s_Relay = coop::RelaySocket::Join(s_Server, s_Code, s_Error);

                if (s_Relay) {
                    m_Relay = s_Relay->State();
                    s_Host = coop::Endpoint::Relay(coop::kRelayHostPeer);
                    s_Socket = std::move(s_Relay);
                    m_World.Notify("Joining with code " + s_Code + "...");
                }
            }
            else if (coop::ResolveEndpoint(s_Command.address, coop::kDefaultPort, s_Host, s_Error)) {
                s_Socket = coop::UdpSocket::Open(0, s_Error);
            }

            if (!s_Socket) {
                m_World.Notify("Could not join: " + s_Error);
                m_Session.reset();
                continue;
            }

            m_Session->Join(std::move(s_Socket), s_Host, p_Now);
        }
    }
}

void HitmanCoop::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    std::lock_guard s_Lock(m_SessionMutex);

    const double s_Now = Now();
    const double s_FrameGap = m_LastFrame > 0.0 ? s_Now - m_LastFrame : 0.0;
    m_LastFrame = s_Now;
    m_World.SetClock(s_Now);
    m_World.SetViewSize(m_ViewWidth, m_ViewHeight);
    m_World.SetPauseTogether(m_PauseTogetherSetting);
    m_World.SetSounds(m_SoundsSetting);
    m_World.SetFeet(m_FeetSetting);
    m_World.SetMarkRules(m_MarkSecondsSetting.load(), m_MaxMarksSetting.load());
    m_World.m_ListTargets = m_TargetAssistSetting;

    {
        const uint32_t s_Filter = m_ToastFilterSetting;
        m_World.SetToastFilter(s_Filter & 1, s_Filter & 2, s_Filter & 4, s_Filter & 8);
    }

    if (!m_WelcomeShown) {
        m_WelcomeShown = true;
        m_Test.Trace("first frame update");
        m_World.Notify(std::string("HITMAN Co-op ") + c_ModVersion + " by " + c_Author + " is ready. Press ~ and click CO-OP.");

        if (!m_PreviousCrash.empty())
            m_World.NotifyAs(SdkWorld::ToastKind::Warning, "The game closed with an error last time. If co-op was running, "
                "please send " + m_PreviousCrash + " to the mod's author.");
    }

    ProcessCommands(s_Now);

    const bool s_JustEntered = m_World.UpdateSceneState();

    // A new mission (or the same one again): its stats start afresh, and we may wait for the partner.
    if (s_JustEntered) {
        m_Tally.Reset();
        m_MissionStart = s_Now;
        m_WaitEligibleUntil = s_Now + 20.0;
    }

    UpdateLoadStage(s_Now);

    // Back in the mission that was cleared, without it being loaded again: rebuilt in place, a
    // restart (if the clear before it didn't already say so).
    if (s_JustEntered && !m_ClearedMission.empty()) {
        if (m_ClearedMission == m_World.CurrentScene().scene)
            OnMissionRestarted(m_ClearedMission);

        m_ClearedMission.clear();
    }

    if (m_Session) {
        if (s_JustEntered)
            m_Session->OnLocalSceneLoaded();

        auto& s_Policy = m_Session->Config().npcPolicy;
        s_Policy.localBubble = m_LocalBubbleSetting;
        s_Policy.minDrift = m_CorrectNpcsSetting ? 0.75f : 1e9f;
        s_Policy.snapDrift = m_CorrectNpcsSetting ? 6.f : 1e9f;

        // On a slow frame rate, walking the level's NPCs (the mod's costliest work) runs half as often.
        auto& s_Config = m_Session->Config();
        s_Config.npcScanRate = m_SlowMode ? 15.0 : 30.0;

        // How much NPC detail goes to the partner (Settings): less for a slow connection, more for a LAN.
        static constexpr struct {
            double npc, pose;
            size_t poses;
            float range;
        } c_Quality[] = { { 1.0 / 10.0, 1.0 / 8.0, 5, 25.f }, { 1.0 / 15.0, 1.0 / 12.0, 10, 35.f }, { 1.0 / 20.0, 1.0 / 15.0, 14, 45.f } };
        const auto& s_Quality = c_Quality[std::clamp(m_SyncQualitySetting.load(), 0, 2)];
        s_Config.npcInterval = s_Quality.npc;
        s_Config.npcPoseInterval = s_Quality.pose;
        s_Config.npcPoseMax = s_Quality.poses;
        s_Config.npcPoseRange = s_Quality.range;
        UpdatePlayerState();

        const float s_Dt = static_cast<float>(std::min(p_UpdateEvent.m_RealTimeDelta.ToSeconds(), 0.25));

        // Whatever goes wrong in the session (out of memory, a bug), the game goes on without it.
        try {
            m_Session->Tick(s_Now, s_Dt);
        }
        catch (const std::exception& s_Error) {
            Logger::Error("[HitmanCoop] Session error: {}", s_Error.what());
            AppendLog(std::string("session error: ") + s_Error.what());
            // Leaving puts the partner's NPCs back in our game's hands and takes the stand-in away.
            try {
                m_Session->Leave("Co-op stopped after an error", s_Now);
            }
            catch (...) {
            }

            m_Session.reset();
            m_World.NotifyAs(SdkWorld::ToastKind::Warning, "Co-op hit an internal error and stopped (your game is fine). Host or join again.");
        }

        if (m_Session) {
            m_FrameStatus = m_Session->Status();
            m_World.SetPartnerName(m_FrameStatus.peerName.empty() ? "Partner" : m_FrameStatus.peerName);
            WatchRelay(s_Now);
        }

        if (s_JustEntered)
            m_BusyTold = false; // the session told the partner already
    }

    if (!m_Session)
        m_FrameStatus = {};

    UpdateBusy(s_Now, s_FrameGap);
    UpdateHotkeys(s_Now);
    UpdateReconnect(s_Now);
    m_World.UpdateMarks();

    // Only the partner's map, notebook or phone (not their pause menu): ours goes on, if asked.
    {
        const int s_Menu = m_FrameStatus.peerStateKnown ? m_FrameStatus.peerState.menu - 1 : -1;
        const bool s_Browsing = s_Menu == static_cast<int>(EGameUIMenu::eUIMenu_MapMenu) || s_Menu == static_cast<int>(EGameUIMenu::eUIMenu_NotebookMenu) ||
            s_Menu == static_cast<int>(EGameUIMenu::eUIMenu_LogbookMenu) || s_Menu == static_cast<int>(EGameUIMenu::eUIMenu_IntelSelectorMenu) ||
            s_Menu == static_cast<int>(EGameUIMenu::eUIMenu_PhoneMenu);
        m_World.SetPartnerMenuIgnored(m_MapNoPauseSetting && s_Browsing);
    }

    m_World.UpdatePauseCap();
    UpdateWaitForPartner(s_Now, m_FrameStatus);

    // Shots that hit the partner's stand-in: both are told, once in a while.
    if (const int s_Hits = m_FriendlyHits.exchange(0); s_Hits > 0 && s_Now - m_FriendlyToldAt > 6.0 && m_FrameStatus.synced) {
        m_FriendlyToldAt = s_Now;
        m_World.NotifyAs(SdkWorld::ToastKind::Warning, "You hit " + m_FrameStatus.peerName + "'s stand-in. No harm done: their own game decides what hits them.");
        m_Session->SendMissionEvent("Coop_FriendlyFire", "");
    }

    // Time together, for the co-op record (saved every couple of minutes, and at each mission's end).
    if (m_FrameStatus.synced) {
        std::lock_guard s_StatusLock(m_StatusMutex);
        m_Record.seconds += std::min(s_FrameGap, 0.25);

        if (s_Now - m_RecordSavedAt > 120.0) {
            m_RecordSavedAt = s_Now;
            m_SaveRequested = true;
        }
    }

    if (m_CodeCopied.exchange(false))
        m_World.Notify("Join code copied: paste it to your friend.");

    m_World.UpdatePoses();
    PublishStatus();
    RunTestChannel(s_Now);

    // The frame rate in a mission (a slow PC, or a heavy scene): below about 28 fps for 3 s the mod
    // does less; back above 40 fps it does it all again.
    if (m_World.IsInScene()) {
        m_FrameEma = m_FrameEma * 0.95 + std::min(s_FrameGap, 0.25) * 0.05;

        if (m_FrameEma > 1.0 / 28.0) {
            if (m_SlowSince < 0.0)
                m_SlowSince = s_Now;

            if (!m_SlowMode && s_Now - m_SlowSince > 3.0) {
                m_SlowMode = true;
                m_Test.Trace("load: slow frames, scanning NPCs less often");
            }
        }
        else if (m_FrameEma < 1.0 / 40.0) {
            m_SlowSince = -1.0;
            m_SlowMode = false;
        }
    }

    m_ModCostMs = static_cast<float>(m_ModCostMs * 0.95 + (Now() - s_Now) * 1000.0 * 0.05);

    // A heartbeat in the trace, so the last line before a crash says when it happened.
    if (m_Test.Enabled() && s_Now >= m_NextHeartbeat) {
        m_NextHeartbeat = s_Now + 1.0;
        const auto s_State = m_Session ? coop::SessionStateName(m_Session->Status().state) : "Idle";
        m_Test.Trace("beat " + std::to_string(static_cast<long long>(s_Now)) + " " + s_State + " partner " +
            m_World.DescribePartner());
    }
}

void HitmanCoop::RunTestChannel(double p_Now) {
    if (!m_Test.Enabled())
        return;

    {
        std::lock_guard s_InputLock(m_InputMutex);
        ++m_InputFrame;

        // A press nobody asked about for 2 s is dropped.
        if (m_PressPending && p_Now > m_PressExpires)
            m_PressPending = false;
    }

    const std::string s_Command = m_Test.Poll(p_Now);

    if (!s_Command.empty()) {
        Logger::Info("[HitmanCoop] Test command: {}", s_Command);
        const auto s_Space = s_Command.find(' ');
        const std::string s_Verb = s_Command.substr(0, s_Space);
        const std::string s_Arg = s_Space == std::string::npos ? "" : s_Command.substr(s_Space + 1);
        std::lock_guard s_Lock(m_CommandMutex);

        if (s_Verb == "host")
            m_Commands.push_back({ Command::Kind::Host, {}, static_cast<uint16_t>(m_Port), "Test host", m_RelayBuffer });
        else if (s_Verb == "join")
            m_Commands.push_back({ Command::Kind::Join, s_Arg, 0, "Test client", m_RelayBuffer });
        else if (s_Verb == "leave")
            m_Commands.push_back({ Command::Kind::Leave });
        else if (s_Verb == "relay")
            strncpy_s(m_RelayBuffer, s_Arg.c_str(), _TRUNCATE);
        else if (s_Verb == "mark")
            MarkAimed(p_Now);
        else if (s_Verb == "dangermark") {
            // The mark key pressed twice in quick succession.
            MarkAimed(p_Now);
            MarkAimed(p_Now);
        }
        else if (s_Verb == "chat")
            m_Commands.push_back({ Command::Kind::Chat, s_Arg });
        else if (s_Verb == "regroup")
            m_Commands.push_back({ Command::Kind::Regroup });
        else if (s_Verb == "ping")
            m_Commands.push_back({ Command::Kind::Ping });
        else if (s_Verb == "quick")
            m_Commands.push_back({ Command::Kind::QuickChat, QuickText(std::atoi(s_Arg.c_str()) - 1) });
        else if (s_Verb == "skipwait")
            m_SkipWait = true;
        else if (s_Verb == "readycheck")
            m_Commands.push_back({ Command::Kind::ReadyCheck });
        else if (s_Verb == "countdown")
            m_Commands.push_back({ Command::Kind::Countdown });
        else if (s_Verb == "comehere")
            m_Commands.push_back({ Command::Kind::ComeHere });
        else if (s_Verb == "signal") {
            Command s_Signal { Command::Kind::Signal };
            s_Signal.value = std::atoi(s_Arg.c_str());
            m_Commands.push_back(s_Signal);
        }
        else if (s_Verb == "friendlyhit")
            ++m_FriendlyHits;
        else if (s_Verb == "summary")
            ShowDebrief();
        else if (s_Verb == "hideoverlay")
            m_HideOverlay = s_Arg != "off";
        else if (s_Verb == "openwindow")
            m_ShowWindow = s_Arg != "off";
        else if (s_Verb == "diag")
            m_Test.Trace("diag: " + Diagnostics(m_Session ? m_Session->Status() : coop::SessionStatus {}));
        else if (s_Verb == "restart")
            m_Test.Trace(GamePause::Restart() ? "test: restarted as the pause menu does" : "test: RestartLevel not found");
        else if (s_Verb == "npcs")
            m_Test.Trace(m_World.DescribeNotAliveNpcs());
        else if (s_Verb == "classes") {
            // Asks the game for each class's factory, then traces its load status twice.
            std::istringstream s_Names(s_Arg);

            for (std::string s_Name; s_Names >> s_Name;) {
                std::string s_Lower = s_Name;
                std::transform(s_Lower.begin(), s_Lower.end(), s_Lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto& s_Probe = m_ClassProbes.emplace_back();
                s_Probe.name = s_Name;
                Globals::ResourceManager->LoadResource(s_Probe.factory, ZRuntimeResourceID::FromString("[modules:/" + s_Lower + ".class].pc_entitytype"));
            }

            m_ClassReportAt = p_Now + 2.0;
        }
        else if (s_Verb == "animators") {
            std::ofstream s_File(m_Test.Path() + ".bones", std::ios::app);
            s_File << m_World.DumpAnimators();
        }
        else if (s_Verb == "bones") {
            std::ofstream s_File(m_Test.Path() + ".bones", std::ios::app);
            s_File << "##### t=" << p_Now << "\n" << m_World.DumpBones(s_Arg.empty() ? 0x200 : std::stoul(s_Arg, nullptr, 0));
        }
        else if (s_Verb == "types")
            TypeDump::Write(s_Arg, m_Test.Path() + ".types");
        else if (s_Verb == "actorai")
            m_Test.Trace(m_World.DescribeActorAi());
        else if (s_Verb == "partner")
            m_World.SetPartnerEnabled(s_Arg != "off");
        else if (s_Verb == "mirror")
            m_World.SetMirror(s_Arg == "on");
        else if (s_Verb == "victimtest")
        {
            m_World.SetVictimTest(s_Arg != "off");
            m_World.SetVictimTestApply(s_Arg == "pose" ? 1 : s_Arg == "move" ? 2 : 0);
        }
        else if (s_Verb == "rigdump") {
            // The stand-in's own pose next to what it is given, first 40 bones.
            std::vector<PoseHook::Bone> s_Native;
            PoseHook::TargetNativePose(s_Native);
            std::ofstream s_File(m_Test.Path() + ".bones", std::ios::app);
            s_File << "##### rigdump t=" << p_Now << " native " << s_Native.size() << "\n";

            for (size_t i = 0; i < s_Native.size() && i < 40; ++i) {
                char s_Line[160];
                std::snprintf(s_Line, sizeof(s_Line), "native %2zu %7.3f %7.3f %7.3f %7.3f  pos %7.3f %7.3f %7.3f\n", i, s_Native[i].rot[0],
                    s_Native[i].rot[1], s_Native[i].rot[2], s_Native[i].rot[3], s_Native[i].pos[0], s_Native[i].pos[1], s_Native[i].pos[2]);
                s_File << s_Line;
            }

            s_File << m_World.DumpRemoteBones(40);
        }
        else if (s_Verb == "posedump") {
            std::vector<PoseHook::Bone> s_Bones;
            std::ofstream s_File(m_Test.Path() + ".bones", std::ios::app);

            if (!PoseHook::TakeSourcePose(s_Bones))
                s_File << "posedump: no fresh source pose\n";

            for (size_t i = 0; i < s_Bones.size() && i < 12; ++i) {
                char s_Line[200];
                std::snprintf(s_Line, sizeof(s_Line), "bone %2zu rot %7.3f %7.3f %7.3f %7.3f  pos %7.3f %7.3f %7.3f %7.3f\n", i,
                    s_Bones[i].rot[0], s_Bones[i].rot[1], s_Bones[i].rot[2], s_Bones[i].rot[3], s_Bones[i].pos[0],
                    s_Bones[i].pos[1], s_Bones[i].pos[2], s_Bones[i].pos[3]);
                s_File << s_Line;
            }
        }
        else if (s_Verb == "takedown") {
            int s_Event = 2, s_Behavior = 1;
            std::sscanf(s_Arg.c_str(), "%d %d", &s_Event, &s_Behavior);
            m_Test.Trace(m_World.TakedownTest(s_Event, s_Behavior));
        }
        else if (s_Verb == "knockout") {
            unsigned s_Type = 8, s_Context = 1;
            std::sscanf(s_Arg.c_str(), "%u %u", &s_Type, &s_Context);
            m_Test.Trace(m_World.KnockOutTest(s_Type, s_Context));
        }
        else if (s_Verb == "victim")
            m_Test.Trace("victim: " + m_World.DescribeVictim());
        else if (s_Verb == "guards")
            m_World.AddPartnerGuards();
        else if (s_Verb == "herosuit")
            m_World.SetHeroSuitStandIn(s_Arg != "off");
        else if (s_Verb == "noguards")
            m_World.SetGuardOnSpawn(false);
        else if (s_Verb == "rigscan")
            m_Test.Trace(m_World.RigScanTest());
        else if (s_Verb == "doors")
            m_Test.Trace(m_World.DoorTest());
        else if (s_Verb == "doortoggle")
            m_Test.Trace(m_World.DoorToggleTest());
        else if (s_Verb == "gotodoor")
            m_Test.Trace(m_World.GoToDoorTest());
        else if (s_Verb == "doorstate")
            m_Test.Trace(m_World.DoorState());
        else if (s_Verb == "itemtake" || s_Verb == "itemback")
            m_Test.Trace(m_World.ItemTest(s_Verb == "itemtake"));
        else if (s_Verb == "victimsend")
            m_World.SetVictimSend(s_Arg != "off");
        else if (s_Verb == "behind")
            m_Test.Trace("behind: " + m_World.StepBehindNearestNpc(s_Arg.empty() ? 0.7f : std::stof(s_Arg)));
        else if (s_Verb == "calmedfrom")
            m_Test.Trace("calmed from: " + CrashGuards::CalmedFrom());
        else if (s_Verb == "brainless")
            m_World.SetBrainless(std::atoi(s_Arg.c_str()));
        else if (s_Verb == "standin")
            m_Test.Trace(m_World.StandInTest(s_Arg));
        else if (s_Verb == "lonely")
            m_Test.Trace("lonely: " + m_World.StepBehindNearestNpc(s_Arg.empty() ? 0.7f : std::stof(s_Arg), true));
        else if (s_Verb == "nearest")
            m_Test.Trace("nearest: " + m_World.DescribeNearestNpc());
        else if (s_Verb == "poseidentity")
            m_World.SetPoseIdentity(s_Arg == "on");
        else if (s_Verb == "puppet")
            m_World.SetPuppetMode(s_Arg == "on");
        else if (s_Verb == "tracklog") {
            std::remove((m_Test.Path() + ".track.csv").c_str());
            m_World.StartTrackLog(m_Test.Path() + ".track.csv", s_Arg.empty() ? 10.0 : std::atof(s_Arg.c_str()));
        }
        else if (s_Verb == "repromote")
            m_World.Puppet().SetRepromoteEvery(std::atof(s_Arg.c_str()));
        else if (s_Verb == "hold") {
            // hold <analog action> <value> <seconds>: the action reads as that value meanwhile.
            std::istringstream s_Args(s_Arg);
            std::string s_Action;
            float s_Value = 0.f, s_Seconds = 1.f;
            s_Args >> s_Action >> s_Value >> s_Seconds;
            std::lock_guard s_InputLock(m_InputMutex);
            m_AnalogHolds[s_Action] = { s_Value, p_Now + s_Seconds };
        }
        else if (s_Verb == "inputlog") {
            std::lock_guard s_InputLock(m_InputMutex);
            m_InputSeen.clear();
            m_AnalogSeen.clear();
            m_InputLogUntil = p_Now + 3.0;
        }
        else if (s_Verb == "press") {
            // Menus act on every frame an action reads as pressed, so a press is down for exactly
            // one frame: the first frame the game asks about that action.
            std::lock_guard s_InputLock(m_InputMutex);
            m_PressAction = s_Arg;
            m_PressPending = true;
            m_PressFrame = -1;
            m_PressExpires = p_Now + 2.0;
        }
    }

    {
        std::lock_guard s_InputLock(m_InputMutex);

        if (m_InputLogUntil > 0.0 && p_Now >= m_InputLogUntil) {
            std::string s_Names;

            for (const auto& s_Name : m_InputSeen)
                s_Names += s_Name + " ";

            m_Test.Trace("input actions: " + s_Names);
            s_Names.clear();

            for (const auto& s_Name : m_AnalogSeen)
                s_Names += s_Name + " ";

            m_Test.Trace("analog actions: " + s_Names);
            m_InputLogUntil = 0.0;
        }
    }

    if (m_ClassReportAt > 0.0 && p_Now >= m_ClassReportAt) {
        m_ClassReportAt = 0.0;
        std::string s_Report = "classes:";

        for (const auto& s_Probe : m_ClassProbes) {
            const int s_Status = s_Probe.factory.m_nResourceIndex.val < 0 ? -1 : static_cast<int>(s_Probe.factory.GetResourceInfo().status);
            s_Report += " " + s_Probe.name + "=" + std::to_string(s_Status);
        }

        m_Test.Trace(s_Report);
    }

    if (!m_Test.StatusDue(p_Now))
        return;

    const auto s_Status = m_Session ? m_Session->Status() : coop::SessionStatus {};
    const auto s_Scene = m_World.CurrentScene();
    coop::Transform s_Local;
    const bool s_HaveLocal = m_World.GetLocalAvatar(s_Local);

    std::ostringstream s_Json;
    s_Json << "{\"time\":" << p_Now
           << ",\"version\":\"" << c_ModVersion << "\""
           << ",\"game\":\"" << TestChannel::Escape(m_GameVersion) << "\""
           << ",\"role\":" << static_cast<int>(s_Status.role)
           << ",\"state\":\"" << coop::SessionStateName(s_Status.state) << "\""
           << ",\"message\":\"" << TestChannel::Escape(s_Status.message) << "\""
           << ",\"synced\":" << (s_Status.synced ? "true" : "false")
           << ",\"peer\":\"" << TestChannel::Escape(s_Status.peerName) << "\""
           << ",\"pingMs\":" << s_Status.stats.rttMs
           << ",\"npcsTracked\":" << s_Status.npcsTracked
           << ",\"inScene\":" << (m_World.IsInScene() ? "true" : "false")
           << ",\"scene\":\"" << TestChannel::Escape(s_Scene.scene) << "\""
           << ",\"location\":\"" << TestChannel::Escape(s_Scene.Location()) << "\""
           << ",\"bricks\":" << s_Scene.bricks.size()
           << ",\"npcsListed\":" << m_World.LastNpcCount()
           << ",\"partnerSpawned\":" << (m_World.HasPartner() ? "true" : "false")
           << ",\"partnerSpawnFailed\":" << (m_World.PartnerSpawnFailed() ? "true" : "false");

    if (s_HaveLocal)
        s_Json << ",\"local\":[" << s_Local.pos.x << "," << s_Local.pos.y << "," << s_Local.pos.z << "]";

    if (coop::Vec3 s_Partner; m_World.GetPartnerPosition(s_Partner))
        s_Json << ",\"partner\":[" << s_Partner.x << "," << s_Partner.y << "," << s_Partner.z << "]";

    {
        const coop::Vec3 s_Wanted = m_World.PartnerWanted();
        s_Json << ",\"partnerWanted\":[" << s_Wanted.x << "," << s_Wanted.y << "," << s_Wanted.z << "]";
    }

    s_Json << ",\"partnerActor\":\"" << TestChannel::Escape(m_World.DescribePartner()) << "\"";
    s_Json << ",\"shared\":\"puppets " << s_Status.npcsPuppeted << ", client owns " << s_Status.npcsOwnedByClient << ", " <<
        TestChannel::Escape(s_Status.ownerCheck) << "\"";
    s_Json << ",\"pose\":{\"hooked\":" << (PoseHook::Installed() ? "true" : "false") << ",\"calls\":" << PoseHook::Calls()
           << ",\"sourceBones\":" << PoseHook::SourceBoneCount() << ",\"targetBones\":" << PoseHook::TargetBoneCount() << ",\"received\":" << s_Status.posesReceived
           << ",\"partnerPosed\":" << (m_World.PartnerPosed() ? "true" : "false") << "}";
    s_Json << ",\"victims\":\"" << TestChannel::Escape(m_World.DescribeVictims()) << "\"";
    s_Json << ",\"v10\":{\"peerKnown\":" << (s_Status.peerStateKnown ? "true" : "false") << ",\"peerThreat\":" << int(s_Status.peerState.threat)
           << ",\"peerFlags\":" << int(s_Status.peerState.flags) << ",\"peerMenu\":" << int(s_Status.peerState.menu)
           << ",\"peerKills\":" << s_Status.peerState.stats.kills << ",\"ourKills\":" << m_Tally.State().stats.kills
           << ",\"ourChallenges\":" << m_Tally.State().stats.challenges << ",\"ourThreat\":" << int(m_Tally.State().threat)
           << ",\"waiting\":" << (m_Waiting ? "true" : "false") << ",\"level\":" << m_LevelState.load() << ",\"lastMenu\":" << m_LastMenu.load()
           << ",\"loss\":" << s_Status.stats.lossPercent << ",\"jitter\":" << s_Status.stats.jitterMs << ",\"silence\":" << s_Status.peerSilence
           << ",\"slow\":" << (m_SlowMode ? "true" : "false") << ",\"modCostMs\":" << m_ModCostMs.load()
           << ",\"disguise\":\"" << TestChannel::Escape(m_World.PartnerDisguise()) << "\"}";
    s_Json << ",\"v11\":{\"peerColour\":" << s_Status.peerState.colour << ",\"peerAway\":" << ((s_Status.peerState.flags & coop::PlayerFlag_Away) ? "true" : "false")
           << ",\"peerMod\":\"" << TestChannel::Escape(s_Status.peerModVersion) << "\",\"countdownLeft\":" << (m_CountdownEnd - p_Now)
           << ",\"partnerSignal\":" << m_World.m_PartnerSignal << ",\"missions\":" << m_Record.missions
           << ",\"hidden\":" << (m_HideOverlay ? "true" : "false") << ",\"partnerStill\":" << (p_Now - m_PartnerMovedAt) << "}";

    if (m_World.IsInScene())
        s_Json << ",\"camera\":" << m_World.DescribeCamera();

    if (m_Relay)
        s_Json << ",\"relay\":{\"phase\":" << static_cast<int>(m_Relay->phase) << ",\"code\":\"" << m_Relay->code
               << "\",\"error\":\"" << TestChannel::Escape(m_Relay->error) << "\"}";

    s_Json << ",\"toasts\":[";
    bool s_First = true;

    for (const auto& s_Toast : m_World.CopyToasts()) {
        s_Json << (s_First ? "" : ",") << "\"" << TestChannel::Escape(s_Toast.text) << "\"";
        s_First = false;
    }

    s_Json << "]}";
    m_Test.WriteStatus(s_Json.str());
}

void HitmanCoop::WatchRelay(double p_Now) {
    if (!m_Relay || !m_Session)
        return;

    const auto& s_Status = m_FrameStatus;

    if (m_Relay->phase == coop::RelayState::Phase::Ready && s_Status.role == coop::SessionRole::Host && !m_RelayCodeShown) {
        m_RelayCodeShown = true;

        // On stream, the code isn't shown: it's in the CO-OP window (hidden too) and the clipboard.
        if (m_StreamerSetting)
            m_World.Notify("Your join code is ready (hidden: streamer mode). Copy it in the CO-OP window.");
        else
            m_World.Notify("Join code: " + m_Relay->code + ". Your friend types it in their CO-OP window.");
    }

    if (m_Relay->phase == coop::RelayState::Phase::Failed && !m_RelayFailShown) {
        m_RelayFailShown = true;

        if (s_Status.role == coop::SessionRole::Client) {
            // Without the relay a client that joined by code can't reach the host.
            m_World.Notify("Co-op server: " + m_Relay->error);
            m_Session->Leave("Co-op server unavailable", p_Now);
        }
        else {
            m_World.Notify("Co-op server: " + m_Relay->error + " Friends can still join by IP address.");
        }
    }
}

void HitmanCoop::PublishStatus() {
    coop::SessionStatus s_Status = m_FrameStatus;
    SdkWorld::Overlay s_Overlay = m_World.BuildOverlay();

    // Not over a game menu, nor in a cutscene (the camera far from our 47): names and marks would
    // float over the scene's own pictures.
    if (s_Overlay.inMission && (GamePause::MenuOpen() || coop::Distance(s_Overlay.camera, s_Overlay.local) > 10.f)) {
        s_Overlay.hidden = true;
        s_Overlay.partnerShown = false;
        s_Overlay.marks.clear();
    }

    const double s_Now = Now();

    if (!s_Status.synced)
        m_SyncedSince = -1.0;
    else if (m_SyncedSince < 0.0)
        m_SyncedSince = s_Now;

    // The partner standing still: they may have stepped away from their PC.
    if (!s_Status.synced || !s_Overlay.partnerShown || coop::Distance(s_Overlay.partnerFeet, m_PartnerLastPos) > 0.3f) {
        m_PartnerLastPos = s_Overlay.partnerFeet;
        m_PartnerMovedAt = s_Now;
    }

    // Our hotkey's "hide everything": the overlay draws nothing.
    if (m_HideOverlay)
        s_Overlay.hidden = true;

    std::lock_guard s_Lock(m_StatusMutex);
    m_Status = std::move(s_Status);
    m_Overlay = std::move(s_Overlay);
    m_OverlayReconnecting = m_Reconnecting;
    m_OverlayOurs = m_Tally.State();
    m_OverlayWaiting = m_Waiting;
    m_OverlayWaitLeft = c_WaitMax - (s_Now - m_WaitStart);
    m_OverlayTogether = m_SyncedSince < 0.0 ? -1.0 : s_Now - m_SyncedSince;
    m_OverlayCountdownEnd = m_CountdownEnd;
    m_OverlayReady = s_Now - m_PartnerAskedReadyAt < 60.0 ? 2 : s_Now - m_ReadyAskedAt < 30.0 ? 1 : 0;
    m_OverlayPartnerStill = s_Now - m_PartnerMovedAt;
    m_OverlayCanReconnect = m_LastJoin.has_value() && s_Status.state == coop::SessionState::Failed;
    m_InMission = m_World.IsInScene();
    m_RelayStatus.reset();

    if (m_Relay && m_Session && m_Session->Active())
        m_RelayStatus = *m_Relay;
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_Parameters) {
    std::lock_guard s_Lock(m_SessionMutex);

    coop::SceneDesc s_Scene;
    s_Scene.scene.assign(p_Parameters.m_SceneResource.c_str(), p_Parameters.m_SceneResource.size());
    s_Scene.codename.assign(p_Parameters.m_CodeNameHint.c_str(), p_Parameters.m_CodeNameHint.size());

    for (const auto& s_Brick : p_Parameters.m_aAdditionalBrickResources)
        s_Scene.bricks.emplace_back(s_Brick.c_str(), s_Brick.size());

    // A client starting any mission at the host's location is taken to the host's exact mission
    // there. Never to another location: the client only plays locations it started itself.
    coop::SceneDesc s_HostScene;

    if (m_Session && m_FollowHostSetting && SdkWorld::IsMissionScene(s_Scene.scene) &&
        m_Session->RedirectFor(s_Scene, s_HostScene)) {
        // The mission's own packages (e.g. a different mission's bricks at this location) may not be
        // mounted yet; mount them as the Outfits mod does.
        const auto s_SceneRid = ZRuntimeResourceID::FromString("[" + ToLowerCopy(s_HostScene.scene) + "].pc_entitytype");
        const auto& s_Chunks = SDK()->GetChunkIndicesForRuntimeResourceId(s_SceneRid);

        if (s_Chunks.size() == 0) {
            m_World.Notify("Can't find the host's mission (" + s_HostScene.codename +
                ") in your game files, so you stay in the mission you picked.");
        }
        else {
            for (const uint32_t s_Chunk : s_Chunks) {
                if (!SDK()->IsChunkMounted(s_Chunk))
                    SDK()->MountChunk(s_Chunk);
            }

            p_Parameters.m_SceneResource = ZString(s_HostScene.scene);
            p_Parameters.m_aAdditionalBrickResources.clear();

            for (const auto& s_Brick : s_HostScene.bricks)
                p_Parameters.m_aAdditionalBrickResources.push_back(ZString(s_Brick));

            p_Parameters.m_CodeNameHint = ZString(s_HostScene.codename);
            s_Scene = s_HostScene;

            m_World.Notify("Taking you to " + m_Session->Status().peerName + "'s mission.");
            Logger::Info("[HitmanCoop] Redirected scene load to {}", s_HostScene.scene);
        }
    }

    // The mission just cleared, loaded again: a restart that reloads everything (or a save of it
    // loaded). Restart Mission itself clears the scene and rebuilds it without loading (below).
    const bool s_Restart = !m_ClearedMission.empty() && m_ClearedMission == s_Scene.scene;
    m_ClearedMission.clear();

    if (s_Restart)
        OnMissionRestarted(s_Scene.scene);

    // The load may stall our game for a while: the partner waits instead of timing us out.
    if (m_Session) {
        m_Session->NotifyBusy(Now(), 120, coop::LoadStage_Loading, s_Scene.scene);
        m_BusyTold = true;
        m_BusyToldAt = Now();
    }

    m_Test.Trace("scene: loading " + s_Scene.scene + (s_Restart ? " (restart)" : ""));
    m_World.OnSceneLoading(s_Scene);
    return HookResult<bool>(HookAction::Continue());
}

void HitmanCoop::OnMissionRestarted(const std::string& p_Scene) {
    // The partner's mission restarts too, unless this restart is the one we made for them.
    if (m_World.TakeRestartForPartner()) {
        m_Test.Trace("restart: the one made for the partner");
        return;
    }

    if (m_Session) {
        m_Session->OnLocalRestart(p_Scene);
        m_Test.Trace("restart: ours, told to the partner");
    }
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene) {
    std::lock_guard s_Lock(m_SessionMutex);

    // A mission cleared but not unloaded is being rebuilt as it was: Restart Mission (3.280.0
    // clears it and never loads the scene again). Fully unloaded, it may be loaded again (see
    // OnLoadScene) or left for another scene.
    // A restart already told (by its ContractFailed event) is not looked for again.
    const bool s_InMission = m_World.IsInScene() && SdkWorld::IsMissionScene(m_World.CurrentScene().scene) && !m_RestartTold;
    m_RestartTold = false;
    m_ClearedMission = s_InMission && p_FullyUnloadScene ? m_World.CurrentScene().scene : std::string();
    m_Test.Trace(std::string("scene: clearing (") + (p_FullyUnloadScene ? "full" : "for a rebuild") + ")");

    if (s_InMission && !p_FullyUnloadScene)
        OnMissionRestarted(m_World.CurrentScene().scene);

    if (m_Session && m_World.IsInScene())
        m_Session->OnLocalSceneUnloaded();

    if (m_Session) {
        m_Session->NotifyBusy(Now(), 90, coop::LoadStage_Clearing, m_World.CurrentScene().scene);
        m_BusyTold = true;
        m_BusyToldAt = Now();
    }

    m_World.OnSceneCleared();
    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, bool, OnActorHit, IBaseCharacter* th, const SHitInfo& p_HitInfo) {
    // The partner's stand-in can't be hurt; your partner's own game decides what happens to them.
    if (m_World.IsPartnerActor(static_cast<ZActor*>(th))) {
        ++m_FriendlyHits;
        return HookResult<bool>(HookAction::Return(), false);
    }

    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, void, OnSetGameTension, ZKnowledge* th, EGameTension p_Tension) {
    // Runs on AI threads: only compares with the stand-in's knowledge, kept apart for this.
    if (CrashGuards::IsStandInKnowledge(th) && p_Tension != EGameTension::EGT_Ambient && p_Tension != EGameTension::EGT_Undefined) {
        CrashGuards::NoteCalmed(static_cast<int32_t>(p_Tension));

        if (m_Test.Enabled()) {
            void* s_Frames[24];
            CrashGuards::NoteCalmedFrom(s_Frames, RtlCaptureStackBackTrace(0, 24, s_Frames, nullptr));
        }

        p_Hook->CallOriginal(th, EGameTension::EGT_Ambient);
        return HookResult<void>(HookAction::Return());
    }

    // An NPC alert in the partner's game: ours doesn't calm down below that meanwhile.
    if (p_Tension != EGameTension::EGT_Undefined) {
        if (const int32_t s_Floor = CrashGuards::AlertFloor(th); s_Floor > static_cast<int32_t>(p_Tension)) {
            p_Hook->CallOriginal(th, static_cast<EGameTension>(s_Floor));
            return HookResult<void>(HookAction::Return());
        }
    }

    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(
    HitmanCoop, void, OnEventSent, ZAchievementManagerSimple* th, uint32_t p_EventIndex, const ZDynamicObject& p_Event
) {
    std::lock_guard s_Lock(m_SessionMutex);

    // Read in any session (our mission's stats count from the start, even before the partner is in).
    if (m_Session && m_Session->Active()) {
        const bool s_Connected = m_Session->Status().state == coop::SessionState::Connected;
        ZString s_Data;
        Functions::ZDynamicObject_ToString->Call(const_cast<ZDynamicObject*>(&p_Event), s_Data);

        const std::string s_Json(s_Data.c_str(), s_Data.size());
        const std::string s_Name = EventName(s_Json);

        // An NPC we killed only to match the partner's game: theirs scores it, ours must not.
        // Its kill, and witnesses or finds of its body: those were the partner's, in their game.
        static const std::unordered_set<std::string> c_PartnerKillEvents = {
            "Kill", "NoticedKill", "MurderedBodySeen", "DeadBodySeen", "BodyFound", "AccidentBodyFound", "Witnesses", "Pacify",
            "Noticed_Pacified"
        };

        if (c_PartnerKillEvents.contains(s_Name) && m_World.IsPartnerKillEvent(s_Json)) {
            // Later events (NoticedKill) name the NPC only by its repository id, given here.
            if (const size_t s_At = s_Json.find("\"RepositoryId\":\""); (s_Name == "Kill" || s_Name == "Pacify") && s_At != std::string::npos) {
                const size_t s_Start = s_At + 16;
                m_World.AddPartnerKillKey(s_Json.substr(s_Start, s_Json.find('"', s_Start) - s_Start));
            }

            m_Test.Trace("event: " + s_Name + " of a partner's victim, dropped");
            return HookResult<void>(HookAction::Return());
        }

        if (s_Name == "Pacify" || s_Name == "Kill")
            m_World.NotePacified(s_Json, s_Name == "Pacify");

        // Ours to count and tell: not a kill made only to mirror the partner's game, nor one of the
        // game's hidden bookkeeping challenges.
        const bool s_Mirror = (s_Name == "Kill" || s_Name == "Pacify") && m_World.IsPartnerKillEvent(s_Json, true);
        const bool s_HiddenChallenge = s_Name == "ChallengeCompleted" && s_Json.find("hideinhud") != std::string::npos;

        if (!s_Mirror && !s_HiddenChallenge && !(s_Name == "ContractFailed" && s_Json.find("OnRestartLevel") != std::string::npos))
            m_Tally.Add(s_Name, s_Json);

        if (s_Connected && m_Session->Synced() && !s_Mirror) {
            if (coop::GameEvents::Takedown s_Down; coop::GameEvents::ReadTakedown(s_Name, s_Json, s_Down))
                m_Session->SendMissionEvent("Coop_Kill", coop::GameEvents::EncodeTakedown(s_Down));

            if (std::string s_Key; s_Name == "ChallengeCompleted" && !s_HiddenChallenge && coop::GameEvents::String(s_Json, "ChallengeName", s_Key))
                m_Session->SendMissionEvent("Coop_Challenge", coop::GameEvents::ChallengeTitle(s_Key));
        }

        if (s_Name == "ContractEnd")
            ShowDebrief();

        // Restart Mission ends the contract this way first, before the scene is cleared: the
        // moment to restart the partner's too. Not passed on as a failed mission.
        if (s_Name == "ContractFailed" && s_Json.find("OnRestartLevel") != std::string::npos) {
            m_Test.Trace("event: ContractFailed (Restart Mission)");

            if (m_World.IsInScene() && SdkWorld::IsMissionScene(m_World.CurrentScene().scene)) {
                m_RestartTold = true;
                OnMissionRestarted(m_World.CurrentScene().scene);
            }

            return HookResult<void>(HookAction::Continue());
        }

        if (s_Name.find("Kill") != std::string::npos || s_Name.find("Pacif") != std::string::npos)
            m_Test.Trace("event: " + s_Name + (s_Name == "Pacify" ? " " + s_Json.substr(std::min<size_t>(s_Json.size(), 200)) : std::string()));
        else
            m_Test.Trace("event: " + s_Name + " " + s_Json.substr(0, std::min<size_t>(s_Json.size(), 1500)));

        if (s_Connected && ShouldForwardEvent(s_Name, s_Json))
            m_Session->SendMissionEvent(s_Name, s_Json);

        // What happens to our 47, in a few words: the partner hears it (each at most every 10 s).
        if (const std::string s_Aware = coop::GameEvents::Awareness(s_Name, s_Json); !s_Aware.empty() && s_Connected && m_Session->Synced()) {
            const double s_Now = Now();

            if (double& s_At = m_AwareSentAt[s_Aware]; s_Now - s_At > 10.0) {
                s_At = s_Now;
                m_Session->SendMissionEvent("Coop_Aware", s_Aware);
            }
        }
    }

    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, void, OnSetGameState, ZLevelManager* th, ZLevelManager::EGameState p_State) {
    // May run on a loading thread: only noted here, acted on in the next frame.
    m_LevelState = static_cast<int>(p_State);
    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, bool, OnTryOpenMenu, ZGameUIManagerEntity* th, EGameUIMenu p_Menu, bool p_Force) {
    m_LastMenu = static_cast<int>(p_Menu);
    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, void, OnWindowActivated, ZApplicationEngineWin32* th, bool p_Activated) {
    m_WindowActive = p_Activated;
    return HookResult<void>(HookAction::Continue());
}

DEFINE_ZHM_PLUGIN(HitmanCoop);

DEFINE_PLUGIN_DETOUR(HitmanCoop, bool, OnInputDigital, ZInputAction* th, int32_t p_ControllerId) {
    if (!th || !th->m_szName)
        return HookResult<bool>(HookAction::Continue());

    std::lock_guard s_Lock(m_InputMutex);
    const double s_Now = Now();

    if (s_Now < m_InputLogUntil)
        m_InputSeen.insert(th->m_szName);

    if (m_PressAction == th->m_szName) {
        if (m_PressPending) {
            m_PressPending = false;
            m_PressFrame = m_InputFrame;
        }

        if (m_PressFrame == m_InputFrame)
            return HookResult<bool>(HookAction::Return(), true);
    }

    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(HitmanCoop, float, OnInputAnalog, ZInputAction* th, int32_t p_ControllerId) {
    if (!th || !th->m_szName)
        return HookResult<float>(HookAction::Continue());

    std::lock_guard s_Lock(m_InputMutex);
    const double s_Now = Now();

    if (s_Now < m_InputLogUntil)
        m_AnalogSeen.insert(th->m_szName);

    const auto s_Hold = m_AnalogHolds.find(th->m_szName);

    if (s_Hold != m_AnalogHolds.end() && s_Now < s_Hold->second.second)
        return HookResult<float>(HookAction::Return(), s_Hold->second.first);

    return HookResult<float>(HookAction::Continue());
}
