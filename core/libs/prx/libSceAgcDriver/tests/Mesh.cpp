#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "execution/VulkanTestDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 192;
constexpr std::uint32_t Height = 128;
constexpr std::array<std::uint8_t, 4> Background{16, 24, 40, 255};
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

alignas(256) constexpr std::array<std::uint32_t, 104> GeometryCode{
    0x8f6a9003, 0x94fe6ac1, 0xbf88000b, 0xd7650006, 0x000100c1, 0xd7660006, 0x00020cc1, 0x93ebff03,
    0x00040018, 0xd7460006, 0x04190c6b, 0x340c0c82, 0xd8340000, 0x00000506, 0xbf8cc07f, 0xbefe04c1,
    0xbf8a0000, 0x938dff02, 0x00090016, 0x9382ff03, 0x00040018, 0x938cff03, 0x00080008, 0xd7650009,
    0x000100c1, 0xd7660009, 0x000212c1, 0xd746000a, 0x04250c02, 0x7da8120c, 0xbf88002e, 0x361600ff,
    0x0000ffff, 0x2c180090, 0x361a02ff, 0x0000ffff, 0xd8d80000, 0x0b00000b, 0xd8d80000, 0x0c00000c,
    0xd8d80000, 0x0d00000d, 0xbf8cc07f, 0xe0382000, 0x8002100b, 0xe0382010, 0x8002140b, 0xe0382000,
    0x8002180c, 0xe0382010, 0x80021c0c, 0xe0382000, 0x8002200d, 0xe0382010, 0x8002240d, 0x161c1483,
    0x161e14ff, 0x00000060, 0xbf8c3f70, 0xdb7c0400, 0x0000100f, 0xdb7c0410, 0x0000140f, 0xdb7c0420,
    0x0000180f, 0xdb7c0430, 0x00001c0f, 0xdb7c0440, 0x0000200f, 0xdb7c0450, 0x0000240f, 0x4a501c81,
    0x4a521c82, 0x3450508a, 0x34525294, 0xd772002a, 0x04a6510e, 0xbf8cc07f, 0xbefe04c1, 0xbf8a0000,
    0x930e830d, 0xbf078002, 0xbf850003, 0x8f0f8c0d, 0x887c0f0e, 0xbf900009, 0x7da8140d, 0xbf880002,
    0xf8000941, 0x0000002a, 0xbefe04c1, 0x7da8140e, 0xbf88000a, 0x34561485, 0xdbfc0400, 0x2c00002b,
    0xdbfc0410, 0x3000002b, 0xbf8cc07f, 0xf80008cf, 0x2f2e2d2c, 0xf800020f, 0x33323130, 0xbf810000,
};

