#ifndef CORE_SHADER_RECOMPILIER_RDNADECODER_INCLUDE_RDNADECODER_DIAGNOSTICSINK_HPP
#define CORE_SHADER_RECOMPILIER_RDNADECODER_INCLUDE_RDNADECODER_DIAGNOSTICSINK_HPP

#include <atomic>

namespace ShaderRecompiler {

// Typed facts the recompiler can report while it works. The host installs a sink (the run report does); without one every call
// is a no-op, so the recompiler keeps building and running on its own. Callbacks must not throw and must not block.
struct DiagnosticSink {
    // Whether anyone is collecting; the recompiler skips the work of producing diagnostics when this says no.
    bool (*enabled)() = nullptr;
    // what: 0 a shader was compiled, 1 a compile failed, 2 an existing compile was reused.
    void (*shader)(int what) = nullptr;
    // A guest instruction that cannot be translated. Unknown parts are null (names) or -1 (opcodeValue).
    void (*unsupportedInstruction)(const char* encoding, const char* opcode, long long opcodeValue, const char* file, int line) = nullptr;
    // status: 0 accepted, 1 rejected, 2 unknown. usage: 0 sampled, 1 storage, 2 color target, 3 depth target, 4 transfer, 5 other.
    void (*imageFormat)(unsigned format, const char* name, int status, int usage) = nullptr;
    // type: 0 uniform buffer, 1 storage buffer, 2 uniform texel buffer, 3 storage texel buffer, 4 sampled image, 5 storage image, 6 sampler.
    // usage: 0 read, 1 write, 2 atomic, 3 other.
    void (*descriptor)(int type, int status, int usage) = nullptr;
};

inline std::atomic<const DiagnosticSink*>& DiagnosticSinkSlot() {
    static std::atomic<const DiagnosticSink*> slot{nullptr};
    return slot;
}

inline void SetDiagnosticSink(const DiagnosticSink* sink) {
    DiagnosticSinkSlot().store(sink, std::memory_order_release);
}

inline const DiagnosticSink* Diagnostics() {
    return DiagnosticSinkSlot().load(std::memory_order_acquire);
}

}

#endif
