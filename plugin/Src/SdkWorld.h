#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "coop/Session.h"

#include "PartnerPuppet.h"
#include "PoseHook.h"

#include "Glacier/EntityFactory.h"
#include "Glacier/Enums.h"
#include "Glacier/ZEntity.h"
#include "Glacier/ZMath.h"
#include "Glacier/ZResource.h"

class ZActor;
class ZGlobalOutfitKit;
class ZTemplateEntityFactory;
class IItem;
class IItemBase;

// IGameWorld backed by the running game through ZHMModSDK. Game thread only,
// except the toast list which the render thread reads under m_ToastMutex.
class SdkWorld final : public coop::IGameWorld {
public:
    // coop::IGameWorld
    bool IsInScene() const override { return m_InScene; }
    coop::SceneDesc CurrentScene() const override { return m_Scene; }
    bool GetLocalAvatar(coop::Transform& p_Out) const override;
    bool GetLocalOutfit(coop::OutfitDesc& p_Out) const override;
    void ShowRemoteAvatar(const coop::Transform& p_Xf) override;
    void HideRemoteAvatar() override;
    void SetRemoteOutfit(const coop::OutfitDesc& p_Outfit) override;
    bool GetLocalPose(coop::Pose& p_Out) override;
    void ShowRemotePose(const coop::Pose& p_Pose) override;
    bool GetLocalSkeleton(std::vector<int16_t>& p_Out) override;
    void SetRemoteSkeleton(const std::vector<int16_t>& p_Parents) override;
    bool GetLocalVictim(uint64_t& p_Id, coop::Transform& p_Xf, coop::Pose& p_Pose, uint8_t& p_Flags) override;
    void ShowRemoteVictim(uint64_t p_Id, const coop::Transform& p_Xf, const coop::Pose& p_Pose, uint8_t p_Flags) override;
    void ReleaseRemoteVictim() override;
    void ListNpcs(std::vector<coop::NpcState>& p_Out) const override;
    void SetNpcTransform(uint64_t p_Id, const coop::Transform& p_Xf) override;
    void SetNpcPuppet(uint64_t p_Id, bool p_Puppet) override;
    bool IsLocallyPaused() const override;
    void SetPartnerPaused(bool p_Paused) override;
    void RestartMission() override;
    // A restart we made for the partner: not told back to them (see HitmanCoop's scene load).
    bool TakeRestartForPartner() {
        const bool s_Was = m_RestartForPartner;
        m_RestartForPartner = false;
        return s_Was;
    }
    bool m_RestartForPartner = false;
    // The partner's restart, made from the frame update once our mission is playing and nothing of
    // ours holds it paused (see UpdateRestart).
    void UpdateRestart(bool p_Playing);
    double m_RestartAskedAt = -1.0;
    void ShowNpcPose(uint64_t p_Id, const coop::Pose& p_Pose) override;
    void ClearNpcPose(uint64_t p_Id) override;
    void WatchNpcPoses(const std::vector<uint64_t>& p_Ids) override;
    bool GetNpcPose(uint64_t p_Id, coop::Pose& p_Out) override;
    // Puppets asked for before the mission has run a few seconds (their AI must have set itself
    // up first, like the stand-in's), applied once it has.
    std::unordered_map<uint64_t, bool> m_PendingPuppets;
    // NPCs the session made puppets (run by the partner's game), so letting go of the partner's
    // victim leaves one of them a puppet.
    std::unordered_set<uint64_t> m_SessionPuppets;
    double m_SceneReadyAt = 0.0;
    void ApplyPendingPuppets();
    std::vector<PoseHook::Bone> m_NpcPoseBones;
    void KillNpc(uint64_t p_Id) override;
    void OnPeerMissionEvent(const std::string& p_Name, const std::string& p_Json) override;
    void CollectItemChanges(std::vector<coop::ItemChange>& p_Out) override;
    void ApplyPeerItem(const coop::ItemChange& p_Change) override;
    void ResetItems() override;
    void CollectAlerts(std::vector<coop::NpcAlert>& p_Out) override;
    void ApplyPeerAlerts(const std::vector<coop::NpcAlert>& p_Alerts) override;
    size_t m_PeerAlertCount = 0; // NPCs kept alert for the partner (test channel)
    void Notify(const std::string& p_Text) override;
    void KnockOutNpc(uint64_t p_Id) override;
    void OnPeerChat(const std::string& p_Text, bool p_Quick) override;
    void OnPeerMark(uint64_t p_Id, const coop::Vec3& p_Pos, uint8_t p_Kind) override;

