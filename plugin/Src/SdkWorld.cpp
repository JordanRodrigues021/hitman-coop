#include "SdkWorld.h"

#include "CrashGuards.h"
#include "GamePause.h"
#include "RigScan.h"
#include "Hooks.h"
#include "BoneProbe.h"

#include "coop/GameEvents.h"
#include "coop/Math.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <sstream>

#include "Functions.h"
#include "Globals.h"
#include "IModSDK.h"
#include "Logging.h"

#include <Glacier/CompileReflection.h>
#include <Glacier/EntityFactory.h>
#include <Glacier/SExternalReferences.h>
#include <Glacier/ZActor.h>
#include <Glacier/ZAction.h>
#include <Glacier/ZItem.h>
#include <Glacier/ZWorldInventory.h>
#include <Glacier/ZPhysics.h>
#include <Glacier/ZCameraEntity.h>
#include <Glacier/ZCollision.h>
#include <Glacier/ZApplicationEngineWin32.h>
#include <Glacier/ZContentKitManager.h>
#include <Glacier/ZDelegate.h>
#include <Glacier/EDynamicEntityType.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZOutfit.h>
#include <Glacier/ZResource.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZSpatialEntity.h>

#include <DirectXMath.h>

namespace {
// Knocked out NPCs, by actor name, from the game's Pacify events: IsPacified() stays false for a
// knocked out NPC in 3.280.0 (and no actor state bit changes either).
std::unordered_set<std::string> g_Pacified;
}

static constexpr size_t c_MaxToasts = 6;
static constexpr size_t c_MaxHistory = 100;


coop::Transform SdkWorld::ToTransform(const SMatrix& p_Matrix) {
    coop::Transform s_Xf;
    s_Xf.pos = { p_Matrix.Trans.x, p_Matrix.Trans.y, p_Matrix.Trans.z };

    // Strip any scale before extracting the rotation.
    DirectX::XMVECTOR s_Scale, s_Rot, s_Pos;

    if (DirectX::XMMatrixDecompose(&s_Scale, &s_Rot, &s_Pos, p_Matrix.DX())) {
        DirectX::XMFLOAT4 s_Q;
        DirectX::XMStoreFloat4(&s_Q, s_Rot);
        s_Xf.rot = coop::Normalize({ s_Q.x, s_Q.y, s_Q.z, s_Q.w });
    }

    return s_Xf;
}

SMatrix SdkWorld::ToMatrix(const coop::Transform& p_Xf) {
    const DirectX::XMVECTOR s_Q = DirectX::XMVectorSet(p_Xf.rot.x, p_Xf.rot.y, p_Xf.rot.z, p_Xf.rot.w);
    SMatrix s_Matrix(DirectX::XMMatrixRotationQuaternion(s_Q));
    s_Matrix.Trans = float4(p_Xf.pos.x, p_Xf.pos.y, p_Xf.pos.z, 1.f);
    return s_Matrix;
}

bool SdkWorld::IsMissionScene(const std::string& p_Scene) {
    std::string s_Lower = p_Scene;
    std::transform(s_Lower.begin(), s_Lower.end(), s_Lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return !s_Lower.empty() && s_Lower.find("/scenes/frontend/") == std::string::npos &&
        s_Lower.find("boot.entity") == std::string::npos;
}

void SdkWorld::OnSceneLoading(const coop::SceneDesc& p_Scene) {
    m_Scene = p_Scene;
    m_SceneLoading = IsMissionScene(p_Scene.scene);
    m_InScene = false;
}

void SdkWorld::OnSceneCleared() {
    // The scene owns every entity in it, including our stand-in. Deleting it here instead left the
    // game holding a freed actor (crash at +0x60a58c on Restart Mission, 3.280.0); see CrashGuards.
    // The "unseen" modifier is switched off (that ends its frame update, which otherwise ran on
    // against the torn-down actor: crash at +0x322f55 after a restart, 3.280.0) and let go of its
    // actor: Restart Mission rebuilds the mission with our entities still in it and switches them
    // on again, and it then read its long-freed actor (crash at +0x322e91 on the third restart).
    // With no actor both its start and its update do nothing.
    if (m_PartnerUnseen && m_PartnerUnseen.GetEntity()) {
        m_PartnerUnseen.GetEntity()->Deactivate(0);
        m_PartnerUnseen.SetProperty("m_rActor", TEntityRef<ZActor>());
    }

    if (m_Trace)
        m_Trace("scene: cleared");

    m_Partner = {};
    m_PartnerAi = {};
    m_PartnerUnseen = {};
    // m_AiFactories and m_UnseenFactories stay loaded: Restart Mission rebuilds the mission with our entities still
    // in it, and releasing their factories here freed them under it (crash at +0x322e91 when the
    // rebuild switched the freed "unseen" modifier on again, 3.280.0).
    m_PartnerActor = nullptr;
    m_RigBody = m_RigAnimator = m_RigSkeleton = nullptr;
    GamePause::Forget();
    CrashGuards::ClearPuppets();
    m_PendingPuppets.clear();
    m_SessionPuppets.clear();
    m_RemoteVictimStilled = false;
    PoseHook::SetWatched({});
    m_RigBones = 0;
    m_RigHidden = false;
    CrashGuards::SetKnowledge(nullptr);
    m_Puppet.Forget();
    m_TakedownVictim = nullptr;
    PoseHook::SetSource(nullptr);
    PoseHook::SetTarget(nullptr);
    PoseHook::ClearTargetPose();
    PoseHook::SetSource(nullptr, 1);
    PoseHook::ClearExtraPoses();
    m_LocalVictim = nullptr;
    m_LocalVictimId = 0;
    m_RemoteVictim = nullptr;
    m_RemoteVictimId = 0;
    m_HeldDown.clear();
    m_RigRejected.clear();
    m_PartnerKills.clear();
    g_Pacified.clear();
    m_Marks.clear();
    m_PartnerWantsPause = false;
    m_RestartAskedAt = -1.0;
    m_HasPartnerSentXf = false;
    m_Items.clear();
    m_PeerHeldItems.clear();
    m_Bodies.clear();
    // The scene owns the copy in the stand-in's hand and frees it with the stand-in.
    m_StandInItem = {};
    m_StandInItemShown.clear();
    m_StandInItemTicket = 0;
    m_StandInItemAskedAt = -1.0;
    m_HeldItem.clear();
    m_Doors.clear();
    m_DoorsFound = false;
    m_DoorsSearchAt = 0.0;
    m_PuppetFailed = false;
    m_PuppetRequestStart = -1.0;
    m_PartnerSpawnFailed = false;
    m_PartnerHidden = false;
    m_PartnerOutfitWaitStart = -1.0;
    m_HasCivilianOutfit = false;
    m_InScene = false;
    m_NpcsById.clear();
    // m_Scene and m_SceneLoading stay: the next scene's LoadScene may already have run.
}

bool SdkWorld::UpdateSceneState() {
    if (!m_SceneLoading || m_InScene)
        return false;

    if (!Globals::Hitman5Module->m_pEntitySceneContext->m_pScene || !*Globals::ApplicationEngineWin32 ||
        !(*Globals::ApplicationEngineWin32)->m_bSceneLoaded)
        return false;

    auto s_Player = SDK()->GetLocalPlayer();

    if (!s_Player || !s_Player.m_entityRef.QueryInterface<ZSpatialEntity>())
        return false;

    m_InScene = true;
    m_SceneReadyAt = m_Now;
    return true;
}

bool SdkWorld::GetLocalAvatar(coop::Transform& p_Out) const {
    if (!m_InScene)
        return false;

    auto s_Player = SDK()->GetLocalPlayer();

    if (!s_Player)
        return false;

    const auto* s_Spatial = s_Player.m_entityRef.QueryInterface<ZSpatialEntity>();

    if (!s_Spatial)
        return false;

    p_Out = ToTransform(s_Spatial->GetObjectToWorldMatrix());
    return true;
}

std::string SdkWorld::OutfitKitToRepositoryId(ZGlobalOutfitKit* p_Kit) const {
    if (const auto s_It = m_OutfitIdCache.find(p_Kit); s_It != m_OutfitIdCache.end())
        return s_It->second;

    std::string s_Id;

    for (const auto& s_Pair : Globals::ContentKitManager->m_repositoryGlobalOutfitKits) {
        if (s_Pair.second.m_pInterfaceRef == p_Kit) {
            const ZString s_Str = s_Pair.first.ToString();
            s_Id.assign(s_Str.c_str(), s_Str.size());
            break;
        }
    }

    m_OutfitIdCache[p_Kit] = s_Id;
    return s_Id;
}

bool SdkWorld::GetLocalOutfit(coop::OutfitDesc& p_Out) const {
    if (!m_InScene)
        return false;

    auto s_Player = SDK()->GetLocalPlayer();

    if (!s_Player || !s_Player.m_pInterfaceRef)
        return false;

    ZHitman5* s_Hitman = s_Player.m_pInterfaceRef;
    ZGlobalOutfitKit* s_Kit = s_Hitman->m_rOutfitKit.m_pInterfaceRef;

    if (!s_Kit)
        return false;

    p_Out.repositoryId = OutfitKitToRepositoryId(s_Kit);
    p_Out.charset = s_Hitman->m_nOutfitCharset;
    p_Out.variation = s_Hitman->m_nOutfitVariation;
    return !p_Out.repositoryId.empty();
}

ZTemplateEntityFactory* SdkWorld::PartnerTemplate(bool& p_Failed) {
    // The same generic NPC template the SDK's Assets mod uses to spawn actors. Missions with NPCs
    // already have it loaded; scenes without any (like the first-time ICA Facility intro) don't,
    // so it is requested the way the SDK's Player and Outfits mods load theirs.
    constexpr auto s_TemplateId = ResId<"[assembly:/templates/gameplay/ai2/actors.template?/npcactor.entitytemplate].pc_entitytype">;

    if (!m_PartnerTemplateRequested) {
        m_PartnerTemplateRequested = true;
        m_PartnerTemplateRequestTime = m_Now;
        Globals::ResourceManager->LoadResource(m_PartnerTemplate, s_TemplateId);
    }

    if (m_PartnerTemplate.m_nResourceIndex.val < 0) {
        p_Failed = true;
        return nullptr;
    }

    const auto s_Status = m_PartnerTemplate.GetResourceInfo().status;

    if (s_Status == RESOURCE_STATUS_VALID && m_PartnerTemplate)
        return m_PartnerTemplate.GetResource();

    if (s_Status == RESOURCE_STATUS_FAILED || m_Now - m_PartnerTemplateRequestTime > 20.0)
        p_Failed = true;

    return nullptr;
}

namespace {
// The partner's stand-in is an NPC to the game, and like any NPC it saw our crimes, panicked,
// counted as a witness and blew our disguise. These two engine entities stop that: an AI modifier
// making it deaf and blind to everything (and unkillable), and a perceptibility switch so other
// NPCs don't perceive it either. Both are pointed at the stand-in after it spawns.
constexpr const char* c_PartnerAiJson = "{\"tempHash\":\"00C0FFEE4A0D0001\",\"tbluHash\":\"00C0FFEE4A0D0002\",\"rootEntity\":\"feedc00f00000001\",\"entities\":{\"feedc00f00000001\":{\"parent\":null,\"name\":\"HitmanCoopPartnerAi\",\"factory\":\"[modules:/zaimodifieractor.class].pc_entitytype\",\"blueprint\":\"[modules:/zaimodifieractor.class].pc_entityblueprint\",\"properties\":{\"m_nScope\":{\"type\":\"EAIModifierScope\",\"value\":\"AIMS_Override\"},\"m_bIgnoreLowNoise\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreSillyHitman\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreAnnoyingHitman\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreDistractions\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreAccidents\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreDeadBodies\":{\"type\":\"bool\",\"value\":true},\"m_bDisableDeadBodySensor\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreWeapons\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreTrespassing\":{\"type\":\"bool\",\"value\":true},\"m_bIgnoreLockdown\":{\"type\":\"bool\",\"value\":true},\"m_bDisableHelpCivilian\":{\"type\":\"bool\",\"value\":true},\"m_bNeverSpectate\":{\"type\":\"bool\",\"value\":true},\"m_bDeafAndBlind\":{\"type\":\"bool\",\"value\":true},\"m_bBlockDeadlyThrow\":{\"type\":\"bool\",\"value\":true},\"m_bBlockMelee\":{\"type\":\"bool\",\"value\":true},\"m_bBlockDeath\":{\"type\":\"bool\",\"value\":true},\"m_bSuppressSocialGreeting\":{\"type\":\"bool\",\"value\":true}}}},\"propertyOverrides\":[],\"overrideDeletes\":[],\"pinConnectionOverrides\":[],\"pinConnectionOverrideDeletes\":[],\"externalScenes\":[],\"subType\":\"template\",\"quickEntityVersion\":3.1,\"extraFactoryDependencies\":[],\"extraBlueprintDependencies\":[],\"comments\":[]}";
constexpr const char* c_PartnerUnseenJson = "{\"tempHash\":\"00C0FFEE4A0D0003\",\"tbluHash\":\"00C0FFEE4A0D0004\",\"rootEntity\":\"feedc00f00000002\",\"entities\":{\"feedc00f00000002\":{\"parent\":null,\"name\":\"HitmanCoopPartnerUnseen\",\"factory\":\"[modules:/zactoraiperceptibilitydisable.class].pc_entitytype\",\"blueprint\":\"[modules:/zactoraiperceptibilitydisable.class].pc_entityblueprint\",\"properties\":{\"m_rEnableCondition\":{\"type\":\"SEntityTemplateReference\",\"value\":\"feedc00f00000003\"}}},\"feedc00f00000003\":{\"parent\":\"feedc00f00000002\",\"name\":\"Always\",\"factory\":\"[modules:/zvaluebool.class].pc_entitytype\",\"blueprint\":\"[modules:/zvaluebool.class].pc_entityblueprint\",\"properties\":{\"m_bValue\":{\"type\":\"bool\",\"value\":true}}}},\"propertyOverrides\":[],\"overrideDeletes\":[],\"pinConnectionOverrides\":[],\"pinConnectionOverrideDeletes\":[],\"externalScenes\":[],\"subType\":\"template\",\"quickEntityVersion\":3.1,\"extraFactoryDependencies\":[],\"extraBlueprintDependencies\":[],\"comments\":[]}";

}

// Each template is loaded once and its factories kept for good: released, the entity's data went
// with them and the game crashed reading the freed names, and a restart keeps the entities from
// before it in the rebuilt mission.
ZEntityRef SdkWorld::SpawnQn(const char* p_Json, const char* p_Name, ZEntityRef p_Parent, QnFactories& p_Keep) {
    if (!p_Keep.loaded) {
        if (!SDK()->LoadQnEntity(ZString(std::string_view(p_Json)), p_Keep.blueprint, p_Keep.factory))
            return {};

        p_Keep.loaded = true;
    }

    ZEntityRef s_Entity;
    Functions::ZEntityManager_NewEntity->Call(Globals::EntityManager, s_Entity, ZString(std::string_view(p_Name)), p_Keep.factory, p_Parent, {}, -1);
    return s_Entity;
}

namespace {
}

void SdkWorld::GuardPartnerAi(bool p_Unseen) {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!m_Partner || !s_Scene)
        return;

    std::string s_Done;

    // Pointed at the stand-in through the engine's property setter, so its change handlers run.
    // (Writing the field directly crashed the game a few seconds later.)
    if (!m_PartnerAi) {
        if (ZEntityRef s_Ai = SpawnQn(c_PartnerAiJson, "HitmanCoopPartnerAi", s_Scene.m_entityRef, m_AiFactories)) {
            if (s_Ai.SetProperty("m_pActor", TEntityRef<IActor>(m_Partner))) {
                s_Ai.GetEntity()->Activate(0);
                // As IOI's own templates do it (a role's OnGetIActor into SetIActor, then Set): the
                // property alone left the modifier without its actor, and the stand-in still noticed
                // a knock-out beside it at once (0.6.0 test).
                const TEntityRef<IActor> s_Actor(m_Partner);
                STypeID* s_Type = (*Globals::TypeRegistry)->GetTypeID(ZString("TEntityRef<IActor>"));

                if (s_Type && s_Type->GetTypeInfo() && s_Type->GetTypeInfo()->m_nTypeSize == sizeof(s_Actor)) {
                    ZObjectRef s_Value;
                    s_Value.Assign(s_Type, const_cast<TEntityRef<IActor>*>(&s_Actor));
                    s_Ai.SignalInputPin(ZString("SetIActor"), s_Value);
                    s_Done += " (actor passed)";
                }

                s_Ai.SignalInputPin(ZString("Set"));
                m_PartnerAi = s_Ai;
                s_Done += " deaf-and-blind";
            }
            else
                s_Done += " (deaf-and-blind: m_pActor not set)";
        }
    }

    if (p_Unseen && !m_PartnerUnseen) {
        if (ZEntityRef s_Unseen = SpawnQn(c_PartnerUnseenJson, "HitmanCoopPartnerUnseen", s_Scene.m_entityRef, m_UnseenFactories)) {
            if (s_Unseen.SetProperty("m_rActor", TEntityRef<ZActor>(m_Partner))) {
                s_Unseen.GetEntity()->Activate(0);
                m_PartnerUnseen = s_Unseen;
                s_Done += " unseen";
            }
            else
                s_Done += " (unseen: m_rActor not set)";
        }
    }

    Logger::Info("[HitmanCoop] Partner stand-in AI guards:{}", s_Done.empty() ? " none (could not be created)" : s_Done);

    if (m_Trace)
        m_Trace("partner: AI guards:" + (s_Done.empty() ? std::string(" none") : s_Done));
}

SdkWorld::Spawn SdkWorld::SpawnPartner(const coop::Transform& p_Xf) {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene)
        return Spawn::Waiting;

    // The actor needs its outfit before it is activated, or it has no body to draw (this is the
    // order the SDK's Assets mod uses). The partner's outfit arrives right after joining; if it
    // hasn't, wear our own for now.
    if (!m_HasPartnerOutfit) {
        if (m_PartnerOutfitWaitStart < 0.0)
            m_PartnerOutfitWaitStart = m_Now;

        if (m_Now - m_PartnerOutfitWaitStart < 3.0)
            return Spawn::Waiting;
    }

    coop::OutfitDesc s_Outfit = m_PartnerOutfit;

    if (!m_HasPartnerOutfit && !GetLocalOutfit(s_Outfit))
        return Spawn::Waiting;

    const coop::OutfitDesc s_Wear = StandInOutfit(s_Outfit);

    // An NPC in one of 47's suits crashes the game when it sees a kill: rather no stand-in yet
    // (a scene without NPCs, like an intro) than that. StandInOutfit looks again next frame.
    if (const auto* s_Kit = FindOutfitKit(s_Wear.repositoryId); s_Kit && s_Kit->m_pInterfaceRef &&
        s_Kit->m_pInterfaceRef->m_bIsHitmanSuit && !m_HeroSuitStandIn)
        return Spawn::Waiting;

    bool s_TemplateFailed = false;
    ZTemplateEntityFactory* s_Template = PartnerTemplate(s_TemplateFailed);

    if (!s_Template) {
        if (!s_TemplateFailed)
            return Spawn::Waiting;

        Logger::Error("[HitmanCoop] The NPC actor template could not be loaded; cannot show partner.");
        Notify("Can't show your partner in this mission (actor template not loaded).");
        return Spawn::Failed;
    }

    ZEntityRef s_Entity;
    SExternalReferences s_ExternalRefs;

    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, s_Entity, "HitmanCoopPartner", m_PartnerTemplate, s_Scene.m_entityRef, s_ExternalRefs, -1
    );

    if (!s_Entity) {
        Logger::Error("[HitmanCoop] Could not spawn the partner actor.");
        Notify("Can't show your partner in this mission (the game refused to spawn it).");
        return Spawn::Failed;
    }

    auto* s_Actor = s_Entity.QueryInterface<ZActor>();

    if (!s_Actor)
        return Spawn::Failed;

    s_Actor->m_sActorName = ZString(m_PartnerName);
    s_Actor->m_bStartEnabled = true;
    s_Actor->m_nOutfitCharset = s_Wear.charset;
    s_Actor->m_nOutfitVariation = s_Wear.variation;
    s_Actor->m_OutfitRepositoryID = ZRepositoryID(ZString(s_Wear.repositoryId));
    s_Actor->m_eRequiredVoiceVariation = EActorVoiceVariation::eAVV_Undefined;
    s_Actor->m_bDisableBumpAnimations = true; // brushed past, it doesn't stagger
    s_Actor->m_bWeaponUnholstered = false;
    s_Actor->m_bBlockDisguisePickup = true; // its clothes can't be taken

    if (m_Brainless & 1)
        s_Actor->m_pCompiledBehaviorTree.m_nResourceIndex.val = -1;

    if (m_Brainless & 2)
        s_Actor->m_DomainConfig = {};

    s_Actor->Activate(0);

    s_Entity.SetProperty("m_eRoomBehaviour", ZSpatialEntity::ERoomBehaviour::ROOM_DYNAMIC);

    if (auto* s_Spatial = s_Entity.QueryInterface<ZSpatialEntity>())
        s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Xf));

    m_Partner = s_Entity;
    m_PartnerActor = s_Actor;
    CrashGuards::SetActor(s_Actor);
    CrashGuards::SetKnowledge(s_Actor->m_pKnowledge);
    ResetPartnerRig(0.5);
    m_RigHidden = false; // spawned visible; hidden on its first pose until matched
    m_RigBody = m_RigAnimator = nullptr;
    m_SkeletonRematches = 0;

    if (m_GuardOnSpawn)
        GuardPartnerAi(true);
    Logger::Info("[HitmanCoop] Spawned partner stand-in.");

    if (m_Trace) {
        char s_Text[128];
        std::snprintf(s_Text, sizeof(s_Text), "partner: spawned, actor %p entity %p", static_cast<void*>(s_Actor),
            static_cast<void*>(s_Entity.m_pObj));
        m_Trace(s_Text);
    }

    // Swap in 47's head (and our own outfit if the partner's hasn't arrived yet).
    if (!m_HasPartnerOutfit)
        m_PartnerOutfit = s_Outfit;

    ApplyPartnerOutfit();

    return Spawn::Done;
}

