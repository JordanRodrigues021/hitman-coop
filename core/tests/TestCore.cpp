#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "SimNetwork.h"
#include "TestFramework.h"
#include "coop/Connection.h"
#include "coop/Interpolation.h"
#include "coop/Protocol.h"

using namespace coop;

TEST(Bytes_RoundTrip) {
    ByteWriter w;
    w.U8(0xAB);
    w.U16(0xBEEF);
    w.U32(0xDEADBEEF);
    w.U64(0x0123456789ABCDEFull);
    w.I16(-1234);
    w.I32(-123456789);
    w.F32(3.5f);
    w.Bool(true);
    w.Str("assembly:/_pro/scenes/missions/miami/scene_flamingo.entity");

    ByteReader r(w.Data());
    uint8_t a; uint16_t b; uint32_t c; uint64_t d; int16_t e; int32_t f; float g; bool h; std::string s;

    CHECK(r.U8(a) && a == 0xAB);
    CHECK(r.U16(b) && b == 0xBEEF);
    CHECK(r.U32(c) && c == 0xDEADBEEF);
    CHECK(r.U64(d) && d == 0x0123456789ABCDEFull);
    CHECK(r.I16(e) && e == -1234);
    CHECK(r.I32(f) && f == -123456789);
    CHECK(r.F32(g) && g == 3.5f);
    CHECK(r.Bool(h) && h);
    CHECK(r.Str(s, 512) && s == "assembly:/_pro/scenes/missions/miami/scene_flamingo.entity");
    CHECK(r.AtEnd());
}

TEST(Bytes_ReaderFailsSafely) {
    const uint8_t s_Data[] = { 1, 2, 3 };
    ByteReader r(s_Data, sizeof(s_Data));
    uint32_t v;
    CHECK(!r.U32(v));
    CHECK(!r.Ok());

    uint8_t b;
    CHECK(!r.U8(b)); // stays failed

    // String longer than the limit.
    ByteWriter w;
    w.Str(std::string(100, 'x'));
    ByteReader r2(w.Data());
    std::string s;
    CHECK(!r2.Str(s, 99));

    // String length pointing past the end.
    const uint8_t s_Bad[] = { 0xFF, 0x00, 'a' };
    ByteReader r3(s_Bad, sizeof(s_Bad));
    CHECK(!r3.Str(s, 1000));
}

TEST(Bytes_RejectsNonFiniteFloats) {
    for (float s_Bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity() }) {
        ByteWriter w;
        w.F32(s_Bad);
        ByteReader r(w.Data());
        float f;
        CHECK(!r.F32(f));
    }
}