alignas(256) constexpr auto Wave32GeometryCode = [] {
    auto code = GeometryCode;
    code[10] = 0x04190a6bu;
    code[28] = 0x04250a02u;
    return code;
}();

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 28> PassthroughCode{
    0x938cff02, 0x0009000c, 0x938dff02, 0x00090016, 0x8f0e8c0d, 0x887c0e0c, 0xbf900009, 0xd7650009,
    0x000100c1, 0xd7660009, 0x000212c1, 0x7da8120d, 0xbf880002, 0xf8000941, 0x00000000, 0xbefe04c1,
    0x7da8120c, 0xbf880009, 0xe0382000, 0x80021005, 0xe0382010, 0x80021405, 0xbf8c3f70, 0xf80008cf,
    0x13121110, 0xf800020f, 0x17161514, 0xbf810000,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

constexpr std::uint32_t Columns = 8;
constexpr std::uint32_t Rows = 5;
constexpr std::uint32_t Triangles = Columns * Rows;

std::array<std::uint8_t, 4> TriangleColor(std::uint32_t triangle) {
    return {static_cast<std::uint8_t>(40u + 29u * triangle), static_cast<std::uint8_t>(200u + 53u * triangle), static_cast<std::uint8_t>(triangle % 2u == 0u ? 60u : 200u), 255};
}

std::array<Vertex, 3> TriangleVertices(std::uint32_t triangle) {
    const float cellWidth = 2.0f / Columns;
    const float cellHeight = 2.0f / Rows;
    const float x = -1.0f + cellWidth * static_cast<float>(triangle % Columns);
    const float y = -1.0f + cellHeight * static_cast<float>(triangle / Columns);
    const auto color = TriangleColor(triangle);
    const std::array<float, 4> rgba{color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f};
    return {{
        {{x + 0.1f * cellWidth, y + 0.1f * cellHeight, 0.5f, 1.0f}, rgba},
        {{x + 0.9f * cellWidth, y + 0.1f * cellHeight, 0.5f, 1.0f}, rgba},
        {{x + 0.5f * cellWidth, y + 0.9f * cellHeight, 0.5f, 1.0f}, rgba},
    }};
}

std::size_t CellPixel(std::uint32_t triangle) {
    const auto column = triangle % Columns;
    const auto row = triangle / Columns;
    const auto x = (column * Width + Width / 2u) / Columns;
    const auto y = Height - 1u - (row * Height + (Height * 4u) / 10u) / Rows;
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

alignas(256) std::array<Vertex, 3 * Triangles> Scrambled{};
alignas(256) std::array<std::uint16_t, 3 * Triangles> Indices{};
alignas(256) std::array<Vertex, 3 * Triangles> Ordered{};
alignas(256) std::array<Vertex, 2 * Columns + 2> Strip{};

constexpr std::uint32_t GridColumns = 12;
constexpr std::uint32_t GridRows = 8;
constexpr std::uint32_t GridVertices = (GridColumns + 1) * (GridRows + 1);
constexpr std::uint32_t BigBase = 70000;
alignas(256) std::array<Vertex, GridVertices> Grid{};
alignas(256) std::array<Vertex, BigBase + GridVertices> BigVertices{};
std::vector<std::uint32_t> GridList;
alignas(256) std::array<std::uint16_t, 2048> GridIndices16{};
alignas(256) std::array<std::uint32_t, 2048> GridIndices32{};
alignas(256) std::array<std::uint32_t, 2048> BigIndices32{};
alignas(256) std::array<std::uint16_t, 256> GridStrip16{};

constexpr std::uint32_t FanRim = 8;
constexpr float FanStep = 6.28318530718f / FanRim;
alignas(256) std::array<Vertex, FanRim + 1> Fan{};
alignas(256) std::array<Vertex, FanRim + 1> FanScrambled{};
alignas(256) std::array<std::uint16_t, FanRim + 1> FanIndices{};
alignas(256) std::array<std::uint16_t, FanRim + 1> FanRestart{};

std::array<std::uint32_t, 4> VertexBufferDescriptor(const void* vertices, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(vertices);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (32u << 16u), count, 0x01016facu};
}

void ClearPixels() {
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) Pixels[i + c] = std::byte{Background[c]};
    }
}

bool PixelIs(std::size_t offset, const std::array<std::uint8_t, 4>& color) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (std::to_integer<std::uint8_t>(Pixels[offset + c]) != color[c]) return false;
    }
    return true;
}

std::string PixelText(std::size_t offset) {
    char text[64];
    std::snprintf(text, sizeof(text), "(%u, %u, %u, %u)", std::to_integer<unsigned>(Pixels[offset]), std::to_integer<unsigned>(Pixels[offset + 1]), std::to_integer<unsigned>(Pixels[offset + 2]), std::to_integer<unsigned>(Pixels[offset + 3]));
    return text;
}

struct MeshDraw {
    ShaderRecompiler::MeshConfiguration mesh;
    AgcDriver::Pm4::DrawParameters draw;
    std::array<std::uint32_t, 4> vertexBuffer;
    std::uint32_t geometryPushBytes = ShaderRecompiler::MeshDrawPushOffsetBytes;
    bool restart = false;
    std::uint32_t waveSize = 64u;
    std::span<const std::uint32_t> code{};
};

