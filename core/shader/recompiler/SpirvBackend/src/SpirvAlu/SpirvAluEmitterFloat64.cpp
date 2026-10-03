#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>

namespace ShaderRecompiler {
namespace {

struct F64Bits {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
};

std::uint32_t TypeF64(SpirvEmitterState& state) {
    return state.module.Type(spv::OpTypeFloat, 64u);
}

std::uint32_t ConstantF64(SpirvEmitterState& state, std::uint64_t bits) {
    return state.module.Constant(spv::OpConstant, TypeF64(state), static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32u));
}

std::uint32_t ToF64(SpirvEmitterState& state, std::uint32_t bits) {
    return Unary(state, spv::OpBitcast, TypeF64(state), bits);
}

std::uint32_t FromF64(SpirvEmitterState& state, std::uint32_t value) {
    return Unary(state, spv::OpBitcast, TypeU64(state), value);
}

F64Bits Split(SpirvEmitterState& state, std::uint32_t bits) {
    F64Bits result{state.module.AllocateId(), state.module.AllocateId()};
    state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), result.low, bits, 0u);
    state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), result.high, bits, 1u);
    return result;
}

std::uint32_t Join(SpirvEmitterState& state, F64Bits bits) {
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeConstruct, TypeU64(state), result, bits.low, bits.high);
    return result;
}

F64Bits SelectBits(SpirvEmitterState& state, std::uint32_t condition, F64Bits trueValue, F64Bits falseValue) {
    return {Select(state, TypeU32(state), condition, trueValue.low, falseValue.low), Select(state, TypeU32(state), condition, trueValue.high, falseValue.high)};
}

std::uint32_t Exact(SpirvEmitterState& state, std::uint32_t opcode, std::uint32_t lhs, std::uint32_t rhs) {
    const auto result = Binary(state, opcode, TypeF64(state), lhs, rhs);
    state.module.AddAnnotation(spv::OpDecorate, result, spv::DecorationNoContraction);
    return result;
}

template<typename... TArguments>
std::uint32_t Glsl(SpirvEmitterState& state, std::uint32_t type, std::uint32_t opcode, TArguments... arguments) {
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpExtInst, type, result, GlslStd450(state), opcode, arguments...);
    return result;
}

struct F64Class {
    F64Bits bits;
    std::uint32_t nan = 0;
    std::uint32_t signalingNan = 0;
    std::uint32_t zero = 0;
    std::uint32_t negative = 0;
};

F64Class Classify(SpirvEmitterState& state, std::uint32_t value) {
    F64Class cls;
    cls.bits = Split(state, value);
    const auto absHigh = Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.high, ConstantU32(state, 0x7fffffffu));
    const auto aboveInf = Binary(state, spv::OpUGreaterThan, TypeBool(state), absHigh, ConstantU32(state, 0x7ff00000u));
    const auto infHigh = Binary(state, spv::OpIEqual, TypeBool(state), absHigh, ConstantU32(state, 0x7ff00000u));
    const auto lowNonzero = Binary(state, spv::OpINotEqual, TypeBool(state), cls.bits.low, ConstantU32(state, 0u));
    cls.nan = Binary(state, spv::OpLogicalOr, TypeBool(state), aboveInf, Binary(state, spv::OpLogicalAnd, TypeBool(state), infHigh, lowNonzero));
    const auto quietBit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.high, ConstantU32(state, 0x00080000u));
    const auto signaling = Binary(state, spv::OpIEqual, TypeBool(state), quietBit, ConstantU32(state, 0u));
    cls.signalingNan = Binary(state, spv::OpLogicalAnd, TypeBool(state), cls.nan, signaling);
    const auto magnitude = Binary(state, spv::OpBitwiseOr, TypeU32(state), absHigh, cls.bits.low);
    cls.zero = Binary(state, spv::OpIEqual, TypeBool(state), magnitude, ConstantU32(state, 0u));
    cls.negative = Binary(state, spv::OpINotEqual, TypeBool(state), absHigh, cls.bits.high);
    return cls;
}

