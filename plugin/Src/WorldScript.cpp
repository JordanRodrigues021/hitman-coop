#include "WorldScript.h"
#include "BlackBox.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace {
std::shared_mutex g_Mutex;
std::unordered_map<void*, WorldScript::Watched> g_ByObject;
std::unordered_map<uint64_t, void*> g_ByKey;
// One bit per hashed pointer: nearly every pin in the game is turned away here, without a lock.
uint8_t g_Filter[8192];
std::atomic<bool> g_Any { false };
std::atomic<bool> g_Shared { true };

std::atomic<float> g_HeroX { 0.f }, g_HeroY { 0.f }, g_HeroZ { 0.f };
std::atomic<bool> g_HeroValid { false };

std::mutex g_FiredMutex;
std::vector<WorldScript::Fired> g_Fired;
// Told only once it stays single: a pin that fires again within a moment is the script's own
// state (the level's "use" prompts repeat one every frame or so), never a player's doing.
struct Candidate {
    WorldScript::Fired fired;
    uint64_t due;
};
std::vector<Candidate> g_Pending;
struct PinSeen {
    uint64_t last = 0;
    uint32_t count = 0;
    bool repeating = false;
};
std::unordered_map<uint64_t, PinSeen> g_PinSeen; // (key, pin) -> when last, and whether it repeats
std::atomic<uint32_t> g_Repeating { 0 };
// (key, pin) -> tick count until which it isn't told: just told (a hold fires it again), or just
// applied for the partner.
std::unordered_map<uint64_t, uint64_t> g_Quiet;

thread_local int t_Replaying = 0;
std::atomic<uint32_t> g_Seen { 0 }, g_Told { 0 }, g_Applied { 0 }, g_Unknown { 0 }, g_Far { 0 }, g_WithData { 0 };

size_t Slot(void* p_Object) {
    return static_cast<size_t>((reinterpret_cast<uintptr_t>(p_Object) >> 4) * 0x9E3779B97F4A7C15ull >> 48);
}

uint64_t QuietKey(uint64_t p_Key, uint32_t p_Pin) {
    return p_Key * 0x9E3779B97F4A7C15ull ^ p_Pin;
}

uint32_t Crc32(const char* p_Text) {
    uint32_t s_Crc = 0xFFFFFFFF;

    for (; *p_Text; ++p_Text) {
        s_Crc ^= static_cast<uint8_t>(*p_Text);

        for (int i = 0; i < 8; ++i)
            s_Crc = (s_Crc >> 1) ^ (0xEDB88320 & (0 - (s_Crc & 1)));
    }

    return ~s_Crc;
}

// Pin ids are the CRC32 of their names; these are the ones interactions are likely to use.
constexpr const char* c_PinNames[] = {
    "Activated", "OnActivated", "ActionActivated", "OnActionActivated", "Activate", "OnActivate", "Triggered", "OnTriggered", "Trigger",
    "Used", "OnUsed", "Use", "OnUse", "Completed", "OnCompleted", "Aborted", "OnAborted", "Started", "OnStarted", "Start", "Stop",
    "OnHoldStarted", "OnHoldAborted", "OnHoldCompleted", "HoldStarted", "HoldCompleted", "Enabled", "Disabled", "OnEnabled", "OnDisabled",
    "OnVisible", "OnHidden", "Visible", "Hidden", "InRange", "OutOfRange", "OnInRange", "OnOutOfRange", "Done", "OnDone", "Executed",
    "OnExecuted", "Interact", "OnInteract", "Interacted", "OnInteracted", "Pressed", "OnPressed", "Released", "OnReleased", "Success",
    "OnSuccess", "Failed", "OnFailed", "Cancelled", "OnCancelled", "Canceled", "OnCanceled", "Interrupted", "OnInterrupted", "Begin", "End",
    "OnBegin", "OnEnd", "Selected", "OnSelected", "Picked", "OnPicked", "Open", "Close", "OnOpen", "OnClose", "On", "Off", "Toggle",
    "OnToggle", "SetVisible", "SetEnabled", "Enable", "Disable", "Show", "Hide", "ActionStarted", "ActionCompleted", "ActionAborted",
    "OnActionStarted", "OnActionCompleted", "OnActionAborted", "SubactionActivated", "OnSubactionActivated", "ItemUsed", "OnItemUsed",
};
}

std::string WorldScript::PinName(uint32_t p_Pin) {
    static std::unordered_map<uint32_t, const char*> s_Names;
    static std::once_flag s_Once;
    std::call_once(s_Once, [] {
        for (const char* s_Name : c_PinNames)
            s_Names.emplace(Crc32(s_Name), s_Name);
    });

    char s_Text[64];

    if (const auto s_It = s_Names.find(p_Pin); s_It != s_Names.end())
        std::snprintf(s_Text, sizeof(s_Text), "%s (%08x)", s_It->second, p_Pin);
    else
        std::snprintf(s_Text, sizeof(s_Text), "pin %08x", p_Pin);

    return s_Text;
}

