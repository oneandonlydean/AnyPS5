#include "prx/libc/include/RunReport.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <regex>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

#if __has_include("RunReportRevision.hpp")
#include "RunReportRevision.hpp"
#endif

#ifndef APS5_REPORT_COMMIT
#define APS5_REPORT_COMMIT ""
#endif
#ifndef APS5_REPORT_RELEASE
#define APS5_REPORT_RELEASE ""
#endif
#ifndef APS5_REPORT_MODIFIED
#define APS5_REPORT_MODIFIED (-1)
#endif

namespace fs = std::filesystem;

namespace RunReport {

namespace {

constexpr const char* ReportName = "anyps5-report.json";
constexpr std::string_view CheckpointPrefix = "anyps5-report.";
constexpr std::string_view CheckpointSuffix = ".ckpt";
constexpr std::string_view LockSuffix = ".lock";

const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::None: return "none";
        case Kind::MissingFunction: return "missing-function";
        case Kind::UnsupportedShader: return "unsupported-shader";
        case Kind::UnsupportedFormat: return "unsupported-format";
        case Kind::Crash: return "crash";
        case Kind::Assert: return "assert";
        case Kind::Other: return "other";
    }
    return "other";
}

const char* ReasonName(Reason reason) {
    switch (reason) {
        case Reason::GuestExit: return "guest-exit";
        case Reason::QuitRequested: return "quit-requested";
        case Reason::RuntimeStop: return "runtime-stop";
        case Reason::Crash: return "crash";
        case Reason::UncleanUnknown: return "unclean-unknown";
        case Reason::Other: return "other";
    }
    return "other";
}

const char* ExitName(Exit exit) {
    switch (exit) {
        case Exit::Clean: return "clean";
        case Exit::Crash: return "crash";
        case Exit::Abort: return "abort";
        case Exit::Killed: return "killed";
    }
    return "abort";
}

const char* StopMessage(const Stop& stop) {
    switch (stop.reason) {
        case Reason::GuestExit: return "The game exited normally.";
        case Reason::QuitRequested: return "The window was closed.";
        case Reason::RuntimeStop: return "The runtime stopped the run on an error it reported.";
        case Reason::Crash: return stop.kind == Kind::Assert ? "A runtime assertion failed." : "The process ended through the abort handler.";
        case Reason::UncleanUnknown: return "The run ended without writing a report; this one was recovered from a checkpoint.";
        case Reason::Other: return "The run ended in a way the runtime did not classify.";
    }
    return "The run ended in a way the runtime did not classify.";
}

class JsonWriter {
public:
    void BeginObject(const char* key = nullptr) { Open(key, '{'); }
    void EndObject() { Close('}'); }
    void BeginArray(const char* key) { Open(key, '['); }
    void EndArray() { Close(']'); }
    void String(const char* key, const std::optional<std::string>& value) { Key(key); if (value) Quote(*value); else text += "null"; }
    void String(const char* key, std::string_view value) { Key(key); Quote(value); }
    void Bool(const char* key, bool value) { Key(key); text += value ? "true" : "false"; }
    void Bool(const char* key, const std::optional<bool>& value) { Key(key); text += value ? (*value ? "true" : "false") : "null"; }
    void Int(const char* key, const std::optional<std::int64_t>& value) { Key(key); text += value ? std::to_string(*value) : "null"; }
    void Number(const char* key, double value) {
        Key(key);
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.3f", std::isfinite(value) && value >= 0.0 ? value : 0.0);
        text += buffer;
    }
    void Null(const char* key) { Key(key); text += "null"; }
    std::string Take() { text += '\n'; return std::move(text); }

private:
    void Key(const char* key) {
        if (!first.empty()) {
            if (!first.back()) text += ',';
            first.back() = false;
            text += '\n';
            text.append(first.size() * 2u, ' ');
        }
        if (key != nullptr) { Quote(key); text += ": "; }
    }
    void Open(const char* key, char bracket) {
        Key(key);
        text += bracket;
        first.push_back(true);
    }
    void Close(char bracket) {
        const bool wasEmpty = first.back();
        first.pop_back();
        if (!wasEmpty) {
            text += '\n';
            text.append(first.size() * 2u, ' ');
        }
        text += bracket;
    }
    void Quote(std::string_view value) {
        text += '"';
        for (const unsigned char character : value) {
            switch (character) {
                case '"': text += "\\\""; break;
                case '\\': text += "\\\\"; break;
                case '\n': text += "\\n"; break;
                case '\r': text += "\\r"; break;
                case '\t': text += "\\t"; break;
                default:
                    if (character < 0x20u || character == 0x7fu) {
                        char escape[8];
                        std::snprintf(escape, sizeof(escape), "\\u%04x", character);
                        text += escape;
                    } else {
                        text += static_cast<char>(character);
                    }
            }
        }
        text += '"';
    }