F64Bits Quiet(SpirvEmitterState& state, F64Bits bits) {
    return {bits.low, Binary(state, spv::OpBitwiseOr, TypeU32(state), bits.high, ConstantU32(state, 0x00080000u))};
}

std::uint32_t QuietResult(SpirvEmitterState& state, std::uint32_t value) {
    const auto cls = Classify(state, FromF64(state, value));
    return Join(state, SelectBits(state, cls.nan, Quiet(state, cls.bits), cls.bits));
}

std::uint32_t MinMax(SpirvEmitterState& state, std::uint32_t lhs, std::uint32_t rhs, bool maxValue) {
    const auto a = Classify(state, lhs);
    const auto b = Classify(state, rhs);
    const auto ordered = Binary(state, maxValue ? spv::OpFOrdGreaterThan : spv::OpFOrdLessThan, TypeBool(state), ToF64(state, lhs), ToF64(state, rhs));
    auto result = SelectBits(state, ordered, a.bits, b.bits);
    const auto bothZero = Binary(state, spv::OpLogicalAnd, TypeBool(state), a.zero, b.zero);
    const auto zero = maxValue ? SelectBits(state, a.negative, b.bits, a.bits) : SelectBits(state, a.negative, a.bits, b.bits);
    result = SelectBits(state, bothZero, zero, result);
    result = SelectBits(state, a.nan, b.bits, result);
    result = SelectBits(state, b.nan, a.bits, result);
    result = SelectBits(state, b.signalingNan, Quiet(state, b.bits), result);
    result = SelectBits(state, a.signalingNan, Quiet(state, a.bits), result);
    return Join(state, result);
}

std::uint32_t Trunc(SpirvEmitterState& state, std::uint32_t value) {
    const auto cls = Classify(state, value);
    const auto exponent = Binary(state, spv::OpBitwiseAnd, TypeU32(state), Binary(state, spv::OpShiftRightLogical, TypeU32(state), cls.bits.high, ConstantU32(state, 20u)), ConstantU32(state, 0x7ffu));
    const auto fractionBits = Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 1075u), exponent);
    const auto highShift = Binary(state, spv::OpISub, TypeU32(state), Glsl(state, TypeU32(state), GLSLstd450UMax, fractionBits, ConstantU32(state, 32u)), ConstantU32(state, 32u));
    const auto lowShift = Glsl(state, TypeU32(state), GLSLstd450UMin, fractionBits, ConstantU32(state, 31u));
    const auto wideFraction = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), fractionBits, ConstantU32(state, 32u));
    const auto lowMask = Select(state, TypeU32(state), wideFraction, ConstantU32(state, 0u), Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 0xffffffffu), lowShift));
    const auto highMask = Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 0xffffffffu), highShift);
    const F64Bits masked{Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.low, lowMask), Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.high, highMask)};
    const F64Bits signedZero{ConstantU32(state, 0u), Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.high, ConstantU32(state, 0x80000000u))};
    const auto belowOne = Binary(state, spv::OpULessThan, TypeBool(state), exponent, ConstantU32(state, 1023u));
    const auto integral = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), exponent, ConstantU32(state, 1075u));
    auto result = SelectBits(state, belowOne, signedZero, masked);
    result = SelectBits(state, integral, cls.bits, result);
    return ToF64(state, Join(state, SelectBits(state, cls.nan, Quiet(state, cls.bits), result)));
}

std::uint32_t Floor(SpirvEmitterState& state, std::uint32_t bits) {
    const auto truncated = Trunc(state, bits);
    const auto below = Binary(state, spv::OpFOrdLessThan, TypeBool(state), ToF64(state, bits), truncated);
    return Select(state, TypeF64(state), below, Exact(state, spv::OpFSub, truncated, ConstantF64(state, 0x3ff0000000000000ull)), truncated);
}

}

std::uint32_t EmitFPAdd64(SpirvEmitterState& state, std::uint32_t arg0, std::uint32_t arg1) {
    return QuietResult(state, Exact(state, spv::OpFAdd, ToF64(state, arg0), ToF64(state, arg1)));
}

std::uint32_t EmitFPMul64(SpirvEmitterState& state, std::uint32_t arg0, std::uint32_t arg1) {
    return QuietResult(state, Exact(state, spv::OpFMul, ToF64(state, arg0), ToF64(state, arg1)));
}

