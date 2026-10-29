#include "prx/libc/include/RunReport.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct NotRun : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int passed = 0, failed = 0, notRun = 0;

// Every case is counted on its own, so one failure does not hide the others and a run that executed nothing is an error.
void Run(const char* name, void (*test)()) {
    // RR_ONLY=<case> runs a single case, so a case that hangs can be timed out on its own.
    if (const char* only = std::getenv("RR_ONLY"); only != nullptr && std::string(only) != name) return;
    try {
        test();
        ++passed;
        std::printf("PASS %s\n", name);
    } catch (const NotRun& reason) {
        ++notRun;
        std::printf("NOT_RUN %s: %s\n", name, reason.what());
    } catch (const std::exception& error) {
        ++failed;
        std::printf("FAIL %s: %s\n", name, error.what());
    }
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

struct Directory {
    Directory() {
        path = fs::temp_directory_path() / ("anyps5-run-report-" + RunReport::GenerateRunId().substr(0, 12));
        fs::create_directories(path);
    }
    ~Directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

constexpr const char* IdA = "0123456789abcdef0123456789abcdef";
constexpr const char* IdB = "fedcba9876543210fedcba9876543210";

RunReport::Settings SettingsFor(const fs::path& executable, const fs::path& user, const char* id) {
    RunReport::Settings settings;
    settings.executableDirectory = executable;
    settings.userStateDirectory = user;
    settings.runId = id;
    settings.worker = false;
    return settings;
}

std::string FlagText(const std::vector<std::string>& arguments) {
    const auto flag = RunReport::FlagFromArguments(arguments);
    return flag ? std::string(*flag) : "<none>";
}

void ToggleTests() {
    using RunReport::ResolveToggle;
    const auto flag = [](const char* value) { return std::optional<std::string_view>(value); };
    const std::optional<std::string_view> none;
    Require(ResolveToggle(none, none).enabled && !ResolveToggle(none, none).explicitlyOn, "the default is not on");
    Require(!ResolveToggle(flag("off"), none).enabled, "--aps5-report=off did not turn the report off");
    Require(ResolveToggle(flag("on"), none).enabled && ResolveToggle(flag("on"), none).explicitlyOn, "--aps5-report=on did not turn the report on");
    Require(!ResolveToggle(none, flag("0")).enabled, "APS5_REPORT=0 did not turn the report off");
    Require(ResolveToggle(none, flag("1")).explicitlyOn, "APS5_REPORT=1 did not turn the report on");
    Require(ResolveToggle(flag("on"), flag("0")).enabled, "the environment variable beat the flag");
    Require(!ResolveToggle(flag("off"), flag("1")).enabled, "the environment variable beat the flag");
    const auto invalid = ResolveToggle(flag("maybe"), flag("0"));
    Require(!invalid.enabled && invalid.warnings.size() == 1, "an invalid flag did not warn and fall to the environment variable");
    const auto invalidBoth = ResolveToggle(flag("yes"), flag("2"));
    Require(invalidBoth.enabled && !invalidBoth.explicitlyOn && invalidBoth.warnings.size() == 2, "two invalid values did not warn twice and fall to the default");
    Require(ResolveToggle(std::nullopt, flag("off")).enabled && ResolveToggle(std::nullopt, flag("off")).warnings.size() == 1, "APS5_REPORT=off is not the documented 0|1");
    Require(ResolveToggle(flag("1"), none).enabled && ResolveToggle(flag("1"), none).warnings.size() == 1, "--aps5-report=1 is not the documented on|off");
    const std::vector<std::string> arguments{"game", "--other", "--aps5-report=off", "--aps5-report=on", "--aps5-reports=off"};
    Require(FlagText(arguments) == "on", "the last --aps5-report flag did not win");
    Require(FlagText({"game", "--aps5-reports=off", "aps5-report=off"}) == "<none>", "a look-alike argument was taken for the flag");
}

void PrivacyTests() {
    RunReport::HostIdentifiers host;
    host.words = {"canaryuser", "canary-host-7", "/home/canaryuser"};
    for (const char* safe : {"Example Game", "Ejemplo del Juego \xc3\x9c" "n\xc3\xaf" "code", "Title 2: The Sequel (1.2)", "01.002.000"}) {
        Require(RunReport::SafeText(safe, host).has_value(), std::string("a safe text was dropped: ") + safe);
    }
    for (const char* unsafe : {"played by canaryuser", "CANARYUSER's game", "on canary-host-7", "saved in /home/canaryuser/saves", "see /home/someone/x", "C:\\Users\\someone\\x", "\\\\server\\share",
                               "connect 192.168.1.20", "mac 00:1a:2b:3c:4d:5e", "mac 00-1a-2b-3c-4d-5e", "v6 fe80:0:0:0:1:2:3:4", "line\nbreak", "tab\there", "", "bad utf8 \xc3\x28", "overlong \xc0\xaf"}) {
        Require(!RunReport::SafeText(unsafe, host).has_value(), std::string("an unsafe text was kept: ") + unsafe);
    }
    Require(RunReport::SafeText(std::string(300, 'a'), host).has_value() && !RunReport::SafeText(std::string(301, 'a'), host).has_value(), "the 300 character cap is wrong");
    std::string wide;
    for (int index = 0; index < 300; ++index) wide += "\xc3\xa9";
    Require(RunReport::SafeText(wide, host).has_value(), "300 two-byte characters were refused");
    Require(!RunReport::SafeText(wide + "\xc3\xa9", host).has_value(), "301 two-byte characters were kept");
}

RunReport::Report SampleReport() {
    RunReport::Report report;
    report.generated = "2026-10-29T12:00:00Z";
    report.build.commit = "40f2a9c3d1b7e5864a0c9d2e7f13b5a8c6d49e01";
    report.build.release = "v0.1.3";
    report.build.modified = false;
    report.game.titleId = "PPSA12345";
    report.game.contentVersion = "01.002.000";
    report.game.name = "Example \"Game\"";
    report.os = "linux";
    report.runId = IdA;
    report.seconds = 12.5;
    report.exitCode = 0;
    return report;
}

void SerializeTests() {
    const auto text = RunReport::Serialize(SampleReport());
    Require(text.size() < RunReport::MaxBytes && text.back() == '\n', "the serialized report is the wrong size");
    for (const char* part : {"{\n  \"format\": \"anyps5-report\",\n  \"version\": 1,\n  \"generated\": \"2026-10-29T12:00:00Z\",\n  \"anyps5\": {\n    \"commit\": \"40f2a9c3d1b7e5864a0c9d2e7f13b5a8c6d49e01\",\n    \"release\": \"v0.1.3\",\n    \"modified\": false\n  },\n",
                             "  \"game\": {\n    \"titleId\": \"PPSA12345\",\n    \"contentId\": null,\n    \"contentVersion\": \"01.002.000\",\n    \"name\": \"Example \\\"Game\\\"\"\n  },\n",
                             "  \"system\": {\n    \"os\": \"linux\",\n    \"osVersion\": null,\n    \"cpu\": null,\n    \"ramGiB\": null,\n    \"gpu\": null\n  },\n",
                             "  \"run\": {\n    \"id\": \"0123456789abcdef0123456789abcdef\",\n    \"seconds\": 12.500,\n    \"exit\": \"clean\",\n    \"exitCode\": 0,\n    \"stage\": \"unknown\",\n    \"milestones\": {},\n    \"framesPresented\": 0,\n    \"averageFps\": null,\n    \"fpsWindowSeconds\": null,\n    \"recovered\": false,\n    \"incomplete\": false\n  },\n",
                             "  \"coverage\": {\n    \"milestones\": false,\n    \"missing\": false,\n    \"shaders\": false,\n    \"unsupported\": false,\n    \"imageFormats\": false,\n    \"descriptors\": false\n  },\n",
                             "  \"truncated\": {\n    \"functions\": 0,\n    \"unsupported\": 0,\n    \"imageFormats\": 0,\n    \"descriptors\": 0\n  },\n  \"missing\": {\n    \"functions\": []\n  },\n",
                             "  \"shaders\": {\n    \"compiled\": 0,\n    \"failed\": 0,\n    \"unsupported\": [],\n    \"imageFormats\": [],\n    \"descriptors\": []\n  },\n",
                             "  \"stop\": {\n    \"kind\": \"none\",\n    \"reason\": \"guest-exit\",\n    \"message\": \"The game exited normally.\",\n    \"source\": null\n  }\n}\n"}) {
        Require(Contains(text, part), std::string("the serialized report lacks: ") + part);
    }
    auto unknown = SampleReport();
    unknown.build = {};
    unknown.game = {};
    unknown.os.reset();
    unknown.exitCode.reset();
    const auto unknownText = RunReport::Serialize(unknown);
    for (const char* part : {"\"commit\": null", "\"release\": null", "\"modified\": null", "\"titleId\": null", "\"contentVersion\": null", "\"name\": null", "\"os\": null", "\"exitCode\": null"}) {
        Require(Contains(unknownText, part), std::string("an unknown value was not null: ") + part);
    }
    auto crash = SampleReport();
    crash.exit = RunReport::Exit::Abort;
    crash.stop = {RunReport::Kind::Assert, RunReport::Reason::Crash, std::string("core/a.cpp:12")};
    const auto crashText = RunReport::Serialize(crash);
    Require(Contains(crashText, "\"kind\": \"assert\"") && Contains(crashText, "\"reason\": \"crash\"") && Contains(crashText, "\"message\": \"A runtime assertion failed.\"") && Contains(crashText, "\"source\": \"core/a.cpp:12\""), "an assertion stop was not described");
    auto controls = SampleReport();
    controls.game.name = std::string("a\"b\\c");
    Require(Contains(RunReport::Serialize(controls), "\"name\": \"a\\\"b\\\\c\""), "a name was not escaped");
    controls.game.name = std::string("a\x01" "b");
    Require(Contains(RunReport::Serialize(controls), "\"name\": null"), "a name with a control character was written");
}

void FinishTests() {
    Directory executable, user;
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
    Require(collector.Start(), "the collector did not start");
    Require(collector.ReportPath() == executable.path / "anyps5-report.json", "the report is not next to the executable");
    const auto checkpoint = Read(collector.CheckpointPath());
    Require(Contains(checkpoint, "\"recovered\": true") && Contains(checkpoint, "\"exit\": \"killed\"") && Contains(checkpoint, "\"reason\": \"unclean-unknown\"") && Contains(checkpoint, "\"exitCode\": null") && Contains(checkpoint, "\"incomplete\": true"), "the checkpoint is not a recovered report");
    Require(fs::exists(executable.path / ("anyps5-report." + std::string(IdA) + ".lock")), "the run holds no lock file");
    collector.NoteQuitRequested();
    collector.Finish(RunReport::Exit::Clean, {RunReport::Kind::None, RunReport::Reason::GuestExit, std::nullopt}, 3);
    const auto report = Read(collector.ReportPath());
    Require(Contains(report, "\"exit\": \"clean\"") && Contains(report, "\"exitCode\": 3") && Contains(report, "\"reason\": \"quit-requested\"") && Contains(report, "\"recovered\": false"), "the finished report is wrong: " + report);
    Require(!fs::exists(collector.CheckpointPath()) && !fs::exists(executable.path / ("anyps5-report." + std::string(IdA) + ".lock")), "the checkpoint or lock survived a clean finish");
    collector.Finish(RunReport::Exit::Abort, {RunReport::Kind::Other, RunReport::Reason::Crash, std::nullopt}, std::nullopt);
    Require(Read(collector.ReportPath()) == report, "a second finish changed the report");
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(executable.path)) { static_cast<void>(entry); ++files; }
    Require(files == 1, "a temporary file was left behind");
}

void ConcurrentFinishTests() {
    Directory executable, user;
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
    Require(collector.Start(), "the collector did not start");
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&collector, index] { collector.Finish(RunReport::Exit::Clean, {RunReport::Kind::None, RunReport::Reason::GuestExit, std::nullopt}, index); });
    }
    for (auto& thread : threads) thread.join();
    Require(collector.Finished() && fs::exists(collector.ReportPath()), "concurrent finishes did not leave one report");
}

