#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Bytes.h"
#include "Math.h"

namespace coop {

// Bump on any wire format change. Peers with different values refuse to connect.
// 2: clients only follow the host within the same location, and report their own scene.
// 9: chat, marks, loading notices, knock-outs, compact NPC snapshots.
// 10: player status (alerts, menus, mission stats), mark kinds, what a loading partner loads.
// 11: the host's mod version in Welcome, a player's colour and "away" in their status.
constexpr uint32_t kProtocolVersion = 11;

constexpr uint16_t kDefaultPort = 47047;
constexpr size_t kMaxPacketSize = 1200;
constexpr size_t kMaxMessageSize = 1100;
constexpr size_t kMaxStringLen = 512;
constexpr size_t kMaxBricks = 64;
constexpr size_t kMaxEventJson = 900;
constexpr uint16_t kMaxNpcsPerSnapshot = 32;
constexpr uint16_t kMaxAlerts = 160;
// 4 bytes a bone; HITMAN's human rig has 231.
constexpr uint16_t kMaxPoseBones = 256;

enum class MsgType : uint8_t {
    Hello = 1,
    Welcome,
    Reject,
    Disconnect,
    SceneInfo,
    SceneReady,
    AvatarState,
    Outfit,
    NpcSnapshot,
    NpcDied,
    NpcKillRequest,
    MissionEvent,
    Pose,
    VictimPose,
    Item,
    Alerts,
    Skeleton,
    NpcOwners,
    NpcPose,
    Pause,
    Restart,
    Chat,
    Mark,
    Busy,
    NpcKnockedOut,
    PlayerState,
    Count,
};

const char* MsgTypeName(MsgType p_Type);

struct SceneDesc {
    std::string scene;
    std::vector<std::string> bricks;
    std::string codename;

    bool SameAs(const SceneDesc& p_Other) const;
    bool Empty() const { return scene.empty(); }

    // The mission's location folder, lowercase: "paris" for ".../scenes/missions/paris/...".
    // Empty when the scene isn't under scenes/missions/.
    std::string Location() const;

    // Both scenes are at the same, known location. Every mission at a location comes with that
    // location, so a player who could start one mission there owns the others too.
    bool SameLocationAs(const SceneDesc& p_Other) const;
};

// "Sapienza" for "coastaltown"; falls back to the folder name.
std::string LocationDisplayName(const std::string& p_Location);

struct OutfitDesc {
    std::string repositoryId; // GUID string of the global outfit kit
    int32_t charset = 0;
    int32_t variation = 0;

    bool operator==(const OutfitDesc& p_Other) const {
        return repositoryId == p_Other.repositoryId && charset == p_Other.charset && variation == p_Other.variation;
    }

    bool operator!=(const OutfitDesc& p_Other) const { return !(*this == p_Other); }
};

enum NpcFlags : uint8_t {
    NpcFlag_Alive = 1 << 0,
    NpcFlag_Pacified = 1 << 1,
};

struct NpcState {
    uint64_t id = 0; // entity id from the scene template, identical on both machines for the same scene
    Transform xf;
    uint8_t flags = 0;
    uint8_t tension = 0; // its own alert level (EGameTension) in the lister's game; not sent