std::uint32_t EmitFPFma64(SpirvEmitterState& state, std::uint32_t arg0, std::uint32_t arg1, std::uint32_t arg2) {
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpExtInst, TypeF64(state), result, GlslStd450(state), GLSLstd450Fma, ToF64(state, arg0), ToF64(state, arg1), ToF64(state, arg2));
    state.module.AddAnnotation(spv::OpDecorate, result, spv::DecorationNoContraction);
    return QuietResult(state, result);
}

std::uint32_t EmitFPMin64(SpirvEmitterState& state, std::uint32_t arg0, std::uint32_t arg1) {
    return MinMax(state, arg0, arg1, false);
}

std::uint32_t EmitFPMax64(SpirvEmitterState& state, std::uint32_t arg0, std::uint32_t arg1) {
    return MinMax(state, arg0, arg1, true);
}

std::uint32_t EmitFPSaturate64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto value = ToF64(state, arg0);
    const auto positive = Binary(state, spv::OpFOrdGreaterThan, TypeBool(state), value, ConstantF64(state, 0u));
    const auto belowOne = Binary(state, spv::OpFOrdLessThan, TypeBool(state), value, ConstantF64(state, 0x3ff0000000000000ull));
    const auto upper = Select(state, TypeF64(state), belowOne, value, ConstantF64(state, 0x3ff0000000000000ull));
    return FromF64(state, Select(state, TypeF64(state), positive, upper, ConstantF64(state, 0u)));
}

std::uint32_t EmitFPLdexp64(SpirvEmitterState& state, std::uint32_t arg0, std::uint32_t arg1) {
    const auto cls = Classify(state, arg0);
    const auto exponentOf = [&](std::uint32_t high) {
        return Binary(state, spv::OpBitwiseAnd, TypeU32(state), Binary(state, spv::OpShiftRightLogical, TypeU32(state), high, ConstantU32(state, 20u)), ConstantU32(state, 0x7ffu));
    };
    const auto subnormal = Binary(state, spv::OpIEqual, TypeBool(state), exponentOf(cls.bits.high), ConstantU32(state, 0u));
    const auto scaled = Exact(state, spv::OpFMul, ToF64(state, arg0), ConstantF64(state, 0x43f0000000000000ull));
    const auto normal = Split(state, FromF64(state, Select(state, TypeF64(state), subnormal, scaled, ToF64(state, arg0))));
    const auto bias = Select(state, TypeU32(state), subnormal, ConstantU32(state, 1022u + 64u), ConstantU32(state, 1022u));
    const auto exponent = Glsl(state, TypeI32(state), GLSLstd450SClamp, Unary(state, spv::OpBitcast, TypeI32(state), arg1), ConstantI32(state, -2200), ConstantI32(state, 2200));
    const auto shift = Binary(state, spv::OpISub, TypeU32(state), exponentOf(normal.high), bias);
    const auto total = Glsl(state, TypeI32(state), GLSLstd450SClamp, Binary(state, spv::OpIAdd, TypeI32(state), Unary(state, spv::OpBitcast, TypeI32(state), shift), exponent), ConstantI32(state, -1100), ConstantI32(state, 1100));
    const auto mantissa = Join(state, {normal.low, Binary(state, spv::OpBitwiseOr, TypeU32(state), Binary(state, spv::OpBitwiseAnd, TypeU32(state), normal.high, ConstantU32(state, 0x800fffffu)), ConstantU32(state, 1022u << 20u))});
    const auto power = [&](std::uint32_t value) {
        const auto biased = Binary(state, spv::OpIAdd, TypeI32(state), value, ConstantI32(state, 1023));
        const auto high = Binary(state, spv::OpShiftLeftLogical, TypeU32(state), Unary(state, spv::OpBitcast, TypeU32(state), biased), ConstantU32(state, 20u));
        return ToF64(state, Join(state, {ConstantU32(state, 0u), high}));
    };
    const auto first = Glsl(state, TypeI32(state), GLSLstd450SClamp, total, ConstantI32(state, -1021), ConstantI32(state, 1023));
    const auto second = Binary(state, spv::OpISub, TypeI32(state), total, first);
    const auto exact = Exact(state, spv::OpFMul, ToF64(state, mantissa), power(first));
    const auto result = Split(state, FromF64(state, Exact(state, spv::OpFMul, exact, power(Glsl(state, TypeI32(state), GLSLstd450SMax, second, ConstantI32(state, -60))))));
    const auto special = Binary(state, spv::OpIEqual, TypeBool(state), exponentOf(cls.bits.high), ConstantU32(state, 0x7ffu));
    const auto passthrough = Binary(state, spv::OpLogicalOr, TypeBool(state), cls.zero, special);
    return Join(state, SelectBits(state, passthrough, SelectBits(state, cls.nan, Quiet(state, cls.bits), cls.bits), result));
}

