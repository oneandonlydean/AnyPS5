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
#include <set>
#include <system_error>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <tuple>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/utsname.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
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

namespace {

std::string CollapseSpaces(std::string_view text) {
    std::string result;
    bool space = true;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0') {
            if (!space) result += ' ';
            space = true;
        } else {
            result += c;
            space = false;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

// The marketing name of the CPU from the CPUID brand string, where the CPU has one.
std::optional<std::string> CpuBrand() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004u) {
        char brand[49] = {};
        for (unsigned leaf = 0; leaf < 3u; ++leaf) {
            __get_cpuid(0x80000002u + leaf, &eax, &ebx, &ecx, &edx);
            std::memcpy(brand + leaf * 16u, &eax, 4);
            std::memcpy(brand + leaf * 16u + 4u, &ebx, 4);
            std::memcpy(brand + leaf * 16u + 8u, &ecx, 4);
            std::memcpy(brand + leaf * 16u + 12u, &edx, 4);
        }
        auto text = CollapseSpaces(brand);
        if (!text.empty()) return text;
    }
#endif
#ifdef __linux__
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto text = CollapseSpaces(line.substr(colon + 1u));
                if (!text.empty()) return text;
            }
        }
    }
#endif
    return std::nullopt;
}

std::int64_t WholeGiB(std::uint64_t bytes) {
    return static_cast<std::int64_t>((bytes + (1ull << 29u)) >> 30u);
}

}

SystemInfo ProbeHost() {
    SystemInfo info;
    info.cpu = CpuBrand();
#if defined(__linux__)
    std::string name;
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            name = line.substr(12u);
            if (name.size() >= 2u && (name.front() == '"' || name.front() == '\'')) name = name.substr(1u, name.size() - 2u);
            break;
        }
    }
    utsname kernel{};
    const std::string version = uname(&kernel) == 0 ? std::string(kernel.release) : std::string();
    if (!name.empty() || !version.empty()) info.osVersion = CollapseSpaces(name.empty() ? "Linux " + version : name + (version.empty() ? "" : " (kernel " + version + ")"));
    struct sysinfo memory{};
    if (sysinfo(&memory) == 0) info.ramGiB = WholeGiB(static_cast<std::uint64_t>(memory.totalram) * memory.mem_unit);
#elif defined(__APPLE__)
    char buffer[256] = {};
    std::size_t size = sizeof(buffer);
    if (sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) == 0) info.osVersion = "macOS " + CollapseSpaces(buffer);
    std::uint64_t memory = 0;
    size = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) info.ramGiB = WholeGiB(memory);
    size = sizeof(buffer);
    std::memset(buffer, 0, sizeof(buffer));
    if (!info.cpu && sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) == 0) info.cpu = CollapseSpaces(buffer);
#elif defined(_WIN32)
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (const auto function = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            if (function(&version) == 0) info.osVersion = "Windows " + std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + " build " + std::to_string(version.dwBuildNumber);
        }
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) info.ramGiB = WholeGiB(memory.ullTotalPhys);
#endif
    if (info.ramGiB && *info.ramGiB < 1) info.ramGiB.reset();
    return info;
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

namespace {

bool NamesHost(std::string_view text, const HostIdentifiers& host) {
    const auto lowered = Lower(text);
    for (const auto& word : host.words) {
        if (word.empty()) continue;
        const auto needle = Lower(word);
        if (needle.size() >= 3u) {
            if (lowered.find(needle) != std::string::npos) return true;
            continue;
        }
        // A one or two character name cannot be searched for as a substring without dropping most texts, so match it as a whole word.
        for (std::size_t at = lowered.find(needle); at != std::string::npos; at = lowered.find(needle, at + 1u)) {
            const bool startsWord = at == 0u || !std::isalnum(static_cast<unsigned char>(lowered[at - 1u]));
            const bool endsWord = at + needle.size() >= lowered.size() || !std::isalnum(static_cast<unsigned char>(lowered[at + needle.size()]));
            if (startsWord && endsWord) return true;
        }
    }
    return false;
}

}

std::optional<std::string> SafeText(std::string_view text, const HostIdentifiers& host) {
    std::size_t characters = 0;
    if (text.empty() || !ValidUtf8(text, characters) || characters > MaxTextCharacters) return std::nullopt;
    if (std::ranges::any_of(text, [](unsigned char character) { return character < 0x20u || character == 0x7fu; })) return std::nullopt;
    if (NamesHost(text, host)) return std::nullopt;
    // The IPv4 form must not depend on word boundaries: "lib_192.0.2.123", "fn192.0.2.123" and "v192.0.2.123" have none before the number.
    static const std::regex addresses(R"(((^|[^0-9])\d{1,3}(\.\d{1,3}){3}($|[^0-9]))|(([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})|((^|[^0-9A-Za-z:])(([0-9A-Fa-f]{1,4})?:){2,7}[0-9A-Fa-f]{0,4}($|[^0-9A-Za-z:]))|(\d{8,})|([0-9A-Fa-f]{16,}))");
    static const std::regex paths(R"((^|[\s"'(=,;:|<>\[{])(/[^\s/]|~/|%[A-Za-z_]+%|[A-Za-z]:[\\/]|\\\\|file:))");
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
    if (!matches(report.build.commit, "[0-9a-f]{40}")) report.build.commit.reset();
    if (!matches(report.build.release, "v[0-9A-Za-z][0-9A-Za-z._-]{0,62}")) report.build.release.reset();
    if (!matches(report.game.titleId, "[A-Z]{4}[0-9]{5}")) report.game.titleId.reset();
    if (!matches(report.game.contentVersion, "[0-9A-Za-z._-]{1,32}")) report.game.contentVersion.reset();
    // A value that fits its pattern can still be a host identifier (a version or release tag that is a user name).
    // The commit is forty hex digits and cannot carry a name, but a short hex-looking name ("dead", "face") would match inside it, so it is
    // not compared with the host's names.
    // These get the full text filter too (names, addresses, account numbers), not only the pattern.
    for (auto* value : {&report.build.release, &report.game.titleId, &report.game.contentVersion, &report.os}) {
        if (*value && !SafeText(**value, host).has_value()) value->reset();
    }
    if (report.game.name) report.game.name = SafeText(*report.game.name, host);
    if (report.stop.source) {
        report.stop.source = SafeText(*report.stop.source, host);
        if (report.stop.source && (report.stop.source->find("..") != std::string::npos || !std::regex_match(*report.stop.source, std::regex(R"([A-Za-z0-9_][A-Za-z0-9_./+-]{0,254}:[0-9]{1,9})")))) report.stop.source.reset();
    }
    if (report.stop.message) report.stop.message = SafeText(*report.stop.message, host);
    if (report.os && *report.os != "linux" && *report.os != "windows" && *report.os != "macos") report.os.reset();
    // The machine: model strings pass the same text filter as everything else; a value outside its schema pattern is left out.
    // Model names are public (a distribution, a CPU, a GPU) and are not compared with the host's own names: a machine named after its
    // distribution would otherwise lose its OS version. Addresses, account numbers, paths and e-mail addresses are still filtered.
    const HostIdentifiers publicNames;
    const auto text = [&](std::optional<std::string>& value) {
        if (value && (value->empty() || !SafeText(*value, publicNames).has_value())) value.reset();
    };
    text(report.machine.osVersion);
    text(report.machine.cpu);
    if (report.machine.ramGiB && (*report.machine.ramGiB < 1 || *report.machine.ramGiB > 1048576)) report.machine.ramGiB.reset();
    if (report.machine.gpu) {
        auto& gpu = *report.machine.gpu;
        text(gpu.name);
        text(gpu.driver);
        if (gpu.vendor && *gpu.vendor != "AMD" && *gpu.vendor != "NVIDIA" && *gpu.vendor != "Intel" && *gpu.vendor != "Apple" && *gpu.vendor != "Other") gpu.vendor.reset();
        if (gpu.vulkan && !matches(gpu.vulkan, "[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,5}")) gpu.vulkan.reset();
    }
    if (!Hex32(report.runId)) report.runId = std::string(32u, '0');
    return report;
}

}


