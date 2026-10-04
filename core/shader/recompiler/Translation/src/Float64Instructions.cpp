#include "Translation/TranslationContext.hpp"
#include <array>
#include <cstdint>

namespace ShaderRecompiler {

IrU64 TranslationContext::readF64(const RdnaOperand& operand) {
    const std::array<IrU32, 2> bits = readF64Bits(operand);
    return IrU64(ir.ConstructU64(bits[0].Value(), bits[1].Value()));
}

bool TranslationContext::float64Operation(const RdnaInstruction& inst, IrOpcode opcode) {
    const std::size_t count = IrOpcodeOperandCount(opcode);
    std::array<IrValue*, 3> args{};
    for (std::uint32_t index = 0u; index < count; ++index) {
        const RdnaOperand& operand = sourceAt(inst, index);
        switch (IrOpcodeArgumentType(opcode, index)) {
            case IrType::U64: args[index] = &readF64(operand).Value(); break;
            case IrType::F32: args[index] = readOperand(operand, IrType::F32); break;
            default: args[index] = &readU32(operand).Value(); break;
        }
    }
    const IrType type = IrOpcodeType(opcode);
    IrValue* result = count == 1u ? &ir.Emit(opcode, type, {args[0]}) : count == 2u ? &ir.Emit(opcode, type, {args[0], args[1]}) : &ir.Emit(opcode, type, {args[0], args[1], args[2]});
    RdnaOperand destination = inst.destination;
    destination.omod = 0u;
    if (type == IrType::U64 && destination.clamp) {
        result = &ir.Emit(IrOpcode::FPSaturate64, IrType::U64, {result});
    }
    writeOperand(destination, result);
    return true;
}

}
