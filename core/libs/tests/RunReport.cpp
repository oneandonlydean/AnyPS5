#include "prx/libc/include/RunReport.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
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
    Require(RunReport::FlagFromArguments(arguments) == std::optional<std::string_view>("on"), "the last --aps5-report flag did not win");
    Require(!RunReport::FlagFromArguments({"game", "--aps5-reports=off", "aps5-report=off"}).has_value(), "a look-alike argument was taken for the flag");
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
    controls.game.name = std::string("a\"b\\c\x01");
    Require(Contains(RunReport::Serialize(controls), "\"a\\\"b\\\\c\\u0001\""), "a name was not escaped");
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

}

#ifndef _WIN32
int RunChild(const fs::path& executable, const fs::path& directory, const std::string& mode) {
    const auto command = "cd '" + directory.string() + "' && APS5_REPORT=1 '" + executable.string() + "' --child " + mode + " 2>/dev/null";
    return std::system(command.c_str());
}

void ChildProcessTests(const fs::path& self) {
    Directory directory;
    const auto copy = directory.path / "game";
    fs::copy_file(self, copy);
    RunChild(copy, directory.path, "exit");
    auto text = Read(directory.path / "anyps5-report.json");
    Require(Contains(text, "\"exit\": \"clean\"") && Contains(text, "\"exitCode\": 7") && Contains(text, "\"reason\": \"guest-exit\"") && Contains(text, "\"kind\": \"none\"") && Contains(text, "\"recovered\": false"), "exit() through the runtime did not write a clean report: " + text);
    fs::remove(directory.path / "anyps5-report.json");
    RunChild(copy, directory.path, "terminate");
    text = Read(directory.path / "anyps5-report.json");
    Require(Contains(text, "\"exitCode\": 0") && Contains(text, "\"reason\": \"quit-requested\""), "a quit request through _exit did not write a quit-requested report: " + text);
    fs::remove(directory.path / "anyps5-report.json");
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
    Require(Contains(Read(directory.path / "anyps5-report.json"), "\"exitCode\": 7"), "the run after a killed one did not write its own report");
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
            std::raise(SIGKILL);
            return 3;
        }
        if (first == "--dump" && argc > 2) {
            Dump(argv[2]);
            return 0;
        }
#endif
        ToggleTests();
        PrivacyTests();
        SerializeTests();
        FinishTests();
        ConcurrentFinishTests();
        FallbackTests();
        AtomicTests();
        RecoveryTests();
        CanaryTests();
        WorkerTests();
#ifndef _WIN32
        ChildProcessTests(fs::absolute(argv[0]));
#endif
        std::puts("run report tests passed: toggle precedence, privacy filter, serialization, finish once, fallback folder, atomic replace, recovery of abandoned runs, canaries, checkpoint worker, exit/_exit/kill in child processes");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
