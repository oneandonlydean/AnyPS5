#include "prx/libc/include/RunReport.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
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
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
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
        // to_chars never reads the numeric locale, so a host set to a comma decimal point still writes valid JSON.
        char buffer[40];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::isfinite(value) && value >= 0.0 ? value : 0.0, std::chars_format::fixed, 3);
        text.append(buffer, result.ec == std::errc() ? result.ptr : buffer);
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
        if (word.empty()) continue;
        const auto needle = Lower(word);
        if (needle.size() >= 3u) {
            if (lowered.find(needle) != std::string::npos) return std::nullopt;
            continue;
        }
        // A one or two character name cannot be searched for as a substring without dropping most texts, so match it as a whole word.
        for (std::size_t at = lowered.find(needle); at != std::string::npos; at = lowered.find(needle, at + 1u)) {
            const bool startsWord = at == 0u || !std::isalnum(static_cast<unsigned char>(lowered[at - 1u]));
            const bool endsWord = at + needle.size() >= lowered.size() || !std::isalnum(static_cast<unsigned char>(lowered[at + needle.size()]));
            if (startsWord && endsWord) return std::nullopt;
        }
    }
    static const std::regex addresses(R"((\b\d{1,3}(\.\d{1,3}){3}\b)|(([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})|((^|[^0-9A-Za-z:])(([0-9A-Fa-f]{1,4})?:){2,7}[0-9A-Fa-f]{0,4}($|[^0-9A-Za-z:]))|(\d{8,}))");
    static const std::regex paths(R"((^|[\s"'(=,;<>\[{])(/[^\s/]|~/|%[A-Za-z_]+%|[A-Za-z]:[\\/]|\\\\|file:))");
    const std::string owned(text);
    if (owned.find('@') != std::string::npos || owned.find("://") != std::string::npos) return std::nullopt;
    if (std::regex_search(owned, addresses) || std::regex_search(owned, paths)) return std::nullopt;
    return owned;
}

HostIdentifiers CurrentHostIdentifiers() {
    HostIdentifiers host;
    for (const char* name : {"USER", "USERNAME", "LOGNAME", "HOME", "USERPROFILE", "HOSTNAME", "COMPUTERNAME"}) {
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') host.words.emplace_back(value);
    }
#ifdef _WIN32
    wchar_t account[257] = {};
    DWORD accountSize = static_cast<DWORD>(std::size(account));
    if (GetUserNameW(account, &accountSize) && account[0] != L'\0') {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, account, -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1) {
            std::string utf8(static_cast<std::size_t>(bytes - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, account, -1, utf8.data(), bytes, nullptr, nullptr);
            host.words.push_back(std::move(utf8));
        }
    }
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD computerSize = static_cast<DWORD>(std::size(computer));
    if (GetComputerNameW(computer, &computerSize) && computer[0] != L'\0') {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, computer, -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1) {
            std::string utf8(static_cast<std::size_t>(bytes - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, computer, -1, utf8.data(), bytes, nullptr, nullptr);
            host.words.push_back(std::move(utf8));
        }
    }
#else
    // The environment is not trusted to name the account: USER is unset under sudo and systemd.
    char buffer[1024];
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(geteuid(), &entry, buffer, sizeof(buffer), &found) == 0 && found != nullptr) {
        if (entry.pw_name != nullptr && *entry.pw_name != '\0') host.words.emplace_back(entry.pw_name);
        if (entry.pw_dir != nullptr && *entry.pw_dir != '\0') host.words.emplace_back(entry.pw_dir);
    }
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1u) == 0 && name[0] != '\0') host.words.emplace_back(name);
#endif
    const auto count = host.words.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto leaf = fs::path(host.words[index]).filename().string();
        if (!leaf.empty() && leaf != host.words[index]) host.words.push_back(leaf);
    }
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

