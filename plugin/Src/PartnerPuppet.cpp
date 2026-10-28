#include "PartnerPuppet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "coop/Math.h"

#include "Functions.h"
#include "Globals.h"

#include <Glacier/Enums.h>
#include <Glacier/EntityFactory.h>
#include <Glacier/SExternalReferences.h>
#include <Glacier/TArray.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZObject.h>
#include <Glacier/ZTypeRegistry.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZSpatialEntity.h>

#include <DirectXMath.h>

// Engine classes used only through reflection; the names are what the type registry knows them by.
class IActor;
class ZActor;
class ZBehaviorEntityBase;

class ZPatrolBehaviorEntity {
public:
    enum class ERotationAlignment : int32_t { RA_NONE = 0, RA_LOOSE = 1, RA_EXACT = 2 };
};


namespace {
SMatrix ToMatrix(const coop::Transform& p_Xf) {
    const DirectX::XMVECTOR s_Q = DirectX::XMVectorSet(p_Xf.rot.x, p_Xf.rot.y, p_Xf.rot.z, p_Xf.rot.w);
    SMatrix s_Matrix(DirectX::XMMatrixRotationQuaternion(s_Q));
    s_Matrix.Trans = float4(p_Xf.pos.x, p_Xf.pos.y, p_Xf.pos.z, 1.f);
    return s_Matrix;
}

void Place(ZEntityRef p_Entity, const coop::Transform& p_Xf) {
    if (auto* s_Spatial = p_Entity.QueryInterface<ZSpatialEntity>())
        s_Spatial->SetObjectToWorldMatrixFromEditor(ToMatrix(p_Xf));
}

// Sets a property from a value of the engine type named here. The type is looked up by the name
// the game's registry uses (the SDK's compile-time names don't match it for nested templates).
template <class T>
bool SetTyped(ZEntityRef p_Entity, const char* p_Property, const char* p_TypeName, const T& p_Value) {
    STypeID* s_Type = (*Globals::TypeRegistry)->GetTypeID(ZString(std::string_view(p_TypeName)));

    if (!s_Type || !s_Type->GetTypeInfo() || s_Type->GetTypeInfo()->m_nTypeSize != sizeof(T))
        return false;

    ZObjectRef s_Value;
    s_Value.Assign(s_Type, const_cast<T*>(&p_Value));
    return p_Entity.SetProperty(ZString(std::string_view(p_Property)), s_Value);
}
}

ZEntityRef PartnerPuppet::Spawn(TResourcePtr<ZTemplateEntityFactory>& p_Factory, const char* p_Name) {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!p_Factory || !s_Scene)
        return {};

    ZEntityRef s_Entity;
    SExternalReferences s_ExternalRefs;
    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, s_Entity, ZString(std::string_view(p_Name)), p_Factory, s_Scene.m_entityRef, s_ExternalRefs, -1
    );

    if (!s_Entity && m_Trace)
        m_Trace(std::string("puppet: could not create ") + p_Name);

    return s_Entity;
}

PartnerPuppet::Load PartnerPuppet::Request(const TraceFn& p_Trace) {
    m_Trace = p_Trace;

    struct Item { TResourcePtr<ZTemplateEntityFactory>* factory; ZRuntimeResourceID id; const char* name; };
    const Item s_Items[] = {
        { &m_SpatialFactory, ResId<"[modules:/zspatialentity.class].pc_entitytype">, "spatial" },
        { &m_PatrolFactory, ResId<"[modules:/zpatrolbehaviorentity.class].pc_entitytype">, "patrol behaviour" },
        { &m_PromoterFactory, ResId<"[modules:/zbehaviorpromoterentity.class].pc_entitytype">, "behaviour promoter" },
        { &m_LookAtFactory, ResId<"[modules:/zactorlookatentity.class].pc_entitytype">, "actor look-at" },
    };

    Load s_Result = Load::Ready;
    std::string s_Report = "puppet: factories";

    for (const auto& s_Item : s_Items) {
        if (s_Item.factory->m_nResourceIndex.val < 0)
            Globals::ResourceManager->LoadResource(*s_Item.factory, s_Item.id);

        const int s_Status = s_Item.factory->m_nResourceIndex.val < 0 ? -1 : static_cast<int>(s_Item.factory->GetResourceInfo().status);
        s_Report += std::string(" | ") + s_Item.name + " " + std::to_string(s_Status);

        if (s_Status == -1 || s_Status == RESOURCE_STATUS_FAILED)
            s_Result = Load::Failed;
        else if (s_Status != RESOURCE_STATUS_VALID && s_Result == Load::Ready)
            s_Result = Load::Pending;
    }

    if (m_Trace && s_Report != m_LastReport) {
        m_Trace(s_Report);
        m_LastReport = s_Report;
    }

    return s_Result;
}

