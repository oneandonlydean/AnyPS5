#include "Translation/NggPassthrough.hpp"

#include "ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/Structurizer.hpp"
#include "IntermediateRepresentation/IrMetadata.hpp"
#include "Optimization/ConstantFolder.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "Optimization/SsaBuilder.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Translation/InstructionTranslator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ShaderRecompiler {

namespace {

constexpr std::uint64_t MaxContexts = 3'000'000u;
constexpr std::uint32_t MaxCleanBools = 4u;
constexpr int VectorZeroMarker = static_cast<int>(SubgroupMarkerVectorOffset);
constexpr std::uint64_t GroupInfoUnmodelledBits = 0xffffffffu & ~((0x1ffu << 12u) | (0x1ffu << 22u));
constexpr std::uint64_t WaveInfoUnmodelledBits = 0x00ff0000u;
constexpr std::uint32_t AllocationVertexBits = 0x3ffu;
constexpr std::uint32_t AllocationPrimitiveShift = 12u;
constexpr std::uint32_t AllocationPrimitiveBits = 0x7ffu;

struct SubgroupContext {
    bool host;
    std::uint32_t lane;
    std::uint32_t vertices;
    std::uint32_t primitives;
    std::uint32_t wave;
    std::uint32_t waves;
    std::uint32_t groupVertices;
    std::uint32_t groupPrimitives;
    std::uint32_t waveSize;
};

using Word = std::array<std::uint64_t, 4>;

int markerOf(const IrValue* value) {
    if (value->Opcode() != IrOpcode::GetUserData || value->ArgumentCount() != 1u) return -1;
    const auto reg = value->Argument(0)->Register();
    if (reg.bank != RegisterBank::UserData || reg.index < SubgroupMarkerFirstRegister) return -1;
    return static_cast<int>(reg.index - SubgroupMarkerFirstRegister);
}

bool activeLanes(const IrValue* value) {
    if (value->Opcode() != IrOpcode::Ballot || value->ArgumentCount() != 1u) return false;
    const auto* argument = value->Argument(0)->Resolve();
    return argument->HasImmediate() && argument->Type() == IrType::Bool && argument->ImmediateBool();
}

bool forbiddenOpcode(IrOpcode opcode) {
    const std::string_view name = IrOpcodeName(opcode);
    for (const std::string_view part : {"Store", "Atomic", "Shared", "Lane", "Ballot", "Dpp", "Permlane", "Bpermute", "PermuteU32", "Wqm", "Gds", "Discard", "EmitVertex", "WriteImage", "ShaderClock", "RealtimeClock", "Barrier"}) {
        if (name.find(part) != std::string_view::npos && opcode != IrOpcode::LaneId) return true;
    }
    return false;
}

std::uint64_t widthBits(IrType type) {
    if (type == IrType::Bool) return 1u;
    if (type == IrType::U64 || type == IrType::S64) return ~0ull;
    return 0xffffffffu;
}

std::uint64_t mask(IrType type, std::uint64_t value) {
    if (type == IrType::Bool) return value != 0u ? 1u : 0u;
    if (type == IrType::U64 || type == IrType::S64) return value;
    return value & 0xffffffffu;
}

std::uint64_t signExtend32(std::uint64_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value))));
}

class Analysis {
public:
    Analysis(IrProgram& program, const NggSubgroupLimits& limits) : program(program), limits(limits) {}

    std::optional<std::string> Run() {
        std::vector<IrValue*> values;
        for (const auto& block : program.Blocks()) {
            for (IrValue* value : block->Instructions()) {
                if (value != nullptr && !value->IsEmpty()) {
                    values.push_back(value);
                    blockOf.emplace(value, block.get());
                }
            }
        }
        for (IrValue* value : values) {
            if (forbiddenOpcode(value->Opcode()) && !activeLanes(value)) return std::string("uses ") + std::string(IrOpcodeName(value->Opcode()));
            if (value->Opcode() == IrOpcode::Loop || value->Opcode() == IrOpcode::LoopMerge) return std::string("contains a loop");
        }
        if (auto failure = buildContexts()) return failure;
        for (IrValue* value : values) classify(value);
        std::uint32_t primitiveExports = 0;
        std::uint32_t allocations = 0;
        for (IrValue* value : values) {
            const auto opcode = value->Opcode();
            if (opcode == IrOpcode::SetAttribute) {
                const auto& info = program.Metadata().exportInfo.at(value->Flags<ExportFlags>().index);
                if (info.kind == ExportTargetKind::Primitive) {
                    if (!onEveryPath(blockOf.at(value))) return std::string("the primitive export is conditional");
                    if (auto failure = checkPrimitiveExport(*value)) return failure;
                    primitiveExports++;
                    continue;
                }
                for (std::size_t i = 0; i < value->ArgumentCount(); i++) {
                    if (dependent(value->Argument(i))) return std::string("an export depends on the subgroup or another lane");
                }
                continue;
            }
            if (opcode == IrOpcode::Reference || opcode == IrOpcode::BranchConditional) {
                for (std::size_t i = 0; i < value->ArgumentCount(); i++) {
                    if (dependent(value->Argument(i))) return std::string("a branch depends on the subgroup or another lane");
                }
                continue;
            }
            if (opcode == IrOpcode::MeshAllocate) {
                if (!onEveryPath(blockOf.at(value))) return std::string("the allocation request is conditional");
                if (auto failure = checkAllocation(*value)) return failure;
                allocations++;
                continue;
            }
            if (opcode == IrOpcode::Sendmsg) continue;
            if (value->MayHaveSideEffects()) {
                for (std::size_t i = 0; i < value->ArgumentCount(); i++) {
                    if (dependent(value->Argument(i))) return std::string(IrOpcodeName(opcode)) + " depends on the subgroup or another lane";
                }
            }
        }
        if (allocations != 1u) return std::string("sends ") + std::to_string(allocations) + " allocation requests instead of one";
        if (primitiveExports != 1u) return std::string("exports ") + std::to_string(primitiveExports) + " primitives instead of one";
        return std::nullopt;
    }

private:
    struct State {
        bool dependent = false;
        bool resolved = false;
        std::optional<bool> constant;
    };