// Test channel: AI configuration of the stand-in next to a normal NPC of the mission.
std::string SdkWorld::DescribeActorAi() const {
    const auto s_Describe = [](const char* p_Label, ZActor* p_Actor) {
        const auto* s_Inventory = p_Actor->m_pInventoryHandler;
        char s_Text[512];
        std::snprintf(s_Text, sizeof(s_Text),
            "%s: vision=%p btree=%d domain=%p character=%p knowledge=%p weaponIndex=%d grenadeIndex=%d unholstered=%d "
            "keys=%zu enforced=%zu mainWeapon=%p items=%zu weaponId=%s\n",
            p_Label, static_cast<void*>(p_Actor->m_AgentVisionConfiguration.m_pInterfaceRef),
            p_Actor->m_pCompiledBehaviorTree.m_nResourceIndex.val, static_cast<void*>(p_Actor->m_DomainConfig.m_pInterfaceRef),
            static_cast<void*>(p_Actor->m_rCharacter.m_pInterfaceRef),
            static_cast<void*>(p_Actor->m_pKnowledge), p_Actor->m_nWeaponIndex, p_Actor->m_nGrenadeIndex,
            p_Actor->m_bWeaponUnholstered ? 1 : 0, static_cast<size_t>(p_Actor->m_InventoryItemKeys.size()),
            static_cast<size_t>(p_Actor->m_aEnforcedOutfits.size()),
            s_Inventory ? static_cast<void*>(s_Inventory->m_rMainWeapon.m_pInterfaceRef) : nullptr,
            s_Inventory ? static_cast<size_t>(s_Inventory->m_aInventory.size()) : 0,
            s_Inventory ? s_Inventory->m_rWeaponID.ToString().c_str() : "-");
        return std::string(s_Text);
    };

    std::string s_Out = m_PartnerActor ? s_Describe("partner", m_PartnerActor) : "partner: none\n";
    int s_Shown = 0;

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        ZActor* s_Actor = s_Entry.m_pInterfaceRef;

        if (!s_Actor || s_Actor == m_PartnerActor || !s_Actor->m_bEnabled || !s_Actor->IsAlive())
            continue;

        s_Out += s_Describe("npc", s_Actor);

        if (++s_Shown == 8)
            break;
    }

    return s_Out;
}

std::string SdkWorld::DumpAnimators() {
    std::string s_Out;

    if (auto s_Player = SDK()->GetLocalPlayer()) {
        s_Out += BoneProbe::AnimatorTable("47 m_Animator", s_Player.m_pInterfaceRef->m_Animator);
        s_Out += BoneProbe::AnimatorTable("47 morpheme", s_Player.m_pInterfaceRef->m_pMorphemeEntity.m_entityRef);
    }

    if (m_PartnerActor)
        s_Out += BoneProbe::AnimatorTable("stand-in morpheme", m_PartnerActor->m_pMorphemeEntity.m_entityRef);

    if (m_VrAnimatorFactory.m_nResourceIndex.val < 0)
        Globals::ResourceManager->LoadResource(m_VrAnimatorFactory, ResId<"[modules:/zvrboneanimator.class].pc_entitytype">);

    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (m_VrAnimatorFactory && s_Scene && !m_VrAnimator) {
        SExternalReferences s_ExternalRefs;
        Functions::ZEntityManager_NewEntity->Call(Globals::EntityManager, m_VrAnimator, "HitmanCoopProbeVR", m_VrAnimatorFactory,
            s_Scene.m_entityRef, s_ExternalRefs, -1);
    }

    s_Out += BoneProbe::AnimatorTable("ZVRBoneAnimator", m_VrAnimator);
    return s_Out;
}

void SdkWorld::UpdatePoses() {
    void* s_Source = nullptr;
    void* s_Target = nullptr;

    UpdateStandInItem();

    if (m_InScene) {
        if (auto s_Player = SDK()->GetLocalPlayer())
            s_Source = s_Player.m_pInterfaceRef->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();

        if (m_PartnerActor && !m_PartnerHidden && m_PartnerActor->m_pAnimatedActor)
            s_Target = m_PartnerActor->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();
    }

    if (s_Source && !PoseHook::Installed() && !m_PoseHookFailed) {
        m_PoseHookFailed = !PoseHook::Install(s_Source);

        if (m_Trace)
            m_Trace(m_PoseHookFailed ? "pose: could not install the hook" : "pose: hook installed");
    }

    PoseHook::SetSource(s_Source);

    if (m_Mirror && s_Target) {
        // Test: the stand-in copies our own 47.
        if (PoseHook::TakeSourcePose(m_PoseScratch)) {
            if (m_PoseIdentity) {
                for (auto& s_Bone : m_PoseScratch) {
                    s_Bone.rot[0] = s_Bone.rot[1] = s_Bone.rot[2] = 0.f;
                    s_Bone.rot[3] = 1.f;
                }
            }

            PoseHook::SetTargetPose(m_PoseScratch, false);
        }
    }
    else if (!PartnerPosed()) {
        // Still the target, unposed: its own pose is recorded (see CheckPartnerSkeleton).
        PoseHook::ClearTargetPose();
    }

    // The game rebuilt its body (an outfit change does, a little after the request): match again.
    if (m_PartnerActor && s_Target) {
        const void* s_Body = m_PartnerActor->m_pAnimatedActor;

        // A new outfit's skeleton streams in after the request, sometimes after our match: the old
        // match on the new skeleton twisted its arms up (seen on video). Its rig is watched too.
        const void* s_Skeleton = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(s_Target) + 0x50);
        const uint32_t s_Bones = PoseHook::TargetBoneCount();
        const bool s_SkeletonChanged = m_PartnerRig == 1 && m_SkeletonRematches < 4 &&
            ((m_RigSkeleton && s_Skeleton != m_RigSkeleton) || (m_RigBones && s_Bones != m_RigBones));
        m_SkeletonRematches += s_SkeletonChanged ? 1 : 0;

        if ((m_RigBody && s_Body != m_RigBody) || (m_RigAnimator && s_Target != m_RigAnimator) || s_SkeletonChanged) {
            ResetPartnerRig(0.3);

            if (m_Trace)
                m_Trace(s_SkeletonChanged ? "partner: new skeleton, matching it again" : "partner: new body, matching its skeleton again");
        }

        m_RigBody = s_Body;
        m_RigAnimator = s_Target;

        // Remembered as matched only once the match is made (see CheckPartnerSkeleton).
        if (m_PartnerRig != 1) {
            m_RigSkeleton = nullptr;
            m_RigBones = 0;
        }
        else if (!m_RigSkeleton) {
            m_RigSkeleton = s_Skeleton;
            m_RigBones = s_Bones;
        }
    }

    PoseHook::SetTarget(s_Target);
    PickLocalVictim();
    ApplyPendingPuppets();
}

namespace {
void* AnimatorOf(ZActor* p_Actor) {
    return p_Actor ? p_Actor->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>() : nullptr;
}

void BonesToPose(const std::vector<PoseHook::Bone>& p_Bones, coop::Pose& p_Out) {
    p_Out.rotations.resize(p_Bones.size());

    for (size_t i = 0; i < p_Bones.size(); ++i) {
        const auto& s_R = p_Bones[i].rot;
        p_Out.rotations[i] = { s_R[0], s_R[1], s_R[2], s_R[3] };
    }

    p_Out.rootPos = { p_Bones[0].pos[0], p_Bones[0].pos[1], p_Bones[0].pos[2] };
    p_Out.pelvisPos = { p_Bones[1].pos[0], p_Bones[1].pos[1], p_Bones[1].pos[2] };
}

void PoseToBones(const coop::Pose& p_Pose, std::vector<PoseHook::Bone>& p_Out) {
    p_Out.resize(p_Pose.rotations.size());

    for (size_t i = 0; i < p_Pose.rotations.size(); ++i) {
        const auto& s_Q = p_Pose.rotations[i];
        p_Out[i] = { { s_Q.x, s_Q.y, s_Q.z, s_Q.w }, { 0.f, 0.f, 0.f, 0.f } };
    }

    if (p_Out.size() >= 2) {
        p_Out[0].pos[0] = p_Pose.rootPos.x;
        p_Out[0].pos[1] = p_Pose.rootPos.y;
        p_Out[0].pos[2] = p_Pose.rootPos.z;
        p_Out[1].pos[0] = p_Pose.pelvisPos.x;
        p_Out[1].pos[1] = p_Pose.pelvisPos.y;
        p_Out[1].pos[2] = p_Pose.pelvisPos.z;
    }
}

// Bones whose rotations are more than ~90 degrees apart. Two NPCs with different skeleton
// layouts (same bone count) disagreed on about 100 of 231 even both standing; one pose against
// another on the same layout, far fewer.
int CountDisagreeing(const std::vector<PoseHook::Bone>& p_A, const std::vector<PoseHook::Bone>& p_B) {
    int s_Count = 0;

    for (size_t i = 0; i < p_A.size() && i < p_B.size(); ++i) {
        const auto& a = p_A[i].rot;
        const auto& b = p_B[i].rot;

        if (std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0.7f)
            ++s_Count;
    }

    return s_Count + static_cast<int>(p_A.size() > p_B.size() ? p_A.size() - p_B.size() : p_B.size() - p_A.size());
}

coop::Quat QMul(const coop::Quat& a, const coop::Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

coop::Vec3 QRotate(const coop::Quat& q, const coop::Vec3& v) {
    const coop::Quat r = QMul(QMul(q, { v.x, v.y, v.z, 0.f }), { -q.x, -q.y, -q.z, q.w });
    return { r.x, r.y, r.z };
}

// p_Xf moved from where p_From is to where p_To is, keeping its place relative to it.
coop::Transform Rebase(const coop::Transform& p_Xf, const coop::Transform& p_From, const coop::Transform& p_To) {
    const coop::Quat s_Inv = { -p_From.rot.x, -p_From.rot.y, -p_From.rot.z, p_From.rot.w };
    coop::Transform s_Out = p_Xf;
    s_Out.pos = p_To.pos + QRotate(p_To.rot, QRotate(s_Inv, p_Xf.pos - p_From.pos));
    s_Out.rot = coop::Normalize(QMul(p_To.rot, QMul(s_Inv, p_Xf.rot)));
    return s_Out;
}

bool Pacified(ZActor* p_Actor) {
    return p_Actor->IsPacified() || g_Pacified.contains(p_Actor->m_sActorName.c_str());
}

// IsDead() is true for a knocked out NPC too (3.280.0).
bool Dead(ZActor* p_Actor) {
    return p_Actor->IsDead() && !Pacified(p_Actor);
}

// Matches two skeletons' bones by their place in the hierarchy (root to root, then each parent's
// children in order), for target bone -> source bone. 47's skeleton has a bone NPC ones lack (26,
// a child of 14), and everything after it was one index off: limbs got their neighbours' poses.
std::vector<int32_t> MatchBones(const std::vector<int32_t>& p_Source, const std::vector<int32_t>& p_Target) {
    std::vector<int32_t> s_Map(p_Target.size(), -1);
    std::vector<std::vector<int32_t>> s_SourceChildren(p_Source.size());
    std::vector<size_t> s_NextChild(p_Source.size(), 0);

    for (size_t i = 1; i < p_Source.size(); ++i)
        if (p_Source[i] >= 0)
            s_SourceChildren[p_Source[i]].push_back(static_cast<int32_t>(i));

    if (p_Source.empty() || p_Target.empty())
        return s_Map;

    s_Map[0] = 0;

    for (size_t j = 1; j < p_Target.size(); ++j) {
        const int32_t s_Parent = p_Target[j] >= 0 ? s_Map[p_Target[j]] : -1;

        if (s_Parent < 0)
            continue;

        if (auto& s_Next = s_NextChild[s_Parent]; s_Next < s_SourceChildren[s_Parent].size())
            s_Map[j] = s_SourceChildren[s_Parent][s_Next++];
    }

    return s_Map;
}

bool ActorDown(ZActor* p_Actor) {
    return p_Actor->IsDead() || Pacified(p_Actor);
}
}

void SdkWorld::PickLocalVictim() {
    coop::Transform s_Local;
    ZActor* s_Best = nullptr;
    uint64_t s_BestId = 0;
    float s_BestScore = 1e9f;

    // Whoever 47 is in contact with: an NPC within arm's reach (a choke, a melee kill, a push),
    // a body being dragged, and the last victim for a few seconds after it goes down, so the
    // fall plays out. Bodies lying near 47 aren't streamed.
    if (m_InScene && GetLocalAvatar(s_Local)) {
        for (const auto& [s_Id, s_Actor] : m_NpcsById) {
            if (!s_Actor || !s_Actor->m_bEnabled || m_HeldDown.count(s_Id) || s_Actor == m_RemoteVictim)
                continue;

            ZEntityRef s_Ref;
            s_Actor->GetID(s_Ref);
            const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>();

            if (!s_Spatial)
                continue;

            const float s_Distance = coop::Distance(ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos, s_Local.pos);
            const bool s_Down = ActorDown(s_Actor);
            float s_Score;

            if (s_Actor->m_bIsBeingDragged && s_Distance < 3.f)
                s_Score = s_Distance - 10.f;
            // (No time yet on the frame it goes down: it was dropped right there, released standing
            // to the partner, whose copy then got up again.)
            else if (s_Actor == m_LocalVictim && s_Down && (m_LocalVictimDownAt < 0.0 || m_Now - m_LocalVictimDownAt < 4.0) &&
                     s_Distance < 3.f)
                s_Score = s_Distance - 5.f;
            else if (!s_Down && s_Distance < 1.f)
                s_Score = s_Distance;
            else
                continue;

            if (s_Score < s_BestScore) {
                s_Best = s_Actor;
                s_BestId = s_Id;
                s_BestScore = s_Score;
            }
        }
    }

    if (s_Best != m_LocalVictim) {
        m_LocalVictim = s_Best;
        m_LocalVictimId = s_BestId;
        m_LocalVictimDownAt = -1.0;
        m_VictimBones.clear();
        PoseHook::SetSource(AnimatorOf(s_Best), 1);
        std::vector<PoseHook::Bone> s_Stale;
        PoseHook::TakeSourcePose(s_Stale, 1);
    }

    if (m_LocalVictim && m_LocalVictimDownAt < 0.0 && ActorDown(m_LocalVictim))
        m_LocalVictimDownAt = m_Now;
}

bool SdkWorld::GetLocalVictim(uint64_t& p_Id, coop::Transform& p_Xf, coop::Pose& p_Pose, uint8_t& p_Flags) {
    if (m_VictimSendOff)
        return false;

    if (!m_InScene || !m_LocalVictim)
        return false;

    std::vector<PoseHook::Bone> s_Fresh;

    if (PoseHook::TakeSourcePose(s_Fresh, 1))
        m_VictimBones = std::move(s_Fresh);

    if (m_VictimBones.size() < 2)
        return false;

    ZEntityRef s_Ref;
    m_LocalVictim->GetID(s_Ref);
    const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>();

    if (!s_Spatial)
        return false;

    p_Id = m_LocalVictimId;
    p_Xf = ToTransform(s_Spatial->GetObjectToWorldMatrix());
    p_Flags = (Dead(m_LocalVictim) ? 0 : coop::NpcFlag_Alive) |
        (Pacified(m_LocalVictim) ? coop::NpcFlag_Pacified : 0);
    BonesToPose(m_VictimBones, p_Pose);
    return true;
}

void SdkWorld::ShowRemoteVictim(uint64_t p_Id, const coop::Transform& p_Xf, const coop::Pose& p_Pose, uint8_t p_Flags) {
    if (!m_InScene || m_Mirror)
        return;

    ZActor* s_Actor = FindNpc(p_Id);

    if (m_VictimTest) {
        // Test (the bot echoing our own takedown): another NPC near the echo plays the victim,
        // since ours is busy in our own takedown. One whose skeleton doesn't fit is passed over.
        s_Actor = m_RemoteVictim;

        for (float s_Best = 30.f; const auto& [s_Id, s_Npc] : m_NpcsById) {
            if (s_Actor || !s_Npc || s_Npc == m_LocalVictim || !s_Npc->m_bEnabled || ActorDown(s_Npc) || m_RigRejected.contains(s_Npc))
                continue;

            ZEntityRef s_Ref;
            s_Npc->GetID(s_Ref);
            const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>();

            if (!s_Spatial)
                continue;

            if (const float s_D = coop::Distance(ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos, p_Xf.pos); s_D < s_Best) {
                s_Best = s_D;
                s_Actor = s_Npc;
                p_Id = s_Id;
            }
        }

        if (s_Actor == m_RemoteVictim)
            p_Id = m_RemoteVictimId;
    }

    if (!s_Actor || s_Actor == m_LocalVictim)
        return;

    if (s_Actor != m_RemoteVictim) {
        ReleaseRemoteVictim();
        m_RemoteVictim = s_Actor;
        m_RemoteVictimId = p_Id;
        m_RemoteVictimRig = 0;
        m_RemoteVictimDowned = m_HeldDown.count(p_Id) != 0;
        m_RemoteVictimDragged = false;
        m_RemoteVictimStilled = false;

        // In the partner's hands, it reacts to nothing here: a guard both players take down at
        // once otherwise saw our own takedown first and raised the alarm. Its AI comes back when
        // the partner lets go (or for the game's own knock-out, see PacifyForPartner).
        if (!ActorDown(s_Actor) && m_Now - m_SceneReadyAt > 3.0) {
            CrashGuards::SetPuppet(s_Actor, true);
            m_RemoteVictimStilled = true;

            if (m_Trace)
                m_Trace("victim: " + std::string(s_Actor->m_sActorName.c_str()) + " held still for the partner");
        }
    }

    m_RemoteVictimFlags = p_Flags;

    // Down on the partner's screen: knocked out or killed here too, at once, and left to fall the
    // game's own way. Its streamed pose is no use from here: a fall is a ragdoll, and an NPC's
    // animation pose stays standing under one (ours floated up, standing, while it fell). Only a
    // drag (an animation again) is posed from then on.
    if (!(p_Flags & coop::NpcFlag_Alive) || (p_Flags & coop::NpcFlag_Pacified)) {
        if (!m_RemoteVictimDowned) {
            PoseHook::ClearPoseFor(AnimatorOf(s_Actor));
            PutDown(s_Actor, p_Id, p_Flags);
            m_RemoteVictimDowned = true;
            m_RemoteVictimDownPos = p_Xf.pos;
            return;
        }

        if (!m_RemoteVictimDragged && coop::Distance(m_RemoteVictimDownPos, p_Xf.pos) < 0.75f)
            return;

        m_RemoteVictimDragged = true;
    }

    ZEntityRef s_Ref;
    s_Actor->GetID(s_Ref);

    // Held by the partner (a choke, a drag): placed against where the stand-in actually is, which
    // trails the partner a little while it walks, or the victim floated beside its arms.
    coop::Transform s_Xf = p_Xf;

    if (m_Partner && !m_PartnerHidden && m_HasPartnerSentXf && coop::Distance(p_Xf.pos, m_PartnerSentXf.pos) < 3.f) {
        if (const auto* s_Partner = m_Partner.QueryInterface<ZSpatialEntity>())
            s_Xf = Rebase(p_Xf, m_PartnerSentXf, ToTransform(s_Partner->GetObjectToWorldMatrix()));
    }

    if (auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>(); s_Spatial && m_VictimTestApply != 1)
        s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(s_Xf));

