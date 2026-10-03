#include "Recompiler.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include "CacheKey.hpp"
#include "CompiledVariant.hpp"
#include "ShaderDiskCache.hpp"
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include "ControlFlow/include/ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/include/ControlFlow/Structurizer.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "IntermediateRepresentation/include/IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/ConstantFolder.hpp"
#include "Optimization/include/Optimization/DeadCodeEliminator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/include/Optimization/ReadLaneEliminator.hpp"
#include "Optimization/include/Optimization/RequestMemoryView.hpp"
#include "Optimization/include/Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/include/Optimization/ResourceTracker.hpp"
#include "Optimization/include/Optimization/ShaderInfoCollector.hpp"
#include "Optimization/include/Optimization/SrtWalker.hpp"
#include "Optimization/include/Optimization/SsaBuilder.hpp"
#include "SpirvBackend/include/SpirvBackend/SpirvEmitter.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include "SpirvBackend/SpirvMemory/SpirvInputOutput.hpp"
#include "Translation/include/Translation/InstructionTranslator.hpp"
#include "Translation/include/Translation/ShaderInputInfoBuilder.hpp"
#include <exception>
#include <stdexcept>
#include <string>
#include <ControlFlow/RequestSerializer.hpp>

namespace ShaderRecompiler {

namespace {

ShaderStageKind toShaderStageKind(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Compute:
        return ShaderStageKind::Compute;
    case ShaderStage::Vertex:
        return ShaderStageKind::Vertex;
    case ShaderStage::TessellationControl:
        return ShaderStageKind::TessellationControl;
    case ShaderStage::TessellationEvaluation:
        return ShaderStageKind::TessellationEvaluation;
    case ShaderStage::Fragment:
        return ShaderStageKind::Pixel;
    case ShaderStage::Local:
        return ShaderStageKind::Local;
    case ShaderStage::Mesh:
        return ShaderStageKind::Mesh;
    case ShaderStage::Geometry:
        break;
    }
    throw std::runtime_error("ShaderRecompiler::Recompile: unsupported shader stage");
}

}

namespace {

// Debug aid: whether `list` (hex code addresses, comma separated, or "all") names the program.
bool ListedProgram(const std::string& list, const RecompileRequest& request) {
    if (list.empty()) return false;
    if (list == "all") return true;
    char address[32];
    std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(request.shader.codeAddress));
    return list.find(address) != std::string::npos;
}

std::string EnvironmentText(const char* variable) {
    const char* text = std::getenv(variable);
    return text != nullptr ? std::string(text) : std::string();
}

}

namespace {

// The threads of a wave64 compute workgroup that spans several host subgroups of a 32-wide host,
// where the lane layout matters; 0 for every other program.
std::uint32_t SplitWorkgroupThreads(const RecompileRequest& request) {
    if (request.shader.stage != ShaderStage::Compute || request.context.waveSize != 64u || request.target.subgroupSize != 32u || !request.context.compute.has_value()) return 0u;
    const auto& compute = *request.context.compute;
    const auto threads = std::max(compute.numThreads[0], 1u) * std::max(compute.numThreads[1], 1u) * std::max(compute.numThreads[2], 1u);
    // A workgroup of at most 32 threads is one host subgroup in either layout.
    return threads > 32u ? threads : 0u;
}

// Whether the SingleLane exchange slots fit the workgroup memory beside the program's LDS.
bool SingleLaneFits(const RecompileRequest& request, std::uint32_t threads) {
    const auto limit = request.target.maxWorkgroupSharedMemoryBytes;
    return limit == 0u || (request.context.compute->ldsSizeDwords + SingleLaneExchangeDwords(threads)) * 4u <= limit;
}

}

WaveLayout WaveLayoutFor(const RecompileRequest& request) {
    const auto threads = SplitWorkgroupThreads(request);
    if (threads == 0u) return WaveLayout::Auto;
    static const std::string singleLane = EnvironmentText("APS5_SINGLE_LANE");
    static const std::string twoLane = EnvironmentText("APS5_TWO_LANE");
    auto layout = request.waveLayout;
    if (ListedProgram(twoLane, request)) layout = WaveLayout::TwoLane;
    else if (ListedProgram(singleLane, request)) layout = WaveLayout::SingleLane;
    if (layout == WaveLayout::SingleLane && !SingleLaneFits(request, threads)) layout = WaveLayout::TwoLane;
    return layout;
}

bool LayoutChosenByProbe(const RecompileRequest& request) {
    return request.target.localMemoryProbe != nullptr && SplitWorkgroupThreads(request) != 0u && WaveLayoutFor(request) == WaveLayout::Auto;
}

bool InexactSingleLane(const RecompileRequest& request) {
    static const std::string inexact = EnvironmentText("APS5_INEXACT_SINGLE_LANE");
    return ListedProgram(inexact, request);
}