    std::string text;
    std::vector<bool> first;
};

bool ValidUtf8(std::string_view text, std::size_t& characters) {
    characters = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = lead < 0x80u ? 1u : (lead >> 5u) == 0x6u ? 2u : (lead >> 4u) == 0xeu ? 3u : (lead >> 3u) == 0x1eu ? 4u : 0u;
        if (length == 0u || index + length > text.size()) return false;
        std::uint32_t code = length == 1u ? lead : lead & (0x7fu >> length);
        for (std::size_t follow = 1; follow < length; ++follow) {
            const auto byte = static_cast<unsigned char>(text[index + follow]);
            if ((byte & 0xc0u) != 0x80u) return false;
            code = (code << 6u) | (byte & 0x3fu);
        }
        if ((length == 2u && code < 0x80u) || (length == 3u && code < 0x800u) || (length == 4u && (code < 0x10000u || code > 0x10ffffu)) || (code >= 0xd800u && code <= 0xdfffu)) return false;
        index += length;
        ++characters;
    }
    return true;
}

std::string Lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

bool Hex32(std::string_view text) {
    return text.size() == 32u && std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}

std::string GenerateRunId() {
    std::random_device device;
    std::array<std::uint32_t, 4> words{};
    for (auto& word : words) word = device();
    char text[33];
    std::snprintf(text, sizeof(text), "%08x%08x%08x%08x", words[0], words[1], words[2], words[3]);
    return text;
}

std::string FormatUtc(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    char text[24];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

std::optional<std::string> SafeText(std::string_view text, const HostIdentifiers& host) {
    std::size_t characters = 0;
    if (text.empty() || !ValidUtf8(text, characters) || characters > MaxTextCharacters) return std::nullopt;
    if (std::ranges::any_of(text, [](unsigned char character) { return character < 0x20u || character == 0x7fu; })) return std::nullopt;
    const auto lowered = Lower(text);
    for (const auto& word : host.words) {
        if (word.size() >= 3u && lowered.find(Lower(word)) != std::string::npos) return std::nullopt;
    }
    static const std::regex addresses(R"((\b\d{1,3}(\.\d{1,3}){3}\b)|(([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})|(([0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}))");
    static const std::regex paths(R"((^|[\s"'(])(/(home|Users|root|var|tmp|mnt|media)/|[A-Za-z]:[\\/]|\\\\))");
    const std::string owned(text);
    if (std::regex_search(owned, addresses) || std::regex_search(owned, paths)) return std::nullopt;
    return owned;
}

HostIdentifiers CurrentHostIdentifiers() {
    HostIdentifiers host;
    for (const char* name : {"USER", "USERNAME", "LOGNAME", "HOME", "USERPROFILE", "HOSTNAME", "COMPUTERNAME"}) {
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') host.words.emplace_back(value);
    }
    if (!host.words.empty()) {
        std::vector<std::string> expanded;
        for (const auto& word : host.words) {
            const auto name = fs::path(word).filename().string();
            if (name.size() >= 3u) expanded.push_back(name);
        }
        host.words.insert(host.words.end(), expanded.begin(), expanded.end());
    }
#ifndef _WIN32
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1u) == 0 && name[0] != '\0') host.words.emplace_back(name);
#endif
    return host;
}