    PoseToBones(p_Pose, m_RemoteVictimBones);
    void* s_Animator = AnimatorOf(s_Actor);

    // The pose only goes on a skeleton laid out like the one it came from: the same NPC on both
    // machines should be, but on another layout it came out upside down. Until the NPC's own pose
    // has been seen (registering it with an empty pose records it), it is only moved.
    if (m_RemoteVictimRig == 0) {
        std::vector<PoseHook::Bone> s_Native;

        if (PoseHook::NativePoseOf(s_Animator, s_Native) == 0 || s_Native.empty()) {
            PoseHook::SetPoseFor(s_Animator, {});
            return;
        }

        m_RemoteVictimRigDiffer = CountDisagreeing(m_RemoteVictimBones, s_Native);
        m_RemoteVictimRig = m_RemoteVictimRigDiffer <= 60 ? 1 : -1;

        if (m_Trace)
            m_Trace("victim: " + std::string(s_Actor->m_sActorName.c_str()) + " skeleton " + (m_RemoteVictimRig > 0 ? "fits" : "differs") +
                " (" + std::to_string(m_RemoteVictimRigDiffer) + " bones apart)");

        if (m_RemoteVictimRig < 0 && m_VictimTest) {
            m_RigRejected.insert(s_Actor);
            PoseHook::ClearPoseFor(s_Animator);
            m_RemoteVictim = nullptr;
            return;
        }
    }

    if (m_RemoteVictimRig > 0 && m_VictimTestApply != 2)
        PoseHook::SetPoseFor(s_Animator, m_RemoteVictimBones);
    else
        PoseHook::SetPoseFor(s_Animator, {});

    ++m_VictimPosesShown;
}

void SdkWorld::ReleaseRemoteVictim() {
    ZActor* s_Actor = m_RemoteVictim;
    m_RemoteVictim = nullptr;

    if (!s_Actor || !m_InScene)
        return;

    const bool s_Alive = (m_RemoteVictimFlags & coop::NpcFlag_Alive) != 0;
    const bool s_Down = !s_Alive || (m_RemoteVictimFlags & coop::NpcFlag_Pacified) != 0;

    if (m_Trace)
        m_Trace("victim: released " + std::string(s_Actor->m_sActorName.c_str()) + " flags " + std::to_string(m_RemoteVictimFlags));

    if (m_RemoteVictimStilled && !m_SessionPuppets.contains(m_RemoteVictimId))
        CrashGuards::SetPuppet(s_Actor, false);

    // Left by the partner where their stream put it: not ours to tell them about.
    AgreeBody(m_RemoteVictimId, s_Actor);

    m_RemoteVictimStilled = false;

    // Already put down here: a body dragged keeps the pose it was left in, one that fell stays
    // where it fell.
    if (m_RemoteVictimDowned)
        return;

    // Only brushed past it, its own AI takes over again; down (though never streamed so), put down.
    PoseHook::ClearPoseFor(AnimatorOf(s_Actor));

    if (s_Down)
        PutDown(s_Actor, m_RemoteVictimId, m_RemoteVictimFlags);
}

void SdkWorld::PutDown(ZActor* p_Actor, uint64_t p_Id, uint8_t p_Flags) {
    const bool s_Alive = (p_Flags & coop::NpcFlag_Alive) != 0;

    if (s_Alive)
        m_HeldDown.insert(p_Id);

    if (!ActorDown(p_Actor)) {
        if (s_Alive)
            PacifyForPartner(p_Actor);
        else
            KillForPartner(p_Actor, EDamageEvent::eDE_UNDEFINED, EDeathBehavior::eDB_IMPACT_ANIM);
    }

    if (m_Trace)
        m_Trace(std::string("victim: ") + p_Actor->m_sActorName.c_str() + (s_Alive ? " knocked out" : " killed") + ", as on the partner's screen");
}

std::string SdkWorld::StepBehindNearestNpc(float p_Distance, bool p_Loneliest) {
    coop::Transform s_Local;
    auto s_Player = SDK()->GetLocalPlayer();

    if (!GetLocalAvatar(s_Local) || !s_Player)
        return "no 47";

    ZActor* s_Best = nullptr;
    float s_BestDistance = 30.f;
    float s_BestAlone = 0.f;
    SMatrix s_Matrix;

    // Loneliest: the civilian farthest from any other NPC, anywhere, so whatever happens there
    // has no witness but the stand-in.
    std::vector<coop::Vec3> s_Others;

    if (p_Loneliest) {
        for (const auto& [s_Id, s_Npc] : m_NpcsById) {
            ZEntityRef s_Ref;

            if (s_Npc && s_Npc->m_bEnabled && !ActorDown(s_Npc) && (s_Npc->GetID(s_Ref), s_Ref))
                if (const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>())
                    s_Others.push_back(ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos);
        }
    }

    for (const auto& [s_Id, s_Npc] : m_NpcsById) {
        if (!s_Npc || !s_Npc->m_bEnabled || ActorDown(s_Npc))
            continue;

        // A civilian: guards stand where 47 is trespassing (past the Paris checkpoint), and
        // appearing next to one there turned the whole mission hostile.
        if (const auto* s_Kit = s_Npc->m_rOutfit.m_pInterfaceRef; !s_Kit || s_Kit->m_eActorType != EActorType::eAT_Civilian)
            continue;

        ZEntityRef s_Ref;
        s_Npc->GetID(s_Ref);
        const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial)
            continue;

        const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();

        if (p_Loneliest) {
            const coop::Vec3 s_At = ToTransform(s_M).pos;
            float s_Alone = 1e9f;

            for (const auto& s_Other : s_Others)
                if (const float s_D = coop::Distance(s_Other, s_At); s_D > 0.05f)
                    s_Alone = std::min(s_Alone, s_D);

            if (s_Alone > s_BestAlone && s_Alone < 1e8f) {
                s_Best = s_Npc;
                s_BestAlone = s_Alone;
                s_BestDistance = coop::Distance(s_At, s_Local.pos);
                s_Matrix = s_M;
            }

            continue;
        }

        if (const float s_D = coop::Distance(ToTransform(s_M).pos, s_Local.pos); s_D < s_BestDistance) {
            s_Best = s_Npc;
            s_BestDistance = s_D;
            s_Matrix = s_M;
        }
    }

    if (!s_Best)
        return "no civilian within 30 m";

    // Characters face -Y in their own frame, so "behind" is along +Y.
    s_Matrix.Trans.x += s_Matrix.YAxis.x * p_Distance;
    s_Matrix.Trans.y += s_Matrix.YAxis.y * p_Distance;

    if (auto* s_Spatial = s_Player.m_entityRef.QueryInterface<ZSpatialEntity>())
        s_Spatial->SetObjectToWorldMatrixFromEditor(s_Matrix);

    return std::string(s_Best->m_sActorName.c_str()) + " was " + std::to_string(s_BestDistance) + " m away" +
        (p_Loneliest ? ", " + std::to_string(s_BestAlone) + " m from any other NPC" : std::string());
}

std::string SdkWorld::DescribeVictims() const {
    char s_Text[160];
    std::snprintf(s_Text, sizeof(s_Text), "local %llx%s | remote %llx shown %llu | held %zu",
        static_cast<unsigned long long>(m_LocalVictim ? m_LocalVictimId : 0), m_VictimBones.empty() ? "" : " (posed)",
        static_cast<unsigned long long>(m_RemoteVictim ? m_RemoteVictimId : 0),
        static_cast<unsigned long long>(m_VictimPosesShown), m_HeldDown.size());
    std::string s_Out = s_Text;

    for (const uint64_t s_Id : m_HeldDown) {
        if (ZActor* s_Held = FindNpc(s_Id)) {
            std::snprintf(s_Text, sizeof(s_Text), " | held %s dead=%d pacified=%d", s_Held->m_sActorName.c_str(), s_Held->IsDead(), s_Held->IsPacified());
            s_Out += s_Text;
        }
    }

    if (m_LocalVictim) {
        std::snprintf(s_Text, sizeof(s_Text), " | local %s dead=%d ko=%d dragged=%d | remote flags=%d | known ko %zu", m_LocalVictim->m_sActorName.c_str(),
            Dead(m_LocalVictim), Pacified(m_LocalVictim), m_LocalVictim->m_bIsBeingDragged, m_RemoteVictimFlags, g_Pacified.size());
        s_Out += s_Text;
    }

    // Root and pelvis of our 47 and of our victim, to compare the two skeletons' conventions.
    const auto s_Bones = [&](const char* p_Name, const std::vector<PoseHook::Bone>& p_Bones) {
        for (size_t i = 0; i < 2 && i < p_Bones.size(); ++i) {
            const auto& b = p_Bones[i];
            std::snprintf(s_Text, sizeof(s_Text), " | %s%zu r(%.2f %.2f %.2f %.2f) p(%.2f %.2f %.2f) n%zu", p_Name, i, b.rot[0], b.rot[1],
                b.rot[2], b.rot[3], b.pos[0], b.pos[1], b.pos[2], p_Bones.size());
            s_Out += s_Text;
        }
    };
    s_Bones("own", m_LocalBones);
    s_Bones("vic", m_VictimBones);
    s_Bones("shown", m_RemoteVictimBones);

    if (m_RemoteVictim) {
        std::vector<PoseHook::Bone> s_Native;
        const uint32_t s_Count = PoseHook::NativePoseOf(AnimatorOf(m_RemoteVictim), s_Native);
        s_Bones("native", s_Native);
        s_Out += " | native count " + std::to_string(s_Count);

        s_Out += " | rig " + std::to_string(m_RemoteVictimRig) + " apart " + std::to_string(m_RemoteVictimRigDiffer);
    }
    return s_Out;
}

bool SdkWorld::GetLocalPose(coop::Pose& p_Out) {
    if (!m_InScene)
        return false;

    std::vector<PoseHook::Bone> s_Fresh;

    if (PoseHook::TakeSourcePose(s_Fresh))
        m_LocalBones = std::move(s_Fresh);

    if (m_LocalBones.size() < 2)
        return false;

    p_Out.rotations.resize(m_LocalBones.size());

    for (size_t i = 0; i < m_LocalBones.size(); ++i) {
        const auto& s_R = m_LocalBones[i].rot;
        p_Out.rotations[i] = { s_R[0], s_R[1], s_R[2], s_R[3] };
    }

    p_Out.rootPos = { m_LocalBones[0].pos[0], m_LocalBones[0].pos[1], m_LocalBones[0].pos[2] };
    p_Out.pelvisPos = { m_LocalBones[1].pos[0], m_LocalBones[1].pos[1], m_LocalBones[1].pos[2] };
    return true;
}

void SdkWorld::ShowRemotePose(const coop::Pose& p_Pose) {
    m_RemoteBones.resize(p_Pose.rotations.size());

    for (size_t i = 0; i < p_Pose.rotations.size(); ++i) {
        const auto& s_Q = p_Pose.rotations[i];
        m_RemoteBones[i] = { { s_Q.x, s_Q.y, s_Q.z, s_Q.w }, { 0.f, 0.f, 0.f, 0.f } };
    }

    if (m_RemoteBones.size() >= 2) {
        m_RemoteBones[0].pos[0] = p_Pose.rootPos.x;
        m_RemoteBones[0].pos[1] = p_Pose.rootPos.y;
        m_RemoteBones[0].pos[2] = p_Pose.rootPos.z;
        m_RemoteBones[1].pos[0] = p_Pose.pelvisPos.x;
        m_RemoteBones[1].pos[1] = p_Pose.pelvisPos.y;
        m_RemoteBones[1].pos[2] = p_Pose.pelvisPos.z;
    }

    CheckPartnerSkeleton();

    // Posed once its skeleton is matched to 47's (half a second after it appears or changes
    // outfit): posed before that, bone for bone by index, its arms were twisted up for a moment.
    // Meanwhile it just stands (its AI reasoning is off, so nothing else shows).
    if (!m_Mirror) {
        if (m_PartnerRig == 1)
            PoseHook::SetTargetPose(m_RemoteBones, false);
        else
            PoseHook::ClearTargetPose();
    }

    // Shown a few frames after it is posed with the matched skeleton (not while away, see Hide).
    // Their poses in another layout than the skeleton they sent (in flight around an outfit
    // change): not posed, and hidden, until they agree (at most 2 s, then posed as they come).
    bool s_LayoutOk = m_RemoteParents.empty() || m_RemoteParents.size() == m_RemoteBones.size();

    if (s_LayoutOk)
        m_LayoutMismatchSince = -1.0;
    else if (m_LayoutMismatchSince < 0.0)
        m_LayoutMismatchSince = m_Now;
    else if (m_Now - m_LayoutMismatchSince > 2.0)
        s_LayoutOk = true;

    if (!s_LayoutOk && !m_Mirror)
        PoseHook::ClearTargetPose();

    // Every frame while not ready: the rebuilt body comes back visible on its own (seen on video,
    // its arms up against a wall for half a second after a disguise change).
    if (m_Partner && !m_PartnerHidden && !m_Mirror) {
        const bool s_Ready = m_PartnerRig == 1 && s_LayoutOk && m_Now - m_PartnerRigReadyAt > 0.1;

        if (!s_Ready) {
            if (!m_RigHidden && m_Trace)
                m_Trace("partner: hidden until its skeleton is matched");

            m_Partner.SetProperty("m_bVisible", false);
            m_RigHidden = true;
        }
        else if (m_RigHidden) {
            m_Partner.SetProperty("m_bVisible", true);
            m_RigHidden = false;

            if (m_Trace)
                m_Trace("partner: shown, posed with the matched skeleton");
        }
    }

    m_RemotePoseTime = m_Now;
}

ZActor* SdkWorld::NearestNpc(float* p_Distance) const {
    coop::Transform s_Local;

    if (!GetLocalAvatar(s_Local))
        return nullptr;

    ZActor* s_Best = nullptr;
    float s_BestDistance = 1e9f;

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        ZActor* s_Actor = s_Entry.m_pInterfaceRef;

        if (!s_Actor || s_Actor == m_PartnerActor || !s_Actor->m_bEnabled || !s_Entry.m_entityRef)
            continue;

        const auto* s_Spatial = s_Entry.m_entityRef.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial)
            continue;

        const float s_Distance = coop::Distance(ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos, s_Local.pos);

        if (s_Distance < s_BestDistance) {
            s_Best = s_Actor;
            s_BestDistance = s_Distance;
        }
    }

    if (p_Distance)
        *p_Distance = s_BestDistance;

    return s_Best;
}

std::string SdkWorld::DescribeNearestNpc() const {
    float s_Distance = 0.f;
    ZActor* s_Actor = NearestNpc(&s_Distance);

    if (!s_Actor)
        return "none";

    char s_Text[200];
    std::snprintf(s_Text, sizeof(s_Text), "%s at %.1f m: alive=%d dead=%d pacified=%d dragged=%d enabled=%d",
        s_Actor->m_sActorName.c_str(), s_Distance, s_Actor->IsAlive() ? 1 : 0, s_Actor->IsDead() ? 1 : 0,
        s_Actor->IsPacified() ? 1 : 0, s_Actor->m_bIsBeingDragged ? 1 : 0, s_Actor->m_bEnabled ? 1 : 0);
    return s_Text;
}

std::string SdkWorld::TakedownTest(int p_DamageEvent, int p_DeathBehavior) {
    ZActor* s_Actor = NearestNpc(nullptr);

    if (!s_Actor)
        return "takedown: no NPC";

    const std::string s_Before = DescribeNearestNpc();
    m_TakedownVictim = s_Actor;
    TEntityRef<IItem> s_Item;
    TEntityRef<ZSetpieceEntity> s_Setpiece;
    Functions::ZActor_KillActor->Call(s_Actor, s_Item, s_Setpiece, static_cast<EDamageEvent>(p_DamageEvent),
        static_cast<EDeathBehavior>(p_DeathBehavior));
    return "takedown: before " + s_Before + " | after " + DescribeVictim();
}

namespace {
// An engine struct built from its reflection data, so fields are found by name and the layout
// always matches the running game.
class ReflectedValue {
public:
    explicit ReflectedValue(const char* p_TypeName) {
        STypeID* s_Id = (*Globals::TypeRegistry)->GetTypeID(ZString(std::string_view(p_TypeName)));
        m_Type = s_Id ? s_Id->GetTypeInfo() : nullptr;

        if (!m_Type || !m_Type->IsClass() || !m_Type->m_pTypeFunctions)
            return;

        m_Data = _aligned_malloc(m_Type->m_nTypeSize, std::max<size_t>(16, m_Type->m_nTypeAlignment));
        std::memset(m_Data, 0, m_Type->m_nTypeSize);
        m_Type->m_pTypeFunctions->placementConstruct(m_Data);
    }

    ~ReflectedValue() {
        if (m_Data) {
            m_Type->m_pTypeFunctions->destruct(m_Data);
            _aligned_free(m_Data);
        }
    }

    ReflectedValue(const ReflectedValue&) = delete;
    ReflectedValue& operator=(const ReflectedValue&) = delete;

    bool Valid() const { return m_Data != nullptr; }
    void* Data() const { return m_Data; }

    template <class T>
    bool Set(const char* p_Field, const T& p_Value) {
        const auto* s_Class = reinterpret_cast<const IClassType*>(m_Type);

        for (uint16_t i = 0; m_Data && i < s_Class->m_nProperties; ++i) {
            const auto& s_Property = s_Class->m_pProperties[i];

            if (s_Property.m_pszPropertyName && std::strcmp(s_Property.m_pszPropertyName, p_Field) == 0) {
                const IType* s_FieldType = s_Property.m_propertyInfo.m_Type ? s_Property.m_propertyInfo.m_Type->GetTypeInfo() : nullptr;

                if (!s_FieldType || s_FieldType->m_nTypeSize != sizeof(T))
                    return false;

                std::memcpy(static_cast<uint8_t*>(m_Data) + s_Property.m_propertyInfo.m_nExtraData, &p_Value, sizeof(T));
                return true;
            }
        }

        return false;
    }

private:
    IType* m_Type = nullptr;
    void* m_Data = nullptr;
};
}

std::string SdkWorld::KnockOutTest(uint32_t p_DeathType, uint32_t p_DeathContext) {
    ZActor* s_Actor = NearestNpc(nullptr);

    if (!s_Actor)
        return "knockout: no NPC";

    m_TakedownVictim = s_Actor;
    const std::string s_Before = DescribeVictim();
    ReflectedValue s_Hit("SHitInfo");

    if (!s_Hit.Valid())
        return "knockout: SHitInfo isn't known";

    ZEntityRef s_Victim;
    s_Actor->GetID(s_Victim);
    ZEntityRef s_Owner;

    if (auto s_Player = SDK()->GetLocalPlayer())
        s_Owner = s_Player.m_entityRef;

    const bool s_Ok = s_Hit.Set("m_rHitEntity", s_Victim) && s_Hit.Set("m_rWeaponOwner", s_Owner) &&
        s_Hit.Set("m_nDeathType", p_DeathType) && s_Hit.Set("m_nDeathContext", p_DeathContext);

    const bool s_Result = Hooks::ZActor_YouGotHit->Call(static_cast<IBaseCharacter*>(s_Actor), *static_cast<const SHitInfo*>(s_Hit.Data()));
    return std::string("knockout: fields ") + (s_Ok ? "set" : "NOT set") + ", hit returned " + (s_Result ? "true" : "false") +
        " | before " + s_Before + " | after " + DescribeVictim();
}