bool PartnerPuppet::BuildSlot(Slot& p_Slot) {
    p_Slot.target = Spawn(m_SpatialFactory, "HitmanCoopPartnerTarget");
    p_Slot.patrol = Spawn(m_PatrolFactory, "HitmanCoopPartnerPatrol");
    p_Slot.promoter = Spawn(m_PromoterFactory, "HitmanCoopPartnerPromoter");

    if (!p_Slot.target || !p_Slot.patrol || !p_Slot.promoter)
        return false;

    TArray<TEntityRef<ZSpatialEntity>> s_Waypoints;
    s_Waypoints.push_back(TEntityRef<ZSpatialEntity>(p_Slot.target));

    // Exact rotation: when the partner stops, the stand-in ends up facing the same way.
    const bool s_Ok = SetTyped(p_Slot.patrol, "m_aWaypoints", "TArray<TEntityRef<ZSpatialEntity>>", s_Waypoints) &&
        SetTyped(p_Slot.patrol, "m_MoveSpeed", "EMoveSpeed", EMoveSpeed::MS_Walking) &&
        SetTyped(p_Slot.patrol, "m_bAlignPosition", "bool", false) &&
        SetTyped(p_Slot.patrol, "m_fStopMoveDistance", "float32", 0.3f) &&
        SetTyped(p_Slot.patrol, "m_AlignRotation", "ZPatrolBehaviorEntity.ERotationAlignment", ZPatrolBehaviorEntity::ERotationAlignment::RA_EXACT) &&
        SetTyped(p_Slot.promoter, "m_pActor", "TEntityRef<IActor>", TEntityRef<IActor>(m_Actor)) &&
        SetTyped(p_Slot.promoter, "m_pBehavior", "TEntityRef<ZBehaviorEntityBase>", TEntityRef<ZBehaviorEntityBase>(p_Slot.patrol));

    if (!s_Ok && m_Trace)
        m_Trace("puppet: a patrol property could not be set");

    return s_Ok;
}

bool PartnerPuppet::Attach(ZEntityRef p_Actor, const coop::Transform& p_At, const TraceFn& p_Trace) {
    m_Trace = p_Trace;
    m_Actor = p_Actor;

    for (auto& s_Slot : m_Slots) {
        if (!BuildSlot(s_Slot)) {
            Detach();
            return false;
        }
    }

    m_LookTarget = Spawn(m_SpatialFactory, "HitmanCoopPartnerLook");
    m_LookAt = Spawn(m_LookAtFactory, "HitmanCoopPartnerLookAt");

    if (m_LookAt && m_LookTarget) {
        TArray<TEntityRef<ZActor>> s_Actors;
        s_Actors.push_back(TEntityRef<ZActor>(m_Actor));
        const bool s_Ok = SetTyped(m_LookAt, "m_aActors", "TArray<TEntityRef<ZActor>>", s_Actors) &&
            SetTyped(m_LookAt, "m_rLookAtEntity", "TEntityRef<ZSpatialEntity>", TEntityRef<ZSpatialEntity>(m_LookTarget));
        SetTyped(m_LookAt, "m_fSettingDistanceMax", "float32", 1000.f);
        SetTyped(m_LookAt, "m_fSettingDuration", "float32", 100000.f);
        SetTyped(m_LookAt, "m_bExclusive", "bool", true);
        m_LookAt.SignalInputPin("Enable");

        if (m_Trace)
            m_Trace(std::string("puppet: look-at ") + (s_Ok ? "on" : "not set up"));
    }

    m_Attached = true;
    m_NextSlot = 0;
    SendTo(p_At, true, 0.0);

    if (m_Trace)
        m_Trace("puppet: attached with " + std::to_string(c_Slots) + " slots");

    return true;
}

