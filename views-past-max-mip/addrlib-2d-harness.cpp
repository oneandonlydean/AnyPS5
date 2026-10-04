#include "addrinterface.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CIASICIDGFXENGINE_ARCTICISLAND 0x0000000D

static void* ADDR_API allocSysMem(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
static ADDR_E_RETURNCODE ADDR_API freeSysMem(const ADDR_FREESYSMEM_INPUT* in) { std::free(in->pVirtAddr); return ADDR_OK; }

static ADDR_HANDLE create(unsigned family, unsigned revision, unsigned gbAddrConfig) {
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = CIASICIDGFXENGINE_ARCTICISLAND;
    in.chipFamily = family;
    in.chipRevision = revision;
    in.regValue.gbAddrConfig = gbAddrConfig;
    in.callbacks.allocSysMem = allocSysMem;
    in.callbacks.freeSysMem = freeSysMem;
    if (AddrCreate(&in, &out) != ADDR_OK) { std::fprintf(stderr, "AddrCreate failed\n"); std::exit(1); }
    return out.hLib;
}

static void describe(ADDR_HANDLE lib, AddrSwizzleMode swizzle, const char* name, unsigned bpp, unsigned w, unsigned h, unsigned mips) {
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT info{};
    ADDR2_MIP_INFO mipInfo[16]{};
    in.size = sizeof(in);
    info.size = sizeof(info);
    info.pMipInfo = mipInfo;
    in.swizzleMode = swizzle;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.bpp = bpp;
    in.width = w;
    in.height = h;
    in.numSlices = 1;
    in.numMipLevels = mips;
    in.numSamples = 1;
    in.numFrags = 1;
    in.flags.texture = 1;
    if (Addr2ComputeSurfaceInfo(lib, &in, &info) != ADDR_OK) { std::printf("%s %ux%u %u bpp, %u levels: surface info failed\n", name, w, h, bpp, mips); return; }
    std::printf("%s %ux%u %u bpp, %u levels: surfSize 0x%llx, firstMipIdInTail %u, mipChainInTail %u, mipChainPitch %u, mipChainHeight %u\n", name, w, h, bpp, mips, (unsigned long long)info.surfSize, info.firstMipIdInTail, info.mipChainInTail, info.mipChainPitch, info.mipChainHeight);
    for (unsigned mip = 0; mip < mips; ++mip) {
        const auto& m = mipInfo[mip];
        ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT a{};
        ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT r{};
        a.size = sizeof(a);
        r.size = sizeof(r);
        a.mipId = mip;
        a.swizzleMode = swizzle;
        a.flags = in.flags;
        a.resourceType = ADDR_RSRC_TEX_2D;
        a.bpp = bpp;
        a.unalignedWidth = w;
        a.unalignedHeight = h;
        a.numSlices = 1;
        a.numMipLevels = mips;
        a.numSamples = 1;
        a.numFrags = 1;
        const unsigned lw = std::max(w >> mip, 1u), lh = std::max(h >> mip, 1u);
        unsigned long long first = 0, last = 0;
        if (Addr2ComputeSurfaceAddrFromCoord(lib, &a, &r) == ADDR_OK) first = r.addr;
        a.x = lw - 1;
        a.y = lh - 1;
        if (Addr2ComputeSurfaceAddrFromCoord(lib, &a, &r) == ADDR_OK) last = r.addr;
        std::printf("  level %2u %4ux%-4u offset 0x%08llx macroBlockOffset 0x%08llx mipTailOffset 0x%05x tailCoord (%u, %u) pitch %u height %u texel(0,0) 0x%llx texel(last) 0x%llx\n", mip, lw, lh, (unsigned long long)m.offset, (unsigned long long)m.macroBlockOffset, m.mipTailOffset, m.mipTailCoordX, m.mipTailCoordY, m.pitch, m.height, first, last);
    }
}

static void texels(ADDR_HANDLE lib, AddrSwizzleMode swizzle, unsigned bpp, unsigned w, unsigned h, unsigned mips, unsigned mip) {
    std::printf("  texels of level %u (%u levels), x 0..7, y 0:", mip, mips);
    for (unsigned x = 0; x < 8; ++x) {
        ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT a{};
        ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT r{};
        a.size = sizeof(a);
        r.size = sizeof(r);
        a.x = x;
        a.mipId = mip;
        a.swizzleMode = swizzle;
        a.flags.texture = 1;
        a.resourceType = ADDR_RSRC_TEX_2D;
        a.bpp = bpp;
        a.unalignedWidth = w;
        a.unalignedHeight = h;
        a.numSlices = 1;
        a.numMipLevels = mips;
        a.numSamples = 1;
        a.numFrags = 1;
        if (Addr2ComputeSurfaceAddrFromCoord(lib, &a, &r) != ADDR_OK) { std::printf(" fail"); continue; }
        std::printf(" 0x%llx", (unsigned long long)r.addr);
    }
    std::printf("\n");
}

int main() {
    const struct { const char* name; unsigned family, revision, config; } chips[] = {
        {"Navi10 (gb_addr_config 0x44)", 0x8F, 0x01, 0x00000044},
    };
    for (const auto& chip : chips) {
        std::printf("== %s\n", chip.name);
        ADDR_HANDLE lib = create(chip.family, chip.revision, chip.config);
        for (unsigned mips : {6u, 7u, 9u, 11u}) describe(lib, ADDR_SW_64KB_R_X, "SW_64KB_R_X", 64, 1920, 1080, mips);
        for (unsigned mips : {1u, 2u}) describe(lib, ADDR_SW_64KB_R_X, "SW_64KB_R_X", 64, 1920, 1080, mips);
        for (unsigned mips : {2u, 3u}) describe(lib, ADDR_SW_LINEAR, "SW_LINEAR", 32, 16, 16, mips);
        for (unsigned mips : {3u, 4u}) describe(lib, ADDR_SW_64KB_R_X, "SW_64KB_R_X", 32, 256, 256, mips);
        texels(lib, ADDR_SW_64KB_R_X, 32, 256, 256, 4, 3);
        texels(lib, ADDR_SW_64KB_R_X, 32, 256, 256, 3, 2);
    }
    return 0;
}