TEST(Protocol_RoundTripAll) {
    {
        MsgHello m { kProtocolVersion, "0.1.0", "3.240.1", "Alice" }, o;
        CHECK(Decode(Encode(m), o) && o.protocol == m.protocol && o.modVersion == "0.1.0" && o.gameVersion == "3.240.1" &&
            o.playerName == "Alice");
    }
    {
        MsgSceneInfo m, o;
        m.epoch = 7;
        m.scene.scene = "assembly:/_pro/scenes/missions/paris/_scene_paris.entity";
        m.scene.bricks = { "assembly:/_pro/scenes/missions/paris/mission_peacock.brick", "b2" };
        m.scene.codename = "peacock";
        CHECK(Decode(Encode(m), o) && o.epoch == 7 && o.scene.SameAs(m.scene) && o.scene.codename == "peacock");
    }
    {
        MsgAvatarState m, o;
        m.epoch = 3;
        m.timeMs = 99;
        m.xf.pos = { 1.5f, -2.25f, 30.f };
        m.xf.rot = Normalize({ 0.1f, 0.7f, -0.2f, 0.6f });
        CHECK(Decode(Encode(m), o));
        CHECK(o.epoch == 3 && o.timeMs == 99);
        CHECK(Distance(o.xf.pos, m.xf.pos) < 1e-6f);
        CHECK(AngleBetween(o.xf.rot, m.xf.rot) < 0.001f);
    }
    {
        MsgNpcSnapshot m, o;
        m.epoch = 2;
        m.tick = 5;

        for (uint16_t i = 0; i < kMaxNpcsPerSnapshot; ++i) {
            NpcState s;
            s.id = 0xFEEDE71590000000ull + i;
            s.xf.pos = { static_cast<float>(i), 1.f, 2.f };
            s.flags = NpcFlag_Alive;
            m.npcs.push_back(s);
        }

        const auto s_Bytes = Encode(m);
        CHECK(s_Bytes.size() <= kMaxMessageSize);
        CHECK(Decode(s_Bytes, o) && o.npcs.size() == kMaxNpcsPerSnapshot && o.npcs[31].id == m.npcs[31].id);

        m.npcs.push_back(m.npcs[0]); // one over the cap
        CHECK(!Decode(Encode(m), o));
    }
    {
        MsgOutfit m { { "fd56a934-f402-4b02-bdf7-9bf3d6ed9a2b", 2, 1 } }, o;
        CHECK(Decode(Encode(m), o) && o.outfit == m.outfit);

        m.outfit.charset = -1;
        CHECK(!Decode(Encode(m), o));
    }
    {
        MsgNpcDied m { 4, 0x1234 }, o;
        CHECK(Decode(Encode(m), o) && o.epoch == 4 && o.id == 0x1234);
    }
    {
        MsgMissionEvent m { "ContractEnd", "{\"x\":1}" }, o;
        CHECK(Decode(Encode(m), o) && o.name == m.name && o.json == m.json);
    }
    {
        MsgChat m { 1, "Wait for me" }, o;
        CHECK(Decode(Encode(m), o) && o.quick == 1 && o.text == m.text);
        m.text.assign(kMaxChatLen + 1, 'x');
        CHECK(!Decode(Encode(m), o));
    }
    {
        MsgMark m { 3, 0x1002, { 1.f, -2.f, 3.5f } }, o;
        CHECK(Decode(Encode(m), o) && o.epoch == 3 && o.id == 0x1002 && Distance(o.pos, m.pos) < 1e-6f);
        m.pos.x = 1e7f;
        CHECK(!Decode(Encode(m), o));
    }
    {
        MsgBusy m { 120 }, o;
        CHECK(Decode(Encode(m), o) && o.seconds == 120 && o.stage == LoadStage_None && o.scene.empty());
        m = { 60, LoadStage_Preparing, "assembly:/_pro/scenes/missions/paris/_scene_paris.entity" };
        CHECK(Decode(Encode(m), o) && o.stage == LoadStage_Preparing && o.scene == m.scene);
        m.stage = LoadStage_Count;
        CHECK(!Decode(Encode(m), o));
    }
    {
        // Marks: their kind, and never a NaN or far-off place.
        MsgMark m { 3, 7, { 1.f, 2.f, 3.f }, MarkKind_Danger }, o;
        CHECK(Decode(Encode(m), o) && o.kind == MarkKind_Danger);
        m.kind = 9;
        CHECK(!Decode(Encode(m), o));
        m.kind = MarkKind_Remove;
        m.pos.y = std::numeric_limits<float>::quiet_NaN();
        CHECK(!Decode(Encode(m), o));
    }
    {
        // A NaN position anywhere a transform is read is refused (the game's physics can't take one).
        MsgAvatarState m, o;
        m.xf.pos = { 1.f, std::numeric_limits<float>::quiet_NaN(), 0.f };
        CHECK(!Decode(Encode(m), o));
        m.xf.pos = { 1.f, std::numeric_limits<float>::infinity(), 0.f };
        CHECK(!Decode(Encode(m), o));
        MsgNpcSnapshot s, so;
        NpcState n;
        n.xf.pos = { std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f };
        s.npcs.push_back(n);
        CHECK(!Decode(Encode(s), so));
    }
    {
        MsgPlayerState m, o;
        m.flags = PlayerFlag_Trespassing;
        m.threat = Threat_Hunted;
        m.menu = 4;
        m.stats.kills = 3;
        m.stats.challenges = 65535;
        m.colour = 0x3080FF;
        m.flags |= PlayerFlag_Away;
        CHECK(Decode(Encode(m), o) && o == m);
        m.colour = 0x1000000;
        CHECK(!Decode(Encode(m), o));
        m.colour = 0;
        m.threat = Threat_Count;
        CHECK(!Decode(Encode(m), o));
        m.threat = 0;
        m.flags = 0x80;
        CHECK(!Decode(Encode(m), o));
    }
    {
        // NPC snapshots pack rotations into 32 bits: still well within a degree.
        MsgNpcSnapshot m, o;
        NpcState s;
        s.id = 7;
        s.xf.pos = { -310.25f, 12.5f, 4.f };
        s.xf.rot = Normalize({ 0.f, 0.f, 0.38f, 0.92f });
        s.flags = NpcFlag_Alive | NpcFlag_Pacified;
        m.npcs.push_back(s);
        CHECK(Decode(Encode(m), o) && o.npcs.size() == 1 && Distance(o.npcs[0].xf.pos, s.xf.pos) < 1e-6f);
        CHECK(AngleBetween(o.npcs[0].xf.rot, s.xf.rot) < 0.01f && o.npcs[0].flags == s.flags);
    }
}

