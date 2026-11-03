#include "coop/Protocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>

namespace coop {

const char* MsgTypeName(MsgType p_Type) {
    switch (p_Type) {
        case MsgType::Hello: return "Hello";
        case MsgType::Welcome: return "Welcome";
        case MsgType::Reject: return "Reject";
        case MsgType::Disconnect: return "Disconnect";
        case MsgType::SceneInfo: return "SceneInfo";
        case MsgType::SceneReady: return "SceneReady";
        case MsgType::AvatarState: return "AvatarState";
        case MsgType::Outfit: return "Outfit";
        case MsgType::NpcSnapshot: return "NpcSnapshot";
        case MsgType::NpcDied: return "NpcDied";
        case MsgType::NpcKillRequest: return "NpcKillRequest";
        case MsgType::MissionEvent: return "MissionEvent";
        case MsgType::Item: return "Item";
        case MsgType::Alerts: return "Alerts";
        case MsgType::Skeleton: return "Skeleton";
        case MsgType::NpcOwners: return "NpcOwners";
        case MsgType::NpcPose: return "NpcPose";
        case MsgType::Pause: return "Pause";
        case MsgType::Restart: return "Restart";
        case MsgType::Chat: return "Chat";
        case MsgType::Mark: return "Mark";
        case MsgType::Busy: return "Busy";
        case MsgType::NpcKnockedOut: return "NpcKnockedOut";
        case MsgType::PlayerState: return "PlayerState";
        case MsgType::NpcCheck: return "NpcCheck";
        case MsgType::Pose: return "Pose";
        case MsgType::VictimPose: return "VictimPose";
        default: return "Unknown";
    }
}

static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);

        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }

    return true;
}