namespace {

// The layout the request compiles to: its WaveLayoutFor, with an Auto program compiled as TwoLane
// first (see compileVariant); Auto where no layout applies.
WaveLayout CompiledLayout(const RecompileRequest& request) {
    if (SplitWorkgroupThreads(request) == 0u) return WaveLayout::Auto;
    const auto layout = WaveLayoutFor(request);
    return layout == WaveLayout::Auto ? WaveLayout::TwoLane : layout;
}

// The stage input metadata of a request (a mesh-stage program's with its subgroup configuration).
ShaderStageInputInfo RequestInputInfo(const RecompileRequest& request) {
    const auto* mesh = request.graphics && request.graphics->mesh ? &*request.graphics->mesh : nullptr;
    // Debug aid: APS5_INEXACT_SINGLE_LANE=<hex code addresses, comma separated, or "all"> lays the
    // listed wave64 programs out at one lane per invocation with each 32-lane half acting as a wave
    // of its own (what APS5_SINGLE_LANE did before SingleLane): NOT exact, since ballots, lane
    // reads and branches only see the invocation's half and lanes 32-63 read as lanes 0-31.
    if (InexactSingleLane(request)) return BuildShaderStageInputInfo(toShaderStageKind(request.shader.stage), request.context, 64u, mesh, false);
    return BuildShaderStageInputInfo(toShaderStageKind(request.shader.stage), request.context, request.target.subgroupSize, mesh, CompiledLayout(request) == WaveLayout::SingleLane);
}

// The request with its layout fixed to SingleLane.
RecompileRequest SingleLaneRequest(const RecompileRequest& request) {
    auto single = request;
    single.waveLayout = WaveLayout::SingleLane;
    return single;
}

}

IrProgram PrepareResourceProgram(const RecompileRequest& request) {
    const auto stageKind = toShaderStageKind(request.shader.stage);
    const auto inputInfo = RequestInputInfo(request);

    constexpr RdnaInstructionDecoder decoder;
    const auto decoded = decoder.Decode(request.shader.code);

    constexpr GraphBuilder graphBuilder;
    auto cfg = graphBuilder.Build(decoded);

    constexpr Structurizer structurizer;
    structurizer.Structurize(cfg);

    TranslateOptions translateOptions {};
    translateOptions.stage = stageKind;
    translateOptions.shaderHash = request.shader.codeAddress;
    translateOptions.waveSize = request.context.waveSize;
    translateOptions.userDataBaseRegister = request.context.userDataBaseRegister;
    translateOptions.userDataCount = static_cast<std::uint32_t>(request.context.userData.size());
    translateOptions.embeddedFetch = nullptr;
    translateOptions.fragmentShaderBarycentricEnabled = request.target.fragmentShaderBarycentricEnabled;
    translateOptions.inputInfo = inputInfo;

    constexpr InstructionTranslator translator;

    EmbeddedFetchPlan embeddedFetch;
    if ((stageKind == ShaderStageKind::Vertex || stageKind == ShaderStageKind::Local) && inputInfo.vertex != nullptr && inputInfo.vertex->fetchEmbedded) {
        constexpr EmbeddedVertexFetchAnalyzer embeddedFetchAnalyzer;
        embeddedFetch = embeddedFetchAnalyzer.Analyze(decoded, inputInfo.vertex->fetchAttribReg, inputInfo.vertex->fetchBufferReg, request.context.userDataBaseRegister, static_cast<std::uint32_t>(request.context.userData.size()), request.context.waveSize);
    }
    translateOptions.embeddedFetch = embeddedFetch.loads.empty() ? nullptr : &embeddedFetch;

    auto program = translator.Translate(decoded, cfg, translateOptions);
    // Debug aid: APS5_DUMP_IR=<hex code address> (or "all") prints the program after each front-end pass.
    const auto dumpIr = [&](const char* pass) {
        static const std::string list = [] { const char* text = std::getenv("APS5_DUMP_IR"); return text ? std::string(text) : std::string(); }();
        if (list.empty()) return;
        char address[32];
        std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(request.shader.codeAddress));
        if (list != "all" && list.find(address) == std::string::npos) return;
        std::fprintf(stderr, "==== IR 0x%s after %s\n%s\n", address, pass, ProgramToString(program).c_str());
    };
    dumpIr("translate");

    constexpr SsaBuilder ssaBuilder;
    ssaBuilder.Rewrite(program);
    dumpIr("ssa");

    constexpr ConstantFolder constantFolder;
    constexpr DeadCodeEliminator deadCodeEliminator;

    constantFolder.Fold(program);
    ResolveControlFlowIdentities(program);
    deadCodeEliminator.RemoveIdentities(program);
    deadCodeEliminator.Eliminate(program);
    dumpIr("fold");

    constexpr ReadLaneEliminator readLaneEliminator;
    const auto readLaneStats = readLaneEliminator.Eliminate(program, translateOptions.waveSize);
    if (readLaneStats.rewrittenReads != 0u) {
        constantFolder.Fold(program);
        ResolveControlFlowIdentities(program);
        deadCodeEliminator.RemoveIdentities(program);
        deadCodeEliminator.Eliminate(program);
    }

    constexpr SrtWalker srtWalker;
    srtWalker.BuildPlan(program);
    deadCodeEliminator.Eliminate(program);
    dumpIr("srt");

    constexpr ResourceTracker resourceTracker;
    resourceTracker.Track(program);
    deadCodeEliminator.Eliminate(program);
    dumpIr("resources");

    return program;
}

