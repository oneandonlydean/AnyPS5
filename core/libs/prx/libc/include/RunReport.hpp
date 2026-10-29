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
// The same, with the message of the exception that is ending the run (null when there is none). A failure the runtime noted on the
// way is the stop of the run only when it is the one this message belongs to.
extern "C" void LibcRunReportFinishAbortWith_nid_no_patch(int kind, const char* terminatingMessage) noexcept;
// True while a run report is being collected. Callers that would do work only for the report check this first.
extern "C" bool LibcRunReportEnabled_nid_no_patch() noexcept;
// "0", "1" or null: what APS5_REPORT must be in a relaunched process so that the choice made with --aps5-report survives it.
extern "C" const char* LibcRunReportRelaunchEnvironment_nid_no_patch() noexcept;
// A relaunch finishes the report before it replaces the process; when the replacement fails this reopens the run.
extern "C" void LibcRunReportResume_nid_no_patch() noexcept;
// Finishes the report before a relaunch. False means the report was not finalized (a checkpoint write is stuck): the files and
// descriptors of the run are still in use and must not be closed.
extern "C" bool LibcRunReportHandoff_nid_no_patch(int exitCode) noexcept;
// Starts the checkpoint worker where it could not be started during loading (Windows); a no-op elsewhere.
extern "C" void LibcRunReportBeginWorker_nid_no_patch() noexcept;
// Every runtime library built from the same checkout registers itself; a library from another build makes `modified` unknown.
extern "C" void LibcRunReportRegisterComponent_nid_no_patch(const char* name, const char* commit, int modified) noexcept;

// What the runtime observed while the game ran. Each call is cheap and safe to make from any thread; none of them blocks on I/O.
// milestone: 0 loaded, 1 started, 2 first draw submitted, 3 first frame presented.
extern "C" void LibcRunReportMilestone_nid_no_patch(int milestone) noexcept;
// One guest frame was accepted for presentation.
extern "C" void LibcRunReportFramePresented_nid_no_patch() noexcept;
// A function the runtime does not implement was called. `file` is the compile-time path of the call site and is reduced to a
// source-tree relative name; the library is taken from it.
extern "C" void LibcRunReportMissingFunction_nid_no_patch(const char* name, const char* file, int line) noexcept;
// what: 0 compiled, 1 failed, 2 cache hit.
extern "C" void LibcRunReportShader_nid_no_patch(int what) noexcept;
// A shader instruction the recompiler cannot translate. Anything unknown is null (encoding, opcode) or -1 (opcodeValue).
extern "C" void LibcRunReportUnsupportedInstruction_nid_no_patch(const char* encoding, const char* opcode, long long opcodeValue, const char* file, int line) noexcept;
// status: 0 accepted, 1 rejected, 2 unknown. usage: 0 sampled, 1 storage, 2 color-target, 3 depth-target, 4 transfer, 5 other.
extern "C" void LibcRunReportImageFormat_nid_no_patch(unsigned format, const char* name, int status, int usage) noexcept;
// type: 0 UniformBuffer .. 6 Sampler in schema order. status as above. usage: 0 read, 1 write, 2 atomic, 3 other.
extern "C" void LibcRunReportDescriptor_nid_no_patch(int type, int status, int usage) noexcept;
// Lock-free counters kept by the library that sees the events: 7 descriptor types x 4 usages (row-major), and one per guest image
// format. The run report reads them when it writes a report; they are never reset.
extern "C" void LibcRunReportSetDescriptorCounters_nid_no_patch(const std::atomic<unsigned long long>* counters) noexcept;
extern "C" void LibcRunReportSetFormatCounters_nid_no_patch(const std::atomic<unsigned long long>* counters, unsigned count) noexcept;
// The run ended on a shader instruction the recompiler cannot translate (the failure reached the driver as fatal).
extern "C" void LibcRunReportUnsupportedShaderStop_nid_no_patch(const char* encoding, const char* opcode, long long opcodeValue, const char* file, int line) noexcept;
// The graphics device the runtime selected: PCI vendor id, device name, driver text and the Vulkan API version word. Called once, after
// the device is chosen; the report writes `system.gpu` from it (null when this was never called, e.g. the run stopped before GPU setup).
extern "C" void LibcRunReportSetGpu_nid_no_patch(unsigned vendorId, const char* name, const char* driver, unsigned apiVersion) noexcept;
// Writes the key a rejected format is filed under (the text its lookup throws) into `buffer`, always null-terminated, and returns its
// length. Other libraries get the key through this call: C++ symbols of libc are not exported to them, only the _nid_no_patch names.
extern "C" unsigned long LibcRunReportUnsupportedFormatKey_nid_no_patch(unsigned format, char* buffer, unsigned long size) noexcept;
// A guest image format the runtime cannot handle ended the run.
extern "C" void LibcRunReportUnsupportedFormatStop_nid_no_patch(unsigned format, const char* file, int line) noexcept;