TEST(Math_QuatPackingIsAccurate) {
    std::mt19937 s_Rng(7);
    std::uniform_real_distribution<float> s_Dist(-1.f, 1.f);
    float s_Worst = 0.f;

    for (int i = 0; i < 20000; ++i) {
        const Quat s_Q = Normalize({ s_Dist(s_Rng), s_Dist(s_Rng), s_Dist(s_Rng), s_Dist(s_Rng) });
        s_Worst = std::max(s_Worst, AngleBetween(s_Q, UnpackQuat(PackQuat(s_Q))));
    }

    // Identity and its negation survive exactly enough.
    CHECK(AngleBetween(UnpackQuat(PackQuat({ 0.f, 0.f, 0.f, 1.f })), {}) < 0.002f);
    CHECK(AngleBetween(UnpackQuat(PackQuat({ 0.f, 0.f, 0.f, -1.f })), {}) < 0.002f);
    CHECK_MSG(s_Worst < 0.25f * 3.14159265f / 180.f, "worst error %.3f degrees", s_Worst * 180.f / 3.14159265f);
}

TEST(Protocol_PoseFitsAndRoundTrips) {
    std::mt19937 s_Rng(3);
    std::uniform_real_distribution<float> s_Dist(-1.f, 1.f);
    MsgPose m, o;
    m.epoch = 9;
    m.timeMs = 1234;
    m.pose.rootPos = { 0.f, 0.f, 0.02f };
    m.pose.pelvisPos = { 0.01f, -0.03f, 0.95f };

    for (int i = 0; i < 231; ++i)
        m.pose.rotations.push_back(Normalize({ s_Dist(s_Rng), s_Dist(s_Rng), s_Dist(s_Rng), s_Dist(s_Rng) }));

    const auto s_Bytes = Encode(m);
    CHECK_MSG(s_Bytes.size() <= kMaxMessageSize, "%zu bytes", s_Bytes.size());
    REQUIRE(Decode(s_Bytes, o));
    CHECK(o.epoch == 9 && o.timeMs == 1234 && o.pose.rotations.size() == 231);
    CHECK(Distance(o.pose.pelvisPos, m.pose.pelvisPos) < 1e-6f);

    float s_Worst = 0.f;
    for (size_t i = 0; i < 231; ++i)
        s_Worst = std::max(s_Worst, AngleBetween(o.pose.rotations[i], m.pose.rotations[i]));
    CHECK(s_Worst < 0.004f);

    // The largest rig still fits; a pelvis kilometres away doesn't decode.
    m.pose.rotations.resize(kMaxPoseBones);
    CHECK(Encode(m).size() <= kMaxMessageSize);
    m.pose.pelvisPos.z = 5000.f;
    CHECK(!Decode(Encode(m), o));
}

TEST(Protocol_VictimPoseFitsAndRoundTrips) {
    MsgVictimPose m, o;
    m.body.epoch = 4;
    m.body.timeMs = 77;
    m.body.pose.rotations.assign(231, Quat {});
    m.id = 0x1234567890abcdefull;
    m.xf.pos = { 3.f, -2.f, 1.f };
    m.flags = NpcFlag_Pacified;

    const auto s_Bytes = Encode(m);
    CHECK_MSG(s_Bytes.size() <= kMaxMessageSize, "%zu bytes", s_Bytes.size());
    REQUIRE(Decode(s_Bytes, o));
    CHECK(o.id == m.id && o.flags == NpcFlag_Pacified && o.body.pose.rotations.size() == 231);
    CHECK(Distance(o.xf.pos, m.xf.pos) < 1e-6f);
}

