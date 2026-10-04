#include "addrinterface.h"
#include <cstdio>
#include <cstdint>
#define CIASICIDGFXENGINE_ARCTICISLAND 0x0000000D
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

static void* ADDR_API allocSysMem(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
static ADDR_E_RETURNCODE ADDR_API freeSysMem(const ADDR_FREESYSMEM_INPUT* in) { std::free(in->pVirtAddr); return ADDR_OK; }

static ADDR_HANDLE create() {
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = CIASICIDGFXENGINE_ARCTICISLAND;
    in.chipFamily = 0x8F;
    in.chipRevision = 0x01;
    in.regValue.gbAddrConfig = 0x00000044;
    in.callbacks.allocSysMem = allocSysMem;
    in.callbacks.freeSysMem = freeSysMem;
    if (AddrCreate(&in, &out) != ADDR_OK) { std::fprintf(stderr, "AddrCreate failed\n"); std::exit(1); }
    return out.hLib;
}

struct Surface { int swizzle; unsigned bpp; unsigned format; unsigned w, h, d, mips; };

static unsigned shiftCeil(unsigned v, unsigned s) { return (v + (1u << s) - 1u) >> s; }

int main(int argc, char** argv) {
    ADDR_HANDLE lib = create();
    const bool sample = argc > 1 && std::string(argv[1]) == "sample";
    std::vector<Surface> surfaces;
    if (sample) {
        surfaces = {
            {ADDR_SW_4KB_S, 32, 56, 8, 8, 8, 2},
            {ADDR_SW_4KB_S, 32, 56, 33, 20, 12, 6},
            {ADDR_SW_64KB_S, 64, 71, 40, 24, 20, 6},
            {ADDR_SW_64KB_S, 8, 1, 100, 60, 70, 7},
            {ADDR_SW_4KB_S, 128, 77, 9, 5, 3, 4},
        };
    } else {
        const int modes[] = {ADDR_SW_4KB_S, ADDR_SW_64KB_S};
        const unsigned bpps[] = {8, 16, 32, 64, 128};
        const unsigned formats[] = {1, 7, 22, 71, 77};
        const unsigned dims[][3] = {{1, 1, 1}, {2, 3, 1}, {8, 8, 8}, {16, 4, 2}, {17, 9, 5}, {33, 20, 12}, {64, 64, 32}, {40, 24, 20}, {100, 60, 70}, {130, 3, 9}, {5, 129, 33}, {256, 256, 4}};
        for (int m : modes) for (unsigned b = 0; b < 5; ++b) for (const auto& d : dims) {
            unsigned maxMips = 1;
            while ((std::max({d[0], d[1], d[2]}) >> maxMips) != 0) ++maxMips;
            for (unsigned mips = 1; mips <= maxMips; ++mips) surfaces.push_back({m, bpps[b], formats[b], d[0], d[1], d[2], mips});
        }
    }
    FILE* out = sample ? nullptr : std::fopen("addrlib-navi10-thick.bin", "wb");
    for (const auto& s : surfaces) {
        ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT info{};
        ADDR2_MIP_INFO mipInfo[16]{};
        in.size = sizeof(in);
        info.size = sizeof(info);
        info.pMipInfo = mipInfo;
        in.swizzleMode = static_cast<AddrSwizzleMode>(s.swizzle);
        in.resourceType = ADDR_RSRC_TEX_3D;
        in.bpp = s.bpp;
        in.width = s.w;
        in.height = s.h;
        in.numSlices = s.d;
        in.numMipLevels = s.mips;
        in.numSamples = 1;
        in.numFrags = 1;
        in.flags.texture = 1;
        if (Addr2ComputeSurfaceInfo(lib, &in, &info) != ADDR_OK) { std::fprintf(stderr, "surface info failed\n"); return 1; }
        std::vector<std::uint32_t> words;
        auto addressOf = [&](unsigned mip, unsigned x, unsigned y, unsigned z) {
            ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT a{};
            ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT r{};
            a.size = sizeof(a);
            r.size = sizeof(r);
            a.x = x; a.y = y; a.slice = z; a.mipId = mip;
            a.swizzleMode = in.swizzleMode;
            a.flags = in.flags;
            a.resourceType = ADDR_RSRC_TEX_3D;
            a.bpp = s.bpp;
            a.unalignedWidth = s.w;
            a.unalignedHeight = s.h;
            a.numSlices = s.d;
            a.numMipLevels = s.mips;
            a.numSamples = 1;
            a.numFrags = 1;
            if (Addr2ComputeSurfaceAddrFromCoord(lib, &a, &r) != ADDR_OK) { std::fprintf(stderr, "addr failed\n"); std::exit(1); }
            return r.addr;
        };
        if (sample) {
            std::printf("%s %u bpp %ux%ux%u mips %u: surfSize %llu firstMipInTail %u\n", s.swizzle == ADDR_SW_4KB_S ? "SW_4KB_S" : "SW_64KB_S", s.bpp, s.w, s.h, s.d, s.mips, (unsigned long long)info.surfSize, info.firstMipIdInTail);
            for (unsigned mip = 0; mip < s.mips; ++mip) {
                const unsigned w = std::max(s.w >> mip, 1u), h = std::max(s.h >> mip, 1u), d = std::max(s.d >> mip, 1u);
                const unsigned pts[5][3] = {{0, 0, 0}, {w - 1, h - 1, d - 1}, {w / 2, h / 3, d / 2}, {w / 3, h - 1, 0}, {w - 1, 0, d - 1}};
                for (const auto& p : pts) std::printf("{%u, %u, %u, %u, 0x%llx}, ", mip, p[0], p[1], p[2], (unsigned long long)addressOf(mip, p[0], p[1], p[2]));
                std::printf("\n");
            }
            continue;
        }
        std::uint32_t header[9] = {static_cast<std::uint32_t>(s.swizzle), s.format, s.w, s.h, s.d, s.mips, info.firstMipIdInTail, static_cast<std::uint32_t>(info.surfSize), static_cast<std::uint32_t>(info.surfSize >> 32)};
        std::fwrite(header, sizeof(header), 1, out);
        for (unsigned mip = 0; mip < s.mips; ++mip) {
            const unsigned w = std::max(s.w >> mip, 1u), h = std::max(s.h >> mip, 1u), d = std::max(s.d >> mip, 1u);
            std::vector<std::uint32_t> coords;
            const std::uint64_t total = static_cast<std::uint64_t>(w) * h * d;
            std::uint64_t state = 0x9e3779b97f4a7c15ull ^ (total * 31 + mip);
            if (total <= 8192) {
                for (unsigned z = 0; z < d; ++z) for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) { coords.push_back(x); coords.push_back(y); coords.push_back(z); }
            } else {
                for (unsigned i = 0; i < 4096; ++i) {
                    state = state * 6364136223846793005ull + 1442695040888963407ull;
                    const auto r = state >> 16;
                    coords.push_back(static_cast<unsigned>(r % w)); coords.push_back(static_cast<unsigned>((r / w) % h)); coords.push_back(static_cast<unsigned>((r / w / h) % d));
                }
            }
            std::uint32_t count = static_cast<std::uint32_t>(coords.size() / 3);
            std::fwrite(&count, 4, 1, out);
            std::vector<std::uint64_t> addrs;
            for (std::size_t i = 0; i < coords.size(); i += 3) addrs.push_back((static_cast<std::uint64_t>(coords[i]) << 40) | (static_cast<std::uint64_t>(coords[i + 1]) << 20) | coords[i + 2]);
            std::fwrite(addrs.data(), 8, addrs.size(), out);
            addrs.clear();
            for (std::size_t i = 0; i < coords.size(); i += 3) addrs.push_back(addressOf(mip, coords[i], coords[i + 1], coords[i + 2]));
            std::fwrite(addrs.data(), 8, addrs.size(), out);
        }
    }
    if (out) std::fclose(out);
    std::fprintf(stderr, "%zu surfaces\n", surfaces.size());
    return 0;
}
