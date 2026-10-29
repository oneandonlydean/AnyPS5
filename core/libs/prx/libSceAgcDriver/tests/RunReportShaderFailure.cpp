// The run report counts shader compiles through the recompiler's diagnostic sink: a failed attempt is counted once, whether it
// failed while the program was prepared (the decoder rejected an instruction) or while it was compiled, and a good compile is
// counted as compiled. These run the real Recompile on real shader words.
#include "Recompiler.hpp"
#include "RdnaDecoder/DiagnosticSink.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libc/include/RunReport.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace ShaderRecompiler;

std::atomic<int> compiled{0}, failed{0}, reused{0}, unsupported{0};
std::atomic<int> lastEncodingIsSop1{0};

void Shader(int what) {
    (what == 0 ? compiled : what == 1 ? failed : reused).fetch_add(1);
}

void Unsupported(const char* encoding, const char*, long long, const char*, int) {
    unsupported.fetch_add(1);
    if (encoding != nullptr && std::string_view(encoding) == "SOP1") lastEncodingIsSop1 = 1;
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::array<std::uint32_t, 4> MovStore{0x7e020300u, 0xe0700000u, 0x80000100u, 0xbf810000u};
// SOP1 with an opcode the decoder has no entry for, then s_endpgm.
constexpr std::array<std::uint32_t, 2> UnsupportedSop1{0xbe807e80u, 0xbf810000u};

RecompileResult recompile(std::span<const std::uint32_t> code, bool useCache) {
    const std::array<std::uint32_t, 2> capabilities{spv::CapabilityShader, spv::CapabilityGroupNonUniform};
    const std::array<std::uint32_t, 4> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu};
    const std::array<std::string_view, 1> extensions{"SPV_KHR_storage_buffer_storage_class"};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00403000u;
    request.target.spirvVersion = 0x00010600u;
    request.target.subgroupSize = 64;
    request.target.bdaAbiVersion = 1;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
    request.target.maxWorkgroupInvocations = 1024;
    request.target.maxWorkgroupSharedMemoryBytes = 49152;
    request.layout = {0, 0, 0, 128};
    request.useCache = useCache;
    return Recompile(request);
}

// The text the graphics layer really throws for a rejected format finds the stop the run report filed for it.
void verifyFormatStop() {
    std::string text;
    try {
        static_cast<void>(AgcDriver::Graphics::ResolveTextureFormat(9999u));
    } catch (const std::exception& error) {
        text = error.what();
    }
    require(!text.empty(), "an unknown guest format was resolved");
    RunReport::Settings settings;
    settings.executableDirectory = std::filesystem::temp_directory_path() / "anyps5-run-report-format-stop";
    std::filesystem::create_directories(settings.executableDirectory);
    settings.worker = false;
    RunReport::Collector collector(std::move(settings));
    require(collector.Start(), "the collector did not start");
    collector.NoteSoftStop(RunReport::Stop{RunReport::Kind::UnsupportedFormat, RunReport::Reason::RuntimeStop, std::nullopt, "A guest image format is not supported (format 9999)."}, RunReport::UnsupportedFormatKey(9999u));
    const auto stop = collector.StopFor(text);
    require(stop && stop->kind == RunReport::Kind::UnsupportedFormat, "the real message '" + text + "' did not find the stop of a rejected format");
    collector.Finish(RunReport::Exit::Clean, {}, 0);
    std::error_code ignored;
    std::filesystem::remove_all(collector.ReportPath().parent_path(), ignored);
}

void verify() {
    // The disk cache would turn a compile into a reuse; this test counts compiles, so it never reads one.
#ifdef _WIN32
    _putenv_s("ANYPS5_NO_SHADER_CACHE", "1");
#else
    setenv("ANYPS5_NO_SHADER_CACHE", "1", 1);
#endif
    verifyFormatStop();
    static const DiagnosticSink sink{.shader = Shader, .unsupportedInstruction = Unsupported};
    SetDiagnosticSink(&sink);
    for (const bool cache : {false, true}) {
        compiled = failed = reused = unsupported = 0;
        const auto before = std::string(cache ? "cached" : "uncached");
        recompile(MovStore, cache);
        require(compiled == 1 && failed == 0, before + ": a good compile was not counted as one compile (compiled " + std::to_string(compiled) + ", failed " + std::to_string(failed) + ")");
        bool threw = false;
        try {
            recompile(UnsupportedSop1, cache);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, before + ": the shader with an unsupported instruction compiled");
        require(unsupported >= 1 && lastEncodingIsSop1 == 1, before + ": the unsupported instruction was not reported with its encoding");
        require(failed == 1, before + ": the failed attempt was counted " + std::to_string(failed) + " times, not once");
        require(compiled == 1, before + ": a failed attempt was counted as a compile");
    }
}

}

int main() {
    try {
        verify();
        std::puts("run report shader failure tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
