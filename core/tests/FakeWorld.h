#pragma once

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop/Session.h"

// Stands in for HITMAN: a player, an optional partner stand-in, and NPCs walking circles.
class FakeWorld final : public coop::IGameWorld {
public:
    explicit FakeWorld(std::string p_Name) : name(std::move(p_Name)) {}

    bool IsInScene() const override { return inScene; }
    coop::SceneDesc CurrentScene() const override { return scene; }

    bool GetLocalAvatar(coop::Transform& p_Out) const override {
        if (!inScene)
            return false;

        p_Out = player;
        return true;
    }

    bool GetLocalOutfit(coop::OutfitDesc& p_Out) const override {
        if (!inScene)
            return false;

        p_Out = outfit;
        return true;
    }

    void ShowRemoteAvatar(const coop::Transform& p_Xf) override {
        if (!remoteShown)
            ++remoteSpawns;

        remoteShown = true;
        remoteXf = p_Xf;
    }

    void HideRemoteAvatar() override {
        remoteShown = false;
        ++remoteHides;
    }

    bool GetLocalPose(coop::Pose& p_Out) override {
        if (!inScene || pose.rotations.empty())
            return false;

        p_Out = pose;
        return true;
    }

    void ShowRemotePose(const coop::Pose& p_Pose) override {
        remotePose = p_Pose;
        ++remotePoses;
    }

    bool GetLocalVictim(uint64_t& p_Id, coop::Transform& p_Xf, coop::Pose& p_Pose, uint8_t& p_Flags) override {
        if (!inScene || !victim.id)
            return false;

        p_Id = victim.id;
        p_Xf = victim.xf;
        p_Pose = victim.pose;
        p_Flags = victim.flags;
        return true;
    }

    void ShowRemoteVictim(uint64_t p_Id, const coop::Transform& p_Xf, const coop::Pose& p_Pose, uint8_t p_Flags) override {
        remoteVictim = { p_Id, p_Xf, p_Pose, p_Flags };
        ++remoteVictimPoses;
    }

    void ReleaseRemoteVictim() override {
        remoteVictim = {};
        ++remoteVictimReleases;
    }

    void SetRemoteOutfit(const coop::OutfitDesc& p_Outfit) override {
        remoteOutfit = p_Outfit;
        ++remoteOutfitChanges;
    }

    void ListNpcs(std::vector<coop::NpcState>& p_Out) const override {
        if (!inScene)
            return;

        for (const auto& [s_Id, s_Npc] : npcs)
            p_Out.push_back(s_Npc);
    }

    void SetNpcTransform(uint64_t p_Id, const coop::Transform& p_Xf) override {
        if (auto s_It = npcs.find(p_Id); s_It != npcs.end()) {
            s_It->second.xf = p_Xf;
            npcPhase[p_Id] = PhaseFromPosition(p_Id, p_Xf.pos);
            ++npcCorrections;
        }
    }

    void SetNpcPuppet(uint64_t p_Id, bool p_Puppet) override {
        if (p_Puppet)
            puppets.insert(p_Id);
        else
            puppets.erase(p_Id);
    }

    void ShowNpcPose(uint64_t p_Id, const coop::Pose& p_Pose) override { shownNpcPoses[p_Id] = p_Pose; }
    void ClearNpcPose(uint64_t p_Id) override { shownNpcPoses.erase(p_Id); }
    void WatchNpcPoses(const std::vector<uint64_t>& p_Ids) override { watchedNpcs = p_Ids; }

    bool GetNpcPose(uint64_t p_Id, coop::Pose& p_Out) override {
        const auto s_It = npcPoses.find(p_Id);

        if (s_It == npcPoses.end())
            return false;

        p_Out = s_It->second;
        return true;
    }

    void KillNpc(uint64_t p_Id) override {
        ++killCalls[p_Id];

        if (auto s_It = npcs.find(p_Id); s_It != npcs.end())
            s_It->second.flags &= ~coop::NpcFlag_Alive;
    }

    void CollectItemChanges(std::vector<coop::ItemChange>& p_Out) override {
        p_Out.insert(p_Out.end(), itemChanges.begin(), itemChanges.end());
        itemChanges.clear();
    }

    void ApplyPeerItem(const coop::ItemChange& p_Change) override { peerItems.push_back(p_Change); }
    void ResetItems() override { ++itemResets; }
    void CollectAlerts(std::vector<coop::NpcAlert>& p_Out) override { p_Out = alerts; }
    bool GetLocalSkeleton(std::vector<int16_t>& p_Out) override {
        p_Out = skeleton;
        return !skeleton.empty();
    }
    void SetRemoteSkeleton(const std::vector<int16_t>& p_Parents) override {
        remoteSkeleton = p_Parents;
        ++remoteSkeletons;
    }
    void ApplyPeerAlerts(const std::vector<coop::NpcAlert>& p_Alerts) override { peerAlerts = p_Alerts; }

    void OnPeerMissionEvent(const std::string& p_Name, const std::string& p_Json) override {
        peerEvents.push_back(p_Name + ":" + p_Json);
    }