    // Notifications come in kinds, each its own colour; the same line again within a few seconds
    // counts up instead of stacking.
    enum class ToastKind { Info, Partner, Chat, Warning };
    void NotifyAs(ToastKind p_Kind, const std::string& p_Text);

    // Marks: an NPC (followed as it moves) or a place, by our player or the partner, shown on
    // both screens for a while (Settings: how long, and how many each).
    void SetMarkRules(double p_Seconds, int p_Max) { m_MarkSeconds = p_Seconds; m_MaxMarks = p_Max; }
    double MarkSeconds() const { return m_MarkSeconds; }
    // What our player is aiming at (the screen centre): the NPC nearest the line of sight, or the
    // floor where that line meets it. False if neither.
    bool PickAimed(uint64_t& p_Id, coop::Vec3& p_Pos, std::string& p_Label);
    void AddMark(uint64_t p_Id, const coop::Vec3& p_Pos, bool p_Ours, const std::string& p_Label, bool p_Danger = false);
    // Our own mark on that NPC (or, for a place, within a few metres of it), if one is up: how
    // long ago it was made and whether it warns of danger.
    bool OurMarkOn(uint64_t p_Id, const coop::Vec3& p_Pos, double& p_Age, bool& p_Danger) const;
    // That one mark of ours or the partner's taken away.
    void RemoveMark(bool p_Ours, uint64_t p_Id, const coop::Vec3& p_Pos);
    // Once a frame: a marked NPC that went down takes its mark with it (and both are told).
    void UpdateMarks();
    void SetViewSize(float p_Width, float p_Height) { m_ViewWidth = p_Width; m_ViewHeight = p_Height; }
    // The partner's disguise as the game names it ("Kitchen Assistant"), or "their suit".
    std::string PartnerDisguise() const;
    // Our partner's pause holds ours this long at most (an AFK partner can't freeze our game).
    static constexpr double c_PauseHoldMax = 120.0;
    void UpdatePauseCap();
    // Soft system sounds for the partner's chat, marks and alerts.
    void SetSounds(bool p_On) { m_Sounds = p_On; }
    // The partner asked where we are (or we asked): their name tag and arrow pulse a few seconds.
    void Pulse() { m_PulseUntil = m_Now + 5.0; }
    // Distances said in feet instead of metres.
    void SetFeet(bool p_On) { m_Feet = p_On; }
    std::string Distance(float p_Metres) const;
    // Which partner notifications show (the chat log and history keep everything).
    void SetToastFilter(bool p_KillFeed, bool p_Challenges, bool p_Alerts, bool p_Chat) {
        m_ToastKillFeed = p_KillFeed;
        m_ToastChallenges = p_Challenges;
        m_ToastAlerts = p_Alerts;
        m_ToastChat = p_Chat;
    }
    // The partner's pause holds ours unless it is only their map or notebook and we said so.
    void SetPartnerMenuIgnored(bool p_Ignored) { m_PartnerMenuIgnored = p_Ignored; }
    // Mission events of ours first (ready checks, countdowns ...): true when it handled one.
    std::function<bool(const std::string&, const std::string&)> m_EventHook;
    // The partner's signal (Signal_*) over their head, and since when.
    int m_PartnerSignal = -1;
    double m_PartnerSignalAt = -100.0;
    // Living mission targets, for the radar's target assist.
    bool m_ListTargets = false;

    // What the overlay draws, gathered on the game thread each frame for the render thread.
    struct Overlay {
        bool inMission = false;
        bool hidden = false;        // a game menu or a cutscene: nothing drawn over it
        bool partnerShown = false;
        coop::Vec3 partnerHead;
        coop::Vec3 partnerFeet;
        float partnerDistance = 0.f;
        float partnerHeight = 0.f;  // metres above our 47 (below: negative)
        std::string partnerDisguise;
        bool pulse = false;         // "where are you?": the partner's tag stands out
        int signal = -1;            // the partner's signal over their head (Signal_*), -1: none
        float signalAge = 0.f;
        float markSeconds = 20.f;
        std::vector<coop::Vec3> targets; // target assist (radar), if on
        coop::Vec3 camera;
        coop::Vec3 cameraForward;
        coop::Vec3 cameraRight;     // the screen's right, in the world (the radar's sideways)
        coop::Vec3 local;
        struct Marker {
            coop::Vec3 pos;
            coop::Vec3 ground;      // where it is on the ground (the radar)
            std::string label;
            bool ours = false;
            bool danger = false;
            bool target = false;
            float age = 0.f;
            float distance = 0.f;
        };
        std::vector<Marker> marks;
    };
    Overlay BuildOverlay() const;

