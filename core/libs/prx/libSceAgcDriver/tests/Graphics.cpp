#include "BdaTests.hpp"
#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ParallelCompare.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "SpirvBackend/SpirvOptimizer.hpp"
#include "RdnaDecoder/RdnaImageOpDecoder.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

alignas(256) std::array<std::byte, 1024> colorMemory{};

AgcDriver::QueueState makeState() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    queue.context = {
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040}
    };
    const auto address = reinterpret_cast<std::uintptr_t>(colorMemory.data());
    queue.context[0x318] = static_cast<std::uint32_t>(address >> 8u);
    queue.context[0x390] = static_cast<std::uint32_t>(address >> 40u);
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x110] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-2.0f);
    queue.context[0x112] = std::bit_cast<std::uint32_t>(2.0f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = 0;
    queue.context[0xb4] = 0;
    queue.context[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    return queue;
}

template<typename TAction>
void expectFailure(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected failure: ") + error.what());
        return;
    }
    throw std::runtime_error("expected graphics rejection: " + std::string(reason));
}

void stateTests() {
    AgcDriver::QueueState initial;
    Require(initial.context.at(0x200) == 0 && initial.context.at(0x83) == 0xffff, "initial context state is missing");
    initial.context[0x200] = 7;
    initial.context[0xdead] = 1;
    initial.ClearContext();
    Require(initial.context.at(0x200) == 0 && !initial.context.contains(0xdead), "context reset did not restore defaults");
    Require(initial.userConfig.at(0x24b) == 0, "primitive restart must be disabled in initial queue state");
    initial.userConfig[0x24b] = 1;
    initial.ClearContext();
    Require(initial.userConfig.at(0x24b) == 1, "context clear must preserve user configuration");
    initial = AgcDriver::QueueState{};
    Require(initial.userConfig.at(0x24b) == 0, "queue reset must disable primitive restart");
    auto queue = makeState();
    auto state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.color.address == reinterpret_cast<std::uintptr_t>(colorMemory.data()) && state.color.bytes == colorMemory.size(), "render-target address or size changed");
    Require(state.viewport.y == 4 && state.viewport.height == -4, "negative viewport height was lost");
    Require(state.color.format == VK_FORMAT_R8G8B8A8_UNORM, "RGBA format changed");
    queue.userConfig[0x24b] = 1;
    queue.context[0x1b3] = 2;
    queue.context[0x1b4] = 2;
    (void)AgcDriver::Graphics::DecodeState(queue);
    Require(AgcDriver::Graphics::DrawRejection(queue, false).empty(), "primitive restart rejected a non-indexed draw");
    queue.userConfig[0x242] = 9;
    Require(AgcDriver::Graphics::DrawRejection(queue, true).find("point, line and triangle") != std::string::npos, "primitive restart was accepted for patches");
    queue.userConfig[0x242] = 6;
    queue.context[0x103] = 0xffffffffu;
    Require(AgcDriver::Graphics::DrawRejection(queue, true).empty(), "primitive restart was rejected for an indexed strip");
    Require(AgcDriver::Graphics::DecodeState(queue).primitiveRestart, "primitive restart was not decoded for a strip");
    queue.context[0x103] = 5;
    Require(AgcDriver::Graphics::DrawRejection(queue, true).find("all ones") != std::string::npos, "a restart index other than all ones was accepted");
    queue = makeState();
    queue.userConfig.erase(0x24b);
    queue.context[0x2a5] = 0;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "user-config bank at DWORD 0x24b");
    queue = makeState();
    queue.context[0x90] = 0x80010003;
    queue.context[0x91] = 0x30020;
    state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.scissor.offset.x == 3 && state.scissor.offset.y == 1 && state.scissor.extent.width == 29 && state.scissor.extent.height == 2, "scissor intersection changed");
    // DCC_ENABLE: the target is rendered uncompressed and only its fast-clear keys are read.
    queue.context[0x31c] |= 0x10000000;
    queue.context[0x325] = 0x1234;
    Require(AgcDriver::Graphics::DecodeState(queue).color.dccAddress == 0x123400, "DCC key address changed");
    queue.context[0x31c] |= 0x20000000;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "DCC");
    queue = makeState();
    queue.context.erase(0x3b8);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
    queue = makeState();
    queue.context[0x3b8] |= 5u << 14u;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "unsupported color tile mode");
    queue = makeState();
    queue.context[0x3b0] = (62u << 14u) | 3u;
    Require(AgcDriver::Graphics::DecodeState(queue).color.bytes == 64u * 4u * 4u, "padded linear pitch changed");
    queue = makeState();
    // A second written slot needs its own CB_COLOR1 registers.
    queue.context[0x8e] = 0xff;
    queue.context[0x8f] = 0xff;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "color export format 0");
    queue.context[0x8f] = 0xfff;
    queue.context[0x1c5] = 0x999;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
    queue.context[0x8e] = 0xf0f;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register in context bank at DWORD 0x33a");
    alignas(256) static std::array<std::byte, 1024> slotTwoMemory{};
    const auto slotTwo = reinterpret_cast<std::uintptr_t>(slotTwoMemory.data());
    for (const auto offset : {0x31bu, 0x31cu, 0x31du}) queue.context[offset + 2u * 0xfu] = queue.context.at(offset);
    for (const auto offset : {0x3b0u, 0x3b8u}) queue.context[offset + 2u] = queue.context.at(offset);
    queue.context[0x318 + 2u * 0xfu] = static_cast<std::uint32_t>(slotTwo >> 8u);
    queue.context[0x390 + 2u] = static_cast<std::uint32_t>(slotTwo >> 40u);
    queue.context[0x1e2] = 0x40010001u;
    for (std::uint32_t i = 0; i < 4; ++i) queue.context[0x105 + i] = 0;
    queue.context[0x1c5] = 0x909u;
    state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.colors.size() == 2 && state.blends.size() == 3 && state.colors[0].exportIndex == 0 && state.colors[1].exportIndex == 2 && state.colors[1].address == slotTwo, "the slots around an unwritten MRT slot did not keep their attachments");
    Require(state.blends[1].colorWriteMask == 0 && !state.blends[1].blendEnable && state.blends[2].blendEnable && state.blends[2].colorWriteMask == 0xfu && !state.blends[0].blendEnable, "the unwritten MRT slot was not an unused attachment");
    Require(AgcDriver::Graphics::ExportMappings(state)[2] == state.colors[1].componentMapping && AgcDriver::Graphics::ExportMappings(state)[1] == 0xe4u, "export 2 did not take MRT slot 2's component mapping");
    queue.context[0x8e] = 0xf00u;
    state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.hasColorTarget && state.colors.size() == 1 && state.blends.size() == 3 && state.colors[0].exportIndex == 2 && state.color.address == slotTwo && state.blend.blendEnable && state.renderExtent.width == 64, "a draw writing MRT slot 2 alone did not leave slots 0 and 1 unused");
    Require(state.blends[0].colorWriteMask == 0 && state.blends[1].colorWriteMask == 0, "unwritten MRT slots below the written one have color writes");
    queue = makeState();
    alignas(256) static std::array<std::byte, 1024> slotFourMemory{};
    const auto slotFour = reinterpret_cast<std::uintptr_t>(slotFourMemory.data());
    for (const auto offset : {0x31bu, 0x31cu, 0x31du}) queue.context[offset + 4u * 0xfu] = queue.context.at(offset);
    for (const auto offset : {0x3b0u, 0x3b8u}) queue.context[offset + 4u] = queue.context.at(offset);
    queue.context[0x318 + 4u * 0xfu] = static_cast<std::uint32_t>(slotFour >> 8u);
    queue.context[0x390 + 4u] = static_cast<std::uint32_t>(slotFour >> 40u);
    queue.context[0x1e4] = 0x40010001u;
    for (std::uint32_t i = 0; i < 4; ++i) queue.context[0x105 + i] = 0;
    queue.context[0x8e] = 0x3000fu;
    queue.context[0x8f] = 0xf000fu;
    queue.context[0x1c5] = 0x99u;
    state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.colors.size() == 2 && state.colors[0].address == reinterpret_cast<std::uintptr_t>(colorMemory.data()) && state.colors[1].address == slotFour, "the second export did not reach MRT slot 4");
    Require(!state.blends[0].blendEnable && state.blends[1].blendEnable && state.blends[1].colorWriteMask == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT), "MRT slot 4 did not take its own blend control and target mask");
    Require(AgcDriver::Graphics::ExportMappings(state)[1] == state.colors[1].componentMapping, "export 1 did not take MRT slot 4's component mapping");
    queue.context[0x8e] = 0x30000u;
    state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.colors.size() == 1 && state.blends.size() == 2 && state.colors[0].exportIndex == 1 && state.colors[0].address == slotFour && state.blends[0].colorWriteMask == 0 && state.blends[1].blendEnable, "export 1 alone did not reach MRT slot 4 through attachment 1");
    // Without a depth surface (DB_Z_INFO / DB_STENCIL_INFO absent: FORMAT INVALID) the DB passes
    // every test: no attachment (see depthTests for surfaces).
    queue = makeState();
    queue.context[0x1b3] = 2;
    queue.context[0x1b4] = 2;
    for (const auto control : {2u, 0x007007b4u, 0x007007b6u}) {
        queue.context[0x200] = control;
        Require(!AgcDriver::Graphics::DecodeState(queue).depth.attached, "a depth test without a depth surface needed a depth target");
        Require(AgcDriver::Graphics::DrawRejection(queue, false).empty(), "a depth test without a depth surface was rejected");
    }
    queue.context[0x200] = 8;
    Require(!AgcDriver::Graphics::DecodeState(queue).depth.attached && AgcDriver::Graphics::DrawRejection(queue, false).empty(), "depth bounds without a depth surface needed a depth target");
    for (const auto control : {0x40000000u, 0x80000000u}) {
        queue.context[0x200] = control;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "depth-conditional color writes");
        Require(AgcDriver::Graphics::DrawRejection(queue, false).find("depth-conditional color writes") != std::string::npos, "the precheck accepted depth-conditional color writes");
    }
    queue = makeState();
    queue.context[0x10f] = 0x7fc00000;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "non-finite");
    // The register facade: every register the decoders read is in DrawKeyRegisters (a wrong hit of
    // the draw key otherwise), and nothing is recorded without a log pointer.
    queue = makeState();
    std::vector<AgcDriver::Graphics::RegisterRead> log;
    AgcDriver::Graphics::RegisterReadLog() = &log;
    queue.context[0x1b3] = 2;
    queue.context[0x1b4] = 2;
    state = AgcDriver::Graphics::DecodeState(queue);
    Require(AgcDriver::Graphics::DrawRejection(queue, true).empty(), "precheck rejected the reference state");
    static_cast<void>(AgcDriver::Graphics::DecodePixelStageInfo(queue.context, AgcDriver::Graphics::ExportMappings(state)));
    AgcDriver::Graphics::RegisterReadLog() = nullptr;
    Require(!log.empty(), "the register facade recorded nothing");
    for (const auto read : log) Require(AgcDriver::Graphics::DrawKeyCovers(read), "DrawKeyRegisters lacks a register the decoders read: " + std::string(AgcDriver::Graphics::RegisterBankName(read.bank)) + " " + std::to_string(read.offset));
    Require(!AgcDriver::Graphics::DrawKeyCovers({AgcDriver::Graphics::RegisterBank::Context, 0x100}) && AgcDriver::Graphics::DrawKeyCovers({AgcDriver::Graphics::RegisterBank::Shader, 0xab}) && !AgcDriver::Graphics::DrawKeyCovers({AgcDriver::Graphics::RegisterBank::Shader, 0xac}), "DrawKeyRegisters coverage changed");
    log.clear();
    static_cast<void>(AgcDriver::Graphics::DecodeState(queue));
    Require(log.empty(), "the register facade recorded without a log");
}

// A Z_32_FLOAT + STENCIL_8 surface as Astro Bot binds it (64x4 like makeState's color target):
// depth test and write with LESS_EQUAL, stencil off, HTILE enabled, a depth clear value of 1.
AgcDriver::QueueState makeDepthState() {
    auto queue = makeState();
    queue.context[0x1b3] = 2;
    queue.context[0x1b4] = 2;
    queue.context[0x0] = 0;
    queue.context[0x2] = 0;
    queue.context[0x5] = 0x51405;
    queue.context[0x7] = (3u << 16u) | 63u;
    queue.context[0xa] = 0;
    queue.context[0xb] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x10] = 0xa0000183u;
    queue.context[0x11] = 0x20000181u;
    queue.context[0x12] = queue.context[0x14] = 0x51356;
    queue.context[0x13] = queue.context[0x15] = 0x513dd;
    queue.context[0x10b] = 0;
    queue.context[0x10c] = queue.context[0x10d] = 0x01ffff00u;
    queue.context[0x200] = 0x007007b6u;
    return queue;
}

// DecodeState and the precheck reject the state with the same reason.
void expectDepthRejection(const AgcDriver::QueueState& queue, std::string_view reason) {
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, reason);
    const auto rejection = AgcDriver::Graphics::DrawRejection(queue, false);
    Require(rejection.find(reason) != std::string::npos, "the precheck disagrees with the decode: '" + rejection + "' for " + std::string(reason));
}