namespace RunReport {

inline constexpr std::size_t MaxBytes = 65536;
inline constexpr std::size_t MaxListedFunctions = 200;
inline constexpr std::size_t MaxListedFormats = 100;
inline constexpr std::size_t MaxTextCharacters = 300;

enum class Kind { None, MissingFunction, UnsupportedShader, UnsupportedFormat, Crash, Assert, Other };
enum class Reason { GuestExit, QuitRequested, RuntimeStop, Crash, UncleanUnknown, Other };
enum class Exit { Clean, Crash, Abort, Killed };

struct Stop {
    Kind kind = Kind::None;
    Reason reason = Reason::GuestExit;
    std::optional<std::string> source;
    // A message built from typed fields; the default for the kind and reason is used when it is empty.
    std::optional<std::string> message;
};

enum class Stage { Unknown, Loads, Starts, FirstDraw, Presents };
enum class Status { Accepted, Rejected, Unknown };

struct FunctionEntry {
    std::optional<std::string> library;
    std::string name;
    std::int64_t calls = 0;
};

struct UnsupportedEntry {
    std::optional<std::string> encoding;
    std::optional<std::string> opcode;
    std::optional<std::int64_t> opcodeValue;
    std::optional<std::vector<std::string>> modifiers;
    std::int64_t count = 0;
};

struct FormatEntry {
    std::int64_t format = 0;
    std::optional<std::string> name;
    Status status = Status::Unknown;
    int usage = 5;
    std::int64_t count = 0;
};

struct DescriptorEntry {
    int type = 0;
    Status status = Status::Unknown;
    int usage = 3;
    std::int64_t count = 0;
};

struct Coverage {
    bool milestones = false, missing = false, shaders = false, unsupported = false, imageFormats = false, descriptors = false;
};

struct Truncated {
    std::int64_t functions = 0, unsupported = 0, imageFormats = 0, descriptors = 0;
};

// Everything the run observed, in the order of the report.
struct Observations {
    std::optional<double> loaded, started, firstDraw, firstPresent;
    std::int64_t framesPresented = 0;
    std::optional<double> averageFps, fpsWindowSeconds;
    std::int64_t shadersCompiled = 0, shadersFailed = 0;
    std::optional<std::int64_t> cacheHits;
    std::vector<FunctionEntry> functions;
    std::vector<UnsupportedEntry> unsupported;
    std::vector<FormatEntry> imageFormats;
    std::vector<DescriptorEntry> descriptors;
    Coverage coverage;
    Truncated truncated;
    // A hook had to drop at least one observation (a table was busy); the report is marked incomplete.
    bool lostObservations = false;
};

struct BuildInfo {
    std::optional<std::string> commit;
    std::optional<std::string> release;
    std::optional<bool> modified;
};

struct GpuInfo {
    std::optional<std::string> vendor;
    std::optional<std::string> name;
    std::optional<std::string> driver;
    std::optional<std::string> vulkan;
};

// The machine the run was on: operating system version, CPU model, total memory in whole GiB and the selected GPU. Model strings only:
// nothing that names the machine or its user.
struct SystemInfo {
    std::optional<std::string> osVersion;
    std::optional<std::string> cpu;
    std::optional<std::int64_t> ramGiB;
    std::optional<GpuInfo> gpu;
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
    SystemInfo machine;
    std::string runId;
    double seconds = 0.0;
    Exit exit = Exit::Clean;
    std::optional<std::int64_t> exitCode;
    bool recovered = false;
    bool incomplete = false;
    Stop stop;
    Observations observed;
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
    // How long an observation hook waits for the tables before it gives the observation up (and counts it as lost).
    std::chrono::milliseconds tableWait{200};
    // Component names that must have registered the same commit for `modified` to be known.
    std::vector<std::string> requiredComponents;
    // Hooks start the checkpoint worker when it could not be started during loading (Windows), the first time they ask for a checkpoint.
    bool deferredWorker = false;
    // Replaces the probe of the host (tests).
    std::optional<SystemInfo> machine;
    // Which sections this build collects; a section that is off is reported as not covered, not as empty.
    Coverage coverage;
};

void RegisterComponent(std::string_view name, std::string_view commit, int modified);
// A call site as `core/...:line` (the part of the compile-time path inside the source tree), or nothing.
std::optional<std::string> SourceOf(std::string_view file, int line);
// The key a rejected format is filed under: the text the lookup throws, without any prefix the thrower adds. An abort whose message
// ends with it is the stop for that format.
std::string UnsupportedFormatKey(unsigned format);
// Reads the operating system version, the CPU model and the memory size of this host (the GPU is reported by the device code).
SystemInfo ProbeHost();
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
    // The OS handle of the lock file (Windows: HANDLE; elsewhere null), for the process-exit cleanup.
    void* NativeHandle() const;
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
    // Observation hooks. The milestone and failure calls also write a checkpoint, so a run that dies right after still reports them.
    void NoteMilestone(int milestone);
    void NoteFramePresented();
    void NoteMissingFunction(std::string_view library, std::string_view name, std::string_view source, std::string_view terminatingMessage = {});
    void NoteShader(int what);
    void NoteUnsupportedInstruction(std::optional<std::string> encoding, std::optional<std::string> opcode, std::optional<std::int64_t> opcodeValue, std::string_view source);
    void NoteImageFormat(std::int64_t format, std::optional<std::string> name, Status status, int usage);
    void NoteDescriptor(int type, Status status, int usage);
    void NoteStop(Stop stop);
    // The device the runtime selected (system.gpu); also requests a checkpoint.
    void NoteGpu(GpuInfo gpu);
    // `prefixed`: the thrower puts a known prefix ("AGC graphics: ") in front of the message, so that form matches too. Every other
    // failure matches its message exactly: "read" must not be taken for the end of "pread".
    void NoteSoftStop(Stop stop, std::string terminatingMessage, bool prefixed = false);
    void NoteUnsupportedShaderStop(std::optional<std::string> encoding, std::optional<std::string> opcode, std::string_view source);
    void SetDescriptorCounters(const std::atomic<unsigned long long>* counters);
    void SetFormatCounters(const std::atomic<unsigned long long>* counters, unsigned count);
    // The stop a typed failure recorded, used by an abort that has no better explanation.
    std::optional<Stop> PendingStop() const;
    // The stop for an abort that ends with `terminatingMessage`: a fatal failure first, otherwise a failure noted earlier whose
    // own message equals it. A failure the game caught and went on from never names a later, unrelated stop.
    std::optional<Stop> StopFor(std::string_view terminatingMessage) const;
    // Holds the observation tables, so a test can make the hooks time out.
    std::unique_lock<std::timed_mutex> LockTablesForTest();
    bool WriteCheckpoint();
    bool WriteCheckpoint(std::chrono::milliseconds wait);
    // Reopens a run that Finish has already finished (a relaunch whose replacement failed).
    bool Resume();
    // True when the last Finish wrote the report and released the run's files.
    bool Finalized() const noexcept { return finalized; }
    void BeginWorker();
    // Reads the game metadata now; a run that fails early still names its game.
    void NoteGame();

private:
    void Work();
    void RefreshGame(bool force = false);
    BuildInfo Provenance() const;
public:
    struct Tables;
private:
    Observations Observe() const;
    void CheckpointSoon();
    void NoteLost(int section);