// Glacier resource paths are case-insensitive ("_PRO" and "_pro" both appear).
static std::string Lower(std::string p_Text) {
    for (auto& c : p_Text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return p_Text;
}

static std::string_view FileName(std::string_view p_Path) {
    const auto s_Slash = p_Path.find_last_of("/\\");
    return s_Slash == std::string_view::npos ? p_Path : p_Path.substr(s_Slash + 1);
}

bool SceneDesc::IsMachineBrick(const std::string& p_Brick) {
    // "intel_", a core count, "core": every simulation quality brick in the game's missions is
    // named so (from the missions' own brick lists).
    const std::string s_Lower = Lower(p_Brick);
    const std::string_view s_Name = FileName(s_Lower);

    for (size_t s_At = s_Name.find("intel_"); s_At != std::string_view::npos; s_At = s_Name.find("intel_", s_At + 1)) {
        size_t s_End = s_At + 6;

        while (s_End < s_Name.size() && std::isdigit(static_cast<unsigned char>(s_Name[s_End])))
            ++s_End;

        if (s_End > s_At + 6 && s_Name.substr(s_End, 4) == "core")
            return true;
    }

    return false;
}

std::vector<std::string> SceneDesc::MissionBricks() const {
    std::vector<std::string> s_Out;

    for (const auto& s_Brick : bricks)
        if (!IsMachineBrick(s_Brick))
            s_Out.push_back(Lower(s_Brick));

    std::sort(s_Out.begin(), s_Out.end());
    return s_Out;
}

bool SceneDesc::SameAs(const SceneDesc& p_Other) const {
    return EqualsIgnoreCase(scene, p_Other.scene) && MissionBricks() == p_Other.MissionBricks();
}

bool SceneDesc::DiffersInDifficulty(const SceneDesc& p_Other) const {
    if (!EqualsIgnoreCase(scene, p_Other.scene))
        return false;

    const auto s_Mine = MissionBricks();
    const auto s_Theirs = p_Other.MissionBricks();
    std::vector<std::string> s_Apart;
    std::set_symmetric_difference(s_Mine.begin(), s_Mine.end(), s_Theirs.begin(), s_Theirs.end(), std::back_inserter(s_Apart));

    return std::any_of(s_Apart.begin(), s_Apart.end(), [](const std::string& p_Brick) {
        return FileName(p_Brick).substr(0, 11) == "difficulty_";
    });
}

std::string SceneDesc::Location() const {
    std::string s_Lower = scene;

    for (auto& c : s_Lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static constexpr std::string_view c_Marker = "/scenes/missions/";
    const auto s_Start = s_Lower.find(c_Marker);

    if (s_Start == std::string::npos)
        return {};

    const auto s_From = s_Start + c_Marker.size();
    const auto s_End = s_Lower.find('/', s_From);

    if (s_End == std::string::npos || s_End == s_From)
        return {};

    return s_Lower.substr(s_From, s_End - s_From);
}

bool SceneDesc::SameLocationAs(const SceneDesc& p_Other) const {
    const std::string s_Mine = Location();
    return !s_Mine.empty() && s_Mine == p_Other.Location();
}

std::string LocationDisplayName(const std::string& p_Location) {
    // Folder names as they appear in scene paths (checked against the missions' scene paths in
    // Peacock's contract data).
    // Only used in messages, never to decide anything; unknown folders are shown as-is.
    static const std::pair<std::string_view, std::string_view> c_Names[] = {
        { "thefacility", "ICA Facility" }, { "paris", "Paris" }, { "coastaltown", "Sapienza" },
        { "marrakesh", "Marrakesh" }, { "bangkok", "Bangkok" }, { "colorado", "Colorado" },
        { "hokkaido", "Hokkaido" }, { "miami", "Miami" }, { "colombia", "Santa Fortuna" },
        { "mumbai", "Mumbai" }, { "skunk", "Whittleton Creek" }, { "northamerica", "Whittleton Creek" },
        { "theark", "Isle of Sgail" }, { "northsea", "Isle of Sgail" }, { "sheep", "Hawke's Bay" },
        { "hawk", "Himmelstein" }, { "salty", "Hantu Port" }, { "caged", "Siberia" }, { "snug", "the Safehouse" },
        { "greedy", "New York" }, { "opulent", "Haven Island" }, { "golden", "Dubai" },
        { "ancestral", "Dartmoor" }, { "edgy", "Berlin" }, { "wet", "Chongqing" },
        { "elegant", "Mendoza" }, { "trapped", "Carpathian Mountains" }, { "rocky", "Ambrose Island" },
    };

    for (const auto& [s_Folder, s_Name] : c_Names) {
        // Prefix match, so variants like "colorado_2" still get a name.
        if (p_Location.rfind(s_Folder, 0) == 0)
            return std::string(s_Name);
    }

    return p_Location.empty() ? std::string("an unknown location") : p_Location;
}

void WriteTransform(ByteWriter& w, const Transform& p_Xf) {
    w.F32(p_Xf.pos.x);
    w.F32(p_Xf.pos.y);
    w.F32(p_Xf.pos.z);

    const Quat s_Rot = Normalize(p_Xf.rot);
    const auto s_Quantize = [](float v) {
        return static_cast<int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
    };

    w.I16(s_Quantize(s_Rot.x));
    w.I16(s_Quantize(s_Rot.y));
    w.I16(s_Quantize(s_Rot.z));
    w.I16(s_Quantize(s_Rot.w));
}

bool ReadTransform(ByteReader& r, Transform& p_Xf) {
    int16_t s_Q[4];

    if (!r.F32(p_Xf.pos.x) || !r.F32(p_Xf.pos.y) || !r.F32(p_Xf.pos.z))
        return false;

    for (auto& s_Component : s_Q) {
        if (!r.I16(s_Component))
            return false;
    }

    // Positions in HITMAN levels stay well within a few km of the origin. Written so NaN fails too
    // (a NaN position handed to the game's physics can crash it).
    constexpr float c_MaxCoord = 100000.f;

    if (!(std::fabs(p_Xf.pos.x) <= c_MaxCoord) || !(std::fabs(p_Xf.pos.y) <= c_MaxCoord) || !(std::fabs(p_Xf.pos.z) <= c_MaxCoord))
        return r.Fail();

    p_Xf.rot = Normalize({ s_Q[0] / 32767.f, s_Q[1] / 32767.f, s_Q[2] / 32767.f, s_Q[3] / 32767.f });
    return true;
}

void MsgHello::Write(ByteWriter& w) const {
    w.U32(protocol);
    w.Str(modVersion);
    w.Str(gameVersion);
    w.Str(playerName);
}

bool MsgHello::Read(ByteReader& r) {
    return r.U32(protocol) && r.Str(modVersion, 64) && r.Str(gameVersion, 64) && r.Str(playerName, 64);
}

void MsgWelcome::Write(ByteWriter& w) const {
    w.Str(hostName);
    w.Str(gameVersion);
    w.Str(modVersion);
}

bool MsgWelcome::Read(ByteReader& r) {
    return r.Str(hostName, 64) && r.Str(gameVersion, 64) && r.Str(modVersion, 32);
}

void MsgReject::Write(ByteWriter& w) const {
    w.Str(reason);
}

bool MsgReject::Read(ByteReader& r) {
    return r.Str(reason, kMaxStringLen);
}

void MsgSceneInfo::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.Str(scene.scene);
    w.Str(scene.codename);
    w.U8(static_cast<uint8_t>(scene.bricks.size()));

    for (const auto& s_Brick : scene.bricks)
        w.Str(s_Brick);
}

bool MsgSceneInfo::Read(ByteReader& r) {
    uint8_t s_Count;

    if (!r.U32(epoch) || !r.Str(scene.scene, kMaxStringLen) || !r.Str(scene.codename, 128) || !r.U8(s_Count))
        return false;

    if (s_Count > kMaxBricks)
        return r.Fail();

    scene.bricks.resize(s_Count);

    for (auto& s_Brick : scene.bricks) {
        if (!r.Str(s_Brick, kMaxStringLen))
            return false;
    }

    return true;
}

void MsgSceneReady::Write(ByteWriter& w) const {
    w.U32(epoch);
}

bool MsgSceneReady::Read(ByteReader& r) {
    return r.U32(epoch);
}

void MsgAvatarState::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U32(timeMs);
    WriteTransform(w, xf);
}