void WorldScript::SetWatched(std::vector<Watched> p_List) {
    std::unique_lock s_Lock(g_Mutex);
    g_ByObject.clear();
    g_ByKey.clear();
    std::memset(g_Filter, 0, sizeof(g_Filter));

    for (auto& s_Entry : p_List) {
        if (!s_Entry.object || !s_Entry.key || g_ByObject.contains(s_Entry.object))
            continue;

        // Two entities with one key (the walk met a template twice) could be told apart in
        // neither game: watched, never shared.
        if (!g_ByKey.emplace(s_Entry.key, s_Entry.object).second)
            s_Entry.shared = false;

        const size_t s_Slot = Slot(s_Entry.object);
        g_Filter[s_Slot >> 3] |= static_cast<uint8_t>(1 << (s_Slot & 7));
        g_ByObject.emplace(s_Entry.object, s_Entry);
    }

    g_Any = !g_ByObject.empty();
    std::lock_guard s_FiredLock(g_FiredMutex);
    g_Fired.clear();
    g_Quiet.clear();
    g_Pending.clear();
    g_PinSeen.clear();
}

void WorldScript::Clear() {
    SetWatched({});
}

void WorldScript::SetHero(float p_X, float p_Y, float p_Z, bool p_Valid) {
    g_HeroX = p_X;
    g_HeroY = p_Y;
    g_HeroZ = p_Z;
    g_HeroValid = p_Valid;
}

void WorldScript::SetShared(bool p_On) {
    g_Shared = p_On;
}

bool WorldScript::Shared() {
    return g_Shared;
}

WorldScript::ValueKind WorldScript::KindOf(const char* p_TypeName) {
    if (!p_TypeName)
        return Value_None;

    if (std::strcmp(p_TypeName, "int32") == 0)
        return Value_Int32;

    if (std::strcmp(p_TypeName, "bool") == 0)
        return Value_Bool;

    if (std::strcmp(p_TypeName, "float32") == 0)
        return Value_Float;

    if (std::strcmp(p_TypeName, "uint32") == 0)
        return Value_UInt32;

    return Value_Other;
}

void WorldScript::OnOutputPin(void* p_Object, uint32_t p_Pin, ValueKind p_Kind, uint32_t p_Bits, const char* p_TypeName) {
    if (!g_Any.load(std::memory_order_relaxed) || !p_Object)
        return;

    const size_t s_Slot = Slot(p_Object);

    if (!(g_Filter[s_Slot >> 3] & (1 << (s_Slot & 7))))
        return;

    Watched s_Entry;
    {
        std::shared_lock s_Lock(g_Mutex);
        const auto s_It = g_ByObject.find(p_Object);

        if (s_It == g_ByObject.end())
            return;

        s_Entry = s_It->second;
    }

    g_Seen.fetch_add(1, std::memory_order_relaxed);

    // One we are signalling for the partner right now, and whatever it sets off at once.
    if (t_Replaying > 0)
        return;

    const uint64_t s_Now = GetTickCount64();
    const uint64_t s_Key = QuietKey(s_Entry.key, p_Pin);
    bool s_Repeating = false, s_Note = true;
    {
        std::lock_guard s_Lock(g_FiredMutex);
        PinSeen& s_Seen = g_PinSeen[s_Key];

        if (s_Seen.count && s_Now - s_Seen.last < 1500 && !s_Seen.repeating) {
            s_Seen.repeating = true;
            g_Repeating.fetch_add(1, std::memory_order_relaxed);
            // The one waiting to be told was the start of the repeats: not told.
            std::erase_if(g_Pending, [&](const Candidate& p_Candidate) {
                return p_Candidate.fired.key == s_Entry.key && p_Candidate.fired.pin == p_Pin;
            });
        }

        s_Repeating = s_Seen.repeating;
        // A repeating pin is noted three times, not thousands (the flight recorder would be full of it).
        s_Note = ++s_Seen.count <= 3;
        s_Seen.last = s_Now;
    }

    float s_Distance = -1.f;

    if (s_Entry.hasPos && g_HeroValid.load(std::memory_order_relaxed)) {
        const float s_X = s_Entry.pos[0] - g_HeroX.load(std::memory_order_relaxed), s_Y = s_Entry.pos[1] - g_HeroY.load(std::memory_order_relaxed),
                    s_Z = s_Entry.pos[2] - g_HeroZ.load(std::memory_order_relaxed);
        s_Distance = std::sqrt(s_X * s_X + s_Y * s_Y + s_Z * s_Z);
    }

    const char* s_Why = "will be told to the partner if it stays single";

    if (!s_Entry.shared)
        s_Why = "not a kind that is shared";
    else if (!g_Shared.load(std::memory_order_relaxed))
        s_Why = "sharing is switched off";
    else if (p_Kind == Value_Other) {
        s_Why = "carries a value that can't be sent: not shared";
        g_WithData.fetch_add(1, std::memory_order_relaxed);
    }
    else if (s_Repeating) {
        s_Why = "repeats: the script's own state, not shared";
    }
    else if (s_Distance < 0.f || s_Distance > 4.f) {
        // No place to tell, or our 47 is nowhere near: the script did this, as it does in the
        // partner's game too.
        s_Why = s_Distance < 0.f ? "no place to tell whose it is: not shared" : "our 47 isn't at it: the script's own";
        g_Far.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        std::lock_guard s_Lock(g_FiredMutex);
        uint64_t& s_Until = g_Quiet[s_Key];

        if (s_Now < s_Until)
            s_Why = "just told or just applied: not again";
        else if (g_Pending.size() < 64)
            g_Pending.push_back({ { s_Entry.key, p_Pin, p_Kind, p_Bits }, s_Now + 800 });
    }

    if (s_Note)
        BlackBox::Note("script", "%s of '%s' (type %x, key %016llx, %.1f m from our 47%s%s, value %08x): %s", PinName(p_Pin).c_str(), s_Entry.name, s_Entry.type,
            static_cast<unsigned long long>(s_Entry.key), s_Distance, p_Kind != Value_None ? ", with a " : "",
            p_Kind != Value_None ? (p_TypeName ? p_TypeName : "value") : "", p_Bits, s_Why);
}