void hostImportBudgetTests() {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryHeapCount = 2;
    memory.memoryHeaps[0] = {24ull << 30u, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    memory.memoryHeaps[1] = {47ull << 30u, 0};
    Require(AgcDriver::Graphics::DefaultHostImportBudget(memory) == (45ull << 30u), "the host import budget is not the system memory heap less 2 GiB");
    memory.memoryHeaps[1].size = 8ull << 30u;
    Require(AgcDriver::Graphics::DefaultHostImportBudget(memory) == (6ull << 30u), "a small system memory heap lowered the host import budget below 6 GiB");
    memory.memoryHeapCount = 1;
    Require(AgcDriver::Graphics::DefaultHostImportBudget(memory) == (6ull << 30u), "a device without a system memory heap has no 6 GiB host import budget");
}

void hostImportRefusalTests() {
    using AgcDriver::Graphics::HostImportRefusal;
    using AgcDriver::Graphics::HostMapping;
    using AgcDriver::Graphics::ParseHostMapping;
    const auto parse = [](std::string_view line) {
        const auto mapping = ParseHostMapping(line);
        Require(mapping.has_value(), "a /proc/self/maps line did not parse: " + std::string(line));
        return *mapping;
    };
    const auto image = parse("55c30f4c8000-55c3169b6000 r--p 00004000 103:02 9437219                   /home/dean/ps5/astrobot-gate/eboot.linux");
    Require(image.begin == 0x55c30f4c8000ull && image.end == 0x55c3169b6000ull && image.readable && !image.writable && !image.shared && image.fileBacked, "an executable image line parsed wrong");
    const auto data = parse("55c31849c000-55c31d79b000 rw-p 08fd8000 103:02 9437219                   /home/dean/ps5/astrobot-gate/eboot.linux");
    Require(data.readable && data.writable && !data.shared && data.fileBacked, "a private writable file line parsed wrong");
    const auto direct = parse("500000000-587400000 rw-s 00000000 00:01 2049                       /memfd:direct memory (deleted)");
    Require(direct.begin == 0x500000000ull && direct.end == 0x587400000ull && direct.writable && direct.shared && direct.fileBacked, "a shared memfd line parsed wrong");
    const auto heap = parse("55c349800000-55c34b000000 rw-p 00000000 00:00 0                          [heap]");
    Require(heap.writable && !heap.shared && !heap.fileBacked, "the brk heap parsed as file-backed");
    const auto anonymous = parse("7fab9c1cc000-7fab9c34c000 rw-p 00000000 00:00 0 ");
    Require(anonymous.writable && !anonymous.fileBacked, "an anonymous mapping parsed wrong");
    Require(!parse("7fab9c1cc000-7fab9c34c000 rw-p 00000000 00:00 0\n").fileBacked, "an anonymous mapping read with its newline parsed as file-backed");
    const auto none = parse("7fab9c34c000-7fab9c35c000 ---p 00000000 00:00 0");
    Require(!none.readable && !none.writable, "an inaccessible mapping parsed as accessible");
    Require(!ParseHostMapping("").has_value() && !ParseHostMapping("not a mapping").has_value() && !ParseHostMapping("2000-1000 rw-p 00000000 00:00 0").has_value(), "a malformed maps line parsed");

    const std::vector<HostMapping> mappings{
        {0x10000, 0x20000, true, true, false, false},
        {0x20000, 0x30000, true, true, true, true},
        {0x30000, 0x31000, true, false, false, false},
        {0x40000, 0x50000, true, true, false, true},
        {0x50000, 0x51000, false, false, false, false},
    };
    Require(HostImportRefusal(mappings, 0x10000, 0x20000) == nullptr, "anonymous writable memory was refused");
    Require(HostImportRefusal(mappings, 0x18000, 0x28000) == nullptr, "anonymous memory followed by shared memory was refused");
    Require(HostImportRefusal(mappings, 0x20000, 0x30000) == nullptr, "shared memfd memory was refused");
    Require(HostImportRefusal(mappings, 0x2f000, 0x31000) != nullptr, "a range ending in a read-only page was offered");
    Require(HostImportRefusal(mappings, 0x30000, 0x31000) != nullptr, "a read-only page was offered");
    Require(HostImportRefusal(mappings, 0x31000, 0x32000) != nullptr, "an unmapped page was offered");
    Require(HostImportRefusal(mappings, 0x2f000, 0x32000) != nullptr, "a range running past its mappings was offered");
    Require(HostImportRefusal(mappings, 0x40000, 0x41000) != nullptr, "a small private file-backed range was offered");
    Require(HostImportRefusal(mappings, 0x40000, 0x50000) != nullptr, "a 64 KiB private file-backed range was offered");
    const std::vector<HostMapping> large{{0x100000, 0x200000, true, true, false, true}};
    Require(HostImportRefusal(large, 0x100000, 0x111000) == nullptr, "a private file-backed range past 64 KiB, which the driver imports, was refused");
    Require(HostImportRefusal(mappings, 0x50000, 0x51000) != nullptr, "an inaccessible page was offered");
    Require(HostImportRefusal(mappings, 0x8000, 0x11000) != nullptr, "a range starting before its mappings was offered");
    Require(HostImportRefusal({}, 0x10000, 0x11000) != nullptr, "a range without mappings was offered");
    Require(HostImportRefusal(mappings, 0x10000, 0x10000) != nullptr, "an empty range was offered");
}

void depthSurfaceTexelTests() {
    using AgcDriver::Graphics::DepthTexelBytes;
    Require(DepthTexelBytes(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_R32_SFLOAT) == 4 && DepthTexelBytes(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_D32_SFLOAT) == 4, "32-bit float depth is not sampled as its texels");
    Require(DepthTexelBytes(VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_R16_UNORM) == 2 && DepthTexelBytes(VK_FORMAT_D16_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_D16_UNORM) == 2, "16-bit depth is not sampled as its texels");
    Require(DepthTexelBytes(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_R8_UINT) == 1 && DepthTexelBytes(VK_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_R8_UNORM) == 1, "the stencil is not sampled as 8-bit texels");
    Require(DepthTexelBytes(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_R16_UNORM) == 0 && DepthTexelBytes(VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_R32_SFLOAT) == 0, "depth was sampled as texels of another size");
    Require(DepthTexelBytes(VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_R32_SFLOAT) == 0, "24-bit depth was sampled as 32-bit floats");
    Require(DepthTexelBytes(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_R8_UINT) == 0, "a depth format without stencil gave stencil texels");
    using AgcDriver::Graphics::WritesDepthImage;
    AgcDriver::Graphics::DepthState state{};
    state.depthWrite = true;
    Require(!WritesDepthImage(state), "a draw without a depth attachment writes one");
    state.attached = true;
    Require(WritesDepthImage(state), "a depth write does not write the depth image");
    state.depthWrite = false;
    state.depthTest = true;
    Require(!WritesDepthImage(state), "a depth test writes the depth image");
    state.clearStencil = true;
    Require(WritesDepthImage(state), "a stencil clear does not write the depth image");
    state.clearStencil = false;
    state.stencilTest = true;
    state.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
    state.back = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
    Require(WritesDepthImage(state), "a stencil replace does not write the depth image");
    state.front.writeMask = 0;
    Require(!WritesDepthImage(state), "a stencil operation under a zero write mask writes the depth image");
}

void depthTests() {
    using namespace AgcDriver::Graphics;
    auto queue = makeDepthState();
    auto state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty(), "the precheck rejected a depth-tested draw");
    Require(state.depth.attached && state.depth.depthTest && state.depth.depthWrite && state.depth.depthCompare == VK_COMPARE_OP_LESS_OR_EQUAL && !state.depth.stencilTest, "Z_ENABLE, Z_WRITE_ENABLE and ZFUNC were not decoded");
    Require(state.depthTarget.address == 0x5135600u && state.depthTarget.stencilAddress == 0x513dd00u && state.depthTarget.htileAddress == 0x5140500u, "depth surface addresses changed");
    Require(state.depthTarget.extent.width == 64 && state.depthTarget.extent.height == 4 && state.depthTarget.zFormat == 3 && state.depthTarget.stencil && state.depthTarget.slice == 0, "depth surface layout changed");
    Require(state.depth.depthClearValue == 1.0f && state.depth.stencilClearValue == 0 && !state.depth.clearDepth && !state.depth.clearStencil && !state.depth.depthBias, "depth clear values changed");
    Require(state.viewport.minDepth == 0 && state.viewport.maxDepth == 1, "a depth-tested draw changed the viewport depth range");
    // The framebuffer covers what every attachment covers.
    queue.context[0x7] = (1u << 16u) | 31u;
    state = DecodeState(queue);
    Require(state.renderExtent.width == 32 && state.renderExtent.height == 2, "a smaller depth surface did not bound the framebuffer");
    queue = makeDepthState();
    // ZFUNC shares VkCompareOp's order; an always-passing test that writes nothing needs no target.
    for (std::uint32_t func = 0; func < 8; ++func) {
        queue.context[0x200] = (0x007007b6u & ~0x70u) | (func << 4u);
        state = DecodeState(queue);
        Require(state.depth.attached && state.depth.depthCompare == static_cast<VkCompareOp>(func), "ZFUNC was not decoded");
        queue.context[0x200] &= ~4u;
        Require(DecodeState(queue).depth.attached == (func != 7), "a depth test that cannot fail or write needed a target");
        Require(DrawRejection(queue, false).empty(), "the precheck rejected a depth function");
    }
    // Z_ENABLE off leaves Z_WRITE_ENABLE without effect; DB_DEPTH_VIEW.Z_READ_ONLY turns writes off.
    queue = makeDepthState();
    queue.context[0x200] = 0x007007b4u;
    Require(!DecodeState(queue).depth.attached, "a depth write without the depth test needed a target");
    queue = makeDepthState();
    queue.context[0x2] = 1u << 24u;
    state = DecodeState(queue);
    Require(state.depth.attached && state.depth.depthTest && !state.depth.depthWrite, "Z_READ_ONLY did not disable depth writes");
    // A surface whose format is INVALID has no tests (the DB passes everything).
    queue = makeDepthState();
    queue.context[0x10] = 0x80000180u;
    queue.context[0x11] = 0x20000180u;
    Require(!DecodeState(queue).depth.attached && DrawRejection(queue, false).empty(), "a depth test without a depth surface needed a target");
    // Z_16 without stencil, and one layer of an array surface (a shadow cascade).
    queue = makeDepthState();
    queue.context[0x10] = 0x80000181u;
    queue.context[0x11] = 0;
    queue.context[0x2] = (2u << 13u) | 2u;
    state = DecodeState(queue);
    Require(state.depth.attached && state.depthTarget.zFormat == 1 && !state.depthTarget.stencil && state.depthTarget.stencilAddress == 0 && state.depthTarget.slice == 2, "a Z_16 array layer was not decoded");
    // Unsupported surfaces and modes, rejected alike by the decode and the precheck.
    const std::array<std::tuple<std::uint32_t, std::uint32_t, const char*>, 9> rejected{{
        {0x10, 0xa0000187u, "multisampled depth"},
        {0x10, 0xa0000182u, "Z_24"},
        {0x14, 0x51357u, "separate depth read and write"},
        {0x15, 0x513deu, "separate stencil read and write"},
        {0x2, (3u << 13u) | 1u, "layered depth rendering"},
        {0x2, 1u << 26u, "depth mip"},
        {0x0, 1u << 12u, "decompress"},
        {0x200, 0x407007b6u, "depth-conditional color writes"},
        {0x200, 0x807007b6u, "depth-conditional color writes"},
    }};
    for (const auto& [offset, value, reason] : rejected) {
        queue = makeDepthState();
        queue.context[offset] = value;
        expectDepthRejection(queue, reason);
    }
    // Stencil: an EQUAL front test replacing on pass, a separate ALWAYS back face that increments.
    queue = makeDepthState();
    queue.context[0x200] = (0x007007b6u & ~0x700u) | 1u | (2u << 8u);
    queue.context[0x10b] = (3u << 4u) | (5u << 16u);
    queue.context[0x10c] = 0x01ff0f05u;
    queue.context[0x10d] = 0x01ffff10u;
    state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty(), "the precheck rejected a stencil test");
    const auto& front = state.depth.front;
    const auto& back = state.depth.back;
    Require(state.depth.stencilTest && front.compareOp == VK_COMPARE_OP_EQUAL && front.passOp == VK_STENCIL_OP_REPLACE && front.failOp == VK_STENCIL_OP_KEEP && front.depthFailOp == VK_STENCIL_OP_KEEP && front.reference == 5 && front.compareMask == 0x0f && front.writeMask == 0xff, "the front stencil state was not decoded");
    Require(back.compareOp == VK_COMPARE_OP_ALWAYS && back.passOp == VK_STENCIL_OP_INCREMENT_AND_CLAMP && back.reference == 0x10 && back.compareMask == 0xff, "the back stencil state was not decoded");
    // Without BACKFACE_ENABLE the back face uses the front state.
    queue.context[0x200] &= ~0x80u;
    state = DecodeState(queue);
    Require(state.depth.back.compareOp == VK_COMPARE_OP_EQUAL && state.depth.back.passOp == VK_STENCIL_OP_REPLACE && state.depth.back.reference == 5, "the front stencil state did not cover back faces");
    // Increments by more than one, and bitwise operations, have no Vulkan form.
    queue.context[0x10b] = 5u << 4u;
    queue.context[0x10c] = 0x02ff0f05u;
    expectDepthRejection(queue, "stencil operations");
    queue.context[0x10b] = 10u << 4u;
    expectDepthRejection(queue, "stencil operations");
    // ...unless the test always passes: the writes are dropped (as before depth targets existed).
    queue.context[0x200] = 0x007007b6u | 1u;
    state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty() && state.depth.attached && !state.depth.stencilTest, "inexpressible stencil writes of an always-passing test were not dropped");
    // A stencil-only surface.
    queue = makeDepthState();
    queue.context[0x10] = 0;
    queue.context[0x200] = 1u | (4u << 8u);
    state = DecodeState(queue);
    Require(state.depth.attached && state.depth.stencilTest && !state.depth.depthTest && state.depthTarget.zFormat == 0 && state.depthTarget.address == 0x513dd00u && state.depthTarget.htileAddress == 0, "a stencil-only surface was not decoded");
    // Clears (DB_RENDER_CONTROL): an always-passing test writing DB_DEPTH_CLEAR through a pinned
    // viewport depth range, DB_STENCIL_CLEAR through REPLACE.
    queue = makeDepthState();
    queue.context[0x0] = 3;
    queue.context[0x200] = 0x00700700u;
    queue.context[0xb] = std::bit_cast<std::uint32_t>(0.25f);
    queue.context[0xa] = 0x5a;
    state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty(), "the precheck rejected a clear");
    Require(state.depth.attached && state.depth.clearDepth && state.depth.clearStencil && state.depth.depthTest && state.depth.depthWrite && state.depth.depthCompare == VK_COMPARE_OP_ALWAYS, "a depth clear did not write every covered pixel");
    Require(state.viewport.minDepth == 0.25f && state.viewport.maxDepth == 0.25f, "a depth clear did not pin the depth range to the clear value");
    Require(state.depth.stencilTest && state.depth.front.passOp == VK_STENCIL_OP_REPLACE && state.depth.front.compareOp == VK_COMPARE_OP_ALWAYS && state.depth.front.reference == 0x5a && state.depth.front.writeMask == 0xff && state.depth.back.reference == 0x5a, "a stencil clear did not replace with the clear value");
    // Depth bias: radeonsi's scale * 16 and units (* 4 for Z_16) back to Vulkan's factors.
    queue = makeDepthState();
    queue.context[0x205] = 0x240u | 0x1800u;
    queue.context[0x2de] = 0x1e9;
    queue.context[0x2df] = 0;
    queue.context[0x2e0] = queue.context[0x2e2] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x2e1] = queue.context[0x2e3] = std::bit_cast<std::uint32_t>(3.0f);
    state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty() && state.depth.depthBias && state.depth.depthBiasSlope == 2.0f && state.depth.depthBiasConstant == 3.0f, "depth bias was not decoded");
    queue.context[0x10] = 0x80000181u;
    Require(DecodeState(queue).depth.depthBiasConstant == 0.75f, "Z_16 depth bias units were not scaled");
    queue.context[0x10] = 0xa0000183u;
    queue.context[0x2e3] = std::bit_cast<std::uint32_t>(4.0f);
    expectDepthRejection(queue, "different front and back depth bias");
    queue.context[0x205] = 0x240u | 0x800u;
    expectDepthRejection(queue, "one face only");
    queue.context[0x205] = 0x240u | 0x800u | 2u;
    Require(DecodeState(queue).depth.depthBias, "depth bias of the only rasterized face was lost");
    queue = makeDepthState();
    queue.context[0x200] = 0x007007beu;
    queue.context[0x8] = std::bit_cast<std::uint32_t>(0.25f);
    queue.context[0x9] = std::bit_cast<std::uint32_t>(0.75f);
    state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty() && state.depth.attached && state.depth.depthBounds && state.depth.depthBoundsMin == 0.25f && state.depth.depthBoundsMax == 0.75f && state.depth.depthTest && state.depth.depthWrite, "depth bounds were not decoded");
    queue.context[0x200] = 8u;
    state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty() && state.depth.attached && state.depth.depthBounds && !state.depth.depthTest && !state.depth.depthWrite && !state.depth.stencilTest, "a depth bounds test alone did not attach the depth surface");
    queue.context[0x9] = std::bit_cast<std::uint32_t>(0.125f);
    Require(DecodeState(queue).depth.depthBoundsMax == 0.125f, "inverted depth bounds were not kept");
    queue.context[0x200] = 0x007007b6u;
    Require(!DecodeState(queue).depth.depthBounds, "depth bounds were decoded without DEPTH_BOUNDS_ENABLE");
    queue.context.erase(0x9);
    Require(!DecodeState(queue).depth.depthBounds, "a disabled depth bounds test read DB_DEPTH_BOUNDS_MAX");
    queue.context[0x200] = 0x007007beu;
    expectFailure([&] { DecodeState(queue); }, "missing register in context bank at DWORD 0x9");
    Require(DrawRejection(queue, false).empty(), "the precheck gave a verdict without DB_DEPTH_BOUNDS_MAX");
    queue.context[0x9] = 0x7f800000u;
    expectDepthRejection(queue, "non-finite depth bounds");
    queue.context[0x9] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x0] = 1u;
    expectDepthRejection(queue, "depth bounds on a depth or stencil clear");
    queue = makeDepthState();
    queue.context[0x10] = 0;
    queue.context[0x200] = 8u | 1u | (4u << 8u);
    queue.context[0x8] = 0;
    queue.context[0x9] = std::bit_cast<std::uint32_t>(1.0f);
    expectDepthRejection(queue, "depth bounds without a depth surface");
    // The register facade over a depth state: every register the depth rules read is in the key.
    queue = makeDepthState();
    queue.context[0x200] = (0x007007b6u & ~0x700u) | 1u | 8u | (2u << 8u);
    queue.context[0x8] = 0;
    queue.context[0x9] = std::bit_cast<std::uint32_t>(0.5f);
    queue.context[0x205] = 0x240u | 0x1800u;
    queue.context[0x2de] = 0x1e9;
    queue.context[0x2df] = 0;
    queue.context[0x2e0] = queue.context[0x2e2] = queue.context[0x2e1] = queue.context[0x2e3] = 0;
    queue.context[0x1a] = queue.context[0x1b] = queue.context[0x1c] = queue.context[0x1d] = queue.context[0x1e] = 0;
    std::vector<RegisterRead> log;
    RegisterReadLog() = &log;
    state = DecodeState(queue);
    Require(DrawRejection(queue, true).empty(), "precheck rejected the depth reference state");
    RegisterReadLog() = nullptr;
    for (const auto read : log) Require(DrawKeyCovers(read), "DrawKeyRegisters lacks a register the depth decode reads: " + std::string(RegisterBankName(read.bank)) + " " + std::to_string(read.offset));
    for (const auto offset : {0x0u, 0x2u, 0x5u, 0x7u, 0x8u, 0x9u, 0xau, 0xbu, 0x10u, 0x11u, 0x12u, 0x15u, 0x1au, 0x1eu, 0x10bu, 0x10du, 0x2e3u}) {
        Require(std::any_of(log.begin(), log.end(), [&](const RegisterRead& read) { return read.bank == RegisterBank::Context && read.offset == offset; }), "the depth decode did not read context register " + std::to_string(offset));
    }
}

void hardwareScreenOffsetTests() {
    auto queue = makeState();
    queue.context[0x90] = 0x80010003;
    queue.context[0x91] = 0x30020;
    const auto reference = AgcDriver::Graphics::DecodeState(queue);
    for (const auto offset : {0u, 1u, 0x10000u, 0x0020003cu, 0x01ff0000u, 0x000001ffu, 0x01ff01ffu}) {
        queue.context[0x8d] = offset;
        const auto state = AgcDriver::Graphics::DecodeState(queue);
        Require(state.viewport.x == reference.viewport.x && state.viewport.y == reference.viewport.y && state.viewport.width == reference.viewport.width && state.viewport.height == reference.viewport.height && state.viewport.minDepth == reference.viewport.minDepth && state.viewport.maxDepth == reference.viewport.maxDepth, "hardware guard-band offset changed the viewport");
        Require(state.scissor.offset.x == reference.scissor.offset.x && state.scissor.offset.y == reference.scissor.offset.y && state.scissor.extent.width == reference.scissor.extent.width && state.scissor.extent.height == reference.scissor.extent.height, "hardware guard-band offset changed the scissor");
        Require(state.renderExtent.width == reference.renderExtent.width && state.renderExtent.height == reference.renderExtent.height, "hardware guard-band offset changed the framebuffer extent");
    }
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        if (bit < 9 || (bit >= 16 && bit < 25)) continue;
        queue.context[0x8d] = 1u << bit;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "reserved PA_SU_HARDWARE_SCREEN_OFFSET bits");
    }
    queue.context.erase(0x8d);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
}

void ShaderStageTests() {
    auto queue = makeState();
    for (const auto routing : {0x2000u, 0x2010u}) {
        for (const auto vertexWave32 : {false, true}) {
            for (const auto fragmentWave32 : {false, true}) {
                queue.context[0x2d5] = routing | (vertexWave32 ? 0x00400000u : 0u);
                queue.context[0x1b6] = fragmentWave32 ? 0x8000u : 0u;
                const auto state = AgcDriver::Graphics::DecodeState(queue);
                Require(state.stages.path == AgcDriver::Graphics::ShaderPath::Vertex, "vertex routing changed");
                Require(state.stages.vertexWaveSize == (vertexWave32 ? 32u : 64u), "incorrect vertex wave size");
                Require(state.stages.fragmentWaveSize == (fragmentWave32 ? 32u : 64u), "incorrect fragment wave size");
            }
        }
    }
    queue = makeState();
    queue.context[0x2d5] = 0x2020;
    queue.userConfig[0x25b] = (64u << 9u) | 21u;
    queue.context[0x1ff] = 64;
    queue.context[0x2ce] = 3;
    queue.context[0x29b] = 2;
    queue.context[0x2ab] = 4;
    queue.shader[0x8a] = 3u << 29u;
    queue.shader[0x8b] = 3u << 16u;
    auto stages = AgcDriver::Graphics::DecodeState(queue).stages;
    Require(stages.path == AgcDriver::Graphics::ShaderPath::Geometry && stages.mesh && stages.mesh->primitivesPerGroup == 21 && stages.mesh->verticesPerGroup == 63, "geometry assembly changed");
    Require(stages.mesh->maxVertices == 64 && stages.mesh->maxPrimitives == 21 && stages.mesh->threadsPerGroup == 64 && stages.mesh->esgsItemSize == 4, "geometry subgroup outputs changed");
    for (const auto routing : {0x02002000u, 0x02002010u, 0x02402000u, 0x02402010u}) {
        queue.context[0x2d5] = routing;
        queue.context[0x2ce] = 0;
        queue.context[0x29b] = 0;
        const auto pass = AgcDriver::Graphics::DecodeState(queue).stages;
        Require(pass.path == AgcDriver::Graphics::ShaderPath::Geometry && pass.mesh && pass.mesh->passthrough && pass.mesh->verticesPerGroup == 63 && pass.mesh->primitivesPerGroup == 21, "passthrough subgroup assembly changed");
        Require(pass.vertexWaveSize == ((routing & 0x00400000u) ? 32u : 64u), "passthrough wave size changed");
    }
    {
        auto fan = makeState();
        fan.userConfig[0x242] = 5;
        fan.userConfig[0x24b] = 1;
        fan.context[0x103] = 0xffffffffu;
        fan.context[0x2d5] = 0x2030;
        fan.userConfig[0x25b] = 0x4020;
        fan.context[0x1ff] = 256;
        fan.context[0x2ce] = 8;
        fan.context[0x29b] = 2;
        fan.context[0x2ab] = 4;
        fan.shader[0x8a] = 3u << 29u;
        fan.shader[0x8b] = 3u << 16u;
        fan.context[0x1b3] = 2;
        fan.context[0x1b4] = 2;
        const auto state = AgcDriver::Graphics::DecodeState(fan);
        Require(AgcDriver::Graphics::DrawRejection(fan, true).empty(), "the precheck rejected an indexed triangle fan with restart into a geometry shader");
        Require(state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN && state.primitiveRestart && state.stages.path == AgcDriver::Graphics::ShaderPath::Geometry && state.stages.mesh && !state.stages.mesh->passthrough, "a triangle fan did not decode as geometry input");
        const auto& mesh = *state.stages.mesh;
        Require(mesh.inputPrimitive == 5 && mesh.primitivesPerGroup == 30 && mesh.verticesPerGroup == 32 && mesh.maxVertices == 256 && mesh.maxPrimitives == 192 && mesh.threadsPerGroup == 256 && mesh.esgsItemSize == 4, "triangle fan subgroup assembly changed");
        fan.userConfig[0x25b] = (3u << 9u) | 3u;
        Require(AgcDriver::Graphics::DecodeState(fan).stages.mesh->primitivesPerGroup == 1, "a three-vertex subgroup did not take one fan triangle");
        fan.userConfig[0x25b] = (2u << 9u) | 3u;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(fan); }, "invalid geometry subgroup");
        fan.userConfig[0x25b] = 0x4020;
        for (const auto routing : {0x02002000u, 0x02402010u}) {
            fan.context[0x2d5] = routing;
            expectFailure([&] { AgcDriver::Graphics::DecodeState(fan); }, "unsupported geometry input or output assembly");
        }
        fan.context[0x2d5] = 0x2030;
        fan.userConfig[0x242] = 3;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(fan); }, "unsupported geometry input or output assembly");
    }
    queue.context[0x2d5] = 0x2020;
    queue.context[0x2ce] = 3;
    queue.context[0x29b] = 2;
    queue.context[0x2ab] = 0;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "invalid VGT_ESGS_RING_ITEMSIZE");
    queue.context[0x2ab] = 4;
    queue.userConfig[0x25b] = 0;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "invalid geometry subgroup");
    queue = makeState();
    queue.context[0x2d5] = 0x200d;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "Patch topology and HS_EN disagree");
    queue.userConfig[0x242] = 9;
    queue.context[0x2d6] = (3u << 8u) | (3u << 14u);
    queue.context[0x2db] = 1u | (2u << 2u) | (2u << 5u);
    stages = AgcDriver::Graphics::DecodeState(queue).stages;
    Require(stages.path == AgcDriver::Graphics::ShaderPath::Tessellation && stages.tessellation && stages.tessellation->inputControlPoints == 3, "tessellation routing changed");
    queue.context[0x2d5] = 0x202d;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "combined tessellation and geometry");
    queue.context[0x2d5] = 0x200d;
    queue.context[0x2d6] = 0;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "control-point counts");
    queue = makeState();
    for (const auto value : {0x2003u, 0x2018u, 0x20c0u, 0x80002000u}) {
        queue.context[0x2d5] = value;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "reserved");
    }
    for (const auto bit : {1u, 8u, 0x40u, 0x100u, 0x200u, 0x400u, 0x1000u, 0x4000u, 0x8000u, 0x80000u, 0x200000u, 0x800000u, 0x1000000u}) {
        queue.context[0x2d5] = 0x2000u | bit;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "unsupported vertex");
    }
    queue.context[0x2d5] = 0;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "without PRIMGEN_EN");
    queue.context.erase(0x2d5);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
    queue.context[0x2d5] = 0x2000;
    queue.context.erase(0x1b6);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
}

constexpr std::array<std::uint8_t, 8> IdentityExports{0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u};

std::vector<spv::BuiltIn> pixelBuiltinsRead(std::uint32_t ena, std::uint32_t addr, std::uint32_t source) {
    auto queue = makeState();
    queue.context[0x1b3] = ena;
    queue.context[0x1b4] = addr;
    const auto pixel = AgcDriver::Graphics::DecodePixelStageInfo(queue.context, IdentityExports);
    const std::array<std::uint32_t, 3> code{0xf800180fu, source * 0x01010101u, 0xbf810000u};
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = true;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    const auto& words = result.spirv.Words();
    std::map<std::uint32_t, spv::BuiltIn> builtins;
    std::vector<spv::BuiltIn> read;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        const auto op = static_cast<spv::Op>(words[at] & 0xffffu);
        if (op == spv::OpDecorate && words[at + 2] == spv::DecorationBuiltIn) builtins[words[at + 1]] = static_cast<spv::BuiltIn>(words[at + 3]);
        if (op != spv::OpLoad && op != spv::OpAccessChain && op != spv::OpInBoundsAccessChain) continue;
        const auto found = builtins.find(words[at + 3]);
        if (found != builtins.end() && std::find(read.begin(), read.end(), found->second) == read.end()) read.push_back(found->second);
    }
    return read;
}