bool MsgAvatarState::Read(ByteReader& r) {
    return r.U32(epoch) && r.U32(timeMs) && ReadTransform(r, xf);
}

void MsgVictimPose::Write(ByteWriter& w) const {
    body.Write(w);
    w.U64(id);
    WriteTransform(w, xf);
    w.U8(flags);
}

bool MsgVictimPose::Read(ByteReader& r) {
    return body.Read(r) && r.U64(id) && ReadTransform(r, xf) && r.U8(flags);
}

namespace {
int16_t Millimetres(float p_Metres) {
    return static_cast<int16_t>(std::clamp(std::lround(p_Metres * 1000.f), -32000L, 32000L));
}
}

bool MsgNpcPose::ApplyTo(Pose& p_Base) const {
    if (full) {
        p_Base.rotations = rotations;
    }
    else {
        if (p_Base.rotations.size() != boneCount || changed.size() != (boneCount + 7u) / 8u)
            return false;

        size_t s_Next = 0;

        for (size_t i = 0; i < boneCount; ++i) {
            if (!(changed[i / 8] & (1u << (i % 8))))
                continue;

            if (s_Next >= rotations.size())
                return false;

            p_Base.rotations[i] = rotations[s_Next++];
        }
    }

    p_Base.rootPos = rootPos;
    p_Base.pelvisPos = pelvisPos;
    return true;
}

void MsgNpcPose::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U32(timeMs);
    w.U64(id);

    for (const Vec3* s_Pos : { &rootPos, &pelvisPos }) {
        w.U16(static_cast<uint16_t>(Millimetres(s_Pos->x)));
        w.U16(static_cast<uint16_t>(Millimetres(s_Pos->y)));
        w.U16(static_cast<uint16_t>(Millimetres(s_Pos->z)));
    }

    const uint16_t s_Count = std::min<uint16_t>(boneCount, kMaxPoseBones);
    w.U16(s_Count);
    w.U8(full ? 1 : 0);

    if (full) {
        for (size_t i = 0; i < s_Count; ++i)
            w.U32(PackQuat(i < rotations.size() ? rotations[i] : Quat {}));

        return;
    }

    const size_t s_MaskBytes = (s_Count + 7u) / 8u;

    for (size_t i = 0; i < s_MaskBytes; ++i)
        w.U8(i < changed.size() ? changed[i] : 0);

    // As many rotations as bits set (rotations beyond that are never written).
    size_t s_Set = 0;

    for (size_t i = 0; i < s_Count; ++i)
        if (i / 8 < changed.size() && (changed[i / 8] & (1u << (i % 8))))
            ++s_Set;

    for (size_t i = 0; i < s_Set; ++i)
        w.U32(PackQuat(i < rotations.size() ? rotations[i] : Quat {}));
}