void FallbackTests() {
    Directory root, user;
    std::ofstream(root.path / "file") << "not a directory";
    RunReport::Collector collector(SettingsFor(root.path / "file" / "below", user.path, IdA));
    Require(collector.Start(), "the collector did not fall back to the user folder");
    Require(collector.ReportPath() == user.path / "reports" / IdA / "anyps5-report.json", "the fallback report is not under reports/<run id>/");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    Require(fs::exists(collector.ReportPath()), "the fallback report was not written");
    RunReport::Collector nowhere(SettingsFor(root.path / "file" / "below", root.path / "file" / "also", IdB));
    Require(!nowhere.Start(), "a collector with no writable folder started");
    nowhere.Finish(RunReport::Exit::Clean, {}, 0);
}

void AtomicTests() {
    Directory directory;
    const auto target = directory.path / "anyps5-report.json";
    std::string error;
    Require(RunReport::WriteAtomically(target, "first\n", "aaaaaaaa", error) && Read(target) == "first\n", "a first write failed");
    Require(RunReport::WriteAtomically(target, "second\n", "bbbbbbbb", error) && Read(target) == "second\n", "the old report was not replaced");
#ifndef _WIN32
    fs::create_symlink(directory.path / "elsewhere", directory.path / "link.json");
    std::ofstream(directory.path / "elsewhere") << "keep";
    Require(!RunReport::WriteAtomically(directory.path / "link.json", "x\n", "cccccccc", error) && Read(directory.path / "elsewhere") == "keep", "a symbolic link destination was written through");
    fs::create_directory(directory.path / "folder.json");
    Require(!RunReport::WriteAtomically(directory.path / "folder.json", "x\n", "dddddddd", error), "a folder destination was replaced");
#endif
    Require(!fs::exists(directory.path / ".anyps5-report.json.aaaaaaaa.tmp"), "a temporary file was left behind");
}