void DrawMesh(AgcDriver::VulkanDevice& device, const MeshDraw& setup) {
    const auto target = device.Target();
    std::vector<std::uint32_t> userData(12, 0u);
    const auto index = AgcDriver::Graphics::MeshIndexBufferDescriptor(setup.draw);
    std::copy(index.begin(), index.end(), userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    std::copy(setup.vertexBuffer.begin(), setup.vertexBuffer.end(), userData.begin() + 8);
    const std::span<const std::uint32_t> code = !setup.code.empty() ? setup.code : setup.waveSize == 32u ? std::span<const std::uint32_t>(Wave32GeometryCode) : std::span<const std::uint32_t>(GeometryCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> geometryMemory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    ShaderRecompiler::RecompileRequest geometry{
        {ShaderStage::Mesh, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {setup.waveSize, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, geometryMemory},
        target,
        {0, 0, 0, setup.geometryPushBytes},
        ShaderRecompiler::GraphicsCompileContext{0, {}, setup.mesh, std::nullopt, {setup.draw.indexAddress, setup.draw.indexCount, setup.draw.indexSize, setup.draw.instanceCount}}
    };
    const auto meshResult = ShaderRecompiler::Recompile(geometry);
    const auto meshPush = static_cast<std::uint32_t>(meshResult.pushConstants.size());
    Require((meshPush == 0) == (setup.geometryPushBytes == 0), "the geometry program's push data does not follow its layout");

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 1;
    pixel.interpolatorSettings[0] = 0x400u;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, meshPush, ShaderRecompiler::MeshDrawPushOffsetBytes - meshPush},
        std::nullopt
    };
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Mesh, &meshResult, 0},
        {ShaderStage::Fragment, &pixelResult, meshPush}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Geometry, 0x20u, setup.waveSize, 64, setup.mesh, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = setup.mesh.inputPrimitive == 6u ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : setup.mesh.inputPrimitive == 5u ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.primitiveRestart = setup.restart;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    device.Draw(state, setup.draw, shaders);
    device.WaitIdle();
}

void DrawVertexPath(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t vertexCount, const std::array<std::uint32_t, 4>& vertexBuffer) {
    const auto target = device.Target();
    const std::vector<std::uint32_t> userData(vertexBuffer.begin(), vertexBuffer.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 8, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64},
        ShaderRecompiler::GraphicsCompileContext{8, {}, std::nullopt, std::nullopt, {0, vertexCount, 0, 1}}
    };
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 1;
    pixel.interpolatorSettings[0] = 0x400u;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, vertexPush, 128 - vertexPush},
        std::nullopt
    };
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0x02002000u, 64, 64, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    device.Draw(state, {0, vertexCount, 0, 1, 0, false}, shaders);
    device.WaitIdle();
}

constexpr ShaderRecompiler::MeshConfiguration SmallSubgroup{4u, 4u, 12u, 12u, 4u, 64u, 1024u, 0u, 4u};
constexpr ShaderRecompiler::MeshConfiguration WideSubgroup{4u, 32u, 96u, 96u, 32u, 128u, 2048u, 0u, 4u};