    IrProgram& program;
    NggSubgroupLimits limits;
    std::unordered_map<const IrValue*, State> states;
    std::unordered_map<const IrValue*, const IrBlock*> blockOf;
    std::vector<SubgroupContext> vertexContexts;
    std::vector<SubgroupContext> laneContexts;

    std::optional<std::string> buildContexts() {
        const auto wave = limits.waveSize;
        if ((wave != 32u && wave != 64u) || limits.vertices == 0u || limits.primitives == 0u || limits.vertices > 256u || limits.primitives > 256u) return std::string("invalid subgroup limits");
        const std::uint64_t estimate = static_cast<std::uint64_t>(limits.vertices) * (limits.primitives + 1u) * ((std::max(limits.vertices, limits.primitives) + wave - 1u) / wave) * wave;
        if (estimate > MaxContexts) return std::string("subgroup limits too large to verify");
        for (std::uint32_t gv = 1; gv <= limits.vertices; gv++) {
            for (std::uint32_t gp = 1; gp <= limits.primitives; gp++) {
                const auto waves = (std::max(gv, gp) + wave - 1u) / wave;
                for (std::uint32_t k = 0; k < waves; k++) {
                    const auto nv = std::min(gv > k * wave ? gv - k * wave : 0u, wave);
                    const auto np = std::min(gp > k * wave ? gp - k * wave : 0u, wave);
                    for (std::uint32_t lane = 0; lane < wave; lane++) {
                        const SubgroupContext context{false, lane, nv, np, k, waves, gv, gp, wave};
                        laneContexts.push_back(context);
                        if (lane < nv) vertexContexts.push_back(context);
                    }
                }
            }
        }
        for (std::uint32_t lane = 0; lane < wave; lane++) vertexContexts.push_back({true, lane, wave, wave, 0u, 1u, wave, wave, wave});
        return std::nullopt;
    }

    bool dependent(const IrValue* value) {
        const auto it = states.find(value->Resolve());
        return it != states.end() && it->second.dependent && !it->second.resolved;
    }

    std::optional<bool> constantOf(const IrValue* value) {
        const auto* resolved = value->Resolve();
        if (resolved->HasImmediate() && resolved->Type() == IrType::Bool) return resolved->ImmediateBool();
        const auto it = states.find(resolved);
        if (it != states.end() && it->second.resolved) return it->second.constant;
        return std::nullopt;
    }

    void classify(IrValue* value) {
        auto& state = states[value];
        if (state.resolved) return;
        const auto opcode = value->Opcode();
        if (markerOf(value) >= 0 || opcode == IrOpcode::LaneId || activeLanes(value)) {
            state.dependent = true;
            return;
        }
        bool any = false;
        const auto select = opcode == IrOpcode::Select || opcode == IrOpcode::SelectU32 || opcode == IrOpcode::SelectU1 || opcode == IrOpcode::SelectF32;
        if (select && value->ArgumentCount() == 3u) {
            if (const auto condition = constantOf(value->Argument(0))) {
                state.dependent = dependent(value->Argument(*condition ? 1u : 2u));
                return;
            }
        }
        if ((opcode == IrOpcode::LogicalAnd || opcode == IrOpcode::LogicalOr) && value->ArgumentCount() == 2u) {
            for (std::size_t i = 0; i < 2u; i++) {
                const auto constant = constantOf(value->Argument(i));
                if (constant && *constant == (opcode == IrOpcode::LogicalOr)) {
                    state.dependent = true;
                    state.resolved = true;
                    state.constant = *constant;
                    return;
                }
            }
        }
        for (std::size_t i = 0; i < value->ArgumentCount(); i++) any = any || dependent(value->Argument(i));
        state.dependent = any;
        if (any && value->Type() == IrType::Bool && !value->IsPhi()) resolve(value, state);
    }