bool SdkWorld::PacifyForPartner(ZActor* p_Actor) {
    // A real knock-out: a hit with eDT_PACIFY, as from a thrown object, so the game has it
    // unconscious (not dead, no "non-target killed"). Its events are dropped like a kill's.
    m_PartnerKills.insert(p_Actor->m_sActorName.c_str());
    g_Pacified.insert(p_Actor->m_sActorName.c_str());
    CrashGuards::SetPuppet(p_Actor, false); // its AI back first, for the game's own knock-out handling

    ReflectedValue s_Hit("SHitInfo");

    if (!s_Hit.Valid())
        return false;

    ZEntityRef s_Victim;
    p_Actor->GetID(s_Victim);

    if (!s_Hit.Set("m_rHitEntity", s_Victim) || !s_Hit.Set("m_rWeaponOwner", m_Partner) ||
        !s_Hit.Set("m_nDeathType", static_cast<uint32_t>(EDeathType::eDT_PACIFY)) ||
        !s_Hit.Set("m_nDeathContext", static_cast<uint32_t>(EDeathContext::eDC_NOT_HERO)))
        return false;

    return Hooks::ZActor_YouGotHit->Call(static_cast<IBaseCharacter*>(p_Actor), *static_cast<const SHitInfo*>(s_Hit.Data()));
}

void SdkWorld::NotePacified(const std::string& p_EventJson, bool p_Pacified) {
    const std::string c_Key = "\"ActorName\":\"";

    if (const size_t s_At = p_EventJson.find(c_Key); s_At != std::string::npos) {
        const size_t s_Start = s_At + c_Key.size();
        const std::string s_Name = p_EventJson.substr(s_Start, p_EventJson.find('"', s_Start) - s_Start);

        if (p_Pacified)
            g_Pacified.insert(s_Name);
        else
            g_Pacified.erase(s_Name);
    }
}

std::string SdkWorld::DescribeVictim() const {
    ZActor* s_Actor = m_TakedownVictim;

    if (!s_Actor)
        return "no victim";

    char s_Text[200];
    std::snprintf(s_Text, sizeof(s_Text), "%s: alive=%d dead=%d pacified=%d dragged=%d enabled=%d", s_Actor->m_sActorName.c_str(),
        s_Actor->IsAlive() ? 1 : 0, s_Actor->IsDead() ? 1 : 0, s_Actor->IsPacified() ? 1 : 0, s_Actor->m_bIsBeingDragged ? 1 : 0,
        s_Actor->m_bEnabled ? 1 : 0);
    return s_Text;
}

std::string SdkWorld::DumpBones(size_t p_Bytes) const {
    std::string s_Out;

    if (auto s_Player = SDK()->GetLocalPlayer())
        s_Out += BoneProbe::Dump("47", s_Player.m_pInterfaceRef, p_Bytes);

    s_Out += BoneProbe::Dump("stand-in", m_PartnerActor, p_Bytes);

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        if (s_Entry.m_pInterfaceRef && s_Entry.m_pInterfaceRef != m_PartnerActor && s_Entry.m_pInterfaceRef->m_bEnabled) {
            s_Out += BoneProbe::Dump("npc", s_Entry.m_pInterfaceRef, p_Bytes);
            break;
        }
    }

    return s_Out;
}

std::string SdkWorld::DescribeCamera() const {
    const auto s_Camera = Functions::GetCurrentCamera->Call();

    if (!s_Camera)
        return "null";

    const SMatrix s_M = s_Camera->GetObjectToWorldMatrix();
    char s_Text[320];
    std::snprintf(s_Text, sizeof(s_Text),
        "{\"pos\":[%.2f,%.2f,%.2f],\"x\":[%.3f,%.3f,%.3f],\"y\":[%.3f,%.3f,%.3f],\"z\":[%.3f,%.3f,%.3f]}",
        s_M.Trans.x, s_M.Trans.y, s_M.Trans.z, s_M.XAxis.x, s_M.XAxis.y, s_M.XAxis.z, s_M.YAxis.x, s_M.YAxis.y,
        s_M.YAxis.z, s_M.ZAxis.x, s_M.ZAxis.y, s_M.ZAxis.z);
    return s_Text;
}

std::string SdkWorld::StandInTest(const std::string& p_What) {
    if (!m_PartnerActor || !m_Partner)
        return "standin: none";

    // IActor (at ZActor +0x2E8): +0x08 enables, +0x10 disables (ZActorPickerEffectState, 3.280.0).
    using Fn = void (*)(void*);
    auto* s_Actor = reinterpret_cast<uint8_t*>(m_PartnerActor) + 0x2E8;
    void** s_Table = *reinterpret_cast<void***>(s_Actor);

    if (p_What == "disable" && m_PartnerActor->m_bEnabled)
        reinterpret_cast<Fn>(s_Table[2])(s_Actor);
    else if (p_What == "enable" && !m_PartnerActor->m_bEnabled)
        reinterpret_cast<Fn>(s_Table[1])(s_Actor);
    else if (p_What == "visible")
        m_Partner.SetProperty("m_bVisible", true);
    else if (p_What == "invisible")
        m_Partner.SetProperty("m_bVisible", false);

    return "standin " + p_What + ": " + DescribePartner();
}

bool SdkWorld::GetLocalSkeleton(std::vector<int16_t>& p_Out) {
    void* s_Ours = nullptr;

    if (auto s_Player = SDK()->GetLocalPlayer())
        s_Ours = s_Player.m_pInterfaceRef->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();

    const uint32_t s_Bones = PoseHook::SourceBoneCount();

    if (!m_InScene || !s_Ours || s_Bones < 2)
        return false;

    // Looked for again only when our rig changes (an outfit can bring another).
    const void* s_Rig = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(s_Ours) + 0x50);

    if (s_Rig != m_LocalParentsRig || s_Bones != m_LocalParentsBones) {
        m_LocalParentsRig = s_Rig;
        m_LocalParentsBones = s_Bones;
        m_LocalParents.clear();
        const auto s_Found = RigScan::FindParents(s_Ours, s_Bones);

        if (!s_Found.empty())
            for (const int32_t s_Parent : s_Found[0].parents)
                m_LocalParents.push_back(static_cast<int16_t>(s_Parent));
    }

    p_Out = m_LocalParents;
    return !p_Out.empty();
}

void SdkWorld::SetRemoteSkeleton(const std::vector<int16_t>& p_Parents) {
    m_RemoteParents.assign(p_Parents.begin(), p_Parents.end());

    // Matched again (hidden meanwhile) as soon as their poses are in it.
    if (m_PartnerActor) {
        ResetPartnerRig(0.05);
        m_SkeletonRematches = 0;
    }

    if (m_Trace)
        m_Trace("partner: their 47's skeleton arrived (" + std::to_string(p_Parents.size()) + " bones)");
}

std::string SdkWorld::DumpRemoteBones(size_t p_Count) const {
    std::string s_Out;

    for (size_t i = 0; i < m_RemoteBones.size() && i < p_Count; ++i) {
        char s_Line[160];
        std::snprintf(s_Line, sizeof(s_Line), "remote %2zu %7.3f %7.3f %7.3f %7.3f  pos %7.3f %7.3f %7.3f\n", i, m_RemoteBones[i].rot[0],
            m_RemoteBones[i].rot[1], m_RemoteBones[i].rot[2], m_RemoteBones[i].rot[3], m_RemoteBones[i].pos[0], m_RemoteBones[i].pos[1],
            m_RemoteBones[i].pos[2]);
        s_Out += s_Line;
    }

    return s_Out;
}

std::string SdkWorld::DescribePartner() const {
    if (!m_PartnerActor)
        return "none";

    const int32_t s_Tension = CrashGuards::TensionOf(m_PartnerActor->m_pKnowledge);

    char s_Text[512];
    std::snprintf(s_Text, sizeof(s_Text),
        "anim=%p enabled=%d activated=%d alive=%d dead=%d hidden=%d outfit=%s tension=%d icon=%d enforcer=%d%d%d "
        "muted_lines=%u/%u calmed=%u highest_wanted=%d no_close_combat=%d no_drag=%d pose_updates=%llu skipped_thinks=%u puppets=%zu",
        static_cast<void*>(m_PartnerActor->m_pAnimatedActor),
        m_PartnerActor->m_bEnabled ? 1 : 0, m_PartnerActor->m_bActorActivated ? 1 : 0,
        m_PartnerActor->IsAlive() ? 1 : 0, m_PartnerActor->IsDead() ? 1 : 0, m_PartnerActor->m_bBodyHidden ? 1 : 0,
        m_PartnerActor->m_OutfitRepositoryID.ToString().c_str(), s_Tension, m_PartnerActor->m_bHasAIIcon ? 1 : 0,
        m_PartnerActor->m_bIsActiveEnforcer ? 1 : 0, m_PartnerActor->m_bIsPotentialEnforcer ? 1 : 0,
        m_PartnerActor->m_bIsDynamicEnforcer ? 1 : 0, CrashGuards::MutedLines(), CrashGuards::AllLines(), CrashGuards::Calmed(), CrashGuards::HighestWanted(),
        m_PartnerActor->m_bUnk38 ? 1 : 0, m_PartnerActor->m_bUnk39 ? 1 : 0, static_cast<unsigned long long>(PoseHook::TargetCalls()), CrashGuards::SkippedThinks(), CrashGuards::PuppetCount());
    return s_Text;
}

void SdkWorld::ShowRemoteAvatar(const coop::Transform& p_Xf) {
    if (!m_InScene || !m_PartnerEnabled)
        return;

    // Never an enforcer: in the partner's disguise the stand-in counted as one for that outfit
    // and saw through ours. The game may set these again, so every frame.
    if (m_PartnerActor) {
        m_PartnerActor->m_bIsActiveEnforcer = false;
        m_PartnerActor->m_bIsPotentialEnforcer = false;
        m_PartnerActor->m_bIsDynamicEnforcer = false;
        m_PartnerActor->m_bActiveSentry = false;
        // Close combat (Subdue, Snap Neck, Push) and Drag blocked, the bits the game's own
        // ZActorPickerEffectState sets (m_bBlockCloseCombat, m_bBlockDrag): no prompt on it.
        m_PartnerActor->m_bUnk38 = true;
        m_PartnerActor->m_bUnk39 = true;
        // Its reasoning off once its AI has set itself up as a calm, living NPC (2 s after it shows).
        CrashGuards::SetKnowledge(m_PartnerActor->m_pKnowledge, m_Now - m_PartnerShownTime < 2.0);
    }


    m_PartnerSentXf = p_Xf;
    m_HasPartnerSentXf = true;

    // Horizontal speed, smoothed over a few frames, picks walk / jog / sprint.
    if (m_PartnerWantedTime > 0.0 && m_Now > m_PartnerWantedTime) {
        const float s_Dt = static_cast<float>(m_Now - m_PartnerWantedTime);
        const float s_Dx = p_Xf.pos.x - m_PartnerWanted.x, s_Dy = p_Xf.pos.y - m_PartnerWanted.y;
        const float s_Speed = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy) / s_Dt;
        const float s_Blend = std::min(1.f, s_Dt / 0.3f);
        m_PartnerSpeed += (std::min(s_Speed, 15.f) - m_PartnerSpeed) * s_Blend;

        if (s_Speed < 15.f) {
            m_PartnerVelocity.x += (s_Dx / s_Dt - m_PartnerVelocity.x) * s_Blend;
            m_PartnerVelocity.y += (s_Dy / s_Dt - m_PartnerVelocity.y) * s_Blend;
        }
    }

    m_PartnerWanted = p_Xf.pos;
    m_PartnerWantedTime = m_Now;

    if (!m_Partner) {
        if (m_PartnerSpawnFailed)
            return;

        const Spawn s_Result = SpawnPartner(p_Xf);

        if (s_Result != Spawn::Done) {
            m_PartnerSpawnFailed = s_Result == Spawn::Failed;
            return;
        }

        m_PartnerShownTime = m_Now;
    }

    if (m_PartnerHidden) {
        // Back from a leave: show the stand-in again, where the partner is now.
        if (auto* s_Spatial = m_Partner.QueryInterface<ZSpatialEntity>())
            s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Xf));

        if (m_Trace)
            m_Trace("partner: showing again");

        m_Partner.SetProperty("m_bVisible", true);
        m_PartnerHidden = false;
        m_RigHidden = false; // hidden again at once if its skeleton isn't matched yet
        m_PartnerShownTime = m_Now;
    }

    // Only when it isn't already there. The stand-in is still an NPC, and its own AI would walk it
    // off (away from a body, say), so this compares with where it actually is.
    auto* s_Spatial = m_Partner.QueryInterface<ZSpatialEntity>();

    if (!s_Spatial)
        return;

    const coop::Transform s_Now = ToTransform(s_Spatial->GetObjectToWorldMatrix());

    // With the partner's skeleton pose arriving, the stand-in is placed exactly where the
    // partner is and moves like them; the AI must not walk it anywhere.
    if (PartnerPosed()) {
        if (m_Puppet.Attached())
            m_Puppet.Pause();
    }
    else if (m_PuppetMode && m_Now - m_PartnerShownTime > 1.5) {
        if (!m_Puppet.Attached() && !m_PuppetFailed) {
            if (m_PuppetRequestStart < 0.0)
                m_PuppetRequestStart = m_Now;

            const auto s_Load = m_Puppet.Request(m_Trace);

            if (s_Load == PartnerPuppet::Load::Ready)
                m_PuppetFailed = !m_Puppet.Attach(m_Partner, p_Xf, m_Trace);
            else if (s_Load == PartnerPuppet::Load::Failed || m_Now - m_PuppetRequestStart > 15.0)
                m_PuppetFailed = true;
        }

        if (m_Puppet.Attached()) {
            // Characters face their local -Y; look 10 m ahead at head height.
            const DirectX::XMVECTOR s_Q = DirectX::XMVectorSet(p_Xf.rot.x, p_Xf.rot.y, p_Xf.rot.z, p_Xf.rot.w);
            DirectX::XMFLOAT3 s_Fwd;
            DirectX::XMStoreFloat3(&s_Fwd, DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.f, -1.f, 0.f, 0.f), s_Q));
            const coop::Vec3 s_Look { p_Xf.pos.x + 10.f * s_Fwd.x, p_Xf.pos.y + 10.f * s_Fwd.y, p_Xf.pos.z + 1.6f };
            m_Puppet.Resume();
            const bool s_Promoted = m_Puppet.Steer(p_Xf, m_PartnerVelocity, s_Now.pos, s_Look, m_Now);

            if (m_Now < m_TrackUntil) {
                if (FILE* s_File = std::fopen(m_TrackPath.c_str(), "a")) {
                    const coop::Vec3 s_Goal = m_Puppet.Goal();
                    std::fprintf(s_File, "%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d\n", m_Now - m_TrackStart, p_Xf.pos.x,
                        p_Xf.pos.y, s_Now.pos.x, s_Now.pos.y, s_Goal.x, s_Goal.y, m_PartnerSpeed, s_Promoted ? 1 : 0, m_Puppet.SpeedLevel());
                    std::fclose(s_File);
                }
            }

            // Only a big gap (a vault, a ladder, lag) is closed by teleporting.
            if (coop::Distance(p_Xf.pos, s_Now.pos) > 8.f)
                s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Xf));

            return;
        }
    }

    // Test channel: where it was asked to be and where it actually is, every frame.
    if (m_Now < m_TrackUntil) {
        if (FILE* s_File = std::fopen(m_TrackPath.c_str(), "a")) {
            std::fprintf(s_File, "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", m_Now - m_TrackStart, p_Xf.pos.x, p_Xf.pos.y, p_Xf.pos.z,
                s_Now.pos.x, s_Now.pos.y, s_Now.pos.z, PartnerPosed() ? 1 : 0);
            std::fclose(s_File);
        }
    }

    if (coop::Distance(p_Xf.pos, s_Now.pos) < 0.01f && coop::AngleBetween(p_Xf.rot, s_Now.rot) < 0.005f)
        return;

    s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Xf));
}

void SdkWorld::HideRemoteAvatar() {
    if (!m_Partner || m_PartnerHidden)
        return;

    // Only made invisible, in place. Each of these crashed the game within a second (tested in
    // Paris, 3.280.0): deleting the live actor, teleporting it far outside the map, Deactivate().
    if (m_Trace)
        m_Trace("partner: hiding");

    // Stop posing it first: a restart tears the actor down (m_pAnimatedActor goes null) and the
    // game then crashed reading through it (HITMAN3+0x16acc2 / +0x177ad0, 3.280.0).
    PoseHook::SetTarget(nullptr);
    PoseHook::ClearTargetPose();
    m_PartnerHidden = true;

    if (!m_PartnerActor || !m_PartnerActor->m_pAnimatedActor) {
        if (m_Trace)
            m_Trace("partner: being torn down, left alone");
        return;
    }

    m_Partner.SetProperty("m_bVisible", false);

    if (m_Trace)
        m_Trace("partner: hidden");
}

void SdkWorld::SetRemoteOutfit(const coop::OutfitDesc& p_Outfit) {
    m_PartnerOutfit = p_Outfit;
    m_HasPartnerOutfit = true;

    if (m_PartnerActor)
        ApplyPartnerOutfit();
}

// Mirrors Assets::EquipOutfit from the SDK: temporarily point the kit's "Actor" variations at
// the "HeroA" ones so the stand-in wears the outfit with 47's head, then restore them.
const TEntityRef<ZGlobalOutfitKit>* SdkWorld::FindOutfitKit(const std::string& p_RepositoryId) {
    const auto s_It = Globals::ContentKitManager->m_repositoryGlobalOutfitKits.find(ZRepositoryID(ZString(p_RepositoryId)));
    return s_It == Globals::ContentKitManager->m_repositoryGlobalOutfitKits.end() ? nullptr : &s_It->second;
}

// What the stand-in actually wears. 47's own suits are player outfits: an NPC wearing one crashes
// the game as soon as it reacts to something (it looks for NPC data those suits don't have; seen
// in Paris, 3.280.0, within seconds of a kill near the stand-in). So for those, the stand-in wears
// the mission's most common civilian outfit instead, still with 47's head. Disguises taken from
// NPCs are NPC outfits and are worn as they are.
coop::OutfitDesc SdkWorld::StandInOutfit(const coop::OutfitDesc& p_Wanted) {
    const auto* s_Wanted = FindOutfitKit(p_Wanted.repositoryId);

    if (s_Wanted && s_Wanted->m_pInterfaceRef && (!s_Wanted->m_pInterfaceRef->m_bIsHitmanSuit || m_HeroSuitStandIn))
        return p_Wanted;

    if (!m_HasCivilianOutfit) {
        std::unordered_map<ZGlobalOutfitKit*, std::pair<int, ZActor*>> s_Counts;

        for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
            ZActor* s_Actor = s_Entry.m_pInterfaceRef;
            ZGlobalOutfitKit* s_Kit = s_Actor ? s_Actor->m_rOutfit.m_pInterfaceRef : nullptr;

            if (!s_Kit || s_Actor == m_PartnerActor || !s_Actor->m_bEnabled || s_Kit->m_bIsHitmanSuit)
                continue;

            // Civilian outfits first; any other NPC outfit only if the mission has no civilians,
            // because an NPC in one of 47's suits crashes the game when it sees a kill.
            const bool s_Civilian = s_Kit->m_eActorType == EActorType::eAT_Civilian && !s_Kit->m_bWeaponsAllowed;
            auto& s_Count = s_Counts[s_Kit];
            s_Count.first += s_Civilian ? 1000 : 1;
            s_Count.second = s_Actor;
        }

        int s_BestScore = -1;

        for (const auto& [s_Kit, s_Count] : s_Counts) {
            // Prefer outfits 47 can wear: they have the HeroA variations that carry his head.
            const int s_Score = s_Count.first + (s_Kit->m_bHeroDisguiseAvailable ? 100000 : 0);

            if (s_Score > s_BestScore) {
                s_BestScore = s_Score;
                m_CivilianOutfit.repositoryId = OutfitKitToRepositoryId(s_Kit);
                m_CivilianOutfit.charset = s_Count.second->m_nOutfitCharset;
                m_CivilianOutfit.variation = 0; // the first variation is the one with a 47 version
            }
        }

        m_HasCivilianOutfit = s_BestScore >= 0 && !m_CivilianOutfit.repositoryId.empty();

        if (m_Trace)
            m_Trace("partner: stand-in outfit for 47's suits: " + (m_HasCivilianOutfit ? m_CivilianOutfit.repositoryId : "none found"));
    }

    return m_HasCivilianOutfit ? m_CivilianOutfit : p_Wanted;
}