Toggle ResolveToggle(std::optional<std::string_view> flag, std::optional<std::string_view> environment) {
    Toggle toggle;
    const auto parse = [&](std::optional<std::string_view> value, std::string_view on, std::string_view off, const char* source) {
        if (!value) return Choice::Default;
        if (*value == on) return Choice::On;
        if (*value == off) return Choice::Off;
        toggle.warnings.push_back(std::string("ignoring the invalid ") + source + " value '" + std::string(*value) + "'");
        return Choice::Default;
    };
    Choice choice = parse(flag, "on", "off", "--aps5-report");
    if (choice == Choice::Default) choice = parse(environment, "1", "0", "APS5_REPORT");
    toggle.enabled = choice != Choice::Off;
    toggle.explicitlyOn = choice == Choice::On;
    return toggle;
}

std::optional<std::string_view> FlagFromArguments(const std::vector<std::string>& arguments) {
    constexpr std::string_view prefix = "--aps5-report=";
    std::optional<std::string_view> found;
    for (const auto& argument : arguments) {
        if (std::string_view(argument).substr(0, prefix.size()) == prefix) found = std::string_view(argument).substr(prefix.size());
    }
    return found;
}

std::vector<std::string> CommandLineArguments() {
    std::vector<std::string> arguments;
#if defined(_WIN32)
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int index = 0; index < count; ++index) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
            if (bytes <= 1) continue;
            std::string utf8(static_cast<std::size_t>(bytes - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, utf8.data(), bytes, nullptr, nullptr);
            arguments.push_back(std::move(utf8));
        }
        LocalFree(wide);
    }
#elif defined(__APPLE__)
    char** argv = *_NSGetArgv();
    for (int index = 0; index < *_NSGetArgc(); ++index) arguments.emplace_back(argv[index]);