    enum class NodeKind : std::uint8_t { Fixed, Clean, Lane, GroupInfo, WaveInfo, ActiveLanes, Operation };

    struct Node {
        NodeKind kind = NodeKind::Fixed;
        IrOpcode opcode = IrOpcode::Void;
        IrType type = IrType::Void;
        std::array<std::uint32_t, 4> args{};
        std::uint32_t argumentCount = 0;
        Word fixed{};
        std::uint32_t clean = 0;
    };

    struct Evaluation {
        std::vector<Node> nodes;
        std::unordered_map<const IrValue*, std::uint32_t> slots;
        std::vector<const IrValue*> cleanBools;
    };

    static bool supported(IrOpcode opcode) {
        switch (opcode) {
        case IrOpcode::IAdd32: case IrOpcode::IAdd64: case IrOpcode::ISub32: case IrOpcode::ISub64: case IrOpcode::IMul32: case IrOpcode::IMul64:
        case IrOpcode::BitwiseAnd32: case IrOpcode::BitwiseAnd64: case IrOpcode::BitwiseOr32: case IrOpcode::BitwiseXor32: case IrOpcode::BitwiseNot32:
        case IrOpcode::ShiftLeftLogical32: case IrOpcode::ShiftRightLogical32: case IrOpcode::ShiftRightArithmetic32: case IrOpcode::ShiftLeftLogical64: case IrOpcode::ShiftRightLogical64:
        case IrOpcode::BitFieldUExtract: case IrOpcode::BitCount32: case IrOpcode::BitCount64: case IrOpcode::UMin32: case IrOpcode::UMax32: case IrOpcode::SMin32: case IrOpcode::SMax32:
        case IrOpcode::IEqual32: case IrOpcode::IEqual64: case IrOpcode::INotEqual32: case IrOpcode::INotEqual64: case IrOpcode::ULessThan32: case IrOpcode::ULessThan64:
        case IrOpcode::ULessThanEqual32: case IrOpcode::UGreaterThan32: case IrOpcode::UGreaterThan64: case IrOpcode::UGreaterThanEqual32:
        case IrOpcode::SLessThan32: case IrOpcode::SLessThanEqual32: case IrOpcode::SGreaterThan32: case IrOpcode::SGreaterThanEqual32:
        case IrOpcode::LogicalAnd: case IrOpcode::LogicalOr: case IrOpcode::LogicalXor: case IrOpcode::LogicalNot:
        case IrOpcode::Select: case IrOpcode::SelectU32: case IrOpcode::SelectU1:
        case IrOpcode::CompositeConstructU64: case IrOpcode::CompositeExtractU64: case IrOpcode::CompositeConstructU32x2: case IrOpcode::CompositeConstructU32x3: case IrOpcode::CompositeConstructU32x4:
        case IrOpcode::CompositeExtractU32x2: case IrOpcode::CompositeExtractU32x3: case IrOpcode::CompositeExtractU32x4: case IrOpcode::IAddCarry32:
            return true;
        default:
            return false;
        }
    }

    std::optional<std::uint32_t> collect(const IrValue* value, Evaluation& evaluation) {
        value = value->Resolve();
        if (const auto found = evaluation.slots.find(value); found != evaluation.slots.end()) return found->second;
        Node node;
        node.type = value->Type();
        const auto it = states.find(value);
        const bool isDependent = it != states.end() && it->second.dependent && !it->second.resolved;
        const int marker = markerOf(value);
        if (!isDependent) {
            if (value->HasImmediate()) {
                node.fixed[0] = value->Type() == IrType::Bool ? (value->ImmediateBool() ? 1u : 0u) : value->Type() == IrType::U64 || value->Type() == IrType::S64 ? value->ImmediateU64() : value->ImmediateU32();
            } else if (value->Type() != IrType::Bool) {
                return std::nullopt;
            } else if (const auto constant = constantOf(value)) {
                node.fixed[0] = *constant ? 1u : 0u;
            } else {
                if (evaluation.cleanBools.size() >= MaxCleanBools) return std::nullopt;
                node.kind = NodeKind::Clean;
                node.clean = static_cast<std::uint32_t>(evaluation.cleanBools.size());
                evaluation.cleanBools.push_back(value);
            }
        } else if (value->Opcode() == IrOpcode::LaneId) {
            node.kind = NodeKind::Lane;
        } else if (activeLanes(value)) {
            node.kind = NodeKind::ActiveLanes;
        } else if (marker == 2 || marker == 3) {
            node.kind = marker == 2 ? NodeKind::GroupInfo : NodeKind::WaveInfo;
        } else if (marker >= 0 || value->IsPhi() || !supported(value->Opcode()) || value->ArgumentCount() > 4u) {
            return std::nullopt;
        } else {
            node.kind = NodeKind::Operation;
            node.opcode = value->Opcode();
            node.argumentCount = static_cast<std::uint32_t>(value->ArgumentCount());
            for (std::size_t i = 0; i < value->ArgumentCount(); i++) {
                const auto slot = collect(value->Argument(i), evaluation);
                if (!slot) return std::nullopt;
                node.args[i] = *slot;
            }
        }
        const auto slot = static_cast<std::uint32_t>(evaluation.nodes.size());
        evaluation.nodes.push_back(node);
        evaluation.slots.emplace(value, slot);
        return slot;
    }