    Settings settings;
    std::filesystem::path directory;
    std::filesystem::path reportPath;
    std::filesystem::path checkpointPath;
    std::filesystem::path lockPath;
    std::unique_ptr<FileLock> lock;
    std::chrono::steady_clock::time_point started;
    std::mutex mutex;
    std::timed_mutex writeMutex;
    std::condition_variable changed;
    std::thread worker;
    bool stopWorker = false;
    bool workerBusy = false;
    std::atomic<int> state{0};
    bool quitRequested = false;
    bool started_ = false;
    struct GameState;
    std::shared_ptr<GameState> game;
    mutable std::mutex machineMutex;
    SystemInfo machineInfo;
    bool workerRunning = false;
    bool workerWanted = false;
    bool reportPublished = false;
    std::string publishedBytes;
    std::atomic<bool> finalized{false};
    std::shared_ptr<Tables> tables;
    std::atomic<std::int64_t> milestoneNanos[4];
    std::atomic<std::int64_t> frames{0};
    std::atomic<std::int64_t> firstFrameNanos{-1};
    std::atomic<std::int64_t> lastFrameNanos{-1};
    std::atomic<std::int64_t> compiled{0}, failedShaders{0}, cacheHitCount{0};
    std::atomic<int> checkpointsRequested{0};
    std::atomic<bool> checkpointWanted{false};
    std::atomic<int> lost[4];
    std::atomic<const std::atomic<unsigned long long>*> descriptorCounters{nullptr};
    std::atomic<const std::atomic<unsigned long long>*> formatCounters{nullptr};
    std::atomic<unsigned> formatCounterCount{0};
    HostIdentifiers host;
};

}

#endif