    // Our 47 just behind the partner's stand-in, facing their way. False (and why) if it can't.
    bool RegroupToPartner(std::string& p_Why);
    // Off: the partner's pause menu doesn't pause our game (they are told it is open).
    void SetPauseTogether(bool p_On);
    // Whether our game should be held for the partner now (their pause, and our settings).
    bool WantPartnerHold() const { return m_PartnerWantsPause && m_PauseTogether && !m_PauseCapped && !m_PartnerMenuIgnored; }
    bool PartnerHoldsPause() const { return m_PartnerWantsPause; }

    struct ChatLine {
        std::string text;
        bool ours = false;
        bool quick = false;
        double time = 0.0;
    };
    void AddChat(const std::string& p_Text, bool p_Ours, bool p_Quick);
    std::deque<ChatLine> CopyChat() const;
    void ClearChat();
    // The partner's lines so far (all of them, not just those kept): what is new since the Chat tab was seen.
    size_t PartnerChatCount() const;
    size_t m_PartnerChatCount = 0; // under m_ToastMutex

    // Called by the plugin.
    void OnSceneLoading(const coop::SceneDesc& p_Scene);
    void OnSceneCleared();

    // Polls whether the local 47 now exists. Returns true on the frame the mission becomes playable.
    bool UpdateSceneState();

    bool IsPartnerActor(const void* p_Character) const;
    void SetPartnerName(const std::string& p_Name) { m_PartnerName = p_Name; }

    // For the test channel.
    bool HasPartner() const { return static_cast<bool>(m_Partner) && !m_PartnerHidden; }
    bool PartnerSpawnFailed() const { return m_PartnerSpawnFailed; }
    // For diagnostics: the stand-in's state, and why it isn't shown if it isn't.
    std::string DescribeStandIn() const;
    size_t LastNpcCount() const { return m_NpcsById.size(); }
    bool GetPartnerPosition(coop::Vec3& p_Out) const;
    coop::Vec3 PartnerWanted() const { return m_PartnerWanted; }
    std::string DescribeNotAliveNpcs() const;
    std::string DescribePartner() const;
    std::string DumpRemoteBones(size_t p_Count) const; // test: the partner's pose as last given
    // Test channel: the stand-in's actor disabled or enabled the way level scripts do it
    // (ZActorPickerEffectState), or its geometry shown.
    std::string StandInTest(const std::string& p_What);
    // Test channel: the next stand-in spawns without its behaviour tree (1), its HTN domain (2) or both (3).
    void SetBrainless(int p_Mask) { m_Brainless = p_Mask; }
    int m_Brainless = 0;
    // Test channel: animation data of 47, the stand-in and one NPC.
    std::string DumpBones(size_t p_Bytes) const;
    std::string DumpAnimators();
    // Test channel: KillActor on the nearest NPC with a given damage event and death behaviour,
    // then what state it ended in.
    std::string TakedownTest(int p_DamageEvent, int p_DeathBehavior);
    std::string DescribeNearestNpc() const;
    std::string DescribeVictim() const;
    std::string KnockOutTest(uint32_t p_DeathType, uint32_t p_DeathContext);
    // Test channel: the stand-in copies the local 47's skeleton pose every frame.
    void SetMirror(bool p_On) { m_Mirror = p_On; m_PoseIdentity = false; }
    // Test: the partner's victim (the bot echoing our own takedown) is played by the nearest other
    // NPC whose skeleton fits, since our own victim is busy being ours.
    void SetVictimTest(bool p_On) { m_VictimTest = p_On; }
    // Test: 0 applies both the partner's victim transform and pose, 1 the pose only, 2 the move only.
    void SetVictimTestApply(int p_Mode) { m_VictimTestApply = p_Mode; }
    // Test: stops sending our victim, as walking away from the body would.
    void SetVictimSend(bool p_On) { m_VictimSendOff = !p_On; }
    void AddPartnerGuards() { GuardPartnerAi(true); }
    void SetGuardOnSpawn(bool p_On) { m_GuardOnSpawn = p_On; }
    // Test: the stand-in wears 47's own suit when the partner does (crashed before the AI guards).
    void SetHeroSuitStandIn(bool p_On) { m_HeroSuitStandIn = p_On; }
    bool m_HeroSuitStandIn = false;
    // Test: the nearest item as if the partner took it (true), or put it back (false).
    std::string ItemTest(bool p_Take);
    std::string DoorTest();
    std::string RigScanTest();
    std::string DoorToggleTest();
    std::string DoorState();
    std::string GoToDoorTest();