bool readsBuiltin(const std::vector<spv::BuiltIn>& read, spv::BuiltIn builtin) {
    return std::find(read.begin(), read.end(), builtin) != read.end();
}

std::vector<std::uint32_t> pixelNoPerspectiveLocations(bool barycentricEnabled) {
    auto queue = makeState();
    queue.context[0x1b3] = 0x22u;
    queue.context[0x1b4] = 0x22u;
    queue.context[0x1b6] = 2u;
    queue.context[0x191] = 0u;
    queue.context[0x192] = 1u;
    const auto pixel = AgcDriver::Graphics::DecodePixelStageInfo(queue.context, IdentityExports);
    const std::array<std::uint32_t, 8> code{0xc8100000u, 0xc8110001u, 0xc8140402u, 0xc8150403u, 0xf800180fu, 0x05040504u, 0xbf810000u, 0xbf810000u};
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = barycentricEnabled;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    const auto& words = result.spirv.Words();
    std::map<std::uint32_t, std::uint32_t> locations;
    std::vector<std::uint32_t> noPerspective;
    std::uint32_t perVertex = 0;
    bool perspectiveBarycentrics = false;
    bool linearBarycentrics = false;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        if (static_cast<spv::Op>(words[at] & 0xffffu) != spv::OpDecorate) continue;
        if (words[at + 2] == spv::DecorationLocation) locations[words[at + 1]] = words[at + 3];
        if (words[at + 2] == spv::DecorationNoPerspective) noPerspective.push_back(words[at + 1]);
        if (words[at + 2] == spv::DecorationPerVertexKHR) perVertex++;
        if (words[at + 2] == spv::DecorationBuiltIn) {
            perspectiveBarycentrics |= words[at + 3] == spv::BuiltInBaryCoordKHR;
            linearBarycentrics |= words[at + 3] == spv::BuiltInBaryCoordNoPerspKHR;
        }
    }
    if (barycentricEnabled) Require(perVertex == 2u && perspectiveBarycentrics && linearBarycentrics, "explicit interpolation must preserve both parameter vertices and both barycentric inputs");
    std::vector<std::uint32_t> result2;
    for (const auto id : noPerspective) result2.push_back(locations.count(id) != 0 ? locations.at(id) : 0xffffffffu);
    return result2;
}

void PixelInputLayoutTests() {
    using ShaderRecompiler::PixelInput;
    using ShaderRecompiler::PixelInputVgpr;
    auto queue = makeState();
    const auto decode = [&](std::uint32_t ena, std::uint32_t addr) {
        queue.context[0x1b3] = ena;
        queue.context[0x1b4] = addr;
        return AgcDriver::Graphics::DecodePixelStageInfo(queue.context, IdentityExports);
    };
    auto pixel = decode(0x326u, 0x326u);
    Require(pixel.inputAddr == 0x326u && pixel.hasPerspectiveCenterVgpr && pixel.perspectiveCentroid && pixel.noPerspective && !pixel.linearCentroid && pixel.posX && pixel.posY && !pixel.posZ, "the centroid input flags were not decoded");
    Require(PixelInputVgpr(pixel.inputAddr, PixelInput::PerspectiveCenter) == 0u && PixelInputVgpr(pixel.inputAddr, PixelInput::PerspectiveCentroid) == 2u && PixelInputVgpr(pixel.inputAddr, PixelInput::LinearCenter) == 4u && PixelInputVgpr(pixel.inputAddr, PixelInput::PositionX) == 6u && PixelInputVgpr(pixel.inputAddr, PixelInput::PositionY) == 7u, "the centroid layout moved the inputs");
    pixel = decode(0x1146u, 0x1146u);
    Require(pixel.linearCentroid && !pixel.noPerspective && PixelInputVgpr(pixel.inputAddr, PixelInput::LinearCentroid) == 4u && PixelInputVgpr(pixel.inputAddr, PixelInput::PositionX) == 6u && PixelInputVgpr(pixel.inputAddr, PixelInput::FrontFace) == 7u, "the linear centroid layout moved the inputs");
    pixel = decode(0x506u, 0x7afu);
    Require(pixel.inputAddr == 0x7afu && !pixel.posY && pixel.posZ && PixelInputVgpr(pixel.inputAddr, PixelInput::PerspectiveCentroid) == 4u && PixelInputVgpr(pixel.inputAddr, PixelInput::PositionX) == 12u && PixelInputVgpr(pixel.inputAddr, PixelInput::PositionZ) == 14u, "ADDR-only inputs did not reserve their VGPRs");
    for (const auto bit : {0x8u, 0x80u, 0x4000u, 0x8000u}) {
        expectFailure([&] { static_cast<void>(decode(0x2u | bit, 0x2u | bit)); }, "unsupported SPI_PS_INPUT_ENA/ADDR");
    }
    pixel = decode(0x546u, 0x7c7u);
    {
        ShaderRecompiler::RecompileRequest request{};
        const std::array<std::uint32_t, 1> code{0xbf810000u};
        request.shader = {ShaderRecompiler::ShaderStage::Fragment, 0x30000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.pixel = pixel;
        const ShaderRecompiler::RequestSerializer serializer;
        const auto back = serializer.Deserialize(serializer.Serialize(request));
        const auto& p = *back.request.context.pixel;
        Require(p.inputAddr == 0x7c7u && p.hasPerspectiveCenterVgpr && p.perspectiveCentroid && p.linearCentroid && !p.noPerspective && p.posX && !p.posY && p.posZ, "the pixel input layout did not survive serialization");
    }
    for (const auto source : {0u, 2u, 3u}) {
        const auto read = pixelBuiltinsRead(0x106u, 0x106u, source);
        Require(readsBuiltin(read, spv::BuiltInBaryCoordKHR) && !readsBuiltin(read, spv::BuiltInFragCoord), "a centroid-layout I/J VGPR does not hold the barycentrics: v" + std::to_string(source));
    }
    const auto noPerspective = pixelNoPerspectiveLocations(false);
    Require(noPerspective.size() == 1 && noPerspective[0] == 1u, "only the parameter interpolated through the linear pair must be NoPerspective");
    Require(pixelNoPerspectiveLocations(true).empty(), "explicit interpolation must not interpolate parameter arrays a second time");
    auto read = pixelBuiltinsRead(0x106u, 0x106u, 4u);
    Require(readsBuiltin(read, spv::BuiltInFragCoord) && !readsBuiltin(read, spv::BuiltInBaryCoordKHR), "POS_X is not in v4 after the center and centroid pairs");
    read = pixelBuiltinsRead(0x326u, 0x326u, 5u);
    Require(readsBuiltin(read, spv::BuiltInBaryCoordNoPerspKHR) && !readsBuiltin(read, spv::BuiltInFragCoord), "LINEAR_CENTER's J is not v5");
    read = pixelBuiltinsRead(0x326u, 0x326u, 6u);
    Require(readsBuiltin(read, spv::BuiltInFragCoord) && !readsBuiltin(read, spv::BuiltInBaryCoordKHR) && !readsBuiltin(read, spv::BuiltInBaryCoordNoPerspKHR), "POS_X is not in v6 after three I/J pairs");
    read = pixelBuiltinsRead(0x102u, 0x106u, 4u);
    Require(readsBuiltin(read, spv::BuiltInFragCoord), "an ADDR-only centroid pair did not reserve v2/v3");
    read = pixelBuiltinsRead(0x102u, 0x106u, 2u);
    Require(!readsBuiltin(read, spv::BuiltInFragCoord) && !readsBuiltin(read, spv::BuiltInBaryCoordKHR), "an ADDR-only centroid pair was loaded");
}

void DisabledColorTests() {
    auto queue = makeState();
    queue.context[0x8e] = 0;
    for (const auto offset : {0x31cu, 0x31bu, 0x31du, 0x3b0u, 0x3b8u, 0x390u, 0x318u, 0x1e0u}) queue.context.erase(offset);
    const auto state = AgcDriver::Graphics::DecodeState(queue);
    Require(!state.hasColorTarget && state.color.address == 0 && state.color.bytes == 0, "disabled color writes accessed a color surface");
    Require(state.renderExtent.width == 64 && state.renderExtent.height == 4, "attachment-free framebuffer lost screen scissor extent");
    queue.context[0xd] = 0;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "empty framebuffer extent");
    queue = makeState();
    queue.context[0x8e] = 3;
    const auto partial = AgcDriver::Graphics::DecodeState(queue);
    Require(partial.hasColorTarget && partial.blend.colorWriteMask == 3, "partial color write mask changed");
    queue.context.erase(0x31c);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
}

// DB metadata blits: DB_RENDER_CONTROL HTILE operations, no depth/stencil/color work, a skipped pixel
// shader, so SPI_PS_INPUT_ENA/ADDR may be absent.
void depthMetadataBlitTests() {
    using AgcDriver::Graphics::DepthMetadataBlit;
    auto queue = makeState();
    for (const auto [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0x0, 0x60}, {0x200, 0}, {0x202, 0xcc0000}, {0x203, 0}, {0x1c4, 0}, {0x1c5, 0}, {0x8e, 0}, {0x8f, 0}}) queue.context[offset] = value;
    queue.context.erase(0x1b3);
    queue.context.erase(0x1b4);
    Require(DepthMetadataBlit(queue), "an in-place HTILE decompress blit was not recognized");
    queue.context[0x0] = 0x10;
    Require(DepthMetadataBlit(queue), "an HTILE resummarize blit was not recognized");
    // Normal CB mode with no enabled channel the shader exports writes no color either.
    queue.context[0x202] = 0xcc0010;
    queue.context[0x8e] = 0xf;
    Require(DepthMetadataBlit(queue), "a blit with a target mask but no shader channels was refused");
    queue.context[0x8f] = 0xf;
    Require(!DepthMetadataBlit(queue), "a blit writing color was taken for a DB metadata blit");
    queue.context[0x8f] = 0;
    for (const auto [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0x0, 0}, {0x0, 0x61}, {0x0, 0x1060}, {0x200, 2}, {0x200, 1}, {0x203, 0x40}, {0x203, 0x400}, {0x203, 1}, {0x1c4, 1}, {0x1c5, 4}, {0x2dc, 0xaa01}}) {
        auto changed = queue;
        changed.context[offset] = value;
        Require(!DepthMetadataBlit(changed), "a draw with depth, stencil or pixel shader work was taken for a DB metadata blit");
    }
    auto absent = queue;
    absent.context.erase(0x203);
    Require(!DepthMetadataBlit(absent), "a draw missing DB_SHADER_CONTROL was taken for a DB metadata blit");
}

// MIMG words: the gfx10.3 BVH intersection opcode as Astro Bot's ray-query kernels issue it (NSA,
// R128 and UNRM set) decodes, and reserved control bits are refused with the words.
void mimgDecodeTests() {
    const std::array<std::uint32_t, 5> bvh{0xf1989f07u, 0x00040505u, 0, 0, 0};
    Require(ShaderRecompiler::DecodeRdnaMimg(0, bvh, 0).op == ShaderRecompiler::RdnaOpcode::ImageBvhIntersectRay, "Astro Bot's BVH intersection encoding was not decoded");
    // image_sample (0x20) with TFE (bit 16): a reserved control bit, named with the words.
    const std::array<std::uint32_t, 2> tfe{0xf0800f00u | (1u << 16u), 0x00000000u};
    expectFailure([&] { ShaderRecompiler::DecodeRdnaMimg(0, tfe, 0); }, "reserved MIMG control bits (words f0810f00 00000000)");
}

void ColorViewTests() {
    auto queue = makeState();
    queue.context[0x31b] = 1u << 26u;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "mip exceeds");
    queue.context[0x31b] = 1u << 13u;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "array views");
}

alignas(256) std::array<std::uint8_t, 4> dccKeys{};

void metadataPassTests() {
    using AgcDriver::Graphics::ColorMetadataPass;
    using AgcDriver::Graphics::DecodeColorMetadataPass;
    auto queue = makeState();
    queue.context[0x0] = 0;
    Require(!DecodeColorMetadataPass(queue).has_value(), "normal color rendering decoded as a metadata pass");
    queue.context[0x202] = 0xcc0020;
    queue.context[0x323] = 0x11223344;
    queue.context[0x324] = 0x55667788;
    auto pass = DecodeColorMetadataPass(queue);
    Require(pass.has_value() && pass->mode == ColorMetadataPass::Mode::EliminateFastClear && pass->targets.size() == 1, "fast-clear eliminate did not decode");
    Require(pass->targets[0].address == reinterpret_cast<std::uintptr_t>(colorMemory.data()) && pass->targets[0].extent.width == 64 && pass->targets[0].extent.height == 4, "fast-clear eliminate target changed");
    Require(pass->targets[0].clearWords[0] == 0x11223344 && pass->targets[0].clearWords[1] == 0x55667788, "fast-clear eliminate lost CB_COLOR_CLEAR_WORD");
    queue.context[0x1c5] = 2;
    queue.context[0x8f] = 1;
    Require(DecodeColorMetadataPass(queue).has_value(), "the blit's export format refused a metadata pass");
    queue.context[0x202] = 0xcc0060;
    pass = DecodeColorMetadataPass(queue);
    Require(pass.has_value() && pass->mode == ColorMetadataPass::Mode::DccDecompress, "DCC decompress did not decode");
    queue.context[0x8e] = 0;
    Require(DecodeColorMetadataPass(queue)->targets.empty(), "a disabled target joined the metadata pass");
    queue.context[0x8e] = 0xf;
    queue.context[0x202] = 0xcc0061;
    expectFailure([&] { DecodeColorMetadataPass(queue); }, "nonstandard ROP");
    queue.context[0x202] = 0x330060;
    expectFailure([&] { DecodeColorMetadataPass(queue); }, "nonstandard ROP");
    queue.context[0x202] = 0xcc0060;
    queue.context[0x200] = 2;
    expectFailure([&] { DecodeColorMetadataPass(queue); }, "depth or stencil work");
    queue.context[0x200] = 0x70;
    Require(DecodeColorMetadataPass(queue).has_value(), "an always-pass depth function without a test refused the pass");
    queue.context[0x91] = 0x40020;
    expectFailure([&] { DecodeColorMetadataPass(queue); }, "over part of a color target");
    queue.context[0x91] = 0x40040;
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(16.0f);
    expectFailure([&] { DecodeColorMetadataPass(queue); }, "over part of a color target");
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x202] = 0xcc0030;
    queue.context[0x1c5] = 9;
    queue.context[0x8f] = 0xf;
    Require(!DecodeColorMetadataPass(queue).has_value(), "resolve decoded as a metadata pass");
    Require(AgcDriver::Graphics::DrawRejection(queue, false).find("mode resolve") != std::string::npos, "the resolve rejection does not name the mode");

    queue = makeState();
    queue.context[0x0] = 0;
    queue.context[0x202] = 0xcc0020;
    queue.context[0x31c] |= 0x10000000;
    const auto keysAddress = reinterpret_cast<std::uintptr_t>(dccKeys.data());
    queue.context[0x325] = static_cast<std::uint32_t>(keysAddress >> 8u);
    queue.context[0x3a8] = static_cast<std::uint32_t>(keysAddress >> 40u);
    queue.context[0x323] = 0x11223344;
    queue.context[0x324] = 0;
    pass = DecodeColorMetadataPass(queue);
    Require(pass.has_value() && pass->targets.size() == 1 && pass->targets[0].dccAddress == keysAddress, "the metadata pass lost the target's DCC keys");
    auto mipmapped = queue;
    mipmapped.context[0x3b0] |= 1u << 28u;
    expectFailure([&] { DecodeColorMetadataPass(mipmapped); }, "mipmapped DCC");
    const AgcDriver::Graphics::Context context{};
    const auto texels = [&](std::uint32_t value) {
        for (std::size_t offset = 0; offset < colorMemory.size(); offset += 4) {
            std::uint32_t texel = 0;
            std::memcpy(&texel, colorMemory.data() + offset, 4);
            if (texel != value) return false;
        }
        return true;
    };
    const auto keysAre = [&](std::uint8_t key) { return std::all_of(dccKeys.begin(), dccKeys.end(), [&](std::uint8_t value) { return value == key; }); };
    std::memset(colorMemory.data(), 0x5a, colorMemory.size());
    dccKeys.fill(0x20);
    AgcDriver::Graphics::RunColorMetadataPass(context, *pass);
    Require(texels(0x11223344) && keysAre(0xff), "a register fast clear was not eliminated into the texels");
    std::memset(colorMemory.data(), 0x5a, colorMemory.size());
    dccKeys.fill(0xc0);
    AgcDriver::Graphics::RunColorMetadataPass(context, *pass);
    Require(texels(0xffffffffu) && keysAre(0xff), "a 1111 fast clear was not eliminated into the texels");
    std::memset(colorMemory.data(), 0x5a, colorMemory.size());
    AgcDriver::Graphics::RunColorMetadataPass(context, *pass);
    Require(texels(0x5a5a5a5au) && keysAre(0xff), "a pass over uncompressed keys changed the texels");
    dccKeys = {0x20, 0xff, 0x20, 0x20};
    expectFailure([&] { AgcDriver::Graphics::RunColorMetadataPass(context, *pass); }, "per-block metadata");
    Require(texels(0x5a5a5a5au), "a refused pass changed the texels");
    pass->targets[0].dccAddress = 0;
    AgcDriver::Graphics::RunColorMetadataPass(context, *pass);
    Require(texels(0x5a5a5a5au), "a pass over a target without DCC changed its texels");
}

void DepthClipTests() {
    auto queue = makeState();
    const auto direct = AgcDriver::Graphics::DecodeState(queue);
    Require(!direct.negativeOneToOne && direct.viewport.minDepth == 0 && direct.viewport.maxDepth == 1, "zero-to-one depth transform changed");
    queue.context[0x204] = 0;
    queue.context[0x113] = std::bit_cast<std::uint32_t>(0.5f);
    queue.context[0x114] = std::bit_cast<std::uint32_t>(0.5f);
    const auto symmetric = AgcDriver::Graphics::DecodeState(queue);
    Require(symmetric.negativeOneToOne && symmetric.viewport.minDepth == 0 && symmetric.viewport.maxDepth == 1, "negative-one-to-one depth transform is incorrect");
    queue.context[0x113] = std::bit_cast<std::uint32_t>(-0.5f);
    const auto reversed = AgcDriver::Graphics::DecodeState(queue);
    Require(reversed.viewport.minDepth == 1 && reversed.viewport.maxDepth == 0, "reversed depth transform is incorrect");
    queue.context[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    queue.context[0x114] = 0;
    const auto unrestricted = AgcDriver::Graphics::DecodeState(queue);
    Require(unrestricted.viewport.minDepth == -1 && unrestricted.viewport.maxDepth == 1, "unrestricted viewport depth was normalized");
    queue.context[0x113] = std::bit_cast<std::uint32_t>(-1.0f);
    const auto unrestrictedReversed = AgcDriver::Graphics::DecodeState(queue);
    Require(unrestrictedReversed.viewport.minDepth == 1 && unrestrictedReversed.viewport.maxDepth == -1, "reversed unrestricted viewport depth changed");
    queue.context[0x113] = 0x7f7fffff;
    queue.context[0x114] = 0x7f7fffff;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "unsupported viewport transform");
    queue.context[0x114] = std::bit_cast<std::uint32_t>(0.5f);
    queue.context[0x113] = std::bit_cast<std::uint32_t>(0.5f);
    queue.context[0xb4] = std::bit_cast<std::uint32_t>(2.0f);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "inverted viewport depth clamp");
    queue.context[0xb4] = 0;
    // ZCLIP_NEAR/FAR_DISABLE (bits 26, 27) become depth clamping.
    queue.context[0x204] = 0x0c080000u;
    Require(AgcDriver::Graphics::DecodeState(queue).depthClamp, "near/far clip disable did not clamp depth");
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        if (bit == 19 || bit == 26 || bit == 27) continue;
        queue.context[0x204] = 1u << bit;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "PA_CL_CLIP_CNTL");
    }
}

void InitialContextTests() {
    const auto configured = makeState();
    AgcDriver::QueueState queue;
    Require(queue.context.at(0x3) == 0 && queue.context.at(0x8) == 0 && queue.context.at(0x9) == 0x3f800000, "depth bounds overlap render override");
    Require(queue.context.at(0x2dc) == 0xaa00 && queue.context.at(0x313) == 0x6000 && queue.context.at(0x2f9) == 0x2d, "initial raster controls are incomplete");
    Require(queue.context.at(0x30e) == 0xffffffff && queue.context.at(0x30f) == 0xffffffff, "initial sample mask excludes samples");
    queue.userConfig[0x242] = 4;
    for (const auto offset : {0x2d5u, 0x204u, 0x8eu, 0x8fu, 0x1c3u, 0x1c5u, 0x31cu, 0x3b0u, 0x3b8u, 0x318u, 0x390u, 0x10fu, 0x110u, 0x111u, 0x112u, 0x113u, 0x114u, 0xb4u, 0xb5u}) queue.context.at(offset) = configured.context.at(offset);
    const auto state = AgcDriver::Graphics::DecodeState(queue);
    Require(state.color.address == reinterpret_cast<std::uintptr_t>(colorMemory.data()) && state.color.bytes == colorMemory.size(), "sparse guest setup lost its render target");
    Require(queue.context.at(0x206) == 0x43f, "initial homogeneous viewport mode changed");
    for (const auto control : {0x3fu, 0x43eu, 0x53fu, 0x63fu, 0x8000043fu}) {
        queue.context[0x206] = control;
        expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "PA_CL_VTE_CNTL=0x");
    }
    queue.context[0x206] = 0x43f;
    queue.context[0x2dc] |= 1;
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "alpha-to-coverage");
    queue.context.erase(0x2dc);
    expectFailure([&] { AgcDriver::Graphics::DecodeState(queue); }, "missing register");
    queue.ClearContext();
    Require(queue.context.at(0x2dc) == 0xaa00 && queue.context.at(0x318) == 0 && queue.context.at(0x8e) == 0, "clear did not restore controls and discard target state");
    Require(!queue.context.contains(0xdead), "unknown context register acquired a default");
}