bool MsgNpcPose::Read(ByteReader& r) {
    if (!r.U32(epoch) || !r.U32(timeMs) || !r.U64(id))
        return false;

    for (Vec3* s_Pos : { &rootPos, &pelvisPos }) {
        uint16_t s_X, s_Y, s_Z;

        if (!r.U16(s_X) || !r.U16(s_Y) || !r.U16(s_Z))
            return false;

        s_Pos->x = static_cast<int16_t>(s_X) / 1000.f;
        s_Pos->y = static_cast<int16_t>(s_Y) / 1000.f;
        s_Pos->z = static_cast<int16_t>(s_Z) / 1000.f;
    }

    uint8_t s_Full = 0;

    if (!r.U16(boneCount) || !r.U8(s_Full))
        return false;

    if (boneCount > kMaxPoseBones || s_Full > 1)
        return r.Fail();

    full = s_Full != 0;
    changed.clear();
    rotations.clear();
    size_t s_Rotations = boneCount;

    if (!full) {
        changed.resize((boneCount + 7u) / 8u);
        s_Rotations = 0;

        for (auto& s_Byte : changed) {
            if (!r.U8(s_Byte))
                return false;
        }

        // Bits past the last bone must be clear.
        if (boneCount % 8 && !changed.empty() && (changed.back() >> (boneCount % 8)))
            return r.Fail();

        for (const uint8_t s_Byte : changed)
            for (int b = 0; b < 8; ++b)
                s_Rotations += (s_Byte >> b) & 1u;
    }

    rotations.resize(s_Rotations);

    for (auto& s_Rot : rotations) {
        uint32_t s_Packed = 0;

        if (!r.U32(s_Packed))
            return false;

        s_Rot = UnpackQuat(s_Packed);
    }

    return true;
}

void MsgPose::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U32(timeMs);

    for (const Vec3* s_Pos : { &pose.rootPos, &pose.pelvisPos }) {
        w.F32(s_Pos->x);
        w.F32(s_Pos->y);
        w.F32(s_Pos->z);
    }

    const size_t s_Count = std::min<size_t>(pose.rotations.size(), kMaxPoseBones);
    w.U16(static_cast<uint16_t>(s_Count));

    for (size_t i = 0; i < s_Count; ++i)
        w.U32(PackQuat(pose.rotations[i]));
}

bool MsgPose::Read(ByteReader& r) {
    if (!r.U32(epoch) || !r.U32(timeMs))
        return false;

    for (Vec3* s_Pos : { &pose.rootPos, &pose.pelvisPos }) {
        if (!r.F32(s_Pos->x) || !r.F32(s_Pos->y) || !r.F32(s_Pos->z))
            return false;

        // Root and pelvis are offsets from the character's origin: a few metres at most.
        if (!std::isfinite(s_Pos->x) || !std::isfinite(s_Pos->y) || !std::isfinite(s_Pos->z) ||
            std::fabs(s_Pos->x) > 10.f || std::fabs(s_Pos->y) > 10.f || std::fabs(s_Pos->z) > 10.f)
            return r.Fail();
    }

    uint16_t s_Count = 0;

    if (!r.U16(s_Count))
        return false;

    if (s_Count > kMaxPoseBones)
        return r.Fail();

    pose.rotations.resize(s_Count);

    for (auto& s_Rot : pose.rotations) {
        uint32_t s_Packed = 0;

        if (!r.U32(s_Packed))
            return false;

        s_Rot = UnpackQuat(s_Packed);
    }

    return true;
}

void MsgOutfit::Write(ByteWriter& w) const {
    w.Str(outfit.repositoryId);
    w.I32(outfit.charset);
    w.I32(outfit.variation);
}

bool MsgOutfit::Read(ByteReader& r) {
    if (!r.Str(outfit.repositoryId, 64) || !r.I32(outfit.charset) || !r.I32(outfit.variation))
        return false;

    if (outfit.charset < 0 || outfit.charset > 255 || outfit.variation < 0 || outfit.variation > 255)
        return r.Fail();

    return true;
}

void MsgNpcSnapshot::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U32(tick);
    w.U32(timeMs);
    w.U8(static_cast<uint8_t>(npcs.size()));

    // Rotations packed into 32 bits (as bones are): 16 bytes a transform instead of 20.
    for (const auto& s_Npc : npcs) {
        w.U64(s_Npc.id);
        w.F32(s_Npc.xf.pos.x);
        w.F32(s_Npc.xf.pos.y);
        w.F32(s_Npc.xf.pos.z);
        w.U32(PackQuat(s_Npc.xf.rot));
        w.U8(s_Npc.flags);
    }
}