    // Doors (sent along with items, see coop::ItemFlag_Door), by their scene key.
    struct Door {
        ZEntityRef ref;
        bool open = false;
        bool known = false;
    };
    std::unordered_map<uint64_t, Door> m_Doors;
    bool m_DoorsFound = false;
    double m_DoorsSearchAt = 0.0;
    void FindDoors();
    void CollectDoorChanges(std::vector<coop::ItemChange>& p_Out);
    void ApplyPeerDoor(const coop::ItemChange& p_Change);
    void ForEachSceneEntity(const std::function<void(uint64_t, const ZEntityRef&)>& p_Do) const;

    // Bodies (NPCs down), where each was last agreed to lie: moved in our game since (dragged,
    // dumped, put away), the partner is told once it rests. Moved or put away by the partner, ours
    // is hidden: a ragdoll can't be put anywhere (its physics takes it back to where it lay, with
    // its matrix set once, every frame, or with the ragdoll off and on again: Paris, 3.280.0), and
    // left there it was found where the partner had taken it from.
    struct Body {
        coop::Vec3 agreed;
        coop::Vec3 last;
        double stillSince = 0.0;
        double downAt = 0.0;
        bool hidden = false;
        bool gone = false;
    };
    std::unordered_map<uint64_t, Body> m_Bodies;
    void CollectBodyChanges(std::vector<coop::ItemChange>& p_Out);
    void ApplyPeerBody(const coop::ItemChange& p_Change);
    void AgreeBody(uint64_t p_Id, ZActor* p_Actor);
    std::string BodyTest(const std::string& p_Arg);
    // What the partner's 47 has in hand, in the stand-in's: a copy of it made here (by repository id)
    // and given to the stand-in the way the game gives NPCs their items, in its right hand.
    void SetPartnerHeldItem(const std::string& p_RepositoryId);
    // What our own 47 has in hand: its repository id, empty if nothing.
    std::string LocalHeldItem() const;
    std::string HeldItemsTest() const;
    std::string StandInItemTest(const std::string& p_Arg);
    void UpdateStandInItem();
    void DropStandInItem();
    void OnStandInItemCreated(uint32_t p_Ticket, TEntityRef<IItemBase> p_Item);
    std::string m_StandInItemWanted;
    std::string m_StandInItemShown; // what it holds, or was asked for (ticket pending)
    TEntityRef<IItem> m_StandInItem;
    uint32_t m_StandInItemTicket = 0;
    double m_StandInItemAskedAt = -1.0;
    double m_StandInItemFailedAt = -100.0;
    int m_StandInAttach = 0; // EAttachLocation (test: other places)
    bool m_StandInWeapon = false;
    mutable double m_HeldCheckAt = 0.0;
    mutable std::string m_HeldItem;
    uint64_t m_ItemTestId = 0;
    coop::Transform m_ItemTestXf;
    // True for an event about an NPC we killed only to mirror the partner's game.
    // p_EvenTargets: also a target's (which still count here, but aren't sent back to the partner).
    bool IsPartnerKillEvent(const std::string& p_EventJson, bool p_EvenTargets = false) const;
    void AddPartnerKillKey(const std::string& p_Key) { m_PartnerKills.insert(p_Key); }
    // A Pacify (true) or Kill (false) event from our game: that NPC is knocked out, or no longer
    // just knocked out. IsPacified() doesn't say so.
    void NotePacified(const std::string& p_EventJson, bool p_Pacified);
    // Test channel: every bone of the stand-in gets an identity rotation, to see the override.
    void SetPoseIdentity(bool p_On) { m_PoseIdentity = p_On; m_Mirror = p_On; }
    // Once a frame on the game thread: points the pose hook at the local 47 and the stand-in.
    void UpdatePoses();
    std::string DescribeVictims() const;
    // Test: puts our 47 that far behind the nearest standing NPC, facing the same way (ready to subdue).
    std::string StepBehindNearestNpc(float p_Distance, bool p_Loneliest = false);
    // 2 s of grace: a network gap must not hand the stand-in to its own AI (it walked off, then snapped back).
    bool PartnerPosed() const { return m_Now - m_RemotePoseTime < 2.0; }
    std::string DescribeActorAi() const;
    // Test channel trace: notifications and stand-in lifecycle, so a crash can be placed in time.
    void SetTrace(std::function<void(const std::string&)> p_Trace) { m_Trace = std::move(p_Trace); }
    // Test channel: "partner off" never spawns the stand-in, to tell its effects apart from the rest.
    void SetPartnerEnabled(bool p_Enabled) { m_PartnerEnabled = p_Enabled; }
    // Test channel: our 47's outfit can't be named (as on some players' PCs).
    void SetOutfitUnknownTest(bool p_On) { m_OutfitUnknownTest = p_On; }
    // Test channel: walk the stand-in with the game's AI instead of teleporting it.
    void SetPuppetMode(bool p_On) { m_PuppetMode = p_On; }
    PartnerPuppet& Puppet() { return m_Puppet; }
    // Test channel: log every frame of the stand-in's tracking to a CSV for a while.
    void StartTrackLog(const std::string& p_Path, double p_Seconds) { m_TrackPath = p_Path; m_TrackUntil = m_Now + p_Seconds; m_TrackStart = m_Now; }
    // The active camera's position and axes as JSON, for aiming test screenshots.
    std::string DescribeCamera() const;