// A materialized result of one variant over one snapshot (Recompile(request, capture)): the
// shared immutable object every later capture that reproduces the snapshot receives, so Populate
// and the per-request copy run once per distinct snapshot.
struct ResultMemoEntry {
    std::uint64_t variantId;
    std::uint64_t hash;
    std::shared_ptr<const RecompileResult> result;
};

struct SourceEntry {
    std::mutex mutex;
    // The code the entry was built for: the key carries only a hash of it, so a candidate entry is
    // accepted only when its code matches word for word. Owned here because the request's span
    // points into a registration the driver may replace while the entry lives on.
    std::vector<std::uint32_t> code;
    std::shared_ptr<const IrResourcePlan> plan;
    // The layout an Auto wave64 program settled on (under mutex): its first variant's, so later
    // variants skip the spill probe (see compileVariant).
    WaveLayout settledLayout = WaveLayout::Auto;
    std::uint32_t settledWorkgroupReserve = 0;
    // A plan build that threw (an unsupported resource chain or control flow) is remembered and
    // rethrown: the front end ran every pass before failing, ~13 ms per dispatch of a shader the
    // title issues every frame (0x1048947300 at the intro video). APS5_NO_FAILURE_MEMO=1 rebuilds.
    std::exception_ptr planFailure;
    std::unique_ptr<IrProgram> program;
    std::vector<std::shared_ptr<const CompiledVariant>> variants;
    // The result memo, most recently used first, at most ResultMemoEntries (under mutex).
    std::list<ResultMemoEntry> memo;
    std::unordered_map<std::uint64_t, std::list<ResultMemoEntry>::iterator> memoIndex;
};

namespace {

struct ResourceProgram {
    explicit ResourceProgram(const RecompileRequest& request) : program(std::make_unique<IrProgram>(PrepareResourceProgram(request))), plan(std::make_shared<const IrResourcePlan>(ResourceMaterializer{}.ExtractPlan(*program))) {}