void SdkWorld::ApplyPartnerOutfit() {
    const coop::OutfitDesc s_Use = StandInOutfit(m_PartnerOutfit);
    const auto* s_KitPtr = FindOutfitKit(s_Use.repositoryId);

    if (!s_KitPtr) {
        Logger::Warn("[HitmanCoop] Partner outfit {} is not known here.", s_Use.repositoryId);
        return;
    }

    const TEntityRef<ZGlobalOutfitKit>& s_KitRef = *s_KitPtr;
    ZGlobalOutfitKit* s_Kit = s_KitRef.m_pInterfaceRef;

    if (!s_Kit || s_Use.charset < 0 || static_cast<size_t>(s_Use.charset) >= s_Kit->m_aCharSets.size())
        return;

    ZOutfitVariationCollection* s_Collection = s_Kit->m_aCharSets[s_Use.charset].m_pInterfaceRef;

    if (!s_Collection)
        return;

    auto* s_ActorType = &s_Collection->m_aCharacters[static_cast<size_t>(ECharSetCharacterType::ECSCT_Actor)];
    auto* s_HeroType = &s_Collection->m_aCharacters[static_cast<size_t>(ECharSetCharacterType::ECSCT_HeroA)];

    // Only the variations that have a 47 version get his head; NPCs often wear later ones.
    int s_Variation = s_Use.variation;

    if (s_HeroType->m_pInterfaceRef && s_HeroType->m_pInterfaceRef->m_aVariations.size() > 0 &&
        static_cast<size_t>(s_Variation) >= s_HeroType->m_pInterfaceRef->m_aVariations.size())
        s_Variation = 0;

    std::vector<ZRuntimeResourceID> s_Original;

    const bool s_UseHero = s_ActorType->m_pInterfaceRef && s_HeroType->m_pInterfaceRef;

    if (s_UseHero) {
        auto& s_ActorVariations = s_ActorType->m_pInterfaceRef->m_aVariations;
        const auto& s_HeroVariations = s_HeroType->m_pInterfaceRef->m_aVariations;
        const size_t s_Count = std::min(s_ActorVariations.size(), s_HeroVariations.size());

        for (const auto& s_Variation : s_ActorVariations)
            s_Original.push_back(s_Variation.m_pInterfaceRef ? s_Variation.m_pInterfaceRef->m_Outfit : ZRuntimeResourceID {});

        for (size_t i = 0; i < s_Count; ++i) {
            if (s_ActorVariations[i].m_pInterfaceRef && s_HeroVariations[i].m_pInterfaceRef)
                s_ActorVariations[i].m_pInterfaceRef->m_Outfit = s_HeroVariations[i].m_pInterfaceRef->m_Outfit;
        }
    }

    // Never one of 47's suits (see StandInOutfit): keep what it wears.
    if (s_Kit->m_bIsHitmanSuit && !m_HeroSuitStandIn) {
        if (m_Trace)
            m_Trace("partner: no NPC outfit to stand in for 47's suit here, keeping the current one");

        return;
    }

    Functions::ZActor_SetOutfit->Call(m_PartnerActor, s_KitRef, s_Use.charset, s_Variation, false);

    // Matched again for the new body (see CheckPartnerSkeleton), hidden meanwhile.
    ResetPartnerRig(0.5);
    m_SkeletonRematches = 0;

    if (!s_Original.empty()) {
        auto& s_ActorVariations = s_ActorType->m_pInterfaceRef->m_aVariations;

        for (size_t i = 0; i < std::min(s_ActorVariations.size(), s_Original.size()); ++i) {
            if (s_ActorVariations[i].m_pInterfaceRef)
                s_ActorVariations[i].m_pInterfaceRef->m_Outfit = s_Original[i];
        }
    }
}

void SdkWorld::ResetPartnerRig(double p_CheckIn) {
    // Not even one more frame with the old match on the new body.
    PoseHook::ClearTargetPose();
    m_PartnerRig = 0;
    m_PartnerRigCheckAt = m_Now + p_CheckIn;
    m_RigParents.clear();
    m_BoneMap.clear();
    PoseHook::SetTargetMap({});
    PoseHook::SetTargetMask({});
}

void SdkWorld::CheckPartnerSkeleton() {
    if (!m_PartnerActor || m_PartnerRig != 0 || m_Now < m_PartnerRigCheckAt)
        return;

    // Decided by the two skeletons' layouts (their parent tables), never by comparing poses: those
    // differ whenever the two are doing different things, and 0.5.1 waited for them to match,
    // leaving the stand-in to its own AI meanwhile (it yelled, fled and snapped back).
    const uint32_t s_TargetBones = PoseHook::TargetBoneCount();
    const uint32_t s_SourceBones = PoseHook::SourceBoneCount();
    void* s_Ours = nullptr;

    if (auto s_Player = SDK()->GetLocalPlayer())
        s_Ours = s_Player.m_pInterfaceRef->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();

    // The partner's own skeleton when they sent it (and their poses are in that layout yet);
    // else ours, as before they could send it.
    const bool s_HaveRemote = !m_RemoteParents.empty();

    if (s_HaveRemote && m_RemoteParents.size() != m_RemoteBones.size() && m_Now - m_PartnerRigCheckAt < 2.0)
        return;

    if (s_TargetBones > 1 && (s_HaveRemote || (s_SourceBones > 1 && s_Ours))) {
        const auto s_Theirs = RigScan::FindParents(AnimatorOf(m_PartnerActor), s_TargetBones);
        std::vector<RigScan::Found> s_Hero;

        if (s_HaveRemote)
            s_Hero.push_back({ "sent by the partner", m_RemoteParents, 2 });
        else
            s_Hero = RigScan::FindParents(s_Ours, s_SourceBones);

        if (!s_Theirs.empty() && !s_Hero.empty()) {
            // NPC skeletons lack one of 47's bones (26), so every later bone was one index off and
            // limbs got their neighbours' poses: matched bone for bone instead.
            m_RigParents = s_Theirs[0].parents;
            m_BoneMap = s_Theirs[0].parents == s_Hero[0].parents ? std::vector<int32_t> {} : MatchBones(s_Hero[0].parents, s_Theirs[0].parents);
            PoseHook::SetTargetMap(m_BoneMap);
            m_PartnerRig = 1;
            m_PartnerRigReadyAt = m_Now;

            if (m_Trace) {
                // How far its own pose is from the partner's, bone for bone as mapped (both
                // standing: a few; a rig whose bones are oriented differently: many).
                std::vector<PoseHook::Bone> s_Native, s_Mapped;
                PoseHook::TargetNativePose(s_Native);

                for (size_t j = 0; j < s_Native.size(); ++j) {
                    const int32_t s_From = m_BoneMap.empty() ? static_cast<int32_t>(j) : m_BoneMap[j];
                    s_Mapped.push_back(s_From >= 0 && static_cast<size_t>(s_From) < m_RemoteBones.size() ? m_RemoteBones[s_From] : s_Native[j]);
                }

                m_Trace(std::string(m_BoneMap.empty() ? "partner: skeleton is the same as 47's" : "partner: skeleton matched to 47's bone for bone") +
                    " (" + std::to_string(s_TargetBones) + " bones, partner's 47 " + std::to_string(s_Hero[0].parents.size()) +
                    (s_HaveRemote ? " as they sent it, " : " taken as ours, ") +
                    std::to_string(CountDisagreeing(s_Native, s_Mapped)) + " apart from its own pose)");
            }

            return;
        }
    }

    // Skeletons not readable yet: try again shortly, and after 5 s pose as it is.
    if (m_Now - m_PartnerRigCheckAt < 5.0)
        return;

    m_PartnerRig = 1;
    m_PartnerRigReadyAt = m_Now;

    if (m_Trace)
        m_Trace("partner: skeletons couldn't be read, posing bone for bone by index");
}

bool SdkWorld::IsPartnerActor(const void* p_Character) const {
    return m_PartnerActor && p_Character && static_cast<const void*>(m_PartnerActor) == p_Character;
}

void SdkWorld::ListNpcs(std::vector<coop::NpcState>& p_Out) const {
    m_NpcsById.clear();

    if (!m_InScene)
        return;

    // Entity ids come from the mission's templates, so the same NPC has the same id on both machines.
    // An id shared by two actors (one template instanced twice) can't be matched up reliably
    // between the machines, so those actors aren't synced at all.
    auto& s_IdCount = m_IdCountScratch;
    s_IdCount.clear();

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        if (s_Entry.m_pInterfaceRef && s_Entry.m_pInterfaceRef != m_PartnerActor && s_Entry.m_entityRef &&
            s_Entry.m_entityRef->GetType())
            ++s_IdCount[s_Entry.m_entityRef->GetType()->m_nEntityID];
    }

    // Disabled actors are pooled NPCs that haven't entered the mission yet (Paris has 30, parked
    // under the map). They aren't dead, so they're left out until the game enables them.

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        ZActor* s_Actor = s_Entry.m_pInterfaceRef;

        if (!s_Actor || s_Actor == m_PartnerActor || !s_Entry.m_entityRef || !s_Entry.m_entityRef->GetType() ||
            !s_Actor->m_bEnabled)
            continue;

        const uint64_t s_Id = s_Entry.m_entityRef->GetType()->m_nEntityID;

        if (s_IdCount[s_Id] > 1)
            continue;

        const auto* s_Spatial = s_Entry.m_entityRef.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial)
            continue;

        coop::NpcState s_State;
        s_State.id = s_Id;
        s_State.xf = ToTransform(s_Spatial->GetObjectToWorldMatrix());
        s_State.flags = (Dead(s_Actor) ? 0 : coop::NpcFlag_Alive) | (Pacified(s_Actor) ? coop::NpcFlag_Pacified : 0);
        s_State.tension = static_cast<uint8_t>(std::clamp<int32_t>(CrashGuards::TensionOf(s_Actor->m_pKnowledge), 0, 255));

        // Knocked out on the partner's screen: dead here only for want of a knock-out call.
        if (m_HeldDown.count(s_Id))
            s_State.flags = coop::NpcFlag_Alive | coop::NpcFlag_Pacified;

        m_NpcsById[s_Id] = s_Actor;
        p_Out.push_back(s_State);
    }
}

bool SdkWorld::GetPartnerPosition(coop::Vec3& p_Out) const {
    if (!m_Partner)
        return false;

    const auto* s_Spatial = m_Partner.QueryInterface<ZSpatialEntity>();

    if (!s_Spatial)
        return false;

    p_Out = ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos;
    return true;
}

std::string SdkWorld::DescribeNotAliveNpcs() const {
    std::string s_Out;
    int s_Count = 0;

    for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
        ZActor* s_Actor = s_Entry.m_pInterfaceRef;

        if (!s_Actor || s_Actor->IsAlive() || !s_Entry.m_entityRef || !s_Entry.m_entityRef->GetType())
            continue;

        const auto* s_Spatial = s_Entry.m_entityRef.QueryInterface<ZSpatialEntity>();
        const auto s_Pos = s_Spatial ? ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos : coop::Vec3 {};
        char s_Line[256];
        std::snprintf(s_Line, sizeof(s_Line),
            "npc %016llx dead=%d enabled=%d activated=%d hidden=%d startEnabled=%d at (%.1f, %.1f, %.1f)\n",
            static_cast<unsigned long long>(s_Entry.m_entityRef->GetType()->m_nEntityID), s_Actor->IsDead() ? 1 : 0,
            s_Actor->m_bEnabled ? 1 : 0, s_Actor->m_bActorActivated ? 1 : 0, s_Actor->m_bBodyHidden ? 1 : 0,
            s_Actor->m_bStartEnabled ? 1 : 0, s_Pos.x, s_Pos.y, s_Pos.z);
        s_Out += s_Line;
        ++s_Count;
    }

    return std::to_string(s_Count) + " actors not alive\n" + s_Out;
}

void SdkWorld::CollectAlerts(std::vector<coop::NpcAlert>& p_Out) {
    for (const auto& [s_Id, s_Npc] : m_NpcsById) {
        if (!s_Npc || s_Npc == m_PartnerActor || !s_Npc->m_bEnabled || ActorDown(s_Npc) || p_Out.size() >= coop::kMaxAlerts)
            continue;

        const int32_t s_Tension = CrashGuards::TensionOf(s_Npc->m_pKnowledge);

        // Only what our game came to by itself: an NPC we keep alert for the partner is theirs.
        if (s_Tension <= static_cast<int32_t>(EGameTension::EGT_Ambient) ||
            s_Tension <= CrashGuards::AlertFloor(s_Npc->m_pKnowledge))
            continue;

        p_Out.push_back({ s_Id, static_cast<uint8_t>(std::min<int32_t>(s_Tension, 255)) });
    }
}

void SdkWorld::ApplyPeerAlerts(const std::vector<coop::NpcAlert>& p_Alerts) {
    // At most "alerted": the partner's guards searching or hunting for them make ours alert and
    // searching too, but never hunting or fighting our 47 for what the partner did.
    constexpr int32_t c_Cap = static_cast<int32_t>(EGameTension::EGT_AlertedHigh);
    std::vector<std::pair<const void*, int32_t>> s_Floors;

    for (const auto& s_Alert : p_Alerts) {
        ZActor* s_Npc = FindNpc(s_Alert.id);

        if (!s_Npc || s_Npc == m_PartnerActor || !s_Npc->m_pKnowledge || ActorDown(s_Npc))
            continue;

        const int32_t s_Floor = std::min<int32_t>(s_Alert.tension, c_Cap);
        s_Floors.emplace_back(s_Npc->m_pKnowledge, s_Floor);

        if (CrashGuards::TensionOf(s_Npc->m_pKnowledge) < s_Floor)
            Hooks::ZKnowledge_SetGameTension->Call(s_Npc->m_pKnowledge, static_cast<EGameTension>(s_Floor));
    }

    // Held a little longer than the partner sends (every 0.2 s), so a lost packet changes nothing.
    CrashGuards::SetAlertFloors(s_Floors, 1000);

    if (m_Trace && (s_Floors.empty() != (m_PeerAlertCount == 0)))
        m_Trace("alerts: " + std::to_string(s_Floors.size()) + " NPCs kept alert for the partner");

    m_PeerAlertCount = s_Floors.size();
}

ZActor* SdkWorld::FindNpc(uint64_t p_Id) const {
    const auto s_It = m_NpcsById.find(p_Id);
    return s_It == m_NpcsById.end() ? nullptr : s_It->second;
}

void SdkWorld::SetNpcTransform(uint64_t p_Id, const coop::Transform& p_Xf) {
    ZActor* s_Actor = FindNpc(p_Id);

    // The partner's victim is placed by their stream, not the host's snapshot; ours by our 47.
    if (!s_Actor || s_Actor == m_RemoteVictim || s_Actor == m_LocalVictim)
        return;

    ZEntityRef s_Ref;
    s_Actor->GetID(s_Ref);

    if (auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>())
        s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Xf));
}

bool SdkWorld::IsLocallyPaused() const {
    return m_InScene && GamePause::MenuOpen();
}

void SdkWorld::SetPartnerPaused(bool p_Paused) {
    m_PartnerWantsPause = p_Paused;
    m_PartnerPauseSince = p_Paused ? m_Now : -1.0;
    m_PauseCapped = false;
    GamePause::SetPartnerPause(WantPartnerHold());

    if (m_Trace)
        m_Trace(std::string("pause: ") + (p_Paused ? "held for the partner" : "released") +
            (GamePause::Available() ? "" : " (pause not found in this game build)"));
}

void SdkWorld::RestartMission() {
    if (!m_InScene)
        return;

    // Not at once: this comes in with the partner's messages, at any moment. Restarting while our
    // own restart or load was under way, or with our pause for the partner still held, crashed the
    // game (players' reports, 0.11.0).
    if (m_RestartAskedAt < 0.0)
        m_RestartAskedAt = m_Now;
}

void SdkWorld::UpdateRestart(bool p_Playing) {
    if (m_RestartAskedAt < 0.0)
        return;

    // Our mission was cleared meanwhile (our own restart, or a load): that one does instead.
    if (!m_InScene) {
        m_RestartAskedAt = -1.0;

        if (m_Trace)
            m_Trace("restart: for the partner, left out (our mission is reloading already)");

        return;
    }

    if (m_Now - m_RestartAskedAt > 60.0) {
        m_RestartAskedAt = -1.0;
        Notify("Your partner restarted the mission: restart yours from the pause menu.");
        return;
    }

    if (!p_Playing || m_Now - m_SceneReadyAt < 2.0)
        return;

    // Their pause menu, which they restarted from, held ours: let go first, restart a frame later.
    if (m_PartnerWantsPause || GamePause::Holding()) {
        m_PartnerWantsPause = false;
        GamePause::SetPartnerPause(false);
        return;
    }

    m_RestartAskedAt = -1.0;
    m_RestartForPartner = true;

    if (!GamePause::Restart()) {
        m_RestartForPartner = false;
        Notify("Your partner restarted the mission: restart yours from the pause menu (this game version can't do it by itself).");
    }

    if (m_Trace)
        m_Trace(std::string("restart: for the partner") + (GamePause::CanRestart() ? "" : " (RestartLevel not found)"));
}

void SdkWorld::SetNpcPuppet(uint64_t p_Id, bool p_Puppet) {
    if (m_Now - m_SceneReadyAt < 3.0) {
        m_PendingPuppets[p_Id] = p_Puppet;
        return;
    }

    m_PendingPuppets.erase(p_Id);

    if (p_Puppet)
        m_SessionPuppets.insert(p_Id);
    else
        m_SessionPuppets.erase(p_Id);

    if (ZActor* s_Actor = FindNpc(p_Id))
        CrashGuards::SetPuppet(s_Actor, p_Puppet);
}

void SdkWorld::ApplyPendingPuppets() {
    if (m_PendingPuppets.empty() || m_Now - m_SceneReadyAt < 3.0)
        return;

    for (const auto& [s_Id, s_Puppet] : m_PendingPuppets) {
        if (s_Puppet)
            m_SessionPuppets.insert(s_Id);
        else
            m_SessionPuppets.erase(s_Id);

        if (ZActor* s_Actor = FindNpc(s_Id))
            CrashGuards::SetPuppet(s_Actor, s_Puppet);
    }

    if (m_Trace)
        m_Trace("shared NPCs: " + std::to_string(CrashGuards::PuppetCount()) + " run by the partner's game (puppets here)");

    m_PendingPuppets.clear();
}

void SdkWorld::ShowNpcPose(uint64_t p_Id, const coop::Pose& p_Pose) {
    ZActor* s_Actor = FindNpc(p_Id);

    // Our own takedown victim moves with our 47.
    if (!s_Actor || s_Actor == m_LocalVictim || s_Actor == m_RemoteVictim)
        return;

    if (void* s_Animator = AnimatorOf(s_Actor)) {
        PoseToBones(p_Pose, m_NpcPoseBones);
        PoseHook::SetPoseFor(s_Animator, m_NpcPoseBones);
    }
}

void SdkWorld::ClearNpcPose(uint64_t p_Id) {
    if (ZActor* s_Actor = FindNpc(p_Id); s_Actor && s_Actor != m_RemoteVictim)
        PoseHook::ClearPoseFor(AnimatorOf(s_Actor));
}

void SdkWorld::WatchNpcPoses(const std::vector<uint64_t>& p_Ids) {
    std::vector<void*> s_Animators;

    for (const uint64_t s_Id : p_Ids)
        if (ZActor* s_Actor = FindNpc(s_Id))
            if (void* s_Animator = AnimatorOf(s_Actor))
                s_Animators.push_back(s_Animator);

    PoseHook::SetWatched(s_Animators);
}

bool SdkWorld::GetNpcPose(uint64_t p_Id, coop::Pose& p_Out) {
    ZActor* s_Actor = FindNpc(p_Id);

    if (!s_Actor || !PoseHook::WatchedPose(AnimatorOf(s_Actor), m_NpcPoseBones) || m_NpcPoseBones.size() < 2)
        return false;

    BonesToPose(m_NpcPoseBones, p_Out);
    return true;
}

void SdkWorld::KillNpc(uint64_t p_Id) {
    ZActor* s_Actor = FindNpc(p_Id);

    if (!s_Actor || !s_Actor->IsAlive())
        return;

    if (m_Trace) {
        char s_Text[96];
        std::snprintf(s_Text, sizeof(s_Text), "kill: actor %p", static_cast<void*>(s_Actor));
        m_Trace(s_Text);
    }

    KillForPartner(s_Actor, EDamageEvent::eDE_UNDEFINED, EDeathBehavior::eDB_IMPACT_ANIM);
}

