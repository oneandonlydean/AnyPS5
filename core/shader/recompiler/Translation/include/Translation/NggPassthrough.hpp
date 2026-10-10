#ifndef CORE_SHADER_RECOMPILIER_TRANSLATION_INCLUDE_TRANSLATION_NGGPASSTHROUGH_HPP
#define CORE_SHADER_RECOMPILIER_TRANSLATION_INCLUDE_TRANSLATION_NGGPASSTHROUGH_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ShaderRecompiler {

struct NggSubgroupLimits {
    std::uint32_t waveSize = 64;
    std::uint32_t vertices = 0;
    std::uint32_t primitives = 0;
};

[[nodiscard]] std::optional<std::string> NggPassthroughSubgroupDependence(std::span<const std::uint32_t> code, std::uint32_t userDataCount, const NggSubgroupLimits& limits);

}

#endif
