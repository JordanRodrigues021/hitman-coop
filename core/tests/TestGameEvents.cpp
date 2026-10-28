#include "TestFramework.h"
#include "coop/GameEvents.h"

using namespace coop;
using namespace coop::GameEvents;

// Shapes as the game sends them (3.280.0 traces), cut down to the fields read.
static const std::string c_Trespass = R"({"Timestamp":60.98,"Name":"Trespassing","Value":{"IsTrespassing":true,"RoomId":81}})";
static const std::string c_Arrest = R"({"Name":"AmbientChanged","Value":{"PreviousAmbientValue":5,"AmbientValue":7,"PreviousAmbient":"AlertedHigh","Ambient":"Arrest"}})";
static const std::string c_Challenge = R"({"Name":"ChallengeCompleted","_DONTSEND":true,"Value":{"ChallengeId":"3fb1","ChallengeName":"UI_CHALLENGES_GLOBAL_FRESH_DISGUISE_NAME"}})";

TEST(GameEvents_ReadFields) {
    std::string s;
    bool b = false;
    CHECK(Bool(c_Trespass, "IsTrespassing", b) && b);
    CHECK(String(c_Arrest, "Ambient", s) && s == "Arrest");
    CHECK(String(c_Arrest, "PreviousAmbient", s) && s == "AlertedHigh");
    CHECK(!String(c_Arrest, "AmbientValue", s)); // a number, not a string
    CHECK(!Bool(c_Arrest, "Missing", b));
    const std::string s_Escaped = "{\"A\":\"say \\\"hi\\\"\"}";
    CHECK(String(s_Escaped, "A", s) && s == "say \"hi\"");
    CHECK(!String(R"({"A":"never closed)", "A", s));
    const std::string s_Unicode = "{\"ActorName\":\"Andr\\u00e9 Beaul\\u00e9\"}";
    CHECK(String(s_Unicode, "ActorName", s) && PlainText(s) == "Andre Beaule");
    CHECK(PlainText("Andr\xC3\xA9 Beaul\xC3\xA9\x01!") == "Andre Beaule!");
}

TEST(GameEvents_AwarenessAndThreat) {
    CHECK(Awareness("Trespassing", c_Trespass) == "trespassing");
    CHECK(Awareness("Trespassing", R"({"Value":{"IsTrespassing":false}})").empty());
    CHECK(Awareness("AmbientChanged", c_Arrest) == "arrest");
    CHECK(ThreatOf("AmbientChanged", c_Arrest) == Threat_Arrest);
    CHECK(ThreatOf("AmbientChanged", R"({"Value":{"Ambient":"Ambient"}})") == Threat_Calm);
    CHECK(ThreatOf("AmbientChanged", R"({"Value":{"Ambient":"Weird"}})") == -1);
    CHECK(ThreatOf("Spotted", c_Arrest) == -1);
    CHECK(Awareness("Spotted", "{}") == "spotted");
    CHECK(Awareness("Kill", "{}").empty());
}

TEST(GameEvents_ChallengeTitles) {
    CHECK(ChallengeTitle("UI_CHALLENGES_GLOBAL_FRESH_DISGUISE_NAME") == "Fresh Disguise");
    CHECK(ChallengeTitle("UI_CHALLENGES_GLOBAL_NON_TARGET_KILL_NAME") == "Non Target Kill");
    CHECK(ChallengeTitle("UI_CHALLENGES_PARIS_SA_NAME") == "Paris SA");
    CHECK(ChallengeTitle("UI_CHALLENGES_GLOBAL_ON_THE_ROCKS_NAME") == "On The Rocks");
    CHECK(ChallengeTitle("") == "a challenge");
    CHECK(ChallengeTitle("___") == "a challenge");
}

TEST(GameEvents_TakedownsRoundTrip) {
    Takedown t;
    REQUIRE(ReadTakedown("Kill", R"({"Value":{"ActorName":"Viktor Novikov","IsTarget":true,"Accident":false,"IsHeadshot":true}})", t));
    CHECK(t.target && t.headshot && !t.accident && !t.knockout && t.name == "Viktor Novikov");
    CHECK(DescribeTakedown("Jordan", t) == "Jordan eliminated Viktor Novikov (target) with a headshot.");

    Takedown o;
    REQUIRE(DecodeTakedown(EncodeTakedown(t), o));
    CHECK(o.target && o.headshot && o.name == t.name);

    REQUIRE(ReadTakedown("Pacify", R"({"Value":{"ActorName":"John Doe","IsTarget":false}})", t));
    CHECK(DescribeTakedown("Sam", t) == "Sam knocked out someone.");
    REQUIRE(ReadTakedown("Kill", R"({"Value":{"IsTarget":true,"Accident":true}})", t));
    CHECK(DescribeTakedown("Sam", t) == "Sam eliminated a target, an accident.");

    CHECK(!ReadTakedown("Spotted", "{}", t));
    CHECK(!DecodeTakedown("no bar", o));
    CHECK(!DecodeTakedown("TQ|x", o));
    const std::string s_Noisy = std::string("|\x01\x02") + "Bob";
    CHECK(DecodeTakedown(s_Noisy, o) && o.name == "Bob" && !o.target);
}

TEST(GameEvents_TallyAndSilentAssassin) {
    Tally a, b;
    CHECK(SilentAssassinBroken(a.State(), b.State()).empty());

    CHECK(a.Add("Kill", R"({"Value":{"IsTarget":true}})"));
    CHECK(a.Add("Trespassing", c_Trespass));
    CHECK(!a.Add("Trespassing", c_Trespass)); // no change
    CHECK(a.Add("AmbientChanged", c_Arrest));
    CHECK(a.Add("ChallengeCompleted", c_Challenge));
    CHECK(a.State().stats.kills == 1 && a.State().stats.targets == 1 && a.State().stats.nonTargetKills == 0);
    CHECK((a.State().flags & PlayerFlag_Trespassing) && a.State().threat == Threat_Arrest && a.State().stats.challenges == 1);
    CHECK(SilentAssassinBroken(a.State(), b.State()).empty()); // a target kill keeps it

    // The same body found by three NPCs is one body.
    const std::string s_Found = R"({"Name":"BodyFound","Value":{"DeadBody":{"RepositoryId":"f750"}}})";
    CHECK(b.Add("BodyFound", s_Found));
    CHECK(!b.Add("BodyFound", s_Found));
    CHECK(b.State().stats.bodiesFound == 1);
    CHECK(SilentAssassinBroken(a.State(), b.State()) == "a body was found");

    // Caught on camera, then the footage erased.
    Tally c;
    c.Add("SecuritySystemRecorder", R"({"Value":{"event":"spotted"}})");
    CHECK(SilentAssassinBroken(c.State(), a.State()).find("camera") != std::string::npos);
    c.Add("SecuritySystemRecorder", R"({"Value":{"event":"erased"}})");
    CHECK(SilentAssassinBroken(c.State(), a.State()).empty());

    c.Add("Kill", R"({"Value":{"IsTarget":false}})");
    CHECK(SilentAssassinBroken(a.State(), c.State()) == "a non-target was killed");
    c.Reset();
    CHECK(c.State() == MsgPlayerState {});
}