void SdkWorld::KillForPartner(ZActor* p_Actor, EDamageEvent p_Event, EDeathBehavior p_Behavior) {
    // Its AI back first: the game's own death handling runs as for any NPC.
    CrashGuards::SetPuppet(p_Actor, false);

    // The partner's game already scored this one; ours drops the Kill event it raises (see
    // IsPartnerKillEvent). Without that a partner's knock-out counted as our non-target kill.
    m_PartnerKills.insert(p_Actor->m_sActorName.c_str());

    TEntityRef<IItem> s_Item;
    TEntityRef<ZSetpieceEntity> s_Setpiece;
    Functions::ZActor_KillActor->Call(p_Actor, s_Item, s_Setpiece, p_Event, p_Behavior);
}

bool SdkWorld::IsPartnerKillEvent(const std::string& p_EventJson, bool p_EvenTargets) const {
    // A target's events always count here: the mission's objectives are made of them (the host
    // killing a target kills it here too, and this player has to see it done).
    if (!p_EvenTargets && p_EventJson.find("\"IsTarget\":true") != std::string::npos)
        return false;

    for (const auto& s_Name : m_PartnerKills)
        if (!s_Name.empty() && p_EventJson.find("\"" + s_Name + "\"") != std::string::npos)
            return true;

    return false;
}

namespace {
// Items placed in the scene share their entity id across machines; a few templates are placed
// more than once, and those ids say nothing, so they aren't synced.
template <class F>
void ForEachSceneItem(F&& p_Do) {
    if (!Globals::WorldInventory)
        return;

    std::unordered_map<uint64_t, int> s_Count;

    const auto s_Usable = [](const TEntityRef<IItemBase>& p_Entry) {
        return p_Entry.m_entityRef && p_Entry.m_entityRef.GetEntity() && p_Entry.m_entityRef->GetType();
    };

    for (const auto& s_Entry : Globals::WorldInventory->m_aItems)
        if (s_Usable(s_Entry))
            ++s_Count[s_Entry.m_entityRef->GetType()->m_nEntityID];

    for (const auto& s_Entry : Globals::WorldInventory->m_aItems) {
        if (!s_Usable(s_Entry))
            continue;

        const uint64_t s_Id = s_Entry.m_entityRef->GetType()->m_nEntityID;

        if (s_Count[s_Id] != 1)
            continue;

        if (auto* s_Item = s_Entry.m_entityRef.QueryInterface<ZHM5Item>())
            p_Do(s_Id, s_Item, s_Entry.m_entityRef);
    }
}

// Hidden: invisible and can't be picked up. Shown: both back.
void SetItemShown(ZHM5Item* p_Item, bool p_Shown) {
    if (p_Item->m_rGeomentity.m_entityRef)
        p_Item->m_rGeomentity.m_entityRef.SetProperty("m_bVisible", p_Shown);

    if (p_Item->m_rItemPickupEnabled.m_entityRef)
        p_Item->m_rItemPickupEnabled.m_entityRef.SetProperty("m_bValue", p_Shown);
}
}

ZHM5Item* SdkWorld::FindItem(uint64_t p_Id, ZEntityRef& p_Ref) const {
    ZHM5Item* s_Found = nullptr;

    ForEachSceneItem([&](uint64_t p_ItemId, ZHM5Item* p_Item, const ZEntityRef& p_ItemRef) {
        if (p_ItemId == p_Id) {
            s_Found = p_Item;
            p_Ref = p_ItemRef;
        }
    });

    return s_Found;
}

void SdkWorld::CollectItemChanges(std::vector<coop::ItemChange>& p_Out) {
    if (!m_InScene || m_Now < m_NextItemScan)
        return;

    m_NextItemScan = m_Now + 0.1;
    CollectDoorChanges(p_Out);
    CollectBodyChanges(p_Out);
    const auto s_Player = SDK()->GetLocalPlayer();

    if (!s_Player)
        return;

    ForEachSceneItem([&](uint64_t p_Id, ZHM5Item* p_Item, const ZEntityRef& p_Ref) {
        if (m_PeerHeldItems.contains(p_Id))
            return;

        const bool s_Ours = p_Item->m_pOwner == s_Player.m_entityRef;
        auto s_It = m_Items.find(p_Id);

        if (s_Ours && (s_It == m_Items.end() || !s_It->second.ours)) {
            m_Items[p_Id] = { true, 0.0, {} };
            p_Out.push_back({ p_Id, coop::ItemFlag_Taken, {} });
            return;
        }

        if (s_Ours || s_It == m_Items.end())
            return;

        const auto* s_Spatial = p_Ref.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial)
            return;

        const coop::Transform s_Xf = ToTransform(s_Spatial->GetObjectToWorldMatrix());
        auto& s_Tracked = s_It->second;

        // Let go of (dropped, placed, thrown): back in the world, followed while it moves.
        if (s_Tracked.ours) {
            s_Tracked = { false, m_Now + 4.0, s_Xf };
            p_Out.push_back({ p_Id, 0, s_Xf });
            return;
        }

        if (m_Now > s_Tracked.followUntil) {
            m_Items.erase(s_It);
            return;
        }

        if (coop::Distance(s_Xf.pos, s_Tracked.sent.pos) > 0.02f || coop::AngleBetween(s_Xf.rot, s_Tracked.sent.rot) > 0.02f) {
            s_Tracked.sent = s_Xf;
            s_Tracked.followUntil = std::max(s_Tracked.followUntil, m_Now + 1.0);
            p_Out.push_back({ p_Id, 0, s_Xf });
        }
    });
}

void SdkWorld::ResetItems() {
    if (m_InScene) {
        for (const uint64_t s_Id : m_PeerHeldItems) {
            ZEntityRef s_Ref;

            if (ZHM5Item* s_Item = FindItem(s_Id, s_Ref))
                SetItemShown(s_Item, true);
        }
    }

    m_PeerHeldItems.clear();
    m_Items.clear();
}

void SdkWorld::ApplyPeerItem(const coop::ItemChange& p_Change) {
    if (!m_InScene)
        return;

    if (p_Change.flags & coop::ItemFlag_Door) {
        ApplyPeerDoor(p_Change);
        return;
    }

    if (p_Change.flags & coop::ItemFlag_Body) {
        ApplyPeerBody(p_Change);
        return;
    }

    ZEntityRef s_Ref;
    ZHM5Item* s_Item = FindItem(p_Change.id, s_Ref);

    if (!s_Item)
        return;

    // Both took it at once: ours stays ours.
    if (const auto s_Player = SDK()->GetLocalPlayer(); s_Player && s_Item->m_pOwner == s_Player.m_entityRef)
        return;

    if (p_Change.Taken()) {
        m_PeerHeldItems.insert(p_Change.id);
        SetItemShown(s_Item, false);

        if (m_Trace)
            m_Trace("item: " + std::to_string(p_Change.id) + " taken by the partner");

        return;
    }

    if (m_PeerHeldItems.erase(p_Change.id)) {
        SetItemShown(s_Item, true);

        if (m_Trace)
            m_Trace("item: " + std::to_string(p_Change.id) + " put back by the partner");
    }

    if (auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>())
        s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Change.xf));
}

namespace {
bool ActorTransform(ZActor* p_Actor, coop::Transform& p_Out) {
    ZEntityRef s_Ref;
    p_Actor->GetID(s_Ref);

    if (const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>()) {
        p_Out = SdkWorld::ToTransform(s_Spatial->GetObjectToWorldMatrix());
        return true;
    }

    return false;
}
}

void SdkWorld::AgreeBody(uint64_t p_Id, ZActor* p_Actor) {
    coop::Transform s_Xf;

    if (!p_Actor || !ActorTransform(p_Actor, s_Xf))
        return;

    auto& s_Body = m_Bodies[p_Id];
    s_Body.agreed = s_Body.last = s_Xf.pos;
    s_Body.stillSince = m_Now;
}

void SdkWorld::CollectBodyChanges(std::vector<coop::ItemChange>& p_Out) {
    if (m_NpcsById.empty())
        return;

    for (const auto& [s_Id, s_Actor] : m_NpcsById) {
        coop::Transform s_Xf;

        if (!s_Actor || !ActorDown(s_Actor) || !ActorTransform(s_Actor, s_Xf))
            continue;

        const auto [s_It, s_New] = m_Bodies.try_emplace(s_Id);
        auto& s_Body = s_It->second;

        if (s_New) {
            s_Body.downAt = m_Now;
            s_Body.hidden = s_Actor->m_bBodyHidden;
        }

        // Where it fell, in each game its own way (a fall takes a few seconds): nothing to tell. The
        // partner's victim moves with their stream.
        if (m_Now - s_Body.downAt < 4.0 || s_Actor == m_RemoteVictim) {
            s_Body.agreed = s_Body.last = s_Xf.pos;
            s_Body.stillSince = m_Now;
            continue;
        }

        if (coop::Distance(s_Xf.pos, s_Body.last) > 0.05f) {
            s_Body.last = s_Xf.pos;
            s_Body.stillSince = m_Now;
        }

        if (s_Actor->m_bIsBeingDragged || s_Actor->m_bIsBeingDumped)
            continue;

        const bool s_Hidden = s_Actor->m_bBodyHidden;
        // More than a nudge (walking into a body shifts it a little), and at rest for a moment.
        const bool s_Moved = coop::Distance(s_Xf.pos, s_Body.agreed) > 1.5f && m_Now - s_Body.stillSince > 0.6;

        if (s_Hidden == s_Body.hidden && !s_Moved)
            continue;

        s_Body.hidden = s_Hidden;
        s_Body.agreed = s_Xf.pos;
        p_Out.push_back({ s_Id, static_cast<uint8_t>(coop::ItemFlag_Body | (s_Hidden ? coop::ItemFlag_BodyHidden : 0)), s_Xf });

        if (m_Trace)
            m_Trace(std::string("body: ") + s_Actor->m_sActorName.c_str() + (s_Hidden ? " put away" : " moved") + ", told to the partner");
    }

    // A body the game took out of the mission (disabled) after it was put away somewhere.
    for (auto& [s_Id, s_Body] : m_Bodies) {
        if (s_Body.gone || s_Body.hidden || m_NpcsById.contains(s_Id))
            continue;

        for (const auto& s_Entry : Globals::ActorManager->m_activatedActors) {
            if (s_Entry.m_pInterfaceRef && !s_Entry.m_pInterfaceRef->m_bEnabled && s_Entry.m_entityRef &&
                s_Entry.m_entityRef->GetType() && s_Entry.m_entityRef->GetType()->m_nEntityID == s_Id) {
                s_Body.gone = s_Body.hidden = true;
                coop::Transform s_Where;
                s_Where.pos = s_Body.agreed;
                p_Out.push_back({ s_Id, static_cast<uint8_t>(coop::ItemFlag_Body | coop::ItemFlag_BodyHidden), s_Where });
                break;
            }
        }
    }
}

void SdkWorld::ApplyPeerBody(const coop::ItemChange& p_Change) {
    ZActor* s_Actor = FindNpc(p_Change.id);

    // In our 47's hands right now, or not a body here (yet).
    if (!s_Actor || s_Actor == m_LocalVictim || !ActorDown(s_Actor) || s_Actor->m_bIsBeingDragged || s_Actor->m_bIsBeingDumped)
        return;

    // At rest there: the partner has let go of it.
    if (s_Actor == m_RemoteVictim)
        ReleaseRemoteVictim();

    // Hidden with the game's own flag for a body in a closet or a container, so no NPC here finds
    // it, and out of sight.
    ZEntityRef s_Ref;
    s_Actor->GetID(s_Ref);
    s_Actor->m_bBodyHidden = true;
    s_Ref.SetProperty("m_bVisible", false);

    AgreeBody(p_Change.id, s_Actor);
    auto& s_Body = m_Bodies[p_Change.id];
    s_Body.hidden = true;

    if (m_Trace)
        m_Trace(std::string("body: ") + s_Actor->m_sActorName.c_str() +
            ((p_Change.flags & coop::ItemFlag_BodyHidden) ? " put away" : " moved") + " by the partner, hidden here");
}

std::string SdkWorld::BodyTest(const std::string& p_Arg) {
    // body | body move | body hide: the nearest body, and the partner's word about it.
    coop::Transform s_Local;

    if (!GetLocalAvatar(s_Local))
        return "body: no 47";

    ZActor* s_Best = nullptr;
    uint64_t s_BestId = 0;
    float s_BestD = 30.f;
    coop::Transform s_BestXf;

    for (const auto& [s_Id, s_Actor] : m_NpcsById) {
        coop::Transform s_Xf;

        if (s_Actor && ActorDown(s_Actor) && ActorTransform(s_Actor, s_Xf))
            if (const float s_D = coop::Distance(s_Xf.pos, s_Local.pos); s_D < s_BestD) {
                s_Best = s_Actor;
                s_BestId = s_Id;
                s_BestD = s_D;
                s_BestXf = s_Xf;
            }
    }

    if (!s_Best)
        return "body: none within 30 m";

    char s_Text[256];
    std::snprintf(s_Text, sizeof(s_Text), "%s at (%.2f, %.2f, %.2f), %.1f m: dead=%d dragged=%d dumped=%d hidden=%d", s_Best->m_sActorName.c_str(),
        s_BestXf.pos.x, s_BestXf.pos.y, s_BestXf.pos.z, s_BestD, s_Best->IsDead() ? 1 : 0, s_Best->m_bIsBeingDragged ? 1 : 0,
        s_Best->m_bIsBeingDumped ? 1 : 0, s_Best->m_bBodyHidden ? 1 : 0);

    // As the partner's word: it moved (or was put away) there.
    if (p_Arg == "move" || p_Arg == "hide")
        ApplyPeerBody({ s_BestId, static_cast<uint8_t>(coop::ItemFlag_Body | (p_Arg == "hide" ? coop::ItemFlag_BodyHidden : 0)), s_BestXf });

    return std::string("body: ") + s_Text;
}

namespace {
// Our 47's item in hand: of the items it owns, the one being drawn. Items in its pockets are owned
// but not drawn (their geometry hidden).
ZHM5Item* HeldBy(const ZEntityRef& p_Owner, ZEntityRef* p_Ref = nullptr) {
    ZHM5Item* s_Held = nullptr;

    ForEachSceneItem([&](uint64_t, ZHM5Item* p_Item, const ZEntityRef& p_ItemRef) {
        if (s_Held || p_Item->m_pOwner != p_Owner || !p_Item->m_rGeomentity.m_entityRef || !p_Item->m_pItemConfigDescriptor)
            return;

        if (p_Item->m_rGeomentity.m_entityRef.GetProperty<bool>("m_bVisible").Get()) {
            s_Held = p_Item;

            if (p_Ref)
                *p_Ref = p_ItemRef;
        }
    });

    return s_Held;
}
}

std::string SdkWorld::LocalHeldItem() const {
    // A few times a second: it walks the level's items.
    if (m_Now < m_HeldCheckAt)
        return m_HeldItem;

    m_HeldCheckAt = m_Now + 0.25;
    m_HeldItem.clear();
    const auto s_Player = SDK()->GetLocalPlayer();

    if (m_InScene && s_Player)
        if (const ZHM5Item* s_Item = HeldBy(s_Player.m_entityRef))
            m_HeldItem = s_Item->m_pItemConfigDescriptor->m_ItemID.ToString().c_str();

    return m_HeldItem;
}

std::string SdkWorld::HeldItemsTest() const {
    coop::Transform s_Local;
    const auto s_Player = SDK()->GetLocalPlayer();

    if (!GetLocalAvatar(s_Local) || !s_Player)
        return "held: no 47";

    std::string s_Out;
    int s_Count = 0;

    ForEachSceneItem([&](uint64_t p_Id, ZHM5Item* p_Item, const ZEntityRef& p_Ref) {
        if (p_Item->m_pOwner != s_Player.m_entityRef)
            return;

        const auto* s_Spatial = p_Ref.QueryInterface<ZSpatialEntity>();
        const coop::Transform s_Xf = s_Spatial ? ToTransform(s_Spatial->GetObjectToWorldMatrix()) : coop::Transform {};
        const bool s_Geom = p_Item->m_rGeomentity.m_entityRef && p_Item->m_rGeomentity.m_entityRef.GetProperty<bool>("m_bVisible").Get();
        const bool s_Visible = p_Item->m_rItemVisible.m_entityRef && p_Item->m_rItemVisible.m_entityRef.GetProperty<bool>("m_bValue").Get();
        char s_Line[320];
        std::snprintf(s_Line, sizeof(s_Line), " | %s [%s] geom=%d visible=%d at %.2f m (dz %.2f)",
            p_Item->m_pItemConfigDescriptor ? p_Item->m_pItemConfigDescriptor->m_sTitle.c_str() : "?",
            p_Item->m_pItemConfigDescriptor ? p_Item->m_pItemConfigDescriptor->m_ItemID.ToString().c_str() : "?", s_Geom ? 1 : 0,
            s_Visible ? 1 : 0, coop::Distance(s_Xf.pos, s_Local.pos), s_Xf.pos.z - s_Local.pos.z);
        s_Out += s_Line;
        ++s_Count;
    });

    return "held: " + std::to_string(s_Count) + " owned, in hand [" + LocalHeldItem() + "]" + s_Out;
}

void SdkWorld::SetPartnerHeldItem(const std::string& p_RepositoryId) {
    m_StandInItemWanted = p_RepositoryId;
}

void SdkWorld::DropStandInItem() {
    if (m_StandInItem && m_PartnerActor) {
        m_PartnerActor->ReleaseItem(m_StandInItem, false);
        Functions::ZWorldInventory_DestroyItem->Call(Globals::WorldInventory, TEntityRef<IItemBase>(m_StandInItem.m_entityRef));

        if (m_Trace)
            m_Trace("held item: " + m_StandInItemShown + " taken from the stand-in's hand");
    }

    m_StandInItem = {};
    m_StandInItemShown.clear();
}

void SdkWorld::UpdateStandInItem() {
    if (!m_InScene || !m_PartnerActor || !m_PartnerActor->m_pInventoryHandler || !Globals::WorldInventory)
        return;

    // Hidden (out of the mission, or far off): nothing in its hands to give away.
    const std::string s_Want = m_PartnerHidden ? std::string() : m_StandInItemWanted;

    // Asked for already: the copy is on its way (a few frames while its resource loads).
    if (m_StandInItemTicket && m_Now - m_StandInItemAskedAt < 5.0)
        return;

    m_StandInItemTicket = 0;

    if (s_Want == m_StandInItemShown)
        return;

    DropStandInItem();

    // An item the game can't make here (not in this mission's resources) is tried again later.
    if (s_Want.empty() || m_Now - m_StandInItemFailedAt < 10.0)
        return;

    ZEntityRef s_ActorRef = m_PartnerActor->m_pInventoryHandler->m_rActor.m_entityRef;

    if (!s_ActorRef)
        return;

    const uint64_t s_NewId = Functions::ZEntityManager_GenerateDynamicObjectID->Call(Globals::EntityManager, s_ActorRef,
        EDynamicEntityType::eDET_CharacterInventoryItem, 0);
    const ZMemberDelegate<SdkWorld, void(uint32_t, TEntityRef<IItemBase>)> s_Delegate(this, &SdkWorld::OnStandInItemCreated);
    const uint32_t s_Ticket = Functions::ZWorldInventory_RequestNewItem->Call(Globals::WorldInventory, ZRepositoryID(ZString(s_Want)),
        s_Delegate, s_NewId, false, {}, s_ActorRef);

    if (s_Ticket == *Globals::WorldInventory_InvalidTicket) {
        m_StandInItemFailedAt = m_Now;

        if (m_Trace)
            m_Trace("held item: " + s_Want + " can't be made here");

        return;
    }

    ZActorInventoryHandler::SPendingItemInfo s_Pending;
    s_Pending.m_nTicket = s_Ticket;
    s_Pending.m_eAttachLocation = static_cast<EAttachLocation>(m_StandInAttach);
    s_Pending.m_eMaxTension = EGameTension::EGT_Undefined;
    s_Pending.m_bLeftHand = false;
    s_Pending.m_bWeapon = m_StandInWeapon;
    m_PartnerActor->m_pInventoryHandler->m_aPendingItems.push_back(s_Pending);

    m_StandInItemTicket = s_Ticket;
    m_StandInItemAskedAt = m_Now;
    m_StandInItemShown = s_Want;

    if (m_Trace)
        m_Trace("held item: asked for " + s_Want);
}