    struct Toast {
        std::string text;
        double time;
        ToastKind kind = ToastKind::Info;
        int count = 1;
    };

    std::deque<Toast> CopyToasts() const;
    // Every notification of the session (the last 100), for the CO-OP window.
    std::deque<Toast> CopyHistory() const;
    void SetClock(double p_Now) { m_Now = p_Now; }

    static bool IsMissionScene(const std::string& p_Scene);
    static coop::Transform ToTransform(const SMatrix& p_Matrix);
    static SMatrix ToMatrix(const coop::Transform& p_Xf);

private:
    enum class Spawn { Done, Waiting, Failed };

    Spawn SpawnPartner(const coop::Transform& p_Xf);
    static const TEntityRef<ZGlobalOutfitKit>* FindOutfitKit(const std::string& p_RepositoryId);
    coop::OutfitDesc StandInOutfit(const coop::OutfitDesc& p_Wanted);
    ZTemplateEntityFactory* PartnerTemplate(bool& p_Failed);
    void ApplyPartnerOutfit();
    ZActor* FindNpc(uint64_t p_Id) const;
    ZActor* NearestNpc(float* p_Distance) const;
    std::string OutfitKitToRepositoryId(ZGlobalOutfitKit* p_Kit) const;

    bool m_InScene = false;
    bool m_SceneLoading = false;
    coop::SceneDesc m_Scene;

    ZEntityRef m_Partner;
    ZActor* m_PartnerActor = nullptr;
    bool m_PartnerSpawnFailed = false;
    bool m_PartnerHidden = false;
    bool m_PartnerEnabled = true;
    bool m_OutfitUnknownTest = false;
    // Without poses from the partner (an older game build the hook can't attach to), the
    // stand-in walks by the game's AI instead.
    bool m_PuppetMode = true;
    bool m_PuppetFailed = false;
    double m_PuppetRequestStart = -1.0;
    TResourcePtr<ZTemplateEntityFactory> m_VrAnimatorFactory;
    ZEntityRef m_VrAnimator;
    ZActor* m_TakedownVictim = nullptr;
    bool m_Mirror = false;
    bool m_VictimTest = false;
    int m_VictimTestApply = 0;
    bool m_VictimSendOff = false;
    bool m_PoseIdentity = false;
    bool m_PoseHookFailed = false;
    std::vector<PoseHook::Bone> m_PoseScratch;
    std::vector<PoseHook::Bone> m_LocalBones;
    std::vector<PoseHook::Bone> m_RemoteBones;
    // The NPC our 47 is choking, killing or dragging, streamed to the partner.
    void PickLocalVictim();
    ZActor* m_LocalVictim = nullptr;
    uint64_t m_LocalVictimId = 0;
    double m_LocalVictimDownAt = -1.0;
    std::vector<PoseHook::Bone> m_VictimBones;
    // The partner's victim, posed and placed as on their screen.
    ZActor* m_RemoteVictim = nullptr;
    uint64_t m_RemoteVictimId = 0;
    uint8_t m_RemoteVictimFlags = 0;
    std::vector<PoseHook::Bone> m_RemoteVictimBones;
    uint64_t m_VictimPosesShown = 0;
    int m_RemoteVictimRig = 0; // 0: not checked yet, 1: its skeleton fits the pose, -1: it doesn't
    int m_RemoteVictimRigDiffer = 0;
    std::unordered_set<ZActor*> m_RigRejected; // test only
    coop::Transform m_PartnerSentXf; // where the partner said they are, before the stand-in follows
    bool m_HasPartnerSentXf = false;

