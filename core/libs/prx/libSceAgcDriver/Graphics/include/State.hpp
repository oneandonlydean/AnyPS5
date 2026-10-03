#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STATE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include "Recompiler.hpp"

namespace AgcDriver::Graphics {

enum class ShaderPath {
    Vertex,
    Geometry,
    Tessellation,
    TessellationGeometry
};

struct ShaderStages {
    ShaderPath path;
    std::uint32_t registerValue;
    std::uint32_t vertexWaveSize;
    std::uint32_t fragmentWaveSize;
    std::optional<ShaderRecompiler::MeshConfiguration> mesh;
    std::optional<ShaderRecompiler::TessellationConfiguration> tessellation;
};

struct ColorTarget {
    std::uint64_t address;
    VkExtent2D extent;
    VkFormat format;
    std::size_t bytes;
    std::uint8_t componentMapping;
    ColorTileMode tileMode = ColorTileMode::Linear;
    std::uint32_t elementBytes = 4;
    // DCC metadata of a compressed target (CB_COLOR_INFO DCC_ENABLE), or 0 (see DccMetadata.hpp).
    std::uint64_t dccAddress = 0;
    bool dccAlphaOnMsb = false;
    // CB_COLOR_CLEAR_WORD0/1: the texel (in memory order, 64 bits at most) the DCC register clear code
    // (DccKeys::ClearRegister) stands for.
    std::array<std::uint32_t, 2> clearWords{};
    std::uint64_t surfaceAddress = 0;
    VkExtent2D surfaceExtent{};
    std::uint32_t mipCount = 1;
    std::uint32_t mip = 0;
    bool mipTail = false;
    std::uint32_t slot = 0;
    std::uint32_t exportIndex = 0;
    std::uint32_t depth = 1;
    std::uint32_t depthSlice = 0;
};

// The depth/stencil surface a draw tests against (DB_Z_INFO, DB_STENCIL_INFO, the DB_*_BASE words,
// DB_DEPTH_SIZE_XY). Its contents live in a GPU-resident image (DepthTarget.hpp) keyed by these
// fields; the guest memory behind the surface is neither read nor written (see DepthTarget.hpp).
struct DepthTarget {
    // DB_Z_READ_BASE (the same as DB_Z_WRITE_BASE), or the stencil base of a stencil-only surface.
    std::uint64_t address = 0;
    std::uint64_t stencilAddress = 0;
    // DB_HTILE_DATA_BASE when DB_Z_INFO.TILE_SURFACE_ENABLE is set (HTILE fills mean clears), else 0.
    std::uint64_t htileAddress = 0;
    VkExtent2D extent{};
    // DB_Z_INFO.FORMAT (0 none, 1 Z_16, 3 Z_32_FLOAT) and whether DB_STENCIL_INFO.FORMAT is STENCIL_8.
    std::uint32_t zFormat = 0;
    bool stencil = false;
    // DB_Z_INFO.SW_MODE, for the record (the surface is never detiled).
    std::uint32_t tileMode = 0;
    // DB_DEPTH_VIEW.SLICE_START of a single-layer view into an array surface.
    std::uint32_t slice = 0;
};

// The depth/stencil state of a draw that renders with a depth attachment (`attached`), decoded from
// DB_DEPTH_CONTROL, DB_STENCIL_CONTROL, DB_STENCILREFMASK(_BF), DB_DEPTH_VIEW and DB_RENDER_CONTROL.
// A depth or stencil clear draw (DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE / STENCIL_CLEAR_ENABLE, the
// fast clear the DB does through HTILE) is rendered as an always-passing test that writes the clear
// values: DecodeState pins the viewport depth range to DB_DEPTH_CLEAR and replaces the stencil
// operations with REPLACE of DB_STENCIL_CLEAR. The clear values are decoded for every attached draw:
// a pending clear of the resident image (a new image, an HTILE fill) is resolved with them.
struct DepthState {
    bool attached = false;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
    bool stencilTest = false;
    VkStencilOpState front{};
    VkStencilOpState back{};
    bool clearDepth = false;
    bool clearStencil = false;
    float depthClearValue = 0;
    std::uint32_t stencilClearValue = 0;
    // PA_SU_SC_MODE_CNTL.POLY_OFFSET_*_ENABLE with PA_SU_POLY_OFFSET_*, in Vulkan's units.
    bool depthBias = false;
    float depthBiasConstant = 0;
    float depthBiasSlope = 0;
    float depthBiasClamp = 0;
    bool depthBounds = false;
    float depthBoundsMin = 0;
    float depthBoundsMax = 1;
};

struct State {
    ShaderStages stages;
    // The first written MRT slot; `blends` holds one state per color export, attachment i being
    // export MRTi (exports go to the slots CB_SHADER_MASK enables, in order: ColorTarget::slot), and
    // `colors` the written slots with their export indices (an unwritten one is VK_ATTACHMENT_UNUSED).
    ColorTarget color;
    std::vector<ColorTarget> colors;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    bool hasColorTarget;
    bool rectList = false;
    VkExtent2D renderExtent;
    VkPrimitiveTopology topology;
    bool primitiveRestart = false;
    VkViewport viewport;
    bool negativeOneToOne;
    bool depthClamp = false;
    VkRect2D scissor;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    VkPipelineColorBlendAttachmentState blend;
    std::array<float, 4> blendConstants;
    // Meaningful only with `depth.attached`.
    DepthTarget depthTarget;
    DepthState depth;
};

ShaderStages DecodeShaderStages(const QueueState& queue);
State DecodeState(const QueueState& queue);
// One color buffer (CB_COLOR<slot>_*): its surface, format, extent of the viewed mip and DCC metadata.
ColorTarget DecodeColorBuffer(const Registers& context, std::uint32_t slot);

// A draw in one of the CB's metadata modes (CB_COLOR_CONTROL.MODE): ELIMINATE_FAST_CLEAR writes the
// fast-clear color into the fast-cleared pixels it covers, DCC_DECOMPRESS stores the pixels it covers
// uncompressed; either way the shader's output is not what lands, and afterwards the covered pixels'
// texels read as the metadata said they did. Targets are the enabled color buffers with a format.
struct ColorMetadataPass {
    enum class Mode { EliminateFastClear, DccDecompress };
    Mode mode;
    std::vector<ColorTarget> targets;
};
// The metadata pass a draw's registers describe, or nothing for a draw in another CB mode. Throws
// when the pass does more than the metadata operation over whole targets (depth or stencil work,
// multisampling, a nonstandard ROP, a viewport or scissor short of a target).
std::optional<ColorMetadataPass> DecodeColorMetadataPass(const QueueState& queue);
// A DB metadata blit: DB_RENDER_CONTROL names HTILE operations only (DEPTH/STENCIL_COMPRESS_DISABLE,
// an in-place expansion of the surface; RESUMMARIZE_ENABLE, HTILE recomputed from it), the depth and
// stencil tests, writes and bounds are off, nothing reaches a color target (CB mode disable, or no
// enabled channel the shader exports), and the pixel shader is one the hardware does not run (no
// color, depth, stencil or mask export, no kill, no alpha-to-mask, EXEC_ON_NOOP clear), so its input
// registers (SPI_PS_INPUT_ENA/ADDR) need not be set. HTILE is not modeled (DepthTarget.hpp: the
// surface's memory is not coherent with its resident image, HTILE fills only mark clears), so such a
// blit changes nothing here. A register a rule needs that is absent makes it no blit.
bool DepthMetadataBlit(const QueueState& queue);
std::array<std::uint8_t, 8> ExportMappings(const State& state);
// The message DecodeState (or the pixel stage decode after it) would throw for the register rules
// this precheck covers, evaluated without exceptions before the draw is decoded; empty when they
// pass (DecodeState still checks everything). A register a rule needs that is absent is no verdict.
std::string DrawRejection(const QueueState& queue, bool indexed);

// The recording facade of the draw decoders (design_cpu_final M8, step 8a): every register read
// of DecodeShaderStages, DecodeState, DrawRejection, DecodePixelStageInfo and Driver::draw's
// program prepare goes through NoteRegisterRead, which appends the bank and offset to the calling
// thread's log while one is set (null in production; Driver::draw sets one on a miss under
// APS5_VERIFY_DRAW_RECIPE=1 and checks the log against DrawKeyRegisters).
enum class RegisterBank : std::uint8_t { Context, Shader, UserConfig, Count };
struct RegisterRead {
    RegisterBank bank;
    std::uint32_t offset;
};
std::vector<RegisterRead>*& RegisterReadLog();
inline void NoteRegisterRead(RegisterBank bank, std::uint32_t offset) {
    if (auto* log = RegisterReadLog()) log->push_back({bank, offset});
}
inline const char* RegisterBankName(RegisterBank bank) {
    return bank == RegisterBank::Context ? "context" : bank == RegisterBank::Shader ? "shader" : "user-config";
}

// The frozen table of the registers the draw decoders read, as [first, first + count) per bank in
// bank and offset order: the draw key (Driver::draw) hashes every present register of these
// ranges with its offset, so the decoded state, pixel and program inputs are a pure function of
// the key. A range covers whole register groups (the eight CB_COLOR slots, the 32 user words of
// each bank, the interpolator controls) whether or not a draw's slot count or user count reaches
// them: a stricter key, never a wrong one. A register the decoders read that the table lacks would
// be a wrong hit, which the facade's log finds (DrawKeyCovers) on every verified miss.
struct DrawKeyRange {
    RegisterBank bank;
    std::uint32_t first;
    std::uint32_t count;
};
inline constexpr std::array<DrawKeyRange, 46> DrawKeyRegisters{{
    // DB_RENDER_CONTROL, DB_DEPTH_VIEW, DB_RENDER_OVERRIDE, DB_HTILE_DATA_BASE, DB_DEPTH_SIZE_XY, DB_DEPTH_BOUNDS_MIN/MAX,
    // DB_STENCIL_CLEAR and DB_DEPTH_CLEAR with PA_SC_SCREEN_SCISSOR, DB_Z_INFO .. DB_STENCIL_WRITE_BASE,
    // the *_BASE_HI words.
    {RegisterBank::Context, 0x000, 1}, {RegisterBank::Context, 0x002, 2}, {RegisterBank::Context, 0x005, 1}, {RegisterBank::Context, 0x007, 7}, {RegisterBank::Context, 0x010, 6}, {RegisterBank::Context, 0x01a, 5},
    // The window offset/scissor and clip rect, the edge rule, the hardware screen offset,
    // CB_TARGET_MASK/CB_SHADER_MASK, the generic and viewport 0 scissors, the viewport 0 depth clamp,
    // the blend constants, DB_STENCIL_CONTROL and DB_STENCILREFMASK(_BF), the viewport 0 transform.
    {RegisterBank::Context, 0x080, 4}, {RegisterBank::Context, 0x08c, 4}, {RegisterBank::Context, 0x090, 2}, {RegisterBank::Context, 0x094, 2}, {RegisterBank::Context, 0x0b4, 2}, {RegisterBank::Context, 0x105, 4}, {RegisterBank::Context, 0x10b, 3}, {RegisterBank::Context, 0x10f, 6},
    // SPI_PS_INPUT_CNTL_0..31, SPI_PS_INPUT_ENA/ADDR, SPI_PS_IN_CONTROL, SPI_SHADER_POS/Z/COL_FORMAT,
    // CB_BLEND0..7_CONTROL, GE_MAX_OUTPUT_PER_SUBGROUP.
    {RegisterBank::Context, 0x191, 32}, {RegisterBank::Context, 0x1b3, 2}, {RegisterBank::Context, 0x1b6, 1}, {RegisterBank::Context, 0x1c3, 3}, {RegisterBank::Context, 0x1e0, 8}, {RegisterBank::Context, 0x1ff, 1},
    // DB_DEPTH_CONTROL .. PA_CL_VS_OUT_CNTL, PA_SC_MODE_CNTL_0/1, VGT_GS_MODE, VGT_GS_VERT_ITEMSIZE,
    // VGT_ESGS_RING_ITEMSIZE, VGT_SHADER_STAGES_EN/GS_ONCHIP, VGT_TF_PARAM/DB_ALPHA_TO_MASK, PA_SC_AA_CONFIG and
    // PA_SU_VTX_CNTL, the sample masks, PA_SC_CONSERVATIVE_RASTERIZATION_CNTL; PA_SU_POLY_OFFSET_*.
    {RegisterBank::Context, 0x200, 8}, {RegisterBank::Context, 0x292, 2}, {RegisterBank::Context, 0x29b, 1}, {RegisterBank::Context, 0x2ab, 1}, {RegisterBank::Context, 0x2ce, 1}, {RegisterBank::Context, 0x2d5, 2}, {RegisterBank::Context, 0x2db, 2}, {RegisterBank::Context, 0x2de, 6}, {RegisterBank::Context, 0x2f8, 2}, {RegisterBank::Context, 0x30e, 2}, {RegisterBank::Context, 0x313, 1},
    // CB_COLOR0..7_BASE .. DCC_BASE (15 words a slot), CB_COLOR0..7_BASE_EXT, DCC_BASE_EXT, ATTRIB2, ATTRIB3.
    {RegisterBank::Context, 0x318, 0x78}, {RegisterBank::Context, 0x390, 8}, {RegisterBank::Context, 0x3a8, 0x18},
    // The pixel program address, RSRC2 and user words; the geometry-back user pointer and program
    // address; the vertex/geometry-front RSRC1/RSRC2 and user words; the vertex program address;
    // the hull user pointer, program address, RSRC2 and user words; the local program address.
    {RegisterBank::Shader, 0x008, 0x24}, {RegisterBank::Shader, 0x082, 2}, {RegisterBank::Shader, 0x088, 2}, {RegisterBank::Shader, 0x08a, 0x22}, {RegisterBank::Shader, 0x0c8, 2}, {RegisterBank::Shader, 0x102, 2}, {RegisterBank::Shader, 0x108, 2}, {RegisterBank::Shader, 0x10b, 0x21}, {RegisterBank::Shader, 0x148, 2},
    // GE_PRIM_TYPE, GE_MULTI_PRIM_IB_RESET_EN, the geometry subgroup sizes.
    {RegisterBank::UserConfig, 0x242, 1}, {RegisterBank::UserConfig, 0x24b, 1}, {RegisterBank::UserConfig, 0x25b, 1},
}};
// Whether DrawKeyRegisters holds the read.
bool DrawKeyCovers(RegisterRead read);

}

#endif