    static Word laneUnknowns(const Node& node, const SubgroupContext& context, const std::vector<Word>& scratch, const std::vector<Word>& unknowns) {
        const auto full = widthBits(node.type);
        Word result{};
        if (node.kind == NodeKind::ActiveLanes) {
            if (!context.host) return result;
            const auto lanes = [&](std::uint32_t first) -> std::uint64_t {
                if (first >= context.waveSize) return 0u;
                const auto count = std::min(context.waveSize - first, 32u);
                const auto all = count == 32u ? 0xffffffffull : (1ull << count) - 1u;
                return context.lane >= first && context.lane < first + 32u ? all & ~(1ull << (context.lane - first)) : all;
            };
            return {lanes(0u), lanes(32u), 0u, 0u};
        }
        if (node.kind != NodeKind::Operation) return result;
        const auto u = [&](std::size_t i) -> const Word& { return unknowns[node.args[i]]; };
        const auto a = [&](std::size_t i) -> const Word& { return scratch[node.args[i]]; };
        bool any = false;
        for (std::uint32_t i = 0; i < node.argumentCount; i++) {
            for (const auto bits : u(i)) any = any || bits != 0u;
        }
        if (!any) return result;
        switch (node.opcode) {
        case IrOpcode::BitwiseAnd32: case IrOpcode::BitwiseAnd64:
            result[0] = ((u(0)[0] & (a(1)[0] | u(1)[0])) | (u(1)[0] & (a(0)[0] | u(0)[0]))) & full;
            break;
        case IrOpcode::BitwiseOr32:
            result[0] = ((u(0)[0] & ~(a(1)[0] & ~u(1)[0])) | (u(1)[0] & ~(a(0)[0] & ~u(0)[0]))) & full;
            break;
        case IrOpcode::ShiftLeftLogical32: case IrOpcode::ShiftRightLogical32: case IrOpcode::ShiftLeftLogical64: case IrOpcode::ShiftRightLogical64: {
            const auto width = node.opcode == IrOpcode::ShiftLeftLogical64 || node.opcode == IrOpcode::ShiftRightLogical64 ? 64u : 32u;
            if (u(1)[0] != 0u) {
                result[0] = full;
                break;
            }
            const auto amount = a(1)[0] & (width - 1u);
            result[0] = node.opcode == IrOpcode::ShiftLeftLogical32 || node.opcode == IrOpcode::ShiftLeftLogical64 ? (u(0)[0] << amount) & full : (u(0)[0] & full) >> amount;
            break;
        }
        case IrOpcode::Select: case IrOpcode::SelectU32: case IrOpcode::SelectU1:
            if (u(0)[0] != 0u) result.fill(full);
            else result = a(0)[0] != 0u ? u(1) : u(2);
            break;
        case IrOpcode::CompositeExtractU32x2: case IrOpcode::CompositeExtractU32x3: case IrOpcode::CompositeExtractU32x4:
            result[0] = u(1)[0] != 0u ? full : u(0)[a(1)[0] & 3u];
            break;
        case IrOpcode::CompositeConstructU32x2: case IrOpcode::CompositeConstructU32x3: case IrOpcode::CompositeConstructU32x4:
            for (std::uint32_t i = 0; i < node.argumentCount; i++) result[i] = u(i)[0];
            break;
        default:
            result.fill(full);
            break;
        }
        return result;
    }