struct MockDescriptorWrite {
    std::uint32_t binding;
    std::uint32_t count;
    VkDescriptorType type;
    std::vector<VkDescriptorBufferInfo> buffers;
};

struct MockVulkan {
    std::uint64_t next = 1;
    std::int64_t live = 0;
    std::map<VkBuffer, VkDeviceSize> bufferSizes;
    std::map<VkBuffer, VkBufferUsageFlags> bufferUsage;
    std::map<VkDeviceMemory, VkMemoryAllocateFlags> allocationFlags;
    std::map<VkBuffer, VkDeviceMemory> bufferMemory;
    std::map<VkBuffer, VkDeviceSize> bufferOffset;
    std::uint32_t allocations = 0;
    std::uint32_t frees = 0;
    std::map<VkDeviceMemory, std::vector<std::byte>> memories;
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    std::vector<VkDescriptorPoolSize> poolSizes;
    std::uint32_t poolMaxSets = 0;
    std::vector<MockDescriptorWrite> writes;
    std::uint32_t boundSets = 0;
    std::uint32_t boundFirst = 0;
    VkPipelineBindPoint boundPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
    std::map<VkPipelineLayout, VkDeviceSize> pipelineLayoutPushConstantSize;
    std::uint32_t pipelineCreateCount = 0;
    std::vector<std::array<std::uint32_t, 3>> pipelineSpecializations;
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    std::vector<VkAttachmentDescription> renderPassAttachments;
    std::optional<VkAttachmentReference> renderPassDepth;
    std::vector<VkAttachmentReference> renderPassColors;
    std::uint32_t blendAttachments = 0;
    std::vector<VkDynamicState> dynamicStates;
    std::optional<std::pair<float, float>> depthBounds;
    std::optional<VkPipelineDepthStencilStateCreateInfo> depthStencil;
    VkPipelineRasterizationStateCreateInfo raster{};
    std::vector<std::vector<std::uint32_t>> shaderModules;
    std::vector<std::byte> lastPushConstants;
    struct { std::uint32_t x = 0, y = 0, z = 0; } lastDispatchGroups;
};

MockVulkan mock;

template<typename THandle>
THandle makeHandle() {
    const auto value = mock.next++;
    if constexpr (std::is_pointer_v<THandle>) return reinterpret_cast<THandle>(static_cast<std::uintptr_t>(value));
    else return static_cast<THandle>(value);
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* buffer) {
    *buffer = makeHandle<VkBuffer>();
    mock.bufferSizes[*buffer] = info->size;
    mock.bufferUsage[*buffer] = info->usage;
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements) {
    *requirements = {mock.bufferSizes.at(buffer), 1, 1};
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    *memory = makeHandle<VkDeviceMemory>();
    mock.memories[*memory] = std::vector<std::byte>(info->allocationSize);
    if (info->pNext != nullptr) {
        const auto* flags = static_cast<const VkMemoryAllocateFlagsInfo*>(info->pNext);
        mock.allocationFlags[*memory] = flags->flags;
        Require(flags->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO && flags->flags == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, "invalid BDA allocation flags");
    }
    ++mock.live;
    ++mock.allocations;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    Require(offset + mock.bufferSizes.at(buffer) <= mock.memories.at(memory).size(), "mock buffer memory is bound past its allocation");
    mock.bufferMemory[buffer] = memory;
    mock.bufferOffset[buffer] = offset;
    return VK_SUCCESS;
}

std::span<std::byte> mockBufferMemory(VkBuffer buffer) {
    auto& memory = mock.memories.at(mock.bufferMemory.at(buffer));
    return std::span<std::byte>(memory).subspan(static_cast<std::size_t>(mock.bufferOffset.at(buffer)), static_cast<std::size_t>(mock.bufferSizes.at(buffer)));
}

