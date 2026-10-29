#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_RUNREPORT_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_RUNREPORT_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define RUN_REPORT_HAS_FINISH_HOOK 1

struct RunReportGame {
    char titleId[16];
    char contentVersion[32];
    char name[320];
};

extern "C" void LibcRunReportSetGameProvider_nid_no_patch(bool (*provider)(RunReportGame*));
extern "C" void LibcRunReportQuitRequested_nid_no_patch() noexcept;
extern "C" void LibcRunReportFinishExit_nid_no_patch(int exitCode) noexcept;
extern "C" void LibcRunReportFinishAbort_nid_no_patch(int kind) noexcept;
// "0", "1" or null: what APS5_REPORT must be in a relaunched process so that the choice made with --aps5-report survives it.
extern "C" const char* LibcRunReportRelaunchEnvironment_nid_no_patch() noexcept;
// A relaunch finishes the report before it replaces the process; when the replacement fails this reopens the run.
extern "C" void LibcRunReportResume_nid_no_patch() noexcept;
// Starts the checkpoint worker where it could not be started during loading (Windows); a no-op elsewhere.
extern "C" void LibcRunReportBeginWorker_nid_no_patch() noexcept;
// Every runtime library built from the same checkout registers itself; a library from another build makes `modified` unknown.
extern "C" void LibcRunReportRegisterComponent_nid_no_patch(const char* name, const char* commit, int modified) noexcept;

namespace RunReport {

inline constexpr std::size_t MaxBytes = 65536;
inline constexpr std::size_t MaxTextCharacters = 300;

enum class Kind { None, MissingFunction, UnsupportedShader, UnsupportedFormat, Crash, Assert, Other };
enum class Reason { GuestExit, QuitRequested, RuntimeStop, Crash, UncleanUnknown, Other };
enum class Exit { Clean, Crash, Abort, Killed };

struct Stop {
    Kind kind = Kind::None;
    Reason reason = Reason::GuestExit;
    std::optional<std::string> source;
};

struct BuildInfo {
    std::optional<std::string> commit;
    std::optional<std::string> release;
    std::optional<bool> modified;
};

struct GameInfo {
    std::optional<std::string> titleId;
    std::optional<std::string> contentVersion;
    std::optional<std::string> name;
};

struct Report {
    std::string generated;
    BuildInfo build;
    GameInfo game;
    std::optional<std::string> os;
    std::string runId;
    double seconds = 0.0;
    Exit exit = Exit::Clean;
    std::optional<std::int64_t> exitCode;
    bool recovered = false;
    bool incomplete = false;
    Stop stop;
};

struct HostIdentifiers {
    std::vector<std::string> words;
};

enum class Choice { Default, On, Off };

struct Toggle {
    bool enabled = true;
    bool explicitlyOn = false;
    std::vector<std::string> warnings;
};

struct Settings {
    std::filesystem::path executableDirectory;
    std::filesystem::path userStateDirectory;
    std::string runId;
    std::chrono::milliseconds checkpointInterval{5000};
    bool worker = true;
    BuildInfo build;
    std::function<GameInfo()> game;
    // Called by Finish after the report is on disk and before the checkpoint is removed (tests only).
    std::function<void()> beforeCheckpointRemoval;
    // Called while a checkpoint is being written and the write lock is held (tests only).
    std::function<void()> duringCheckpointWrite;
    // How long Finish waits for a checkpoint write that is in progress before it leaves the files alone.
    std::chrono::milliseconds lockWait{2000};
    // Component names that must have registered the same commit for `modified` to be known.
    std::vector<std::string> requiredComponents;
};

void RegisterComponent(std::string_view name, std::string_view commit, int modified);
std::string GenerateRunId();
std::string FormatUtc(std::chrono::system_clock::time_point time);
std::string Serialize(const Report& report);
std::string Serialize(const Report& report, const HostIdentifiers& host);
std::optional<std::string> SafeText(std::string_view text, const HostIdentifiers& host);
HostIdentifiers CurrentHostIdentifiers();
Toggle ResolveToggle(std::optional<std::string_view> flag, std::optional<std::string_view> environment);
std::optional<std::string> FlagFromArguments(const std::vector<std::string>& arguments);
std::vector<std::string> CommandLineArguments();
std::filesystem::path ExecutableDirectory();
std::filesystem::path UserStateDirectory();
bool WriteAtomically(const std::filesystem::path& target, std::string_view bytes, std::string_view uniqueName, std::string& error);
bool ReadBounded(const std::filesystem::path& path, std::size_t limit, std::string& bytes);
// Checkpoints directly in `directories` are recovered to <directory>/reports/<id>/; those in the run-id folders directly
// below `nestedRoots` are recovered in place, next to the checkpoint.
std::vector<std::filesystem::path> RecoverAbandonedRuns(const std::vector<std::filesystem::path>& directories, std::string& warnings, const std::vector<std::filesystem::path>& nestedRoots = {});
// Rebuilds the report of a run from its checkpoint, or nothing when the bytes are not a checkpoint of run `id`.
std::optional<Report> ParseCheckpoint(std::string_view bytes, std::string_view id, const HostIdentifiers& host);

class FileLock {
public:
    static std::unique_ptr<FileLock> TryAcquire(const std::filesystem::path& path, bool& busy);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    // False when the lock file was removed and recreated between opening it and locking it.
    bool IsCurrent() const;
    // Deletes the lock file while the lock is still held, then releases it.
    void RemoveAndRelease();
private:
    FileLock() = default;
    struct Handle;
    std::unique_ptr<Handle> handle;
    std::filesystem::path path;
};

class Collector {
public:
    explicit Collector(Settings settings);
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;
    bool Start();
    void Finish(Exit exit, Stop stop, std::optional<std::int64_t> exitCode);
    void NoteQuitRequested() noexcept;
    bool Finished() const noexcept;
    const std::filesystem::path& ReportPath() const noexcept { return reportPath; }
    const std::filesystem::path& CheckpointPath() const noexcept { return checkpointPath; }
    Report Snapshot(Exit exit, Stop stop, std::optional<std::int64_t> exitCode, bool recovered) const;
    bool WriteCheckpoint();
    // Reopens a run that Finish has already finished (a relaunch whose replacement failed).
    bool Resume();
    void BeginWorker();
    // Reads the game metadata now; a run that fails early still names its game.
    void NoteGame();

private:
    void Work();
    void RefreshGame(bool force = false);
    BuildInfo Provenance() const;

    Settings settings;
    std::filesystem::path directory;
    std::filesystem::path reportPath;
    std::filesystem::path checkpointPath;
    std::filesystem::path lockPath;
    std::unique_ptr<FileLock> lock;
    std::chrono::steady_clock::time_point started;
    std::mutex mutex;
    std::timed_mutex writeMutex;
    mutable std::mutex gameMutex;
    std::condition_variable changed;
    std::thread worker;
    bool stopWorker = false;
    bool workerBusy = false;
    std::atomic<int> state{0};
    bool quitRequested = false;
    bool started_ = false;
    GameInfo gameCache;
    bool gameKnown = false;
    int gameAttempts = 0;
    HostIdentifiers host;
};

}

#endif