std::uint32_t EmitFPRoundEven64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto cls = Classify(state, arg0);
    const auto value = ToF64(state, arg0);
    const auto magnitude = Glsl(state, TypeF64(state), GLSLstd450FAbs, value);
    const auto shift = ConstantF64(state, 0x4330000000000000ull);
    const auto rounded = Split(state, FromF64(state, Exact(state, spv::OpFSub, Exact(state, spv::OpFAdd, magnitude, shift), shift)));
    const auto sign = Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.high, ConstantU32(state, 0x80000000u));
    const F64Bits signedRounded{rounded.low, Binary(state, spv::OpBitwiseOr, TypeU32(state), rounded.high, sign)};
    const auto integral = Binary(state, spv::OpFOrdGreaterThanEqual, TypeBool(state), magnitude, shift);
    const auto result = SelectBits(state, integral, cls.bits, signedRounded);
    return Join(state, SelectBits(state, cls.nan, Quiet(state, cls.bits), result));
}

std::uint32_t EmitFPFloor64(SpirvEmitterState& state, std::uint32_t arg0) {
    return FromF64(state, Floor(state, arg0));
}

std::uint32_t EmitFPCeil64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto truncated = Trunc(state, arg0);
    const auto above = Binary(state, spv::OpFOrdGreaterThan, TypeBool(state), ToF64(state, arg0), truncated);
    return FromF64(state, Select(state, TypeF64(state), above, Exact(state, spv::OpFAdd, truncated, ConstantF64(state, 0x3ff0000000000000ull)), truncated));
}

std::uint32_t EmitFPTrunc64(SpirvEmitterState& state, std::uint32_t arg0) {
    return FromF64(state, Trunc(state, arg0));
}

std::uint32_t EmitFPFract64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto value = ToF64(state, arg0);
    const auto fraction = Exact(state, spv::OpFSub, value, Floor(state, arg0));
    const auto belowOne = ConstantF64(state, 0x3fefffffffffffffull);
    const auto overflow = Binary(state, spv::OpFOrdGreaterThan, TypeBool(state), fraction, belowOne);
    return QuietResult(state, Select(state, TypeF64(state), overflow, belowOne, fraction));
}

std::uint32_t EmitFPFrexpMant64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto cls = Classify(state, arg0);
    const auto split = Glsl(state, state.module.Type(spv::OpTypeStruct, TypeF64(state), TypeI32(state)), GLSLstd450FrexpStruct, ToF64(state, arg0));
    const auto mantissa = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, TypeF64(state), mantissa, split, 0u);
    const auto exponentBits = Binary(state, spv::OpBitwiseAnd, TypeU32(state), cls.bits.high, ConstantU32(state, 0x7ff00000u));
    const auto special = Binary(state, spv::OpIEqual, TypeBool(state), exponentBits, ConstantU32(state, 0x7ff00000u));
    const auto passthrough = SelectBits(state, cls.nan, Quiet(state, cls.bits), cls.bits);
    return Join(state, SelectBits(state, special, passthrough, Split(state, FromF64(state, mantissa))));
}