#else
    std::ifstream file("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (std::size_t begin = 0; begin < all.size();) {
        const auto end = all.find('\0', begin);
        arguments.push_back(all.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
#endif
    return arguments;
}

fs::path ExecutableDirectory() {
    std::error_code error;
#if defined(_WIN32)
    std::wstring path(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) return {};
    path.resize(length);
    return fs::path(path).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    return fs::weakly_canonical(path, error).parent_path();
#else
    return fs::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

fs::path UserStateDirectory() {
#if defined(_WIN32)
    if (const char* base = std::getenv("LOCALAPPDATA"); base != nullptr && *base != '\0') return fs::path(base) / "AnyPS5";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') return fs::path(home) / "Library" / "Application Support" / "AnyPS5";
#else
    if (const char* base = std::getenv("XDG_DATA_HOME"); base != nullptr && *base == '/') return fs::path(base) / "AnyPS5";
    if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/') return fs::path(home) / ".local" / "share" / "AnyPS5";
#endif
    return {};
}

std::string Serialize(const Report& report) {
    JsonWriter json;
    json.BeginObject();
    json.String("format", std::string_view("anyps5-report"));
    json.Int("version", std::int64_t{1});
    json.String("generated", std::string_view(report.generated));
    json.BeginObject("anyps5");
    json.String("commit", report.build.commit);
    json.String("release", report.build.release);
    json.Bool("modified", report.build.modified);
    json.EndObject();
    json.BeginObject("game");
    json.String("titleId", report.game.titleId);
    json.Null("contentId");
    json.String("contentVersion", report.game.contentVersion);
    json.String("name", report.game.name);
    json.EndObject();
    json.BeginObject("system");
    json.String("os", report.os);
    json.Null("osVersion");
    json.Null("cpu");
    json.Null("ramGiB");
    json.Null("gpu");
    json.EndObject();
    json.BeginObject("run");
    json.String("id", std::string_view(report.runId));
    json.Number("seconds", report.seconds);
    json.String("exit", std::string_view(ExitName(report.exit)));
    json.Int("exitCode", report.exitCode);
    json.String("stage", std::string_view("unknown"));
    json.BeginObject("milestones");
    json.EndObject();
    json.Int("framesPresented", std::int64_t{0});
    json.Null("averageFps");
    json.Null("fpsWindowSeconds");
    json.Bool("recovered", report.recovered);
    json.Bool("incomplete", report.incomplete);
    json.EndObject();
    json.BeginObject("coverage");
    for (const char* key : {"milestones", "missing", "shaders", "unsupported", "imageFormats", "descriptors"}) json.Bool(key, false);
    json.EndObject();
    json.BeginObject("truncated");
    for (const char* key : {"functions", "unsupported", "imageFormats", "descriptors"}) json.Int(key, std::int64_t{0});
    json.EndObject();
    json.BeginObject("missing");
    json.BeginArray("functions");
    json.EndArray();
    json.EndObject();
    json.BeginObject("shaders");
    json.Int("compiled", std::int64_t{0});
    json.Int("failed", std::int64_t{0});
    json.BeginArray("unsupported");
    json.EndArray();
    json.BeginArray("imageFormats");
    json.EndArray();
    json.BeginArray("descriptors");
    json.EndArray();
    json.EndObject();
    json.BeginObject("stop");
    json.String("kind", std::string_view(KindName(report.stop.kind)));
    json.String("reason", std::string_view(ReasonName(report.stop.reason)));
    json.String("message", std::string_view(StopMessage(report.stop)));
    json.String("source", report.stop.source);
    json.EndObject();
    json.EndObject();
    auto text = json.Take();
    if (text.size() > MaxBytes) throw std::runtime_error("run report: the serialized report exceeds 65,536 bytes");
    return text;
}

bool WriteAtomically(const fs::path& target, std::string_view bytes, std::string_view uniqueName, std::string& error) {
    std::error_code code;
    const auto status = fs::symlink_status(target, code);
    if (!code && status.type() != fs::file_type::not_found && status.type() != fs::file_type::regular) {
        error = "the destination is a symbolic link or not a regular file";
        return false;
    }
    const auto temporary = target.parent_path() / ("." + target.filename().string() + "." + std::string(uniqueName) + ".tmp");
    bool written = false;
#ifdef _WIN32
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        written = static_cast<bool>(file);
    }
#else
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor >= 0) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = ::write(descriptor, bytes.data() + done, bytes.size() - done);
            if (count <= 0) break;
            done += static_cast<std::size_t>(count);
        }
        written = done == bytes.size() && ::fsync(descriptor) == 0;
        ::close(descriptor);
    }
#endif
    if (!written) {
        fs::remove(temporary, code);
        error = "writing the temporary file failed";
        return false;
    }
    fs::rename(temporary, target, code);
    if (code) {
        fs::remove(temporary, code);
        error = "replacing the destination failed";
        return false;
    }
    return true;
}

struct FileLock::Handle {
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
#else
    int file = -1;
#endif
};

std::unique_ptr<FileLock> FileLock::TryAcquire(const fs::path& path, bool& busy) {
    busy = false;
    std::unique_ptr<FileLock> lock(new FileLock);
    lock->handle = std::make_unique<Handle>();
#ifdef _WIN32
    lock->handle->file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock->handle->file == INVALID_HANDLE_VALUE) {
        busy = GetLastError() == ERROR_SHARING_VIOLATION;
        return nullptr;
    }
#else
    lock->handle->file = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock->handle->file < 0) return nullptr;
    if (::flock(lock->handle->file, LOCK_EX | LOCK_NB) != 0) {
        busy = errno == EWOULDBLOCK;
        return nullptr;
    }
#endif
    return lock;
}