VKAPI_ATTR VkResult VKAPI_CALL mockMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags, void** data) {
    Require(offset == 0, "mock memory must be mapped from offset zero");
    *data = mock.memories.at(memory).data();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockUnmapMemory(VkDevice, VkDeviceMemory) {}

VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {
    --mock.live;
    ++mock.frees;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* info, const VkAllocationCallbacks*, VkDescriptorSetLayout* layout) {
    *layout = makeHandle<VkDescriptorSetLayout>();
    mock.layoutBindings.assign(info->pBindings, info->pBindings + info->bindingCount);
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo* info, const VkAllocationCallbacks*, VkDescriptorPool* pool) {
    *pool = makeHandle<VkDescriptorPool>();
    mock.poolSizes.assign(info->pPoolSizes, info->pPoolSizes + info->poolSizeCount);
    mock.poolMaxSets = info->maxSets;
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
    Require(info->descriptorSetCount == 1, "exactly one descriptor set must be allocated");
    sets[0] = makeHandle<VkDescriptorSet>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockFreeDescriptorSets(VkDevice, VkDescriptorPool, std::uint32_t, const VkDescriptorSet*) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockUpdateDescriptorSets(VkDevice, std::uint32_t count, const VkWriteDescriptorSet* writes, std::uint32_t copyCount, const VkCopyDescriptorSet*) {
    Require(copyCount == 0, "descriptor copies are not expected");
    for (std::uint32_t i = 0; i < count; ++i) {
        MockDescriptorWrite write{writes[i].dstBinding, writes[i].descriptorCount, writes[i].descriptorType, {}};
        write.buffers.assign(writes[i].pBufferInfo, writes[i].pBufferInfo + writes[i].descriptorCount);
        mock.writes.push_back(write);
    }
}

VKAPI_ATTR void VKAPI_CALL mockCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint point, VkPipelineLayout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet*, std::uint32_t, const std::uint32_t*) {
    mock.boundPoint = point;
    mock.boundFirst = first;
    mock.boundSets = count;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL mockGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) {
    Require(mock.bufferMemory.contains(info->buffer), "BDA buffer was not bound");
    Require((mock.bufferUsage.at(info->buffer) & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0, "BDA buffer usage is missing");
    Require((mock.allocationFlags.at(mock.bufferMemory.at(info->buffer)) & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) != 0, "BDA allocation flags are missing");
    return 0x100000000000ULL + reinterpret_cast<std::uintptr_t>(info->buffer) * 0x10000;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* info, const VkAllocationCallbacks*, VkPipelineLayout* layout) {
    *layout = makeHandle<VkPipelineLayout>();
    mock.pipelineLayoutPushConstantSize[*layout] = info->pushConstantRangeCount > 0 ? info->pPushConstantRanges[0].size : 0;
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks*, VkShaderModule* module) {
    *module = makeHandle<VkShaderModule>();
    mock.shaderModules.emplace_back(info->pCode, info->pCode + info->codeSize / sizeof(std::uint32_t));
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateComputePipelines(VkDevice, VkPipelineCache, std::uint32_t count, const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks*, VkPipeline* pipelines) {
    Require(count == 1, "mock expects exactly one compute pipeline per call");
    Require(infos[0].stage.pSpecializationInfo != nullptr, "compute pipeline must provide specialization data");
    // The detiler's constants 0-2 (element size, block size, addressing family) are what the tests
    // check; the swizzle equation and block extent follow them.
    std::array<std::uint32_t, 3> values{};
    Require(infos[0].stage.pSpecializationInfo->dataSize >= sizeof(values), "compute pipeline specialization data has an unexpected size");
    std::memcpy(values.data(), infos[0].stage.pSpecializationInfo->pData, sizeof(values));
    *pipelines = makeHandle<VkPipeline>();
    mock.pipelineSpecializations.push_back(values);
    ++mock.pipelineCreateCount;
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateRenderPass(VkDevice, const VkRenderPassCreateInfo* info, const VkAllocationCallbacks*, VkRenderPass* pass) {
    Require(info->subpassCount == 1, "mock expects one subpass");
    mock.renderPassAttachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
    const auto* depth = info->pSubpasses[0].pDepthStencilAttachment;
    mock.renderPassDepth = depth != nullptr ? std::optional<VkAttachmentReference>(*depth) : std::nullopt;
    const auto& subpass = info->pSubpasses[0];
    mock.renderPassColors.assign(subpass.pColorAttachments, subpass.pColorAttachments + subpass.colorAttachmentCount);
    *pass = makeHandle<VkRenderPass>();
    ++mock.live;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateGraphicsPipelines(VkDevice, VkPipelineCache, std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks*, VkPipeline* pipelines) {
    Require(count == 1, "mock expects exactly one graphics pipeline per call");
    const auto* depthStencil = infos[0].pDepthStencilState;
    mock.depthStencil = depthStencil != nullptr ? std::optional<VkPipelineDepthStencilStateCreateInfo>(*depthStencil) : std::nullopt;
    mock.raster = *infos[0].pRasterizationState;
    mock.blendAttachments = infos[0].pColorBlendState->attachmentCount;
    mock.dynamicStates.assign(infos[0].pDynamicState->pDynamicStates, infos[0].pDynamicState->pDynamicStates + infos[0].pDynamicState->dynamicStateCount);
    *pipelines = makeHandle<VkPipeline>();
    ++mock.pipelineCreateCount;
    ++mock.live;
    return VK_SUCCESS;
}

// Every depth format but D16_UNORM_S8_UINT (which NVIDIA lacks) can be a depth attachment.
VKAPI_ATTR void VKAPI_CALL mockFormatProperties(VkPhysicalDevice, VkFormat format, VkFormatProperties* properties) {
    *properties = {};
    if (format != VK_FORMAT_D16_UNORM_S8_UINT) properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {
    --mock.live;
}

VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline pipeline) {
    mock.boundPipeline = pipeline;
}

VKAPI_ATTR void VKAPI_CALL mockCmdSetViewport(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*) {}

VKAPI_ATTR void VKAPI_CALL mockCmdSetScissor(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*) {}

VKAPI_ATTR void VKAPI_CALL mockCmdSetDepthBounds(VkCommandBuffer, float minimum, float maximum) {
    mock.depthBounds = std::make_pair(minimum, maximum);
}

VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t size, const void* values) {
    const auto* bytes = static_cast<const std::byte*>(values);
    mock.lastPushConstants.assign(bytes, bytes + size);
}

VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    mock.lastDispatchGroups = {x, y, z};
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    auto memory = mockBufferMemory(buffer);
    Require(offset + size <= memory.size(), "a buffer update exceeds its buffer");
    std::memcpy(memory.data() + offset, data, static_cast<std::size_t>(size));
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkGetBufferDeviceAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferDeviceAddress)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCreateBuffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindBufferMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockMapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockUnmapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
        {"vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(mockCreateDescriptorSetLayout)},
        {"vkDestroyDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyDescriptorSetLayout)},
        {"vkCreateDescriptorPool", reinterpret_cast<PFN_vkVoidFunction>(mockCreateDescriptorPool)},
        {"vkDestroyDescriptorPool", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyDescriptorPool)},
        {"vkAllocateDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateDescriptorSets)},
        {"vkFreeDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(mockFreeDescriptorSets)},
        {"vkUpdateDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(mockUpdateDescriptorSets)},
        {"vkCmdBindDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindDescriptorSets)},
        {"vkCreatePipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(mockCreatePipelineLayout)},
        {"vkDestroyPipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyPipelineLayout)},
        {"vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(mockCreateShaderModule)},
        {"vkDestroyShaderModule", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyShaderModule)},
        {"vkCreateComputePipelines", reinterpret_cast<PFN_vkVoidFunction>(mockCreateComputePipelines)},
        {"vkDestroyPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyPipeline)},
        {"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(mockCreateRenderPass)},
        {"vkDestroyRenderPass", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyRenderPass)},
        {"vkCreateGraphicsPipelines", reinterpret_cast<PFN_vkVoidFunction>(mockCreateGraphicsPipelines)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdSetViewport", reinterpret_cast<PFN_vkVoidFunction>(mockCmdSetViewport)},
        {"vkCmdSetScissor", reinterpret_cast<PFN_vkVoidFunction>(mockCmdSetScissor)},
        {"vkCmdSetDepthBounds", reinterpret_cast<PFN_vkVoidFunction>(mockCmdSetDepthBounds)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)}
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

AgcDriver::Graphics::Context mockContext() {
    AgcDriver::Graphics::Context context{};
    context.deviceProc = mockProc;
    context.memory.memoryTypeCount = 1;
    context.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    context.limits.minStorageBufferOffsetAlignment = 1;
    context.limits.maxBoundDescriptorSets = 1;
    context.limits.maxStorageBufferRange = 4096;
    context.limits.maxPerStageDescriptorStorageBuffers = 16;
    context.limits.maxPerStageResources = 128;
    context.limits.maxDescriptorSetStorageBuffers = 32;
    return context;
}

void bufferPoolTests() {
    using AgcDriver::Graphics::Buffer;
    const auto context = mockContext();
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags refreshable = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer first = VK_NULL_HANDLE;
    VkBuffer second = VK_NULL_HANDLE;
    {
        Buffer a(context, 300, storage);
        Buffer b(context, 300, storage);
        first = a.Handle();
        second = b.Handle();
    }
    {
        Buffer other(context, 300, refreshable);
        Require(other.Handle() != first && other.Handle() != second, "a free buffer of another usage was taken");
        Buffer larger(context, 600, storage);
        Require(larger.Handle() != first && larger.Handle() != second, "a free buffer of another size class was taken");
        Buffer classed(context, 400, storage);
        Require(classed.Handle() == first, "the most recently freed buffer of the size class was not taken");
        Buffer next(context, 260, storage);
        Require(next.Handle() == second, "the other free buffer of the size class was not taken");
        Buffer none(context, 300, storage);
        Require(none.Handle() != first && none.Handle() != second, "a buffer in use was taken");
    }
}

void bufferSlabTests() {
    using AgcDriver::Graphics::Buffer;
    using AgcDriver::Graphics::BufferPool;
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Require(BufferPool::SlabEligible(512, 256, 512, 64), "a size class with a smaller alignment and atom was refused a slab");
    Require(!BufferPool::SlabEligible(512, 1024, 512, 64), "a size class below its alignment was given a slab");
    Require(!BufferPool::SlabEligible(512, 256, 640, 64), "a buffer needing more than its class was given a slab");
    Require(!BufferPool::SlabEligible(512, 256, 512, 1024), "a size class below the non-coherent atom was given a slab");
    Require(!BufferPool::SlabEligible(std::size_t{1} << 20u, 256, std::size_t{1} << 20u, 64), "an exact-size buffer was given a slab");
    {
        auto context = mockContext();
        context.limits.nonCoherentAtomSize = 64;
        {
            Buffer a(context, 300, storage);
            Buffer b(context, 300, storage);
            Buffer c(context, 260, storage);
            Require(mock.allocations == 1, "buffers of one size class did not share one memory block");
            Require(mock.bufferMemory.at(a.Handle()) == mock.bufferMemory.at(b.Handle()) && mock.bufferMemory.at(b.Handle()) == mock.bufferMemory.at(c.Handle()), "buffers of one size class were bound to different blocks");
            std::set<VkDeviceSize> offsets;
            for (const auto handle : {a.Handle(), b.Handle(), c.Handle()}) {
                const auto offset = mock.bufferOffset.at(handle);
                Require(offset % 512 == 0, "a slab slot is not aligned to its size class");
                Require(offsets.insert(offset).second, "two live buffers share a slab slot");
            }
            for (auto* buffer : {&a, &b, &c}) {
                Require(buffer->Bytes().data() == mockBufferMemory(buffer->Handle()).data(), "a slab buffer's mapping is not its slot");
            }
            std::memset(a.Bytes().data(), 0x11, a.Bytes().size());
            std::memset(b.Bytes().data(), 0x22, b.Bytes().size());
            std::memset(c.Bytes().data(), 0x33, c.Bytes().size());
            Require(std::all_of(a.Bytes().begin(), a.Bytes().end(), [](std::byte value) { return value == std::byte{0x11}; }), "a neighbouring slab buffer overwrote another");
            Require(std::all_of(b.Bytes().begin(), b.Bytes().end(), [](std::byte value) { return value == std::byte{0x22}; }), "a neighbouring slab buffer overwrote another");
            Buffer larger(context, 600, storage);
            Require(mock.allocations == 2, "another size class did not get a block of its own");
        }
        Require(mock.frees == 0, "releasing slab buffers freed device memory");
        Buffer again(context, 300, storage);
        Require(mock.allocations == 2, "a released slab buffer was not reused");
    }
    Require(mock.live == 0, "slab blocks outlived their pool");
    mock = MockVulkan{};
    {
        auto context = mockContext();
        BufferPool pool(context);
        constexpr std::size_t slot = std::size_t{512} << 10u;
        const auto perBlock = static_cast<std::size_t>(BufferPool::SlabBlockBytes(slot) / slot);
        std::vector<AgcDriver::Graphics::SlabSlot> slots;
        for (std::size_t i = 0; i <= perBlock; ++i) {
            const auto taken = pool.TakeSlot(context, 0, slot, false);
            Require(taken.has_value(), "a slab slot could not be taken");
            slots.push_back(*taken);
        }
        Require(mock.allocations == 2 && pool.SlabBlocks() == 2, "a full block did not open a second one");
        std::set<std::pair<VkDeviceMemory, VkDeviceSize>> distinct;
        for (const auto& taken : slots) distinct.insert({taken.memory, taken.offset});
        Require(distinct.size() == slots.size(), "a slab handed out one slot twice");
        for (const auto& taken : slots) pool.PutSlot(taken.memory, taken.offset);
        Require(mock.frees == 1 && pool.SlabBlocks() == 1, "emptied blocks were not freed down to one spare");
        const auto reused = pool.TakeSlot(context, 0, slot, false);
        Require(reused.has_value() && mock.allocations == 2, "the spare block was not reused");
        pool.PutSlot(reused->memory, reused->offset);
    }
    Require(mock.live == 0, "slab blocks outlived their pool");
}

void descriptorRecycleTests() {
    using AgcDriver::Graphics::DescriptorCache;
    const auto context = mockContext();
    DescriptorCache cache(context);
    const std::array<VkDescriptorSetLayoutBinding, 1> first{{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
    const std::array<VkDescriptorSetLayoutBinding, 1> second{{{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
    const std::array<std::uint32_t, 4> firstKey{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    const std::array<std::uint32_t, 4> secondKey{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    const auto firstLayout = cache.Layout(firstKey, first);
    const auto secondLayout = cache.Layout(secondKey, second);
    const std::array<VkDescriptorPoolSize, 1> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    const auto freed = cache.Allocate(firstLayout, sizes);
    Require(freed.set != VK_NULL_HANDLE && freed.layout == firstLayout, "a chain set does not name its layout");
    cache.Free(freed);
    const auto other = cache.Allocate(secondLayout, sizes);
    Require(other.set != freed.set, "a freed set of another layout was handed out");
    const auto again = cache.Allocate(firstLayout, sizes);
    Require(again.set == freed.set && again.pool == freed.pool && again.layout == firstLayout, "a freed set of the same layout was not reused");
    const auto busy = cache.Allocate(firstLayout, sizes);
    Require(busy.set != freed.set, "a set in use was handed out again");
    cache.Free(other);
    cache.Free(again);
    cache.Free(busy);
}

using Role = ShaderRecompiler::DescriptorRole;
using Kind = ShaderRecompiler::DescriptorKind;

alignas(16) std::array<std::uint32_t, 4> guestFirst{0x11111111, 0x22222222, 0x33333333, 0x44444444};
alignas(16) std::array<std::uint32_t, 8> guestSecond{1, 2, 3, 4, 5, 6, 7, 8};
alignas(16) std::array<std::uint32_t, 2> guestThird{0xaaaaaaaa, 0xbbbbbbbb};

std::vector<std::uint32_t> vsharp(const void* pointer, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, bytes, 0x31000000u};
}

std::vector<std::uint32_t> join(std::vector<std::uint32_t> first, const std::vector<std::uint32_t>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

ShaderRecompiler::DescriptorBinding makeBinding(Role role, std::uint32_t binding, std::uint32_t count, std::vector<std::uint32_t> words) {
    ShaderRecompiler::DescriptorBinding result;
    result.kind = Kind::StorageBuffer;
    result.role = role;
    result.descriptorSet = 0;
    result.binding = binding;
    result.count = count;
    result.guestDescriptor = std::move(words);
    return result;
}

bool sameBytes(std::span<const std::byte> memory, const void* expected, std::size_t bytes) {
    return memory.size() >= bytes && std::memcmp(memory.data(), expected, bytes) == 0;
}

const MockDescriptorWrite& findWrite(std::uint32_t binding) {
    for (const auto& write : mock.writes) {
        if (write.binding == binding) return write;
    }
    throw std::runtime_error("expected descriptor write is missing for binding " + std::to_string(binding));
}

const VkDescriptorSetLayoutBinding& findLayoutBinding(std::uint32_t binding) {
    for (const auto& item : mock.layoutBindings) {
        if (item.binding == binding) return item;
    }
    throw std::runtime_error("expected descriptor set layout binding is missing for binding " + std::to_string(binding));
}

std::span<const std::byte> bufferBytes(VkBuffer buffer) {
    return mockBufferMemory(buffer);
}

void expectResourceFailure(const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, std::string_view reason) {
    mock = MockVulkan{};
    const auto context = mockContext();
    const auto color = AgcDriver::Graphics::DecodeState(makeState()).color;
    expectFailure([&] { AgcDriver::Graphics::ShaderResources resources(context, vertex, fragment, color, 0, 0); }, reason);
    Require(mock.live == 0, "failed shader resources leaked Vulkan objects");
}

void expectSingleFailure(const ShaderRecompiler::DescriptorBinding& binding, std::string_view reason) {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult fragment;
    vertex.bindings.push_back(binding);
    expectResourceFailure(vertex, fragment, reason);
}

void expectSingleAccepted(const ShaderRecompiler::DescriptorBinding& binding, std::string_view what) {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult fragment;
    vertex.bindings.push_back(binding);
    mock = MockVulkan{};
    const auto context = mockContext();
    const auto color = AgcDriver::Graphics::DecodeState(makeState()).color;
    { AgcDriver::Graphics::ShaderResources resources(context, vertex, fragment, color, 0, 0); }
    Require(mock.live == 0, std::string(what) + " leaked Vulkan objects");
}

void pushConstantTests() {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult fragment;
    vertex.pushConstants.assign(8, std::byte{1});
    fragment.pushConstants.assign(12, std::byte{2});
    std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 8}}};
    Require(AgcDriver::Graphics::PushConstantStages(shaders) == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT), "push constant stage union changed");
    const auto bytes = AgcDriver::Graphics::AssemblePushConstants(shaders);
    Require(bytes.size() == 128 && bytes[0] == std::byte{1} && bytes[7] == std::byte{1} && bytes[8] == std::byte{2} && bytes[19] == std::byte{2} && bytes[20] == std::byte{0} && bytes[127] == std::byte{0}, "assembled push constants are misplaced");
    shaders[1].pushConstantOffset = 4;
    expectFailure([&] { AgcDriver::Graphics::AssemblePushConstants(shaders); }, "overlap");
    shaders[1].pushConstantOffset = 120;
    expectFailure([&] { AgcDriver::Graphics::AssemblePushConstants(shaders); }, "outside the pipeline push constant block");
    shaders[1].pushConstantOffset = 2;
    expectFailure([&] { AgcDriver::Graphics::AssemblePushConstants(shaders); }, "DWORD aligned");
    shaders[1].pushConstantOffset = 8;
    fragment.pushConstants.assign(6, std::byte{2});
    expectFailure([&] { AgcDriver::Graphics::AssemblePushConstants(shaders); }, "DWORD aligned");
    fragment.pushConstants.clear();
    shaders[1].pushConstantOffset = 999;
    Require(AgcDriver::Graphics::PushConstantStages(shaders) == VK_SHADER_STAGE_VERTEX_BIT, "empty push constants contributed a stage");
    Require(AgcDriver::Graphics::AssemblePushConstants(shaders)[8] == std::byte{0}, "empty push constants were copied");
    shaders[1].program = nullptr;
    expectFailure([&] { AgcDriver::Graphics::AssemblePushConstants(shaders); }, "missing compiled shader");
}

void resourceTests() {
    mock = MockVulkan{};
    const auto context = mockContext();
    const auto state = AgcDriver::Graphics::DecodeState(makeState());
    const auto commands = reinterpret_cast<VkCommandBuffer>(std::uintptr_t{1});
    {
        ShaderRecompiler::RecompileResult vertex;
        ShaderRecompiler::RecompileResult fragment;
        vertex.bindings.push_back(makeBinding(Role::GuestBuffers, 0, 2, join(vsharp(guestFirst.data(), 16), vsharp(guestSecond.data(), 32))));
        vertex.bindings.push_back(makeBinding(Role::ShaderData, 5, 1, {7, 8, 9}));
        fragment.bindings.push_back(makeBinding(Role::FlattenedSrt, 43, 1, {1, 2}));
        fragment.bindings.push_back(makeBinding(Role::GuestBuffers, 44, 1, vsharp(guestThird.data(), 8)));
        AgcDriver::Graphics::ShaderResources resources(context, vertex, fragment, state.color, 0, 0);
        Require(resources.Layout() != VK_NULL_HANDLE, "descriptor set layout was not created");
        Require(mock.layoutBindings.size() == 4 && mock.writes.size() == 4, "one layout binding and one write per shader binding are expected");
        Require(findLayoutBinding(0).descriptorCount == 2 && findLayoutBinding(0).stageFlags == VK_SHADER_STAGE_VERTEX_BIT && findLayoutBinding(0).descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "guest buffer array layout binding is incorrect");
        Require(findLayoutBinding(5).descriptorCount == 1 && findLayoutBinding(5).stageFlags == VK_SHADER_STAGE_VERTEX_BIT, "shader data layout binding is incorrect");
        Require(findLayoutBinding(43).descriptorCount == 1 && findLayoutBinding(43).stageFlags == VK_SHADER_STAGE_FRAGMENT_BIT, "flattened SRT layout binding is incorrect");
        Require(findLayoutBinding(44).descriptorCount == 1 && findLayoutBinding(44).stageFlags == VK_SHADER_STAGE_FRAGMENT_BIT, "fragment guest buffer layout binding is incorrect");
        Require(mock.poolMaxSets == 1 && mock.poolSizes.size() == 1 && mock.poolSizes[0].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && mock.poolSizes[0].descriptorCount == 5, "descriptor pool must hold one set with every storage descriptor");
        const auto& array = findWrite(0);
        Require(array.count == 2 && array.buffers.size() == 2 && array.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "guest buffer array write is incorrect");
        Require(array.buffers[0].offset == 0 && array.buffers[0].range == 16 && array.buffers[1].offset == 0 && array.buffers[1].range == 32, "guest buffers must be bound at zero offset with their descriptor size");
        Require(sameBytes(bufferBytes(array.buffers[0].buffer), guestFirst.data(), 16) && sameBytes(bufferBytes(array.buffers[1].buffer), guestSecond.data(), 32), "guest buffer contents were not uploaded");
        const std::array<std::uint32_t, 3> data{7, 8, 9};
        Require(findWrite(5).buffers.size() == 1 && findWrite(5).buffers[0].range == 12 && sameBytes(bufferBytes(findWrite(5).buffers[0].buffer), data.data(), 12), "shader data buffer is incorrect");
        const std::array<std::uint32_t, 2> srt{1, 2};
        Require(findWrite(43).buffers.size() == 1 && findWrite(43).buffers[0].range == 8 && sameBytes(bufferBytes(findWrite(43).buffers[0].buffer), srt.data(), 8), "flattened SRT buffer is incorrect");
        Require(findWrite(44).buffers.size() == 1 && findWrite(44).buffers[0].range == 8 && sameBytes(bufferBytes(findWrite(44).buffers[0].buffer), guestThird.data(), 8), "fragment guest buffer is incorrect");
        resources.Bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE);
        Require(mock.boundPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && mock.boundFirst == 0 && mock.boundSets == 1, "exactly one descriptor set must be bound at set zero");
        auto first = mockBufferMemory(array.buffers[0].buffer);
        std::memset(first.data(), 0xab, 16);
        auto shaderData = mockBufferMemory(findWrite(5).buffers[0].buffer);
        std::memset(shaderData.data(), 0xcd, 12);
        resources.WriteBack();
        Require(guestFirst[0] == 0xabababab && guestFirst[3] == 0xabababab, "guest buffer was not written back");
        Require(guestSecond[0] == 1 && guestSecond[7] == 8 && guestThird[0] == 0xaaaaaaaa, "unmodified guest buffers changed on write back");
    }
    Require(mock.live == 0, "shader resources leaked Vulkan objects");
    mock = MockVulkan{};
    {
        ShaderRecompiler::RecompileResult vertex;
        ShaderRecompiler::RecompileResult fragment;
        AgcDriver::Graphics::ShaderResources resources(context, vertex, fragment, state.color, 0, 0);
        Require(resources.Layout() != VK_NULL_HANDLE && mock.layoutBindings.empty() && mock.poolSizes.empty() && mock.writes.empty(), "a shader without bindings must produce only an empty set layout");
        resources.Bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE);
        Require(mock.boundSets == 0, "an empty descriptor set was bound");
        resources.WriteBack();
    }
    Require(mock.live == 0, "empty shader resources leaked Vulkan objects");
    mock = MockVulkan{};
    {
        ShaderRecompiler::RecompileResult compute;
        compute.bindings.push_back(makeBinding(Role::GuestBuffers, 3, 1, vsharp(guestThird.data(), 8)));
        const AgcDriver::Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &compute, 0};
        AgcDriver::Graphics::ShaderResources resources(context, shader);
        Require(mock.layoutBindings.size() == 1 && findLayoutBinding(3).stageFlags == VK_SHADER_STAGE_COMPUTE_BIT && findLayoutBinding(3).descriptorCount == 1, "compute layout binding is incorrect");
        resources.Bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, VK_NULL_HANDLE);
        Require(mock.boundPoint == VK_PIPELINE_BIND_POINT_COMPUTE && mock.boundSets == 1, "compute descriptors were bound to the wrong bind point");
        guestThird = {0xaaaaaaaa, 0xbbbbbbbb};
        std::memset(mockBufferMemory(findWrite(3).buffers[0].buffer).data(), 0x5a, 8);
        resources.WriteBack();
        Require(guestThird[0] == 0x5a5a5a5a && guestThird[1] == 0x5a5a5a5a, "compute buffer was not written back");
        guestThird = {0xaaaaaaaa, 0xbbbbbbbb};
    }
    Require(mock.live == 0, "compute resources leaked Vulkan objects");
    mock = MockVulkan{};
    {
        auto descriptor = vsharp(guestSecond.data(), 2);
        descriptor[1] |= 16u << 16u;
        descriptor[3] = 0x0004dfacu;
        ShaderRecompiler::RecompileResult compute;
        compute.bindings.push_back(makeBinding(Role::GuestBuffers, 3, 1, descriptor));
        const AgcDriver::Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &compute, 0};
        AgcDriver::Graphics::ShaderResources resources(context, shader);
        const auto& buffer = findWrite(3).buffers.at(0);
        Require(buffer.range == sizeof(guestSecond), "strided buffer range does not cover every record");
        Require(sameBytes(bufferBytes(buffer.buffer), guestSecond.data(), sizeof(guestSecond)), "strided buffer contents were not uploaded");
        const std::uint32_t changed = 0x12345678u;
        auto bytes = mockBufferMemory(buffer.buffer);
        std::memcpy(bytes.data() + 16, &changed, sizeof(changed));
        resources.WriteBack();
        Require(guestSecond[4] == changed && guestSecond[0] == 1 && guestSecond[7] == 8, "strided buffer write back changed the wrong record");
        guestSecond[4] = 5;
    }
    Require(mock.live == 0, "strided buffer resources leaked Vulkan objects");
    {
        ShaderRecompiler::RecompileResult vertex;
        const AgcDriver::Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0};
        expectFailure([&] { AgcDriver::Graphics::ShaderResources resources(context, shader); }, "compute resources require a compute shader");
    }
    const auto base = makeBinding(Role::GuestBuffers, 0, 1, vsharp(guestThird.data(), 8));
    const auto changed = [&](auto mutate) {
        auto binding = base;
        mutate(binding);
        return binding;
    };
    // Image and sampler descriptors are T# (8 dwords) and S# (4 dwords) words.
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::GuestImages; binding.kind = Kind::SampledImage; }), "8 dwords");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::GuestImages; binding.kind = Kind::StorageImage; }), "8 dwords");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::GuestSamplers; binding.kind = Kind::Sampler; binding.guestDescriptor.resize(3); }), "4 dwords");
    // GDS binds the device's GDS buffer, which a context without one cannot.
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::Gds; binding.guestDescriptor.clear(); }), "no GDS buffer");
    {
        mock = MockVulkan{};
        auto gdsContext = context;
        gdsContext.gdsBuffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(0x6d5));
        ShaderRecompiler::RecompileResult vertex;
        ShaderRecompiler::RecompileResult fragment;
        vertex.bindings.push_back(changed([](auto& binding) { binding.role = Role::Gds; binding.binding = 7; binding.guestDescriptor.clear(); }));
        AgcDriver::Graphics::ShaderResources resources(gdsContext, vertex, fragment, state.color, 0, 0);
        const auto& write = findWrite(7);
        Require(write.buffers.size() == 1 && write.buffers[0].buffer == gdsContext.gdsBuffer && write.buffers[0].offset == 0 && write.buffers[0].range == AgcDriver::Pm4::GdsBytes, "the GDS binding does not name the device's GDS buffer");
        Require(resources.WritesMemory(), "a GDS binding does not count as a memory write");
        auto array = changed([](auto& binding) { binding.role = Role::Gds; binding.count = 2; binding.guestDescriptor.clear(); });
        ShaderRecompiler::RecompileResult arrayed;
        arrayed.bindings.push_back(array);
        expectFailure([&] { AgcDriver::Graphics::ShaderResources failed(gdsContext, arrayed, fragment, state.color, 0, 0); }, "must not be an array");
    }
    Require(mock.live == 0, "GDS resources leaked Vulkan objects");
    mock = MockVulkan{};
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::BdaPagetable; binding.guestDescriptor.clear(); }), "BDA table and fault descriptors");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::FaultBuffer; binding.guestDescriptor.clear(); }), "BDA table and fault descriptors");
    expectSingleFailure(changed([](auto& binding) { binding.kind = Kind::UniformBuffer; }), "unsupported descriptor kind UniformBuffer");
    expectSingleFailure(changed([](auto& binding) { binding.kind = Kind::UniformTexelBuffer; }), "unsupported descriptor kind UniformTexelBuffer");
    expectSingleFailure(changed([](auto& binding) { binding.kind = Kind::StorageTexelBuffer; }), "unsupported descriptor kind StorageTexelBuffer");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::ShaderData; binding.kind = Kind::SampledImage; }), "unsupported descriptor kind SampledImage");
    expectSingleFailure(changed([](auto& binding) { binding.descriptorSet = 1; }), "unexpected descriptor set");
    expectSingleFailure(changed([](auto& binding) { binding.readOnly = true; }), "read-only descriptors are unsupported");
    expectSingleFailure(changed([](auto& binding) { binding.count = 0; }), "empty descriptor binding");
    expectSingleFailure(changed([](auto& binding) { binding.count = 2; }), "four DWORDs per array element");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::ShaderData; binding.count = 2; binding.guestDescriptor = {1, 2}; }), "must not be arrays");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::ShaderData; binding.guestDescriptor.clear(); }), "empty shader data descriptor");
    expectSingleFailure(changed([](auto& binding) { binding.role = Role::FlattenedSrt; binding.guestDescriptor.clear(); }), "empty shader data descriptor");
    expectSingleFailure(changed([](auto& binding) { binding.guestDescriptor[1] |= 0x40000000u; }), "reserved bits");
    expectSingleFailure(changed([](auto& binding) { binding.guestDescriptor[3] |= 0x40000000u; }), "unsupported type");
    expectSingleFailure(changed([](auto& binding) { binding.guestDescriptor[1] |= 0x3fffu << 16u; binding.guestDescriptor[2] = 0xffffffffu; }), "descriptor range limit");
    expectSingleFailure(changed([](auto& binding) { binding.guestDescriptor[2] = 8192; }), "descriptor range limit");
    expectSingleAccepted(changed([](auto& binding) { binding.guestDescriptor = vsharp(reinterpret_cast<const void*>(0x1000), 8); }), "an unmapped V#");
    expectSingleFailure(changed([&](auto& binding) { binding.guestDescriptor = vsharp(reinterpret_cast<const void*>(state.color.address), 64); }), "aliases the render target");
    // A descriptor over unmapped memory is a sparse region that reads as zeros (GuestBufferMemory),
    // not a failed build.
    {
        mock = MockVulkan{};
        ShaderRecompiler::RecompileResult vertex;
        ShaderRecompiler::RecompileResult fragment;
        vertex.bindings.push_back(changed([](auto& binding) { binding.guestDescriptor = vsharp(reinterpret_cast<const void*>(0x1000), 8); }));
        AgcDriver::Graphics::ShaderResources resources(context, vertex, fragment, state.color, 0, 0);
    }
    expectSingleAccepted(changed([](auto& binding) { binding.count = 3; binding.guestDescriptor = join(join(vsharp(guestFirst.data(), 16), vsharp(guestSecond.data(), 32)), vsharp(reinterpret_cast<const void*>(0x1000), 8)); }), "an unmapped V# element");
    expectSingleFailure(changed([&](auto& binding) { binding.count = 2; binding.guestDescriptor = join(vsharp(guestFirst.data(), 16), vsharp(reinterpret_cast<const void*>(state.color.address), 64)); }), "aliases the render target");
    expectSingleFailure(changed([](auto& binding) { binding.count = 17; binding.guestDescriptor.assign(68, 0); }), "per-stage limits");
    {
        ShaderRecompiler::RecompileResult vertex;
        ShaderRecompiler::RecompileResult fragment;
        vertex.bindings.push_back(makeBinding(Role::GuestBuffers, 0, 1, vsharp(guestFirst.data(), 16)));
        fragment.bindings.push_back(makeBinding(Role::GuestBuffers, 0, 1, vsharp(guestSecond.data(), 32)));
        expectResourceFailure(vertex, fragment, "duplicate shader binding");
        fragment.bindings.front().binding = 1;
        fragment.bindings.front().guestDescriptor = vsharp(guestFirst.data(), 16);
    }
}

void misalignedShaderDataTests() {
    mock = MockVulkan{};
    auto context = mockContext();
    context.limits.minStorageBufferOffsetAlignment = 16;
    const auto commands = reinterpret_cast<VkCommandBuffer>(std::uintptr_t{1});
    alignas(64) std::array<std::uint32_t, 8> guest{1, 2, 3, 4, 5, 6, 7, 8};
    {
        ShaderRecompiler::RecompileResult compute;
        compute.bindings.push_back(makeBinding(Role::GuestBuffers, 0, 2, join(vsharp(guest.data(), 32), vsharp(guest.data() + 1, 8))));
        compute.bindings.push_back(makeBinding(Role::ShaderData, 1, 1, {0x11, 0x22, 0}));
        compute.memoryOffsetDword = 2;
        auto live = compute;
        live.bindings[1].guestDescriptor = {0x33, 0x44, 0};
        const AgcDriver::Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &compute, 0};
        const AgcDriver::Graphics::CompiledShader liveShader{ShaderRecompiler::ShaderStage::Compute, &live, 0};
        AgcDriver::Graphics::ShaderResources resources(context, shader);
        const auto& views = findWrite(0);
        Require(views.buffers.size() == 2 && views.buffers[0].range == 32 && views.buffers[1].range == 12 && views.buffers[1].offset == views.buffers[0].offset, "the misaligned view is not bound from the aligned offset below it");
        const auto data = findWrite(1).buffers.at(0).buffer;
        const std::array<std::uint32_t, 3> patched{0x11, 0x22, 0x400};
        Require(sameBytes(bufferBytes(data), patched.data(), sizeof(patched)), "the shader data buffer does not hold the misaligned view's offset");
        Require(resources.RefreshData(commands, liveShader), "a refresh with different words recorded nothing");
        const std::array<std::uint32_t, 3> refreshed{0x33, 0x44, 0x400};
        Require(sameBytes(bufferBytes(data), refreshed.data(), sizeof(refreshed)), "a data refresh dropped the misaligned view's offset");
        Require(!resources.DataWordsDiffer(liveShader) && resources.DataWordsHash() == AgcDriver::Graphics::ShaderResources::DataWordsHash(liveShader), "the refreshed template's words are not the dispatch's");
    }
    Require(mock.live == 0, "misaligned shader data resources leaked Vulkan objects");
}

struct ModuleShape {
    bool fragment = false;
    bool push = false;
    std::uint32_t pushLength = 32;
    std::uint32_t pushStride = 4;
    std::uint32_t bufferArray = 0;
    bool plainBuffer = false;
    bool shaderData = false;
    bool vertexInput = false;
    bool barycentric = false;
    bool barycentricNoPerspective = false;
    std::uint32_t barycentricComponents = 3;
    bool perVertex = false;
    std::uint32_t perVertexLength = 3;
    bool parameterOutput = false;
    std::uint32_t parameterLocation = 0;
    bool rectParameters = false;
    bool secondTarget = false;
    bool fragDepth = false;
    bool depthReplacing = false;
};

void emit(std::vector<std::uint32_t>& out, spv::Op op, std::initializer_list<std::uint32_t> operands) {
    out.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16u) | static_cast<std::uint32_t>(op));
    out.insert(out.end(), operands.begin(), operands.end());
}