void SdkWorld::OnStandInItemCreated(uint32_t p_Ticket, TEntityRef<IItemBase> p_Item) {
    if (!m_PartnerActor || !m_PartnerActor->m_pInventoryHandler || p_Ticket != m_StandInItemTicket)
        return;

    for (auto& s_Pending : m_PartnerActor->m_pInventoryHandler->m_aPendingItems) {
        if (s_Pending.m_nTicket == p_Ticket) {
            s_Pending.m_rItem = TEntityRef<IItem>(p_Item.m_entityRef);
            m_StandInItem = s_Pending.m_rItem;
            break;
        }
    }

    Functions::ZActorInventoryHandler_FinalizePendingItems->Call(m_PartnerActor->m_pInventoryHandler);
    m_StandInItemTicket = 0;

    // Never picked up from its hands, by our 47 or anyone.
    if (auto* s_Item = p_Item.m_entityRef.QueryInterface<ZHM5Item>())
        if (s_Item->m_rItemPickupEnabled.m_entityRef)
            s_Item->m_rItemPickupEnabled.m_entityRef.SetProperty("m_bValue", false);

    if (m_Trace)
        m_Trace("held item: " + m_StandInItemShown + " in the stand-in's hand");
}

std::string SdkWorld::StandInItemTest(const std::string& p_Arg) {
    // standinitem <attach> <weapon 0/1> <repository id | mine | off>
    std::istringstream s_Args(p_Arg);
    std::string s_What;
    int s_Weapon = 0;
    s_Args >> m_StandInAttach >> s_Weapon >> s_What;
    m_StandInWeapon = s_Weapon != 0;

    if (s_What == "mine") {
        const auto s_Player = SDK()->GetLocalPlayer();
        s_What.clear();

        ForEachSceneItem([&](uint64_t, ZHM5Item* p_Item, const ZEntityRef&) {
            if (s_What.empty() && s_Player && p_Item->m_pOwner == s_Player.m_entityRef && p_Item->m_pItemConfigDescriptor)
                s_What = p_Item->m_pItemConfigDescriptor->m_ItemID.ToString().c_str();
        });
    }

    if (s_What == "off")
        s_What.clear();

    DropStandInItem();
    m_StandInItemTicket = 0;
    m_StandInItemFailedAt = -100.0;
    SetPartnerHeldItem(s_What);
    return "standinitem: attach " + std::to_string(m_StandInAttach) + " weapon " + std::to_string(s_Weapon) + " [" + s_What + "]";
}

void SdkWorld::ForEachSceneEntity(const std::function<void(uint64_t, const ZEntityRef&)>& p_Do) const {
    if (!Globals::Hitman5Module || !Globals::Hitman5Module->m_pEntitySceneContext)
        return;

    // Each entity's key is the chain of sub-entity ids down from its brick: the same on both
    // machines for the same scene, and unique, where an entity's own id repeats per template copy.
    std::function<void(ZEntityType**, ZEntityBlueprintFactoryBase*, uint64_t, int)> s_Walk =
        [&](ZEntityType** p_Entity, ZEntityBlueprintFactoryBase* p_Blueprint, uint64_t p_Key, int p_Depth) {
            if (!p_Entity || !p_Blueprint || p_Depth > 16)
                return;

            const int64_t s_Count = p_Blueprint->GetSubEntitiesCount();

            for (int i = 0; i < s_Count; ++i) {
                ZEntityType** s_Sub = p_Blueprint->GetSubEntity(p_Entity, i);

                if (!s_Sub)
                    continue;

                const uint64_t s_Key = (p_Key ^ p_Blueprint->GetSubEntityId(i)) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
                ZEntityRef s_Ref;
                s_Ref.m_pObj = s_Sub;
                p_Do(s_Key, s_Ref);
                s_Walk(s_Sub, p_Blueprint->GetSubEntityBlueprint(i), s_Key, p_Depth + 1);
            }
        };

    for (const auto& s_Brick : Globals::Hitman5Module->m_pEntitySceneContext->m_aLoadedBricks) {
        const uint64_t s_BrickKey = (static_cast<uint64_t>(s_Brick.m_RuntimeResourceID.GetID()) * 0x9E3779B97F4A7C15ull) | 1;
        s_Walk(s_Brick.m_EntityRef.m_pObj, s_Brick.m_EntityRef.GetBlueprintFactory(), s_BrickKey, 0);
    }
}

namespace {
// A property's address in an entity, found by name in the running game's reflection data (so it
// holds across game versions). Only for plain fields (no getter).
const uint8_t* PropertyAddress(const ZEntityRef& p_Ref, const char* p_Name) {
    const auto* s_Entity = p_Ref.GetEntity();
    const auto* s_Type = s_Entity ? s_Entity->GetType() : nullptr;

    if (!s_Type || !s_Type->m_pPropertyData)
        return nullptr;

    const uint32_t s_Id = Hash::Crc32(p_Name, std::strlen(p_Name));

    for (uint32_t i = 0; i < s_Type->m_pPropertyData->size(); ++i) {
        const SPropertyData& s_Property = (*s_Type->m_pPropertyData)[i];

        if (s_Property.m_nPropertyID != s_Id)
            continue;

        const auto* s_Info = s_Property.GetPropertyInfo();

        if (!s_Info || (s_Info->m_propertyInfo.m_Flags & EPropertyInfoFlags::E_HAS_GETTER_SETTER))
            return nullptr;

        return reinterpret_cast<const uint8_t*>(p_Ref.m_pObj) + s_Property.m_nPropertyOffset;
    }

    return nullptr;
}

// A door's m_rDoorOpen (a TEntityRef<ZValueBool>), read through to its m_bValue.
bool ReadDoorOpen(const ZEntityRef& p_Door, bool& p_Open) {
    const auto* s_RefAddress = PropertyAddress(p_Door, "m_rDoorOpen");

    if (!s_RefAddress)
        return false;

    const ZEntityRef& s_Value = *reinterpret_cast<const ZEntityRef*>(s_RefAddress);

    if (!s_Value.GetEntity())
        return false;

    const auto* s_Bool = PropertyAddress(s_Value, "m_bValue");

    if (!s_Bool)
        return false;

    p_Open = *s_Bool != 0;
    return true;
}
}

void SdkWorld::FindDoors() {
    m_Doors.clear();
    m_DoorsFound = true;

    if (!*Globals::TypeRegistry)
        return;

    const STypeID* s_Single = (*Globals::TypeRegistry)->GetTypeID(ZString("ZHM5SingleDoor2"));
    const STypeID* s_Double = (*Globals::TypeRegistry)->GetTypeID(ZString("ZHM5DoubleDoor2"));

    if (!s_Single && !s_Double)
        return;

    LARGE_INTEGER s_Start, s_End, s_Freq;
    QueryPerformanceCounter(&s_Start);

    ForEachSceneEntity([&](uint64_t p_Key, const ZEntityRef& p_Ref) {
        if ((s_Single && p_Ref.QueryInterface(s_Single)) || (s_Double && p_Ref.QueryInterface(s_Double)))
            m_Doors[p_Key] = { p_Ref, false, false };
    });

    for (auto& [s_Key, s_Door] : m_Doors)
        s_Door.known = ReadDoorOpen(s_Door.ref, s_Door.open);

    QueryPerformanceCounter(&s_End);
    QueryPerformanceFrequency(&s_Freq);

    if (m_Trace)
        m_Trace("doors: " + std::to_string(m_Doors.size()) + " found in " +
            std::to_string(static_cast<int>((s_End.QuadPart - s_Start.QuadPart) * 1000 / s_Freq.QuadPart)) + " ms");
}

void SdkWorld::CollectDoorChanges(std::vector<coop::ItemChange>& p_Out) {
    // Found once per mission, a few seconds in (2 million entities to look through in Paris).
    if (!m_DoorsFound) {
        if (m_DoorsSearchAt == 0.0)
            m_DoorsSearchAt = m_Now + 5.0;

        if (m_Now < m_DoorsSearchAt)
            return;

        FindDoors();
    }

    coop::Transform s_Local;

    if (m_Doors.empty() || !GetLocalAvatar(s_Local))
        return;

    for (auto& [s_Key, s_Door] : m_Doors) {
        const auto* s_Spatial = s_Door.ref.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial)
            continue;

        const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();
        const coop::Vec3 s_Pos { s_M.Trans.x, s_M.Trans.y, s_M.Trans.z };
        const float s_Distance = coop::Distance(s_Pos, s_Local.pos);

        if (s_Distance > 6.f)
            continue;

        bool s_Open = false;

        if (!ReadDoorOpen(s_Door.ref, s_Open))
            continue;

        // Only doors our 47 is right at: the ones it opened or closed. (NPCs open doors in both
        // games on their own.)
        if (s_Door.known && s_Open != s_Door.open && s_Distance < 2.5f) {
            const float s_Side = s_M.YAxis.x * (s_Local.pos.x - s_Pos.x) + s_M.YAxis.y * (s_Local.pos.y - s_Pos.y);
            const uint8_t s_Flags = coop::ItemFlag_Door | (s_Open ? coop::ItemFlag_DoorOpen : 0) |
                (s_Side > 0.f ? coop::ItemFlag_DoorFromFront : 0);
            p_Out.push_back({ s_Key, s_Flags, {} });
        }

        s_Door.open = s_Open;
        s_Door.known = true;
    }
}

void SdkWorld::ApplyPeerDoor(const coop::ItemChange& p_Change) {
    if (!m_DoorsFound)
        FindDoors();

    const auto s_It = m_Doors.find(p_Change.id);

    if (s_It == m_Doors.end())
        return;

    auto& s_Door = s_It->second;
    const bool s_Open = (p_Change.flags & coop::ItemFlag_DoorOpen) != 0;
    bool s_Now = false;

    if (ReadDoorOpen(s_Door.ref, s_Now) && s_Now == s_Open)
        return;

    // Swung away from where the partner stood, as theirs did.
    if (!s_Open)
        s_Door.ref.SignalInputPin(ZString("CloseDoor"));
    else
        s_Door.ref.SignalInputPin(ZString(std::string_view((p_Change.flags & coop::ItemFlag_DoorFromFront) ? "OpenDoorIn" : "OpenDoorOut")));

    // Taken as known, so it isn't sent back.
    s_Door.open = s_Open;
    s_Door.known = true;

    if (m_Trace)
        m_Trace(std::string("door: ") + (s_Open ? "opened" : "closed") + " by the partner");
}

std::string SdkWorld::DoorToggleTest() {
    if (!m_DoorsFound)
        FindDoors();

    coop::Transform s_Local;

    if (!GetLocalAvatar(s_Local))
        return "door: no 47";

    uint64_t s_Best = 0;
    float s_BestDistance = 40.f;

    for (const auto& [s_Key, s_Door] : m_Doors) {
        if (const auto* s_Spatial = s_Door.ref.QueryInterface<ZSpatialEntity>()) {
            const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();

            if (const float s_D = coop::Distance({ s_M.Trans.x, s_M.Trans.y, s_M.Trans.z }, s_Local.pos); s_D < s_BestDistance) {
                s_BestDistance = s_D;
                s_Best = s_Key;
            }
        }
    }

    if (!s_Best)
        return "door: none within 40 m of " + std::to_string(m_Doors.size());

    bool s_Open = false;
    const bool s_Read = ReadDoorOpen(m_Doors[s_Best].ref, s_Open);
    ApplyPeerDoor({ s_Best, static_cast<uint8_t>(coop::ItemFlag_Door | (s_Open ? 0 : coop::ItemFlag_DoorOpen)), {} });
    return "door: " + std::to_string(s_BestDistance) + " m away, was " + (s_Read ? (s_Open ? "open" : "closed") : "unreadable") + ", toggled";
}

std::string SdkWorld::GoToDoorTest() {
    if (!m_DoorsFound)
        FindDoors();

    coop::Transform s_Local;
    auto s_Player = SDK()->GetLocalPlayer();

    if (!GetLocalAvatar(s_Local) || !s_Player)
        return "door: no 47";

    float s_Best = 1e9f;
    SMatrix s_Door;

    for (const auto& [s_Key, s_Entry] : m_Doors) {
        if (const auto* s_Spatial = s_Entry.ref.QueryInterface<ZSpatialEntity>()) {
            const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();

            if (const float s_D = coop::Distance({ s_M.Trans.x, s_M.Trans.y, s_M.Trans.z }, s_Local.pos); s_D < s_Best) {
                s_Best = s_D;
                s_Door = s_M;
            }
        }
    }

    if (s_Best > 1e8f)
        return "door: none";

    // 1.5 m in front of it, facing it.
    SMatrix s_To = s_Door;
    s_To.Trans.x += s_Door.YAxis.x * 1.5f;
    s_To.Trans.y += s_Door.YAxis.y * 1.5f;

    if (auto* s_Spatial = s_Player.m_entityRef.QueryInterface<ZSpatialEntity>())
        s_Spatial->SetObjectToWorldMatrixFromEditor(s_To);

    return "door: went to one " + std::to_string(s_Best) + " m away";
}

std::string SdkWorld::DoorState() {
    std::string s_Out = "door states near:";
    coop::Transform s_Local;

    if (!GetLocalAvatar(s_Local))
        return s_Out;

    for (const auto& [s_Key, s_Door] : m_Doors) {
        if (const auto* s_Spatial = s_Door.ref.QueryInterface<ZSpatialEntity>()) {
            const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();

            if (coop::Distance({ s_M.Trans.x, s_M.Trans.y, s_M.Trans.z }, s_Local.pos) < 40.f) {
                bool s_Open = false;
                s_Out += ReadDoorOpen(s_Door.ref, s_Open) ? (s_Open ? " open" : " closed") : " ?";
            }
        }
    }

    return s_Out;
}

std::string SdkWorld::RigScanTest() {
    std::string s_Out;
    std::vector<int32_t> s_Tables[2];
    void* s_Animators[2] = { nullptr, AnimatorOf(m_PartnerActor) };

    if (auto s_Player = SDK()->GetLocalPlayer())
        s_Animators[0] = s_Player.m_pInterfaceRef->m_pMorphemeEntity.m_entityRef.QueryInterface<IBoneAnimator>();

    for (int a = 0; a < 2; ++a) {
        s_Out += a == 0 ? "rig 47:" : " | rig stand-in:";

        if (!s_Animators[a]) {
            s_Out += " no animator";
            continue;
        }

        const auto s_Found = RigScan::FindParents(s_Animators[a], 231);
        s_Out += " " + std::to_string(s_Found.size()) + " found";

        for (const auto& s_Hit : s_Found) {
            s_Out += " [" + s_Hit.path + " w" + std::to_string(s_Hit.width) + ":";

            for (size_t i = 0; i < s_Hit.parents.size(); ++i)
                s_Out += " " + std::to_string(s_Hit.parents[i]);

            s_Out += "]";
        }

        if (!s_Found.empty())
            s_Tables[a] = s_Found[0].parents;
    }

    if (!s_Tables[0].empty() && !s_Tables[1].empty())
        s_Out += std::string(" | same parents: ") + (s_Tables[0] == s_Tables[1] ? "yes" : "no");

    return s_Out;
}

std::string SdkWorld::DoorTest() {
    const STypeID* s_DoorType = (*Globals::TypeRegistry)->GetTypeID(ZString("IHM5Door"));
    size_t s_Entities = 0, s_Doors = 0;
    std::map<std::string, int> s_Classes;

    ForEachSceneEntity([&](uint64_t, const ZEntityRef& p_Ref) {
        ++s_Entities;

        if (s_DoorType && p_Ref.QueryInterface(s_DoorType)) {
            ++s_Doors;
            const auto* s_Type = p_Ref.GetEntity() ? p_Ref.GetEntity()->GetType() : nullptr;

            if (s_Type && s_Type->m_pInterfaceData && s_Type->m_pInterfaceData->size() > 0) {
                const STypeID* s_First = (*s_Type->m_pInterfaceData)[0].m_Type;
                const IType* s_Info = s_First ? s_First->GetTypeInfo() : nullptr;
                ++s_Classes[s_Info && s_Info->pszTypeName ? s_Info->pszTypeName : "?"];
            }
        }
    });

    std::string s_Out = "doors: " + std::to_string(s_Doors) + " of " + std::to_string(s_Entities) + " entities (IHM5Door " +
        (s_DoorType ? "known" : "unknown") + ")";

    for (const auto& [s_Name, s_N] : s_Classes)
        s_Out += " | " + s_Name + " x" + std::to_string(s_N);

    return s_Out;
}

std::string SdkWorld::ItemTest(bool p_Take) {
    coop::Transform s_Local;

    if (!GetLocalAvatar(s_Local))
        return "item: no 47";

    if (!p_Take) {
        if (!m_ItemTestId)
            return "item: none taken";

        ApplyPeerItem({ m_ItemTestId, 0, m_ItemTestXf });
        return "item: " + std::to_string(m_ItemTestId) + " put back";
    }

    float s_Best = 20.f;
    uint64_t s_BestId = 0;
    std::string s_Name;

    ForEachSceneItem([&](uint64_t p_Id, ZHM5Item* p_Item, const ZEntityRef& p_Ref) {
        const auto* s_Spatial = p_Ref.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial || p_Item->m_pOwner)
            return;

        const coop::Transform s_Xf = ToTransform(s_Spatial->GetObjectToWorldMatrix());

        if (const float s_D = coop::Distance(s_Xf.pos, s_Local.pos); s_D < s_Best) {
            s_Best = s_D;
            s_BestId = p_Id;
            m_ItemTestXf = s_Xf;
            s_Name = p_Item->m_pItemConfigDescriptor ? p_Item->m_pItemConfigDescriptor->m_sTitle.c_str() : "?";
        }
    });

    if (!s_BestId)
        return "item: none within 20 m";

    m_ItemTestId = s_BestId;
    ApplyPeerItem({ s_BestId, coop::ItemFlag_Taken, {} });
    return "item: " + s_Name + " (" + std::to_string(s_BestId) + ", " + std::to_string(s_Best) + " m) taken";
}

void SdkWorld::OnPeerMissionEvent(const std::string& p_Name, const std::string& p_Json) {
    // What happens to the partner's 47 in their game, in a few words (see HitmanCoop's AwarenessOf).
    static const std::map<std::string, std::string> c_Aware = {
        { "spotted", " was spotted." }, { "disguise_blown", "'s disguise was blown!" }, { "trespassing", " is trespassing." },
        { "caught_trespassing", " was caught trespassing." }, { "body_found", ": a body was found near them." },
        { "combat", " is in combat!" }, { "hunted", " is being hunted." }, { "arrest", " is being arrested!" },
    };

    if (m_EventHook && m_EventHook(p_Name, p_Json))
        return;

    if (p_Name == "Coop_Aware") {
        if (const auto s_It = c_Aware.find(p_Json); s_It != c_Aware.end() && m_ToastAlerts)
            NotifyAs(p_Json == "spotted" || p_Json == "trespassing" ? ToastKind::Partner : ToastKind::Warning, m_PartnerName + s_It->second);
    }
    else if (p_Name == "Coop_Kill") {
        // The partner's kill feed: who went down, and how.
        if (coop::GameEvents::Takedown s_Down; m_ToastKillFeed && coop::GameEvents::DecodeTakedown(p_Json, s_Down))
            NotifyAs(ToastKind::Partner, coop::GameEvents::DescribeTakedown(m_PartnerName, s_Down));
    }
    else if (p_Name == "Coop_Challenge" && m_ToastChallenges)
        NotifyAs(ToastKind::Partner, m_PartnerName + " completed a challenge: " + coop::GameEvents::PlainText(p_Json, 60) + ".");
    else if (p_Name == "Coop_Ping") {
        Pulse();
        NotifyAs(ToastKind::Partner, m_PartnerName + " is looking for you: follow the arrow.");
    }
    else if (p_Name == "Coop_Regroup")
        NotifyAs(ToastKind::Partner, m_PartnerName + " regrouped with you.");
    else if (p_Name == "ContractEnd")
        NotifyAs(ToastKind::Partner, m_PartnerName + " completed the mission.");
    else if (p_Name == "ContractFailed")
        NotifyAs(ToastKind::Warning, m_PartnerName + "'s mission failed.");
    else
        Logger::Debug("[HitmanCoop] Partner event {}: {}", p_Name, p_Json);
}

