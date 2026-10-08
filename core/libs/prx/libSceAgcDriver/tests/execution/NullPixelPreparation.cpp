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

}

int main() {
    try {
        const auto registered = AgcDriver::DriverDetail::NullPixelRegisteredState();
        const auto prepared = AgcDriver::Graphics::DecodePixelStageInfo(registered.context, {});
        const std::array<std::uint8_t, 8> mappings{0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u, 0xe4u};
        const auto drawn = AgcDriver::Graphics::DecodePixelStageInfo(AgcDriver::InitialContextRegisters(), mappings, true);
        if (Key(prepared) != Key(drawn)) throw std::runtime_error("the registered null pixel program does not match a draw without a pixel program");
        std::cout << "null pixel preparation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