bool MsgNpcSnapshot::Read(ByteReader& r) {
    uint8_t s_Count;

    if (!r.U32(epoch) || !r.U32(tick) || !r.U32(timeMs) || !r.U8(s_Count))
        return false;

    if (s_Count > kMaxNpcsPerSnapshot)
        return r.Fail();

    npcs.resize(s_Count);

    for (auto& s_Npc : npcs) {
        uint32_t s_Rot = 0;

        if (!r.U64(s_Npc.id) || !r.F32(s_Npc.xf.pos.x) || !r.F32(s_Npc.xf.pos.y) || !r.F32(s_Npc.xf.pos.z) ||
            !r.U32(s_Rot) || !r.U8(s_Npc.flags))
            return false;

        if (!(std::fabs(s_Npc.xf.pos.x) <= 1e5f) || !(std::fabs(s_Npc.xf.pos.y) <= 1e5f) || !(std::fabs(s_Npc.xf.pos.z) <= 1e5f))
            return r.Fail();

        s_Npc.xf.rot = UnpackQuat(s_Rot);

        if (s_Npc.flags & ~(NpcFlag_Alive | NpcFlag_Pacified))
            return r.Fail();
    }

    return true;
}

void MsgMark::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U64(id);
    w.F32(pos.x);
    w.F32(pos.y);
    w.F32(pos.z);
    w.U8(kind);
}

bool MsgMark::Read(ByteReader& r) {
    if (!r.U32(epoch) || !r.U64(id) || !r.F32(pos.x) || !r.F32(pos.y) || !r.F32(pos.z) || !r.U8(kind))
        return false;

    // Not NaN either: comparisons with NaN fail, so they are written to pass only for real numbers.
    if (!(std::fabs(pos.x) <= 1e5f) || !(std::fabs(pos.y) <= 1e5f) || !(std::fabs(pos.z) <= 1e5f) || kind > MarkKind_Remove)
        return r.Fail();

    return true;
}

const char* LoadStageName(uint8_t p_Stage) {
    switch (p_Stage) {
        case LoadStage_Clearing: return "leaving the mission";
        case LoadStage_Loading: return "loading";
        case LoadStage_Preparing: return "preparing the level";
        case LoadStage_Activating: return "setting up the mission";
        case LoadStage_Starting: return "starting";
        default: return "loading";
    }
}

void MsgPlayerState::Write(ByteWriter& w) const {
    w.U8(flags);
    w.U8(threat);
    w.U8(menu);
    w.U32(colour);

    for (const uint16_t s_Value : { stats.kills, stats.targets, stats.knockouts, stats.nonTargetKills, stats.spotted,
             stats.bodiesFound, stats.recorded, stats.challenges })
        w.U16(s_Value);

    w.Str(heldItem);
}

bool MsgPlayerState::Read(ByteReader& r) {
    if (!r.U8(flags) || !r.U8(threat) || !r.U8(menu) || !r.U32(colour))
        return false;

    for (uint16_t* s_Value : { &stats.kills, &stats.targets, &stats.knockouts, &stats.nonTargetKills, &stats.spotted,
             &stats.bodiesFound, &stats.recorded, &stats.challenges })
        if (!r.U16(*s_Value))
            return false;

    if (!r.Str(heldItem, 64))
        return false;

    if ((flags & ~(PlayerFlag_Trespassing | PlayerFlag_EvidenceGone | PlayerFlag_Away)) || threat >= Threat_Count || menu > 32 || colour > 0xFFFFFF)
        return r.Fail();

    return true;
}

void MsgNpcDied::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U64(id);
}

bool MsgNpcDied::Read(ByteReader& r) {
    return r.U32(epoch) && r.U64(id);
}

void MsgItem::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U64(change.id);
    w.U8(change.flags);
    WriteTransform(w, change.xf);
}

bool MsgItem::Read(ByteReader& r) {
    return r.U32(epoch) && r.U64(change.id) && r.U8(change.flags) && ReadTransform(r, change.xf);
}