void SdkWorld::Notify(const std::string& p_Text) {
    // The session's own words: about the partner unless it's about trouble.
    const bool s_Trouble = p_Text.find("Lost") != std::string::npos || p_Text.find("timed out") != std::string::npos ||
        p_Text.find("lost connection") != std::string::npos || p_Text.find("struggling") != std::string::npos ||
        p_Text.find("refused") != std::string::npos || p_Text.find("Could not") != std::string::npos ||
        p_Text.find("Can't") != std::string::npos || p_Text.find("differ") != std::string::npos;
    NotifyAs(s_Trouble ? ToastKind::Warning : ToastKind::Info, p_Text);
}

void SdkWorld::NotifyAs(ToastKind p_Kind, const std::string& p_Text) {
    Logger::Info("[HitmanCoop] {}", p_Text);

    if (m_Trace)
        m_Trace("notify: " + p_Text);

    // A soft sound for what the partner says or warns about (not for our own doings), at most
    // one every 1.5 s. MessageBeep plays it without waiting.
    static double s_LastSound = -100.0;

    if (m_Sounds && p_Kind != ToastKind::Info && m_Now - s_LastSound > 1.5) {
        s_LastSound = m_Now;
        MessageBeep(p_Kind == ToastKind::Warning ? MB_ICONEXCLAMATION : p_Kind == ToastKind::Chat ? MB_ICONASTERISK : MB_OK);
    }

    std::lock_guard s_Lock(m_ToastMutex);

    if (!m_Toasts.empty() && m_Toasts.back().text == p_Text && m_Now - m_Toasts.back().time < 6.0) {
        m_Toasts.back().time = m_Now;
        ++m_Toasts.back().count;

        if (!m_History.empty())
            m_History.back().count = m_Toasts.back().count;

        return;
    }

    m_Toasts.push_back({ p_Text, m_Now, p_Kind, 1 });
    m_History.push_back(m_Toasts.back());

    while (m_Toasts.size() > c_MaxToasts)
        m_Toasts.pop_front();

    while (m_History.size() > c_MaxHistory)
        m_History.pop_front();
}

std::deque<SdkWorld::Toast> SdkWorld::CopyToasts() const {
    std::lock_guard s_Lock(m_ToastMutex);
    return m_Toasts;
}

std::deque<SdkWorld::Toast> SdkWorld::CopyHistory() const {
    std::lock_guard s_Lock(m_ToastMutex);
    return m_History;
}

void SdkWorld::AddChat(const std::string& p_Text, bool p_Ours, bool p_Quick) {
    {
        std::lock_guard s_Lock(m_ToastMutex);
        m_Chat.push_back({ p_Text, p_Ours, p_Quick, m_Now });
        m_PartnerChatCount += p_Ours ? 0 : 1;

        while (m_Chat.size() > 40)
            m_Chat.pop_front();
    }

    if (!p_Ours && m_ToastChat)
        NotifyAs(ToastKind::Chat, m_PartnerName + ": " + p_Text);
}

void SdkWorld::ClearChat() {
    std::lock_guard s_Lock(m_ToastMutex);
    m_Chat.clear();
}

size_t SdkWorld::PartnerChatCount() const {
    std::lock_guard s_Lock(m_ToastMutex);
    return m_PartnerChatCount;
}

std::deque<SdkWorld::ChatLine> SdkWorld::CopyChat() const {
    std::lock_guard s_Lock(m_ToastMutex);
    return m_Chat;
}

void SdkWorld::OnPeerChat(const std::string& p_Text, bool p_Quick) {
    // Printable text only: it is drawn as is.
    std::string s_Text;

    for (const char c : p_Text)
        if (static_cast<unsigned char>(c) >= 0x20)
            s_Text += c;

    if (!s_Text.empty())
        AddChat(s_Text, false, p_Quick);
}

void SdkWorld::KnockOutNpc(uint64_t p_Id) {
    ZActor* s_Actor = FindNpc(p_Id);

    // Taken down here already (our own victim, or the partner's streamed one).
    if (!s_Actor || ActorDown(s_Actor) || s_Actor == m_LocalVictim)
        return;

    PutDown(s_Actor, p_Id, coop::NpcFlag_Alive);
}

std::string SdkWorld::NpcLabel(const ZActor* p_Actor) {
    if (!p_Actor)
        return "a spot";

    const auto* s_Kit = p_Actor->m_rOutfit.m_pInterfaceRef;
    const char* s_Kind = p_Actor->m_bContractTarget ? "target" : s_Kit && s_Kit->m_eActorType == EActorType::eAT_Guard ? "guard" : "civilian";

    // The overlay's font has plain letters only: accented ones are written without their accents,
    // "Andre Beaule".
    const std::string s_Name = coop::GameEvents::PlainText(std::string(p_Actor->m_sActorName.c_str(), p_Actor->m_sActorName.size()));

    if (s_Name.empty() || s_Name == "John Doe")
        return std::string("a ") + s_Kind;

    return s_Name + " (" + s_Kind + ")";
}

bool SdkWorld::PickAimed(uint64_t& p_Id, coop::Vec3& p_Pos, std::string& p_Label) {
    coop::Transform s_Local;

    if (!m_InScene || m_ViewWidth < 1.f || !GetLocalAvatar(s_Local))
        return false;

    SVector3 s_Origin, s_Dir;

    if (!SDK()->ScreenToWorld(SVector2(m_ViewWidth * 0.5f, m_ViewHeight * 0.5f), s_Origin, s_Dir))
        return false;

    const coop::Vec3 s_From { s_Origin.x - s_Dir.x, s_Origin.y - s_Dir.y, s_Origin.z - s_Dir.z }; // the camera
    const coop::Vec3 s_Ray { s_Dir.x, s_Dir.y, s_Dir.z };

    // The NPC whose chest is nearest the line of sight: within about 2.5 degrees (0.5 m of it up
    // close), and 60 m (past that, what's in line is rarely what the player is looking at).
    float s_Best = 1e9f;
    ZActor* s_Picked = nullptr;
    coop::Vec3 s_PickedAt;

    for (const auto& [s_Id, s_Npc] : m_NpcsById) {
        ZEntityRef s_Ref;

        if (!s_Npc || !s_Npc->m_bEnabled || (s_Npc->GetID(s_Ref), !s_Ref))
            continue;

        const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>();

        if (!s_Spatial)
            continue;

        const coop::Vec3 s_Feet = ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos;
        const coop::Vec3 s_Chest = s_Feet + coop::Vec3 { 0.f, 0.f, 1.3f };
        const coop::Vec3 s_To = s_Chest - s_From;
        const float s_Along = s_To.x * s_Ray.x + s_To.y * s_Ray.y + s_To.z * s_Ray.z;

        if (s_Along < 0.5f || s_Along > 60.f)
            continue;

        const float s_Off = coop::Distance(s_Chest, s_From + s_Ray * s_Along);

        if (s_Off > std::max(0.5f, s_Along * 0.045f))
            continue;

        if (const float s_Score = s_Off / s_Along; s_Score < s_Best) {
            s_Best = s_Score;
            s_Picked = s_Npc;
            p_Id = s_Id;
            s_PickedAt = s_Feet;
        }
    }

    if (s_Picked) {
        p_Pos = s_PickedAt;
        p_Label = NpcLabel(s_Picked);
        return true;
    }

    // No one there: whatever the line of sight hits, by the game's own collision (a wall, a door,
    // a balcony, a car), from just past our 47 (who is in the way of an over-the-shoulder camera).
    if (Globals::CollisionManager && *Globals::CollisionManager) {
        const float s_Skip = std::max(0.8f, coop::Distance(s_From, s_Local.pos + coop::Vec3 { 0.f, 0.f, 1.4f }) + 0.6f);
        const coop::Vec3 s_Start = s_From + s_Ray * s_Skip;
        const coop::Vec3 s_End = s_From + s_Ray * 150.f;
        ZRayQueryInput s_Input {
            .m_vFrom = float4(s_Start.x, s_Start.y, s_Start.z, 1.f),
            .m_vTo = float4(s_End.x, s_End.y, s_End.z, 1.f),
        };
        ZRayQueryOutput s_Output {};

        if ((*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Output) && s_Output.m_bHasHit) {
            const coop::Vec3 s_Hit { s_Output.m_vPosition.x, s_Output.m_vPosition.y, s_Output.m_vPosition.z };

            if (std::isfinite(s_Hit.x) && std::isfinite(s_Hit.y) && std::isfinite(s_Hit.z) && coop::Distance(s_Hit, s_From) < 151.f) {
                p_Id = 0;
                p_Pos = s_Hit;
                p_Label = "a spot";
                return true;
            }
        }
    }

    // Nothing hit (or no collision in this build): the floor at our 47's level, where the line of
    // sight comes down to it.
    const float s_Drop = s_From.z - s_Local.pos.z;

    if (s_Ray.z > -0.02f || s_Drop <= 0.f)
        return false;

    const float s_T = s_Drop / -s_Ray.z;

    if (s_T > 100.f)
        return false;

    p_Id = 0;
    p_Pos = s_From + s_Ray * s_T;
    p_Label = "a spot";
    return true;
}

// A mark on that NPC, or for a place one within a few metres of it.
static bool MarksSame(uint64_t p_Id, const coop::Vec3& p_Pos, uint64_t p_OtherId, const coop::Vec3& p_OtherPos) {
    return p_Id != 0 ? p_Id == p_OtherId : p_OtherId == 0 && coop::Distance(p_Pos, p_OtherPos) < 2.5f;
}

void SdkWorld::AddMark(uint64_t p_Id, const coop::Vec3& p_Pos, bool p_Ours, const std::string& p_Label, bool p_Danger) {
    // A few marks per player (Settings, the partner's up to 3): the same one again replaces it, and
    // past that the oldest goes.
    std::erase_if(m_Marks, [&](const MarkState& p_Mark) { return p_Mark.ours == p_Ours && MarksSame(p_Id, p_Pos, p_Mark.id, p_Mark.pos); });
    const size_t s_Max = static_cast<size_t>(p_Ours ? std::clamp(m_MaxMarks, 1, 3) : 3);

    for (size_t s_Count = std::count_if(m_Marks.begin(), m_Marks.end(), [&](const MarkState& p_Mark) { return p_Mark.ours == p_Ours; });
         s_Count >= s_Max; --s_Count)
        m_Marks.erase(std::find_if(m_Marks.begin(), m_Marks.end(), [&](const MarkState& p_Mark) { return p_Mark.ours == p_Ours; }));

    ZActor* s_Actor = p_Id ? FindNpc(p_Id) : nullptr;
    m_Marks.push_back({ p_Id, p_Pos, m_Now, p_Ours, p_Label, p_Danger, s_Actor && s_Actor->m_bContractTarget, s_Actor && ActorDown(s_Actor) });

    if (m_Trace)
        m_Trace(std::string("mark: ") + (p_Ours ? "ours, " : "the partner's, ") + p_Label + (p_Danger ? " (danger)" : ""));
}

bool SdkWorld::OurMarkOn(uint64_t p_Id, const coop::Vec3& p_Pos, double& p_Age, bool& p_Danger) const {
    for (const auto& s_Mark : m_Marks) {
        if (s_Mark.ours && m_Now - s_Mark.time <= m_MarkSeconds && MarksSame(p_Id, p_Pos, s_Mark.id, s_Mark.pos)) {
            p_Age = m_Now - s_Mark.time;
            p_Danger = s_Mark.danger;
            return true;
        }
    }

    return false;
}

void SdkWorld::RemoveMark(bool p_Ours, uint64_t p_Id, const coop::Vec3& p_Pos) {
    std::erase_if(m_Marks, [&](const MarkState& p_Mark) { return p_Mark.ours == p_Ours && MarksSame(p_Id, p_Pos, p_Mark.id, p_Mark.pos); });
}

void SdkWorld::UpdateMarks() {
    std::erase_if(m_Marks, [&](const MarkState& p_Mark) {
        if (m_Now - p_Mark.time > m_MarkSeconds)
            return true;

        ZActor* s_Actor = p_Mark.id && !p_Mark.down ? FindNpc(p_Mark.id) : nullptr;

        if (!s_Actor || !ActorDown(s_Actor))
            return false;

        NotifyAs(ToastKind::Info, (p_Mark.ours ? "Your mark is down: " : m_PartnerName + "'s mark is down: ") + p_Mark.label + ".");
        return true;
    });
}

void SdkWorld::OnPeerMark(uint64_t p_Id, const coop::Vec3& p_Pos, uint8_t p_Kind) {
    if (p_Kind == coop::MarkKind_Remove) {
        RemoveMark(false, p_Id, p_Pos);

        if (m_Trace)
            m_Trace("mark: the partner took theirs back");

        return;
    }

    ZActor* s_Actor = p_Id ? FindNpc(p_Id) : nullptr;
    const bool s_Danger = p_Kind == coop::MarkKind_Danger;
    const std::string s_Label = s_Actor ? NpcLabel(s_Actor) : s_Danger ? "danger" : "a spot";
    AddMark(s_Actor ? p_Id : 0, p_Pos, false, s_Label, s_Danger);

    coop::Transform s_Local;
    const std::string s_Distance = GetLocalAvatar(s_Local) ? " (" + Distance(coop::Distance(s_Local.pos, p_Pos)) + ")" : std::string();

    if (s_Danger)
        NotifyAs(ToastKind::Warning, m_PartnerName + " warns you: " + (s_Actor ? s_Label : std::string("danger there")) + s_Distance + "!");
    else
        NotifyAs(ToastKind::Partner, m_PartnerName + " marked " + s_Label + s_Distance + ".");
}

SdkWorld::Overlay SdkWorld::BuildOverlay() const {
    Overlay s_Out;
    coop::Transform s_Local;
    s_Out.inMission = m_InScene && GetLocalAvatar(s_Local);

    if (!s_Out.inMission)
        return s_Out;

    s_Out.local = s_Local.pos;

    if (const auto s_Camera = Functions::GetCurrentCamera->Call()) {
        const SMatrix s_M = s_Camera->GetObjectToWorldMatrix();
        s_Out.camera = { s_M.Trans.x, s_M.Trans.y, s_M.Trans.z };
    }

    // Which way the view looks, and which way is right on screen (for the radar), from the lines
    // of sight through the screen's centre and a point right of it: no guess at the engine's axes.
    if (SVector3 s_Origin, s_Dir, s_RightOrigin, s_RightDir; m_ViewWidth >= 1.f &&
        SDK()->ScreenToWorld(SVector2(m_ViewWidth * 0.5f, m_ViewHeight * 0.5f), s_Origin, s_Dir) &&
        SDK()->ScreenToWorld(SVector2(m_ViewWidth * 0.5f + 100.f, m_ViewHeight * 0.5f), s_RightOrigin, s_RightDir)) {
        s_Out.cameraForward = { s_Dir.x, s_Dir.y, s_Dir.z };
        s_Out.cameraRight = { s_RightDir.x - s_Dir.x, s_RightDir.y - s_Dir.y, s_RightDir.z - s_Dir.z };
    }

    coop::Vec3 s_Partner;

    if (HasPartner() && GetPartnerPosition(s_Partner)) {
        s_Out.partnerShown = true;
        s_Out.partnerFeet = s_Partner;
        s_Out.partnerHead = s_Partner + coop::Vec3 { 0.f, 0.f, 2.05f };
        s_Out.partnerDistance = coop::Distance(s_Local.pos, s_Partner);
        s_Out.partnerHeight = s_Partner.z - s_Local.pos.z;
        s_Out.partnerDisguise = PartnerDisguise();
    }

    s_Out.pulse = m_Now < m_PulseUntil;

    s_Out.markSeconds = static_cast<float>(m_MarkSeconds);

    if (m_PartnerSignal >= 0 && m_Now - m_PartnerSignalAt < 4.0) {
        s_Out.signal = m_PartnerSignal;
        s_Out.signalAge = static_cast<float>(m_Now - m_PartnerSignalAt);
    }

    // Target assist: the mission's living targets.
    if (m_ListTargets) {
        for (const auto& [s_Id, s_Npc] : m_NpcsById) {
            ZEntityRef s_Ref;

            if (!s_Npc || !s_Npc->m_bContractTarget || !s_Npc->m_bEnabled || ActorDown(s_Npc) || (s_Npc->GetID(s_Ref), !s_Ref))
                continue;

            if (const auto* s_Spatial = s_Ref.QueryInterface<ZSpatialEntity>())
                s_Out.targets.push_back(ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos);
        }
    }

    for (const auto& s_Mark : m_Marks) {
        if (m_Now - s_Mark.time > m_MarkSeconds)
            continue;

        Overlay::Marker s_Marker;
        s_Marker.pos = s_Mark.pos;

        // An NPC's mark follows it.
        if (ZActor* s_Actor = s_Mark.id ? FindNpc(s_Mark.id) : nullptr) {
            ZEntityRef s_Ref;
            s_Actor->GetID(s_Ref);

            if (const auto* s_Spatial = s_Ref ? s_Ref.QueryInterface<ZSpatialEntity>() : nullptr)
                s_Marker.pos = ToTransform(s_Spatial->GetObjectToWorldMatrix()).pos;
        }

        s_Marker.distance = coop::Distance(s_Local.pos, s_Marker.pos);
        s_Marker.ground = s_Marker.pos;
        s_Marker.pos = s_Marker.pos + coop::Vec3 { 0.f, 0.f, s_Mark.id ? 2.2f : 0.3f };
        s_Marker.label = s_Mark.label;
        s_Marker.ours = s_Mark.ours;
        s_Marker.danger = s_Mark.danger;
        s_Marker.target = s_Mark.target;
        s_Marker.age = static_cast<float>(m_Now - s_Mark.time);
        s_Out.marks.push_back(std::move(s_Marker));
    }

    return s_Out;
}

bool SdkWorld::RegroupToPartner(std::string& p_Why) {
    auto s_Player = SDK()->GetLocalPlayer();
    const auto* s_Stand = m_Partner && HasPartner() ? m_Partner.QueryInterface<ZSpatialEntity>() : nullptr;

    if (!m_InScene || !s_Player || !s_Stand) {
        p_Why = "your partner isn't in the mission with you";
        return false;
    }

    auto* s_Spatial = s_Player.m_entityRef.QueryInterface<ZSpatialEntity>();

    if (!s_Spatial) {
        p_Why = "your 47 isn't there";
        return false;
    }

    // A step behind them (characters face -Y in their own frame), where they just came from.
    SMatrix s_Matrix = s_Stand->GetObjectToWorldMatrix();
    s_Matrix.Trans.x += s_Matrix.YAxis.x * 1.2f;
    s_Matrix.Trans.y += s_Matrix.YAxis.y * 1.2f;
    s_Spatial->SetObjectToWorldMatrixFromEditor(s_Matrix);

    if (m_Trace)
        m_Trace("regroup: our 47 moved behind the stand-in");

    return true;
}

void SdkWorld::SetPauseTogether(bool p_On) {
    if (p_On == m_PauseTogether)
        return;

    m_PauseTogether = p_On;
    GamePause::SetPartnerPause(WantPartnerHold());
}

void SdkWorld::UpdatePauseCap() {
    // A partner who walked away with their pause menu open: after a while our game goes on.
    if (m_PartnerWantsPause && !m_PauseCapped && m_PartnerPauseSince >= 0.0 && m_Now - m_PartnerPauseSince >= c_PauseHoldMax) {
        m_PauseCapped = true;
        NotifyAs(ToastKind::Warning, m_PartnerName + " has been paused for 2 minutes: your game goes on. It pauses with theirs again next time.");
    }

    // Every frame: their menu (only their map, say) can change while they stay paused.
    if (m_InScene)
        GamePause::SetPartnerPause(WantPartnerHold());
}

std::string SdkWorld::Distance(float p_Metres) const {
    char s_Text[24];
    std::snprintf(s_Text, sizeof(s_Text), m_Feet ? "%.0f ft" : "%.0f m", m_Feet ? p_Metres * 3.28084f : p_Metres);
    return s_Text;
}

std::string SdkWorld::PartnerDisguise() const {
    if (!m_HasPartnerOutfit)
        return {};

    const auto* s_Kit = FindOutfitKit(m_PartnerOutfit.repositoryId);

    if (!s_Kit || !s_Kit->m_pInterfaceRef)
        return {};

    if (s_Kit->m_pInterfaceRef->m_bIsHitmanSuit)
        return "their suit";

    const ZString& s_Name = s_Kit->m_pInterfaceRef->m_sCommonName;
    return coop::GameEvents::PlainText(std::string(s_Name.c_str(), s_Name.size()), 40);
}