void RecoveryTests() {
    Directory executable, user;
    {
        RunReport::Collector abandoned(SettingsFor(executable.path, user.path, IdA));
        Require(abandoned.Start(), "the first collector did not start");
        RunReport::Collector live(SettingsFor(executable.path, user.path, IdB));
        Require(live.Start(), "the second collector did not start");
        Require(!fs::exists(executable.path / "reports"), "a live run's checkpoint was recovered");
        RunReport::Collector third(SettingsFor(executable.path, user.path, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        Require(third.Start(), "the third collector did not start");
        Require(!fs::exists(executable.path / "reports"), "a live run's checkpoint was recovered by a later run");
        third.Finish(RunReport::Exit::Clean, {}, 0);
        live.Finish(RunReport::Exit::Clean, {}, 0);
    }
    Require(fs::exists(executable.path / ("anyps5-report." + std::string(IdA) + ".ckpt")), "the abandoned checkpoint is gone");
    RunReport::Collector next(SettingsFor(executable.path, user.path, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    Require(next.Start(), "the next collector did not start");
    const auto recovered = executable.path / "reports" / IdA / "anyps5-report.json";
    const auto text = Read(recovered);
    Require(Contains(text, "\"recovered\": true") && Contains(text, "\"incomplete\": true") && Contains(text, "\"exit\": \"killed\"") && Contains(text, std::string("\"id\": \"") + IdA + "\""), "the recovered report is wrong: " + text);
    Require(!fs::exists(executable.path / ("anyps5-report." + std::string(IdA) + ".ckpt")) && !fs::exists(executable.path / ("anyps5-report." + std::string(IdA) + ".lock")), "the recovered run's checkpoint or lock survived");
    Require(!Contains(Read(executable.path / "anyps5-report.json"), IdA), "recovery overwrote the adjacent report");
    next.Finish(RunReport::Exit::Clean, {}, 0);
    Require(fs::exists(executable.path / "anyps5-report.json") && fs::exists(recovered), "a later report replaced the recovered one");
}

void CanaryTests() {
    Directory executable, user;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.game = [] {
        RunReport::GameInfo game;
        game.titleId = "PPSA12345";
        game.contentVersion = "01.002.000";
        game.name = "Game of " + std::string(std::getenv("USER") != nullptr && std::string(std::getenv("USER")).size() >= 3u ? std::getenv("USER") : "canaryuser") + " at 10.1.2.3";
        return game;
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    const auto text = Read(collector.ReportPath());
    Require(Contains(text, "\"name\": null") && !Contains(text, "10.1.2.3") && Contains(text, "\"titleId\": \"PPSA12345\""), "a name with a host identifier was copied: " + text);
    auto invalid = SettingsFor(executable.path, user.path, IdB);
    invalid.game = [] {
        RunReport::GameInfo game;
        game.titleId = "ppsa1";
        game.contentVersion = "01.002 000";
        return game;
    };
    RunReport::Collector other(std::move(invalid));
    Require(other.Start(), "the second collector did not start");
    other.Finish(RunReport::Exit::Clean, {}, 0);
    const auto otherText = Read(other.ReportPath());
    Require(Contains(otherText, "\"titleId\": null") && Contains(otherText, "\"contentVersion\": null"), "an invalid title id or version was copied");
}

void WorkerTests() {
    Directory executable, user;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.worker = true;
    settings.checkpointInterval = std::chrono::milliseconds(20);
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    const auto first = Read(collector.CheckpointPath());
    std::string later = first;
    for (int attempt = 0; attempt < 100 && later == first; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        later = Read(collector.CheckpointPath());
    }
    Require(later != first && Contains(later, "\"recovered\": true"), "the checkpoint worker did not refresh the checkpoint");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    Require(!fs::exists(collector.CheckpointPath()), "the checkpoint survived a finish with a worker");
}

// ---- regressions of the step 2 review (row 700); each fails on 1c3ac7fa52 ----

void FlagFromTemporaryTest() {
    // The flag text must outlive the argument vector it came from (the startup path built it from a temporary).
    const auto flag = RunReport::FlagFromArguments(std::vector<std::string>{"game", "--aps5-report=off"});
    std::vector<std::string> churn;
    for (int index = 0; index < 64; ++index) churn.emplace_back(64, 'x');
    Require(flag && std::string(*flag) == "off", "the flag text did not survive its argument vector");
}

void FinishWhileProviderBlockedTest() {
    Directory executable, user;
    std::mutex metadata;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.worker = true;
    settings.checkpointInterval = std::chrono::milliseconds(10);
    settings.game = [&metadata] {
        std::lock_guard guard(metadata);
        RunReport::GameInfo game;
        game.titleId = "PPSA12345";
        return game;
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    std::unique_lock held(metadata);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The failing thread holds the metadata lock and the checkpoint worker waits for it: Finish must still return.
    auto done = std::async(std::launch::async, [&collector] { collector.Finish(RunReport::Exit::Abort, {RunReport::Kind::Assert, RunReport::Reason::Crash, std::nullopt}, std::nullopt); });
    const bool returned = done.wait_for(std::chrono::seconds(8)) == std::future_status::ready;
    held.unlock();
    done.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Require(returned, "Finish blocked while the game metadata lock was held");
    Require(Contains(Read(collector.ReportPath()), "\"kind\": \"assert\""), "the report of the assertion was not written");
    Require(!fs::exists(collector.CheckpointPath()), "the checkpoint survived the finish");
}

void CleanFinishReadsGameOnceTest() {
    Directory executable, user;
    std::atomic<int> calls{0};
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.game = [&calls] {
        ++calls;
        RunReport::GameInfo game;
        game.titleId = "PPSA12345";
        game.name = "Example Game";
        return game;
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    Require(calls == 1 && Contains(Read(collector.ReportPath()), "\"titleId\": \"PPSA12345\""), "a clean finish did not read the game once");
}

void FallbackRecoveryTest() {
    Directory root, user;
    std::ofstream(root.path / "file") << "not a directory";
    const auto unwritable = root.path / "file" / "below";
    {
        RunReport::Collector abandoned(SettingsFor(unwritable, user.path, IdA));
        Require(abandoned.Start(), "the collector did not fall back to the user folder");
        Require(fs::exists(user.path / "reports" / IdA / ("anyps5-report." + std::string(IdA) + ".ckpt")), "the fallback run has no checkpoint");
    }
    RunReport::Collector next(SettingsFor(unwritable, user.path, IdB));
    Require(next.Start(), "the next collector did not fall back to the user folder");
    const auto text = Read(user.path / "reports" / IdA / "anyps5-report.json");
    Require(Contains(text, "\"recovered\": true") && Contains(text, "\"exit\": \"killed\"") && Contains(text, std::string("\"id\": \"") + IdA + "\""), "the fallback run was not recovered: " + text);
    Require(!fs::exists(user.path / "reports" / IdA / ("anyps5-report." + std::string(IdA) + ".ckpt")), "the recovered fallback checkpoint survived");
    next.Finish(RunReport::Exit::Clean, {}, 0);
}

void RemovalBeforeUnlockTest() {
#if !defined(_WIN32) && defined(RUN_REPORT_HAS_FINISH_HOOK)
    Directory executable, user;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    bool recoveredDuringFinish = false;
    settings.beforeCheckpointRemoval = [&] {
        std::string warnings;
        recoveredDuringFinish = !RunReport::RecoverAbandonedRuns({executable.path}, warnings).empty();
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    Require(!recoveredDuringFinish && !fs::exists(executable.path / "reports"), "a finishing run's checkpoint was recovered while the run still held its lock");
#else
    throw NotRun("this tree has no finish hook");
#endif
}

void FinishRecoverStressTest() {
#ifdef _WIN32
    throw NotRun("POSIX lock semantics");
#else
    Directory executable, user;
    std::atomic<bool> stop{false};
    std::atomic<int> falsePositives{0};
    std::thread recoverer([&] {
        while (!stop) {
            std::string warnings;
            falsePositives += static_cast<int>(RunReport::RecoverAbandonedRuns({executable.path}, warnings).size());
        }
    });
    for (int round = 0; round < 400; ++round) {
        RunReport::Collector collector(SettingsFor(executable.path, user.path, round % 2 ? IdA : IdB));
        if (!collector.Start()) continue;
        collector.Finish(RunReport::Exit::Clean, {}, 0);
    }
    stop = true;
    recoverer.join();
    Require(falsePositives == 0, "a run that finished cleanly was recovered as killed " + std::to_string(falsePositives.load()) + " times");
#endif
}

void ParentLinkTest() {
#ifdef _WIN32
    throw NotRun("symbolic links need a privilege here");
#else
    Directory directory;
    fs::create_directories(directory.path / "real");
    fs::create_directory_symlink(directory.path / "real", directory.path / "link");
    std::string error;
    const bool wrote = RunReport::WriteAtomically(directory.path / "link" / "anyps5-report.json", "x\n", "aaaaaaaa", error);
    Require(!wrote && !fs::exists(directory.path / "real" / "anyps5-report.json"), "a write went through a symbolic link in the parent folder");
    // A planted link for the fallback folder is not used either.
    Directory user, other;
    fs::create_directory_symlink(other.path, user.path / "reports");
    std::ofstream(directory.path / "file") << "not a directory";
    RunReport::Collector collector(SettingsFor(directory.path / "file" / "below", user.path, IdA));
    Require(!collector.Start(), "a collector used the reports folder that was a link");
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(other.path)) { static_cast<void>(entry); ++entries; }
    Require(entries == 0, "something was written through the planted reports link");
#endif
}

std::string CheckpointBody(const std::string& id, const std::string& name, const std::string& generated = "2026-10-29T12:00:00Z") {
    auto report = SampleReport();
    report.generated = generated;
    report.runId = id;
    report.exit = RunReport::Exit::Killed;
    report.exitCode.reset();
    report.recovered = report.incomplete = true;
    report.stop = {RunReport::Kind::Other, RunReport::Reason::UncleanUnknown, std::nullopt};
    report.game.name = name;
    RunReport::HostIdentifiers none;
    return RunReport::Serialize(report, none);
}

void PlantCheckpoint(const fs::path& directory, const std::string& id, const std::string& body) {
    std::ofstream(directory / ("anyps5-report." + id + ".ckpt"), std::ios::binary) << body;
}

void RecoveryValidationTest() {
    Directory executable, user;
    PlantCheckpoint(executable.path, IdA, "{\"private\": \"alice@example.com /home/alice\"}\n");
    PlantCheckpoint(executable.path, IdB, CheckpointBody(IdA, "Example Game"));
    PlantCheckpoint(executable.path, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", std::string(RunReport::MaxBytes + 10u, ' '));
    PlantCheckpoint(executable.path, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", CheckpointBody("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "Example Game").substr(0, 200));
    PlantCheckpoint(executable.path, "cccccccccccccccccccccccccccccccc", [] { auto body = CheckpointBody("cccccccccccccccccccccccccccccccc", "Example Game"); body.replace(body.find("2026-10-29T12:00:00Z"), 20u, "not a time"); return body; }());
    RunReport::Collector collector(SettingsFor(executable.path, user.path, "dddddddddddddddddddddddddddddddd"));
    Require(collector.Start(), "the collector did not start");
    std::size_t published = 0;
    if (fs::exists(executable.path / "reports")) {
        for (const auto& entry : fs::recursive_directory_iterator(executable.path / "reports")) {
            if (entry.is_regular_file()) ++published;
        }
    }
    std::string names;
    if (fs::exists(executable.path / "reports")) {
        for (const auto& entry : fs::recursive_directory_iterator(executable.path / "reports")) names += entry.path().string() + " ";
    }
    Require(published == 0, "an invalid checkpoint was published as a recovered report (" + std::to_string(published) + " files): " + names);
    collector.Finish(RunReport::Exit::Clean, {}, 0);
}

void RecoverySanitizesTest() {
    Directory executable, user;
    constexpr const char* IdC = "cccccccccccccccccccccccccccccccc";
    PlantCheckpoint(executable.path, IdC, CheckpointBody(IdC, "Example Game"));
    auto dirty = CheckpointBody(IdA, "Example Game");
    dirty.replace(dirty.find("Example Game"), 12u, "see fe80::1 and bob@example.com");
    PlantCheckpoint(executable.path, IdA, dirty);
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdB));
    Require(collector.Start(), "the collector did not start");
    const auto good = Read(executable.path / "reports" / IdC / "anyps5-report.json");
    Require(Contains(good, "\"recovered\": true") && Contains(good, "\"name\": \"Example Game\""), "a valid checkpoint was not recovered: " + good);
    // A checkpoint whose text the filters would change is not one this runtime wrote: it is rejected whole, not republished in part.
    Require(!fs::exists(executable.path / "reports" / IdA / "anyps5-report.json"), "a checkpoint with private text was republished");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
}

void RecoveryRejectsLinkTest() {
#ifdef _WIN32
    throw NotRun("symbolic links need a privilege here");
#else
    Directory executable, user, elsewhere;
    const auto real = elsewhere.path / "real.ckpt";
    std::ofstream(real, std::ios::binary) << CheckpointBody(IdA, "Example Game");
    fs::create_symlink(real, executable.path / ("anyps5-report." + std::string(IdA) + ".ckpt"));
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdB));
    Require(collector.Start(), "the collector did not start");
    Require(!fs::exists(executable.path / "reports" / IdA / "anyps5-report.json"), "a checkpoint that is a symbolic link was recovered");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
#endif
}

void PrivacyIsolatedTest() {
    RunReport::HostIdentifiers host;
    host.words = {"ab", "canaryuser", "canary-host-7"};
    for (const char* unsafe : {"User ab", "ab", "played by AB.", "connect 2001:db8::1", "ping fe80::1", "loopback ::1", "any ::", "mail alice@example.com", "read /opt/private/report",
                               "open /run/user/1000/x", "go to ~/Documents/saves", "account 123456789012", "see file:///etc/passwd", "set %USERPROFILE%\\x", "(/srv/data)", "http://example.org/x"}) {
        Require(!RunReport::SafeText(unsafe, host).has_value(), std::string("an unsafe text was kept: ") + unsafe);
    }
    for (const char* safe : {"Tabby and abacus", "Mission: Impossible: Redux", "Face Off: Dead", "Part 1/2", "Strike & Fire", "Game 2049", "Alpha: Omega"}) {
        Require(RunReport::SafeText(safe, host).has_value(), std::string("a safe text was dropped: ") + safe);
    }
}

void PrivacySweepTest() {
#ifdef _WIN32
    throw NotRun("environment canaries are POSIX here");
#else
    ::setenv("USER", "canaryuser9", 1);
    ::setenv("LOGNAME", "canaryuser9", 1);
    ::setenv("HOME", "/home/canaryuser9", 1);
    ::setenv("HOSTNAME", "canary-host-9", 1);
    Directory executable, user;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.worker = true;
    settings.checkpointInterval = std::chrono::milliseconds(10);
    settings.game = [] {
        RunReport::GameInfo game;
        game.titleId = "PPSA12345";
        game.contentVersion = "01.002.000";
        game.name = "Save of canaryuser9";
        return game;
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto checkpoint = Read(collector.CheckpointPath());
    collector.Finish(RunReport::Exit::Abort, {RunReport::Kind::Other, RunReport::Reason::Crash, std::string("canary-host-9 at /home/canaryuser9/x")}, std::nullopt);
    const auto report = Read(collector.ReportPath());
    for (const auto& text : {checkpoint, report}) {
        for (const char* canary : {"canaryuser9", "canary-host-9", "/home/", "@", "::"}) {
            Require(!Contains(text, canary), std::string("the output contains the canary ") + canary + ": " + text);
        }
    }
    Require(Contains(report, "\"source\": null") && Contains(report, "\"name\": null"), "the private fields were not dropped: " + report);
#endif
}

void NumericLocaleTest() {
#ifdef _WIN32
    throw NotRun("locale names differ");
#else
    const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "de_DE", "fr_FR"};
    const std::string previous = std::setlocale(LC_NUMERIC, nullptr) ? std::setlocale(LC_NUMERIC, nullptr) : "C";
    bool changed = false;
    for (const char* name : locales) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            changed = true;
            break;
        }
    }
    if (!changed) throw NotRun("no comma-decimal locale is installed here");
    const auto text = RunReport::Serialize(SampleReport());
    std::setlocale(LC_NUMERIC, previous.c_str());
    Require(Contains(text, "\"seconds\": 12.500"), "the numeric locale changed the written number: " + text);
#endif
}


void AncestorLinkTest() {
#ifdef _WIN32
    throw NotRun("symbolic links need a privilege here");
#else
    Directory directory;
    fs::create_directories(directory.path / "real" / "sub");
    fs::create_directory_symlink(directory.path / "real", directory.path / "link");
    std::string error;
    const bool wrote = RunReport::WriteAtomically(directory.path / "link" / "sub" / "anyps5-report.json", "x\n", "aaaaaaaa", error);
    Require(!wrote && !fs::exists(directory.path / "real" / "sub" / "anyps5-report.json"), "a write went through a symbolic link above the parent folder");
    Require(RunReport::WriteAtomically(directory.path / "real" / "sub" / "anyps5-report.json", "x\n", "bbbbbbbb", error), "a write through real folders failed: " + error);
    std::string bytes;
    Require(!RunReport::ReadBounded(directory.path / "link" / "sub" / "anyps5-report.json", 100, bytes), "a read went through a symbolic link above the parent folder");
    Require(RunReport::ReadBounded(directory.path / "real" / "sub" / "anyps5-report.json", 100, bytes) && bytes == "x\n", "a read through real folders failed");
#endif
}

void MetadataHostTokenTest() {
    RunReport::HostIdentifiers host;
    host.words = {"ab", "canaryuser", "vcanaryrel"};
    auto report = SampleReport();
    report.game.contentVersion = "ab";
    report.build.release = "vcanaryrel";
    report.game.titleId = "PPSA12345";
    const auto text = RunReport::Serialize(report, host);
    Require(Contains(text, "\"contentVersion\": null") && Contains(text, "\"release\": null"), "a version or release that names the host was kept: " + text);
    Require(Contains(text, "\"titleId\": \"PPSA12345\""), "a harmless title id was dropped");
    report.build.commit = "abc1234";
    Require(Contains(RunReport::Serialize(report, host), "\"commit\": null"), "a commit that is not 40 hex digits was kept");
}

void PathDelimiterTest() {
    RunReport::HostIdentifiers host;
    for (const char* unsafe : {"file:/opt/x", "a:/opt/x", "a|/opt/x", "x;/srv/y"}) {
        Require(!RunReport::SafeText(unsafe, host).has_value(), std::string("an unsafe text was kept: ") + unsafe);
    }
}

void FinishWithStalledWriteTest() {
    Directory executable, user;
    std::atomic<bool> armed{false}, stuck{false}, release{false};
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.worker = true;
    settings.checkpointInterval = std::chrono::milliseconds(10);
    settings.lockWait = std::chrono::milliseconds(150);
    settings.duringCheckpointWrite = [&] {
        if (!armed) return;
        stuck = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    armed = true;
    for (int attempt = 0; attempt < 400 && !stuck; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(stuck, "the checkpoint worker never reached the write");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    const bool wrote = fs::exists(collector.ReportPath());
    const bool kept = fs::exists(collector.CheckpointPath());
    release = true;
    Require(!wrote && kept, "Finish went on to replace the files while a checkpoint write was in progress");
}

void ResumeTest() {
    Directory executable, user;
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
    Require(collector.Start(), "the collector did not start");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    Require(collector.Finished() && !fs::exists(collector.CheckpointPath()), "the first finish did not finish");
    Require(collector.Resume(), "the finished run could not be reopened");
    Require(!collector.Finished() && fs::exists(collector.CheckpointPath()) && fs::exists(executable.path / ("anyps5-report." + std::string(IdA) + ".lock")), "a reopened run has no checkpoint and lock");
    std::string warnings;
    Require(RunReport::RecoverAbandonedRuns({executable.path}, warnings).empty(), "a reopened run was recovered while it was running");
    collector.Finish(RunReport::Exit::Abort, {RunReport::Kind::Other, RunReport::Reason::Crash, std::nullopt}, std::nullopt);
    Require(Contains(Read(collector.ReportPath()), "\"exit\": \"abort\"") && !fs::exists(collector.CheckpointPath()), "the second finish did not replace the report");
}

void StrictCheckpointTest() {
    Directory executable, user;
    constexpr const char* IdC = "cccccccccccccccccccccccccccccccc";
    const auto good = CheckpointBody(IdA, "Example Game");
    RunReport::HostIdentifiers host;
    Require(RunReport::ParseCheckpoint(good, IdA, host).has_value(), "a genuine checkpoint was rejected");
    auto missing = CheckpointBody(IdC, "Example Game");
    const auto at = missing.find("  \"coverage\": {");
    Require(at != std::string::npos, "the sample has no coverage object");
    missing.erase(at, missing.find("},\n", at) + 3u - at);
    auto extra = CheckpointBody(IdB, "Example Game");
    extra.insert(extra.find("\"format\""), "\"extra\": 1,\n  ");
    PlantCheckpoint(executable.path, IdC, missing);
    PlantCheckpoint(executable.path, IdB, extra);
    Require(!RunReport::ParseCheckpoint(missing, IdC, host) && !RunReport::ParseCheckpoint(extra, IdB, host), "a checkpoint that is not in the frozen shape was accepted");
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
    Require(collector.Start(), "the collector did not start");
    Require(!fs::exists(executable.path / "reports"), "part of an invalid checkpoint was republished");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
}

void EarlyIdentityTest() {
    Directory executable, user;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.game = [] {
        RunReport::GameInfo game;
        game.titleId = "PPSA12345";
        game.name = "Example Game";
        return game;
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    Require(Contains(Read(collector.CheckpointPath()), "\"titleId\": \"PPSA12345\""), "the first checkpoint does not name the game");
    collector.Finish(RunReport::Exit::Abort, {RunReport::Kind::Assert, RunReport::Reason::Crash, std::nullopt}, std::nullopt);
    Require(Contains(Read(collector.ReportPath()), "\"titleId\": \"PPSA12345\"") && Contains(Read(collector.ReportPath()), "\"name\": \"Example Game\""), "a run that failed early was reported without its game");
}

void ComponentProvenanceTest() {
    const std::string commit(40, 'a');
    const auto modifiedOf = [&](const std::string& name) {
        Directory executable, user;
        auto settings = SettingsFor(executable.path, user.path, IdA);
        settings.build.commit = commit;
        settings.build.modified = false;
        settings.requiredComponents = {name};
        RunReport::Collector collector(std::move(settings));
        Require(collector.Start(), "the collector did not start");
        collector.Finish(RunReport::Exit::Clean, {}, 0);
        const auto text = Read(collector.ReportPath());
        return Contains(text, "\"modified\": null") ? std::string("null") : Contains(text, "\"modified\": true") ? std::string("true") : std::string("false");
    };
    Require(modifiedOf("unregistered-lib") == "null", "a missing component did not make modified unknown");
    RunReport::RegisterComponent("same-lib", commit, 0);
    Require(modifiedOf("same-lib") == "false", "a matching clean component changed modified");
    RunReport::RegisterComponent("other-lib", std::string(40, 'b'), 0);
    Require(modifiedOf("other-lib") == "true", "a component from another commit did not make modified true");
    // A mismatching library counts even when it is not one the run requires.
    Require(modifiedOf("same-lib") == "true", "a registered library from another commit was ignored because it is not required");
    RunReport::RegisterComponent("dirty-lib", commit, 1);
    Require(modifiedOf("dirty-lib") == "true", "a modified component did not make modified true");
}

void ResumeBlockedWorkerTest() {
    Directory executable, user;
    std::mutex metadata;
    auto settings = SettingsFor(executable.path, user.path, IdA);
    settings.worker = true;
    settings.checkpointInterval = std::chrono::milliseconds(10);
    settings.game = [&metadata] {
        std::lock_guard guard(metadata);
        return RunReport::GameInfo{};
    };
    RunReport::Collector collector(std::move(settings));
    std::unique_lock held(metadata);
    Require(collector.Start(), "the collector did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    collector.Finish(RunReport::Exit::Abort, {RunReport::Kind::Other, RunReport::Reason::Crash, std::nullopt}, std::nullopt);
    // The checkpoint worker is stuck behind the metadata lock: reopening the run must not wait for it.
    auto resumed = std::async(std::launch::async, [&collector] { return collector.Resume(); });
    const bool returned = resumed.wait_for(std::chrono::seconds(8)) == std::future_status::ready;
    held.unlock();
    const bool ok = resumed.get();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Require(returned, "Resume waited for a worker that was blocked behind the metadata lock");
    Require(ok && !collector.Finished(), "the run was not reopened");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    Require(fs::exists(collector.ReportPath()), "the reopened run did not finish");
}

void ResumeRetractsReportTest() {
    Directory executable, user;
    {
        RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
        Require(collector.Start(), "the collector did not start");
        collector.Finish(RunReport::Exit::Clean, {}, 0);
        Require(fs::exists(collector.ReportPath()), "the handoff wrote no report");
        Require(collector.Resume(), "the run was not reopened");
        Require(!fs::exists(executable.path / "anyps5-report.json"), "the clean report of the failed handoff stayed visible after the run resumed");
    }
    // The resumed run is killed: only the recovered report may describe it.
    RunReport::Collector next(SettingsFor(executable.path, user.path, IdB));
    Require(next.Start(), "the next collector did not start");
    Require(!fs::exists(executable.path / "anyps5-report.json") && Contains(Read(executable.path / "reports" / IdA / "anyps5-report.json"), "\"exit\": \"killed\""), "a killed resumed run is not reported as killed");
    next.Finish(RunReport::Exit::Clean, {}, 0);
}

void ResumeKeepsNewerReportTest() {
    Directory executable, user;
    RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
    Require(collector.Start(), "the collector did not start");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    // Another run in the same folder finishes after this one's handoff and replaces the report.
    const std::string newer = RunReport::Serialize(SampleReport());
    std::string error;
    Require(RunReport::WriteAtomically(collector.ReportPath(), newer, "bbbbbbbb", error), "the newer report was not written: " + error);
    Require(collector.Resume(), "the run was not reopened");
    Require(Read(collector.ReportPath()) == newer, "Resume deleted a newer completed report");
}

void HandoffOutcomeTest() {
    {
        Directory executable, user;
        RunReport::Collector collector(SettingsFor(executable.path, user.path, IdA));
        Require(collector.Start(), "the collector did not start");
        collector.Finish(RunReport::Exit::Clean, {}, 0);
        Require(collector.Finalized(), "a finished handoff was not reported as finalized");
    }
    Directory executable, user;
    std::atomic<bool> armed{false}, stuck{false}, release{false};
    auto settings = SettingsFor(executable.path, user.path, IdB);
    settings.worker = true;
    settings.checkpointInterval = std::chrono::milliseconds(10);
    settings.lockWait = std::chrono::milliseconds(150);
    settings.duringCheckpointWrite = [&] {
        if (!armed) return;
        stuck = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    RunReport::Collector collector(std::move(settings));
    Require(collector.Start(), "the collector did not start");
    armed = true;
    for (int attempt = 0; attempt < 400 && !stuck; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(stuck, "the checkpoint worker never reached the write");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    const bool finalized = collector.Finalized();
    release = true;
    Require(!finalized, "a handoff that left a checkpoint write in progress was reported as finalized, so a relaunch would close its descriptors");
}

#ifndef _WIN32
int RunChild(const fs::path& executable, const fs::path& directory, const std::string& mode, const std::string& arguments = "", const std::string& environment = "APS5_REPORT=1") {
    const auto command = "cd '" + directory.string() + "' && " + environment + " exec '" + executable.string() + "' --child " + mode + " " + arguments + " 2>/dev/null";
    return std::system(command.c_str());
}

fs::path Self;

fs::path CopySelf(const Directory& directory) {
    const auto copy = directory.path / "game";
    fs::copy_file(Self, copy);
    return copy;
}

std::string Report(const Directory& directory) {
    return Read(directory.path / "anyps5-report.json");
}

void ChildExitTest() {
    Directory directory;
    RunChild(CopySelf(directory), directory.path, "exit");
    const auto text = Report(directory);
    Require(Contains(text, "\"exit\": \"clean\"") && Contains(text, "\"exitCode\": 7") && Contains(text, "\"reason\": \"guest-exit\"") && Contains(text, "\"kind\": \"none\"") && Contains(text, "\"recovered\": false"), "exit() through the runtime did not write a clean report: " + text);
}

void ChildQuitTest() {
    Directory directory;
    RunChild(CopySelf(directory), directory.path, "terminate");
    const auto text = Report(directory);
    Require(Contains(text, "\"exitCode\": 0") && Contains(text, "\"reason\": \"quit-requested\""), "a quit request through _exit did not write a quit-requested report: " + text);
}

void ChildKillTest() {
    Directory directory;
    const auto copy = CopySelf(directory);
    RunChild(copy, directory.path, "kill");
    Require(!fs::exists(directory.path / "anyps5-report.json"), "a killed run wrote a report");
    std::string checkpoint;
    for (const auto& entry : fs::directory_iterator(directory.path)) {
        if (entry.path().extension() == ".ckpt") checkpoint = entry.path().filename().string();
    }
    Require(!checkpoint.empty(), "a killed run left no checkpoint");
    const auto killedId = checkpoint.substr(std::string("anyps5-report.").size(), 32u);
    RunChild(copy, directory.path, "exit");
    const auto recovered = Read(directory.path / "reports" / killedId / "anyps5-report.json");
    Require(Contains(recovered, "\"recovered\": true") && Contains(recovered, "\"exit\": \"killed\"") && Contains(recovered, "\"incomplete\": true"), "the next start did not recover the killed run's report: " + recovered);
    Require(Contains(Report(directory), "\"exitCode\": 7"), "the run after a killed one did not write its own report");
}

void ChildReturnTest() {
    Directory directory;
    const auto copy = CopySelf(directory);
    RunChild(copy, directory.path, "return");
    const auto text = Report(directory);
    Require(Contains(text, "\"exit\": \"clean\"") && Contains(text, "\"recovered\": false"), "returning from the entry did not write a clean report: " + text);
    for (const auto& entry : fs::directory_iterator(directory.path)) {
        Require(entry.path().extension() != ".ckpt" && entry.path().extension() != ".lock", "a clean run left " + entry.path().filename().string());
    }
    RunChild(copy, directory.path, "return");
    Require(!fs::exists(directory.path / "reports"), "the next start reported the clean run as killed");
}

void ChildFlagTest() {
    Directory directory;
    const auto copy = CopySelf(directory);
    const int status = RunChild(copy, directory.path, "exit", "--aps5-report=off", "APS5_REPORT=1");
    Require(WIFEXITED(status) && WEXITSTATUS(status) == 7, "the run with --aps5-report=off did not exit with its own status");
    Require(!fs::exists(directory.path / "anyps5-report.json"), "--aps5-report=off still wrote a report");
    RunChild(copy, directory.path, "exit", "--aps5-report=on", "APS5_REPORT=0");
    Require(fs::exists(directory.path / "anyps5-report.json"), "--aps5-report=on did not write a report");
}

void ChildRelaunchEnvironmentTest() {
    Directory directory;
    const auto copy = CopySelf(directory);
    RunChild(copy, directory.path, "relaunch-env", "--aps5-report=off", "APS5_REPORT=1");
    Require(Contains(Read(directory.path / "relaunch.txt"), "0"), "the choice made with --aps5-report=off is not passed on to a relaunch");
}

void ChildAbortTest() {
    Directory directory;
    const int status = RunChild(CopySelf(directory), directory.path, "abort");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "abort() did not keep its signal");
    const auto text = Report(directory);
    Require(Contains(text, "\"exit\": \"abort\"") && Contains(text, "\"reason\": \"crash\""), "abort() did not write an abort report: " + text);
}

void ChildTerminateTest() {
    Directory directory;
    const int status = RunChild(CopySelf(directory), directory.path, "std-terminate");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "std::terminate did not keep its signal");
    const auto text = Report(directory);
    Require(Contains(text, "\"exit\": \"abort\"") && Contains(text, "\"reason\": \"crash\""), "std::terminate did not write an abort report: " + text);
}

void ChildAssertTest() {
    Directory directory;
    const int status = RunChild(CopySelf(directory), directory.path, "assert");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "a runtime assertion did not keep its signal");
    const auto text = Report(directory);
    Require(Contains(text, "\"kind\": \"assert\"") && Contains(text, "\"exit\": \"abort\""), "a runtime assertion did not write an assert report: " + text);
}

void Dump(const fs::path& directory) {
    fs::create_directories(directory);
    auto write = [&](const char* name, const RunReport::Report& report) {
        std::ofstream(directory / name, std::ios::binary) << RunReport::Serialize(report);
    };
    auto clean = SampleReport();
    write("clean.json", clean);
    auto quit = SampleReport();
    quit.stop.reason = RunReport::Reason::QuitRequested;
    write("quit.json", quit);
    auto assertion = SampleReport();
    assertion.exit = RunReport::Exit::Abort;
    assertion.exitCode.reset();
    assertion.stop = {RunReport::Kind::Assert, RunReport::Reason::Crash, std::nullopt};
    write("assert.json", assertion);
    auto terminate = assertion;
    terminate.stop.kind = RunReport::Kind::Other;
    write("terminate.json", terminate);
    auto recovered = SampleReport();
    recovered.exit = RunReport::Exit::Killed;
    recovered.exitCode.reset();
    recovered.recovered = recovered.incomplete = true;
    recovered.stop = {RunReport::Kind::Other, RunReport::Reason::UncleanUnknown, std::nullopt};
    write("recovered.json", recovered);
    auto unknown = SampleReport();
    unknown.build = {};
    unknown.game = {};
    unknown.os.reset();
    write("unknown.json", unknown);
}
#endif

}

#ifndef _WIN32
extern "C" [[noreturn]] void LibcRuntimeAssertion(const char*, int, const char*, const char*);
extern "C" [[noreturn]] void abort_nid_postfix(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
#endif

int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        const std::string first = argc > 1 ? argv[1] : "";
        if (first == "--child" && argc > 2) {
            const std::string mode = argv[2];
            if (mode == "exit") LibcExit_nid_no_patch(7);
            if (mode == "terminate") {
                LibcRunReportQuitRequested_nid_no_patch();
                LibcTerminate_nid_no_patch(0);
            }
            if (mode == "return") return 5;
            if (mode == "abort") abort_nid_postfix(0, 0, 0, 0, 0, 0);
            if (mode == "std-terminate") std::terminate();
            if (mode == "assert") LibcRuntimeAssertion("file", 1, "function", "condition");
            if (mode == "relaunch-env") {
                const char* choice = LibcRunReportRelaunchEnvironment_nid_no_patch();
                std::ofstream("relaunch.txt") << (choice != nullptr ? choice : "unset");
                return 0;
            }
            std::raise(SIGKILL);
            return 3;
        }
        if (first == "--dump" && argc > 2) {
            Dump(argv[2]);
            return 0;
        }
        Self = fs::absolute(argv[0]);
#endif
        Run("toggle", ToggleTests);
        Run("privacy", PrivacyTests);
        Run("serialize", SerializeTests);
        Run("finish", FinishTests);
        Run("concurrent-finish", ConcurrentFinishTests);
        Run("fallback", FallbackTests);
        Run("atomic", AtomicTests);
        Run("recovery", RecoveryTests);
        Run("canary", CanaryTests);
        Run("worker", WorkerTests);
        Run("flag-from-temporary", FlagFromTemporaryTest);
        Run("finish-while-provider-blocked", FinishWhileProviderBlockedTest);
        Run("clean-finish-reads-game-once", CleanFinishReadsGameOnceTest);
        Run("fallback-recovery", FallbackRecoveryTest);
        Run("removal-before-unlock", RemovalBeforeUnlockTest);
        Run("finish-recover-stress", FinishRecoverStressTest);
        Run("parent-link", ParentLinkTest);
        Run("recovery-validation", RecoveryValidationTest);
        Run("recovery-sanitizes", RecoverySanitizesTest);
        Run("recovery-rejects-link", RecoveryRejectsLinkTest);
        Run("privacy-isolated", PrivacyIsolatedTest);
        Run("privacy-sweep", PrivacySweepTest);
        Run("numeric-locale", NumericLocaleTest);
        Run("ancestor-link", AncestorLinkTest);
        Run("metadata-host-token", MetadataHostTokenTest);
        Run("path-delimiters", PathDelimiterTest);
        Run("finish-with-stalled-write", FinishWithStalledWriteTest);
        Run("resume", ResumeTest);
        Run("strict-checkpoint", StrictCheckpointTest);
        Run("early-identity", EarlyIdentityTest);
        Run("component-provenance", ComponentProvenanceTest);
        Run("resume-blocked-worker", ResumeBlockedWorkerTest);
        Run("resume-retracts-report", ResumeRetractsReportTest);
        Run("resume-keeps-newer-report", ResumeKeepsNewerReportTest);
        Run("handoff-outcome", HandoffOutcomeTest);
#ifndef _WIN32
        Run("child-exit", ChildExitTest);
        Run("child-quit", ChildQuitTest);
        Run("child-kill-and-recover", ChildKillTest);
        Run("child-return-from-entry", ChildReturnTest);
        Run("child-flag", ChildFlagTest);
        Run("child-relaunch-environment", ChildRelaunchEnvironmentTest);
        Run("child-abort", ChildAbortTest);
        Run("child-std-terminate", ChildTerminateTest);
        Run("child-assert", ChildAssertTest);
#endif
        std::printf("run report tests: %d passed, %d failed, %d not run\n", passed, failed, notRun);
        if (passed + failed + notRun == 0) {
            std::fputs("no test ran\n", stderr);
            return 1;
        }
        return failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