    bool Alive() const { return (flags & NpcFlag_Alive) != 0; }
};

struct MsgHello {
    uint32_t protocol = kProtocolVersion;
    std::string modVersion;
    std::string gameVersion;
    std::string playerName;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgWelcome {
    std::string hostName;
    std::string gameVersion;
    std::string modVersion;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgReject {
    std::string reason;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgSceneInfo {
    uint32_t epoch = 0;
    SceneDesc scene;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgSceneReady {
    uint32_t epoch = 0;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgAvatarState {
    uint32_t epoch = 0;
    uint32_t timeMs = 0; // sender clock
    Transform xf;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// The sender's 47's skeleton pose, sent unreliably at the avatar rate.
struct MsgPose {
    uint32_t epoch = 0;
    uint32_t timeMs = 0; // sender clock, same as MsgAvatarState
    Pose pose;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// An NPC the sender's 47 is taking down or dragging: its pose and place, so the other machine
// plays the same choke, kill or drag on its copy of that NPC. Unreliable, at the avatar rate.
struct MsgVictimPose {
    MsgPose body; // epoch, sender time and the NPC's pose
    uint64_t id = 0;
    Transform xf;
    uint8_t flags = 0; // NpcFlag_* as the sender sees the NPC

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgOutfit {
    OutfitDesc outfit;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgNpcSnapshot {
    uint32_t epoch = 0;
    uint32_t tick = 0;
    uint32_t timeMs = 0; // sender clock, for playing it back smoothly
    std::vector<NpcState> npcs;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

struct MsgNpcDied {
    uint32_t epoch = 0;
    uint64_t id = 0;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

enum ItemFlags : uint8_t {
    ItemFlag_Taken = 1 << 0, // in someone's hands or pockets, so gone from the world
    // Not an item but a door (id: its scene key), opened or closed by that player.
    ItemFlag_Door = 1 << 1,
    ItemFlag_DoorOpen = 1 << 2,
    ItemFlag_DoorFromFront = 1 << 3, // the player was on the door's +Y side
};

// An item from the scene (same entity id on both machines) that a player took, or put back into
// the world (dropped, placed, thrown: then sent again as it moves, until it settles).
struct ItemChange {
    uint64_t id = 0;
    uint8_t flags = 0;
    Transform xf;

    bool Taken() const { return (flags & ItemFlag_Taken) != 0; }
};

struct MsgItem {
    uint32_t epoch = 0;
    ItemChange change;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// An NPC that is alert in the sender's game, by its own doing there: how alert (EGameTension,
// above ambient). The receiver keeps its copy at least that alert.
struct NpcAlert {
    uint64_t id = 0;
    uint8_t tension = 0;
};

// Every NPC alert in the sender's game, a few times a second (unreliable: the next one replaces it).
struct MsgAlerts {
    uint32_t epoch = 0;
    std::vector<NpcAlert> alerts;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// The sender's 47's skeleton: each bone's parent (-1 for the root), the layout its poses are in.
// It changes with some outfits, so it is sent again whenever it does (reliable).
struct MsgSkeleton {
    std::vector<int16_t> parents;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// Shared NPCs: each one is run by one player's game, its owner (whoever is nearer, decided by the
// host), and played as a puppet in the other's.
enum NpcOwner : uint8_t {
    NpcOwner_Host = 0,
    NpcOwner_Client = 1,
};

struct NpcOwnership {
    uint64_t id = 0;
    uint8_t owner = NpcOwner_Host;
};

constexpr size_t kMaxOwnershipsPerMsg = 96;

// Host -> client: owners that changed (an NPC not listed yet is the host's). Reliable.
struct MsgNpcOwners {
    uint32_t epoch = 0;
    std::vector<NpcOwnership> owners;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// The sender's player has a pause menu open (1) or closed it (0). Reliable.
struct MsgPause {
    uint8_t paused = 0;

    void Write(ByteWriter& w) const { w.U8(paused); }
    bool Read(ByteReader& r) { return r.U8(paused) && paused <= 1; }
};

// The sender restarted this mission (the scene); the receiver restarts it too. Reliable.
struct MsgRestart {
    std::string scene;

    void Write(ByteWriter& w) const { w.Str(scene); }
    bool Read(ByteReader& r) { return r.Str(scene, 256); }
};

// A line of chat (or a quick message) from the sender's player. Reliable.
constexpr size_t kMaxChatLen = 160;

struct MsgChat {
    uint8_t quick = 0; // 1: one of the quick messages (shown highlighted)
    std::string text;

    void Write(ByteWriter& w) const { w.U8(quick); w.Str(text); }
    bool Read(ByteReader& r) { return r.U8(quick) && quick <= 1 && r.Str(text, kMaxChatLen); }
};

// The sender's player marked an NPC (by id) or, with id 0, a place, for the receiver to see. Reliable.
enum MarkKind : uint8_t {
    MarkKind_Normal = 0,
    MarkKind_Danger = 1, // "watch out" (marked twice in a row)
    MarkKind_Remove = 2, // the sender took their mark back
};

struct MsgMark {
    uint32_t epoch = 0;
    uint64_t id = 0;
    Vec3 pos;
    uint8_t kind = MarkKind_Normal;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// The sender's game is about to stall (loading or clearing a mission) for up to that many seconds,
// so the receiver waits that long before calling it timed out; 0: busy no more. Reliable, flushed
// at once. With the loading stage it is at (LoadStage_*) and the scene it loads, if it knows.
enum LoadStage : uint8_t {
    LoadStage_None = 0,
    LoadStage_Clearing,
    LoadStage_Loading,
    LoadStage_Preparing,
    LoadStage_Activating,
    LoadStage_Starting,
    LoadStage_Count,
};

const char* LoadStageName(uint8_t p_Stage);

struct MsgBusy {
    uint16_t seconds = 0;
    uint8_t stage = LoadStage_None;
    std::string scene;

    void Write(ByteWriter& w) const { w.U16(seconds); w.U8(stage); w.Str(scene); }
    bool Read(ByteReader& r) { return r.U16(seconds) && seconds <= 600 && r.U8(stage) && stage < LoadStage_Count && r.Str(scene, 256); }
};

// What the sender's player is up to, and their mission so far: their part of the co-op HUD, the
// team's Silent Assassin watch and the debrief. Unreliable, a few times a second.
enum PlayerFlags : uint8_t {
    PlayerFlag_Trespassing = 1 << 0,
    PlayerFlag_EvidenceGone = 1 << 1, // camera footage of them destroyed or erased
    PlayerFlag_Away = 1 << 2,         // their game isn't the window in front (they tabbed out)
};

// The NPCs' alert level around the player, as the game names it ("Ambient", "AlertedLow", ...).
enum Threat : uint8_t {
    Threat_Calm = 0,
    Threat_AlertedLow,
    Threat_AlertedHigh,
    Threat_Hunted,
    Threat_Arrest,
    Threat_Combat,
    Threat_Count,
};

struct PlayerStats {
    uint16_t kills = 0;          // everyone they killed (targets too)
    uint16_t targets = 0;
    uint16_t knockouts = 0;
    uint16_t nonTargetKills = 0;
    uint16_t spotted = 0;        // times an NPC spotted them
    uint16_t bodiesFound = 0;
    uint16_t recorded = 0;       // times a security camera caught them
    uint16_t challenges = 0;

    bool operator==(const PlayerStats&) const = default;
};

struct MsgPlayerState {
    uint8_t flags = 0;
    uint8_t threat = Threat_Calm;
    uint8_t menu = 0;            // the game menu they have open (EGameUIMenu + 1), 0: none
    uint32_t colour = 0;         // the colour they picked for themselves (0xRRGGBB), 0: the default
    PlayerStats stats;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
    bool operator==(const MsgPlayerState&) const = default;
};

struct MsgMissionEvent {
    std::string name;
    std::string json;

    void Write(ByteWriter& w) const;
    bool Read(ByteReader& r);
};

// Quaternion components quantized to int16, renormalized on read.
void WriteTransform(ByteWriter& w, const Transform& p_Xf);
bool ReadTransform(ByteReader& r, Transform& p_Xf);

// Decode a message payload into T. Fails on trailing garbage too.
template <class T>
bool Decode(const std::vector<uint8_t>& p_Payload, T& p_Out) {
    ByteReader s_Reader(p_Payload);
    return p_Out.Read(s_Reader) && s_Reader.AtEnd();
}

template <class T>
std::vector<uint8_t> Encode(const T& p_Msg) {
    ByteWriter s_Writer;
    p_Msg.Write(s_Writer);
    return std::move(s_Writer.Data());
}

} // namespace coop
