#include "coop/Interpolation.h"

#include <algorithm>

namespace coop {

NpcCorrectionPolicy::Action NpcCorrectionPolicy::Decide(float p_Drift, float p_DistanceToLocalPlayer) const {
    if (p_DistanceToLocalPlayer < localBubble || p_Drift < minDrift)
        return Action::None;

    if (p_Drift >= snapDrift)
        return Action::Snap;

    return Action::Blend;
}

Transform NpcCorrectionPolicy::Blend(const Transform& p_Local, const Transform& p_Host, float p_Dt) const {
    const float s_T = std::clamp(blendPerSecond * p_Dt, 0.f, 1.f);
    return Lerp(p_Local, p_Host, s_T);
}

} // namespace coop