std::vector<std::uint32_t> makeModule(const ModuleShape& shape) {
    std::vector<std::uint32_t> annotations;
    std::vector<std::uint32_t> declarations;
    std::vector<std::uint32_t> function;
    std::uint32_t next = 1;
    const auto id = [&] { return next++; };
    const auto voidType = id();
    const auto functionType = id();
    const auto floatType = id();
    const auto vectorType = id();
    const auto uintType = id();
    const auto outputPointer = id();
    const auto output = id();
    const auto main = id();
    const auto label = id();
    const auto inputPointer = id();
    const auto input = id();
    std::vector<std::uint32_t> extraInterface;
    emit(declarations, spv::OpTypeVoid, {voidType});
    emit(declarations, spv::OpTypeFunction, {functionType, voidType});
    emit(declarations, spv::OpTypeFloat, {floatType, 32});
    emit(declarations, spv::OpTypeVector, {vectorType, floatType, 4});
    emit(declarations, spv::OpTypeInt, {uintType, 32, 0});
    emit(declarations, spv::OpTypePointer, {outputPointer, spv::StorageClassOutput, vectorType});
    emit(declarations, spv::OpVariable, {outputPointer, output, spv::StorageClassOutput});
    if (shape.parameterOutput) {
        const auto parameter = id();
        emit(declarations, spv::OpVariable, {outputPointer, parameter, spv::StorageClassOutput});
        emit(annotations, spv::OpDecorate, {parameter, spv::DecorationLocation, shape.parameterLocation});
        extraInterface.push_back(parameter);
    }
    if (shape.secondTarget) {
        const auto target = id();
        emit(declarations, spv::OpVariable, {outputPointer, target, spv::StorageClassOutput});
        emit(annotations, spv::OpDecorate, {target, spv::DecorationLocation, 1});
        extraInterface.push_back(target);
    }
    if (shape.rectParameters) {
        const auto pointer = id();
        emit(declarations, spv::OpTypePointer, {pointer, spv::StorageClassInput, vectorType});
        for (std::uint32_t location = 0; location < 2; ++location) {
            const auto parameter = id();
            emit(declarations, spv::OpVariable, {pointer, parameter, spv::StorageClassInput});
            emit(annotations, spv::OpDecorate, {parameter, spv::DecorationLocation, location});
            if (location == 1) emit(annotations, spv::OpDecorate, {parameter, spv::DecorationFlat});
            extraInterface.push_back(parameter);
        }
    }
    if (shape.barycentric) {
        const auto vector = id();
        const auto pointer = id();
        const auto variable = id();
        emit(declarations, spv::OpTypeVector, {vector, floatType, shape.barycentricComponents});
        emit(declarations, spv::OpTypePointer, {pointer, spv::StorageClassInput, vector});
        emit(declarations, spv::OpVariable, {pointer, variable, spv::StorageClassInput});
        emit(annotations, spv::OpDecorate, {variable, spv::DecorationBuiltIn, shape.barycentricNoPerspective ? spv::BuiltInBaryCoordNoPerspKHR : spv::BuiltInBaryCoordKHR});
        extraInterface.push_back(variable);
    }
    if (shape.perVertex) {
        const auto length = id();
        const auto array = id();
        const auto pointer = id();
        const auto variable = id();
        emit(declarations, spv::OpConstant, {uintType, length, shape.perVertexLength});
        emit(declarations, spv::OpTypeArray, {array, vectorType, length});
        emit(declarations, spv::OpTypePointer, {pointer, spv::StorageClassInput, array});
        emit(declarations, spv::OpVariable, {pointer, variable, spv::StorageClassInput});
        emit(annotations, spv::OpDecorate, {variable, spv::DecorationLocation, 0});
        emit(annotations, spv::OpDecorate, {variable, spv::DecorationPerVertexKHR});
        extraInterface.push_back(variable);
    }
    if (shape.fragDepth) {
        const auto pointer = id();
        const auto variable = id();
        emit(declarations, spv::OpTypePointer, {pointer, spv::StorageClassOutput, floatType});
        emit(declarations, spv::OpVariable, {pointer, variable, spv::StorageClassOutput});
        emit(annotations, spv::OpDecorate, {variable, spv::DecorationBuiltIn, spv::BuiltInFragDepth});
        extraInterface.push_back(variable);
    }
    if (shape.vertexInput) {
        emit(declarations, spv::OpTypePointer, {inputPointer, spv::StorageClassInput, vectorType});
        emit(declarations, spv::OpVariable, {inputPointer, input, spv::StorageClassInput});
        emit(annotations, spv::OpDecorate, {input, spv::DecorationLocation, 0});
    }
    if (shape.fragment) emit(annotations, spv::OpDecorate, {output, spv::DecorationLocation, 0});
    else emit(annotations, spv::OpDecorate, {output, spv::DecorationBuiltIn, spv::BuiltInPosition});
    if (shape.push) {
        const auto length = id();
        const auto array = id();
        const auto block = id();
        const auto pointer = id();
        const auto variable = id();
        emit(declarations, spv::OpConstant, {uintType, length, shape.pushLength});
        emit(declarations, spv::OpTypeArray, {array, uintType, length});
        emit(declarations, spv::OpTypeStruct, {block, array});
        emit(declarations, spv::OpTypePointer, {pointer, spv::StorageClassPushConstant, block});
        emit(declarations, spv::OpVariable, {pointer, variable, spv::StorageClassPushConstant});
        emit(annotations, spv::OpDecorate, {array, spv::DecorationArrayStride, shape.pushStride});
        emit(annotations, spv::OpDecorate, {block, spv::DecorationBlock});
        emit(annotations, spv::OpMemberDecorate, {block, 0, spv::DecorationOffset, 0});
    }
    if (shape.bufferArray != 0 || shape.plainBuffer || shape.shaderData) {
        const auto runtime = id();
        const auto block = id();
        emit(declarations, spv::OpTypeRuntimeArray, {runtime, uintType});
        emit(declarations, spv::OpTypeStruct, {block, runtime});
        emit(annotations, spv::OpDecorate, {runtime, spv::DecorationArrayStride, 4});
        emit(annotations, spv::OpDecorate, {block, spv::DecorationBlock});
        emit(annotations, spv::OpMemberDecorate, {block, 0, spv::DecorationOffset, 0});
        const auto declare = [&](std::uint32_t type, std::uint32_t binding) {
            const auto pointer = id();
            const auto variable = id();
            emit(declarations, spv::OpTypePointer, {pointer, spv::StorageClassStorageBuffer, type});
            emit(declarations, spv::OpVariable, {pointer, variable, spv::StorageClassStorageBuffer});
            emit(annotations, spv::OpDecorate, {variable, spv::DecorationDescriptorSet, 0});
            emit(annotations, spv::OpDecorate, {variable, spv::DecorationBinding, binding});
        };
        if (shape.bufferArray != 0) {
            const auto length = id();
            const auto array = id();
            emit(declarations, spv::OpConstant, {uintType, length, shape.bufferArray});
            emit(declarations, spv::OpTypeArray, {array, block, length});
            declare(array, 0);
        }
        if (shape.plainBuffer) declare(block, 0);
        if (shape.shaderData) declare(block, 5);
    }
    emit(function, spv::OpFunction, {voidType, main, 0, functionType});
    emit(function, spv::OpLabel, {label});
    emit(function, spv::OpReturn, {});
    emit(function, spv::OpFunctionEnd, {});
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x10300, 0, next, 0};
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    if (shape.barycentric) {
        emit(words, spv::OpCapability, {spv::CapabilityFragmentBarycentricKHR});
        const std::string extension = "SPV_KHR_fragment_shader_barycentric";
        const auto count = (extension.size() + 4) / 4;
        words.push_back((static_cast<std::uint32_t>(count + 1) << 16u) | spv::OpExtension);
        const auto start = words.size();
        words.resize(start + count, 0);
        for (std::size_t i = 0; i < extension.size(); ++i) words[start + i / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(extension[i])) << ((i % 4) * 8);
    }
    emit(words, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    const auto entryPointOffset = words.size();
    if (shape.vertexInput) emit(words, spv::OpEntryPoint, {spv::ExecutionModelVertex, main, 0x6e69616du, 0, output, input});
    else emit(words, spv::OpEntryPoint, {shape.fragment ? spv::ExecutionModelFragment : spv::ExecutionModelVertex, main, 0x6e69616du, 0, output});
    words[entryPointOffset] += static_cast<std::uint32_t>(extraInterface.size()) << 16u;
    words.insert(words.end(), extraInterface.begin(), extraInterface.end());
    if (shape.fragment) emit(words, spv::OpExecutionMode, {main, spv::ExecutionModeOriginUpperLeft});
    if (shape.depthReplacing) emit(words, spv::OpExecutionMode, {main, spv::ExecutionModeDepthReplacing});
    words.insert(words.end(), annotations.begin(), annotations.end());
    words.insert(words.end(), declarations.begin(), declarations.end());
    words.insert(words.end(), function.begin(), function.end());
    return words;
}

// A depth-tested state makes a render pass with the resident depth image's attachment after the
// colors (always GENERAL, loaded and stored) and the pipeline's depth-stencil state.
void depthPipelineTests() {
    using namespace AgcDriver::Graphics;
    mock = MockVulkan{};
    auto context = mockContext();
    context.formatProperties = mockFormatProperties;
    context.limits.maxColorAttachments = 8;
    ShaderRecompiler::RecompileResult vertex;
    vertex.spirv = makeModule({});
    ShaderRecompiler::RecompileResult fragment;
    fragment.spirv = makeModule({.fragment = true});
    const std::array<CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
    auto queue = makeDepthState();
    queue.context[0x205] = 0x240u | 0x1800u;
    queue.context[0x2de] = 0x1e9;
    queue.context[0x2df] = 0;
    queue.context[0x2e0] = queue.context[0x2e2] = std::bit_cast<std::uint32_t>(32.0f);
    queue.context[0x2e1] = queue.context[0x2e3] = std::bit_cast<std::uint32_t>(3.0f);
    const auto state = DecodeState(queue);
    Require(DepthAttachmentFormat(context, state.depthTarget) == VK_FORMAT_D32_SFLOAT_S8_UINT, "Z_32_FLOAT with stencil did not render as D32_SFLOAT_S8_UINT");
    {
        ShaderResources resources(context, vertex, fragment, state.color, 0, 0);
        const VertexInputLayout input{};
        Pipeline pipeline(context, state, input, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        Require(mock.renderPassAttachments.size() == 2 && mock.renderPassDepth && mock.renderPassDepth->attachment == 1 && mock.renderPassDepth->layout == VK_IMAGE_LAYOUT_GENERAL, "the render pass lacks the depth attachment");
        const auto& depth = mock.renderPassAttachments[1];
        Require(depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT && depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && depth.storeOp == VK_ATTACHMENT_STORE_OP_STORE && depth.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD && depth.stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE && depth.initialLayout == VK_IMAGE_LAYOUT_GENERAL && depth.finalLayout == VK_IMAGE_LAYOUT_GENERAL, "the depth attachment does not keep its contents in GENERAL");
        Require(mock.depthStencil && mock.depthStencil->depthTestEnable && mock.depthStencil->depthWriteEnable && mock.depthStencil->depthCompareOp == VK_COMPARE_OP_LESS_OR_EQUAL && !mock.depthStencil->stencilTestEnable && !mock.depthStencil->depthBoundsTestEnable, "the pipeline's depth-stencil state was not built");
        Require(mock.raster.depthBiasEnable && mock.raster.depthBiasSlopeFactor == 2.0f && mock.raster.depthBiasConstantFactor == 3.0f, "the pipeline lacks the depth bias");
        Require(std::find(mock.dynamicStates.begin(), mock.dynamicStates.end(), VK_DYNAMIC_STATE_DEPTH_BOUNDS) == mock.dynamicStates.end(), "a pipeline without depth bounds made them dynamic");
        mock.depthBounds.reset();
        pipeline.Continue(VK_NULL_HANDLE, state);
        Require(!mock.depthBounds, "a pipeline without depth bounds set them");
    }
    {
        auto bounded = queue;
        bounded.context[0x200] = 8u;
        bounded.context[0x8] = std::bit_cast<std::uint32_t>(0.25f);
        bounded.context[0x9] = std::bit_cast<std::uint32_t>(0.5f);
        auto boundsState = DecodeState(bounded);
        ShaderResources resources(context, vertex, fragment, boundsState.color, 0, 0);
        expectFailure([&] { Pipeline pipeline(context, boundsState, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL); }, "depthBounds feature");
        expectFailure([&] { ValidateDepthBounds(context, boundsState.depth); }, "depthBounds feature");
        context.depthBounds = true;
        ValidateDepthBounds(context, boundsState.depth);
        {
            Pipeline pipeline(context, boundsState, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
            Require(mock.renderPassDepth && mock.depthStencil && mock.depthStencil->depthBoundsTestEnable && !mock.depthStencil->depthTestEnable && !mock.depthStencil->depthWriteEnable, "the pipeline lacks the depth bounds test");
            Require(std::find(mock.dynamicStates.begin(), mock.dynamicStates.end(), VK_DYNAMIC_STATE_DEPTH_BOUNDS) != mock.dynamicStates.end(), "the depth bounds are not dynamic state");
            pipeline.Continue(VK_NULL_HANDLE, boundsState);
            Require(mock.depthBounds && mock.depthBounds->first == 0.25f && mock.depthBounds->second == 0.5f, "the draw's depth bounds were not set");
            boundsState.depth.depthBoundsMax = 0.75f;
            pipeline.Continue(VK_NULL_HANDLE, boundsState);
            Require(mock.depthBounds->second == 0.75f, "a continued draw kept the previous depth bounds");
        }
        boundsState.depth.depthBoundsMax = 1.5f;
        expectFailure([&] { ValidateDepthBounds(context, boundsState.depth); }, "VK_EXT_depth_range_unrestricted");
        context.depthRangeUnrestricted = true;
        ValidateDepthBounds(context, boundsState.depth);
        context.depthRangeUnrestricted = false;
        const auto before = mock.pipelineCreateCount;
        vertex.variantId = 1;
        fragment.variantId = 2;
        auto unbounded = DecodeState(queue);
        unbounded.depth.depthTest = boundsState.depth.depthTest;
        unbounded.depth.depthWrite = boundsState.depth.depthWrite;
        unbounded.depth.depthCompare = boundsState.depth.depthCompare;
        unbounded.depth.depthBias = false;
        auto lowered = boundsState;
        lowered.depth.depthBoundsMin = 0.0f;
        const auto withBounds = CachedPipeline(context, boundsState, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        Require(CachedPipeline(context, lowered, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL) == withBounds, "draws differing only in their depth bounds values did not share a pipeline");
        Require(CachedPipeline(context, unbounded, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL) != withBounds && mock.pipelineCreateCount == before + 2, "the depth bounds test did not separate pipelines");
        ClearCachedPipelines(context.device);
        vertex.variantId = fragment.variantId = 0;
        context.depthBounds = false;
    }
    // Without a depth attachment the pipeline carries no depth-stencil state.
    {
        const auto plain = DecodeState(makeState());
        ShaderResources resources(context, vertex, fragment, plain.color, 0, 0);
        Pipeline pipeline(context, plain, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        Require(mock.renderPassAttachments.size() == 1 && !mock.renderPassDepth && !mock.depthStencil && !mock.raster.depthBiasEnable, "a color-only draw got depth state");
    }
    Require(mock.live == 0, "depth pipelines leaked Vulkan objects");
    // The fallbacks of a device without D16_UNORM_S8_UINT.
    DepthTarget z16{};
    z16.zFormat = 1;
    z16.stencil = true;
    Require(DepthAttachmentFormat(context, z16) == VK_FORMAT_D24_UNORM_S8_UINT, "Z_16 with stencil did not fall back to D24_UNORM_S8_UINT");
    z16.stencil = false;
    Require(DepthAttachmentFormat(context, z16) == VK_FORMAT_D16_UNORM, "Z_16 did not render as D16_UNORM");
    DepthTarget stencilOnly{};
    stencilOnly.stencil = true;
    Require(DepthAttachmentFormat(context, stencilOnly) == VK_FORMAT_S8_UINT && DepthAspects(VK_FORMAT_S8_UINT) == VK_IMAGE_ASPECT_STENCIL_BIT, "a stencil-only surface did not render as S8_UINT");
}

void rectListTests() {
    using namespace ShaderRecompiler;
    using namespace AgcDriver::Graphics;
    RecompileResult vertex;
    vertex.spirv = makeModule({});
    RecompileResult fragment;
    fragment.spirv = makeModule({.fragment = true});
    const std::array<std::uint32_t, 2> capabilities{spv::CapabilityShader, spv::CapabilityTessellation};
    SpirvTarget target{};
    target.vulkanVersion = VK_API_VERSION_1_1;
    target.spirvVersion = 0x00010300u;
    target.supportedCapabilities = capabilities;
    target.tessellation = TessellationTargetLimits{32, 128, 128, 120, 4096, 128, 128};
    for (const auto version : {0x00010300u, 0x00010400u}) {
        target.spirvVersion = version;
        auto auxiliary = BuildRectListShaders(vertex, fragment, target);
        const std::array<CompiledShader, 4> shaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::TessellationControl, &auxiliary.control, 0}, {ShaderStage::TessellationEvaluation, &auxiliary.evaluation, 0}, {ShaderStage::Fragment, &fragment, 0}}};
        for (const auto primitive : {7u, 17u}) {
            auto queue = makeState();
            queue.userConfig[0x242] = primitive;
            queue.context[0x205] = 3;
            auto state = DecodeState(queue);
            Require(state.rectList && state.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && state.cullMode == VK_CULL_MODE_NONE, "rect-list state was not decoded");
            Require(!state.stages.tessellation && state.stages.path == ShaderPath::Vertex, "rect-list changed guest shader routing");
            ValidateShaders(shaders, state, VkPhysicalDeviceSubgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES}, false);
            state.rectList = false;
            expectFailure([&] { ValidateShaders(shaders, state, VkPhysicalDeviceSubgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES}, false); }, "stage count");
        }
    }
    vertex.spirv = makeModule({.parameterOutput = true});
    fragment.spirv = makeModule({.fragment = true, .rectParameters = true});
    vertex.parameterExports = {0};
    fragment.fragmentParameters = {{0, 0, false, false}, {1, 0, true, false}};
    auto auxiliary = BuildRectListShaders(vertex, fragment, target);
    Require(!auxiliary.control.spirv.empty() && !auxiliary.evaluation.spirv.empty(), "rect-list parameter shaders are empty");
    auto parameterQueue = makeState();
    parameterQueue.userConfig[0x242] = 17;
    const auto parameterState = DecodeState(parameterQueue);
    const std::array<CompiledShader, 4> parameterShaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::TessellationControl, &auxiliary.control, 0}, {ShaderStage::TessellationEvaluation, &auxiliary.evaluation, 0}, {ShaderStage::Fragment, &fragment, 0}}};
    ValidateShaders(parameterShaders, parameterState, VkPhysicalDeviceSubgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES}, false);
    fragment.fragmentParameters[0].perVertex = true;
    auto explicitInterpolation = BuildRectListShaders(vertex, fragment, target);
    Require(!explicitInterpolation.control.spirv.empty() && !explicitInterpolation.evaluation.spirv.empty(), "rect-list shaders for an explicitly interpolated parameter are empty");
    fragment.fragmentParameters[0].custom = true;
    expectFailure([&] { static_cast<void>(BuildRectListShaders(vertex, fragment, target)); }, "per-vertex interpolation");
    fragment.fragmentParameters[0].perVertex = false;
    fragment.fragmentParameters[0].custom = false;
    vertex.parameterExports.clear();
    auto unexported = BuildRectListShaders(vertex, fragment, target);
    Require(!unexported.control.spirv.empty() && !unexported.evaluation.spirv.empty(), "rect-list shaders with an unexported parameter are empty");
    fragment.fragmentParameters.clear();
    target.tessellation->maxPatchSize = 3;
    expectFailure([&] { static_cast<void>(BuildRectListShaders(vertex, fragment, target)); }, "device limits");
    target.tessellation.reset();
    expectFailure([&] { static_cast<void>(BuildRectListShaders(vertex, fragment, target)); }, "unavailable");
    auto queue = makeState();
    queue.userConfig[0x242] = 17;
    const auto state = DecodeState(queue);
    const Context context{};
    const AgcDriver::Pm4::DrawParameters draw{0, 4, 0, 1, 0, false};
    expectFailure([&] { Draw(context, state, draw, {}); }, "incomplete rect-list");
}

// Recompiles `code` as a pixel shader with PERSP_CENTER loaded and one SPI_PS_INPUT_CNTL word per
// input.
ShaderRecompiler::RecompileResult recompilePixel(std::initializer_list<std::uint32_t> controls, std::span<const std::uint32_t> code, std::uint32_t colorFormat = 9u, bool barycentric = true) {
    auto queue = makeState();
    queue.context[0x1b3] = 0x2u;
    queue.context[0x1b4] = 0x2u;
    queue.context[0x1b6] = static_cast<std::uint32_t>(controls.size());
    queue.context[0x1c5] = colorFormat;
    std::uint32_t index = 0;
    for (const auto control : controls) queue.context[0x191 + index++] = control;
    const auto pixel = AgcDriver::Graphics::DecodePixelStageInfo(queue.context, IdentityExports);
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = barycentric;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

struct LocatedInput {
    std::uint32_t location;
    bool flat;
    bool perVertex;
};

// The Location-decorated input variables of a module, in location order.
std::vector<LocatedInput> locatedInputs(std::span<const std::uint32_t> words) {
    std::map<std::uint32_t, LocatedInput> decorated;
    std::set<std::uint32_t> inputs;
    for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
        const auto op = static_cast<spv::Op>(words[at] & 0xffffu);
        if (op == spv::OpVariable && words[at + 3] == spv::StorageClassInput) inputs.insert(words[at + 2]);
        if (op != spv::OpDecorate) continue;
        auto& input = decorated[words[at + 1]];
        if (words[at + 2] == spv::DecorationLocation) input.location = words[at + 3] + 1u;
        if (words[at + 2] == spv::DecorationFlat) input.flat = true;
        if (words[at + 2] == spv::DecorationPerVertexKHR) input.perVertex = true;
    }
    std::vector<LocatedInput> result;
    for (const auto& [id, input] : decorated) {
        if (input.location != 0 && inputs.contains(id)) result.push_back({input.location - 1u, input.flat, input.perVertex});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.location < b.location; });
    return result;
}

