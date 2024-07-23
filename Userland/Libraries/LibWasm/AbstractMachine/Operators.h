/*
 * Copyright (c) 2021-2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Result.h>
#include <AK/SIMD.h>
#include <AK/SIMDExtras.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWasm/Types.h>
#include <limits.h>
#include <math.h>

namespace Wasm::Operators {

using namespace AK::SIMD;

#define DEFINE_BINARY_OPERATOR(Name, operation) \
    struct Name {                               \
        template<typename Lhs, typename Rhs>    \
        auto operator()(Lhs lhs, Rhs rhs) const \
        {                                       \
            return lhs operation rhs;           \
        }                                       \
                                                \
        static StringView name()                \
        {                                       \
            return #operation##sv;              \
        }                                       \
    }

DEFINE_BINARY_OPERATOR(Equals, ==);
DEFINE_BINARY_OPERATOR(NotEquals, !=);
DEFINE_BINARY_OPERATOR(GreaterThan, >);
DEFINE_BINARY_OPERATOR(LessThan, <);
DEFINE_BINARY_OPERATOR(LessThanOrEquals, <=);
DEFINE_BINARY_OPERATOR(GreaterThanOrEquals, >=);
DEFINE_BINARY_OPERATOR(Add, +);
DEFINE_BINARY_OPERATOR(Subtract, -);
DEFINE_BINARY_OPERATOR(Multiply, *);
DEFINE_BINARY_OPERATOR(BitAnd, &);
DEFINE_BINARY_OPERATOR(BitOr, |);
DEFINE_BINARY_OPERATOR(BitXor, ^);

#undef DEFINE_BINARY_OPERATOR

struct Divide {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        if constexpr (IsFloatingPoint<Lhs>) {
            return lhs / rhs;
        } else {
            Checked value(lhs);
            value /= rhs;
            if (value.has_overflow())
                return AK::ErrorOr<Lhs, StringView>("Integer division overflow"sv);
            return AK::ErrorOr<Lhs, StringView>(value.value());
        }
    }

    static StringView name() { return "/"sv; }
};

struct Modulo {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        if (rhs == 0)
            return AK::ErrorOr<Lhs, StringView>("Integer division overflow"sv);
        if constexpr (IsSigned<Lhs>) {
            if (rhs == -1)
                return AK::ErrorOr<Lhs, StringView>(0); // Spec weirdness right here, signed division overflow is ignored.
        }
        return AK::ErrorOr<Lhs, StringView>(lhs % rhs);
    }

    static StringView name() { return "%"sv; }
};

struct Average {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        return static_cast<Lhs>((lhs + rhs + 1) / 2);
    }

    static StringView name() { return "avgr"sv; }
};

struct Q15Mul {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        return (lhs * rhs + 0x4000) >> 15;
    }

    static StringView name() { return "q15mul"sv; }
};

struct BitShiftLeft {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const { return lhs << (rhs % (sizeof(lhs) * 8)); }

    static StringView name() { return "<<"sv; }
};

struct BitShiftRight {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const { return lhs >> (rhs % (sizeof(lhs) * 8)); }

    static StringView name() { return ">>"sv; }
};

struct BitAndNot {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const { return lhs & ~rhs; }

    static StringView name() { return "andnot"sv; }
};

struct BitNot {
    template<typename Lhs>
    auto operator()(Lhs lhs) const { return ~lhs; }

    static StringView name() { return "~"sv; }
};

struct BitRotateLeft {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        // generates a single 'rol' instruction if shift is positive
        // otherwise generate a `ror`
        auto const mask = CHAR_BIT * sizeof(Lhs) - 1;
        rhs &= mask;
        return (lhs << rhs) | (lhs >> ((-rhs) & mask));
    }

    static StringView name() { return "rotate_left"sv; }
};

struct BitRotateRight {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        // generates a single 'ror' instruction if shift is positive
        // otherwise generate a `rol`
        auto const mask = CHAR_BIT * sizeof(Lhs) - 1;
        rhs &= mask;
        return (lhs >> rhs) | (lhs << ((-rhs) & mask));
    }

    static StringView name() { return "rotate_right"sv; }
};

template<size_t VectorSize, template<typename> typename SetSign = MakeSigned>
struct VectorAllTrue {
    auto operator()(u128 c) const
    {
        using ElementType = NativeIntegralType<128 / VectorSize>;

        auto any_false = bit_cast<Native128ByteVectorOf<ElementType, SetSign>>(c) == 0;
        return bit_cast<u128>(any_false) == 0;
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).all_true"sv;
        case 8:
            return "vec(16x8).all_true"sv;
        case 4:
            return "vec(32x4).all_true"sv;
        case 2:
            return "vec(64x2).all_true"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize>
struct VectorShiftLeft {
    auto operator()(u128 lhs, i32 rhs) const
    {
        auto shift_value = rhs % (sizeof(lhs) * 8 / VectorSize);
        return bit_cast<u128>(bit_cast<Native128ByteVectorOf<NativeIntegralType<128 / VectorSize>, MakeUnsigned>>(lhs) << shift_value);
    }
    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16)<<"sv;
        case 8:
            return "vec(16x8)<<"sv;
        case 4:
            return "vec(32x4)<<"sv;
        case 2:
            return "vec(64x2)<<"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, template<typename> typename SetSign>
struct VectorShiftRight {
    auto operator()(u128 lhs, i32 rhs) const
    {
        auto shift_value = rhs % (sizeof(lhs) * 8 / VectorSize);
        return bit_cast<u128>(bit_cast<Native128ByteVectorOf<SetSign<NativeIntegralType<128 / VectorSize>>, SetSign>>(lhs) >> shift_value);
    }
    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16)>>"sv;
        case 8:
            return "vec(16x8)>>"sv;
        case 4:
            return "vec(32x4)>>"sv;
        case 2:
            return "vec(64x2)>>"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

struct VectorSwizzle {
    auto operator()(u128 c1, u128 c2) const
    {
        // https://webassembly.github.io/spec/core/bikeshed/#-mathsfi8x16hrefsyntax-instr-vecmathsfswizzle%E2%91%A0
        auto i = bit_cast<Native128ByteVectorOf<i8, MakeSigned>>(c1);
        auto j = bit_cast<Native128ByteVectorOf<i8, MakeSigned>>(c2);
        auto result = shuffle_or_0(i, j);
        return bit_cast<u128>(result);
    }
    static StringView name() { return "vec(8x16).swizzle"sv; }
};

template<size_t VectorSize, template<typename> typename SetSign>
struct VectorExtractLane {
    size_t lane;

    auto operator()(u128 c) const
    {
        auto result = bit_cast<Native128ByteVectorOf<NativeIntegralType<128 / VectorSize>, SetSign>>(c);
        return result[lane];
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).extract_lane"sv;
        case 8:
            return "vec(16x8).extract_lane"sv;
        case 4:
            return "vec(32x4).extract_lane"sv;
        case 2:
            return "vec(64x2).extract_lane"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize>
struct VectorExtractLaneFloat {
    size_t lane;

    auto operator()(u128 c) const
    {
        auto result = bit_cast<NativeFloatingVectorType<128 / VectorSize, VectorSize>>(c);
        return result[lane];
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).extract_lane"sv;
        case 8:
            return "vec(16x8).extract_lane"sv;
        case 4:
            return "vec(32x4).extract_lane"sv;
        case 2:
            return "vec(64x2).extract_lane"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename TrueValueType = NativeIntegralType<128 / VectorSize>>
struct VectorReplaceLane {
    size_t lane;
    using ValueType = Conditional<IsFloatingPoint<TrueValueType>, NativeFloatingType<128 / VectorSize>, NativeIntegralType<128 / VectorSize>>;

    auto operator()(u128 c, TrueValueType value) const
    {
        auto result = bit_cast<Native128ByteVectorOf<ValueType, MakeUnsigned>>(c);
        result[lane] = static_cast<ValueType>(value);
        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).replace_lane"sv;
        case 8:
            return "vec(16x8).replace_lane"sv;
        case 4:
            return "vec(32x4).replace_lane"sv;
        case 2:
            return "vec(64x2).replace_lane"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename Op, template<typename> typename SetSign = MakeSigned>
struct VectorCmpOp {
    auto operator()(u128 c1, u128 c2) const
    {
        using ElementType = NativeIntegralType<128 / VectorSize>;
        auto result = bit_cast<Native128ByteVectorOf<ElementType, SetSign>>(c1);
        auto other = bit_cast<Native128ByteVectorOf<ElementType, SetSign>>(c2);
        Op op;
        for (size_t i = 0; i < VectorSize; ++i) {
            SetSign<ElementType> lhs = result[i];
            SetSign<ElementType> rhs = other[i];
            result[i] = op(lhs, rhs) ? static_cast<MakeUnsigned<ElementType>>(-1) : 0;
        }
        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).cmp"sv;
        case 8:
            return "vec(16x8).cmp"sv;
        case 4:
            return "vec(32x4).cmp"sv;
        case 2:
            return "vec(64x2).cmp"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename Op>
struct VectorFloatCmpOp {
    auto operator()(u128 c1, u128 c2) const
    {
        auto first = bit_cast<NativeFloatingVectorType<128, VectorSize, NativeFloatingType<128 / VectorSize>>>(c1);
        auto other = bit_cast<NativeFloatingVectorType<128, VectorSize, NativeFloatingType<128 / VectorSize>>>(c2);
        using ElementType = NativeIntegralType<128 / VectorSize>;
        Native128ByteVectorOf<ElementType, MakeUnsigned> result;
        Op op;
        for (size_t i = 0; i < VectorSize; ++i)
            result[i] = op(first[i], other[i]) ? static_cast<ElementType>(-1) : 0;
        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 4:
            return "vecf(32x4).cmp"sv;
        case 2:
            return "vecf(64x2).cmp"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

struct Minimum {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        if constexpr (IsFloatingPoint<Lhs> || IsFloatingPoint<Rhs>) {
            if (isnan(lhs) || isnan(rhs)) {
                return isnan(lhs) ? lhs : rhs;
            }
            if (lhs == 0 && rhs == 0) {
                return signbit(lhs) ? lhs : rhs;
            }
        }
        return min(lhs, rhs);
    }

    static StringView name() { return "minimum"sv; }
};

struct Maximum {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        if constexpr (IsFloatingPoint<Lhs> || IsFloatingPoint<Rhs>) {
            if (isnan(lhs) || isnan(rhs)) {
                return isnan(lhs) ? lhs : rhs;
            }
            if (lhs == 0 && rhs == 0) {
                return signbit(lhs) ? rhs : lhs;
            }
        }
        return max(lhs, rhs);
    }

    static StringView name() { return "maximum"sv; }
};

struct PseudoMinimum {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        return rhs < lhs ? rhs : lhs;
    }

    static StringView name() { return "pseudo_minimum"sv; }
};

struct PseudoMaximum {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        return lhs < rhs ? rhs : lhs;
    }

    static StringView name() { return "pseudo_maximum"sv; }
};

struct CopySign {
    template<typename Lhs, typename Rhs>
    auto operator()(Lhs lhs, Rhs rhs) const
    {
        if constexpr (IsSame<Lhs, float>)
            return copysignf(lhs, rhs);
        else if constexpr (IsSame<Lhs, double>)
            return copysign(lhs, rhs);
        else
            static_assert(DependentFalse<Lhs, Rhs>, "Invalid types to CopySign");
    }

    static StringView name() { return "copysign"sv; }
};

// Unary

struct EqualsZero {
    template<typename Lhs>
    auto operator()(Lhs lhs) const { return lhs == 0; }

    static StringView name() { return "== 0"sv; }
};

struct CountLeadingZeros {
    template<typename Lhs>
    i32 operator()(Lhs lhs) const
    {
        if (lhs == 0)
            return sizeof(Lhs) * CHAR_BIT;

        if constexpr (sizeof(Lhs) == 4 || sizeof(Lhs) == 8)
            return count_leading_zeroes(MakeUnsigned<Lhs>(lhs));
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "clz"sv; }
};

struct CountTrailingZeros {
    template<typename Lhs>
    i32 operator()(Lhs lhs) const
    {
        if (lhs == 0)
            return sizeof(Lhs) * CHAR_BIT;

        if constexpr (sizeof(Lhs) == 4 || sizeof(Lhs) == 8)
            return count_trailing_zeroes(MakeUnsigned<Lhs>(lhs));
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "ctz"sv; }
};

struct PopCount {
    template<typename Lhs>
    auto operator()(Lhs lhs) const
    {
        if constexpr (sizeof(Lhs) == 1 || sizeof(Lhs) == 2 || sizeof(Lhs) == 4 || sizeof(Lhs) == 8)
            return popcount(MakeUnsigned<Lhs>(lhs));
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "popcnt"sv; }
};

struct Absolute {
    template<typename Lhs>
    Lhs operator()(Lhs lhs) const
    {
        if constexpr (IsFloatingPoint<Lhs>)
            return AK::abs(lhs);

        if constexpr (IsSigned<Lhs>) {
            if (lhs == NumericLimits<Lhs>::min())
                return NumericLimits<Lhs>::min(); // Return the negation of _i_ modulo 2^N: https://www.w3.org/TR/wasm-core-2/#-hrefop-iabsmathrmiabs_n-i step 3
        }

        return AK::abs(lhs);
    }

    static StringView name() { return "abs"sv; }
};

struct Negate {
    template<typename Lhs>
    Lhs operator()(Lhs lhs) const
    {
        if constexpr (IsFloatingPoint<Lhs>)
            return -lhs;

        if constexpr (IsSigned<Lhs>) {
            if (lhs == NumericLimits<Lhs>::min())
                return NumericLimits<Lhs>::min(); // Return the negation of _i_ modulo 2^N: https://www.w3.org/TR/wasm-core-2/#-hrefop-iabsmathrmiabs_n-i step 3
        }

        return -lhs;
    }

    static StringView name() { return "== 0"sv; }
};

struct Ceil {
    template<typename Lhs>
    auto operator()(Lhs lhs) const
    {
        if constexpr (IsSame<Lhs, float>)
            return ceilf(lhs);
        else if constexpr (IsSame<Lhs, double>)
            return ceil(lhs);
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "ceil"sv; }
};

template<size_t VectorSize, typename Op, template<typename> typename SetSign = MakeSigned>
struct VectorIntegerExtOpPairwise {
    auto operator()(u128 c) const
    {
        using VectorResult = NativeVectorType<128 / VectorSize, VectorSize, SetSign>;
        using VectorInput = NativeVectorType<128 / (VectorSize * 2), VectorSize * 2, SetSign>;
        auto vector = bit_cast<VectorInput>(c);
        VectorResult result;
        Op op;

        // FIXME: Find a way to not loop here
        for (size_t i = 0; i < VectorSize; ++i) {
            result[i] = op(vector[i * 2], vector[(i * 2) + 1]);
        }

        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 8:
            return "vec(16x8).ext_op_pairwise(8x16)"sv;
        case 4:
            return "vec(32x4).ext_op_pairwise(16x8)"sv;
        case 2:
            return "vec(64x2).ext_op_pairwise(32x4)"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

enum class VectorExt {
    High,
    Low,
};

template<size_t VectorSize, VectorExt Mode, template<typename> typename SetSign = MakeSigned>
struct VectorIntegerExt {
    auto operator()(u128 c) const
    {
        using VectorResult = NativeVectorType<128 / VectorSize, VectorSize, SetSign>;
        using VectorInput = NativeVectorType<128 / (VectorSize * 2), VectorSize * 2, SetSign>;
        auto vector = bit_cast<VectorInput>(c);
        VectorResult result;

        // FIXME: Find a way to not loop here
        for (size_t i = 0; i < VectorSize; ++i) {
            if constexpr (Mode == VectorExt::High)
                result[i] = vector[VectorSize + i];
            else if constexpr (Mode == VectorExt::Low)
                result[i] = vector[i];
            else
                VERIFY_NOT_REACHED();
        }

        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 8:
            return "vec(16x8).ext(8x16)"sv;
        case 4:
            return "vec(32x4).ext(16x8)"sv;
        case 2:
            return "vec(64x2).ext(32x4)"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename Op, VectorExt Mode, template<typename> typename SetSign = MakeSigned>
struct VectorIntegerExtOp {
    auto operator()(u128 lhs, u128 rhs) const
    {
        using VectorResult = NativeVectorType<128 / VectorSize, VectorSize, SetSign>;
        using VectorInput = NativeVectorType<128 / (VectorSize * 2), VectorSize * 2, SetSign>;
        auto first = bit_cast<VectorInput>(lhs);
        auto second = bit_cast<VectorInput>(rhs);
        VectorResult result;
        Op op;

        using ResultType = SetSign<NativeIntegralType<128 / VectorSize>>;
        // FIXME: Find a way to not loop here
        for (size_t i = 0; i < VectorSize; ++i) {
            if constexpr (Mode == VectorExt::High) {
                ResultType a = first[VectorSize + i];
                ResultType b = second[VectorSize + i];
                result[i] = op(a, b);
            } else if constexpr (Mode == VectorExt::Low) {
                ResultType a = first[i];
                ResultType b = second[i];
                result[i] = op(a, b);
            } else
                VERIFY_NOT_REACHED();
        }

        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 8:
            return "vec(16x8).ext_op(8x16)"sv;
        case 4:
            return "vec(32x4).ext_op(16x8)"sv;
        case 2:
            return "vec(64x2).ext_op(32x4)"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename Op, template<typename> typename SetSign = MakeSigned>
struct VectorIntegerBinaryOp {
    auto operator()(u128 lhs, u128 rhs) const
    {
        using VectorType = NativeVectorType<128 / VectorSize, VectorSize, SetSign>;
        auto first = bit_cast<VectorType>(lhs);
        auto second = bit_cast<VectorType>(rhs);
        VectorType result;
        Op op;

        // FIXME: Find a way to not loop here
        for (size_t i = 0; i < VectorSize; ++i) {
            result[i] = op(first[i], second[i]);
        }

        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).binary_op"sv;
        case 8:
            return "vec(16x8).binary_op"sv;
        case 4:
            return "vec(32x4).binary_op"sv;
        case 2:
            return "vec(64x2).binary_op"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize>
struct VectorBitmask {
    auto operator()(u128 lhs) const
    {
        using VectorType = NativeVectorType<128 / VectorSize, VectorSize, MakeSigned>;
        auto value = bit_cast<VectorType>(lhs);
        u32 result = 0;

        for (size_t i = 0; i < VectorSize; ++i)
            result |= static_cast<u32>(value[i] < 0) << i;

        return result;
    }

    static StringView name() { return "bitmask"sv; }
};

template<size_t VectorSize>
struct VectorDotProduct {
    auto operator()(u128 lhs, u128 rhs) const
    {
        using VectorInput = NativeVectorType<128 / (VectorSize * 2), VectorSize * 2, MakeSigned>;
        using VectorResult = NativeVectorType<128 / VectorSize, VectorSize, MakeSigned>;
        auto v1 = bit_cast<VectorInput>(lhs);
        auto v2 = bit_cast<VectorInput>(rhs);
        VectorResult result;

        using ResultType = MakeUnsigned<NativeIntegralType<128 / VectorSize>>;
        for (size_t i = 0; i < VectorSize; ++i) {
            ResultType low = v1[i * 2] * v2[i * 2];
            ResultType high = v1[(i * 2) + 1] * v2[(i * 2) + 1];
            result[i] = low + high;
        }

        return bit_cast<u128>(result);
    }

    static StringView name() { return "dot"sv; }
};

template<size_t VectorSize, typename Element>
struct VectorNarrow {
    auto operator()(u128 lhs, u128 rhs) const
    {
        using VectorInput = NativeVectorType<128 / (VectorSize / 2), VectorSize / 2, MakeSigned>;
        using VectorResult = NativeVectorType<128 / VectorSize, VectorSize, MakeUnsigned>;
        auto v1 = bit_cast<VectorInput>(lhs);
        auto v2 = bit_cast<VectorInput>(rhs);
        VectorResult result;

        for (size_t i = 0; i < (VectorSize / 2); ++i) {
            if (v1[i] <= NumericLimits<Element>::min())
                result[i] = NumericLimits<Element>::min();
            else if (v1[i] >= NumericLimits<Element>::max())
                result[i] = NumericLimits<Element>::max();
            else
                result[i] = v1[i];
        }
        for (size_t i = 0; i < (VectorSize / 2); ++i) {
            if (v2[i] <= NumericLimits<Element>::min())
                result[i + VectorSize / 2] = NumericLimits<Element>::min();
            else if (v2[i] >= NumericLimits<Element>::max())
                result[i + VectorSize / 2] = NumericLimits<Element>::max();
            else
                result[i + VectorSize / 2] = v2[i];
        }

        return bit_cast<u128>(result);
    }

    static StringView name() { return "narrow"sv; }
};

template<size_t VectorSize, typename Op, template<typename> typename SetSign = MakeSigned>
struct VectorIntegerUnaryOp {
    auto operator()(u128 lhs) const
    {
        using VectorType = NativeVectorType<128 / VectorSize, VectorSize, SetSign>;
        auto value = bit_cast<VectorType>(lhs);
        VectorType result;
        Op op;

        // FIXME: Find a way to not loop here
        for (size_t i = 0; i < VectorSize; ++i) {
            result[i] = op(value[i]);
        }

        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 16:
            return "vec(8x16).unary_op"sv;
        case 8:
            return "vec(16x8).unary_op"sv;
        case 4:
            return "vec(32x4).unary_op"sv;
        case 2:
            return "vec(64x2).unary_op"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename Op>
struct VectorFloatBinaryOp {
    auto operator()(u128 lhs, u128 rhs) const
    {
        using VectorType = NativeFloatingVectorType<128, VectorSize, NativeFloatingType<128 / VectorSize>>;
        auto first = bit_cast<VectorType>(lhs);
        auto second = bit_cast<VectorType>(rhs);
        VectorType result;
        Op op;
        for (size_t i = 0; i < VectorSize; ++i) {
            result[i] = op(first[i], second[i]);
        }
        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 4:
            return "vecf(32x4).binary_op"sv;
        case 2:
            return "vecf(64x2).binary_op"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t VectorSize, typename Op>
struct VectorFloatUnaryOp {
    auto operator()(u128 lhs) const
    {
        using VectorType = NativeFloatingVectorType<128, VectorSize, NativeFloatingType<128 / VectorSize>>;
        auto value = bit_cast<VectorType>(lhs);
        VectorType result;
        Op op;
        for (size_t i = 0; i < VectorSize; ++i) {
            result[i] = op(value[i]);
        }
        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (VectorSize) {
        case 4:
            return "vecf(32x4).unary_op"sv;
        case 2:
            return "vecf(64x2).unary_op"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<size_t ResultSize, size_t InputSize, typename ResultType, typename InputType, typename Op>
struct VectorConvertOp {
    auto operator()(u128 lhs) const
    {
        using VectorInput = NativeVectorType<128 / InputSize, InputSize, MakeUnsigned>;
        using VectorResult = NativeVectorType<128 / ResultSize, ResultSize, MakeUnsigned>;
        auto value = bit_cast<VectorInput>(lhs);
        VectorResult result;
        Op op;
        auto size = min(InputSize, ResultSize);
        for (size_t i = 0; i < size; ++i)
            result[i] = bit_cast<ResultType>(op(bit_cast<InputType>(value[i])));
        // FIXME: We shouldn't need this, but the auto-vectorizer sometimes doesn't see that we
        // need to pad with zeroes when InputSize < ResultSize (i.e. converting from f64x2 -> f32x4).
        // So we put this here to make sure. Putting [[clang::optnone]] over this function resolves
        // this issue, but that would be pretty unacceptable...
        if constexpr (InputSize < ResultSize) {
            constexpr size_t remaining = ResultSize - InputSize;
            for (size_t i = 0; i < remaining; ++i)
                result[i + InputSize] = 0;
        }
        return bit_cast<u128>(result);
    }

    static StringView name()
    {
        switch (ResultSize) {
        case 4:
            return "vec(32x4).cvt_op"sv;
        case 2:
            return "vec(64x2).cvt_op"sv;
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

struct Floor {
    template<typename Lhs>
    auto operator()(Lhs lhs) const
    {
        if constexpr (IsSame<Lhs, float>)
            return floorf(lhs);
        else if constexpr (IsSame<Lhs, double>)
            return floor(lhs);
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "floor"sv; }
};

struct Truncate {
    template<typename Lhs>
    auto operator()(Lhs lhs) const
    {
        if constexpr (IsSame<Lhs, float>)
            return truncf(lhs);
        else if constexpr (IsSame<Lhs, double>)
            return trunc(lhs);
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "truncate"sv; }
};

struct NearbyIntegral {
    template<typename Lhs>
    auto operator()(Lhs lhs) const
    {
        if constexpr (IsSame<Lhs, float>)
            return nearbyintf(lhs);
        else if constexpr (IsSame<Lhs, double>)
            return nearbyint(lhs);
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "round"sv; }
};

struct SquareRoot {
    template<typename Lhs>
    auto operator()(Lhs lhs) const
    {
        if constexpr (IsSame<Lhs, float>)
            return sqrtf(lhs);
        else if constexpr (IsSame<Lhs, double>)
            return sqrt(lhs);
        else
            VERIFY_NOT_REACHED();
    }

    static StringView name() { return "sqrt"sv; }
};

template<typename Result>
struct Wrap {
    template<typename Lhs>
    Result operator()(Lhs lhs) const
    {
        return static_cast<MakeUnsigned<Result>>(bit_cast<MakeUnsigned<Lhs>>(lhs));
    }

    static StringView name() { return "wrap"sv; }
};

template<typename ResultT>
struct CheckedTruncate {
    template<typename Lhs>
    AK::ErrorOr<ResultT, StringView> operator()(Lhs lhs) const
    {
        if (isnan(lhs) || isinf(lhs)) // "undefined", let's just trap.
            return "Truncation undefined behavior"sv;

        Lhs truncated;
        if constexpr (IsSame<float, Lhs>)
            truncated = truncf(lhs);
        else if constexpr (IsSame<double, Lhs>)
            truncated = trunc(lhs);
        else
            VERIFY_NOT_REACHED();

        // FIXME: This function assumes that all values of ResultT are representable in Lhs
        //        the assumption comes from the fact that this was used exclusively by LibJS,
        //        which only considers values that are all representable in 'double'.
        if (!AK::is_within_range<ResultT>(truncated))
            return "Truncation out of range"sv;

        return static_cast<ResultT>(truncated);
    }

    static StringView name() { return "truncate.checked"sv; }
};

template<typename ResultT>
struct Extend {
    template<typename Lhs>
    ResultT operator()(Lhs lhs) const
    {
        return lhs;
    }

    static StringView name() { return "extend"sv; }
};

template<typename ResultT>
struct Convert {
    template<typename Lhs>
    ResultT operator()(Lhs lhs) const
    {
        auto interpretation = bit_cast<Lhs>(lhs);
        return static_cast<ResultT>(interpretation);
    }

    static StringView name() { return "convert"sv; }
};

template<typename ResultT>
struct Reinterpret {
    template<typename Lhs>
    ResultT operator()(Lhs lhs) const
    {
        return bit_cast<ResultT>(lhs);
    }

    static StringView name() { return "reinterpret"sv; }
};

struct Promote {
    double operator()(float lhs) const
    {
        if (isnan(lhs))
            return nan(""); // FIXME: Ensure canonical NaN remains canonical
        return static_cast<double>(lhs);
    }

    static StringView name() { return "promote"sv; }
};

struct Demote {
    float operator()(double lhs) const
    {
        if (isnan(lhs))
            return nanf(""); // FIXME: Ensure canonical NaN remains canonical

        if (isinf(lhs))
            return copysignf(__builtin_huge_valf(), lhs);

        return static_cast<float>(lhs);
    }

    static StringView name() { return "demote"sv; }
};

template<typename InitialType>
struct SignExtend {
    template<typename Lhs>
    Lhs operator()(Lhs lhs) const
    {
        auto unsigned_representation = bit_cast<MakeUnsigned<Lhs>>(lhs);
        auto truncated_unsigned_representation = static_cast<MakeUnsigned<InitialType>>(unsigned_representation);
        auto initial_value = bit_cast<InitialType>(truncated_unsigned_representation);
        return static_cast<Lhs>(initial_value);
    }

    static StringView name() { return "extend"sv; }
};

template<typename ResultT>
struct SaturatingTruncate {
    template<typename Lhs>
    ResultT operator()(Lhs lhs) const
    {
        if (isnan(lhs))
            return 0;

        if (isinf(lhs)) {
            if (lhs < 0)
                return NumericLimits<ResultT>::min();
            return NumericLimits<ResultT>::max();
        }

        // FIXME: This assumes that all values in ResultT are representable in 'double'.
        //        that assumption is not correct, which makes this function yield incorrect values
        //        for 'edge' values of type i64.
        constexpr auto convert = []<typename ConvertT>(ConvertT truncated_value) {
            if (truncated_value < NumericLimits<ResultT>::min())
                return NumericLimits<ResultT>::min();
            if constexpr (IsSame<ConvertT, float>) {
                if (truncated_value >= static_cast<ConvertT>(NumericLimits<ResultT>::max()))
                    return NumericLimits<ResultT>::max();
            } else {
                if (static_cast<double>(truncated_value) >= static_cast<double>(NumericLimits<ResultT>::max()))
                    return NumericLimits<ResultT>::max();
            }
            return static_cast<ResultT>(truncated_value);
        };

        if constexpr (IsSame<Lhs, float>)
            return convert(truncf(lhs));
        else
            return convert(trunc(lhs));
    }

    static StringView name() { return "truncate.saturating"sv; }
};

template<typename ResultT, typename Op>
struct SaturatingOp {
    template<typename Lhs, typename Rhs>
    ResultT operator()(Lhs lhs, Rhs rhs) const
    {
        Op op;

        double result = op(lhs, rhs);

        if (result <= static_cast<double>(NumericLimits<ResultT>::min())) {
            return NumericLimits<ResultT>::min();
        }

        if (result >= static_cast<double>(NumericLimits<ResultT>::max())) {
            return NumericLimits<ResultT>::max();
        }

        return static_cast<ResultT>(result);
    }

    static StringView name() { return "saturating_op"sv; }
};

}
