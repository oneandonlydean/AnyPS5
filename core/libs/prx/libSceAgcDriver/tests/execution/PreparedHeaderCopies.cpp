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
    throw std::runtime_error("expected ABI resolution failure");
}

void Run() {
    alignas(256) static std::array<std::uint32_t, 1> code{0xbf810000u};
    static struct Header {
        Shader shader{};
        std::array<ShaderRegister, 7> registers{};
        ShaderSpecialRegs specials{};
    } header;
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18;
    header.shader.header_size = sizeof(header);
    header.shader.shader_size = sizeof(code);
    header.shader.code = code.data();
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = header.registers.size();
    header.shader.specials = &header.specials;
    header.specials.dispatch_modifier = 0x8000;
    header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x207, 1}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 0}}};
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    Shader copy = header.shader;
    AgcDriverResolveShaderAbi_nid_postfix(&copy, {}, {});
    static ShaderUserData userData{};
    copy.user_data = &userData;
    ExpectFailure([&] { AgcDriverResolveShaderAbi_nid_postfix(&copy, {}, {}); }, "static ABI refers to a replaced shader header");
    copy.user_data = nullptr;
    std::array<ShaderRegister, 7> otherRegisters = header.registers;
    copy.sh_registers = otherRegisters.data();
    ExpectFailure([&] { AgcDriverResolveShaderAbi_nid_postfix(&copy, {}, {}); }, "static ABI refers to a replaced shader header");
    AgcDriverShutdown_nid_postfix();
}

}

int main() {
    try {
        if (!OpenVulkanTestDevice()) return VulkanTestSkipped;
        Run();
        std::cout << "shader header copy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
