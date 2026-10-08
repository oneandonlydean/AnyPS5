#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

std::vector<std::uint64_t> Key(const ShaderRecompiler::ShaderPixelStageInfo& pixel) {
    ShaderRecompiler::RecompileRequest request{};
    request.shader.stage = ShaderRecompiler::ShaderStage::Fragment;
    request.context.waveSize = pixel.wave32 ? 32u : 64u;
    request.context.pixel = pixel;
    std::vector<std::uint64_t> key;
    ShaderRecompiler::BuildPreparedShaderKey(request, key);
    return key;
}

void CheckSkippedPixelUserData() {
    using namespace AgcDriver::DriverDetail;
    alignas(256) static std::array<std::uint32_t, 64> vertexCode{0xbf810000u};
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(vertexCode.data());
    auto vertex = std::make_shared<ShaderSnapshot>(ShaderSnapshot{vertexAddress, 0, 2, {vertexCode.begin(), vertexCode.end()}, {}});
    auto null = std::make_shared<ShaderSnapshot>(ShaderSnapshot{NullPixelProgramAddress(), 0, 1, {0xbf810000u}, {}});
    ShaderRegistry registry{{vertexAddress, vertex}, {NullPixelProgramAddress(), null}};
    AgcDriver::QueueState queue{};
    queue.shader[0xc8] = static_cast<std::uint32_t>(vertexAddress >> 8u);
    queue.shader[0xc9] = static_cast<std::uint32_t>(vertexAddress >> 40u);
    queue.shader[0x8b] = 0;
    queue.shader[0x008] = 0;
    queue.shader[0x009] = 0;
    queue.shader[0x00b] = 16u << 1u;
    queue.context[0x8e] = 0;
    queue.context[0x8f] = 0;
    DrawDecode decoded{};
    decoded.state.stages.path = AgcDriver::Graphics::ShaderPath::Vertex;
    DecodeGraphicsPrograms(decoded, queue, registry, false, true);
    if (decoded.programs.size() != 2 || decoded.programs.back().snapshot != null) throw std::runtime_error("a draw without a pixel program did not decode the null pixel program");
    if (!decoded.programs.back().userData.empty()) throw std::runtime_error("the null pixel program took user data from a stale SPI_SHADER_PGM_RSRC2_PS");
}

}

int main() {
    try {
        const auto registered = AgcDriver::DriverDetail::NullPixelRegisteredState();
        const auto prepared = AgcDriver::Graphics::DecodePixelStageInfo(registered.context, {});
        const std::array<std::uint8_t, 8> mappings{0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u};
        const auto drawn = AgcDriver::Graphics::DecodePixelStageInfo(AgcDriver::InitialContextRegisters(), mappings, true);
        if (Key(prepared) != Key(drawn)) throw std::runtime_error("the registered null pixel program does not match a draw without a pixel program");
        CheckSkippedPixelUserData();
        std::cout << "null pixel preparation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