std::optional<std::string> FlagFromArguments(const std::vector<std::string>& arguments) {
    constexpr std::string_view prefix = "--aps5-report=";
    std::optional<std::string> found;
    for (const auto& argument : arguments) {
        if (std::string_view(argument).substr(0, prefix.size()) == prefix) found = argument.substr(prefix.size());
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
    return Serialize(report, CurrentHostIdentifiers());
}

namespace {

// Every free-form value is checked again here, so no caller can put a host identifier or a malformed value into a report.
Report Sanitized(Report report, const HostIdentifiers& host) {
    const auto matches = [](const std::optional<std::string>& value, const char* pattern) {
        return value && std::regex_match(*value, std::regex(pattern));
    };
    if (!std::regex_match(report.generated, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"))) report.generated = "1970-01-01T00:00:00Z";
    if (!matches(report.build.commit, "[0-9a-f]{7,64}")) report.build.commit.reset();
    if (!matches(report.build.release, "v[0-9A-Za-z][0-9A-Za-z._-]{0,62}")) report.build.release.reset();
    if (!matches(report.game.titleId, "[A-Z]{4}[0-9]{5}")) report.game.titleId.reset();
    if (!matches(report.game.contentVersion, "[0-9A-Za-z._-]{1,32}")) report.game.contentVersion.reset();
    if (report.game.name) report.game.name = SafeText(*report.game.name, host);
    if (report.stop.source) report.stop.source = SafeText(*report.stop.source, host);
    if (report.os && *report.os != "linux" && *report.os != "windows" && *report.os != "macos") report.os.reset();
    if (!Hex32(report.runId)) report.runId = std::string(32u, '0');
    return report;
}

}

std::string Serialize(const Report& original, const HostIdentifiers& host) {
    const Report report = Sanitized(original, host);
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

namespace {

#ifdef _WIN32
bool ReparseOrMissing(const fs::path& path, bool& reparse, bool& directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}
#endif

}

bool WriteAtomically(const fs::path& target, std::string_view bytes, std::string_view uniqueName, std::string& error) {
    const auto parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    const auto leaf = target.filename().string();
    const auto temporaryName = "." + leaf + "." + std::string(uniqueName) + ".tmp";
#ifdef _WIN32
    bool reparse = false, directory = false;
    if (!ReparseOrMissing(parent, reparse, directory) || reparse || !directory) {
        error = "the destination folder is missing, a link or not a folder";
        return false;
    }
    if (ReparseOrMissing(target, reparse, directory) && (reparse || directory)) {
        error = "the destination is a link or a folder";
        return false;
    }
    const auto temporary = parent / temporaryName;
    // CREATE_NEW never opens an existing file or link, so a precreated temporary name fails the write instead of redirecting it.
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "creating the temporary file failed";
        return false;
    }
    bool written = true;
    for (std::size_t done = 0; written && done < bytes.size();) {
        DWORD count = 0;
        written = WriteFile(file, bytes.data() + done, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20u)), &count, nullptr) && count != 0u;
        done += count;
    }
    written = written && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary.c_str());
        error = "writing the temporary file failed";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = "replacing the destination failed";
        return false;
    }
    return true;
#else
    // Every step goes through one descriptor of the parent folder opened without following a link, so a link in the parent cannot redirect the write.
    const int folder = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder < 0) {
        error = "the destination folder is missing, a link or not a folder";
        return false;
    }
    struct stat existing{};
    if (::fstatat(folder, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(existing.st_mode)) {
        ::close(folder);
        error = "the destination is a symbolic link or not a regular file";
        return false;
    }
    const int descriptor = ::openat(folder, temporaryName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        ::close(folder);
        error = "creating the temporary file failed";
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += static_cast<std::size_t>(count);
    }
    bool durable = done == bytes.size();
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    durable = durable && (::fcntl(descriptor, F_FULLFSYNC) == 0 || ::fsync(descriptor) == 0);
#else
    durable = durable && ::fsync(descriptor) == 0;
#endif
    ::close(descriptor);
    if (!durable) {
        ::unlinkat(folder, temporaryName.c_str(), 0);
        ::close(folder);
        error = "writing the temporary file failed";
        return false;
    }
    if (::renameat(folder, temporaryName.c_str(), folder, leaf.c_str()) != 0) {
        ::unlinkat(folder, temporaryName.c_str(), 0);
        ::close(folder);
        error = "replacing the destination failed";
        return false;
    }
    ::fsync(folder);
    ::close(folder);
    return true;
#endif
}