FileLock::~FileLock() {
    if (!handle) return;
#ifdef _WIN32
    if (handle->file != INVALID_HANDLE_VALUE) CloseHandle(handle->file);
#else
    if (handle->file >= 0) ::close(handle->file);
#endif
}

std::vector<fs::path> RecoverAbandonedRuns(const std::vector<fs::path>& directories, std::string& warnings) {
    std::vector<fs::path> recovered;
    for (const auto& directory : directories) {
        std::error_code error;
        if (directory.empty() || !fs::is_directory(directory, error)) continue;
        std::vector<fs::path> checkpoints;
        for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
            const auto name = it->path().filename().string();
            if (name.size() > CheckpointPrefix.size() + CheckpointSuffix.size() && name.rfind(CheckpointPrefix, 0) == 0 && name.ends_with(CheckpointSuffix) && it->symlink_status(error).type() == fs::file_type::regular) checkpoints.push_back(it->path());
        }
        for (const auto& checkpoint : checkpoints) {
            const auto name = checkpoint.filename().string();
            const auto id = name.substr(CheckpointPrefix.size(), name.size() - CheckpointPrefix.size() - CheckpointSuffix.size());
            if (!Hex32(id)) continue;
            auto lockPath = checkpoint;
            lockPath.replace_extension(std::string(LockSuffix));
            bool busy = false;
            const auto lock = FileLock::TryAcquire(lockPath, busy);
            if (!lock) continue;
            std::ifstream file(checkpoint, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
            const auto destination = directory / "reports" / id;
            std::string failure;
            fs::create_directories(destination, error);
            if (body.empty() || body.size() > MaxBytes || error || !WriteAtomically(destination / ReportName, body, id.substr(0, 8), failure)) {
                warnings += "could not recover the report of run " + id + (failure.empty() ? "" : ": " + failure) + "\n";
                continue;
            }
            fs::remove(checkpoint, error);
            fs::remove(lockPath, error);
            recovered.push_back(destination / ReportName);
        }
    }
    return recovered;
}

Collector::Collector(Settings value) : settings(std::move(value)) {
    if (!Hex32(settings.runId)) settings.runId = GenerateRunId();
    host = CurrentHostIdentifiers();
}

Collector::~Collector() {
    {
        std::lock_guard guard(mutex);
        stopWorker = true;
    }
    changed.notify_all();
    if (worker.joinable()) worker.join();
}

bool Collector::Start() {
    started = std::chrono::steady_clock::now();
    std::vector<fs::path> scan;
    if (!settings.executableDirectory.empty()) scan.push_back(settings.executableDirectory);
    if (!settings.userStateDirectory.empty()) scan.push_back(settings.userStateDirectory / "reports");
    std::string warnings;
    for (const auto& path : RecoverAbandonedRuns(scan, warnings)) std::fprintf(stderr, "[run-report] recovered the report of an earlier run that ended without writing one: %s\n", path.string().c_str());
    if (!warnings.empty()) std::fputs(("[run-report] " + warnings).c_str(), stderr);

    std::vector<fs::path> candidates;
    if (!settings.executableDirectory.empty()) candidates.push_back(settings.executableDirectory);
    if (!settings.userStateDirectory.empty()) candidates.push_back(settings.userStateDirectory / "reports" / settings.runId);
    for (const auto& candidate : candidates) {
        std::error_code error;
        fs::create_directories(candidate, error);
        if (error) continue;
        const auto stem = std::string(CheckpointPrefix) + settings.runId;
        bool busy = false;
        auto acquired = FileLock::TryAcquire(candidate / (stem + std::string(LockSuffix)), busy);
        if (!acquired) continue;
        directory = candidate;
        lock = std::move(acquired);
        reportPath = directory / ReportName;
        checkpointPath = directory / (stem + std::string(CheckpointSuffix));
        lockPath = directory / (stem + std::string(LockSuffix));
        if (!WriteCheckpoint()) {
            lock.reset();
            fs::remove(lockPath, error);
            directory.clear();
            continue;
        }
        break;
    }
    if (directory.empty()) {
        std::fputs("[run-report] no writable folder for the run report; reporting is off for this run\n", stderr);
        return false;
    }
    started_ = true;
    if (settings.worker) worker = std::thread([this] { Work(); });
    return true;
}