TEST(Protocol_RejectsTrailingBytes) {
    auto s_Bytes = Encode(MsgSceneReady { 5 });
    s_Bytes.push_back(0);
    MsgSceneReady o;
    CHECK(!Decode(s_Bytes, o));
}

TEST(Protocol_RejectsAbsurdPositions) {
    MsgAvatarState m;
    m.xf.pos = { 1e9f, 0.f, 0.f };
    MsgAvatarState o;
    CHECK(!Decode(Encode(m), o));
}

TEST(Protocol_SceneCompareIgnoresCase) {
    SceneDesc a { "assembly:/_PRO/Scenes/Missions/Miami/scene_flamingo.entity", { "assembly:/_PRO/a.brick" }, "" };
    SceneDesc b { "assembly:/_pro/scenes/missions/miami/scene_flamingo.entity", { "assembly:/_pro/a.brick" }, "x" };
    CHECK(a.SameAs(b));
    b.bricks.push_back("extra");
    CHECK(!a.SameAs(b));
}

template <class T>
static void FuzzDecoder(std::mt19937& p_Rng, const std::vector<uint8_t>& p_Seed, int p_Iterations) {
    std::uniform_int_distribution<int> s_Byte(0, 255);

    for (int i = 0; i < p_Iterations; ++i) {
        std::vector<uint8_t> s_Data = p_Seed;
        const int s_Mutations = 1 + static_cast<int>(p_Rng() % 8);

        for (int m = 0; m < s_Mutations; ++m) {
            switch (p_Rng() % 4) {
                case 0:
                    if (!s_Data.empty())
                        s_Data[p_Rng() % s_Data.size()] = static_cast<uint8_t>(s_Byte(p_Rng));
                    break;
                case 1:
                    if (!s_Data.empty())
                        s_Data.resize(p_Rng() % s_Data.size());
                    break;
                case 2:
                    s_Data.push_back(static_cast<uint8_t>(s_Byte(p_Rng)));
                    break;
                default:
                    s_Data.insert(s_Data.begin() + (s_Data.empty() ? 0 : p_Rng() % s_Data.size()),
                        static_cast<uint8_t>(s_Byte(p_Rng)));
                    break;
            }
        }

        T s_Out;
        Decode(s_Data, s_Out); // must not crash or overread; result doesn't matter
    }
}

TEST(Fuzz_AllDecoders) {
    std::mt19937 s_Rng(1234);
    constexpr int c_Iterations = 20000;

    MsgSceneInfo s_Scene;
    s_Scene.scene.scene = "assembly:/_pro/scenes/missions/paris/_scene_paris.entity";
    s_Scene.scene.bricks = { "a", "b", "c" };

    MsgNpcSnapshot s_Snapshot;
    s_Snapshot.npcs.resize(5);

    FuzzDecoder<MsgHello>(s_Rng, Encode(MsgHello { 1, "0.1.0", "3.240.1", "Bob" }), c_Iterations);
    FuzzDecoder<MsgWelcome>(s_Rng, Encode(MsgWelcome { "Host", "3.240.1" }), c_Iterations);
    FuzzDecoder<MsgReject>(s_Rng, Encode(MsgReject { "nope" }), c_Iterations);
    FuzzDecoder<MsgSceneInfo>(s_Rng, Encode(s_Scene), c_Iterations);
    FuzzDecoder<MsgSceneReady>(s_Rng, Encode(MsgSceneReady { 1 }), c_Iterations);
    FuzzDecoder<MsgAvatarState>(s_Rng, Encode(MsgAvatarState {}), c_Iterations);
    {
        MsgPose s_Pose;
        s_Pose.pose.rotations.resize(40);
        FuzzDecoder<MsgPose>(s_Rng, Encode(s_Pose), c_Iterations);
    }
    FuzzDecoder<MsgOutfit>(s_Rng, Encode(MsgOutfit {}), c_Iterations);
    FuzzDecoder<MsgNpcSnapshot>(s_Rng, Encode(s_Snapshot), c_Iterations);
    FuzzDecoder<MsgNpcDied>(s_Rng, Encode(MsgNpcDied { 1, 2 }), c_Iterations);
    FuzzDecoder<MsgMissionEvent>(s_Rng, Encode(MsgMissionEvent { "Kill", "{}" }), c_Iterations);
    FuzzDecoder<MsgChat>(s_Rng, Encode(MsgChat { 0, "hello there" }), c_Iterations);
    FuzzDecoder<MsgMark>(s_Rng, Encode(MsgMark { 1, 2, { 3.f, 4.f, 5.f } }), c_Iterations);
    FuzzDecoder<MsgBusy>(s_Rng, Encode(MsgBusy { 60, LoadStage_Loading, "scene" }), c_Iterations);
    FuzzDecoder<MsgPlayerState>(s_Rng, Encode(MsgPlayerState { PlayerFlag_Trespassing, 2, 3, {} }), c_Iterations);
}

