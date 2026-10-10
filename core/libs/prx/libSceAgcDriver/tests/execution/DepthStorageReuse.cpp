#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace {

using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t IdentitySwizzle = 0xfacu;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format8888Uint = 60;
constexpr std::uint32_t HalfDepth = 0x3f000000u;
constexpr std::size_t BlockBytes = 65536;

alignas(256) constexpr std::array<std::uint32_t, 24> StoreCode{
    0x4a500081, 0xd5690014, 0x0201ff28, 0x9e3779b1, 0x4a2a28ff, 0x7f4a7c15, 0x4a2c28ff, 0xfe94f82a,
    0x4a2e28ff, 0x7ddf743f, 0x7e3c0300, 0x7e3e0280, 0xf0281f08, 0x0001141e, 0x7e3e0281, 0xf0281508,
    0x0001141e, 0x7e3e0282, 0xf0281a08, 0x0001141e, 0x7e3e0283, 0xf0281808, 0x0001141e, 0xbf810000,
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "depth storage reuse: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

struct Image {
    const char* name;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
};

std::uint32_t Data(std::uint32_t tid, std::uint32_t index) {
    return (tid + 1u) * 0x9e3779b1u + index * 0x7f4a7c15u;
}

std::uint32_t Stored(std::uint32_t tid, std::uint32_t row) {
    constexpr std::array<std::uint32_t, 4> Dmasks{0xfu, 0x5u, 0xau, 0x8u};
    return (Dmasks[row] & 1u) != 0 ? Data(tid, 0) : 0u;
}

std::uint32_t Initial(std::size_t texel) {
    return static_cast<std::uint32_t>(0x80u + 37u * texel) | 0x01000000u;
}

std::array<std::uint32_t, 8> Descriptor(const void* data, const Image& image) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (image.format << 20u) | (((image.width - 1u) & 3u) << 30u),
        ((image.width - 1u) >> 2u) | ((image.height - 1u) << 14u),
        IdentitySwizzle | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void Store(AgcDriver::VulkanDevice& device, std::uint8_t* memory, const Image& image) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = Descriptor(memory, image);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> regions{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, regions},
        device.Target(),
        {0, 0, 0, 128}
    };
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(memory), BlockBytes, nullptr, "test");
    device.WaitIdle();
}

void Check(AgcDriver::VulkanDevice& device, std::uint8_t* memory, const Image& image, bool seeded) {
    const auto mip = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, image.format, image.width, image.height, 1).front();
    const auto offset = [&](std::uint32_t x, std::uint32_t y) { return mip.tiledOffset + static_cast<std::uint64_t>(y) * mip.pitchBytes + static_cast<std::uint64_t>(x) * 4u; };
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto at = offset(x, y);
            std::uint32_t texel = Initial(static_cast<std::size_t>(at / 4u));
            std::memcpy(memory + at, &texel, 4);
        }
    }
    Store(device, memory, image);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto at = offset(x, y);
            const auto wanted = x < Threads && y < 4u ? Stored(x, y) : seeded ? HalfDepth : Initial(static_cast<std::size_t>(at / 4u));
            std::uint32_t actual = 0;
            std::memcpy(&actual, memory + at, 4);
            if (actual != wanted) {
                char message[200];
                std::snprintf(message, sizeof(message), "%s: texel (%u, %u) is 0x%08x, expected 0x%08x", image.name, x, y, actual, wanted);
                Require(false, message);
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        auto* memory = block.Data();
        const auto shaders = DepthFastClearHarness::Compile(*device);
        const DepthFastClearHarness::Surface surface{reinterpret_cast<std::uintptr_t>(memory), 0, 0, false, VK_FORMAT_D32_SFLOAT};
        Require(DepthFastClearHarness::Draw(*device, shaders, surface, {.depthWrite = true}) == Covered, "the depth surface did not take the draw's depth");
        constexpr auto width = DepthFastClearHarness::Width;
        constexpr auto height = DepthFastClearHarness::Height;
        Check(*device, memory, {"an R32F storage image of another extent over a D32 surface's memory", Format32Float, width / 2u, height / 4u}, false);
        Check(*device, memory, {"an R8G8B8A8 uint storage image over a D32 surface's memory", Format8888Uint, width, height}, false);
        Check(*device, memory, {"an R32F storage image over a D32 surface's own extent", Format32Float, width, height}, true);
        std::puts("depth storage reuse tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