Report Collector::Snapshot(Exit exit, Stop stop, std::optional<std::int64_t> exitCode, bool recovered) const {
    Report report;
    report.generated = FormatUtc(std::chrono::system_clock::now());
    report.build = settings.build;
    if (settings.game) {
        auto game = settings.game();
        if (game.titleId) report.game.titleId = game.titleId;
        report.game.contentVersion = game.contentVersion;
        report.game.name = game.name;
    }
    if (report.game.titleId) {
        static const std::regex titleId("^[A-Z]{4}[0-9]{5}$");
        if (!std::regex_match(*report.game.titleId, titleId)) report.game.titleId.reset();
    }
    if (report.game.contentVersion) {
        static const std::regex version("^[0-9A-Za-z._-]{1,32}$");
        if (!std::regex_match(*report.game.contentVersion, version)) report.game.contentVersion.reset();
    }
    if (report.game.name) report.game.name = SafeText(*report.game.name, host);
#if defined(_WIN32)
    report.os = "windows";
#elif defined(__APPLE__)
    report.os = "macos";
#else
    report.os = "linux";
#endif
    report.runId = settings.runId;
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    report.exit = exit;
    report.exitCode = exitCode;
    report.recovered = recovered;
    report.incomplete = recovered;
    report.stop = std::move(stop);
    return report;
}

bool Collector::WriteCheckpoint() {
    try {
        const auto text = Serialize(Snapshot(Exit::Killed, Stop{Kind::Other, Reason::UncleanUnknown, std::nullopt}, std::nullopt, true));
        std::string error;
        return WriteAtomically(checkpointPath, text, settings.runId.substr(0, 8), error);
    } catch (...) {
        return false;
    }
}

void Collector::Work() {
    std::unique_lock guard(mutex);
    while (!stopWorker) {
        if (changed.wait_for(guard, settings.checkpointInterval, [this] { return stopWorker; })) break;
        guard.unlock();
        WriteCheckpoint();
        guard.lock();
    }
}

void Collector::NoteQuitRequested() noexcept {
    std::lock_guard guard(mutex);
    quitRequested = true;
}

bool Collector::Finished() const noexcept {
    return state == 2;
}

void Collector::Finish(Exit exit, Stop stop, std::optional<std::int64_t> exitCode) {
    std::unique_lock guard(mutex);
    if (!started_) return;
    if (state != 0) {
        changed.wait_for(guard, std::chrono::seconds(3), [this] { return state == 2; });
        return;
    }
    state = 1;
    if (stop.reason == Reason::GuestExit && quitRequested) stop.reason = Reason::QuitRequested;
    stopWorker = true;
    guard.unlock();
    changed.notify_all();
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    try {
        const auto text = Serialize(Snapshot(exit, std::move(stop), exitCode, false));
        std::string error;
        if (WriteAtomically(reportPath, text, settings.runId.substr(0, 8), error)) {
            std::error_code ignored;
            lock.reset();
            fs::remove(checkpointPath, ignored);
            fs::remove(lockPath, ignored);
            std::fprintf(stderr, "[run-report] wrote %s\n", reportPath.string().c_str());
        } else {
            std::fprintf(stderr, "[run-report] could not write %s: %s\n", reportPath.string().c_str(), error.c_str());
        }
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "[run-report] could not write the report: %s\n", failure.what());
    } catch (...) {
        std::fputs("[run-report] could not write the report\n", stderr);
    }
    guard.lock();
    state = 2;
    guard.unlock();
    changed.notify_all();
}

}