bool WorldScript::Take(std::vector<Fired>& p_Out) {
    std::lock_guard s_Lock(g_FiredMutex);
    const uint64_t s_Now = GetTickCount64();

    // Candidates that stayed single are told now.
    for (auto s_It = g_Pending.begin(); s_It != g_Pending.end();) {
        if (s_Now < s_It->due) {
            ++s_It;
            continue;
        }

        g_Quiet[QuietKey(s_It->fired.key, s_It->fired.pin)] = s_Now + 400;
        g_Fired.push_back(s_It->fired);
        g_Told.fetch_add(1, std::memory_order_relaxed);
        s_It = g_Pending.erase(s_It);
    }

    if (g_Fired.empty())
        return false;

    p_Out.insert(p_Out.end(), g_Fired.begin(), g_Fired.end());
    g_Fired.clear();
    return true;
}

void* WorldScript::BeginReplay(uint64_t p_Key, uint32_t p_Pin, std::string& p_Name) {
    void* s_Object = nullptr;
    {
        std::shared_lock s_Lock(g_Mutex);
        const auto s_It = g_ByKey.find(p_Key);

        if (s_It != g_ByKey.end()) {
            s_Object = s_It->second;

            if (const auto s_Entry = g_ByObject.find(s_Object); s_Entry != g_ByObject.end())
                p_Name = s_Entry->second.name;
        }
    }

    if (!s_Object) {
        g_Unknown.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    {
        // Ours for a moment: if this game's script fires the same pin again by itself, it isn't
        // told back to the partner who started it.
        std::lock_guard s_Lock(g_FiredMutex);
        g_Quiet[QuietKey(p_Key, p_Pin)] = GetTickCount64() + 3000;
    }

    g_Applied.fetch_add(1, std::memory_order_relaxed);
    ++t_Replaying;
    return s_Object;
}

void WorldScript::EndReplay() {
    if (t_Replaying > 0)
        --t_Replaying;
}

WorldScript::Counts WorldScript::Stats() {
    Counts s_Counts;
    {
        std::shared_lock s_Lock(g_Mutex);
        s_Counts.watched = g_ByObject.size();

        for (const auto& [s_Object, s_Entry] : g_ByObject)
            s_Counts.shared += s_Entry.shared ? 1 : 0;
    }

    s_Counts.seen = g_Seen;
    s_Counts.told = g_Told;
    s_Counts.applied = g_Applied;
    s_Counts.unknown = g_Unknown;
    s_Counts.scripted = g_Far;
    s_Counts.withData = g_WithData;
    s_Counts.repeating = g_Repeating;
    return s_Counts;
}

std::vector<WorldScript::Watched> WorldScript::List() {
    std::vector<Watched> s_Out;
    std::shared_lock s_Lock(g_Mutex);
    s_Out.reserve(g_ByObject.size());

    for (const auto& [s_Object, s_Entry] : g_ByObject)
        s_Out.push_back(s_Entry);

    return s_Out;
}

std::string WorldScript::Describe() {
    const Counts c = Stats();
    char s_Text[256];
    std::snprintf(s_Text, sizeof(s_Text), "%zu interactions watched, %zu of a shared kind%s; %u pins seen, %u told, %u applied for the partner, "
        "%u of theirs not in this game, %u the script's own, %u repeating, %u with a value that can't be sent",
        c.watched, c.shared, g_Shared ? "" : " (sharing off)", c.seen, c.told, c.applied, c.unknown, c.scripted, c.repeating, c.withData);
    return s_Text;
}