// SPI_PS_INPUT_CNTL_n.OFFSET names the parameter slot an input reads, so pixel inputs are declared
// at their slots: inputs reading one slot share its variable, a slot read both flat and
// interpolated is taken per vertex, and an input with OFFSET bit 5 set is its DEFAULT_VAL constant
// unless FLAT_SHADE is set too, which passes the slot's vertices through unchanged.
void pixelParameterSlotTests() {
    using AgcDriver::Graphics::CompiledShader;
    const VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    auto state = AgcDriver::Graphics::DecodeState(makeState());
    ShaderRecompiler::RecompileResult vertex;
    vertex.spirv = makeModule({.parameterOutput = true});
    // v_interp_p1/p2_f32 v4, attr0.x; v_interp_mov_f32 v5, p0, attr1.x; v_interp_mov_f32 v6, p0,
    // attr2.x; v_interp_mov_f32 v7, p0, attr3.w; exp mrt0 v4, v5, v6, v7 done vm; s_endpgm.
    const std::array<std::uint32_t, 8> mixed{0xc8100000u, 0xc8110001u, 0xc8160402u, 0xc81a0802u, 0xc81e0f02u, 0xf800180fu, 0x07060504u, 0xbf810000u};
    // Slot 0 interpolated, slot 0 flat, and defaulted inputs (OFFSET bit 5 without FLAT_SHADE), one
    // keeping offset bits (0x22), one DEFAULT_VAL (1,1,1,1).
    auto pixel = recompilePixel({0x0u, 0x400u, 0x22u, 0x320u}, mixed);
    auto inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 1 && inputs[0].location == 0 && inputs[0].perVertex, "a slot read flat and interpolated did not become one per-vertex input");
    std::array<CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &pixel, 0}}};
    AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
    // Two interpolated inputs of slot 3 (attr0.x, attr1.y) read one smooth variable.
    const std::array<std::uint32_t, 7> shared{0xc8100000u, 0xc8110001u, 0xc8140500u, 0xc8150501u, 0xf800180fu, 0x05040504u, 0xbf810000u};
    // Without fragment barycentrics the interpolation is Vulkan's, by the variable's decoration.
    pixel = recompilePixel({0x3u, 0x3u}, shared, 9u, false);
    inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 1 && inputs[0].location == 3 && !inputs[0].perVertex && !inputs[0].flat, "inputs reading one slot were not declared once at the slot");
    // With them, v_interp_p1/p2 compute P0 + I*P10 + J*P20 from the slot's vertices.
    pixel = recompilePixel({0x3u, 0x3u}, shared);
    inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 1 && inputs[0].location == 3 && inputs[0].perVertex, "inputs interpolated explicitly from one slot were not declared once at the slot, per vertex");
    // Flat inputs of different slots keep their slots.
    pixel = recompilePixel({0x404u, 0x0u}, shared, 9u, false);
    inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 2 && inputs[0].location == 0 && !inputs[0].flat && inputs[1].location == 4 && inputs[1].flat, "flat and interpolated inputs of different slots moved");
    pixel = recompilePixel({0x404u, 0x0u}, shared);
    inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 2 && inputs[0].location == 0 && inputs[1].location == 4, "explicitly interpolated inputs of different slots moved");
    // Only defaulted inputs: no interface at all.
    pixel = recompilePixel({0x20u, 0x2320u}, shared);
    Require(locatedInputs(pixel.spirv.Words()).empty(), "a defaulted input was declared as a parameter");
    AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
    // OFFSET bit 5 with FLAT_SHADE is no default: Astro Bot's 0x423 passes slot 3 through with its
    // three vertices unchanged, which the program reads with v_interp_mov p0, p10 and p20
    // (GetAttributeAtVertex): v_interp_mov v4, p0, attr0.x; v_interp_mov v5, p10, attr0.x;
    // v_interp_mov v6, p20, attr0.x; v_interp_mov v7, p0, attr0.w; exp mrt0 v4, v5, v6, v7 done vm.
    const std::array<std::uint32_t, 7> vertices{0xc8120002u, 0xc8160000u, 0xc81a0001u, 0xc81e0302u, 0xf800180fu, 0x07060504u, 0xbf810000u};
    const auto subtracts = [](std::span<const std::uint32_t> words) {
        std::size_t count = 0;
        for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) count += (words[at] & 0xffffu) == spv::OpFSub;
        return count;
    };
    pixel = recompilePixel({0x423u}, vertices);
    inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 1 && inputs[0].location == 3 && inputs[0].perVertex, "a pass-through input (OFFSET bit 5 with FLAT_SHADE) was not read per vertex at its slot");
    Require(subtracts(pixel.spirv.Words()) == 0, "v_interp_mov p10/p20 of a pass-through input subtracted vertex 0");
    ShaderRecompiler::RecompileResult slotVertex;
    slotVertex.spirv = makeModule({.parameterOutput = true, .parameterLocation = 3});
    Require(AgcDriver::Graphics::UnwrittenFragmentInputs(slotVertex.spirv.Words(), pixel.spirv.Words()).empty(), "a pass-through input does not read the slot the vertex stage exports");
    const std::array<CompiledShader, 2> slotShaders{{{ShaderRecompiler::ShaderStage::Vertex, &slotVertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &pixel, 0}}};
    AgcDriver::Graphics::ValidateShaders(slotShaders, state, subgroup, true);
    // A plain flat input's p10 and p20 are differences to vertex 0.
    pixel = recompilePixel({0x403u}, vertices);
    inputs = locatedInputs(pixel.spirv.Words());
    Require(inputs.size() == 1 && inputs[0].location == 3 && inputs[0].perVertex && subtracts(pixel.spirv.Words()) == 2, "v_interp_mov p10/p20 of a flat input did not read differences to vertex 0");
    expectFailure([&] { recompilePixel({0x423u, 0x3u}, shared, 9u, false); }, "passes its vertices through unchanged");
    expectFailure([&] { recompilePixel({0x423u, 0x3u}, shared); }, "passes its vertices through unchanged");
    // Slot 5 is read but the vertex shader exports only slot 0: the pipeline's pixel module reads
    // zero there, through a private variable.
    pixel = recompilePixel({0x0u, 0x5u}, shared);
    Require(AgcDriver::Graphics::UnwrittenFragmentInputs(vertex.spirv.Words(), pixel.spirv.Words()) == std::set<std::uint32_t>{5u}, "the unexported slot was not found");
    AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
    {
        mock = MockVulkan{};
        auto context = mockContext();
        context.limits.maxColorAttachments = 8;
        AgcDriver::Graphics::ShaderResources resources(context, vertex, pixel, state.color, 0, 0);
        {
            AgcDriver::Graphics::Pipeline pipeline(context, state, AgcDriver::Graphics::VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
            Require(mock.shaderModules.size() == 2 && mock.shaderModules[0] == vertex.spirv.Words(), "the pipeline changed the vertex module");
            inputs = locatedInputs(mock.shaderModules[1]);
            Require(inputs.size() == 1 && inputs[0].location == 0, "the pipeline's pixel module still reads the unexported slot");
        }
    }
    for (const auto version : {0x00010300u, 0x00010400u}) {
        auto words = pixel.spirv.Words();
        words[1] = version;
        const auto zeroed = AgcDriver::Graphics::ZeroFragmentInputs(words, {5u});
        inputs = locatedInputs(zeroed);
        Require(inputs.size() == 1 && inputs[0].location == 0, "the unexported slot is still an input");
#if ANYPS5_ENABLE_SPIRV_TOOLS
        static_cast<void>(ShaderRecompiler::ValidateAndOptimizeSpirv(zeroed, 0x00403000u, version));
#endif
        ShaderRecompiler::RecompileResult linked = pixel;
        linked.spirv = zeroed;
        shaders[1].program = &linked;
        AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
        shaders[1].program = &pixel;
    }
    expectFailure([&] { static_cast<void>(AgcDriver::Graphics::ZeroFragmentInputs(pixel.spirv.Words(), {6u})); }, "not a located input");
    // A per-vertex slot is read through access chains, which become private pointers too.
    pixel = recompilePixel({0x5u, 0x405u, 0x20u, 0x20u}, mixed);
    Require(AgcDriver::Graphics::UnwrittenFragmentInputs(vertex.spirv.Words(), pixel.spirv.Words()) == std::set<std::uint32_t>{5u}, "the unexported per-vertex slot was not found");
    {
        const auto zeroed = AgcDriver::Graphics::ZeroFragmentInputs(pixel.spirv.Words(), {5u});
        Require(locatedInputs(zeroed).empty(), "the unexported per-vertex slot is still an input");
#if ANYPS5_ENABLE_SPIRV_TOOLS
        static_cast<void>(ShaderRecompiler::ValidateAndOptimizeSpirv(zeroed, 0x00403000u, pixel.spirv.Words()[1]));
#endif
        ShaderRecompiler::RecompileResult linked = pixel;
        linked.spirv = zeroed;
        shaders[1].program = &linked;
        AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
        shaders[1].program = &pixel;
    }
    // exp mrt0 v0 vm; exp mrt1 v0 done vm; s_endpgm: MRT1 is outside CB_TARGET_MASK & CB_SHADER_MASK
    // (one attachment), so the color backend drops it.
    const std::array<std::uint32_t, 5> twoTargets{0xf800100fu, 0x00000000u, 0xf800181fu, 0x00000000u, 0xbf810000u};
    pixel = recompilePixel({}, twoTargets, 0x99u);
    vertex.spirv = makeModule({});
    const auto written = AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
    Require(state.colors.size() == 1 && written == std::set<std::uint32_t>{0u}, "an export past the attachments was not dropped");
}

void colorGapPipelineTests() {
    using namespace AgcDriver::Graphics;
    mock = MockVulkan{};
    auto context = mockContext();
    context.limits.maxColorAttachments = 8;
    ShaderRecompiler::RecompileResult vertex;
    vertex.spirv = makeModule({});
    vertex.variantId = 1;
    ShaderRecompiler::RecompileResult fragment;
    fragment.spirv = makeModule({.fragment = true, .secondTarget = true});
    fragment.variantId = 2;
    const std::array<CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
    const auto plain = DecodeState(makeState());
    auto gap = plain;
    gap.blends.insert(gap.blends.begin(), VkPipelineColorBlendAttachmentState{});
    gap.colors[0].exportIndex = 1;
    const VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    Require(ValidateShaders(shaders, plain, subgroup, false) == std::set<std::uint32_t>{0u}, "an export past the attachments was not dropped");
    Require(ValidateShaders(shaders, gap, subgroup, false) == std::set<std::uint32_t>{0u, 1u}, "the export to the attachment after an unused one was dropped");
    {
        ShaderResources resources(context, vertex, fragment, gap.color, 0, 0);
        Pipeline pipeline(context, gap, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        Require(mock.renderPassAttachments.size() == 1 && mock.renderPassColors.size() == 2 && mock.renderPassColors[0].attachment == VK_ATTACHMENT_UNUSED && mock.renderPassColors[1].attachment == 0 && mock.renderPassColors[1].layout == VK_IMAGE_LAYOUT_GENERAL, "the unwritten MRT slot was not an unused attachment of the subpass");
        Require(mock.blendAttachments == 2, "the blend state does not cover every subpass color attachment");
        auto inverted = gap;
        inverted.colors[0].exportIndex = 2;
        expectFailure([&] { Pipeline broken(context, inverted, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL); }, "blend states do not match");
    }
    {
        ShaderResources resources(context, vertex, fragment, plain.color, 0, 0);
        const auto before = mock.pipelineCreateCount;
        const auto first = CachedPipeline(context, plain, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        Require(CachedPipeline(context, plain, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL) == first && mock.pipelineCreateCount == before + 1, "the same color layout did not share its pipeline");
        auto second = gap;
        second.blends[0] = second.blends[1];
        auto shifted = second;
        shifted.colors[0].exportIndex = 0;
        const auto gapped = CachedPipeline(context, second, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        const auto moved = CachedPipeline(context, shifted, VertexInputLayout{}, resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        Require(gapped != first && moved != gapped && mock.pipelineCreateCount == before + 3, "targets bound to different attachments shared a pipeline");
        Require(mock.renderPassColors.size() == 2 && mock.renderPassColors[0].attachment == 0 && mock.renderPassColors[1].attachment == VK_ATTACHMENT_UNUSED, "a trailing unused attachment was not kept");
    }
    ClearCachedPipelines(context.device);
    Require(mock.live == 0, "color gap pipelines leaked Vulkan objects");
}

void depthExportTests() {
    using namespace AgcDriver::Graphics;
    auto queue = makeDepthState();
    queue.context[0x3] = 0;
    queue.context[0x203] = 0x11;
    queue.context[0x1c4] = 1;
    std::vector<RegisterRead> log;
    RegisterReadLog() = &log;
    const auto state = DecodeState(queue);
    Require(DrawRejection(queue, false).empty(), "the precheck rejected a depth-exporting draw");
    auto pixel = DecodePixelStageInfo(queue.context, ExportMappings(state));
    RegisterReadLog() = nullptr;
    for (const auto read : log) Require(DrawKeyCovers(read), "DrawKeyRegisters lacks a register the depth export decode reads: " + std::string(RegisterBankName(read.bank)) + " " + std::to_string(read.offset));
    Require(std::any_of(log.begin(), log.end(), [](RegisterRead read) { return read.bank == RegisterBank::Context && read.offset == 0x3; }), "the depth export decode did not read DB_RENDER_OVERRIDE");
    Require(state.depth.attached && pixel.depthExportEnable && !pixel.earlyZ && pixel.depthExportMin == 0.0f && pixel.depthExportMax == 1.0f, "Z_EXPORT_ENABLE did not decode with the viewport depth clamp");
    queue.context[0xb4] = std::bit_cast<std::uint32_t>(0.25f);
    queue.context[0xb5] = std::bit_cast<std::uint32_t>(0.75f);
    pixel = DecodePixelStageInfo(queue.context, ExportMappings(DecodeState(queue)));
    Require(pixel.depthExportMin == 0.25f && pixel.depthExportMax == 0.75f, "PA_SC_VPORT_ZMIN_0/ZMAX_0 did not become the exported depth clamp");
    queue.context[0x3] = 0x10000;
    expectFailure([&] { static_cast<void>(DecodePixelStageInfo(queue.context, ExportMappings(state))); }, "DISABLE_VIEWPORT_CLAMP");
    queue.context[0x203] = 0x10;
    queue.context[0x1c4] = 0;
    pixel = DecodePixelStageInfo(queue.context, ExportMappings(state));
    Require(!pixel.depthExportEnable && pixel.depthExportMin == 0.0f && pixel.depthExportMax == 1.0f, "a draw without depth export took the viewport depth clamp");
    for (const auto control : {0x3u, 0x5u, 0x101u, 0x2001u}) {
        auto changed = makeDepthState();
        changed.context[0x3] = 0;
        changed.context[0x203] = control;
        changed.context[0x1c4] = 1;
        expectDepthRejection(changed, "depth export, shader coverage or ordered fragment execution");
    }
    for (const auto format : {2u, 3u, 9u}) {
        auto changed = makeDepthState();
        changed.context[0x203] = 0x11;
        changed.context[0x1c4] = format;
        expectDepthRejection(changed, "depth or sample-mask export");
    }
    for (const auto [control, format] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0x10u, 1u}, {0x11u, 0u}}) {
        auto changed = makeDepthState();
        changed.context[0x203] = control;
        changed.context[0x1c4] = format;
        expectDepthRejection(changed, "depth export without its Z export format");
    }
    auto blit = makeDepthState();
    for (const auto [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0x0, 0x60}, {0x200, 0}, {0x202, 0xcc0000}, {0x203, 0x11}, {0x1c4, 1}, {0x1c5, 0}, {0x8e, 0}, {0x8f, 0}}) blit.context[offset] = value;
    blit.context[0x2dc] = 0xaa00;
    Require(!DepthMetadataBlit(blit), "a depth-exporting draw was taken for a DB metadata blit");
    auto exporting = makeDepthState();
    exporting.context[0x3] = 0;
    exporting.context[0x203] = 0x11;
    exporting.context[0x1c4] = 1;
    const auto exportState = DecodeState(exporting);
    const std::array<std::uint32_t, 3> code{0xf8001881u, 0x00000000u, 0xbf810000u};
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = DecodePixelStageInfo(exporting.context, ExportMappings(exportState));
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(request);
    ShaderRecompiler::RecompileResult vertex;
    vertex.spirv = makeModule({});
    const std::array<CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &pixelResult, 0}}};
    ValidateShaders(shaders, exportState, VkPhysicalDeviceSubgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES}, false);
}