    static std::optional<Word> evaluate(const Evaluation& evaluation, const SubgroupContext& context, std::uint32_t assignment, std::vector<Word>& scratch) {
        thread_local std::vector<Word> unknowns;
        scratch.resize(evaluation.nodes.size());
        unknowns.assign(evaluation.nodes.size(), Word{});
        for (std::size_t index = 0; index < evaluation.nodes.size(); index++) {
            const Node& node = evaluation.nodes[index];
            Word result{};
            switch (node.kind) {
            case NodeKind::Fixed: result = node.fixed; break;
            case NodeKind::Clean: result[0] = (assignment >> node.clean) & 1u; break;
            case NodeKind::Lane: result[0] = context.lane; break;
            case NodeKind::GroupInfo: result[0] = (static_cast<std::uint64_t>(context.groupVertices) << 12u) | (static_cast<std::uint64_t>(context.groupPrimitives) << 22u); break;
            case NodeKind::WaveInfo: result[0] = context.vertices | (context.primitives << 8u) | (context.wave << 24u) | (static_cast<std::uint64_t>(context.waves) << 28u); break;
            case NodeKind::ActiveLanes:
                if (context.host) {
                    result[context.lane / 32u] = 1ull << (context.lane % 32u);
                } else {
                    result = {0xffffffffu, context.waveSize == 64u ? 0xffffffffu : 0u, 0u, 0u};
                }
                break;
            case NodeKind::Operation: {
                const auto arg = [&](std::size_t i) -> const Word& { return scratch[node.args[i]]; };
                const auto a = [&](std::size_t i) { return arg(i)[0]; };
                const auto type = node.type;
                const auto shift = [&](std::uint64_t amount, std::uint32_t width) -> std::optional<std::uint64_t> {
                    if (context.host && width == 32u && amount >= width) return std::nullopt;
                    return amount & (width - 1u);
                };
                switch (node.opcode) {
                case IrOpcode::IAdd32: case IrOpcode::IAdd64: result[0] = mask(type, a(0) + a(1)); break;
                case IrOpcode::ISub32: case IrOpcode::ISub64: result[0] = mask(type, a(0) - a(1)); break;
                case IrOpcode::IMul32: case IrOpcode::IMul64: result[0] = mask(type, a(0) * a(1)); break;
                case IrOpcode::BitwiseAnd32: case IrOpcode::BitwiseAnd64: result[0] = a(0) & a(1); break;
                case IrOpcode::BitwiseOr32: result[0] = a(0) | a(1); break;
                case IrOpcode::BitwiseXor32: result[0] = a(0) ^ a(1); break;
                case IrOpcode::BitwiseNot32: result[0] = mask(type, ~a(0)); break;
                case IrOpcode::ShiftLeftLogical32: case IrOpcode::ShiftRightLogical32: case IrOpcode::ShiftRightArithmetic32: {
                    const auto amount = shift(a(1), 32u);
                    if (!amount) return std::nullopt;
                    result[0] = node.opcode == IrOpcode::ShiftLeftLogical32 ? mask(type, a(0) << *amount) : node.opcode == IrOpcode::ShiftRightLogical32 ? mask(type, a(0)) >> *amount : mask(type, signExtend32(a(0)) >> *amount);
                    break;
                }
                case IrOpcode::ShiftLeftLogical64: case IrOpcode::ShiftRightLogical64: {
                    const auto amount = shift(a(1), 64u);
                    if (!amount) return std::nullopt;
                    result[0] = node.opcode == IrOpcode::ShiftLeftLogical64 ? a(0) << *amount : a(0) >> *amount;
                    break;
                }
                case IrOpcode::BitFieldUExtract: {
                    const auto offset = a(1);
                    const auto count = a(2);
                    if (offset > 31u || count > 32u || offset + count > 32u) return std::nullopt;
                    result[0] = count == 0u ? 0u : (a(0) >> offset) & ((1ull << count) - 1u);
                    break;
                }
                case IrOpcode::BitCount32: case IrOpcode::BitCount64: result[0] = static_cast<std::uint64_t>(std::popcount(a(0))); break;
                case IrOpcode::UMin32: result[0] = std::min(a(0), a(1)); break;
                case IrOpcode::UMax32: result[0] = std::max(a(0), a(1)); break;
                case IrOpcode::SMin32: result[0] = mask(type, static_cast<std::uint64_t>(std::min(static_cast<std::int64_t>(signExtend32(a(0))), static_cast<std::int64_t>(signExtend32(a(1)))))); break;
                case IrOpcode::SMax32: result[0] = mask(type, static_cast<std::uint64_t>(std::max(static_cast<std::int64_t>(signExtend32(a(0))), static_cast<std::int64_t>(signExtend32(a(1)))))); break;
                case IrOpcode::IEqual32: case IrOpcode::IEqual64: result[0] = a(0) == a(1); break;
                case IrOpcode::INotEqual32: case IrOpcode::INotEqual64: result[0] = a(0) != a(1); break;
                case IrOpcode::ULessThan32: case IrOpcode::ULessThan64: result[0] = a(0) < a(1); break;
                case IrOpcode::ULessThanEqual32: result[0] = a(0) <= a(1); break;
                case IrOpcode::UGreaterThan32: case IrOpcode::UGreaterThan64: result[0] = a(0) > a(1); break;
                case IrOpcode::UGreaterThanEqual32: result[0] = a(0) >= a(1); break;
                case IrOpcode::SLessThan32: result[0] = static_cast<std::int64_t>(signExtend32(a(0))) < static_cast<std::int64_t>(signExtend32(a(1))); break;
                case IrOpcode::SLessThanEqual32: result[0] = static_cast<std::int64_t>(signExtend32(a(0))) <= static_cast<std::int64_t>(signExtend32(a(1))); break;
                case IrOpcode::SGreaterThan32: result[0] = static_cast<std::int64_t>(signExtend32(a(0))) > static_cast<std::int64_t>(signExtend32(a(1))); break;
                case IrOpcode::SGreaterThanEqual32: result[0] = static_cast<std::int64_t>(signExtend32(a(0))) >= static_cast<std::int64_t>(signExtend32(a(1))); break;
                case IrOpcode::LogicalAnd: result[0] = (a(0) != 0u) && (a(1) != 0u); break;
                case IrOpcode::LogicalOr: result[0] = (a(0) != 0u) || (a(1) != 0u); break;
                case IrOpcode::LogicalXor: result[0] = (a(0) != 0u) != (a(1) != 0u); break;
                case IrOpcode::LogicalNot: result[0] = a(0) == 0u; break;
                case IrOpcode::Select: case IrOpcode::SelectU32: case IrOpcode::SelectU1: result = a(0) != 0u ? arg(1) : arg(2); break;
                case IrOpcode::CompositeConstructU64: result[0] = (a(0) & 0xffffffffu) | (a(1) << 32u); break;
                case IrOpcode::CompositeExtractU64: result[0] = a(1) == 0u ? a(0) & 0xffffffffu : a(0) >> 32u; break;
                case IrOpcode::CompositeConstructU32x2: result = {a(0), a(1)}; break;
                case IrOpcode::CompositeConstructU32x3: result = {a(0), a(1), a(2)}; break;
                case IrOpcode::CompositeConstructU32x4: result = {a(0), a(1), a(2), a(3)}; break;
                case IrOpcode::CompositeExtractU32x2: case IrOpcode::CompositeExtractU32x3: case IrOpcode::CompositeExtractU32x4: result[0] = arg(0)[a(1) & 3u]; break;
                case IrOpcode::IAddCarry32: {
                    const auto sum = a(0) + a(1);
                    result = {sum & 0xffffffffu, sum >> 32u};
                    break;
                }
                default:
                    return std::nullopt;
                }
                break;
            }
            }
            scratch[index] = result;
            unknowns[index] = laneUnknowns(node, context, scratch, unknowns);
        }
        if (unknowns.back()[0] != 0u) return std::nullopt;
        return scratch.back();
    }

