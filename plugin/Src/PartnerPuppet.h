#pragma once

#include <array>
#include <functional>
#include <string>

#include "coop/Protocol.h"

#include "Glacier/ZEntity.h"
#include "Glacier/ZResource.h"

class ZTemplateEntityFactory;

// Makes the partner's stand-in walk with its own animations instead of being teleported. The
// game's AI moves it to a marker placed where the partner is going: a ZPatrolBehaviorEntity with
// that one waypoint, handed to the actor by a ZBehaviorPromoterEntity, the way level scripts make
// NPCs walk. Promoting the same behaviour again doesn't change where the actor goes, so there is
// a small ring of marker/patrol/promoter slots and each new destination uses the next one.
// A ZActorLookAtEntity turns its head to where the partner looks.
class PartnerPuppet {
public:
    using TraceFn = std::function<void(const std::string&)>;

    enum class Load { Pending, Ready, Failed };
    // Asks the game for the helper classes; call every frame until it isn't Pending.
    Load Request(const TraceFn& p_Trace);
    // Builds the helper entities for this actor. False if the game refused any of them; the
    // caller then keeps teleporting the stand-in.
    bool Attach(ZEntityRef p_Actor, const coop::Transform& p_At, const TraceFn& p_Trace);
    void Detach();
    // Stops walking the actor (it counts as arrived) but keeps the helper entities: deleting them
    // while the actor's AI still held them crashed the game a moment later (it read their freed
    // names), whenever the partner's pose took over again after a gap.
    void Pause();
    // Walking again: the next Steer sends a fresh order.
    void Resume() {
        if (!m_Paused)
            return;

        m_Paused = false;
        m_Goal = { 1e9f, 1e9f, 1e9f };
        m_LastSend = -1e9;
    }
    // The scene was unloaded and took the helper entities with it.
    void Forget();
    bool Attached() const { return m_Attached; }

    void SetRepromoteEvery(double p_Seconds) { m_RepromoteEvery = p_Seconds; }

    // Where the partner is now, their speed, and the point they look at. True on frames where
    // the stand-in was sent to a new destination.
    bool Steer(const coop::Transform& p_Target, const coop::Vec3& p_Velocity, const coop::Vec3& p_Actor,
        const coop::Vec3& p_LookAt, double p_Now);
    coop::Vec3 Goal() const { return m_Goal; }
    int SpeedLevel() const { return m_Speed; }

private:
    static constexpr size_t c_Slots = 3;

    struct Slot {
        ZEntityRef target;
        ZEntityRef patrol;
        ZEntityRef promoter;
    };

    ZEntityRef Spawn(TResourcePtr<ZTemplateEntityFactory>& p_Factory, const char* p_Name);
    bool BuildSlot(Slot& p_Slot);
    void SendTo(const coop::Transform& p_Goal, bool p_Final, double p_Now);

    TraceFn m_Trace;
    std::string m_LastReport;
    bool m_Attached = false;
    bool m_Paused = false;
    ZEntityRef m_Actor;
    std::array<Slot, c_Slots> m_Slots;
    size_t m_NextSlot = 0;
    ZEntityRef m_LookTarget;
    ZEntityRef m_LookAt;

    double m_LastSend = 0.0;
    coop::Vec3 m_Goal {};
    int m_Speed = 1;
    double m_RepromoteEvery = 0.6;
    bool m_WasMoving = false;

    // Held for the whole game so each class factory is only loaded once (never reassigned).
    TResourcePtr<ZTemplateEntityFactory> m_SpatialFactory;
    TResourcePtr<ZTemplateEntityFactory> m_PatrolFactory;
    TResourcePtr<ZTemplateEntityFactory> m_PromoterFactory;
    TResourcePtr<ZTemplateEntityFactory> m_LookAtFactory;
};
