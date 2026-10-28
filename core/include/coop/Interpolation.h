#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

#include "Math.h"

namespace coop {

// Buffers timestamped values (transforms, poses) and plays them back a delay behind real time, so
// the remote avatar moves smoothly between 30 Hz snapshots and through small gaps. The delay can
// follow the link (Retarget with Wanted()): just over the send interval plus the lateness seen
// lately, so a good link plays back with little lag and a jittery one doesn't stall.
// T needs a Lerp(const T&, const T&, float) overload.
template <class T>
class TimedBuffer {
public:
    explicit TimedBuffer(double p_Delay = 0.1, size_t p_Capacity = 64) : m_Delay(p_Delay), m_Capacity(p_Capacity) {}

    // p_SenderTimeMs is the sender's clock. Playback follows the sender's timeline, offset by the
    // smallest recently seen (receive - send) gap, so network jitter doesn't cause stalls and jumps.
    void Push(double p_ReceiveTime, uint32_t p_SenderTimeMs, const T& p_Value) {
        double s_SenderTime;

        if (!m_HasLast) {
            s_SenderTime = p_SenderTimeMs / 1000.0;
        }
        else {
            // Unwrap the 32-bit millisecond clock; drop anything older than what we have.
            const int32_t s_Delta = static_cast<int32_t>(p_SenderTimeMs - m_LastMs);

            if (s_Delta <= 0)
                return;

            s_SenderTime = m_LastSenderTime + s_Delta / 1000.0;
        }

        m_HasLast = true;
        m_LastMs = p_SenderTimeMs;
        m_LastSenderTime = s_SenderTime;
        m_Snapshots.push_back({ s_SenderTime, p_Value });

        while (m_Snapshots.size() > m_Capacity)
            m_Snapshots.pop_front();

        const double s_Offset = p_ReceiveTime - s_SenderTime;
        m_Offsets.push_back(s_Offset);

        while (m_Offsets.size() > m_Capacity)
            m_Offsets.pop_front();

        // How late this one came against the quickest lately; the worst of those, held for a few
        // seconds (fading), so a link that bunches up now and then keeps enough delay meanwhile.
        const double s_Late = s_Offset - *std::min_element(m_Offsets.begin(), m_Offsets.end());

        if (m_HasReceive) {
            const double s_Gap = std::max(0.0, p_ReceiveTime - m_LastReceive);
            m_Late = std::max(m_Late * std::exp(-s_Gap / c_LateMemory), std::min(s_Late, 1.0));

            const double s_Interval = s_SenderTime - m_PrevSenderTime;

            if (s_Interval > 0.0 && s_Interval < 1.0)
                m_Interval += (s_Interval - m_Interval) * 0.1;
        }

        m_HasReceive = true;
        m_LastReceive = p_ReceiveTime;
        m_PrevSenderTime = s_SenderTime;
    }

    // The delay this stream needs: one send interval, the lateness seen lately and a little margin.
    double Wanted(double p_Min, double p_Max) const {
        return std::clamp(m_Interval + m_Late + 0.012, p_Min, p_Max);
    }

    // The delay to play back at from now on, reached gradually (at most 5% faster or slower than
    // real time), so a change never shows as a jump.
    void Retarget(double p_Delay) { m_TargetDelay = p_Delay; }
    double Delay() const { return m_Delay; }

    // False until the first snapshot arrives. Call once per frame with increasing p_Now.
    bool Sample(double p_Now, T& p_Out) {
        if (m_Snapshots.empty())
            return false;

        const double s_Measured = *std::min_element(m_Offsets.begin(), m_Offsets.end());

        if (!m_HasApplied) {
            m_AppliedOffset = s_Measured;
            m_HasApplied = true;
        }
        else {
            // Play back at most 10% faster or slower than real time while converging.
            const double s_MaxStep = std::max(0.0, p_Now - m_LastSampleTime) * 0.1;
            m_AppliedOffset += std::clamp(s_Measured - m_AppliedOffset, -s_MaxStep, s_MaxStep);
        }

        if (m_TargetDelay > 0.0 && m_HasSampled) {
            const double s_MaxStep = std::max(0.0, p_Now - m_LastSampleTime) * 0.05;
            m_Delay += std::clamp(m_TargetDelay - m_Delay, -s_MaxStep, s_MaxStep);
        }

        m_HasSampled = true;
        m_LastSampleTime = p_Now;
        const double s_Target = p_Now - m_AppliedOffset - m_Delay;

        if (s_Target <= m_Snapshots.front().senderTime) {
            p_Out = m_Snapshots.front().value;
            return true;
        }

        for (size_t i = 1; i < m_Snapshots.size(); ++i) {
            const auto& s_A = m_Snapshots[i - 1];
            const auto& s_B = m_Snapshots[i];

            if (s_Target <= s_B.senderTime) {
                const double s_Span = s_B.senderTime - s_A.senderTime;
                const float s_T = s_Span > 1e-6 ? static_cast<float>((s_Target - s_A.senderTime) / s_Span) : 1.f;
                p_Out = Lerp(s_A.value, s_B.value, s_T);
                return true;
            }
        }

        // Past the newest snapshot: hold the last one instead of extrapolating into walls.
        p_Out = m_Snapshots.back().value;
        return true;
    }

    void Clear() {
        m_Snapshots.clear();
        m_Offsets.clear();
        m_HasLast = false;
        m_HasApplied = false;
        m_HasSampled = false;
        m_HasReceive = false;
        m_Late = 0.0;
    }

    bool Empty() const { return m_Snapshots.empty(); }

private:
    struct Snapshot {
        double senderTime; // seconds, unwrapped
        T value;
    };

    static constexpr double c_LateMemory = 6.0; // seconds for a late burst to fade by e

    double m_Delay;
    double m_TargetDelay = 0.0; // 0: fixed at m_Delay
    size_t m_Capacity;
    bool m_HasSampled = false;
    bool m_HasReceive = false;
    double m_LastReceive = 0.0;
    double m_PrevSenderTime = 0.0;
    double m_Late = 0.0;
    double m_Interval = 1.0 / 30.0;
    std::deque<Snapshot> m_Snapshots;
    std::deque<double> m_Offsets;
    bool m_HasLast = false;
    uint32_t m_LastMs = 0;
    double m_LastSenderTime = 0.0;

    // The offset actually used for playback. It slews towards the measured one so a
    // new latency minimum shifts playback gradually instead of skipping ahead.
    bool m_HasApplied = false;
    double m_AppliedOffset = 0.0;
    double m_LastSampleTime = 0.0;
};

using SnapshotBuffer = TimedBuffer<Transform>;
using PoseBuffer = TimedBuffer<Pose>;

// Decides whether the client should pull its local copy of an NPC towards the host's copy.
// NPCs near the client's own 47 are left alone so local reactions (spotting, chasing) can play out.
struct NpcCorrectionPolicy {
    float minDrift = 0.75f;         // ignore drift below this (metres)
    float snapDrift = 6.f;          // teleport straight to the host position above this
    float localBubble = 25.f;       // no correction within this distance of the local player (seen sliding through walls at 8)
    float blendPerSecond = 4.f;     // fraction of the gap closed per second while blending

    enum class Action { None, Blend, Snap };

    Action Decide(float p_Drift, float p_DistanceToLocalPlayer) const;

    // Position after blending for p_Dt seconds.
    Transform Blend(const Transform& p_Local, const Transform& p_Host, float p_Dt) const;
};

} // namespace coop