    static std::vector<Word> unmodelledBits(const Evaluation& evaluation) {
        std::vector<Word> unknown(evaluation.nodes.size());
        for (std::size_t index = 0; index < evaluation.nodes.size(); index++) {
            const Node& node = evaluation.nodes[index];
            const auto full = widthBits(node.type);
            Word result{};
            if (node.kind == NodeKind::GroupInfo) {
                result[0] = GroupInfoUnmodelledBits;
            } else if (node.kind == NodeKind::WaveInfo) {
                result[0] = WaveInfoUnmodelledBits;
            } else if (node.kind == NodeKind::Operation) {
                const auto u = [&](std::size_t i) -> const Word& { return unknown[node.args[i]]; };
                const auto constant = [&](std::size_t i) -> std::optional<std::uint64_t> {
                    const Node& argument = evaluation.nodes[node.args[i]];
                    if (argument.kind != NodeKind::Fixed) return std::nullopt;
                    return argument.fixed[0];
                };
                bool any = false;
                for (std::uint32_t i = 0; i < node.argumentCount; i++) {
                    for (const auto bits : u(i)) any = any || bits != 0u;
                }
                const auto shifted = [&](std::uint32_t width) -> std::optional<std::uint64_t> {
                    const auto amount = constant(1);
                    if (!amount || *amount >= width) return std::nullopt;
                    return *amount;
                };
                const auto upward = [&](std::uint64_t bits) -> std::uint64_t {
                    bits &= full;
                    if (bits == 0u) return 0u;
                    return full & ~((bits & (~bits + 1u)) - 1u);
                };
                switch (node.opcode) {
                case IrOpcode::BitwiseAnd32: case IrOpcode::BitwiseAnd64:
                    if (const auto c = constant(1)) result[0] = u(0)[0] & *c;
                    else if (const auto c0 = constant(0)) result[0] = u(1)[0] & *c0;
                    else result[0] = u(0)[0] | u(1)[0];
                    break;
                case IrOpcode::BitwiseOr32:
                    if (const auto c = constant(1)) result[0] = u(0)[0] & ~*c;
                    else if (const auto c0 = constant(0)) result[0] = u(1)[0] & ~*c0;
                    else result[0] = u(0)[0] | u(1)[0];
                    break;
                case IrOpcode::BitwiseXor32: result[0] = u(0)[0] | u(1)[0]; break;
                case IrOpcode::BitwiseNot32: result[0] = u(0)[0] & full; break;
                case IrOpcode::ShiftLeftLogical32: case IrOpcode::ShiftLeftLogical64: case IrOpcode::ShiftRightLogical32: case IrOpcode::ShiftRightLogical64: case IrOpcode::ShiftRightArithmetic32: {
                    const bool wide = node.opcode == IrOpcode::ShiftLeftLogical64 || node.opcode == IrOpcode::ShiftRightLogical64;
                    const auto amount = shifted(wide ? 64u : 32u);
                    if (!amount) {
                        const std::uint64_t amountBits = wide ? 63u : 31u;
                        result[0] = (u(0)[0] & full) != 0u || (u(1)[0] & amountBits) != 0u ? full : 0u;
                        break;
                    }
                    const auto bits = u(0)[0] & full;
                    if (node.opcode == IrOpcode::ShiftLeftLogical32 || node.opcode == IrOpcode::ShiftLeftLogical64) result[0] = (bits << *amount) & full;
                    else result[0] = bits >> *amount;
                    if (node.opcode == IrOpcode::ShiftRightArithmetic32 && (bits & 0x80000000u) != 0u) result[0] |= full & ~(full >> *amount);
                    break;
                }
                case IrOpcode::BitFieldUExtract: {
                    const auto offset = constant(1);
                    const auto count = constant(2);
                    if (!offset || !count || *offset > 31u || *count > 32u || *offset + *count > 32u) {
                        result[0] = any ? full : 0u;
                        break;
                    }
                    result[0] = *count == 0u ? 0u : (u(0)[0] >> *offset) & ((1ull << *count) - 1u);
                    break;
                }
                case IrOpcode::CompositeConstructU64: result[0] = (u(0)[0] & 0xffffffffu) | (u(1)[0] << 32u); break;
                case IrOpcode::CompositeExtractU64:
                    if (const auto c = constant(1)) result[0] = *c == 0u ? u(0)[0] & 0xffffffffu : u(0)[0] >> 32u;
                    else result[0] = any ? full : 0u;
                    break;
                case IrOpcode::CompositeConstructU32x2: case IrOpcode::CompositeConstructU32x3: case IrOpcode::CompositeConstructU32x4:
                    for (std::uint32_t i = 0; i < node.argumentCount; i++) result[i] = u(i)[0];
                    break;
                case IrOpcode::CompositeExtractU32x2: case IrOpcode::CompositeExtractU32x3: case IrOpcode::CompositeExtractU32x4:
                    if (const auto c = constant(1); c && u(1)[0] == 0u) result[0] = u(0)[*c & 3u];
                    else result[0] = any ? full : 0u;
                    break;
                case IrOpcode::Select: case IrOpcode::SelectU32: case IrOpcode::SelectU1:
                    if (u(0)[0] != 0u) result.fill(any ? full : 0u);
                    else for (std::size_t i = 0; i < result.size(); i++) result[i] = u(1)[i] | u(2)[i];
                    break;
                case IrOpcode::LogicalAnd: case IrOpcode::LogicalOr: {
                    const bool absorbing = node.opcode == IrOpcode::LogicalOr;
                    const auto c0 = constant(0);
                    const auto c1 = constant(1);
                    if ((c0 && (*c0 != 0u) == absorbing) || (c1 && (*c1 != 0u) == absorbing)) result[0] = 0u;
                    else result[0] = any ? 1u : 0u;
                    break;
                }
                case IrOpcode::IAdd32: case IrOpcode::IAdd64: case IrOpcode::ISub32: case IrOpcode::ISub64: case IrOpcode::IMul32: case IrOpcode::IMul64:
                    result[0] = upward(u(0)[0] | u(1)[0]);
                    break;
                case IrOpcode::IAddCarry32:
                    if (any) result = {upward(u(0)[0] | u(1)[0]) & 0xffffffffu, 1u};
                    break;
                default:
                    if (any) result.fill(full);
                    break;
                }
            }
            unknown[index] = result;
        }
        return unknown;
    }