// Drives two Connections over the simulated network.
struct ConnectionPair {
    ConnectionPair(uint32_t p_Seed, uint16_t p_Seq, uint16_t p_RelId) :
        net(SimNetwork::Create(p_Seed)),
        sa(net->Open({ 1, 1 })),
        sb(net->Open({ 2, 2 })),
        a({ 2, 2 }, p_Seq, p_RelId),
        b({ 1, 1 }, static_cast<uint16_t>(p_Seq + 31000), p_RelId) {}

    void Step(double p_Now) {
        net->SetTime(p_Now);
        Pump(*sa, a, p_Now, inboxA);
        Pump(*sb, b, p_Now, inboxB);

        std::vector<std::vector<uint8_t>> s_Out;
        a.Flush(p_Now, s_Out);
        for (auto& d : s_Out) sa->SendTo({ 2, 2 }, d.data(), d.size());

        s_Out.clear();
        b.Flush(p_Now, s_Out);
        for (auto& d : s_Out) sb->SendTo({ 1, 1 }, d.data(), d.size());
    }

    static void Pump(IDatagramSocket& p_Sock, Connection& p_Conn, double p_Now, std::vector<Message>& p_Inbox) {
        uint8_t s_Buf[2048];
        Endpoint s_From;
        int n;

        while ((n = p_Sock.RecvFrom(s_From, s_Buf, sizeof(s_Buf))) >= 0) {
            p_Conn.Receive(s_Buf, static_cast<size_t>(n), p_Now);
            Message m;
            while (p_Conn.PopMessage(m)) p_Inbox.push_back(std::move(m));
        }
    }

    std::shared_ptr<SimNetwork> net;
    std::unique_ptr<IDatagramSocket> sa, sb;
    Connection a, b;
    std::vector<Message> inboxA, inboxB;
};