namespace {

std::atomic<bool (*)(RunReportGame*)> gameProvider{nullptr};
RunReport::Collector* collector = nullptr;

RunReport::GameInfo ProvideGame() {
    RunReport::GameInfo game;
    const auto provider = gameProvider.load(std::memory_order_acquire);
    RunReportGame fields{};
    if (provider == nullptr || !provider(&fields)) return game;
    fields.titleId[sizeof(fields.titleId) - 1u] = '\0';
    fields.contentVersion[sizeof(fields.contentVersion) - 1u] = '\0';
    fields.name[sizeof(fields.name) - 1u] = '\0';
    if (fields.titleId[0] != '\0') game.titleId = fields.titleId;
    if (fields.contentVersion[0] != '\0') game.contentVersion = fields.contentVersion;
    if (fields.name[0] != '\0') game.name = fields.name;
    return game;
}

bool GameProcess() {
    try {
        std::error_code error;
        for (const char* name : {"/app0/sce_sys/param.json", "/app0/sce_sys/param.sfo"}) {
            if (std::filesystem::exists(ResolvePath_nid_no_patch(name), error)) return true;
        }
    } catch (...) {
    }
    return false;
}

bool StartRunReport() noexcept {
    try {
        const char* environment = std::getenv("APS5_REPORT");
        const auto flag = RunReport::FlagFromArguments(RunReport::CommandLineArguments());
        const auto toggle = RunReport::ResolveToggle(flag, environment != nullptr ? std::optional<std::string_view>(environment) : std::nullopt);
        for (const auto& warning : toggle.warnings) std::fprintf(stderr, "[run-report] %s\n", warning.c_str());
        if (!toggle.enabled || (!toggle.explicitlyOn && !GameProcess())) return false;
        RunReport::Settings settings;
        settings.executableDirectory = RunReport::ExecutableDirectory();
        settings.userStateDirectory = RunReport::UserStateDirectory();
        settings.runId = RunReport::GenerateRunId();
#ifdef _WIN32
        settings.worker = false;
#endif
        if (*APS5_REPORT_COMMIT != '\0') settings.build.commit = APS5_REPORT_COMMIT;
        if (*APS5_REPORT_RELEASE != '\0') settings.build.release = APS5_REPORT_RELEASE;
        if (APS5_REPORT_MODIFIED >= 0) settings.build.modified = APS5_REPORT_MODIFIED != 0;
        settings.game = ProvideGame;
        auto* created = new RunReport::Collector(std::move(settings));
        if (!created->Start()) {
            delete created;
            return false;
        }
        collector = created;
        return true;
    } catch (...) {
        return false;
    }
}

[[maybe_unused]] const bool g_runReportStarted = StartRunReport();

}

extern "C" void LibcRunReportSetGameProvider_nid_no_patch(bool (*provider)(RunReportGame*)) {
    gameProvider.store(provider, std::memory_order_release);
}

extern "C" void LibcRunReportQuitRequested_nid_no_patch() noexcept {
    if (collector != nullptr) collector->NoteQuitRequested();
}

extern "C" void LibcRunReportFinishExit_nid_no_patch(int exitCode) noexcept {
    if (collector == nullptr) return;
    collector->Finish(RunReport::Exit::Clean, RunReport::Stop{RunReport::Kind::None, RunReport::Reason::GuestExit, std::nullopt}, exitCode);
}

extern "C" void LibcRunReportFinishAbort_nid_no_patch(int kind) noexcept {
    if (collector == nullptr) return;
    const auto stopKind = kind == static_cast<int>(RunReport::Kind::Assert) ? RunReport::Kind::Assert : kind == static_cast<int>(RunReport::Kind::Crash) ? RunReport::Kind::Crash : RunReport::Kind::Other;
    collector->Finish(RunReport::Exit::Abort, RunReport::Stop{stopKind, RunReport::Reason::Crash, std::nullopt}, std::nullopt);
}