    std::string signature(const Evaluation& evaluation) const {
        std::string key;
        key.reserve(16u + evaluation.nodes.size() * sizeof(Node));
        const auto append = [&key](const auto& field) { key.append(reinterpret_cast<const char*>(&field), sizeof(field)); };
        append(limits.waveSize);
        append(limits.vertices);
        append(limits.primitives);
        for (const Node& node : evaluation.nodes) {
            append(node.kind);
            append(node.opcode);
            append(node.type);
            append(node.args);
            append(node.argumentCount);
            append(node.fixed);
            append(node.clean);
        }
        return key;
    }

    void resolve(IrValue* value, State& state) {
        Evaluation evaluation;
        if (!collect(value, evaluation)) return;
        struct Resolution {
            bool resolved;
            std::optional<bool> constant;
        };
        static std::mutex cacheMutex;
        static std::unordered_map<std::string, Resolution> cache;
        const auto key = signature(evaluation);
        {
            std::lock_guard lock(cacheMutex);
            if (const auto found = cache.find(key); found != cache.end()) {
                state.resolved = found->second.resolved;
                state.constant = found->second.constant;
                return;
            }
        }
        const auto remember = [&](bool resolved, std::optional<bool> constant) {
            std::lock_guard lock(cacheMutex);
            cache.emplace(key, Resolution{resolved, constant});
            state.resolved = resolved;
            state.constant = constant;
        };
        if (unmodelledBits(evaluation).back()[0] != 0u) {
            remember(false, std::nullopt);
            return;
        }
        std::vector<Word> scratch;
        std::optional<bool> constant;
        bool first = true;
        for (std::uint32_t assignment = 0; assignment < (1u << evaluation.cleanBools.size()); assignment++) {
            std::optional<std::uint64_t> reference;
            for (const auto& context : vertexContexts) {
                const auto word = evaluate(evaluation, context, assignment, scratch);
                if (!word || (reference && *reference != (*word)[0])) {
                    remember(false, std::nullopt);
                    return;
                }
                if (!reference) reference = (*word)[0];
            }
            const bool bit = reference.value_or(0u) != 0u;
            if (first) constant = bit;
            else if (constant && *constant != bit) constant.reset();
            first = false;
        }
        remember(true, constant);
    }