static void RunReliableChaos(uint32_t p_Seed, uint16_t p_Seq, uint16_t p_RelId, double p_Loss, int p_Count) {
    ConnectionPair p(p_Seed, p_Seq, p_RelId);
    p.net->config = { 0.01, 0.2, p_Loss, 0.1, false };

    int s_Sent = 0;
    int s_Unreliable = 0;
    double t = 0.0;

    for (; t < 120.0; t += 1.0 / 60.0) {
        // Feed a few messages per tick in both directions, mixed with unreliable traffic.
        for (int k = 0; k < 4 && s_Sent < p_Count; ++k, ++s_Sent) {
            ByteWriter w;
            w.U32(static_cast<uint32_t>(s_Sent));
            p.a.SendReliable(MsgType::MissionEvent, w.Data());
            p.b.SendReliable(MsgType::MissionEvent, w.Data());
        }

        ByteWriter u;
        u.U32(static_cast<uint32_t>(s_Unreliable++));
        p.a.SendUnreliable(MsgType::AvatarState, u.Data());

        p.Step(t);

        if (s_Sent == p_Count && p.a.Stats().reliablePending == 0 && p.b.Stats().reliablePending == 0)
            break;
    }

    const auto s_Check = [&](const std::vector<Message>& p_Inbox, const char* p_Side) {
        uint32_t s_Expected = 0;
        bool s_InOrder = true;
        int64_t s_LastUnreliable = -1;
        bool s_UnreliableMonotonic = true;

        for (const auto& m : p_Inbox) {
            ByteReader r(m.payload);
            uint32_t v = 0;
            r.U32(v);

            if (m.type == MsgType::MissionEvent) {
                if (v != s_Expected) s_InOrder = false;
                ++s_Expected;
            }
            else {
                if (static_cast<int64_t>(v) <= s_LastUnreliable) s_UnreliableMonotonic = false;
                s_LastUnreliable = v;
            }
        }

        CHECK_MSG(s_InOrder && s_Expected == static_cast<uint32_t>(p_Count), "%s got %u of %d reliable, in order=%d",
            p_Side, s_Expected, p_Count, s_InOrder);
        CHECK_MSG(s_UnreliableMonotonic, "%s saw stale unreliable messages", p_Side);
    };

    s_Check(p.inboxB, "B");
    s_Check(p.inboxA, "A");
    CHECK_MSG(t < 120.0, "did not drain within 120 s (pending a=%zu b=%zu)", p.a.Stats().reliablePending,
        p.b.Stats().reliablePending);
}

TEST(Connection_ReliableInOrderUnderChaos) {
    RunReliableChaos(1, 0, 0, 0.25, 3000);
}

TEST(Connection_ReliableSurvivesSequenceWrap) {
    RunReliableChaos(2, 65400, 64000, 0.2, 4000);
}

TEST(Connection_ReliableUnderHeavyLoss) {
    RunReliableChaos(3, 100, 100, 0.5, 600);
}

TEST(Connection_MeasuresRtt) {
    ConnectionPair p(4, 0, 0);
    p.net->config = { 0.05, 0.05, 0.0, 0.0, false };

    for (double t = 0.0; t < 3.0; t += 1.0 / 60.0)
        p.Step(t);

    // 50 ms each way plus up to one tick of ack delay on each side.
    const double s_Rtt = p.a.Stats().rttMs;
    CHECK_MSG(s_Rtt > 95.0 && s_Rtt < 140.0, "rtt %.1f ms", s_Rtt);
}

TEST(Connection_RejectsOversizedMessages) {
    Connection c({ 1, 1 });
    CHECK(!c.SendReliable(MsgType::MissionEvent, std::vector<uint8_t>(kMaxMessageSize + 1)));
    CHECK(c.SendReliable(MsgType::MissionEvent, std::vector<uint8_t>(kMaxMessageSize)));

    std::vector<std::vector<uint8_t>> s_Out;
    c.Flush(0.0, s_Out);
    REQUIRE(!s_Out.empty());

    for (const auto& d : s_Out)
        CHECK(d.size() <= kMaxPacketSize);
}

TEST(Fuzz_ConnectionReceive) {
    std::mt19937 s_Rng(99);

    // Real packets to mutate.
    Connection s_Sender({ 1, 1 });
    for (int i = 0; i < 20; ++i) {
        s_Sender.SendReliable(MsgType::Outfit, Encode(MsgOutfit {}));
        s_Sender.SendUnreliable(MsgType::AvatarState, Encode(MsgAvatarState {}));
    }

    std::vector<std::vector<uint8_t>> s_Seeds;
    s_Sender.Flush(0.0, s_Seeds);
    REQUIRE(!s_Seeds.empty());

    Connection s_Victim({ 2, 2 });

    for (int i = 0; i < 100000; ++i) {
        std::vector<uint8_t> d = s_Seeds[s_Rng() % s_Seeds.size()];

        if (i % 3 == 0) {
            d.resize(s_Rng() % 64);
            for (auto& b : d) b = static_cast<uint8_t>(s_Rng());
            if (d.size() >= 2) { d[0] = kPacketMagic & 0xFF; d[1] = kPacketMagic >> 8; }
        }
        else {
            const int s_Mut = 1 + static_cast<int>(s_Rng() % 6);
            for (int m = 0; m < s_Mut && !d.empty(); ++m) d[2 + s_Rng() % (d.size() - 2)] = static_cast<uint8_t>(s_Rng());
            if (s_Rng() % 4 == 0) d.resize(s_Rng() % (d.size() + 1));
        }

        s_Victim.Receive(d.data(), d.size(), i * 0.001);

        Message m;
        while (s_Victim.PopMessage(m)) {
            CHECK(m.payload.size() <= kMaxMessageSize);
        }
    }

    CHECK(s_Victim.Stats().packetsMalformed > 0);
}

