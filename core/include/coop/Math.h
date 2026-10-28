#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop {

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct Quat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

struct Transform {
    Vec3 pos;
    Quat rot;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

inline float Length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Distance(Vec3 a, Vec3 b) { return Length(a - b); }
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

inline float Dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Quat Normalize(Quat q) {
    const float s_Len = std::sqrt(Dot(q, q));

    if (s_Len < 1e-6f)
        return {};

    return { q.x / s_Len, q.y / s_Len, q.z / s_Len, q.w / s_Len };
}

// Normalized lerp along the shortest arc. Good enough for 10-30 Hz snapshots.
inline Quat Nlerp(Quat a, Quat b, float t) {
    if (Dot(a, b) < 0.f)
        b = { -b.x, -b.y, -b.z, -b.w };

    return Normalize({
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t,
    });
}

// Angle between two rotations in radians.
inline float AngleBetween(Quat a, Quat b) {
    const float s_Dot = std::min(1.f, std::fabs(Dot(Normalize(a), Normalize(b))));
    return 2.f * std::acos(s_Dot);
}

inline Transform Lerp(const Transform& a, const Transform& b, float t) {
    return { Lerp(a.pos, b.pos, t), Nlerp(a.rot, b.rot, t) };
}

// A character's skeleton pose: every bone's parent-relative rotation, plus the positions of the
// root and pelvis (other bones keep the receiving character's own bone lengths).
struct Pose {
    std::vector<Quat> rotations;
    Vec3 rootPos;
    Vec3 pelvisPos;
};

inline Pose Lerp(const Pose& a, const Pose& b, float t) {
    if (a.rotations.size() != b.rotations.size())
        return t < 0.5f ? a : b;

    Pose s_Out;
    s_Out.rotations.resize(a.rotations.size());

    for (size_t i = 0; i < a.rotations.size(); ++i)
        s_Out.rotations[i] = Nlerp(a.rotations[i], b.rotations[i], t);

    s_Out.rootPos = Lerp(a.rootPos, b.rootPos, t);
    s_Out.pelvisPos = Lerp(a.pelvisPos, b.pelvisPos, t);
    return s_Out;
}

// "Smallest three" packing of a unit quaternion into 32 bits: 2 bits say which component is
// largest (it is rebuilt from the other three), then 10 bits for each of the others. Worst
// error is under a quarter of a degree.
inline uint32_t PackQuat(Quat q) {
    q = Normalize(q);
    const float s_C[4] = { q.x, q.y, q.z, q.w };
    int s_Largest = 0;

    for (int i = 1; i < 4; ++i)
        if (std::fabs(s_C[i]) > std::fabs(s_C[s_Largest]))
            s_Largest = i;

    // q and -q are the same rotation: make the dropped component positive.
    const float s_Sign = s_C[s_Largest] < 0.f ? -1.f : 1.f;
    constexpr float c_Range = 0.70710678f; // the other three are within +-1/sqrt(2)
    uint32_t s_Packed = static_cast<uint32_t>(s_Largest) << 30;
    int s_Shift = 20;

    for (int i = 0; i < 4; ++i) {
        if (i == s_Largest)
            continue;

        // Symmetric around zero (-511..511, stored +511), so 0 is exact.
        const long s_Step = std::clamp(std::lround(s_C[i] * s_Sign / c_Range * 511.f), -511L, 511L);
        s_Packed |= static_cast<uint32_t>(s_Step + 511) << s_Shift;
        s_Shift -= 10;
    }

    return s_Packed;
}

inline Quat UnpackQuat(uint32_t p_Packed) {
    constexpr float c_Range = 0.70710678f;
    const int s_Largest = static_cast<int>(p_Packed >> 30);
    float s_C[4] {};
    float s_SumSq = 0.f;
    int s_Shift = 20;

    for (int i = 0; i < 4; ++i) {
        if (i == s_Largest)
            continue;

        const int s_Step = static_cast<int>((p_Packed >> s_Shift) & 1023u) - 511;
        s_C[i] = static_cast<float>(std::clamp(s_Step, -511, 511)) / 511.f * c_Range;
        s_SumSq += s_C[i] * s_C[i];
        s_Shift -= 10;
    }

    s_C[s_Largest] = std::sqrt(std::max(0.f, 1.f - s_SumSq));
    return Normalize({ s_C[0], s_C[1], s_C[2], s_C[3] });
}

} // namespace coop