namespace {

const char* StatusName(Status status) {
    return status == Status::Accepted ? "accepted" : status == Status::Rejected ? "rejected" : "unknown";
}

const char* FormatUsageName(int usage) {
    static constexpr const char* names[] = {"sampled", "storage", "color-target", "depth-target", "transfer", "other"};
    return names[usage >= 0 && usage < 6 ? usage : 5];
}

const char* DescriptorTypeName(int type) {
    static constexpr const char* names[] = {"UniformBuffer", "StorageBuffer", "UniformTexelBuffer", "StorageTexelBuffer", "SampledImage", "StorageImage", "Sampler"};
    return names[type >= 0 && type < 7 ? type : 0];
}

const char* DescriptorUsageName(int usage) {
    static constexpr const char* names[] = {"read", "write", "atomic", "other"};
    return names[usage >= 0 && usage < 4 ? usage : 3];
}

bool Matches(const std::string& text, const char* pattern) {
    return std::regex_match(text, std::regex(pattern));
}

std::int64_t CountOf(const FunctionEntry& entry) { return entry.calls; }
template <typename Entry> std::int64_t CountOf(const Entry& entry) { return entry.count; }

// Drops entries that do not fit the schema or that name the host, orders lists by count (ties by their text) and applies the list caps.
// Dropped entries are counted in `truncated`, so a list never looks complete when it is not.
void NormalizeObservations(Observations& o, const HostIdentifiers& host) {
    const auto clean = [&](auto& list, std::int64_t& dropped, std::size_t cap, auto valid, auto order) {
        const auto before = list.size();
        std::erase_if(list, [&](auto& entry) { return CountOf(entry) < 1 || !valid(entry); });
        dropped += static_cast<std::int64_t>(before - list.size());
        std::stable_sort(list.begin(), list.end(), order);
        if (list.size() > cap) {
            dropped += static_cast<std::int64_t>(list.size() - cap);
            list.resize(cap);
        }
    };
    const auto optionalOk = [&](std::optional<std::string>& value, const char* pattern) {
        // A structured field is checked like free text: names, addresses and account numbers fit the patterns but are not allowed.
        if (value && (!Matches(*value, pattern) || !SafeText(*value, host).has_value())) value.reset();
    };
    for (auto& entry : o.functions) optionalOk(entry.library, "[A-Za-z0-9_.-]{1,64}");
    clean(o.functions, o.truncated.functions, MaxListedFunctions,
          [&](const FunctionEntry& entry) { return Matches(entry.name, "[A-Za-z0-9_.$]{1,128}") && SafeText(entry.name, host).has_value(); },
          [](const FunctionEntry& a, const FunctionEntry& b) { return a.calls != b.calls ? a.calls > b.calls : std::tie(a.library, a.name) < std::tie(b.library, b.name); });
    for (auto& entry : o.unsupported) {
        optionalOk(entry.encoding, "[A-Za-z0-9_]{1,32}");
        optionalOk(entry.opcode, "[A-Za-z0-9_]{1,64}");
        if (entry.opcodeValue && (*entry.opcodeValue < 0 || *entry.opcodeValue > 4294967295LL)) entry.opcodeValue.reset();
        if (entry.modifiers) {
            bool ok = entry.modifiers->size() <= 16u;
            for (const auto& modifier : *entry.modifiers) ok = ok && Matches(modifier, "[a-z0-9][a-z0-9-]{0,31}") && SafeText(modifier, host).has_value();
            if (!ok) entry.modifiers.reset();
        }
    }
    clean(o.unsupported, o.truncated.unsupported, MaxListedFunctions, [](const UnsupportedEntry&) { return true; },
          [](const UnsupportedEntry& a, const UnsupportedEntry& b) {
              return a.count != b.count ? a.count > b.count : std::tie(a.encoding, a.opcode, a.opcodeValue) < std::tie(b.encoding, b.opcode, b.opcodeValue);
          });
    for (auto& entry : o.imageFormats) optionalOk(entry.name, "[A-Za-z0-9_]{1,64}");
    clean(o.imageFormats, o.truncated.imageFormats, MaxListedFormats, [](const FormatEntry& entry) { return entry.format >= 0 && entry.format <= 4294967295LL; },
          [](const FormatEntry& a, const FormatEntry& b) { return a.count != b.count ? a.count > b.count : std::tie(a.format, a.usage) < std::tie(b.format, b.usage); });
    clean(o.descriptors, o.truncated.descriptors, MaxListedFormats, [](const DescriptorEntry& entry) { return entry.type >= 0 && entry.type < 7; },
          [](const DescriptorEntry& a, const DescriptorEntry& b) { return a.count != b.count ? a.count > b.count : std::tie(a.type, a.usage, a.status) < std::tie(b.type, b.usage, b.status); });
    // A section that was not collected is never written as if it were empty (the schema ties the two together).
    if (!o.coverage.milestones) {
        o.loaded = o.started = o.firstDraw = o.firstPresent = std::nullopt;
        o.framesPresented = 0;
        o.averageFps = o.fpsWindowSeconds = std::nullopt;
    }
    if (!o.coverage.missing) o.functions.clear(), o.truncated.functions = 0;
    if (!o.coverage.shaders) o.shadersCompiled = o.shadersFailed = 0, o.cacheHits.reset();
    if (!o.coverage.unsupported) o.unsupported.clear(), o.truncated.unsupported = 0;
    if (!o.coverage.imageFormats) o.imageFormats.clear(), o.truncated.imageFormats = 0;
    if (!o.coverage.descriptors) o.descriptors.clear(), o.truncated.descriptors = 0;
    if (o.averageFps && !(*o.averageFps >= 0.0 && *o.averageFps <= 1.0e6)) o.averageFps.reset();
    if (o.fpsWindowSeconds && !(*o.fpsWindowSeconds >= 0.001 && *o.fpsWindowSeconds <= 1.0e9)) o.fpsWindowSeconds.reset();
    if (o.framesPresented <= 1 || !o.fpsWindowSeconds) o.averageFps.reset(), o.fpsWindowSeconds.reset();
    if (o.framesPresented < 0) o.framesPresented = 0;
    for (auto* seconds : {&o.loaded, &o.started, &o.firstDraw, &o.firstPresent}) {
        if (*seconds && !(**seconds >= 0.0 && **seconds <= 1.0e9)) seconds->reset();
    }
    for (auto* counter : {&o.shadersCompiled, &o.shadersFailed}) if (*counter < 0) *counter = 0;
    if (o.cacheHits && *o.cacheHits < 0) o.cacheHits = 0;
}

std::string RenderReport(const Report& report) {
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
    json.String("osVersion", report.machine.osVersion);
    json.String("cpu", report.machine.cpu);
    json.Int("ramGiB", report.machine.ramGiB);
    if (report.machine.gpu) {
        json.BeginObject("gpu");
        json.String("vendor", report.machine.gpu->vendor);
        json.String("name", report.machine.gpu->name);
        json.String("driver", report.machine.gpu->driver);
        json.String("vulkan", report.machine.gpu->vulkan);
        json.EndObject();
    } else {
        json.Null("gpu");
    }
    json.EndObject();
    json.BeginObject("run");
    json.String("id", std::string_view(report.runId));
    json.Number("seconds", report.seconds);
    json.String("exit", std::string_view(ExitName(report.exit)));
    json.Int("exitCode", report.exitCode);
    const auto& o = report.observed;
    const char* stage = o.firstPresent ? "presents" : o.firstDraw ? "first-draw" : o.started ? "starts" : o.loaded ? "loads" : "unknown";
    json.String("stage", std::string_view(stage));
    json.BeginObject("milestones");
    if (o.loaded) json.Number("loaded", *o.loaded);
    if (o.started) json.Number("started", *o.started);
    if (o.firstDraw) json.Number("firstDraw", *o.firstDraw);
    if (o.firstPresent) json.Number("firstPresent", *o.firstPresent);
    json.EndObject();
    json.Int("framesPresented", o.framesPresented);
    if (o.averageFps) json.Number("averageFps", *o.averageFps); else json.Null("averageFps");
    if (o.fpsWindowSeconds) json.Number("fpsWindowSeconds", *o.fpsWindowSeconds); else json.Null("fpsWindowSeconds");
    json.Bool("recovered", report.recovered);
    json.Bool("incomplete", report.incomplete);
    json.EndObject();
    json.BeginObject("coverage");
    json.Bool("milestones", o.coverage.milestones);
    json.Bool("missing", o.coverage.missing);
    json.Bool("shaders", o.coverage.shaders);
    json.Bool("unsupported", o.coverage.unsupported);
    json.Bool("imageFormats", o.coverage.imageFormats);
    json.Bool("descriptors", o.coverage.descriptors);
    json.EndObject();
    json.BeginObject("truncated");
    json.Int("functions", o.truncated.functions);
    json.Int("unsupported", o.truncated.unsupported);
    json.Int("imageFormats", o.truncated.imageFormats);
    json.Int("descriptors", o.truncated.descriptors);
    json.EndObject();
    json.BeginObject("missing");
    json.BeginArray("functions");
    for (const auto& entry : o.functions) {
        json.BeginObject();
        json.String("library", entry.library);
        json.String("name", std::string_view(entry.name));
        json.Int("calls", entry.calls);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    json.BeginObject("shaders");
    json.Int("compiled", o.shadersCompiled);
    json.Int("failed", o.shadersFailed);
    if (o.cacheHits) json.Int("cacheHits", o.cacheHits);
    json.BeginArray("unsupported");
    for (const auto& entry : o.unsupported) {
        json.BeginObject();
        json.String("encoding", entry.encoding);
        json.String("opcode", entry.opcode);
        json.Int("opcodeValue", entry.opcodeValue);
        if (entry.modifiers) {
            json.BeginArray("modifiers");
            for (const auto& modifier : *entry.modifiers) json.String(nullptr, std::string_view(modifier));
            json.EndArray();
        } else {
            json.Null("modifiers");
        }
        json.Int("count", entry.count);
        json.EndObject();
    }
    json.EndArray();
    json.BeginArray("imageFormats");
    for (const auto& entry : o.imageFormats) {
        json.BeginObject();
        json.Int("format", entry.format);
        json.String("name", entry.name);
        json.String("status", std::string_view(StatusName(entry.status)));
        json.String("usage", std::string_view(FormatUsageName(entry.usage)));
        json.Int("count", entry.count);
        json.EndObject();
    }
    json.EndArray();
    json.BeginArray("descriptors");
    for (const auto& entry : o.descriptors) {
        json.BeginObject();
        json.String("type", std::string_view(DescriptorTypeName(entry.type)));
        json.String("status", std::string_view(StatusName(entry.status)));
        json.String("usage", std::string_view(DescriptorUsageName(entry.usage)));
        json.Int("count", entry.count);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    json.BeginObject("stop");
    json.String("kind", std::string_view(KindName(report.stop.kind)));
    json.String("reason", std::string_view(ReasonName(report.stop.reason)));
    json.String("message", report.stop.message ? std::string_view(*report.stop.message) : std::string_view(StopMessage(report.stop)));
    json.String("source", report.stop.source);
    json.EndObject();
    json.EndObject();
    return json.Take();
}

}

std::string Serialize(const Report& original, const HostIdentifiers& host) {
    Report report = Sanitized(original, host);
    NormalizeObservations(report.observed, host);
    for (;;) {
        auto text = RenderReport(report);
        if (text.size() <= MaxBytes) return text;
        // Over the size limit: the lowest-ranked entry of the longest list goes first; the build, game and stop details never do.
        auto& o = report.observed;
        const std::size_t sizes[4] = {o.functions.size(), o.unsupported.size(), o.imageFormats.size(), o.descriptors.size()};
        std::size_t longest = 0;
        for (std::size_t index = 1; index < 4; ++index) if (sizes[index] > sizes[longest]) longest = index;
        if (sizes[longest] == 0) throw std::runtime_error("run report: the serialized report exceeds 65,536 bytes");
        switch (longest) {
            case 0: o.functions.pop_back(); ++o.truncated.functions; break;
            case 1: o.unsupported.pop_back(); ++o.truncated.unsupported; break;
            case 2: o.imageFormats.pop_back(); ++o.truncated.imageFormats; break;
            default: o.descriptors.pop_back(); ++o.truncated.descriptors; break;
        }
    }
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

// Every folder from the drive root down to `folder` is opened without following a link or junction and kept open. The handles are
// opened with a share mode that allows no rename and no delete, so none of the folders can be replaced by a link while the chain is
// held: a check made on a name stays true for the writes that follow, which is what a directory handle gives on POSIX.
class PinnedChain {
public:
    PinnedChain() = default;
    PinnedChain(const PinnedChain&) = delete;
    PinnedChain& operator=(const PinnedChain&) = delete;
    ~PinnedChain() {
        for (const HANDLE handle : handles) CloseHandle(handle);
    }
    // False when a folder is missing, is not a folder, or is a link, a junction or any other reparse point.
    bool Pin(const fs::path& folder) {
        const auto absolute = fs::absolute(folder).lexically_normal();
        auto at = absolute.root_path();
        if (at.empty()) return false;
        std::vector<fs::path> chain{at};
        for (const auto& part : absolute.relative_path()) {
            if (part.empty() || part == L".") continue;
            at /= part;
            chain.push_back(at);
        }
        for (const auto& path : chain) {
            const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            handles.push_back(handle);
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(handle, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
        }
        return true;
    }

private:
    std::vector<HANDLE> handles;
};
#else
// Opens `folder` one component at a time with O_NOFOLLOW, so a link anywhere in the chain (not only in the last name) fails the open.
int OpenFolderNoFollow(const fs::path& folder) {
    const auto absolute = fs::absolute(folder).lexically_normal();
    std::vector<fs::path> parts;
    for (const auto& part : absolute.relative_path()) {
        if (!part.empty() && part != ".") parts.push_back(part);
    }
#ifdef O_PATH
    // A folder on the way only has to be passed through: O_PATH needs search permission and no read permission.
    constexpr int passThrough = O_PATH;
#else
    constexpr int passThrough = O_RDONLY;
#endif
    int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (current < 0) return -1;
        const bool last = index + 1u == parts.size();
        // The last folder is the one the files are made in, and it is flushed, so it is opened for reading when it may be.
        int next = ::openat(current, parts[index].c_str(), (last ? O_RDONLY : passThrough) | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#ifdef O_PATH
        if (next < 0 && last && errno == EACCES) next = ::openat(current, parts[index].c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#endif
        ::close(current);
        current = next;
    }
    return current;
}
#endif

}

bool WriteAtomically(const fs::path& target, std::string_view bytes, std::string_view uniqueName, std::string& error) {
    const auto parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    const auto leaf = target.filename().string();
    const auto temporaryName = "." + leaf + "." + std::string(uniqueName) + ".tmp";
#ifdef _WIN32
    bool reparse = false, directory = false;
    // Held until the function returns, so the folders cannot be swapped between the check and the writes.
    PinnedChain chain;
    if (!chain.Pin(parent)) {
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
    const int folder = OpenFolderNoFollow(parent);
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
    // Without this flush the rename can be lost on a power cut; a file system that cannot flush a folder says EINVAL or ENOTSUP.
    const bool flushed = ::fsync(folder) == 0 || errno == EINVAL || errno == ENOTSUP || errno == EBADF;
    ::close(folder);
    if (!flushed) error = "flushing the destination folder failed";
    return flushed;
#endif
}

bool ReadBounded(const fs::path& path, std::size_t limit, std::string& bytes) {
    bytes.clear();
#ifdef _WIN32
    PinnedChain chain;
    if (!chain.Pin(path.parent_path())) return false;
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
    const int folder = OpenFolderNoFollow(path.parent_path().empty() ? fs::path(".") : path.parent_path());
    if (folder < 0) return false;
    const int descriptor = ::openat(folder, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    ::close(folder);
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

void* FileLock::NativeHandle() const {
#ifdef _WIN32
    return handle ? handle->file : nullptr;
#else
    return nullptr;
#endif
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

namespace {

bool IsNull(const Json& object, std::string_view key) {
    const auto* value = object.type == Json::Type::Object ? object.Find(key) : nullptr;
    return value != nullptr && value->type == Json::Type::Null;
}

std::optional<std::int64_t> IntegerOf(const Json& object, std::string_view key) {
    const auto* value = Member(object, key, Json::Type::Number);
    if (value == nullptr || value->number != std::floor(value->number) || value->number < -9.0e15 || value->number > 9.0e15) return std::nullopt;
    return static_cast<std::int64_t>(value->number);
}

std::optional<std::string> TextOf(const Json& object, std::string_view key) {
    const auto* value = Member(object, key, Json::Type::String);
    return value != nullptr ? std::optional<std::string>(value->text) : std::nullopt;
}

int IndexOf(const std::string& text, std::initializer_list<const char*> names) {
    int index = 0;
    for (const char* name : names) {
        if (text == name) return index;
        ++index;
    }
    return -1;
}

bool StatusOf(const Json& object, Status& status) {
    const auto text = TextOf(object, "status");
    if (!text) return false;
    const int index = IndexOf(*text, {"accepted", "rejected", "unknown"});
    if (index < 0) return false;
    status = static_cast<Status>(index);
    return true;
}

// Reads back what the writer put in a checkpoint. Anything missing, mistyped or unknown fails the whole checkpoint.
bool ParseObservations(const Json& root, const Json& run, Observations& o) {
    const auto* milestones = Member(run, "milestones", Json::Type::Object);
    const auto framesPresented = IntegerOf(run, "framesPresented");
    if (milestones == nullptr || !framesPresented) return false;
    const auto seconds = [&](const char* key) -> std::optional<double> {
        const auto* value = Member(*milestones, key, Json::Type::Number);
        return value != nullptr ? std::optional<double>(value->number) : std::nullopt;
    };
    o.loaded = seconds("loaded");
    o.started = seconds("started");
    o.firstDraw = seconds("firstDraw");
    o.firstPresent = seconds("firstPresent");
    o.framesPresented = *framesPresented;
    if (const auto* fps = Member(run, "averageFps", Json::Type::Number)) o.averageFps = fps->number;
    else if (!IsNull(run, "averageFps")) return false;
    if (const auto* window = Member(run, "fpsWindowSeconds", Json::Type::Number)) o.fpsWindowSeconds = window->number;
    else if (!IsNull(run, "fpsWindowSeconds")) return false;
    const auto* coverage = Member(root, "coverage", Json::Type::Object);
    const auto* truncated = Member(root, "truncated", Json::Type::Object);
    const auto* missing = Member(root, "missing", Json::Type::Object);
    const auto* shaders = Member(root, "shaders", Json::Type::Object);
    if (coverage == nullptr || truncated == nullptr || missing == nullptr || shaders == nullptr) return false;
    const auto flag = [&](const char* key, bool& out) {
        const auto* value = Member(*coverage, key, Json::Type::Bool);
        if (value == nullptr) return false;
        out = value->boolean;
        return true;
    };
    if (!flag("milestones", o.coverage.milestones) || !flag("missing", o.coverage.missing) || !flag("shaders", o.coverage.shaders) ||
        !flag("unsupported", o.coverage.unsupported) || !flag("imageFormats", o.coverage.imageFormats) || !flag("descriptors", o.coverage.descriptors)) return false;
    const auto functions = IntegerOf(*truncated, "functions"), unsupported = IntegerOf(*truncated, "unsupported");
    const auto formats = IntegerOf(*truncated, "imageFormats"), descriptors = IntegerOf(*truncated, "descriptors");
    if (!functions || !unsupported || !formats || !descriptors) return false;
    o.truncated = {*functions, *unsupported, *formats, *descriptors};
    const auto compiled = IntegerOf(*shaders, "compiled"), failed = IntegerOf(*shaders, "failed");
    if (!compiled || !failed) return false;
    o.shadersCompiled = *compiled;
    o.shadersFailed = *failed;
    if (shaders->Find("cacheHits") != nullptr) {
        o.cacheHits = IntegerOf(*shaders, "cacheHits");
        if (!o.cacheHits) return false;
    }
    const auto* functionList = Member(*missing, "functions", Json::Type::Array);
    const auto* unsupportedList = Member(*shaders, "unsupported", Json::Type::Array);
    const auto* formatList = Member(*shaders, "imageFormats", Json::Type::Array);
    const auto* descriptorList = Member(*shaders, "descriptors", Json::Type::Array);
    if (functionList == nullptr || unsupportedList == nullptr || formatList == nullptr || descriptorList == nullptr) return false;
    for (const auto& item : functionList->items) {
        const auto name = TextOf(item, "name"), calls = std::optional<std::string>(), library = TextOf(item, "library");
        const auto count = IntegerOf(item, "calls");
        if (!name || !count || (!library && !IsNull(item, "library"))) return false;
        o.functions.push_back({library, *name, *count});
    }
    for (const auto& item : unsupportedList->items) {
        UnsupportedEntry entry;
        entry.encoding = TextOf(item, "encoding");
        entry.opcode = TextOf(item, "opcode");
        entry.opcodeValue = IntegerOf(item, "opcodeValue");
        const auto count = IntegerOf(item, "count");
        if (!count || (!entry.encoding && !IsNull(item, "encoding")) || (!entry.opcode && !IsNull(item, "opcode")) || (!entry.opcodeValue && !IsNull(item, "opcodeValue"))) return false;
        entry.count = *count;
        if (const auto* modifiers = Member(item, "modifiers", Json::Type::Array)) {
            entry.modifiers = std::vector<std::string>();
            for (const auto& modifier : modifiers->items) {
                if (modifier.type != Json::Type::String) return false;
                entry.modifiers->push_back(modifier.text);
            }
        } else if (!IsNull(item, "modifiers")) {
            return false;
        }
        o.unsupported.push_back(std::move(entry));
    }
    for (const auto& item : formatList->items) {
        FormatEntry entry;
        const auto format = IntegerOf(item, "format"), count = IntegerOf(item, "count");
        const auto usage = TextOf(item, "usage");
        entry.name = TextOf(item, "name");
        if (!format || !count || !usage || !StatusOf(item, entry.status) || (!entry.name && !IsNull(item, "name"))) return false;
        entry.usage = IndexOf(*usage, {"sampled", "storage", "color-target", "depth-target", "transfer", "other"});
        if (entry.usage < 0) return false;
        entry.format = *format;
        entry.count = *count;
        o.imageFormats.push_back(std::move(entry));
    }
    for (const auto& item : descriptorList->items) {
        DescriptorEntry entry;
        const auto type = TextOf(item, "type"), usage = TextOf(item, "usage");
        const auto count = IntegerOf(item, "count");
        if (!type || !usage || !count || !StatusOf(item, entry.status)) return false;
        entry.type = IndexOf(*type, {"UniformBuffer", "StorageBuffer", "UniformTexelBuffer", "StorageTexelBuffer", "SampledImage", "StorageImage", "Sampler"});
        entry.usage = IndexOf(*usage, {"read", "write", "atomic", "other"});
        if (entry.type < 0 || entry.usage < 0) return false;
        entry.count = *count;
        o.descriptors.push_back(entry);
    }
    return true;
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
        report.build.commit = NullableText(*build, "commit", "[0-9a-f]{40}");
        report.build.release = NullableText(*build, "release", "v[0-9A-Za-z][0-9A-Za-z._-]{0,62}");
        if (const auto* modified = Member(*build, "modified", Json::Type::Bool)) report.build.modified = modified->boolean;
    }
    if (const auto* game = Member(root, "game", Json::Type::Object)) {
        report.game.titleId = NullableText(*game, "titleId", "[A-Z]{4}[0-9]{5}");
        report.game.contentVersion = NullableText(*game, "contentVersion", "[0-9A-Za-z._-]{1,32}");
        if (const auto* name = Member(*game, "name", Json::Type::String)) report.game.name = SafeText(name->text, host);
    }
    if (const auto* system = Member(root, "system", Json::Type::Object)) {
        report.os = NullableText(*system, "os", "linux|windows|macos");
        if (const auto* version = Member(*system, "osVersion", Json::Type::String)) report.machine.osVersion = version->text;
        if (const auto* cpu = Member(*system, "cpu", Json::Type::String)) report.machine.cpu = cpu->text;
        report.machine.ramGiB = IntegerOf(*system, "ramGiB");
        if (const auto* gpu = Member(*system, "gpu", Json::Type::Object)) {
            GpuInfo parsed;
            if (const auto* vendor = Member(*gpu, "vendor", Json::Type::String)) parsed.vendor = vendor->text;
            if (const auto* name = Member(*gpu, "name", Json::Type::String)) parsed.name = name->text;
            if (const auto* driver = Member(*gpu, "driver", Json::Type::String)) parsed.driver = driver->text;
            if (const auto* vulkan = Member(*gpu, "vulkan", Json::Type::String)) parsed.vulkan = vulkan->text;
            report.machine.gpu = std::move(parsed);
        }
    }
    if (!ParseObservations(root, *run, report.observed)) return std::nullopt;
    // Our writer is deterministic, so a genuine checkpoint is byte for byte what the fields read back would serialize to.
    // Anything else (a missing object, an extra key, a value the filters would change) is rejected whole, never republished in part.
    if (Serialize(report, host) != bytes) return std::nullopt;
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

namespace {

struct Component {
    std::string name;
    std::string commit;
    int modified = -1;
};

std::mutex componentsMutex;

std::vector<Component>& Components() {
    static std::vector<Component> components;
    return components;
}

}

void RegisterComponent(std::string_view name, std::string_view commit, int modified) {
    std::lock_guard guard(componentsMutex);
    auto& components = Components();
    for (auto& component : components) {
        if (component.name == name) {
            component.commit = commit;
            component.modified = modified;
            return;
        }
    }
    components.push_back({std::string(name), std::string(commit), modified});
}

// `modified` is false only when every required library registered the commit libc was built from, and none registered another one.
// A library from another commit makes the build a mix, so modified is true; a missing or unstamped library leaves it unknown.
BuildInfo Collector::Provenance() const {
    BuildInfo build = settings.build;
    if (settings.requiredComponents.empty() || !build.modified) return build;
    std::lock_guard guard(componentsMutex);
    bool modified = *build.modified;
    bool unknown = false;
    for (const auto& component : Components()) {
        if (!build.commit || component.commit != *build.commit) modified = true;
        else if (component.modified < 0) unknown = true;
        else modified = modified || component.modified != 0;
    }
    for (const auto& name : settings.requiredComponents) {
        if (std::ranges::find(Components(), name, &Component::name) == Components().end()) unknown = true;
    }
    if (modified) build.modified = true;
    else if (unknown) build.modified.reset();
    else build.modified = false;
    return build;
}

#ifdef _WIN32
namespace {

// What the process-exit cleanup needs, kept in plain storage so that it needs no lock, no allocation and no library call beyond Win32.
wchar_t g_exitCheckpoint[1024];
std::atomic<HANDLE> g_exitLock{INVALID_HANDLE_VALUE};

void RememberForExit(const fs::path& checkpoint, void* lock) {
    const auto text = checkpoint.native();
    g_exitCheckpoint[0] = L'\0';
    if (text.size() < std::size(g_exitCheckpoint)) {
        std::copy(text.begin(), text.end(), g_exitCheckpoint);
        g_exitCheckpoint[text.size()] = L'\0';
    }
    g_exitLock = lock != nullptr ? static_cast<HANDLE>(lock) : INVALID_HANDLE_VALUE;
}

void ForgetForExit() {
    g_exitLock = INVALID_HANDLE_VALUE;
    g_exitCheckpoint[0] = L'\0';
}

}
#endif

struct Collector::GameState {
    std::mutex mutex;
    std::condition_variable done;
    GameInfo cache;
    bool known = false;
    int attempts = 0;
    int pending = 0;
};

// Capacity of each table while the run is going; the lists written to the report are cut to their own caps later.
constexpr std::size_t TrackedEntries = 512;

struct Collector::Tables {
    std::timed_mutex mutex;
    std::vector<FunctionEntry> functions;
    std::vector<UnsupportedEntry> unsupported;
    std::vector<FormatEntry> imageFormats;
    std::vector<DescriptorEntry> descriptors;
    Truncated dropped;
    std::set<std::string> droppedFunctionKeys, droppedUnsupportedKeys, droppedFormatKeys;
    // More distinct entries were dropped than the key sets remember: the counts are lower bounds and the report says it is incomplete.
    bool droppedSaturated = false;
    // A failure that ended the run for certain (the driver reported it as fatal).
    std::optional<Stop> fatalStop;
    // Failures noted where they were thrown, each with the message of the exception it threw. The game may catch them.
    struct Soft {
        Stop stop;
        std::string message;
        bool prefixed = false;
    };
    std::vector<Soft> softStops;
};

// A failure filed under the message of the error it threw. The same message is filed once, so a failure that repeats cannot fill the
// list and hide a different one that comes later; the list keeps the most recent distinct ones.
void AddSoft(Collector::Tables& tables, Stop stop, std::string message, bool prefixed = false) {
    if (message.empty()) return;
    auto& list = tables.softStops;
    const auto same = std::ranges::find(list, message, &Collector::Tables::Soft::message);
    if (same != list.end()) return;
    if (list.size() >= 8u) list.erase(list.begin());
    list.push_back({std::move(stop), std::move(message), prefixed});
}

Collector::Collector(Settings value) : settings(std::move(value)), game(std::make_shared<GameState>()), tables(std::make_shared<Tables>()) {
    for (auto& nanos : milestoneNanos) nanos = -1;
    for (auto& count : lost) count = 0;
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
    {
        // What the host is; read once. The GPU is added when the device code reports it.
        auto probed = settings.machine ? *settings.machine : ProbeHost();
        std::lock_guard guard(machineMutex);
        machineInfo = std::move(probed);
    }
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
#ifdef _WIN32
        RememberForExit(checkpointPath, lock->NativeHandle());
#endif
        break;
    }
    if (directory.empty()) {
        std::fputs("[run-report] no writable folder for the run report; reporting is off for this run\n", stderr);
        return false;
    }
    started_ = true;
    if (settings.worker) {
        workerWanted = workerRunning = true;
        worker = std::thread([this] { Work(); });
    }
    NoteGame();
    return true;
}

void Collector::NoteGame() {
    try {
        RefreshGame();
        WriteCheckpoint();
    } catch (...) {
    }
}

void Collector::BeginWorker() {
    try {
        std::lock_guard guard(mutex);
        if (!started_ || state != 0 || stopWorker || workerRunning) return;
        if (worker.joinable()) worker.join();
        workerWanted = workerRunning = true;
        worker = std::thread([this] { Work(); });
    } catch (...) {
    }
}

// Reopens a run whose relaunch failed. The clean report written for the handoff is taken back, so a run that goes on (and may be
// killed) is not left with a clean result next to it. A worker that is still blocked is not joined: it carries on by itself.
bool Collector::Resume() {
    try {
        {
            std::lock_guard guard(mutex);
            if (!started_ || state != 2) return false;
        }
        if (finalized) {
            bool busy = false;
            auto acquired = FileLock::TryAcquire(lockPath, busy);
            if (!acquired) return false;
            lock = std::move(acquired);
        }
        {
            std::lock_guard guard(mutex);
            state = 0;
            stopWorker = false;
            finalized = false;
        }
        if (!WriteCheckpoint()) return false;
#ifdef _WIN32
        RememberForExit(checkpointPath, lock->NativeHandle());
#endif
        if (reportPublished) {
            // Only the report this run wrote is taken back: a newer report from another run in the same folder is not ours to delete.
            std::string current;
            std::error_code error;
            if (ReadBounded(reportPath, MaxBytes, current) && current == publishedBytes) fs::remove(reportPath, error);
            reportPublished = false;
            publishedBytes.clear();
        }
        std::lock_guard guard(mutex);
        if (!workerRunning) {
            if (worker.joinable()) worker.join();
            if (workerWanted) {
                workerRunning = true;
                worker = std::thread([this] { Work(); });
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void Collector::RefreshGame(bool force) {
    const auto state = game;
    std::unique_lock guard(state->mutex);
    if (state->known || !settings.game) return;
    if (state->pending == 0) {
        // The worker gives up after a few failures; the end of a clean run may still try once more.
        if (!force && state->attempts >= 5) return;
        ++state->attempts;
        ++state->pending;
        // The provider may take the metadata lock and parse files. It runs on its own thread and is waited for a moment at most, so
        // neither the checkpoint worker nor a finishing thread can be held by it.
        std::thread([provider = settings.game, state] {
            GameInfo fields;
            try {
                fields = provider();
            } catch (...) {
            }
            std::lock_guard guard(state->mutex);
            if (fields.titleId) {
                state->cache = std::move(fields);
                state->known = true;
            }
            --state->pending;
            state->done.notify_all();
        }).detach();
    }
    state->done.wait_for(guard, std::chrono::seconds(1), [&state] { return state->pending == 0; });
}

Report Collector::Snapshot(Exit exit, Stop stop, std::optional<std::int64_t> exitCode, bool recovered) const {
    Report report;
    report.generated = FormatUtc(std::chrono::system_clock::now());
    report.build = Provenance();
    {
        std::lock_guard guard(machineMutex);
        report.machine = machineInfo;
    }
    {
        std::lock_guard guard(game->mutex);
        report.game = game->cache;
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
    report.observed = Observe();
    // A report that missed observations says so, whether or not it was recovered.
    if (report.observed.lostObservations) report.incomplete = true;
    return report;
}

bool Collector::WriteCheckpoint() {
    return WriteCheckpoint(settings.lockWait);
}

bool Collector::WriteCheckpoint(std::chrono::milliseconds wait) {
    try {
        std::unique_lock guard(writeMutex, std::defer_lock);
        if (!guard.try_lock_for(wait)) return false;
        // A finish that has begun owns the files: a late checkpoint would be taken for a killed run.
        if (started_ && state != 0) return false;
        if (settings.duringCheckpointWrite) settings.duringCheckpointWrite();
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
        if (changed.wait_for(guard, settings.checkpointInterval, [this] { return stopWorker || checkpointWanted.load(); })) {
            if (stopWorker) break;
            checkpointWanted = false;
        }
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
    workerRunning = false;
    changed.notify_all();
}

namespace {

std::int64_t NanosSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

}

// The source of a call site as `core/...:line`, or nothing when the compile-time path is not inside the source tree.
std::string UnsupportedFormatKey(unsigned format) {
    return "unsupported guest texture format " + std::to_string(format);
}

std::optional<std::string> SourceOf(std::string_view file, int line) {
    // `core/` must start a path segment: `/x/notcore/a.cpp` is not inside the source tree.
    auto at = file.rfind("core/");
    while (at != std::string_view::npos && at != 0u && file[at - 1u] != '/' && file[at - 1u] != '\\') at = at == 0u ? std::string_view::npos : file.rfind("core/", at - 1u);
    if (at == std::string_view::npos || line < 1) return std::nullopt;
    const auto relative = std::string(file.substr(at)) + ":" + std::to_string(line);
    if (relative.find("..") != std::string::npos || !std::regex_match(relative, std::regex(R"([A-Za-z0-9_][A-Za-z0-9_./+-]{0,254}:[0-9]{1,9})"))) return std::nullopt;
    return relative;
}

void Collector::CheckpointSoon() {
    // Never writes here: the callers are hooks on the render path and under the GPU lock. The worker writes soon after; the number of
    // requests is bounded so a hot path cannot turn into file I/O. Without a worker the next start of the worker or the finish does.
    if (!started_ || state != 0 || checkpointsRequested.fetch_add(1) >= 64) return;
    checkpointWanted = true;
    changed.notify_all();
    // Where the worker could not start while the libraries loaded, the first request starts it (the hooks run on ordinary threads).
    if (settings.deferredWorker) BeginWorker();
}

std::unique_lock<std::timed_mutex> Collector::LockTablesForTest() {
    return std::unique_lock<std::timed_mutex>(tables->mutex);
}

void Collector::NoteGpu(GpuInfo gpu) {
    {
        std::lock_guard guard(machineMutex);
        machineInfo.gpu = std::move(gpu);
    }
    CheckpointSoon();
}

void Collector::NoteLost(int section) {
    lost[section].fetch_add(1, std::memory_order_relaxed);
}

void Collector::NoteMilestone(int milestone) {
    if (milestone < 0 || milestone > 3 || !started_) return;
    std::int64_t unset = -1;
    if (milestoneNanos[milestone].compare_exchange_strong(unset, NanosSince(started))) CheckpointSoon();
}

void Collector::NoteFramePresented() {
    if (!started_) return;
    const auto now = NanosSince(started);
    if (frames.fetch_add(1, std::memory_order_relaxed) == 0) {
        firstFrameNanos = now;
        NoteMilestone(3);
    }
    lastFrameNanos = now;
}

void Collector::NoteMissingFunction(std::string_view library, std::string_view name, std::string_view source, std::string_view terminatingMessage) {
    if (!started_) return;
    bool added = false;
    {
        std::unique_lock guard(tables->mutex, std::defer_lock);
        if (!guard.try_lock_for(settings.tableWait)) {
            NoteLost(0);
            return;
        }
        auto& list = tables->functions;
        const auto found = std::ranges::find_if(list, [&](const FunctionEntry& entry) {
            return entry.name == name && entry.library.value_or("") == library;
        });
        if (found != list.end()) {
            ++found->calls;
        } else if (list.size() < TrackedEntries) {
            list.push_back({library.empty() ? std::nullopt : std::optional<std::string>(library), std::string(name), 1});
            added = true;
        } else if (tables->droppedFunctionKeys.size() < 4096u) {
            // Entries dropped are counted once each, not once per call.
            if (tables->droppedFunctionKeys.insert(std::string(library) + "|" + std::string(name)).second) ++tables->dropped.functions;
        } else {
            tables->droppedSaturated = true;
        }
        // The game may catch this and go on, so it only names the stop of an abort that ends with this very message.
        Stop stop{Kind::MissingFunction, Reason::RuntimeStop, source.empty() ? std::nullopt : std::optional<std::string>(source), std::nullopt};
        const std::string function(name);
        if (Matches(function, "[A-Za-z0-9_.$]{1,128}")) {
            stop.message = "The game called a function the runtime does not implement: " + (library.empty() ? std::string() : std::string(library) + ".") + function + ".";
        }
        AddSoft(*tables, std::move(stop), std::string(terminatingMessage));
    }
    if (added) CheckpointSoon();
}

void Collector::NoteShader(int what) {
    if (!started_) return;
    (what == 0 ? compiled : what == 1 ? failedShaders : cacheHitCount).fetch_add(1, std::memory_order_relaxed);
}

void Collector::NoteUnsupportedInstruction(std::optional<std::string> encoding, std::optional<std::string> opcode, std::optional<std::int64_t> opcodeValue, std::string_view) {
    if (!started_) return;
    bool added = false;
    {
        std::unique_lock guard(tables->mutex, std::defer_lock);
        if (!guard.try_lock_for(settings.tableWait)) {
            NoteLost(1);
            return;
        }
        auto& list = tables->unsupported;
        const auto found = std::ranges::find_if(list, [&](const UnsupportedEntry& entry) {
            return entry.encoding == encoding && entry.opcode == opcode && entry.opcodeValue == opcodeValue;
        });
        if (found != list.end()) {
            ++found->count;
        } else if (list.size() < TrackedEntries) {
            list.push_back({encoding, opcode, opcodeValue, std::nullopt, 1});
            added = true;
        } else if (tables->droppedUnsupportedKeys.size() < 4096u) {
            const auto key = encoding.value_or("") + "|" + opcode.value_or("") + "|" + (opcodeValue ? std::to_string(*opcodeValue) : std::string());
            if (tables->droppedUnsupportedKeys.insert(key).second) ++tables->dropped.unsupported;
        } else {
            tables->droppedSaturated = true;
        }
    }
    if (added) CheckpointSoon();
}

// A failure that reached the driver as fatal. Seeing an unsupported instruction is not enough: the recompiler also meets them
// while it prepares shaders it may never use.
void Collector::NoteUnsupportedShaderStop(std::optional<std::string> encoding, std::optional<std::string> opcode, std::string_view source) {
    Stop stop{Kind::UnsupportedShader, Reason::RuntimeStop, source.empty() ? std::nullopt : std::optional<std::string>(source), std::nullopt};
    stop.message = encoding && opcode ? "Shader uses an unsupported " + *encoding + " instruction (" + *opcode + ")." : encoding ? "Shader uses an unsupported " + *encoding + " instruction." : "Shader uses an instruction the runtime does not support.";
    NoteStop(std::move(stop));
}

void Collector::SetDescriptorCounters(const std::atomic<unsigned long long>* counters) {
    descriptorCounters = counters;
}

void Collector::SetFormatCounters(const std::atomic<unsigned long long>* counters, unsigned count) {
    formatCounterCount = count;
    formatCounters = counters;
}

void Collector::NoteImageFormat(std::int64_t format, std::optional<std::string> name, Status status, int usage) {
    if (!started_) return;
    bool added = false;
    {
        std::unique_lock guard(tables->mutex, std::defer_lock);
        if (!guard.try_lock_for(settings.tableWait)) {
            NoteLost(2);
            return;
        }
        auto& list = tables->imageFormats;
        const auto found = std::ranges::find_if(list, [&](const FormatEntry& entry) { return entry.format == format && entry.status == status && entry.usage == usage; });
        if (found != list.end()) {
            ++found->count;
        } else if (list.size() < TrackedEntries) {
            list.push_back({format, std::move(name), status, usage, 1});
            added = true;
        } else if (tables->droppedFormatKeys.size() < 4096u) {
            // Entries dropped are counted once each, not once per call.
            if (tables->droppedFormatKeys.insert(std::to_string(format) + "|" + std::to_string(static_cast<int>(status)) + "|" + std::to_string(usage)).second) ++tables->dropped.imageFormats;
        } else {
            tables->droppedSaturated = true;
        }
    }
    if (added) CheckpointSoon();
}

void Collector::NoteDescriptor(int type, Status status, int usage) {
    if (!started_) return;
    std::unique_lock guard(tables->mutex, std::defer_lock);
    if (!guard.try_lock_for(settings.tableWait)) {
        NoteLost(3);
        return;
    }
    auto& list = tables->descriptors;
    const auto found = std::ranges::find_if(list, [&](const DescriptorEntry& entry) { return entry.type == type && entry.status == status && entry.usage == usage; });
    if (found != list.end()) ++found->count;
    else if (list.size() < TrackedEntries) list.push_back({type, status, usage, 1});
    else ++tables->dropped.descriptors;
}

void Collector::NoteStop(Stop stop) {
    std::unique_lock guard(tables->mutex, std::defer_lock);
    if (!guard.try_lock_for(settings.tableWait)) return;
    // Fatal stops are ranked by when they were reported; a later fatal one is not hidden by a failure noted earlier.
    if (!tables->fatalStop) tables->fatalStop = std::move(stop);
}

void Collector::NoteSoftStop(Stop stop, std::string terminatingMessage, bool prefixed) {
    std::unique_lock guard(tables->mutex, std::defer_lock);
    if (!guard.try_lock_for(settings.tableWait)) return;
    AddSoft(*tables, std::move(stop), std::move(terminatingMessage), prefixed);
}

std::optional<Stop> Collector::PendingStop() const {
    std::unique_lock guard(tables->mutex, std::defer_lock);
    if (!guard.try_lock_for(settings.tableWait)) return std::nullopt;
    return tables->fatalStop;
}

std::optional<Stop> Collector::StopFor(std::string_view terminatingMessage) const {
    std::unique_lock guard(tables->mutex, std::defer_lock);
    if (!guard.try_lock_for(settings.tableWait)) return std::nullopt;
    if (tables->fatalStop) return tables->fatalStop;
    if (terminatingMessage.empty()) return std::nullopt;
    // An exact match comes first, so a caught "read" is never taken for the stop of an uncaught "pread".
    for (const auto& soft : tables->softStops) {
        if (!soft.message.empty() && terminatingMessage == soft.message) return soft.stop;
    }
    // A failure whose thrower adds the graphics layer's known prefix matches in that one form only.
    constexpr std::string_view graphicsPrefix = "AGC graphics: ";
    for (const auto& soft : tables->softStops) {
        if (soft.prefixed && !soft.message.empty() && terminatingMessage.size() == graphicsPrefix.size() + soft.message.size() &&
            terminatingMessage.starts_with(graphicsPrefix) && terminatingMessage.ends_with(soft.message)) return soft.stop;
    }
    return std::nullopt;
}

Observations Collector::Observe() const {
    Observations o;
    o.coverage = settings.coverage;
    const auto seconds = [](std::int64_t nanos) { return std::optional<double>(static_cast<double>(nanos) / 1.0e9); };
    if (milestoneNanos[0] >= 0) o.loaded = seconds(milestoneNanos[0]);
    if (milestoneNanos[1] >= 0) o.started = seconds(milestoneNanos[1]);
    if (milestoneNanos[2] >= 0) o.firstDraw = seconds(milestoneNanos[2]);
    if (milestoneNanos[3] >= 0) o.firstPresent = seconds(milestoneNanos[3]);
    o.framesPresented = frames;
    if (o.framesPresented >= 2 && firstFrameNanos >= 0 && lastFrameNanos > firstFrameNanos) {
        const double window = static_cast<double>(lastFrameNanos - firstFrameNanos) / 1.0e9;
        o.fpsWindowSeconds = window;
        o.averageFps = static_cast<double>(o.framesPresented - 1) / window;
    }
    o.shadersCompiled = compiled;
    o.shadersFailed = failedShaders;
    if (settings.coverage.shaders) o.cacheHits = cacheHitCount.load();
    if (const auto* counters = descriptorCounters.load()) {
        for (int type = 0; type < 7; ++type) {
            for (int usage = 0; usage < 4; ++usage) {
                const auto count = counters[type * 4 + usage].load(std::memory_order_relaxed);
                if (count != 0) o.descriptors.push_back({type, Status::Accepted, usage, static_cast<std::int64_t>(count)});
            }
        }
    }
    if (const auto* counters = formatCounters.load()) {
        for (unsigned format = 0; format < formatCounterCount.load(); ++format) {
            const auto count = counters[format].load(std::memory_order_relaxed);
            if (count != 0) o.imageFormats.push_back({static_cast<std::int64_t>(format), std::nullopt, Status::Accepted, 5, static_cast<std::int64_t>(count)});
        }
    }
    std::unique_lock guard(tables->mutex, std::defer_lock);
    if (guard.try_lock_for(settings.tableWait)) {
        o.functions = tables->functions;
        o.unsupported = tables->unsupported;
        o.imageFormats.insert(o.imageFormats.end(), tables->imageFormats.begin(), tables->imageFormats.end());
        o.descriptors.insert(o.descriptors.end(), tables->descriptors.begin(), tables->descriptors.end());
        o.truncated = tables->dropped;
        if (tables->droppedSaturated) o.lostObservations = true;
    } else {
        // A stuck writer: say that the lists are unknown rather than that they are empty.
        o.coverage.missing = o.coverage.unsupported = o.coverage.imageFormats = o.coverage.descriptors = false;
    }
    // An observation a hook had to drop (the table was busy) is never forgotten: the list is marked as missing at least one entry.
    if (lost[0] != 0 && o.coverage.missing) o.truncated.functions += 1;
    if (lost[1] != 0 && o.coverage.unsupported) o.truncated.unsupported += 1;
    if (lost[2] != 0 && o.coverage.imageFormats) o.truncated.imageFormats += 1;
    if (lost[3] != 0 && o.coverage.descriptors) o.truncated.descriptors += 1;
    o.lostObservations = o.lostObservations || lost[0] != 0 || lost[1] != 0 || lost[2] != 0 || lost[3] != 0;
    return o;
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
        if (exit == Exit::Clean) RefreshGame(true);
        std::unique_lock files(writeMutex, std::defer_lock);
        if (!files.try_lock_for(settings.lockWait)) {
            // A checkpoint write is stuck. Touching the same files now could race it, so leave the last checkpoint for the next start to recover.
            std::fputs("[run-report] a checkpoint write did not finish; the report of this run is left to the next start\n", stderr);
            std::lock_guard guard(mutex);
            finalized = false;
            state = 2;
            changed.notify_all();
            return;
        }
        try {
            const auto text = Serialize(Snapshot(exit, std::move(stop), exitCode, false), host);
            std::string error;
            if (WriteAtomically(reportPath, text, settings.runId.substr(0, 8), error)) {
                reportPublished = true;
                publishedBytes = text;
                if (settings.beforeCheckpointRemoval) settings.beforeCheckpointRemoval();
                // The checkpoint goes while the lock is still held, so no other run can find it unlocked and report this run as killed.
                std::error_code ignored;
                fs::remove(checkpointPath, ignored);
                lock->RemoveAndRelease();
#ifdef _WIN32
                ForgetForExit();
#endif
                finalized = true;
                std::fprintf(stderr, "[run-report] wrote %s\n", reportPath.filename().string().c_str());
            } else {
                std::fprintf(stderr, "[run-report] could not write %s: %s\n", reportPath.filename().string().c_str(), error.c_str());
            }
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "[run-report] could not write the report: %s\n", failure.what());
        } catch (...) {
            std::fputs("[run-report] could not write the report\n", stderr);
        }
        files.unlock();
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

[[maybe_unused]] void FinishAtExit() {
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
        settings.requiredComponents = {"libkernel", "libSceVideoOut", "libSceSystemService", "libSceAgcDriver"};
        // Every section is collected by this build's runtime. The loaded and started milestones come from the entry bridge of the
        // converted executable (macOS has it); where it is missing those two are left out, as unreached milestones are.
        settings.coverage = {true, true, true, true, true, true};
#ifdef _WIN32
        settings.deferredWorker = true;
#endif
        auto* created = new RunReport::Collector(std::move(settings));
        if (!created->Start()) {
            delete created;
            return false;
        }
        collector = created;
#ifndef _WIN32
        // On Windows exit handlers of a DLL run inside the loader lock; the process-exit cleanup below is used instead.
        std::atexit(FinishAtExit);
#endif
        return true;
    } catch (...) {
        return false;
    }
}

[[maybe_unused]] const bool g_runReportStarted = StartRunReport();

}

extern "C" void LibcRunReportSetGameProvider_nid_no_patch(bool (*provider)(RunReportGame*)) {
    gameProvider.store(provider, std::memory_order_release);
    // The game's identity goes into the checkpoint as soon as it can be read, so a run that fails early still names its game.
    if (collector != nullptr) collector->NoteGame();
}

extern "C" void LibcRunReportResume_nid_no_patch() noexcept {
    if (collector != nullptr) collector->Resume();
}

extern "C" void LibcRunReportBeginWorker_nid_no_patch() noexcept {
    if (collector != nullptr) collector->BeginWorker();
}

extern "C" void LibcRunReportRegisterComponent_nid_no_patch(const char* name, const char* commit, int modified) noexcept {
    try {
        if (name != nullptr) RunReport::RegisterComponent(name, commit != nullptr ? commit : "", modified);
    } catch (...) {
    }
}

extern "C" void LibcRunReportQuitRequested_nid_no_patch() noexcept {
    if (collector != nullptr) collector->NoteQuitRequested();
}

extern "C" void LibcRunReportFinishExit_nid_no_patch(int exitCode) noexcept {
    if (collector == nullptr) return;
    collector->Finish(RunReport::Exit::Clean, RunReport::Stop{RunReport::Kind::None, RunReport::Reason::GuestExit, std::nullopt}, exitCode);
}

extern "C" bool LibcRunReportHandoff_nid_no_patch(int exitCode) noexcept {
    if (collector == nullptr) return true;
    collector->Finish(RunReport::Exit::Clean, RunReport::Stop{RunReport::Kind::None, RunReport::Reason::GuestExit, std::nullopt}, exitCode);
    return collector->Finalized();
}

extern "C" void LibcRunReportFinishAbortWith_nid_no_patch(int kind, const char* terminatingMessage) noexcept {
    if (collector == nullptr) return;
    const auto stopKind = kind == static_cast<int>(RunReport::Kind::Assert) ? RunReport::Kind::Assert : kind == static_cast<int>(RunReport::Kind::Crash) ? RunReport::Kind::Crash : RunReport::Kind::Other;
    RunReport::Stop stop{stopKind, RunReport::Reason::Crash, std::nullopt, std::nullopt};
    // A typed failure is the stop of this abort only when it is a fatal one, or the one whose message ended the run.
    if (stopKind == RunReport::Kind::Other) {
        if (auto typed = collector->StopFor(terminatingMessage != nullptr ? terminatingMessage : "")) stop = std::move(*typed);
    }
    collector->Finish(RunReport::Exit::Abort, std::move(stop), std::nullopt);
}

extern "C" void LibcRunReportFinishAbort_nid_no_patch(int kind) noexcept {
    LibcRunReportFinishAbortWith_nid_no_patch(kind, nullptr);
}

extern "C" bool LibcRunReportEnabled_nid_no_patch() noexcept {
    return collector != nullptr;
}

extern "C" void LibcRunReportSetGpu_nid_no_patch(unsigned vendorId, const char* name, const char* driver, unsigned apiVersion) noexcept {
    try {
        if (collector == nullptr) return;
        RunReport::GpuInfo gpu;
        gpu.vendor = vendorId == 0x1002u ? "AMD" : vendorId == 0x10deu ? "NVIDIA" : vendorId == 0x8086u ? "Intel" : vendorId == 0x106bu ? "Apple" : "Other";
        if (name != nullptr && *name != '\0') gpu.name = name;
        if (driver != nullptr && *driver != '\0') gpu.driver = driver;
        // VK_API_VERSION: variant 3 bits, major 7, minor 10, patch 12.
        gpu.vulkan = std::to_string((apiVersion >> 22u) & 0x7fu) + "." + std::to_string((apiVersion >> 12u) & 0x3ffu) + "." + std::to_string(apiVersion & 0xfffu);
        collector->NoteGpu(std::move(gpu));
    } catch (...) {
    }
}

extern "C" void LibcRunReportMilestone_nid_no_patch(int milestone) noexcept {
    if (collector != nullptr) collector->NoteMilestone(milestone);
}

extern "C" void LibcRunReportFramePresented_nid_no_patch() noexcept {
    if (collector != nullptr) collector->NoteFramePresented();
}

namespace {

// The library a source belongs to: the folder after core/libs/prx/ in a source-tree relative path (as SourceOf returns it).
std::string LibraryOf(std::string_view source) {
    constexpr std::string_view root = "core/libs/prx/";
    if (!source.starts_with(root)) return {};
    const auto rest = source.substr(root.size());
    const auto end = rest.find('/');
    return end == std::string_view::npos ? std::string() : std::string(rest.substr(0, end));
}

// The exported name, without the suffix the runtime adds to its own symbols.
std::string_view ExportName(std::string_view name) {
    for (const std::string_view suffix : {"_nid_postfix", "_nid_no_patch"}) {
        if (name.ends_with(suffix)) return name.substr(0, name.size() - suffix.size());
    }
    return name;
}

}

extern "C" void LibcRunReportMissingFunction_nid_no_patch(const char* name, const char* file, int line) noexcept {
    try {
        if (collector == nullptr || name == nullptr) return;
        const std::string_view path = file != nullptr ? file : "";
        const auto source = RunReport::SourceOf(path, line);
        collector->NoteMissingFunction(source ? LibraryOf(*source) : std::string(), ExportName(name), source.value_or(""), std::string(name) + " not implemented");
    } catch (...) {
    }
}

extern "C" void LibcRunReportShader_nid_no_patch(int what) noexcept {
    if (collector != nullptr) collector->NoteShader(what);
}

extern "C" void LibcRunReportUnsupportedInstruction_nid_no_patch(const char* encoding, const char* opcode, long long opcodeValue, const char* file, int line) noexcept {
    try {
        if (collector == nullptr) return;
        const auto source = RunReport::SourceOf(file != nullptr ? file : "", line);
        collector->NoteUnsupportedInstruction(encoding != nullptr ? std::optional<std::string>(encoding) : std::nullopt, opcode != nullptr ? std::optional<std::string>(opcode) : std::nullopt,
                                              opcodeValue >= 0 ? std::optional<std::int64_t>(opcodeValue) : std::nullopt, source.value_or(""));
    } catch (...) {
    }
}

extern "C" void LibcRunReportImageFormat_nid_no_patch(unsigned format, const char* name, int status, int usage) noexcept {
    try {
        if (collector == nullptr) return;
        collector->NoteImageFormat(format, name != nullptr ? std::optional<std::string>(name) : std::nullopt,
                                   static_cast<RunReport::Status>(status >= 0 && status < 3 ? status : 2), usage >= 0 && usage < 6 ? usage : 5);
    } catch (...) {
    }
}

extern "C" void LibcRunReportSetDescriptorCounters_nid_no_patch(const std::atomic<unsigned long long>* counters) noexcept {
    if (collector != nullptr) collector->SetDescriptorCounters(counters);
}

extern "C" void LibcRunReportSetFormatCounters_nid_no_patch(const std::atomic<unsigned long long>* counters, unsigned count) noexcept {
    if (collector != nullptr) collector->SetFormatCounters(counters, count);
}

extern "C" void LibcRunReportUnsupportedShaderStop_nid_no_patch(const char* encoding, const char* opcode, long long, const char* file, int line) noexcept {
    try {
        if (collector == nullptr) return;
        const auto source = RunReport::SourceOf(file != nullptr ? file : "", line);
        collector->NoteUnsupportedShaderStop(encoding != nullptr ? std::optional<std::string>(encoding) : std::nullopt, opcode != nullptr ? std::optional<std::string>(opcode) : std::nullopt, source.value_or(""));
    } catch (...) {
    }
}

extern "C" void LibcRunReportDescriptor_nid_no_patch(int type, int status, int usage) noexcept {
    try {
        if (collector != nullptr) collector->NoteDescriptor(type, static_cast<RunReport::Status>(status >= 0 && status < 3 ? status : 2), usage);
    } catch (...) {
    }
}

extern "C" unsigned long LibcRunReportUnsupportedFormatKey_nid_no_patch(unsigned format, char* buffer, unsigned long size) noexcept {
    if (buffer == nullptr || size == 0u) return 0;
    try {
        const auto key = RunReport::UnsupportedFormatKey(format);
        const auto length = std::min<std::size_t>(key.size(), size - 1u);
        std::memcpy(buffer, key.data(), length);
        buffer[length] = '\0';
        return static_cast<unsigned long>(length);
    } catch (...) {
        buffer[0] = '\0';
        return 0;
    }
}

extern "C" void LibcRunReportUnsupportedFormatStop_nid_no_patch(unsigned format, const char* file, int line) noexcept {
    try {
        if (collector == nullptr) return;
        RunReport::Stop stop{RunReport::Kind::UnsupportedFormat, RunReport::Reason::RuntimeStop, RunReport::SourceOf(file != nullptr ? file : "", line), "A guest image format is not supported (format " + std::to_string(format) + ")."};
        // The runtime throws "unsupported guest texture format N" and callers may catch it; only that message ends the run with this stop.
        collector->NoteSoftStop(std::move(stop), RunReport::UnsupportedFormatKey(format), true);
    } catch (...) {
    }
}

extern "C" const char* LibcRunReportRelaunchEnvironment_nid_no_patch() noexcept {
    return relaunchChoice;
}

#ifdef _WIN32
// ExitProcess, which the generated entry stub calls when the guest returns from its entry, detaches this library with a non-null
// reserved argument, after the other threads have been ended, inside the loader lock. Nothing of the ordinary report pipeline may run
// here: it takes locks, allocates, filters text and calls the game's metadata. So no report is written for that exit; the checkpoint
// and the lock file are removed instead, so that a run that returned cleanly is not recovered later as a killed one.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        const HANDLE lock = RunReport::g_exitLock.exchange(INVALID_HANDLE_VALUE);
        if (lock != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            SetFileInformationByHandle(lock, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        if (RunReport::g_exitCheckpoint[0] != L'\0') DeleteFileW(RunReport::g_exitCheckpoint);
    }
    return TRUE;
}
#endif