TEST(Interpolation_PlaysBackWithDelay) {
    SnapshotBuffer b(0.1);
    Transform x0, x1;
    x0.pos = { 0, 0, 0 };
    x1.pos = { 10, 0, 0 };

    Transform out;
    CHECK(!b.Sample(0.0, out));

    b.Push(1.0, 1000, x0);
    b.Push(2.0, 2000, x1);

    CHECK(b.Sample(1.05, out) && out.pos.x == 0.f);           // before the first snapshot + delay
    CHECK(b.Sample(1.6, out) && std::fabs(out.pos.x - 5.f) < 1e-4f); // halfway (1.5 + 0.1)
    CHECK(b.Sample(5.0, out) && out.pos.x == 10.f);           // holds the last pose

    // Out-of-order snapshots are dropped.
    Transform stale;
    stale.pos = { -100, 0, 0 };
    b.Push(2.1, 1000, stale);
    CHECK(b.Sample(5.0, out) && out.pos.x == 10.f);
}

TEST(Interpolation_SmoothUnderJitter) {
    // Sender moves at 6 m/s and sends every 33 ms; each packet takes 20-80 ms.
    // Jitter (60 ms) plus the send interval fits inside the 100 ms playback delay.
    SnapshotBuffer b(0.1);
    std::mt19937 s_Rng(5);
    std::uniform_real_distribution<double> s_Latency(0.02, 0.08);

    struct Pending { double arrive; uint32_t ms; float x; };
    std::vector<Pending> s_InFlight;
    float s_MaxJump = 0.f, s_Prev = 0.f;
    bool s_HavePrev = false;
    const double c_Clock = 5000.0; // sender clock runs 5000 s ahead of ours

    for (int frame = 0; frame < 600; ++frame) {
        const double t = frame / 60.0;

        if (frame % 2 == 0)
            s_InFlight.push_back({ t + s_Latency(s_Rng), static_cast<uint32_t>((t + c_Clock) * 1000.0), static_cast<float>(6.0 * t) });

        std::sort(s_InFlight.begin(), s_InFlight.end(), [](auto& a, auto& b) { return a.arrive < b.arrive; });

        while (!s_InFlight.empty() && s_InFlight.front().arrive <= t) {
            Transform x;
            x.pos.x = s_InFlight.front().x;
            b.Push(t, s_InFlight.front().ms, x);
            s_InFlight.erase(s_InFlight.begin());
        }

        Transform out;
        if (frame > 30 && b.Sample(t, out)) {
            if (s_HavePrev) s_MaxJump = std::max(s_MaxJump, std::fabs(out.pos.x - s_Prev));
            s_Prev = out.pos.x;
            s_HavePrev = true;
        }
    }

    // Ideal per-frame step is 0.1 m; a stall would show up as a 0.2 m catch-up step.
    CHECK_MSG(s_MaxJump < 0.12f, "max per-frame jump %.3f m", s_MaxJump);
}

TEST(Interpolation_CorrectionPolicy) {
    NpcCorrectionPolicy p;
    using A = NpcCorrectionPolicy::Action;

    CHECK(p.Decide(0.2f, 50.f) == A::None);  // tiny drift
    CHECK(p.Decide(2.f, 50.f) == A::Blend);  // drift, far from the player
    CHECK(p.Decide(20.f, 50.f) == A::Snap);  // way off
    CHECK(p.Decide(20.f, 3.f) == A::None);   // near the local player: leave it alone

    Transform a, b;
    b.pos = { 10, 0, 0 };
    const auto s_Half = p.Blend(a, b, 0.125f); // 4/s * 0.125 s = halfway
    CHECK(std::fabs(s_Half.pos.x - 5.f) < 1e-4f);
}
