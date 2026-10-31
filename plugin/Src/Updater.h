#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Keeps HITMAN Co-op up to date by itself. At game start it reads update.txt from the latest
// GitHub release (the version, each file with its size and SHA-256, and what's new), downloads the
// files that differ from the installed ones and checks them, then swaps them in: a DLL the game has
// loaded can be renamed but not overwritten, so the old one becomes <name>.hcold and the new one
// takes its place. The game loads the new version the next time it starts, when the old files are
// deleted and the player is told what's new.
class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Staged, Failed };

    struct Status {
        State state = State::Idle;
        std::string version; // the one staged
        std::string error;
    };

    // A file the update replaces, relative to the game's Retail folder ("mods/HitmanCoop.dll").
    struct File {
        std::string path;
        uint64_t size = 0;
        std::string sha256;
    };

    struct Manifest {
        std::string version;
        std::string base; // where the files are: base + the file's name
        std::vector<File> files;
        std::string notes;
    };

    ~Updater();

    // Deletes what the last update left behind (the .hcold files), at game start.
    static void CleanUp(const std::string& p_GameDir);

    // After an update: what's new in the version now running, once (then forgotten).
    static bool TakeWhatsNew(const std::string& p_GameDir, const std::string& p_Version, std::string& p_Notes);

    // Checks for a newer version than p_Version in the background, and stages it.
    void Start(const std::string& p_GameDir, const std::string& p_Version, const std::string& p_ManifestUrl);
    Status Get() const;

    // update.txt: "version <v>", "base <url>", "file <path> <size> <sha256>", then "notes" and the
    // lines of the changelog. False if it isn't one.
    static bool Parse(const std::string& p_Text, Manifest& p_Out);
    // -1, 0 or 1 as a is older than, the same as, or newer than b ("0.13.0" against "0.12.2").
    static int CompareVersions(const std::string& p_A, const std::string& p_B);
    static std::string Sha256(const std::string& p_Data);

private:
    void Run(std::string p_GameDir, std::string p_Version, std::string p_ManifestUrl);
    void Set(State p_State, const std::string& p_Version = {}, const std::string& p_Error = {});

    mutable std::mutex m_Mutex;
    Status m_Status;
    std::thread m_Thread;
    std::atomic<bool> m_Stop { false };
};