    // Scene items (see coop::ItemChange). Ours: what our 47 holds, and ones it let go of, followed
    // while they move. The partner's: hidden here while they hold them.
    struct TrackedItem {
        bool ours = false;
        double followUntil = 0.0;
        coop::Transform sent;
    };
    std::unordered_map<uint64_t, TrackedItem> m_Items;
    std::unordered_set<uint64_t> m_PeerHeldItems;
    double m_NextItemScan = 0.0;
    class ZHM5Item* FindItem(uint64_t p_Id, ZEntityRef& p_Ref) const;

    // The stand-in's skeleton against the partner's pose, after each outfit change.
    void CheckPartnerSkeleton();

    // Makes the stand-in deaf, blind and unseen to the AI (see SdkWorld.cpp).
    void GuardPartnerAi(bool p_Unseen);
    bool m_GuardOnSpawn = true; // test: off with "noguards", added later with "guards"
    ZEntityRef m_PartnerAi;
    ZEntityRef m_PartnerUnseen;
    // Their templates, loaded once and kept for good (see SpawnQn, OnSceneCleared).
    struct QnFactories {
        TResourcePtr<ZTemplateEntityBlueprintFactory> blueprint;
        TResourcePtr<ZTemplateEntityFactory> factory;
        bool loaded = false;
    };
    QnFactories m_AiFactories;
    QnFactories m_UnseenFactories;
    static ZEntityRef SpawnQn(const char* p_Json, const char* p_Name, ZEntityRef p_Parent, QnFactories& p_Keep);
    int m_PartnerRig = 0; // 0: skeletons not compared yet, 1: compared (m_BoneMap)
    std::vector<int32_t> m_RigParents; // the stand-in's skeleton
    std::vector<int32_t> m_BoneMap;    // its bone -> the partner's 47 bone (empty: the same)
    double m_PartnerRigCheckAt = 0.0;
    // Hidden until its skeleton is matched and posed: a new body (joining, or a disguise change,
    // which rebuilds it) first shows in its raw arms-out pose, and the old match no longer fits.
    bool m_RigHidden = false;
    // Since when it waits for a match. Never more than a few seconds: a player saw no partner at
    // all, so after that it is shown as it is.
    double m_RigHiddenSince = -1.0;
    double m_PartnerRigReadyAt = 0.0;
    const void* m_RigBody = nullptr;     // the animated actor and animator the match was made for
    const void* m_RigAnimator = nullptr;
    const void* m_RigSkeleton = nullptr;  // its animator's rig (+0x50), which a new outfit replaces
    uint32_t m_RigBones = 0;              // and its bone count when matched
    int m_SkeletonRematches = 0;          // since spawn or the last outfit: more than a few, stop watching
    // The partner's 47's skeleton (its poses are in that layout), as they sent it. Matched against
    // ours only when they haven't: our 47's isn't theirs when they wear another outfit (a
    // disguise's has 231 bones, the suit's 233), and that twisted the stand-in's arms up.
    std::vector<int32_t> m_RemoteParents;
    double m_LayoutMismatchSince = -1.0;
    // Ours, sent to them; found again when our rig changes.
    std::vector<int16_t> m_LocalParents;
    const void* m_LocalParentsRig = nullptr;
    uint32_t m_LocalParentsBones = 0;
    void ResetPartnerRig(double p_CheckIn);
    std::unordered_set<std::string> m_PartnerKills; // actor names and repository ids, for this mission
    void KillForPartner(ZActor* p_Actor, EDamageEvent p_Event, EDeathBehavior p_Behavior);
    bool PacifyForPartner(ZActor* p_Actor);
    // Knocks out (alive) or kills the partner's victim here, as it is on their screen.
    void PutDown(ZActor* p_Actor, uint64_t p_Id, uint8_t p_Flags);
    bool m_RemoteVictimDowned = false; // put down here already
    bool m_RemoteVictimDragged = false; // and moved since (a drag), so posed again
    // Its AI held still while the partner has it: standing beside our 47's takedown, it saw that
    // and raised the alarm, though the partner was taking it down at the same moment.
    bool m_RemoteVictimStilled = false;
    coop::Vec3 m_RemoteVictimDownPos;
    // Knocked out by the partner. There is no way to knock an NPC out on command, so here it is
    // killed without a ragdoll and held in the partner's last pose, but still reported as alive.
    std::unordered_set<uint64_t> m_HeldDown;
    double m_RemotePoseTime = -100.0;
    double m_PartnerShownTime = 0.0;
    std::string m_TrackPath;
    double m_TrackUntil = 0.0;
    double m_TrackStart = 0.0;
    coop::Vec3 m_PartnerWanted {};
    double m_PartnerWantedTime = 0.0;
    float m_PartnerSpeed = 0.f;
    coop::Vec3 m_PartnerVelocity {};
    PartnerPuppet m_Puppet;
    // Held for the whole game, so the template stays loaded and is only requested once.
    // (ZResourcePtr has no safe copy-assignment, so it is never reassigned.)
    TResourcePtr<ZTemplateEntityFactory> m_PartnerTemplate;
    bool m_PartnerTemplateRequested = false;
    double m_PartnerTemplateRequestTime = 0.0;
    bool m_HasPartnerOutfit = false;
    double m_PartnerOutfitWaitStart = -1.0;
    // What the stand-in waits for before it can be spawned (logged and told if it takes long), or
    // why it couldn't be; and the outfit it wears.
    Spawn WaitFor(const char* p_What);
    const char* m_SpawnWait = nullptr;
    double m_SpawnWaitSince = -1.0;
    int m_SpawnWaitTold = 0; // 1: logged, 2: told too
    std::string m_SpawnFailure;
    std::string m_StandInWears;
    bool m_StandInShownLogged = false;
    bool m_ToldCivilianStandIn = false;
    bool m_HasCivilianOutfit = false;
    coop::OutfitDesc m_CivilianOutfit;
    coop::OutfitDesc m_PartnerOutfit;
    std::string m_PartnerName = "Partner";

