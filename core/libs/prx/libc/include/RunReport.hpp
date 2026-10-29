#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_RUNREPORT_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_RUNREPORT_HPP

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

struct RunReportGame {
    char titleId[16];
    char contentVersion[32];
    char name[320];
};

extern "C" void LibcRunReportSetGameProvider_nid_no_patch(bool (*provider)(RunReportGame*));
extern "C" void LibcRunReportQuitRequested_nid_no_patch() noexcept;
extern "C" void LibcRunReportFinishExit_nid_no_patch(int exitCode) noexcept;
extern "C" void LibcRunReportFinishAbort_nid_no_patch(int kind) noexcept;

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
};

std::string GenerateRunId();
std::string FormatUtc(std::chrono::system_clock::time_point time);
std::string Serialize(const Report& report);
std::optional<std::string> SafeText(std::string_view text, const HostIdentifiers& host);
HostIdentifiers CurrentHostIdentifiers();
Toggle ResolveToggle(std::optional<std::string_view> flag, std::optional<std::string_view> environment);
std::optional<std::string_view> FlagFromArguments(const std::vector<std::string>& arguments);
std::vector<std::string> CommandLineArguments();
std::filesystem::path ExecutableDirectory();
std::filesystem::path UserStateDirectory();
bool WriteAtomically(const std::filesystem::path& target, std::string_view bytes, std::string_view uniqueName, std::string& error);
std::vector<std::filesystem::path> RecoverAbandonedRuns(const std::vector<std::filesystem::path>& directories, std::string& warnings);

class FileLock {
public:
    static std::unique_ptr<FileLock> TryAcquire(const std::filesystem::path& path, bool& busy);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
private:
    FileLock() = default;
    struct Handle;
    std::unique_ptr<Handle> handle;
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

private:
    void Work();

    Settings settings;
    std::filesystem::path directory;
    std::filesystem::path reportPath;
    std::filesystem::path checkpointPath;
    std::filesystem::path lockPath;
    std::unique_ptr<FileLock> lock;
    std::chrono::steady_clock::time_point started;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool stopWorker = false;
    int state = 0;
    bool quitRequested = false;
    bool started_ = false;
    HostIdentifiers host;
};

}

#endif