    std::optional<std::string> checkPrimitiveExport(const IrValue& exportValue) {
        if (exportValue.ArgumentCount() != 2u) return std::string("malformed primitive export");
        const IrValue* data = exportValue.Argument(0)->Resolve();
        if (data->Opcode() != IrOpcode::CompositeConstructU32x4 || markerOf(data->Argument(0)->Resolve()) != VectorZeroMarker) return std::string("exports a primitive other than the one it received");
        Evaluation evaluation;
        if (!collect(exportValue.Argument(1), evaluation) || !evaluation.cleanBools.empty() || unmodelledBits(evaluation).back()[0] != 0u) return std::string("the primitive export mask is not a subgroup function");
        std::vector<Word> scratch;
        for (const auto& context : laneContexts) {
            const auto word = evaluate(evaluation, context, 0u, scratch);
            if (!word) return std::string("the primitive export mask is not a subgroup function");
            if (((*word)[0] != 0u) != (context.lane < context.primitives)) return std::string("does not export exactly the primitives of its wave");
        }
        return std::nullopt;
    }

    std::optional<std::string> checkAllocation(const IrValue& allocation) {
        Evaluation evaluation;
        if (allocation.ArgumentCount() != 1u || !collect(allocation.Argument(0), evaluation) || !evaluation.cleanBools.empty()) return std::string("the allocation request is not a subgroup function");
        const auto requested = (static_cast<std::uint64_t>(AllocationPrimitiveBits) << AllocationPrimitiveShift) | AllocationVertexBits;
        if ((unmodelledBits(evaluation).back()[0] & requested) != 0u) return std::string("the allocation request is not a subgroup function");
        std::vector<Word> scratch;
        for (const auto& context : laneContexts) {
            if (context.lane != 0u) continue;
            if (context.waves != 1u) return std::string("subgroups of up to ") + std::to_string(std::max(limits.vertices, limits.primitives)) + " threads span more than one wave and every wave requests the allocation";
            const auto word = evaluate(evaluation, context, 0u, scratch);
            if (!word) return std::string("the allocation request is not a subgroup function");
            const auto vertices = (*word)[0] & AllocationVertexBits;
            const auto primitives = ((*word)[0] >> AllocationPrimitiveShift) & AllocationPrimitiveBits;
            if (vertices != context.groupVertices || primitives != context.groupPrimitives) return std::string("does not allocate exactly the vertices and primitives of its subgroup");
        }
        return std::nullopt;
    }

    bool onEveryPath(const IrBlock* block) const {
        const IrBlock* entry = nullptr;
        for (const auto& candidate : program.Blocks()) {
            if (candidate->Predecessors().empty()) {
                if (entry != nullptr) return false;
                entry = candidate.get();
            }
        }
        if (entry == nullptr) return false;
        if (entry == block) return true;
        std::vector<const IrBlock*> pending{entry};
        std::unordered_map<const IrBlock*, bool> seen{{entry, true}};
        while (!pending.empty()) {
            const IrBlock* current = pending.back();
            pending.pop_back();
            if (current->Successors().empty()) return false;
            for (const IrBlock* next : current->Successors()) {
                if (next == block || seen.contains(next)) continue;
                seen.emplace(next, true);
                pending.push_back(next);
            }
        }
        return true;
    }
};

}

std::optional<std::string> NggPassthroughSubgroupDependence(std::span<const std::uint32_t> code, std::uint32_t userDataCount, const NggSubgroupLimits& limits) {
    constexpr RdnaInstructionDecoder decoder;
    const auto decoded = decoder.Decode(code);
    for (const auto& instruction : decoded.instructions) {
        if (instruction.family == RdnaInstructionFamily::DS) return std::string("uses LDS or GDS");
    }
    constexpr GraphBuilder graphBuilder;
    auto cfg = graphBuilder.Build(decoded);
    constexpr Structurizer structurizer;
    structurizer.Structurize(cfg);
    ShaderVertexInputInfo vertex{};
    TranslateOptions options{};
    options.stage = ShaderStageKind::Vertex;
    options.waveSize = limits.waveSize;
    options.userDataBaseRegister = 8u;
    options.userDataCount = std::min(userDataCount, NumScalarRegs - 8u);
    options.inputInfo.vertex = &vertex;
    options.subgroupContextMarkers = true;
    constexpr InstructionTranslator translator;
    auto program = translator.Translate(decoded, cfg, options);
    constexpr SsaBuilder ssaBuilder;
    ssaBuilder.Rewrite(program);
    constexpr ConstantFolder constantFolder;
    constantFolder.Fold(program);
    ResolveControlFlowIdentities(program);
    constexpr DeadCodeEliminator deadCodeEliminator;
    deadCodeEliminator.RemoveIdentities(program);
    deadCodeEliminator.Eliminate(program);
    return Analysis(program, limits).Run();
}

}