void validationTests() {
    AgcDriver::Graphics::State state{};
    state.stages.path = AgcDriver::Graphics::ShaderPath::Vertex;
    ShaderRecompiler::RecompileResult fragment;
    fragment.spirv = makeModule({.fragment = true});
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.parameterOutput = true});
        ShaderRecompiler::RecompileResult pixel;
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &pixel, 0}}};
        const VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        for (const auto noPerspective : {false, true}) {
            pixel.spirv = makeModule({.fragment = true, .barycentric = true, .barycentricNoPerspective = noPerspective, .perVertex = true});
            AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true);
            expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "fragmentShaderBarycentric");
        }
        pixel.spirv = makeModule({.fragment = true, .barycentric = true, .barycentricComponents = 4});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true); }, "invalid barycentric built-in");
        pixel.spirv = makeModule({.fragment = true, .barycentric = true, .perVertex = true, .perVertexLength = 2});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true); }, "three vertices");
        pixel.spirv = makeModule({.fragment = true, .perVertex = true});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true); }, "PerVertexKHR requires");
        vertex.spirv = makeModule({.barycentric = true});
        pixel.spirv = makeModule({.fragment = true});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, true); }, "requires a fragment shader");
        vertex.spirv = makeModule({});
        pixel.spirv = makeModule({.fragment = true, .fragDepth = true, .depthReplacing = true});
        AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false);
        pixel.spirv = makeModule({.fragment = true, .fragDepth = true});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "fragment depth output and DepthReplacing disagree");
        pixel.spirv = makeModule({.fragment = true, .depthReplacing = true});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "fragment depth output and DepthReplacing disagree");
        vertex.spirv = makeModule({.fragDepth = true});
        pixel.spirv = makeModule({.fragment = true});
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "unsupported or duplicate vertex built-in output");
    }
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({});
        ShaderRecompiler::RecompileResult pixel;
        pixel.spirv = makeModule({.fragment = true, .secondTarget = true});
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &pixel, 0}}};
        const VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        Require(state.colors.empty() && AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false) == std::set<std::uint32_t>{0u}, "an export past the attachments was not dropped");
    }
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.vertexInput = true});
        ShaderRecompiler::VertexAttribute attribute{0, 4, {{0x1000, 32u << 16u, 3, 77u << 12u}}, 0};
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
        const VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "missing attribute metadata");
        vertex.vertexAttributes.push_back(attribute);
        AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false);
        vertex.vertexAttributes[0].components = 2;
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "metadata disagrees");
        Require(AgcDriver::Graphics::VertexBufferReadSize(attribute, 2, 1) == 80, "incorrect strided vertex range");
        expectFailure([&] { AgcDriver::Graphics::VertexBufferReadSize(attribute, 3, 1); }, "record count");
        attribute.fetchIndex = 1;
        Require(AgcDriver::Graphics::VertexBufferReadSize(attribute, 100, 2) == 48, "instance attributes used the vertex index");
        Require(AgcDriver::Graphics::VertexBufferReadSize(attribute, 100, 2, 1) == 80, "first instance was ignored");
        expectFailure([&] { AgcDriver::Graphics::VertexBufferReadSize(attribute, 0, 2, 2); }, "record count");
        expectFailure([&] { AgcDriver::Graphics::VertexBufferReadSize(attribute, 0, 2, 0xffffffffu); }, "instance range overflow");
        expectFailure([&] { AgcDriver::Graphics::VertexBufferReadSize(attribute, 0, 4); }, "record count");
        attribute.resource.fields[1] = 0;
        attribute.resource.fields[2] = 16;
        Require(AgcDriver::Graphics::VertexBufferReadSize(attribute, 100, 2) == 16, "zero stride must repeat one value");
        attribute.resource.fields[2] = 8;
        expectFailure([&] { AgcDriver::Graphics::VertexBufferReadSize(attribute, 0, 1); }, "byte range");
        attribute.resource.fields[3] = 113u << 12u;
        expectFailure([&] { AgcDriver::Graphics::DecodeVertexFormat(attribute); }, "unsupported vertex format");
        attribute.resource.fields[3] = 50u << 12u;
        Require(AgcDriver::Graphics::DecodeVertexFormat(attribute).format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && AgcDriver::Graphics::DecodeVertexFormat(attribute).bytes == 4, "2_10_10_10 vertex format was not decoded");
        attribute.resource.fields[3] = 55u << 12u;
        Require(AgcDriver::Graphics::DecodeVertexFormat(attribute).format == VK_FORMAT_A2B10G10R10_SINT_PACK32 && std::string_view(AgcDriver::Graphics::DecodeVertexFormat(attribute).scalar) == "i32", "2_10_10_10 sint vertex format was not decoded");
        attribute.resource.fields[3] = 36u << 12u;
        Require(AgcDriver::Graphics::DecodeVertexFormat(attribute).format == VK_FORMAT_B10G11R11_UFLOAT_PACK32, "10_11_11 float vertex format was not decoded");
        attribute.resource.fields[3] = 43u << 12u;
        expectFailure([&] { AgcDriver::Graphics::DecodeVertexFormat(attribute); }, "unsupported vertex format");
        attribute.resource.fields = {0x1000, 32u << 16u, 3, 77u << 12u};
        attribute.components = 2;
        AgcDriver::Graphics::Context context{};
        context.limits.maxVertexInputBindings = 16;
        context.limits.maxVertexInputAttributes = 16;
        context.limits.maxVertexInputBindingStride = 2048;
        context.formatProperties = [](VkPhysicalDevice, VkFormat format, VkFormatProperties* properties) {
            *properties = {};
            if (format == VK_FORMAT_R32G32_SFLOAT) properties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        };
        const auto layout = AgcDriver::Graphics::BuildVertexInputLayout(context, std::span(&attribute, 1));
        Require(layout.bindings.size() == 1 && layout.bindings[0].stride == 32 && layout.bindings[0].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE, "incorrect instance input binding");
        Require(layout.attributes[0].format == VK_FORMAT_R32G32_SFLOAT && layout.attributes[0].offset == 0 && layout.attributes[0].location == 0, "incorrect vertex attribute format or offset");
        attribute.components = 4;
        expectFailure([&] { AgcDriver::Graphics::BuildVertexInputLayout(context, std::span(&attribute, 1)); }, "device does not support vertex format");
        attribute.components = 2;
        attribute.resource.fields[1] |= 0x80000000u;
        expectFailure([&] { AgcDriver::Graphics::BuildVertexInputLayout(context, std::span(&attribute, 1)); }, "descriptor flags");
    }
    for (const auto capability : {spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle}) {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({});
        vertex.spirv.insert(vertex.spirv.begin() + 5, {(2u << 16u) | spv::OpCapability, static_cast<std::uint32_t>(capability)});
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        subgroup.supportedStages = VK_SHADER_STAGE_VERTEX_BIT;
        subgroup.supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
        AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false);
        subgroup.supportedStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "unsupported for shader stage");
        subgroup.supportedStages = VK_SHADER_STAGE_VERTEX_BIT;
        subgroup.supportedOperations = capability == spv::CapabilityGroupNonUniform ? 0u : VK_SUBGROUP_FEATURE_BASIC_BIT;
        expectFailure([&] { AgcDriver::Graphics::ValidateShaders(shaders, state, subgroup, false); }, "device lacks operations");
    }
    const std::vector<std::uint32_t> words(8, 0);
    const auto validate = [&](const ShaderRecompiler::RecompileResult& vertex, std::uint32_t fragmentOffset) {
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, fragmentOffset}}};
        AgcDriver::Graphics::ValidateShaders(shaders, state, VkPhysicalDeviceSubgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES}, false);
    };
    const auto pushed = [&](const ModuleShape& shape) {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule(shape);
        vertex.pushConstants.assign(8, std::byte{1});
        vertex.bindings.push_back(makeBinding(Role::GuestBuffers, 0, 2, words));
        return vertex;
    };
    validate(pushed({.push = true, .bufferArray = 2}), 8);
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.shaderData = true});
        vertex.bindings.push_back(makeBinding(Role::ShaderData, 5, 1, {1, 2}));
        validate(vertex, 0);
    }
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.bufferArray = 1, .shaderData = true});
        vertex.bindings.push_back(makeBinding(Role::GuestBuffers, 0, 1, {1, 2, 3, 4}));
        vertex.bindings.push_back(makeBinding(Role::ShaderData, 5, 1, {1, 2}));
        validate(vertex, 0);
    }
    expectFailure([&] { validate(pushed({.push = true, .pushLength = 16, .bufferArray = 2}), 8); }, "32 elements");
    expectFailure([&] { validate(pushed({.push = true, .pushStride = 8, .bufferArray = 2}), 8); }, "ArrayStride of 4");
    expectFailure([&] { validate(pushed({.push = false, .bufferArray = 2}), 8); }, "push constant metadata disagrees with SPIR-V");
    {
        auto vertex = pushed({.push = true, .bufferArray = 2});
        vertex.pushConstants.clear();
        expectFailure([&] { validate(vertex, 8); }, "invalid push constant interface");
    }
    {
        auto vertex = pushed({.push = true, .bufferArray = 3});
        expectFailure([&] { validate(vertex, 8); }, "descriptor array length disagrees");
    }
    {
        auto vertex = pushed({.push = true, .plainBuffer = true});
        expectFailure([&] { validate(vertex, 8); }, "must be declared as a descriptor array");
    }
    {
        auto vertex = pushed({.push = true, .bufferArray = 2});
        vertex.bindings.front().readOnly = true;
        expectFailure([&] { validate(vertex, 8); }, "read-only descriptor metadata is unsupported");
    }
    {
        auto vertex = pushed({.push = true, .bufferArray = 2});
        vertex.bindings.front().descriptorSet = 1;
        expectFailure([&] { validate(vertex, 8); }, "descriptor set other than zero");
    }
    {
        auto vertex = pushed({.push = true, .bufferArray = 2});
        vertex.bindings.front().kind = Kind::UniformBuffer;
        expectFailure([&] { validate(vertex, 8); }, "disagrees with recompiler binding metadata");
    }
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.shaderData = true});
        vertex.bindings.push_back(makeBinding(Role::ShaderData, 5, 2, {1, 2}));
        expectFailure([&] { validate(vertex, 0); }, "binding count of one");
    }
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.shaderData = true});
        vertex.bindings.push_back(makeBinding(Role::GuestSamplers, 5, 1, {1, 2}));
        expectFailure([&] { validate(vertex, 0); }, "descriptor role is unsupported");
    }
    {
        ShaderRecompiler::RecompileResult vertex;
        vertex.spirv = makeModule({.shaderData = true});
        expectFailure([&] { validate(vertex, 0); }, "absent from recompiler binding metadata");
    }
}


}

void vertexCopyTests() {
    using AgcDriver::Graphics::PlanVertexCopies;
    using AgcDriver::Graphics::VertexFetch;
    {
        const std::array<VertexFetch, 3> fetches{{{0x1018, 0x1018 + 32 * 9 + 8, 32, 0, 4}, {0x1000, 0x1000 + 32 * 9 + 12, 32, 0, 4}, {0x100c, 0x100c + 32 * 9 + 12, 32, 0, 4}}};
        const auto plan = PlanVertexCopies(fetches);
        Require(plan.copies.size() == 1 && plan.copies[0].first == 0x1000 && plan.copies[0].second == 0x1018 + 32 * 9 + 8, "interleaved attributes were not copied as one union");
        Require(plan.copyOf == std::vector<std::size_t>{0, 0, 0} && plan.offsets == std::vector<std::uint64_t>{0x18, 0, 0xc}, "interleaved attribute offsets are wrong");
    }
    {
        const std::array<VertexFetch, 6> fetches{{
            {0x2000, 0x2100, 32, 0, 4},
            {0x2004, 0x2100, 16, 0, 4},
            {0x2020, 0x2120, 32, 0, 4},
            {0x2002, 0x2102, 32, 0, 4},
            {0x2008, 0x2108, 32, 1, 4},
            {0x2000, 0x2010, 0, 0, 4},
        }};
        const auto plan = PlanVertexCopies(fetches);
        Require(plan.copies.size() == 6, "fetches of other records, strides, rates or alignments shared a copy");
        for (std::size_t i = 0; i < fetches.size(); ++i) {
            const auto& copy = plan.copies[plan.copyOf[i]];
            Require(plan.offsets[i] == 0 && copy.first == fetches[i].begin && copy.second == fetches[i].end, "a lone fetch was not copied exactly");
        }
    }
    {
        const std::array<VertexFetch, 2> fetches{{{0x3000, 0x3100, 24, 0, 2}, {0x3002, 0x3102, 24, 0, 2}}};
        const auto plan = PlanVertexCopies(fetches);
        Require(plan.copies.size() == 1 && plan.copies[0].second == 0x3102 && plan.offsets[1] == 2, "aligned 16-bit attributes of one record were not merged");
    }
    {
        const std::array<VertexFetch, 1> empty{{{0x4000, 0x4000, 16, 0, 4}}};
        expectFailure([&] { PlanVertexCopies(empty); }, "empty vertex fetch");
    }
}

bool recompilesDebugBranch(std::uint32_t opcode) {
    auto queue = makeState();
    queue.context[0x1b3] = 0x2u;
    queue.context[0x1b4] = 0x2u;
    const auto state = AgcDriver::Graphics::DecodeState(queue);
    const auto pixel = AgcDriver::Graphics::DecodePixelStageInfo(queue.context, AgcDriver::Graphics::ExportMappings(state));
    const std::array<std::uint32_t, 4> code{0xbf800000u | (opcode << 16u) | 1u, 0xf800180fu, 0x00000000u, 0xbf810000u};
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Fragment, 0x30000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.pixel = pixel;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = true;
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return !ShaderRecompiler::Recompile(request).spirv.Words().empty();
}

void parallelCompareTests() {
    using AgcDriver::Graphics::CompareSpan;
    using AgcDriver::Graphics::CompareSpansFrom;
    constexpr std::size_t block = 65536;
    constexpr std::size_t count = 48;
    std::vector<std::byte> first(block * count + 100, std::byte{7});
    auto second = first;
    const std::array<std::size_t, 4> changedAt{3 * block + 17, 20 * block, 33 * block + block - 1, 47 * block + 120};
    for (const auto at : changedAt) second[at] = std::byte{9};
    std::vector<CompareSpan> spans;
    for (std::size_t k = 0; k < count; ++k) {
        const auto length = k + 1 == count ? block + 100 : block;
        spans.push_back({first.data() + k * block, second.data() + k * block, length});
    }
    const auto expected = [&](std::size_t k) {
        return std::none_of(changedAt.begin(), changedAt.end(), [&](std::size_t at) { return at >= k * block && at < k * block + spans[k].bytes; });
    };
    for (const std::size_t threshold : {std::size_t{0}, std::size_t{1} << 40u}) {
        for (int round = 0; round < 20; ++round) {
            std::vector<std::uint8_t> equal(count, 2);
            CompareSpansFrom(spans, equal, threshold);
            for (std::size_t k = 0; k < count; ++k) Require((equal[k] == 1) == expected(k) && equal[k] <= 1, "parallel compare misreported a span");
        }
    }
    std::vector<std::uint8_t> left(count, 2), right(count, 2);
    std::thread other([&] {
        for (int round = 0; round < 50; ++round) CompareSpansFrom(spans, right, 0);
    });
    for (int round = 0; round < 50; ++round) CompareSpansFrom(spans, left, 0);
    other.join();
    for (std::size_t k = 0; k < count; ++k) Require((left[k] == 1) == expected(k) && (right[k] == 1) == expected(k), "concurrent parallel compares misreported a span");
    std::vector<std::uint8_t> none;
    CompareSpansFrom({}, none, 0);
}

void meshArgumentTests() {
    using AgcDriver::Graphics::MeshArguments;
    using AgcDriver::Graphics::ResolveMeshArguments;
    AgcDriver::Graphics::Context context{};
    context.meshLimits.maxMeshWorkGroupCount[0] = 1000;
    context.meshLimits.maxMeshWorkGroupCount[1] = 600;
    context.meshLimits.maxMeshWorkGroupTotalCount = 4000;
    const ShaderRecompiler::MeshConfiguration points{1u, 1u, 1u, 1u, 1u, 64u, 1024u, 0u, 4u};
    const ShaderRecompiler::MeshConfiguration triangles{4u, 32u, 96u, 96u, 32u, 128u, 2048u, 0u, 4u};
    const ShaderRecompiler::MeshConfiguration strip{6u, 8u, 10u, 10u, 8u, 64u, 1024u, 0u, 4u};
    const ShaderRecompiler::MeshConfiguration fan{5u, 30u, 32u, 256u, 192u, 256u, 1024u, 0u, 4u};
    const auto same = [](const MeshArguments& a, const MeshArguments& b) { return a.groups == b.groups && a.instances == b.instances && a.layers == b.layers && a.indexCount == b.indexCount && a.firstIndex == b.firstIndex; };
    const auto rules = [&](const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount) { return AgcDriver::Graphics::MeshArgumentRulesFor(context, mesh, indexCount); };
    const auto record = [](std::uint32_t count, std::uint32_t instances, std::uint32_t first) { return AgcDriver::Pm4::DrawArguments{count, instances, first, 0, 0}; };
    Require(same(ResolveMeshArguments(record(1, 512, 0), rules(points, 1)), {1, 512, 1, 1, 0}), "one point, 512 instances");
    Require(same(ResolveMeshArguments(record(1, 0, 0), rules(points, 1)), {0, 0, 0, 1, 0}), "no instances draws nothing");
    Require(same(ResolveMeshArguments(record(0, 4, 0), rules(points, 1)), {0, 0, 0, 0, 0}), "no indices draws nothing");
    Require(same(ResolveMeshArguments(record(96, 2, 0), rules(triangles, 300)), {1, 2, 1, 96, 0}), "one full group");
    Require(same(ResolveMeshArguments(record(99, 2, 0), rules(triangles, 300)), {2, 2, 1, 99, 0}), "a partial second group");
    Require(same(ResolveMeshArguments(record(200, 3, 150), rules(triangles, 300)), {2, 3, 1, 150, 150}), "count clamped to the index buffer");
    Require(same(ResolveMeshArguments(record(9, 1, 300), rules(triangles, 300)), {0, 0, 0, 0, 300}), "first index past the index buffer draws nothing");
    Require(same(ResolveMeshArguments(record(2, 1, 0), rules(triangles, 300)), {0, 0, 0, 2, 0}), "no complete triangle draws nothing");
    Require(same(ResolveMeshArguments(record(10, 1, 0), rules(strip, 64)), {1, 1, 1, 10, 0}), "eight strip triangles are one group");
    Require(same(ResolveMeshArguments(record(11, 1, 0), rules(strip, 64)), {2, 1, 1, 11, 0}), "a ninth strip triangle starts a group");
    Require(same(ResolveMeshArguments(record(6, 1, 0), rules(fan, 6)), {1, 1, 1, 6, 0}), "a four-triangle fan is one group");
    Require(same(ResolveMeshArguments(record(32, 1, 0), rules(fan, 64)), {1, 1, 1, 32, 0}), "thirty fan triangles are one group");
    Require(same(ResolveMeshArguments(record(33, 1, 0), rules(fan, 64)), {2, 1, 1, 33, 0}), "a thirty-first fan triangle starts a group");
    Require(same(ResolveMeshArguments(record(2, 1, 0), rules(fan, 64)), {0, 0, 0, 2, 0}), "a fan without a triangle draws nothing");
    Require(same(ResolveMeshArguments(record(1, 601, 0), rules(points, 1)), {0, 0, 0, 1, 0}), "instances over the device limit draw nothing");
    Require(same(ResolveMeshArguments(record(96 * 7, 600, 0), rules(triangles, 96 * 7)), {0, 0, 0, 96 * 7, 0}), "groups times instances over the device limit draw nothing");
    Require(same(ResolveMeshArguments(record(96 * 6, 600, 0), rules(triangles, 96 * 6)), {6, 600, 1, 96 * 6, 0}), "groups times instances at the device limit");
    Require(same(ResolveMeshArguments(record(0xffffffffu, 1, 0xfffffff0u), rules(points, 0xffffffffu)), {15, 1, 1, 15, 0xfffffff0u}), "first index near the end of a huge index buffer");
    Require(same(ResolveMeshArguments(record(0xffffffffu, 1, 0), rules(points, 0xffffffffu)), {0, 0, 0, 0xffffffffu, 0}), "groups over the device limit draw nothing");
}

void debugBranchTests() {
    for (const auto opcode : {0x17u, 0x18u, 0x19u, 0x1au}) Require(recompilesDebugBranch(opcode), "a conditional debug branch did not recompile");
}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIN_WAIT_MS", "200");
#else
    setenv("APS5_PIN_WAIT_MS", "200", 1);
#endif
    try {
        {
            const AgcDriver::Graphics::Context context{};
            const AgcDriver::Graphics::State state{};
            AgcDriver::Pm4::DrawParameters draw{0, 0, 0, 1, 0, false};
            AgcDriver::Graphics::Draw(context, state, draw, {});
            draw.indexCount = 3;
            draw.instanceCount = 0;
            AgcDriver::Graphics::Draw(context, state, draw, {});
            draw.instanceCount = 2;
            draw.firstInstance = 0xffffffffu;
            expectFailure([&] { AgcDriver::Graphics::Draw(context, state, draw, {}); }, "instance range overflow");
            draw.firstInstance = 0;
            draw.firstVertex = 0xffffffffu;
            expectFailure([&] { AgcDriver::Graphics::Draw(context, state, draw, {}); }, "vertex range overflow");
            draw.firstVertex = 0;
            draw.indexAddress = 1;
            expectFailure([&] { AgcDriver::Graphics::Draw(context, state, draw, {}); }, "must not reference an index buffer");
            draw.indexAddress = 0;
            draw.flags = 1;
            expectFailure([&] { AgcDriver::Graphics::Draw(context, state, draw, {}); }, "draw modifiers");
        }
        stateTests();
        depthTests();
        depthSurfaceTexelTests();
        hostImportBudgetTests();
        hostImportRefusalTests();
        hardwareScreenOffsetTests();
        DepthClipTests();
        ColorViewTests();
        DisabledColorTests();
        metadataPassTests();
        depthMetadataBlitTests();
        depthExportTests();
        mimgDecodeTests();
        ShaderStageTests();
        PixelInputLayoutTests();
        InitialContextTests();
        pushConstantTests();
        resourceTests();
        misalignedShaderDataTests();
        debugBranchTests();
        meshArgumentTests();
        parallelCompareTests();
        validationTests();
        vertexCopyTests();
        pixelParameterSlotTests();
        rectListTests();
        depthPipelineTests();
        colorGapPipelineTests();
        mock = MockVulkan{};
        auto bdaContext = mockContext();
        bdaContext.bufferDeviceAddress = true;
        bdaContext.limits.maxStorageBufferRange = 1u << 27;
        RunBdaResourceTests(bdaContext, {
            [](VkBuffer buffer) -> std::span<std::byte> { return mockBufferMemory(buffer); },
            [](std::uint32_t binding) {
                for (auto it = mock.writes.rbegin(); it != mock.writes.rend(); ++it) {
                    if (it->binding == binding) return it->buffers.at(0);
                }
                throw std::runtime_error("missing BDA test descriptor");
            },
            [](VkDeviceAddress address) {
                const auto offset = address - 0x100000000000ULL;
                const auto buffer = reinterpret_cast<VkBuffer>(offset / 0x10000);
                return mockBufferMemory(buffer).subspan(offset % 0x10000);
            }
        });
        Require(mock.live == 0, "BDA resources leaked Vulkan objects");
        mock = MockVulkan{};
        bufferPoolTests();
        Require(mock.live == 0, "the buffer pool leaked Vulkan objects");
        mock = MockVulkan{};
        bufferSlabTests();
        mock = MockVulkan{};
        descriptorRecycleTests();
        Require(mock.live == 0, "the descriptor cache leaked Vulkan objects");
        RunGuestAllocationTests();
        RunLiveStackAccessTests();
        RunUnwatchedGapTests();
        RunColorTargetLayoutTests();
        RunTextureFormatTests();
        RunTextureTilingTests();
        RunGuestTextureResourceTests();
        RunGuestSamplerResourceTests();
        mock = MockVulkan{};
        auto textureDetilerContext = mockContext();
        textureDetilerContext.limits.minStorageBufferOffsetAlignment = 16;
        textureDetilerContext.limits.maxStorageBufferRange = 256;
        RunTextureDetilerTests(textureDetilerContext, {
            [](std::uint64_t size) {
                VkBuffer buffer{};
                VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bufferInfo.size = size;
                bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                mockCreateBuffer(VK_NULL_HANDLE, &bufferInfo, nullptr, &buffer);
                VkMemoryRequirements requirements{};
                mockGetBufferMemoryRequirements(VK_NULL_HANDLE, buffer, &requirements);
                VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocationInfo.allocationSize = requirements.size;
                VkDeviceMemory memory{};
                mockAllocateMemory(VK_NULL_HANDLE, &allocationInfo, nullptr, &memory);
                mockBindBufferMemory(VK_NULL_HANDLE, buffer, memory, 0);
                return buffer;
            },
            [](VkBuffer buffer) -> std::vector<std::byte>& { return mock.memories.at(mock.bufferMemory.at(buffer)); },
            [] {
                Require(mock.writes.size() >= 2, "expected texture detiling descriptor writes");
                const auto& destinationWrite = mock.writes.back();
                const auto& sourceWrite = mock.writes[mock.writes.size() - 2];
                DetilerCapture capture{};
                capture.groupsX = mock.lastDispatchGroups.x;
                capture.groupsY = mock.lastDispatchGroups.y;
                capture.groupsZ = mock.lastDispatchGroups.z;
                capture.pushConstants = mock.lastPushConstants;
                capture.sourceBuffer = sourceWrite.buffers.at(0).buffer;
                capture.sourceOffset = sourceWrite.buffers.at(0).offset;
                capture.sourceRange = sourceWrite.buffers.at(0).range;
                capture.destinationBuffer = destinationWrite.buffers.at(0).buffer;
                capture.destinationOffset = destinationWrite.buffers.at(0).offset;
                capture.destinationRange = destinationWrite.buffers.at(0).range;
                return capture;
            },
            [] { return mock.pipelineCreateCount; },
            [] { return mock.pipelineSpecializations.back(); }
        });
        std::cout << "Graphics validation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