std::size_t FanPixel(float angle, float radius) {
    const auto x = static_cast<std::uint32_t>((radius * std::cos(angle) + 1.0f) * 0.5f * Width);
    const auto y = static_cast<std::uint32_t>(Height - (radius * std::sin(angle) + 1.0f) * 0.5f * Height);
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

void CheckFan(const char* what) {
    for (std::uint32_t triangle = 0; triangle + 1u < FanRim; ++triangle) {
        const auto offset = FanPixel((static_cast<float>(triangle) + 0.5f) * FanStep, 0.5f);
        Require(PixelIs(offset, TriangleColor(triangle + 1u)), std::string(what) + ": fan triangle " + std::to_string(triangle) + " was not rendered in its first vertex's color, pixel " + PixelText(offset));
    }
    Require(PixelIs(FanPixel((static_cast<float>(FanRim) - 0.5f) * FanStep, 0.5f), Background), std::string(what) + ": the fan was closed");
    Require(PixelIs(0, Background) && PixelIs(Pixels.size() - 4u, Background), std::string(what) + ": the corners changed");
}

void CheckTriangles(const char* what) {
    for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
        const auto offset = CellPixel(triangle);
        Require(PixelIs(offset, TriangleColor(triangle)), std::string(what) + ": triangle " + std::to_string(triangle) + " was not rendered in its color, pixel " + PixelText(offset));
    }
    Require(PixelIs(0, Background) && PixelIs(Pixels.size() - 4u, Background), std::string(what) + ": the corners changed");
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("ANYPS5_NO_SHADER_CACHE", "1");
#else
        setenv("ANYPS5_NO_SHADER_CACHE", "1", 1);
#endif
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            const auto vertices = TriangleVertices(triangle);
            for (std::uint32_t k = 0; k < 3; ++k) {
                Ordered[3 * triangle + k] = vertices[k];
                const auto slot = 3 * (Triangles - 1 - triangle) + (k + 1) % 3;
                Scrambled[slot] = vertices[k];
                Indices[3 * triangle + k] = static_cast<std::uint16_t>(slot);
            }
        }
        const std::array<float, 4> stripColor{128.0f / 255.0f, 1.0f, 64.0f / 255.0f, 1.0f};
        for (std::uint32_t i = 0; i < Strip.size(); ++i) {
            const float x = -1.0f + 2.0f * static_cast<float>(i / 2u) / Columns;
            const float y = i % 2u == 0u ? -1.0f : 0.0f;
            Strip[i] = {{x, y, 0.5f, 1.0f}, stripColor};
        }
        Fan[0] = {{0.0f, 0.0f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
        for (std::uint32_t k = 1; k <= FanRim; ++k) {
            const auto color = TriangleColor(k);
            const float angle = static_cast<float>(k - 1u) * FanStep;
            Fan[k] = {{0.9f * std::cos(angle), 0.9f * std::sin(angle), 0.5f, 1.0f}, {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f}};
        }
        for (std::uint32_t k = 0; k <= FanRim; ++k) {
            FanScrambled[FanRim - k] = Fan[k];
            FanIndices[k] = static_cast<std::uint16_t>(FanRim - k);
            FanRestart[k] = FanIndices[k];
        }
        FanRestart[FanRim] = 0xffffu;

        for (std::uint32_t y = 0; y <= GridRows; ++y) {
            for (std::uint32_t x = 0; x <= GridColumns; ++x) {
                const auto v = y * (GridColumns + 1) + x;
                const float wobble = 0.03f * static_cast<float>((v * 7u) % 5u);
                Grid[v] = {{-0.95f + 1.9f * static_cast<float>(x) / GridColumns + wobble, -0.95f + 1.9f * static_cast<float>(y) / GridRows - wobble, 0.5f, 1.0f}, {static_cast<float>((v * 37u) % 256u) / 255.0f, static_cast<float>((v * 101u) % 256u) / 255.0f, static_cast<float>((v * 53u + 17u) % 256u) / 255.0f, 1.0f}};
                BigVertices[BigBase + v] = Grid[v];
            }
        }
        for (std::uint32_t y = 0; y < GridRows; ++y) {
            for (std::uint32_t x = 0; x < GridColumns; ++x) {
                const auto v = y * (GridColumns + 1) + x;
                const auto across = (x + y) % 2u == 0u;
                for (const auto value : across ? std::array<std::uint32_t, 6>{v, v + 1, v + GridColumns + 2, v, v + GridColumns + 2, v + GridColumns + 1} : std::array<std::uint32_t, 6>{v, v + 1, v + GridColumns + 1, v + 1, v + GridColumns + 2, v + GridColumns + 1}) GridList.push_back(value);
            }
        }
        for (std::uint32_t t = 0; t < 40; ++t) {
            const auto a = (t * 29u) % GridVertices, b = (t * 61u + 13u) % GridVertices, c = (t * 83u + 41u) % GridVertices;
            for (const auto value : {a, b, c}) GridList.push_back(value);
        }
        for (const auto value : {5u, 5u, 9u, 20u, 20u, 20u, 0xffffu, 3u, 4u, 30u, 0xffffu, 0xffffu}) GridList.push_back(value);
        for (std::uint32_t t = 0; t < 30; ++t) {
            const auto a = (t * 47u + 3u) % GridVertices, b = (t * 11u + 70u) % GridVertices, c = (t * 97u + 5u) % GridVertices;
            for (const auto value : {a, b, c}) GridList.push_back(value);
        }
        Require(GridList.size() <= GridIndices16.size(), "the grid list does not fit");
        for (std::size_t i = 0; i < GridList.size(); ++i) {
            GridIndices16[i] = static_cast<std::uint16_t>(GridList[i]);
            GridIndices32[i] = GridList[i];
            BigIndices32[i] = GridList[i] == 0xffffu ? 0xffffffffu : GridList[i] + BigBase;
        }
        for (std::uint32_t i = 0; i < GridStrip16.size(); ++i) {
            const auto column = (i / 2u) % (2u * GridColumns);
            const auto x = column <= GridColumns ? column : 2u * GridColumns - column;
            const auto row = (i / (4u * GridColumns)) % GridRows;
            GridStrip16[i] = static_cast<std::uint16_t>((row + i % 2u) * (GridColumns + 1) + x);
        }

        const auto testDevice = OpenVulkanTestDevice();
        if (!testDevice) return VulkanTestSkipped;
        auto& device = *testDevice;
        const auto target = device.Target();
        if (!target.mesh.has_value()) {
            std::puts("skipped, the device has no VK_EXT_mesh_shader");
            return VulkanTestSkipped;
        }
        if (device.DeviceName().starts_with("llvmpipe")) {
            std::puts("skipped, the device has no mesh shader compiler that handles these programs (llvmpipe)");
            return VulkanTestSkipped;
        }

        for (const auto& [name, subgroup] : {std::pair{"one-wave subgroups", SmallSubgroup}, std::pair{"two-wave subgroups", WideSubgroup}}) {
            ClearPixels();
            DrawMesh(device, {subgroup, {reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true}, VertexBufferDescriptor(Scrambled.data(), static_cast<std::uint32_t>(Scrambled.size()))});
            CheckTriangles((std::string("indexed triangle list, ") + name).c_str());

            ClearPixels();
            DrawMesh(device, {subgroup, {0, static_cast<std::uint32_t>(Ordered.size()), 0, 1, 0, false}, VertexBufferDescriptor(Ordered.data(), static_cast<std::uint32_t>(Ordered.size()))});
            CheckTriangles((std::string("non-indexed triangle list, ") + name).c_str());
        }

        auto wave32Subgroup = SmallSubgroup;
        wave32Subgroup.threadsPerGroup = 32u;
        for (const auto& subgroup : {wave32Subgroup, SmallSubgroup, WideSubgroup}) {
            ClearPixels();
            DrawMesh(device, {subgroup, {reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true}, VertexBufferDescriptor(Scrambled.data(), static_cast<std::uint32_t>(Scrambled.size())), ShaderRecompiler::MeshDrawPushOffsetBytes, false, 32u});
            CheckTriangles("indexed wave32 triangle list");
            ClearPixels();
            DrawMesh(device, {subgroup, {0, static_cast<std::uint32_t>(Ordered.size()), 0, 1, 0, false}, VertexBufferDescriptor(Ordered.data(), static_cast<std::uint32_t>(Ordered.size())), ShaderRecompiler::MeshDrawPushOffsetBytes, false, 32u});
            CheckTriangles("non-indexed wave32 triangle list");
        }

        constexpr std::size_t recordBlockBytes = 65536;
        auto* record = static_cast<std::uint32_t*>(::operator new(recordBlockBytes, std::align_val_t{65536}));
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(record, recordBlockBytes, true, true, true);
        }
        const auto drawIndirect = [&](std::uint32_t count, std::uint32_t instances, std::uint32_t first) {
            const std::array<std::uint32_t, 5> words{count, instances, first, 0u, 0u};
            std::copy(words.begin(), words.end(), record);
            AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
            draw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(record), 0x25u, 20u, 20u, 1u, false, 0u, 0x280u, 0x280u, 0x280u, false, 0u};
            ClearPixels();
            DrawMesh(device, {WideSubgroup, draw, VertexBufferDescriptor(Scrambled.data(), static_cast<std::uint32_t>(Scrambled.size()))});
        };
        drawIndirect(static_cast<std::uint32_t>(Indices.size()), 1, 0);
        CheckTriangles("indirect indexed triangle list");
        drawIndirect(3u * 10u, 1, 3u * 5u);
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            const auto offset = CellPixel(triangle);
            const bool drawn = triangle >= 5u && triangle < 15u;
            Require(drawn ? PixelIs(offset, TriangleColor(triangle)) : PixelIs(offset, Background), "indirect index range: triangle " + std::to_string(triangle) + " pixel " + PixelText(offset));
        }
        drawIndirect(1000u, 1, 3u * (Triangles - 2u));
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            const auto offset = CellPixel(triangle);
            Require(triangle >= Triangles - 2u ? PixelIs(offset, TriangleColor(triangle)) : PixelIs(offset, Background), "indirect index count past the buffer: triangle " + std::to_string(triangle) + " pixel " + PixelText(offset));
        }
        for (const auto& [count, instances, first] : {std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(Indices.size()), 0u, 0u}, std::array<std::uint32_t, 3>{0u, 1u, 0u}, std::array<std::uint32_t, 3>{3u, 1u, static_cast<std::uint32_t>(Indices.size())}}) {
            drawIndirect(count, instances, first);
            for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) Require(PixelIs(CellPixel(triangle), Background), "an empty indirect record drew triangle " + std::to_string(triangle));
        }

        ClearPixels();
        DrawMesh(device, {SmallSubgroup, {0, static_cast<std::uint32_t>(Ordered.size()), 0, 1, 0, false}, VertexBufferDescriptor(Ordered.data(), static_cast<std::uint32_t>(Ordered.size())), 0});
        CheckTriangles("mesh program without push data");

        ClearPixels();
        DrawVertexPath(device, PassthroughCode, static_cast<std::uint32_t>(Ordered.size()), VertexBufferDescriptor(Ordered.data(), static_cast<std::uint32_t>(Ordered.size())));
        CheckTriangles("passthrough program gated by its subgroup counts on the vertex path");

        ClearPixels();
        const ShaderRecompiler::MeshConfiguration strip{6u, 3u, 5u, 9u, 3u, 64u, 1024u, 0u, 4u};
        DrawMesh(device, {strip, {0, static_cast<std::uint32_t>(Strip.size()), 0, 1, 0, false}, VertexBufferDescriptor(Strip.data(), static_cast<std::uint32_t>(Strip.size()))});
        const std::array<std::uint8_t, 4> stripPixel{128, 255, 64, 255};
        for (std::uint32_t y = Height / 2u + 2u; y < Height - 2u; y += 5u) {
            for (std::uint32_t x = 2u; x < Width - 2u; x += 5u) {
                const auto offset = (static_cast<std::size_t>(y) * Width + x) * 4u;
                Require(PixelIs(offset, stripPixel), "triangle strip: pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + PixelText(offset));
            }
        }
        Require(PixelIs((static_cast<std::size_t>(Height / 4u) * Width + Width / 2u) * 4u, Background), "triangle strip: the upper half changed");

        const ShaderRecompiler::MeshConfiguration fan{5u, 3u, 5u, 9u, 3u, 64u, 1024u, 0u, 4u};
        ClearPixels();
        DrawMesh(device, {fan, {0, static_cast<std::uint32_t>(Fan.size()), 0, 1, 0, false}, VertexBufferDescriptor(Fan.data(), static_cast<std::uint32_t>(Fan.size()))});
        CheckFan("non-indexed triangle fan");
        ClearPixels();
        DrawMesh(device, {fan, {reinterpret_cast<std::uintptr_t>(FanIndices.data()), static_cast<std::uint32_t>(FanIndices.size()), 2, 1, 0, true}, VertexBufferDescriptor(FanScrambled.data(), static_cast<std::uint32_t>(FanScrambled.size()))});
        CheckFan("indexed triangle fan");
        ClearPixels();
        DrawMesh(device, {fan, {reinterpret_cast<std::uintptr_t>(FanIndices.data()), static_cast<std::uint32_t>(FanIndices.size()), 2, 1, 0, true}, VertexBufferDescriptor(FanScrambled.data(), static_cast<std::uint32_t>(FanScrambled.size())), ShaderRecompiler::MeshDrawPushOffsetBytes, true});
        CheckFan("indexed triangle fan with restart enabled and no restart index");
        bool refused = false;
        try {
            DrawMesh(device, {fan, {reinterpret_cast<std::uintptr_t>(FanRestart.data()), static_cast<std::uint32_t>(FanRestart.size()), 2, 1, 0, true}, VertexBufferDescriptor(FanScrambled.data(), static_cast<std::uint32_t>(FanScrambled.size())), ShaderRecompiler::MeshDrawPushOffsetBytes, true});
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("primitive restart in a triangle fan") != std::string::npos;
            if (!refused) throw;
        }
        Require(refused, "a triangle fan with a restart index was drawn");
        const std::array<std::uint32_t, 5> fanWords{static_cast<std::uint32_t>(FanIndices.size()), 1u, 0u, 0u, 0u};
        std::copy(fanWords.begin(), fanWords.end(), record);
        AgcDriver::Pm4::DrawParameters fanDraw{reinterpret_cast<std::uintptr_t>(FanIndices.data()), static_cast<std::uint32_t>(FanIndices.size()), 2, 1, 0, true};
        fanDraw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(record), 0x25u, 20u, 20u, 1u, false, 0u, 0x280u, 0x280u, 0x280u, false, 0u};
        ClearPixels();
        DrawMesh(device, {fan, fanDraw, VertexBufferDescriptor(FanScrambled.data(), static_cast<std::uint32_t>(FanScrambled.size()))});
        CheckFan("indirect indexed triangle fan");

        const auto withReuse = [](ShaderRecompiler::MeshConfiguration mesh, std::uint32_t vertices, std::uint32_t primitives) {
            mesh.reuseVertices = vertices;
            mesh.reusePrimitives = primitives;
            return mesh;
        };
        const auto sameAsWithoutReuse = [&](const std::string& what, MeshDraw setup, std::uint32_t vertices, std::uint32_t primitives) {
            ClearPixels();
            DrawMesh(device, setup);
            const auto without = Pixels;
            Require(!std::all_of(without.begin(), without.end(), [&](std::byte value) { return value == without[0]; }), what + ": nothing was drawn");
            setup.mesh = withReuse(setup.mesh, vertices, primitives);
            ClearPixels();
            DrawMesh(device, setup);
            for (std::size_t offset = 0; offset < Pixels.size(); offset += 4) {
                Require(std::memcmp(Pixels.data() + offset, without.data() + offset, 4) == 0, what + ": pixel " + std::to_string(offset / 4) + " is " + PixelText(offset) + " with vertex reuse");
            }
        };
        const ShaderRecompiler::MeshConfiguration robots{4u, 21u, 63u, 64u, 64u, 64u, 0u, 0u, 4u, true};
        const ShaderRecompiler::MeshConfiguration smallPassthrough{4u, 4u, 12u, 12u, 4u, 64u, 0u, 0u, 4u, true};
        const auto gridCount = static_cast<std::uint32_t>(GridList.size());
        const auto grid = VertexBufferDescriptor(Grid.data(), GridVertices);
        const auto big = VertexBufferDescriptor(BigVertices.data(), static_cast<std::uint32_t>(BigVertices.size()));
        struct Program {
            const char* name;
            ShaderRecompiler::MeshConfiguration mesh;
            std::span<const std::uint32_t> code;
            std::uint32_t vertices;
            std::uint32_t primitives;
        };
        for (const auto& program : {Program{"64-vertex passthrough", robots, PassthroughCode, 64u, 64u}, Program{"12-vertex passthrough", smallPassthrough, PassthroughCode, 12u, 4u}, Program{"one-wave geometry", SmallSubgroup, GeometryCode, 12u, 4u}, Program{"two-wave geometry", WideSubgroup, GeometryCode, 96u, 32u}}) {
            const std::string name = program.name;
            auto setup = [&](AgcDriver::Pm4::DrawParameters draw, const std::array<std::uint32_t, 4>& vertices) {
                MeshDraw result{program.mesh, draw, vertices};
                result.code = program.code;
                return result;
            };
            sameAsWithoutReuse(name + ", 16-bit grid", setup({reinterpret_cast<std::uintptr_t>(GridIndices16.data()), gridCount, 2, 1, 0, true}, grid), program.vertices, program.primitives);
            sameAsWithoutReuse(name + ", 32-bit grid", setup({reinterpret_cast<std::uintptr_t>(GridIndices32.data()), gridCount, 4, 1, 0, true}, grid), program.vertices, program.primitives);
            sameAsWithoutReuse(name + ", 32-bit values past 16 bits", setup({reinterpret_cast<std::uintptr_t>(BigIndices32.data()), gridCount, 4, 1, 0, true}, big), program.vertices, program.primitives);
            auto based = setup({reinterpret_cast<std::uintptr_t>(GridIndices16.data()), gridCount, 2, 1, 0, true}, big);
            based.draw.firstVertex = BigBase;
            sameAsWithoutReuse(name + ", base vertex", based, program.vertices, program.primitives);
            sameAsWithoutReuse(name + ", three instances", setup({reinterpret_cast<std::uintptr_t>(GridIndices16.data()), gridCount, 2, 3, 0, true}, grid), program.vertices, program.primitives);
            auto restart = setup({reinterpret_cast<std::uintptr_t>(GridIndices16.data()), gridCount, 2, 1, 0, true}, grid);
            restart.restart = true;
            sameAsWithoutReuse(name + ", restart enabled", restart, program.vertices, program.primitives);
            sameAsWithoutReuse(name + ", trailing indices", setup({reinterpret_cast<std::uintptr_t>(GridIndices16.data()), gridCount - 2u, 2, 1, 0, true}, grid), program.vertices, program.primitives);
            ClearPixels();
            DrawMesh(device, setup({reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true}, VertexBufferDescriptor(Scrambled.data(), static_cast<std::uint32_t>(Scrambled.size()))));
            CheckTriangles((name + ", indexed triangle list without reuse").c_str());
            auto scrambled = setup({reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true}, VertexBufferDescriptor(Scrambled.data(), static_cast<std::uint32_t>(Scrambled.size())));
            scrambled.mesh = withReuse(scrambled.mesh, program.vertices, program.primitives);
            ClearPixels();
            DrawMesh(device, scrambled);
            CheckTriangles((name + ", indexed triangle list with vertex reuse").c_str());
            auto plain = setup({0, static_cast<std::uint32_t>(Ordered.size()), 0, 1, 0, false}, VertexBufferDescriptor(Ordered.data(), static_cast<std::uint32_t>(Ordered.size())));
            plain.mesh = withReuse(plain.mesh, program.vertices, program.primitives);
            ClearPixels();
            DrawMesh(device, plain);
            CheckTriangles((name + ", non-indexed triangle list with the reuse layout").c_str());
        }
        const ShaderRecompiler::MeshConfiguration passthroughStrip{6u, 62u, 64u, 64u, 64u, 64u, 0u, 0u, 4u, true};
        MeshDraw stripDraw{passthroughStrip, {reinterpret_cast<std::uintptr_t>(GridStrip16.data()), static_cast<std::uint32_t>(GridStrip16.size()), 2, 1, 0, true}, grid};
        stripDraw.code = PassthroughCode;
        sameAsWithoutReuse("passthrough indexed strip", stripDraw, 64u, 64u);
        sameAsWithoutReuse("indexed fan", {fan, {reinterpret_cast<std::uintptr_t>(FanIndices.data()), static_cast<std::uint32_t>(FanIndices.size()), 2, 1, 0, true}, VertexBufferDescriptor(FanScrambled.data(), static_cast<std::uint32_t>(FanScrambled.size()))}, 5u, 3u);
        const std::array<std::uint32_t, 5> gridWords{gridCount, 2u, 0u, 0u, 0u};
        std::copy(gridWords.begin(), gridWords.end(), record);
        AgcDriver::Pm4::DrawParameters indirectGrid{reinterpret_cast<std::uintptr_t>(GridIndices16.data()), gridCount, 2, 1, 0, true};
        indirectGrid.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(record), 0x25u, 20u, 20u, 1u, false, 0u, 0x280u, 0x280u, 0x280u, false, 0u};
        sameAsWithoutReuse("indirect geometry grid", {WideSubgroup, indirectGrid, grid}, 96u, 32u);
        MeshDraw indirectPassthrough{robots, indirectGrid, grid};
        indirectPassthrough.code = PassthroughCode;
        sameAsWithoutReuse("indirect passthrough grid", indirectPassthrough, 64u, 64u);

        std::puts("Mesh tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