    std::unique_ptr<IrProgram> program;
    std::shared_ptr<const IrResourcePlan> plan;
};

std::shared_ptr<const IrResourcePlan> makeResourcePlan(const RecompileRequest& request) {
    return ResourceProgram(request).plan;
}

struct SourceKeyHash {
    std::size_t operator()(const std::vector<std::uint64_t>& key) const {
        std::size_t hash = 0;
        for (const auto value : key) {
            hash ^= static_cast<std::size_t>(value) + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
            if constexpr (sizeof(std::size_t) < sizeof(value)) hash ^= static_cast<std::size_t>(value >> 32u);
        }
        return hash;
    }
};

std::shared_ptr<SourceEntry> getSource(const RecompileRequest& request) {
    static std::shared_mutex mutex;
    // Entries whose code hashes alike share a bucket; the code comparison picks the right one.
    static std::unordered_map<std::vector<std::uint64_t>, std::vector<std::shared_ptr<SourceEntry>>, SourceKeyHash> sources;
    struct SourceKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, SourceKeyStorage>();
    RecompileCacheKey::Build(request, key);
    const auto find = [&]() -> std::shared_ptr<SourceEntry> {
        const auto found = sources.find(key);
        if (found == sources.end()) return nullptr;
        for (const auto& entry : found->second) {
            if (std::equal(entry->code.begin(), entry->code.end(), request.shader.code.begin(), request.shader.code.end())) return entry;
        }
        return nullptr;
    };
    std::shared_ptr<SourceEntry> source;
    {
        std::shared_lock lock(mutex);
        source = find();
    }
    if (source == nullptr) {
        std::unique_lock lock(mutex);
        source = find();
        if (source == nullptr) {
            source = std::make_shared<SourceEntry>();
            source->code.assign(request.shader.code.begin(), request.shader.code.end());
            auto& bucket = sources[key];
            if (!bucket.empty()) {
                // A second entry under one key is a code hash collision (or the unhashed key with
                // identical code, which cannot happen); each is reported under the profile switch.
                static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                static std::uint64_t collisions = 0;
                ++collisions;
                if (profile) std::fprintf(stderr, "[recompile] source key collision %llu: %zu entries share a key (%zu code words)\n", static_cast<unsigned long long>(collisions), bucket.size() + 1, request.shader.code.size());
            }
            bucket.push_back(source);
        }
    }
    {
        std::lock_guard lock(source->mutex);
        if (source->plan == nullptr) {
            static const bool memoFailures = std::getenv("APS5_NO_FAILURE_MEMO") == nullptr;
            if (memoFailures && source->planFailure) std::rethrow_exception(source->planFailure);
            try {
                ResourceProgram resource(request);
                source->plan = std::move(resource.plan);
                source->program = std::move(resource.program);
            } catch (...) {
                if (memoFailures) source->planFailure = std::current_exception();
                throw;
            }
        }
    }
    return source;
}

std::array<std::uint32_t, 3> partialThreads(const RecompileRequest& request) {
    return request.context.compute ? request.context.compute->partialThreads : std::array<std::uint32_t, 3>{};
}

// A process-wide id per compiled (or disk-loaded) variant; the driver keys pipeline objects on it.
std::uint64_t nextVariantId() {
    static std::atomic<std::uint64_t> variants{0};
    return variants.fetch_add(1, std::memory_order_relaxed) + 1;
}

CompiledVariant compileVariant(const RecompileRequest& request, IrProgram program, const ResourceSnapshot& resourceSnapshot, const ResourceSpecialization& resourceSpecialization, bool chooseLayout, std::optional<std::uint32_t>* localMemory = nullptr, std::uint32_t workgroupReserveBytes = 0);

constexpr std::uint32_t MinimumWorkgroupReserveBytes = 4096;
constexpr std::uint32_t MaximumWorkgroupReserveBytes = 32768;

std::optional<CompiledVariant> compileWithoutLocalMemory(const RecompileRequest& request, const ResourceSnapshot& resourceSnapshot, const ResourceSpecialization& resourceSpecialization) {
    const auto limit = request.target.maxWorkgroupSharedMemoryBytes;
    const auto threads = SplitWorkgroupThreads(request);
    const auto exchange = CompiledLayout(request) == WaveLayout::SingleLane ? SingleLaneExchangeDwords(threads) : 0u;
    const auto used = (request.context.compute->ldsSizeDwords + exchange) * 4u;
    for (std::uint32_t reserve = MinimumWorkgroupReserveBytes; reserve <= MaximumWorkgroupReserveBytes && (limit == 0u || used + reserve <= limit); reserve *= 2u) {
        std::optional<std::uint32_t> bytes;
        auto variant = compileVariant(request, PrepareResourceProgram(request), resourceSnapshot, resourceSpecialization, false, &bytes, reserve);
        std::fprintf(stderr, "[wave64] program 0x%llx: %s bytes of local memory per invocation with %u bytes of workgroup memory reserved\n", static_cast<unsigned long long>(request.shader.codeAddress), bytes ? std::to_string(*bytes).c_str() : "unknown", reserve);
        if (bytes.has_value() && *bytes == 0u) return variant;
    }
    return std::nullopt;
}

// With `chooseLayout`, an Auto wave64 program whose TwoLane module needs local memory is compiled as
// SingleLane too, and the module needing less local memory is kept (see WaveLayoutFor). With
// `localMemory`, the module's local memory as the target's probe reports it.
CompiledVariant compileVariant(const RecompileRequest& request, IrProgram program, const ResourceSnapshot& resourceSnapshot, const ResourceSpecialization& resourceSpecialization, bool chooseLayout, std::optional<std::uint32_t>* localMemory, std::uint32_t workgroupReserveBytes) {
    const auto inputInfo = RequestInputInfo(request);
    constexpr DeadCodeEliminator deadCodeEliminator;
    constexpr ResourceMaterializer resourceMaterializer;
    resourceMaterializer.Apply(program, resourceSpecialization);

    deadCodeEliminator.RemoveIdentities(program);
    deadCodeEliminator.Eliminate(program);

    constexpr ShaderInfoCollector shaderInfoCollector;
    shaderInfoCollector.Collect(program, inputInfo);

    constexpr BindingAllocator bindingAllocator;
    auto bindings = bindingAllocator.Allocate(program, request.layout);

    constexpr DescriptorBindingBuilder descriptorBindingBuilder;
    descriptorBindingBuilder.Populate(bindings, program, resourceSnapshot, partialThreads(request));

    SpirvTargetOptions targetOptions {};
    targetOptions.vulkanVersion = request.target.vulkanVersion;
    targetOptions.spirvVersion = request.target.spirvVersion;
    targetOptions.subgroupSize = request.target.subgroupSize;
    targetOptions.bdaAbiVersion = request.target.bdaAbiVersion;
    targetOptions.supportedCapabilities = request.target.supportedCapabilities;
    targetOptions.supportedExtensions = request.target.supportedExtensions;
    targetOptions.nonConstantImageOffsets = request.target.nonConstantImageOffsets;
    targetOptions.codeAddress = request.shader.codeAddress;
    targetOptions.workgroupReserveBytes = workgroupReserveBytes;

    constexpr SpirvEmitter spirvEmitter;
    RecompileResult result;
    result.variantId = nextVariantId();
    try {
        result.spirv = spirvEmitter.Emit(program, inputInfo, bindings, targetOptions);
    } catch (const SingleLaneNotExact& error) {
        auto twoLane = request;
        twoLane.waveLayout = WaveLayout::TwoLane;
        std::fprintf(stderr, "[wave64] program 0x%llx: %s; compiling it two lanes per invocation\n", static_cast<unsigned long long>(request.shader.codeAddress), error.what());
        return compileVariant(twoLane, PrepareResourceProgram(twoLane), resourceSnapshot, resourceSpecialization, false, localMemory, workgroupReserveBytes);
    }

#if ANYPS5_ENABLE_SPIRV_TOOLS
    result.spirv = ValidateAndOptimizeSpirv(result.spirv, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets);
#endif
    result.waveLayout = CompiledLayout(request);
    result.workgroupReserveBytes = workgroupReserveBytes;
    const auto& probe = request.target.localMemoryProbe;
    // Only single-wave workgroups: their halves meet at workgroup barriers. With several waves the
    // halves pair through counters in workgroup memory, exact on its own (agc_driver_wave_tests)
    // but Astro Bot's 16x16 light programs (0x500630800 and siblings) laid out that way reproduce
    // the MMU fault in the light-list consumer 0x500597a00 that the layout choice avoids.
    const bool settle = chooseLayout && result.waveLayout == WaveLayout::TwoLane && WaveLayoutFor(request) == WaveLayout::Auto && probe != nullptr;
    const bool choose = settle && SplitWorkgroupThreads(request) <= 64u && SingleLaneFits(request, SplitWorkgroupThreads(request));
    if (probe != nullptr && (localMemory != nullptr || settle)) {
        const auto bytes = probe(request.target.localMemoryProbeContext, result.spirv.Words(), bindings.bindings, request.shader.codeAddress);
        if (localMemory != nullptr) *localMemory = bytes;
        if (settle && !choose && bytes.has_value() && *bytes != 0u) {
            std::fprintf(stderr, "[wave64] program 0x%llx: %u bytes of local memory per invocation at two lanes\n", static_cast<unsigned long long>(request.shader.codeAddress), *bytes);
            if (auto reserved = compileWithoutLocalMemory(request, resourceSnapshot, resourceSpecialization)) return std::move(*reserved);
        }
        if (choose && bytes.has_value() && *bytes != 0u) {
            const auto single = SingleLaneRequest(request);
            std::optional<std::uint32_t> singleBytes;
            auto singleVariant = compileVariant(single, PrepareResourceProgram(single), resourceSnapshot, resourceSpecialization, false, &singleBytes);
            const bool takeSingle = singleBytes.has_value() && *singleBytes < *bytes;
            std::fprintf(stderr, "[wave64] program 0x%llx: %u bytes of local memory per invocation at two lanes, %s at one; %s\n", static_cast<unsigned long long>(request.shader.codeAddress), *bytes, singleBytes ? std::to_string(*singleBytes).c_str() : "unknown", takeSingle ? "taking one lane per invocation" : "keeping two lanes per invocation");
            const auto keptBytes = takeSingle ? singleBytes : bytes;
            if (keptBytes.has_value() && *keptBytes != 0u) {
                if (auto reserved = compileWithoutLocalMemory(takeSingle ? single : request, resourceSnapshot, resourceSpecialization)) return std::move(*reserved);
            }
            if (takeSingle) return singleVariant;
        }
    }

    result.bdaAbiVersion = program.Info().usesDma ? request.target.bdaAbiVersion : 0u;
    result.memoryOffsetDword = bindings.layout.memoryOffsetDword;
    result.vertexOffsetSgpr = program.Info().vertexOffsetSgpr;
    result.instanceOffsetSgpr = program.Info().instanceOffsetSgpr;
    result.vertexOffsetShared = program.Info().vertexOffsetShared;
    result.instanceOffsetShared = program.Info().instanceOffsetShared;
    result.vertexOffsetConflict = program.Info().vertexOffsetConflict;
    result.instanceOffsetConflict = program.Info().instanceOffsetConflict;
    for (const auto& output : program.Info().outputs) {
        if (output.kind == StageOutputKind::Parameter) result.parameterExports.push_back(output.location);
    }
    if (request.shader.stage == ShaderStage::Fragment) result.fragmentParameters = DescribeFragmentParameters(program, inputInfo);
    if (request.shader.stage == ShaderStage::Vertex || request.shader.stage == ShaderStage::Local) {
        if (inputInfo.vertex == nullptr) throw std::runtime_error("vertex input metadata is missing");
        for (const auto& input : program.Info().inputs) {
            if (input.kind != StageInputKind::Parameter) continue;
            if (input.location >= static_cast<std::uint32_t>(inputInfo.vertex->resourcesNum)) throw std::runtime_error("vertex attribute location exceeds resource count");
            result.vertexAttributes.push_back({input.location, input.componentCount, {inputInfo.vertex->resources[input.location].fields}, inputInfo.vertex->resourcesDst[input.location].fetchIndex});
        }
    }

    result.bindings.clear();
    result.pushConstants.clear();
    for (auto& attribute : result.vertexAttributes) attribute.resource = {};
    bindings.bindings.clear();
    bindings.pushConstants.clear();
    return {resourceSpecialization, request.layout, std::move(program).TakeCompiledInfo(), std::move(bindings), std::move(result)};
}

RecompileResult materializeResult(const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot) {
    auto result = variant.result;
    BindingAllocationResult bindings;
    bindings.layout = variant.bindings.layout;
    bindings.pushConstantOffsetBytes = variant.bindings.pushConstantOffsetBytes;
    bindings.pushConstantSizeBytes = variant.bindings.pushConstantSizeBytes;
    DescriptorBindingBuilder{}.Populate(bindings, variant.info.info, variant.info.stage, variant.info.userDataBase, snapshot, partialThreads(request));
    result.bindings = std::move(bindings.bindings);
    result.pushConstants = std::move(bindings.pushConstants);
    result.bdaWrites = variant.info.info.bdaWrites;
    for (auto& attribute : result.vertexAttributes) {
        if (!request.context.vertex || attribute.location >= request.context.vertex->resourcesNum) throw std::runtime_error("Shader cache: invalid vertex attribute metadata");
        attribute.resource = request.context.vertex->resources[attribute.location];
    }
    return result;
}

bool sameLayout(const BindingLayout& left, const BindingLayout& right) {
    return left.descriptorSet == right.descriptorSet && left.firstBinding == right.firstBinding && left.pushConstantOffsetBytes == right.pushConstantOffsetBytes && left.pushConstantSizeBytes == right.pushConstantSizeBytes;
}

// The variant of `source` for the request's layout and the specialization (under the source's
// mutex): the one in memory (`cacheHit`), else the one the shader disk cache stored in an earlier
// run (ShaderDiskCache.hpp), else a fresh compile, which is queued for the disk cache. A request
// under the debug probe neither reads nor writes the disk cache (the probe is only in the
// in-memory key).
std::shared_ptr<const CompiledVariant> findOrCompileVariant(SourceEntry& source, const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization, bool& cacheHit) {
    for (const auto& candidate : source.variants) {
        if (sameLayout(candidate->layout, request.layout) && candidate->specialization == specialization) {
            cacheHit = true;
            return candidate;
        }
    }
    cacheHit = false;
    const bool disk = ShaderDiskCache::Enabled() && !DebugProbeActive();
    std::vector<std::byte> diskKey;
    std::shared_ptr<const CompiledVariant> variant;
    if (disk) {
        ShaderDiskCache::BuildKey(request, request.target.subgroupSize, specialization, {source.settledLayout, source.settledWorkgroupReserve}, diskKey);
        CompiledVariant loaded;
        if (ShaderDiskCache::Load(diskKey, loaded)) {
            loaded.specialization = specialization;
            loaded.layout = request.layout;
            loaded.result.variantId = nextVariantId();
            variant = std::make_shared<const CompiledVariant>(std::move(loaded));
        }
    }
    if (variant == nullptr) {
        // An Auto program's later variants take the layout its first one settled on.
        const bool settledSingle = source.settledLayout == WaveLayout::SingleLane && WaveLayoutFor(request) == WaveLayout::Auto;
        const auto& compiled = settledSingle ? SingleLaneRequest(request) : request;
        auto program = source.program != nullptr && !settledSingle ? std::move(*source.program) : PrepareResourceProgram(compiled);
        source.program.reset();
        const bool settle = source.settledLayout == WaveLayout::Auto;
        variant = std::make_shared<const CompiledVariant>(compileVariant(compiled, std::move(program), snapshot, specialization, settle, nullptr, settle ? 0u : source.settledWorkgroupReserve));
        if (disk) ShaderDiskCache::Store(std::move(diskKey), variant);
    }
    source.program.reset();
    if (source.settledLayout == WaveLayout::Auto && WaveLayoutFor(request) == WaveLayout::Auto) {
        source.settledLayout = variant->result.waveLayout;
        source.settledWorkgroupReserve = variant->result.workgroupReserveBytes;
    }
    source.variants.push_back(variant);
    return variant;
}

// The cached variant of `source` for the specialization, compiled on first use.
RecompileResult materializeVariant(SourceEntry& source, const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization) {
    std::shared_ptr<const CompiledVariant> variant;
    bool cacheHit = false;
    {
        std::lock_guard lock(source.mutex);
        variant = findOrCompileVariant(source, request, snapshot, specialization, cacheHit);
    }
    auto result = materializeResult(*variant, request, snapshot);
    result.cacheHit = cacheHit;
    return result;
}

RecompileResult RecompileImpl(const RecompileRequest& request) {
    static_cast<void>(RequestInputInfo(request));
    RequestMemoryView memory(request.context.memory);
    const auto runtime = memory.MakeRuntime(request.context.userData, request.shader.codeAddress);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    constexpr ResourceMaterializer materializer;
    if (!request.useCache) {
        auto program = PrepareResourceProgram(request);
        const auto plan = materializer.ExtractPlan(program);
        materializer.Materialize(plan, runtime, snapshot, specialization);
        const auto variant = compileVariant(request, std::move(program), snapshot, specialization, true);
        return materializeResult(variant, request, snapshot);
    }
    const auto source = getSource(request);
    materializer.Materialize(*source->plan, runtime, snapshot, specialization);
    return materializeVariant(*source, request, snapshot, specialization);
}

// APS5_NO_RESULT_MEMO=1: every Recompile(request, capture) materializes its own result as before.
bool ResultMemo() {
    static const bool resultMemo = std::getenv("APS5_NO_RESULT_MEMO") == nullptr;
    return resultMemo;
}

constexpr std::size_t ResultMemoEntries = 256;

struct ResultMemoCounters {
    std::atomic<std::uint64_t> hits{0}, misses{0}, evictions{0}, populateNanoseconds{0};
    std::atomic<std::int64_t> lastReport{0};
};

ResultMemoCounters& resultMemoCounters() {
    static ResultMemoCounters counters;
    return counters;
}

void reportResultMemo() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& counters = resultMemoCounters();
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load(std::memory_order_relaxed);
    if (last == 0) {
        counters.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10'000'000'000ll || !counters.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const auto hits = counters.hits.exchange(0, std::memory_order_relaxed);
    const auto misses = counters.misses.exchange(0, std::memory_order_relaxed);
    const auto evictions = counters.evictions.exchange(0, std::memory_order_relaxed);
    const auto populate = counters.populateNanoseconds.exchange(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[recompile] result memo (10 s): %llu hits, %llu misses (%.1f%% hits), Populate %.1f us per miss / %.1f ms in total, %llu evictions\n", static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses), hits + misses != 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0, misses != 0 ? static_cast<double>(populate) / 1000.0 / static_cast<double>(misses) : 0.0, static_cast<double>(populate) / 1e6, static_cast<unsigned long long>(evictions));
}

// Everything materializeResult reads besides the variant: the snapshot (the descriptor words, the
// flattened SRT, the user data, the uniform fill) and, for the vertex family, the V# table the
// attributes are resolved from.
std::uint64_t snapshotHash(const RecompileRequest& request, const ResourceSnapshot& snapshot) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    const auto mixWords = [&](std::span<const std::uint32_t> words) {
        mix(words.size());
        for (const auto word : words) mix(word);
    };
    const auto mixDescriptors = [&](const std::vector<DescriptorValue>& values) {
        mix(values.size());
        for (const auto& value : values) {
            mix(value.dwordCount);
            for (std::uint32_t i = 0; i < value.dwordCount && i < value.dwords.size(); ++i) mix(value.dwords[i]);
        }
    };
    mixDescriptors(snapshot.buffers);
    mixDescriptors(snapshot.images);
    mixDescriptors(snapshot.samplers);
    mixWords(snapshot.flattenedSrt);
    mixWords(snapshot.userData);
    mix(static_cast<std::uint64_t>(snapshot.uniformFill.kind));
    mix(snapshot.uniformFill.resource);
    for (const auto stride : snapshot.uniformFill.groupStride) mix(stride);
    mix(snapshot.uniformFill.words);
    mix(snapshot.uniformFill.value);
    for (const auto threads : partialThreads(request)) mix(threads);
    if (request.context.vertex) {
        const auto& vertex = *request.context.vertex;
        const auto count = std::min<std::uint32_t>(vertex.resourcesNum, ShaderVertexStageInfo::MaxResources);
        mix(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            for (const auto field : vertex.resources[i].fields) mix(field);
        }
    } else {
        mix(1ull << 32u);
    }
    return hash;
}

// The memo'd result of `source`'s variant for the snapshot (design13 R5): a hit returns the shared
// object, a miss materializes outside the source mutex and inserts (a concurrent miss's object is
// as good). `memoHit` reports the hit.
std::shared_ptr<const RecompileResult> materializeMemoized(SourceEntry& source, const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization, bool* memoHit) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::shared_ptr<const CompiledVariant> variant;
    bool cacheHit = false;
    const auto hash = snapshotHash(request, snapshot);
    std::uint64_t index = 0;
    auto& counters = resultMemoCounters();
    {
        std::lock_guard lock(source.mutex);
        variant = findOrCompileVariant(source, request, snapshot, specialization, cacheHit);
        index = (variant->result.variantId * 0x9e3779b97f4a7c15ull) ^ hash;
        const auto found = source.memoIndex.find(index);
        if (found != source.memoIndex.end() && found->second->variantId == variant->result.variantId && found->second->hash == hash) {
            source.memo.splice(source.memo.begin(), source.memo, found->second);
            counters.hits.fetch_add(1, std::memory_order_relaxed);
            if (memoHit != nullptr) *memoHit = true;
            reportResultMemo();
            return found->second->result;
        }
    }
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto result = std::make_shared<RecompileResult>(materializeResult(*variant, request, snapshot));
    result->cacheHit = cacheHit;
    if (profile) counters.populateNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    counters.misses.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const RecompileResult> shared = std::move(result);
    {
        std::lock_guard lock(source.mutex);
        const auto found = source.memoIndex.find(index);
        if (found != source.memoIndex.end()) {
            if (found->second->variantId == variant->result.variantId && found->second->hash == hash) {
                source.memo.splice(source.memo.begin(), source.memo, found->second);
                shared = found->second->result;
            } else {
                source.memo.erase(found->second);
                source.memoIndex.erase(found);
            }
        }
        if (source.memoIndex.find(index) == source.memoIndex.end()) {
            source.memo.push_front({variant->result.variantId, hash, shared});
            source.memoIndex.emplace(index, source.memo.begin());
            while (source.memo.size() > ResultMemoEntries) {
                const auto& last = source.memo.back();
                source.memoIndex.erase((last.variantId * 0x9e3779b97f4a7c15ull) ^ last.hash);
                source.memo.pop_back();
                counters.evictions.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    reportResultMemo();
    return shared;
}

// The capture already resolved the source entry (stage input validation included) and materialized
// the request over exactly the words the driver captured, so neither is repeated here.
std::shared_ptr<const RecompileResult> RecompileImpl(const RecompileRequest& request, const ResourceCapture& capture, bool* memoHit) {
    if (!request.useCache || capture.source == nullptr) {
        auto program = PrepareResourceProgram(request);
        const auto variant = compileVariant(request, std::move(program), capture.snapshot, capture.specialization, true);
        return std::make_shared<const RecompileResult>(materializeResult(variant, request, capture.snapshot));
    }
    if (!ResultMemo()) return std::make_shared<const RecompileResult>(materializeVariant(*capture.source, request, capture.snapshot, capture.specialization));
    return materializeMemoized(*capture.source, request, capture.snapshot, capture.specialization, memoHit);
}

template <typename Impl>
auto recompileReporting(const RecompileRequest& request, Impl&& impl) -> decltype(impl()) {
    try {
        return impl();
    } catch (const std::exception& e) {
        constexpr auto requestSerializer = RequestSerializer{};
        const auto inputInfo = "\nRecompileRequest:\n" + requestSerializer.Serialize(request);
        throw std::runtime_error(std::string("ShaderRecompiler::Recompile: ") + e.what() + inputInfo);
    } catch (...) {
        throw std::runtime_error("ShaderRecompiler::Recompile: unknown exception");
    }
}

}

std::shared_ptr<const IrResourcePlan> GetResourcePlan(const RecompileRequest& request) {
    static_cast<void>(RequestInputInfo(request));
    if (request.useCache) return getSource(request)->plan;
    return makeResourcePlan(request);
}

namespace {

// The capture's materialization; with pure flat slots in the plan the walk's read addresses are
// traced into the capture (ResourceCapture::readTrace).
void materializeCapture(ResourceCapture& capture, const SrtRuntime& runtime) {
    const auto& plan = *capture.plan;
    if (std::none_of(plan.pureFlatSlots.begin(), plan.pureFlatSlots.end(), [](std::uint8_t pure) { return pure != 0u; })) {
        ResourceMaterializer{}.Materialize(plan, runtime, capture.snapshot, capture.specialization);
        return;
    }
    SrtRuntime traced = runtime;
    traced.readTrace = &capture.readTrace;
    ResourceMaterializer{}.Materialize(plan, traced, capture.snapshot, capture.specialization);
    auto& other = capture.readTrace.otherReads;
    std::sort(other.begin(), other.end());
    other.erase(std::unique(other.begin(), other.end()), other.end());
}

}

std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // Validates the stage inputs once per request, as GetResourcePlan and Recompile(request) do.
    static_cast<void>(RequestInputInfo(request));
    auto capture = std::make_shared<ResourceCapture>();
    if (request.useCache) {
        capture->source = getSource(request);
        capture->plan = capture->source->plan;
    } else {
        capture->plan = makeResourcePlan(request);
    }
    if (profile) capture->sourceNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    materializeCapture(*capture, runtime);
    return capture;
}

std::shared_ptr<const SourceHandle> ResolveSource(const RecompileRequest& request) {
    if (!request.useCache) return nullptr;
    static_cast<void>(RequestInputInfo(request));
    return std::make_shared<const SourceHandle>(SourceHandle{getSource(request)});
}

std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime, const SourceHandle& handle) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // The whole vertex family (Vertex, Local, TC, TE, Mesh) validates V# fields the memo key does not cover.
    if (request.shader.stage != ShaderStage::Compute && request.shader.stage != ShaderStage::Fragment) static_cast<void>(RequestInputInfo(request));
    auto capture = std::make_shared<ResourceCapture>();
    capture->source = handle.source;
    capture->plan = handle.source->plan;
    if (profile) capture->sourceNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    materializeCapture(*capture, runtime);
    return capture;
}

RecompileResult Recompile(const RecompileRequest& request) {
    return recompileReporting(request, [&] { return RecompileImpl(request); });
}

std::shared_ptr<const RecompileResult> Recompile(const RecompileRequest& request, const ResourceCapture& capture, bool* memoHit) {
    if (memoHit != nullptr) *memoHit = false;
    return recompileReporting(request, [&] { return RecompileImpl(request, capture, memoHit); });
}

}