    mutable std::unordered_map<uint64_t, ZActor*> m_NpcsById;
    mutable std::unordered_map<uint64_t, int> m_IdCountScratch; // kept to avoid reallocating every scan
    mutable std::unordered_map<ZGlobalOutfitKit*, std::string> m_OutfitIdCache;
    mutable std::unordered_set<ZGlobalOutfitKit*> m_OutfitIdTold; // traced once: named another way, or not at all

    // "Viktor Novikov (guard)", or just "a guard" for the crowd's "John Doe".
    static std::string NpcLabel(const ZActor* p_Actor);
    struct MarkState {
        uint64_t id = 0;
        coop::Vec3 pos;
        double time = 0.0;
        bool ours = false;
        std::string label;
        bool danger = false;
        bool target = false;
        bool down = false; // marked already down (a body): not cleared for being down
    };
    bool m_Sounds = true;
    bool m_Feet = false;
    bool m_ToastKillFeed = true, m_ToastChallenges = true, m_ToastAlerts = true, m_ToastChat = true;
    bool m_PartnerMenuIgnored = false;
    double m_MarkSeconds = 20.0;
    int m_MaxMarks = 1;
    double m_PulseUntil = 0.0;
    double m_PartnerPauseSince = -1.0;
    bool m_PauseCapped = false;
    std::deque<Toast> m_History; // under m_ToastMutex
    std::vector<MarkState> m_Marks;
    float m_ViewWidth = 0.f;
    float m_ViewHeight = 0.f;
    bool m_PauseTogether = true;
    bool m_PartnerWantsPause = false;
    std::deque<ChatLine> m_Chat; // under m_ToastMutex

    double m_Now = 0.0;
    std::function<void(const std::string&)> m_Trace;
    mutable std::mutex m_ToastMutex;
    std::deque<Toast> m_Toasts;
};