std::uint32_t EmitFPFrexpExp64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto bits = Split(state, arg0);
    const auto split = Glsl(state, state.module.Type(spv::OpTypeStruct, TypeF64(state), TypeI32(state)), GLSLstd450FrexpStruct, ToF64(state, arg0));
    const auto exponent = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, TypeI32(state), exponent, split, 1u);
    const auto exponentBits = Binary(state, spv::OpBitwiseAnd, TypeU32(state), bits.high, ConstantU32(state, 0x7ff00000u));
    const auto special = Binary(state, spv::OpIEqual, TypeBool(state), exponentBits, ConstantU32(state, 0x7ff00000u));
    return Select(state, TypeU32(state), special, ConstantU32(state, 0u), Unary(state, spv::OpBitcast, TypeU32(state), exponent));
}

std::uint32_t EmitConvertF32F64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto converted = Unary(state, spv::OpBitcast, TypeU32(state), Unary(state, spv::OpFConvert, TypeF32(state), ToF64(state, arg0)));
    const auto quiet = Binary(state, spv::OpBitwiseOr, TypeU32(state), converted, ConstantU32(state, 0x00400000u));
    return Unary(state, spv::OpBitcast, TypeF32(state), Select(state, TypeU32(state), Classify(state, arg0).nan, quiet, converted));
}

std::uint32_t EmitConvertF64F32(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto converted = Split(state, FromF64(state, Unary(state, spv::OpFConvert, TypeF64(state), arg0)));
    const auto nan = Binary(state, spv::OpFUnordNotEqual, TypeBool(state), arg0, arg0);
    return Join(state, SelectBits(state, nan, Quiet(state, converted), converted));
}

std::uint32_t EmitConvertF64S32(SpirvEmitterState& state, std::uint32_t arg0) {
    return FromF64(state, Unary(state, spv::OpConvertSToF, TypeF64(state), Unary(state, spv::OpBitcast, TypeI32(state), arg0)));
}

std::uint32_t EmitConvertF64U32(SpirvEmitterState& state, std::uint32_t arg0) {
    return FromF64(state, Unary(state, spv::OpConvertUToF, TypeF64(state), arg0));
}

std::uint32_t EmitConvertS32F64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto value = ToF64(state, arg0);
    const auto low = Binary(state, spv::OpFOrdLessThanEqual, TypeBool(state), value, ConstantF64(state, 0xc1e0000000000000ull));
    const auto high = Binary(state, spv::OpFOrdGreaterThanEqual, TypeBool(state), value, ConstantF64(state, 0x41e0000000000000ull));
    const auto ordered = Binary(state, spv::OpFOrdEqual, TypeBool(state), value, value);
    const auto safe = Select(state, TypeF64(state), ordered, value, ConstantF64(state, 0u));
    const auto clamped = Trunc(state, FromF64(state, Select(state, TypeF64(state), Binary(state, spv::OpLogicalOr, TypeBool(state), low, high), ConstantF64(state, 0u), safe)));
    const auto converted = Unary(state, spv::OpBitcast, TypeU32(state), Unary(state, spv::OpConvertFToS, TypeI32(state), clamped));
    const auto saturated = Select(state, TypeU32(state), high, ConstantU32(state, 0x7fffffffu), converted);
    return Select(state, TypeU32(state), low, ConstantU32(state, 0x80000000u), saturated);
}

std::uint32_t EmitConvertU32F64(SpirvEmitterState& state, std::uint32_t arg0) {
    const auto value = ToF64(state, arg0);
    const auto low = Binary(state, spv::OpFUnordLessThanEqual, TypeBool(state), value, ConstantF64(state, 0u));
    const auto high = Binary(state, spv::OpFOrdGreaterThanEqual, TypeBool(state), value, ConstantF64(state, 0x41f0000000000000ull));
    const auto clamped = Trunc(state, FromF64(state, Select(state, TypeF64(state), Binary(state, spv::OpLogicalOr, TypeBool(state), low, high), ConstantF64(state, 0u), value)));
    const auto converted = Unary(state, spv::OpConvertFToU, TypeU32(state), clamped);
    const auto saturated = Select(state, TypeU32(state), high, ConstantU32(state, 0xffffffffu), converted);
    return Select(state, TypeU32(state), low, ConstantU32(state, 0u), saturated);
}

}
