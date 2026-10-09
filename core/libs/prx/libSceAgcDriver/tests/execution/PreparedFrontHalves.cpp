#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void ExpectFailure(TAction action, const char* expected) {
    try { action(); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("expected registration failure");
}

struct Header {
    Shader shader{};
    std::array<ShaderRegister, 4> registers{};
};

void Initialize(Header& header, const std::array<std::uint32_t, 64>& code, std::uint8_t type, std::uint32_t registerCount) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18;
    header.shader.header_size = sizeof(header);
    header.shader.shader_size = sizeof(code);
    header.shader.code = code.data();
    header.shader.type = type;
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = registerCount;
}

void Run() {
    alignas(256) static std::array<std::uint32_t, 64> geometryCode{0xbf810000u};
    alignas(256) static std::array<std::uint32_t, 64> hullCode{0xbf810000u};
    Header geometry;
    Initialize(geometry, geometryCode, 4, 2);
    geometry.registers[0] = {0x8a, 0};
    geometry.registers[1] = {0x8b, 0};
    AgcDriverRegisterShader_nid_postfix(&geometry.shader);
    Header hull;
    Initialize(hull, hullCode, 5, 2);
    hull.registers[0] = {0x14a, 0};
    hull.registers[1] = {0x14b, 0};
    AgcDriverRegisterShader_nid_postfix(&hull.shader);
    alignas(256) static std::array<std::uint32_t, 64> partialCode{0xbf810000u};
    Header partial;
    Initialize(partial, partialCode, 4, 3);
    const auto address = reinterpret_cast<std::uintptr_t>(partialCode.data());
    partial.registers[0] = {0xc8, static_cast<std::uint32_t>(address >> 8u)};
    partial.registers[1] = {0x8a, 0};
    partial.registers[2] = {0x8b, 0};
    ExpectFailure([&] { AgcDriverRegisterShader_nid_postfix(&partial.shader); }, "missing static shader ABI register 201");
    AgcDriverShutdown_nid_postfix();
}

}

int main() {
    try {
        if (!OpenVulkanTestDevice()) return VulkanTestSkipped;
        Run();
        std::cout << "front half registration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