bool ReadBounded(const fs::path& path, std::size_t limit, std::string& bytes) {
    bytes.clear();
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    bool ok = GetFileInformationByHandle(file, &information) && !(information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    char buffer[4096];
    while (ok && bytes.size() <= limit) {
        DWORD count = 0;
        if (!ReadFile(file, buffer, static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), limit + 1u - bytes.size())), &count, nullptr)) { ok = false; break; }
        if (count == 0u) break;
        bytes.append(buffer, count);
    }
    CloseHandle(file);
    return ok && bytes.size() <= limit;
#else
    // One no-follow descriptor is checked and read, so the file cannot be swapped between the check and the read.
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) return false;
    struct stat information{};
    bool ok = ::fstat(descriptor, &information) == 0 && S_ISREG(information.st_mode) && information.st_uid == geteuid();
    char buffer[4096];
    while (ok && bytes.size() <= limit) {
        const auto count = ::read(descriptor, buffer, std::min<std::size_t>(sizeof(buffer), limit + 1u - bytes.size()));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    return ok && bytes.size() <= limit;
#endif
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
    lock->path = path;
#ifdef _WIN32
    lock->handle->file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
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

bool FileLock::IsCurrent() const {
#ifdef _WIN32
    return handle && handle->file != INVALID_HANDLE_VALUE;
#else
    struct stat held{}, named{};
    return handle && handle->file >= 0 && ::fstat(handle->file, &held) == 0 && ::lstat(path.c_str(), &named) == 0 && held.st_dev == named.st_dev && held.st_ino == named.st_ino;
#endif
}

void FileLock::RemoveAndRelease() {
    if (!handle) return;
#ifdef _WIN32
    if (handle->file != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        SetFileInformationByHandle(handle->file, FileDispositionInfo, &disposition, sizeof(disposition));
        CloseHandle(handle->file);
        handle->file = INVALID_HANDLE_VALUE;
    }
#else
    if (handle->file >= 0) {
        ::unlink(path.c_str());
        ::close(handle->file);
        handle->file = -1;
    }
#endif
}

FileLock::~FileLock() {
    if (!handle) return;
#ifdef _WIN32
    if (handle->file != INVALID_HANDLE_VALUE) CloseHandle(handle->file);
#else
    if (handle->file >= 0) ::close(handle->file);
#endif
}

namespace {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* Find(std::string_view key) const {
        for (const auto& [name, value] : members) if (name == key) return &value;
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view value) : text(value) {}
    bool Parse(Json& out) {
        if (!Value(out, 0)) return false;
        Space();
        return at == text.size();
    }

private:
    static constexpr int MaxDepth = 8;
    static constexpr int MaxNodes = 4096;

    void Space() { while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at; }
    bool Literal(std::string_view word) {
        if (text.substr(at, word.size()) != word) return false;
        at += word.size();
        return true;
    }
    bool Text(std::string& out) {
        if (at >= text.size() || text[at] != '"') return false;
        ++at;
        while (at < text.size() && text[at] != '"') {
            const auto character = static_cast<unsigned char>(text[at]);
            if (character < 0x20u) return false;
            if (character != '\\') { out += static_cast<char>(character); ++at; continue; }
            if (++at >= text.size()) return false;
            switch (text[at]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // This writer only escapes control characters; any other \u escape is not from it.
                    unsigned code = 0;
                    if (at + 4u >= text.size() || std::from_chars(text.data() + at + 1u, text.data() + at + 5u, code, 16).ptr != text.data() + at + 5u || code >= 0x80u) return false;
                    out += static_cast<char>(code);
                    at += 4u;
                    break;
                }
                default: return false;
            }
            ++at;
        }
        if (at >= text.size()) return false;
        ++at;
        return true;
    }
    bool Value(Json& out, int depth) {
        if (depth > MaxDepth || ++nodes > MaxNodes) return false;
        Space();
        if (at >= text.size()) return false;
        const char c = text[at];
        if (c == '{') {
            ++at;
            out.type = Json::Type::Object;
            Space();
            if (at < text.size() && text[at] == '}') { ++at; return true; }
            for (;;) {
                Space();
                std::string key;
                Json child;
                if (!Text(key)) return false;
                Space();
                if (at >= text.size() || text[at++] != ':' || !Value(child, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(child));
                Space();
                if (at >= text.size()) return false;
                if (text[at] == ',') { ++at; continue; }
                if (text[at] == '}') { ++at; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++at;
            out.type = Json::Type::Array;
            Space();
            if (at < text.size() && text[at] == ']') { ++at; return true; }
            for (;;) {
                Json child;
                if (!Value(child, depth + 1)) return false;
                out.items.push_back(std::move(child));
                Space();
                if (at >= text.size()) return false;
                if (text[at] == ',') { ++at; continue; }
                if (text[at] == ']') { ++at; return true; }
                return false;
            }
        }
        if (c == '"') { out.type = Json::Type::String; return Text(out.text); }
        if (Literal("true")) { out.type = Json::Type::Bool; out.boolean = true; return true; }
        if (Literal("false")) { out.type = Json::Type::Bool; return true; }
        if (Literal("null")) return true;
        const char* begin = text.data() + at;
        const auto result = std::from_chars(begin, text.data() + text.size(), out.number);
        if (result.ec != std::errc() || result.ptr == begin) return false;
        out.type = Json::Type::Number;
        at += static_cast<std::size_t>(result.ptr - begin);
        return true;
    }

    std::string_view text;
    std::size_t at = 0;
    int nodes = 0;
};

const Json* Member(const Json& object, std::string_view key, Json::Type type) {
    if (object.type != Json::Type::Object) return nullptr;
    const auto* value = object.Find(key);
    return value != nullptr && value->type == type ? value : nullptr;
}

std::optional<std::string> NullableText(const Json& object, std::string_view key, const char* pattern) {
    const auto* value = Member(object, key, Json::Type::String);
    if (value == nullptr || !std::regex_match(value->text, std::regex(pattern))) return std::nullopt;
    return value->text;
}

}

std::optional<Report> ParseCheckpoint(std::string_view bytes, std::string_view id, const HostIdentifiers& host) {
    Json root;
    if (bytes.empty() || bytes.size() > MaxBytes || !JsonReader(bytes).Parse(root)) return std::nullopt;
    const auto* format = Member(root, "format", Json::Type::String);
    const auto* version = Member(root, "version", Json::Type::Number);
    const auto* run = Member(root, "run", Json::Type::Object);
    const auto* stop = Member(root, "stop", Json::Type::Object);
    if (format == nullptr || format->text != "anyps5-report" || version == nullptr || version->number != 1.0 || run == nullptr || stop == nullptr) return std::nullopt;
    const auto* runId = Member(*run, "id", Json::Type::String);
    const auto* exit = Member(*run, "exit", Json::Type::String);
    const auto* recovered = Member(*run, "recovered", Json::Type::Bool);
    const auto* seconds = Member(*run, "seconds", Json::Type::Number);
    const auto* reason = Member(*stop, "reason", Json::Type::String);
    if (runId == nullptr || runId->text != id || exit == nullptr || exit->text != "killed" || recovered == nullptr || !recovered->boolean ||
        seconds == nullptr || !(seconds->number >= 0.0 && seconds->number < 1.0e9) || reason == nullptr || reason->text != "unclean-unknown") return std::nullopt;
    Report report;
    report.generated = NullableText(root, "generated", R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)").value_or("");
    if (report.generated.empty()) return std::nullopt;
    report.runId = std::string(id);
    report.seconds = seconds->number;
    report.exit = Exit::Killed;
    report.recovered = true;
    report.incomplete = true;
    report.stop = Stop{Kind::Other, Reason::UncleanUnknown, std::nullopt};
    if (const auto* build = Member(root, "anyps5", Json::Type::Object)) {
        report.build.commit = NullableText(*build, "commit", "[0-9a-f]{7,64}");
        report.build.release = NullableText(*build, "release", "v[0-9A-Za-z][0-9A-Za-z._-]{0,62}");
        if (const auto* modified = Member(*build, "modified", Json::Type::Bool)) report.build.modified = modified->boolean;
    }
    if (const auto* game = Member(root, "game", Json::Type::Object)) {
        report.game.titleId = NullableText(*game, "titleId", "[A-Z]{4}[0-9]{5}");
        report.game.contentVersion = NullableText(*game, "contentVersion", "[0-9A-Za-z._-]{1,32}");
        if (const auto* name = Member(*game, "name", Json::Type::String)) report.game.name = SafeText(name->text, host);
    }
    if (const auto* system = Member(root, "system", Json::Type::Object)) report.os = NullableText(*system, "os", "linux|windows|macos");
    return report;
}

namespace {

bool EnsureRealDirectory(const fs::path& path) {
    std::error_code error;
    auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) {
        fs::create_directory(path, error);
        status = fs::symlink_status(path, error);
    }
    return !error && status.type() == fs::file_type::directory;
}

fs::path Canonical(const fs::path& path) {
    std::error_code error;
    auto result = fs::weakly_canonical(path, error);
    return error ? path : result;
}

// Recovers one checkpoint whose run is over. `destination` is the folder that receives anyps5-report.json.
void RecoverOne(const fs::path& checkpoint, const std::string& id, const fs::path& destination, std::vector<fs::path>& recovered, std::string& warnings) {
    auto lockPath = checkpoint;
    lockPath.replace_extension(std::string(LockSuffix));
    bool busy = false;
    const auto lock = FileLock::TryAcquire(lockPath, busy);
    if (!lock) return;
    // A finished run removes its checkpoint and then its lock file. Holding a lock on a file that is no longer named, or on a
    // checkpoint that is gone, means that run finished while this one was looking.
    std::string body;
    std::error_code error;
    if (!lock->IsCurrent() || !fs::exists(checkpoint, error)) {
        if (lock->IsCurrent()) lock->RemoveAndRelease();
        return;
    }
    const auto host = CurrentHostIdentifiers();
    std::optional<Report> report;
    if (ReadBounded(checkpoint, MaxBytes, body)) report = ParseCheckpoint(body, id, host);
    if (!report) {
        warnings += "the checkpoint of run " + id + " is not a valid checkpoint; it was set aside as .invalid\n";
        fs::rename(checkpoint, fs::path(checkpoint).replace_extension(".invalid"), error);
        lock->RemoveAndRelease();
        return;
    }
    std::string failure;
    if (!EnsureRealDirectory(destination.parent_path()) || !EnsureRealDirectory(destination) || !WriteAtomically(destination / ReportName, Serialize(*report, host), id.substr(0, 8), failure)) {
        warnings += "could not recover the report of run " + id + (failure.empty() ? "" : ": " + failure) + "\n";
        return;
    }
    fs::remove(checkpoint, error);
    lock->RemoveAndRelease();
    recovered.push_back(destination / ReportName);
}

}

std::vector<fs::path> RecoverAbandonedRuns(const std::vector<fs::path>& directories, std::string& warnings, const std::vector<fs::path>& nestedRoots) {
    std::vector<fs::path> recovered;
    const auto checkpointsIn = [](const fs::path& directory) {
        std::vector<fs::path> checkpoints;
        std::error_code error;
        for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
            const auto name = it->path().filename().string();
            std::error_code statusError;
            if (name.size() > CheckpointPrefix.size() + CheckpointSuffix.size() && name.rfind(CheckpointPrefix, 0) == 0 && name.ends_with(CheckpointSuffix) && it->symlink_status(statusError).type() == fs::file_type::regular) checkpoints.push_back(it->path());
        }
        return checkpoints;
    };
    const auto idOf = [](const fs::path& checkpoint) {
        const auto name = checkpoint.filename().string();
        return name.substr(CheckpointPrefix.size(), name.size() - CheckpointPrefix.size() - CheckpointSuffix.size());
    };
    for (const auto& directory : directories) {
        std::error_code error;
        if (directory.empty() || fs::symlink_status(directory, error).type() != fs::file_type::directory) continue;
        for (const auto& checkpoint : checkpointsIn(directory)) {
            const auto id = idOf(checkpoint);
            if (Hex32(id)) RecoverOne(checkpoint, id, directory / "reports" / id, recovered, warnings);
        }
    }
    for (const auto& root : nestedRoots) {
        std::error_code error;
        if (root.empty() || fs::symlink_status(root, error).type() != fs::file_type::directory) continue;
        std::vector<fs::path> children;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
            std::error_code statusError;
            if (Hex32(it->path().filename().string()) && it->symlink_status(statusError).type() == fs::file_type::directory) children.push_back(it->path());
        }
        for (const auto& child : children) {
            for (const auto& checkpoint : checkpointsIn(child)) {
                const auto id = idOf(checkpoint);
                if (Hex32(id) && id == child.filename().string()) RecoverOne(checkpoint, id, child, recovered, warnings);
            }
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
    const auto executableDirectory = settings.executableDirectory.empty() ? fs::path() : Canonical(settings.executableDirectory);
    const auto userDirectory = settings.userStateDirectory.empty() ? fs::path() : Canonical(settings.userStateDirectory);
    const auto userReports = userDirectory.empty() ? fs::path() : userDirectory / "reports";
    std::string warnings;
    std::vector<fs::path> scan, nested;
    if (!executableDirectory.empty()) scan.push_back(executableDirectory);
    if (!userReports.empty()) nested.push_back(userReports);
    for (const auto& path : RecoverAbandonedRuns(scan, warnings, nested)) std::fprintf(stderr, "[run-report] recovered the report of an earlier run that ended without writing one: %s\n", path.filename().string().c_str());
    if (!warnings.empty()) std::fputs(("[run-report] " + warnings).c_str(), stderr);

    std::vector<fs::path> candidates;
    if (!executableDirectory.empty()) candidates.push_back(executableDirectory);
    if (!userReports.empty()) candidates.push_back(userReports / settings.runId);
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (candidate == executableDirectory) {
            fs::create_directories(candidate, error);
            if (error) continue;
        } else {
            // The folders below the user data folder are made one by one and each must be a real folder, not a link someone else planted.
            std::error_code created;
            fs::create_directories(userDirectory, created);
            if (created || !EnsureRealDirectory(candidate.parent_path()) || !EnsureRealDirectory(candidate)) continue;
        }
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
            lock->RemoveAndRelease();
            lock.reset();
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

void Collector::RefreshGame() {
    {
        std::lock_guard guard(gameMutex);
        if (gameKnown || !settings.game || gameAttempts >= 3) return;
        ++gameAttempts;
    }
    // The provider may take the metadata lock and parse files, so no collector lock is held while it runs.
    auto game = settings.game();
    std::lock_guard guard(gameMutex);
    if (game.titleId) {
        gameCache = std::move(game);
        gameKnown = true;
    }
}

Report Collector::Snapshot(Exit exit, Stop stop, std::optional<std::int64_t> exitCode, bool recovered) const {
    Report report;
    report.generated = FormatUtc(std::chrono::system_clock::now());
    report.build = settings.build;
    {
        std::lock_guard guard(gameMutex);
        report.game = gameCache;
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
        std::unique_lock guard(writeMutex, std::defer_lock);
        if (!guard.try_lock_for(std::chrono::seconds(2))) return false;
        // A finish that has begun owns the files: a late checkpoint would be taken for a killed run.
        if (started_ && state != 0) return false;
        const auto text = Serialize(Snapshot(Exit::Killed, Stop{Kind::Other, Reason::UncleanUnknown, std::nullopt}, std::nullopt, true), host);
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
        workerBusy = true;
        guard.unlock();
        try {
            RefreshGame();
        } catch (...) {
        }
        WriteCheckpoint();
        guard.lock();
        workerBusy = false;
        changed.notify_all();
    }
    workerBusy = false;
    changed.notify_all();
}

void Collector::NoteQuitRequested() noexcept {
    try {
        std::lock_guard guard(mutex);
        quitRequested = true;
    } catch (...) {
    }
}

bool Collector::Finished() const noexcept {
    return state == 2;
}

void Collector::Finish(Exit exit, Stop stop, std::optional<std::int64_t> exitCode) {
    try {
        {
            std::unique_lock guard(mutex);
            if (!started_) return;
            if (state != 0) {
                changed.wait_for(guard, std::chrono::seconds(3), [this] { return state == 2; });
                return;
            }
            state = 1;
            if (stop.reason == Reason::GuestExit && quitRequested) stop.reason = Reason::QuitRequested;
            stopWorker = true;
            changed.notify_all();
            // Never join: the worker may be waiting on a lock that the thread now finishing holds. Give it a moment to leave a write.
            changed.wait_for(guard, std::chrono::seconds(1), [this] { return !workerBusy; });
        }
        // Only a normal end reads the game's metadata; a failing thread may be holding the lock it needs.
        if (exit == Exit::Clean) RefreshGame();
        std::unique_lock files(writeMutex, std::defer_lock);
        static_cast<void>(files.try_lock_for(std::chrono::seconds(2)));
        try {
            const auto text = Serialize(Snapshot(exit, std::move(stop), exitCode, false), host);
            std::string error;
            if (WriteAtomically(reportPath, text, settings.runId.substr(0, 8), error)) {
                if (settings.beforeCheckpointRemoval) settings.beforeCheckpointRemoval();
                // The checkpoint goes while the lock is still held, so no other run can find it unlocked and report this run as killed.
                std::error_code ignored;
                fs::remove(checkpointPath, ignored);
                lock->RemoveAndRelease();
                std::fprintf(stderr, "[run-report] wrote %s\n", reportPath.filename().string().c_str());
            } else {
                std::fprintf(stderr, "[run-report] could not write %s: %s\n", reportPath.filename().string().c_str(), error.c_str());
            }
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "[run-report] could not write the report: %s\n", failure.what());
        } catch (...) {
            std::fputs("[run-report] could not write the report\n", stderr);
        }
        if (files.owns_lock()) files.unlock();
        std::lock_guard guard(mutex);
        state = 2;
        changed.notify_all();
    } catch (...) {
        state = 2;
    }
}

}

namespace {

std::atomic<bool (*)(RunReportGame*)> gameProvider{nullptr};
RunReport::Collector* collector = nullptr;
const char* relaunchChoice = nullptr;

void FinishAtExit() {
    // Returning from the guest entry reaches exit() or ExitProcess without any of the hooks; the exit status is not known here.
    if (collector != nullptr && !collector->Finished()) collector->Finish(RunReport::Exit::Clean, RunReport::Stop{RunReport::Kind::None, RunReport::Reason::GuestExit, std::nullopt}, std::nullopt);
}

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
        const auto arguments = RunReport::CommandLineArguments();
        const auto flag = RunReport::FlagFromArguments(arguments);
        const auto toggle = RunReport::ResolveToggle(flag ? std::optional<std::string_view>(*flag) : std::nullopt, environment != nullptr ? std::optional<std::string_view>(environment) : std::nullopt);
        // The relaunched game does not see this flag on its command line; the environment carries the choice to it.
        if (!toggle.enabled) relaunchChoice = "0";
        else if (toggle.explicitlyOn) relaunchChoice = "1";
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
        std::atexit(FinishAtExit);
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

extern "C" const char* LibcRunReportRelaunchEnvironment_nid_no_patch() noexcept {
    return relaunchChoice;
}