void MsgAlerts::Write(ByteWriter& w) const {
    w.U32(epoch);
    const size_t s_Count = std::min<size_t>(alerts.size(), kMaxAlerts);
    w.U16(static_cast<uint16_t>(s_Count));

    for (size_t i = 0; i < s_Count; ++i) {
        w.U64(alerts[i].id);
        w.U8(alerts[i].tension);
    }
}

bool MsgAlerts::Read(ByteReader& r) {
    uint16_t s_Count;

    if (!r.U32(epoch) || !r.U16(s_Count))
        return false;

    if (s_Count > kMaxAlerts)
        return r.Fail();

    alerts.resize(s_Count);

    for (auto& s_Alert : alerts)
        if (!r.U64(s_Alert.id) || !r.U8(s_Alert.tension))
            return false;

    return true;
}

void MsgNpcCheck::Write(ByteWriter& w) const {
    w.U32(epoch);
    w.U16(round);
    w.U8(part);
    w.U8(parts);
    w.U16(total);
    const size_t s_Count = std::min(npcs.size(), kMaxNpcChecksPerMsg);
    w.U8(static_cast<uint8_t>(s_Count));

    for (size_t i = 0; i < s_Count; ++i) {
        w.U64(npcs[i].id);
        w.F32(npcs[i].pos.x);
        w.F32(npcs[i].pos.y);
        w.F32(npcs[i].pos.z);
        w.U8(npcs[i].flags);
    }
}

bool MsgNpcCheck::Read(ByteReader& r) {
    uint8_t s_Count;

    if (!r.U32(epoch) || !r.U16(round) || !r.U8(part) || !r.U8(parts) || !r.U16(total) || !r.U8(s_Count))
        return false;

    if (s_Count > kMaxNpcChecksPerMsg || parts == 0 || part >= parts)
        return r.Fail();

    npcs.resize(s_Count);

    for (auto& s_Npc : npcs) {
        if (!r.U64(s_Npc.id) || !r.F32(s_Npc.pos.x) || !r.F32(s_Npc.pos.y) || !r.F32(s_Npc.pos.z) || !r.U8(s_Npc.flags))
            return false;

        if (!std::isfinite(s_Npc.pos.x) || !std::isfinite(s_Npc.pos.y) || !std::isfinite(s_Npc.pos.z))
            return r.Fail();
    }

    return true;
}

void MsgNpcOwners::Write(ByteWriter& w) const {
    w.U32(epoch);
    const size_t s_Count = std::min(owners.size(), kMaxOwnershipsPerMsg);
    w.U8(static_cast<uint8_t>(s_Count));

    for (size_t i = 0; i < s_Count; ++i) {
        w.U64(owners[i].id);
        w.U8(owners[i].owner);
    }
}

bool MsgNpcOwners::Read(ByteReader& r) {
    uint8_t s_Count;

    if (!r.U32(epoch) || !r.U8(s_Count))
        return false;

    if (s_Count > kMaxOwnershipsPerMsg)
        return r.Fail();

    owners.resize(s_Count);

    for (auto& s_Owner : owners) {
        if (!r.U64(s_Owner.id) || !r.U8(s_Owner.owner))
            return false;

        if (s_Owner.owner > NpcOwner_Client)
            return r.Fail();
    }

    return true;
}

void MsgSkeleton::Write(ByteWriter& w) const {
    const size_t s_Count = std::min<size_t>(parents.size(), kMaxPoseBones);
    w.U16(static_cast<uint16_t>(s_Count));

    for (size_t i = 0; i < s_Count; ++i)
        w.I16(parents[i]);
}

bool MsgSkeleton::Read(ByteReader& r) {
    uint16_t s_Count;

    if (!r.U16(s_Count))
        return false;

    if (s_Count > kMaxPoseBones)
        return r.Fail();

    parents.resize(s_Count);

    for (size_t i = 0; i < s_Count; ++i) {
        if (!r.I16(parents[i]))
            return false;

        // A tree: every bone's parent comes before it.
        if (parents[i] < -1 || parents[i] >= static_cast<int16_t>(i))
            return r.Fail();
    }

    return true;
}

void MsgMissionEvent::Write(ByteWriter& w) const {
    w.Str(name);
    w.Str(json);
}

bool MsgMissionEvent::Read(ByteReader& r) {
    return r.Str(name, 128) && r.Str(json, kMaxEventJson);
}

} // namespace coop