    void Notify(const std::string& p_Text) override { notes.push_back(p_Text); }
    bool IsLocallyPaused() const override { return paused; }
    void SetPartnerPaused(bool p_Paused) override { heldPaused = p_Paused; }
    void RestartMission() override { ++restarts; }
    void KnockOutNpc(uint64_t p_Id) override {
        ++knockOuts[p_Id];

        if (auto s_It = npcs.find(p_Id); s_It != npcs.end())
            s_It->second.flags |= coop::NpcFlag_Pacified;
    }
    void OnPeerChat(const std::string& p_Text, bool p_Quick) override { chat.push_back((p_Quick ? "!" : "") + p_Text); }
    void OnPeerMark(uint64_t p_Id, const coop::Vec3& p_Pos, uint8_t p_Kind) override {
        marks.push_back({ p_Id, p_Pos });
        markKinds.push_back(p_Kind);
    }

    // Test helpers.
    void LoadScene(const coop::SceneDesc& p_Scene, int p_NpcCount, float p_PhaseOffset) {
        inScene = true;
        scene = p_Scene;
        npcs.clear();
        npcPhase.clear();

        for (int i = 0; i < p_NpcCount; ++i) {
            const uint64_t s_Id = 0x1000 + i;
            npcPhase[s_Id] = p_PhaseOffset;
            coop::NpcState s_Npc;
            s_Npc.id = s_Id;
            s_Npc.flags = coop::NpcFlag_Alive;
            s_Npc.xf.pos = NpcPosition(s_Id, p_PhaseOffset);
            npcs[s_Id] = s_Npc;
        }
    }

    void UnloadScene() {
        inScene = false;
        scene = {};
        npcs.clear();
    }

    // Every NPC walks its own circle at 1.2 m/s.
    void StepNpcs(float p_Dt) {
        for (auto& [s_Id, s_Npc] : npcs) {
            if (!s_Npc.Alive() || puppets.count(s_Id))
                continue;

            float& s_Phase = npcPhase[s_Id];
            s_Phase += p_Dt * 1.2f / Radius(s_Id);
            s_Npc.xf.pos = NpcPosition(s_Id, s_Phase);
        }
    }

    static float Radius(uint64_t p_Id) { return 5.f + static_cast<float>(p_Id % 7); }

    static coop::Vec3 Center(uint64_t p_Id) {
        const float s_I = static_cast<float>(p_Id - 0x1000);
        return { 40.f * std::fmod(s_I, 5.f), 0.f, 40.f * std::floor(s_I / 5.f) };
    }

    static coop::Vec3 NpcPosition(uint64_t p_Id, float p_Phase) {
        const coop::Vec3 s_C = Center(p_Id);
        const float s_R = Radius(p_Id);
        return { s_C.x + s_R * std::cos(p_Phase), 0.f, s_C.z + s_R * std::sin(p_Phase) };
    }

    static float PhaseFromPosition(uint64_t p_Id, coop::Vec3 p_Pos) {
        const coop::Vec3 s_C = Center(p_Id);
        return std::atan2(p_Pos.z - s_C.z, p_Pos.x - s_C.x);
    }

    std::string name;
    bool inScene = false;
    coop::SceneDesc scene;
    coop::Transform player;
    coop::OutfitDesc outfit { "fd56a934-f402-4b02-bdf7-9bf3d6ed9a2b", 0, 0 };

    bool remoteShown = false;
    coop::Transform remoteXf;
    coop::OutfitDesc remoteOutfit;
    coop::Pose pose;
    coop::Pose remotePose;
    int remotePoses = 0;

    struct Victim {
        uint64_t id = 0;
        coop::Transform xf;
        coop::Pose pose;
        uint8_t flags = 0;
    };
    Victim victim;       // the NPC our 47 is taking down (id 0: none)
    Victim remoteVictim; // the partner's, as last shown
    int remoteVictimPoses = 0;
    std::vector<coop::ItemChange> itemChanges; // ours, waiting to be collected
    std::vector<coop::ItemChange> peerItems;   // the partner's, as applied
    int itemResets = 0;
    std::vector<coop::NpcAlert> alerts;     // ours
    std::set<uint64_t> puppets;             // NPCs the partner runs (AI off here)
    bool paused = false;                    // our player has a pause menu open
    bool heldPaused = false;                // held paused for the partner
    int restarts = 0;                       // restarted for the partner
    std::map<uint64_t, int> knockOuts;      // knocked out for the partner
    std::vector<std::string> chat;          // the partner's lines ("!" first: a quick message)
    std::vector<std::pair<uint64_t, coop::Vec3>> marks;
    std::vector<uint8_t> markKinds;
    std::map<uint64_t, coop::Pose> npcPoses;      // our NPCs' poses, as GetNpcPose gives them
    std::map<uint64_t, coop::Pose> shownNpcPoses; // the partner's NPCs' poses, as shown here
    std::vector<uint64_t> watchedNpcs;
    std::vector<int16_t> skeleton;          // ours
    std::vector<int16_t> remoteSkeleton;    // the partner's, as last received
    int remoteSkeletons = 0;
    std::vector<coop::NpcAlert> peerAlerts; // the partner's, as last applied
    int remoteVictimReleases = 0;
    int remoteSpawns = 0;
    int remoteHides = 0;
    int remoteOutfitChanges = 0;

    std::map<uint64_t, coop::NpcState> npcs;
    std::map<uint64_t, float> npcPhase;
    std::map<uint64_t, int> killCalls;
    int npcCorrections = 0;

    std::vector<std::string> notes;
    std::vector<std::string> peerEvents;
};