void PartnerPuppet::SendTo(const coop::Transform& p_Goal, bool p_Final, double p_Now) {
    Slot& s_Slot = m_Slots[m_NextSlot];
    m_NextSlot = (m_NextSlot + 1) % c_Slots;

    Place(s_Slot.target, p_Goal);
    SetTyped(s_Slot.patrol, "m_MoveSpeed", "EMoveSpeed", static_cast<EMoveSpeed>(m_Speed));
    // Only a final stop turns to the partner's facing: aligning on every intermediate goal made
    // the actor stop and turn before taking the next order (about 2 s whenever the partner
    // turned around).
    SetTyped(s_Slot.patrol, "m_AlignRotation", "ZPatrolBehaviorEntity.ERotationAlignment",
        p_Final ? ZPatrolBehaviorEntity::ERotationAlignment::RA_EXACT : ZPatrolBehaviorEntity::ERotationAlignment::RA_NONE);
    SetTyped(s_Slot.patrol, "m_fStopMoveDistance", "float32", p_Final ? 0.3f : 1.0f);
    s_Slot.promoter.SignalInputPin("Promote");

    m_Goal = p_Goal.pos;
    m_LastSend = p_Now;
}

void PartnerPuppet::Detach() {
    const auto Delete = [](ZEntityRef& p_Entity) {
        if (p_Entity)
            Functions::ZEntityManager_DeleteEntity->Call(Globals::EntityManager, p_Entity, {});

        p_Entity = {};
    };

    Delete(m_LookAt);
    Delete(m_LookTarget);

    for (auto& s_Slot : m_Slots) {
        Delete(s_Slot.promoter);
        Delete(s_Slot.patrol);
        Delete(s_Slot.target);
    }

    m_Actor = {};
    m_Attached = false;
}

void PartnerPuppet::Pause() {
    if (!m_Attached || m_Paused)
        return;

    for (auto& s_Slot : m_Slots)
        if (s_Slot.patrol)
            SetTyped(s_Slot.patrol, "m_fStopMoveDistance", "float32", 100000.f);

    m_Paused = true;
}

void PartnerPuppet::Forget() {
    m_Actor = {};
    m_LookTarget = {};
    m_LookAt = {};

    for (auto& s_Slot : m_Slots)
        s_Slot = {};

    m_Attached = false;
    m_Paused = false;
}

bool PartnerPuppet::Steer(
    const coop::Transform& p_Target, const coop::Vec3& p_Velocity, const coop::Vec3& p_Actor, const coop::Vec3& p_LookAt,
    double p_Now
) {
    if (!m_Attached)
        return false;

    coop::Transform s_Look;
    s_Look.pos = p_LookAt;
    Place(m_LookTarget, s_Look);

    const float s_PartnerSpeed = std::sqrt(p_Velocity.x * p_Velocity.x + p_Velocity.y * p_Velocity.y);
    const float s_Gap = coop::Distance(p_Target.pos, p_Actor);
    const bool s_Moving = s_PartnerSpeed > 0.3f;

    // Gait from the partner's speed (47 walks about 1.5 m/s, runs 4, sprints 6+), one step faster
    // when the stand-in has fallen behind: NPCs walk a little slower than 47.
    int s_Speed = s_PartnerSpeed < 2.4f ? 1 : s_PartnerSpeed < 5.f ? 2 : 3;

    if (s_Moving && s_Gap > 2.f)
        s_Speed = std::min(3, s_Speed + (s_Gap > 6.f ? 2 : 1));

    // Where to send it: a little ahead of the partner while they move (so it isn't always
    // arriving just behind them), exactly onto them, facing their way, once they stop.
    coop::Transform s_Goal = p_Target;

    if (s_Moving) {
        const float s_Lead = std::min(0.8f, s_Gap / std::max(s_PartnerSpeed, 0.5f) + 0.4f);
        s_Goal.pos.x += p_Velocity.x * s_Lead;
        s_Goal.pos.y += p_Velocity.y * s_Lead;
    }

    // The AI acts on a new order after a moment and drops orders sent too quickly, so a new one
    // goes out only when it is nearly at the current goal, the partner has moved well away from
    // it, the partner stopped, or the gait changed.
    const float s_ToGoal = coop::Distance(p_Actor, m_Goal);
    const float s_GoalOff = coop::Distance(s_Goal.pos, m_Goal);
    const bool s_Arriving = s_ToGoal < 1.2f && s_GoalOff > 0.4f;
    const bool s_Stale = s_GoalOff > std::max(1.5f, 0.6f * s_Gap);
    const bool s_Stopped = !s_Moving && m_WasMoving;
    const bool s_GaitChanged = s_Speed != m_Speed;
    const double s_MinGap = s_Stopped || s_GaitChanged ? 0.25 : m_RepromoteEvery;

    m_WasMoving = s_Moving;

    if (p_Now - m_LastSend < s_MinGap || !(s_Arriving || s_Stale || s_Stopped || s_GaitChanged))
        return false;

    m_Speed = s_Speed;
    SendTo(s_Goal, !s_Moving, p_Now);
    return true;
}
