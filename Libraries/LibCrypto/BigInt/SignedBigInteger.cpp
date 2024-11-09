/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SignedBigInteger.h"
#include <AK/StringBuilder.h>
#include <math.h>

namespace Crypto {

SignedBigInteger::SignedBigInteger(double value)
    : m_sign(value < 0.0)
    , m_unsigned_data(fabs(value))
{
}

SignedBigInteger SignedBigInteger::import_data(u8 const* ptr, size_t length)
{
    bool sign = *ptr;
    auto unsigned_data = UnsignedBigInteger::import_data(ptr + 1, length - 1);
    return { move(unsigned_data), sign };
}

size_t SignedBigInteger::export_data(Bytes data, bool remove_leading_zeros) const
{
    // FIXME: Support this:
    //        m <0XX> -> m <XX> (if remove_leading_zeros)
    VERIFY(!remove_leading_zeros);

    data[0] = m_sign;
    auto bytes_view = data.slice(1, data.size() - 1);
    return m_unsigned_data.export_data(bytes_view, remove_leading_zeros) + 1;
}

ErrorOr<SignedBigInteger> SignedBigInteger::from_base(u16 N, StringView str)
{
    auto sign = false;
    if (str.length() > 1) {
        auto maybe_sign = str[0];
        if (maybe_sign == '-') {
            str = str.substring_view(1);
            sign = true;
        }
        if (maybe_sign == '+')
            str = str.substring_view(1);
    }
    auto unsigned_data = TRY(UnsignedBigInteger::from_base(N, str));
    return SignedBigInteger { move(unsigned_data), sign };
}

ErrorOr<String> SignedBigInteger::to_base(u16 N) const
{
    StringBuilder builder;

    if (m_sign)
        TRY(builder.try_append('-'));

    auto unsigned_as_base = TRY(m_unsigned_data.to_base(N));
    TRY(builder.try_append(unsigned_as_base.bytes_as_string_view()));

    return builder.to_string();
}

ByteString SignedBigInteger::to_base_deprecated(u16 N) const
{
    return MUST(to_base(N)).to_byte_string();
}

u64 SignedBigInteger::to_u64() const
{
    u64 unsigned_value = m_unsigned_data.to_u64();
    if (!m_sign)
        return unsigned_value;
    return ~(unsigned_value - 1); // equivalent to `-unsigned_value`, but doesn't trigger UBSAN
}

double SignedBigInteger::to_double(UnsignedBigInteger::RoundingMode rounding_mode) const
{
    double unsigned_value = m_unsigned_data.to_double(rounding_mode);
    if (!m_sign)
        return unsigned_value;

    VERIFY(!is_zero());
    return -unsigned_value;
}

FLATTEN SignedBigInteger SignedBigInteger::plus(SignedBigInteger const& other) const
{
    // If both are of the same sign, just add the unsigned data and return.
    if (m_sign == other.m_sign)
        return { other.m_unsigned_data.plus(m_unsigned_data), m_sign };

    // One value is signed while the other is not.
    return m_sign ? other.minus(this->m_unsigned_data) : minus(other.m_unsigned_data);
}

FLATTEN SignedBigInteger SignedBigInteger::minus(SignedBigInteger const& other) const
{
    // If the signs are different, convert the op to an addition.
    if (m_sign != other.m_sign) {
        // -x - y = - (x + y)
        // x - -y = (x + y)
        SignedBigInteger result { other.m_unsigned_data.plus(this->m_unsigned_data) };
        if (m_sign)
            result.negate();
        return result;
    }

    if (!m_sign) {
        // Both operands are positive.
        // x - y = - (y - x)
        if (m_unsigned_data < other.m_unsigned_data) {
            // The result will be negative.
            return { other.m_unsigned_data.minus(m_unsigned_data), true };
        }

        // The result will be either zero, or positive.
        return SignedBigInteger { m_unsigned_data.minus(other.m_unsigned_data) };
    }

    // Both operands are negative.
    // -x - -y = y - x
    if (m_unsigned_data < other.m_unsigned_data) {
        // The result will be positive.
        return SignedBigInteger { other.m_unsigned_data.minus(m_unsigned_data) };
    }
    // y - x = - (x - y)
    if (m_unsigned_data > other.m_unsigned_data) {
        // The result will be negative.
        return SignedBigInteger { m_unsigned_data.minus(other.m_unsigned_data), true };
    }
    // Both operands have the same magnitude, the result is positive zero.
    return SignedBigInteger { 0 };
}

FLATTEN SignedBigInteger SignedBigInteger::plus(UnsignedBigInteger const& other) const
{
    if (m_sign) {
        if (other < m_unsigned_data)
            return { m_unsigned_data.minus(other), true };

        return { other.minus(m_unsigned_data), false };
    }

    return { m_unsigned_data.plus(other), false };
}

FLATTEN SignedBigInteger SignedBigInteger::minus(UnsignedBigInteger const& other) const
{
    if (m_sign)
        return { m_unsigned_data.plus(m_unsigned_data), true };

    if (other < m_unsigned_data)
        return { m_unsigned_data.minus(other), false };

    return { other.minus(m_unsigned_data), true };
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_not() const
{
    // Bitwise operators assume two's complement, while SignedBigInteger uses sign-magnitude.
    // In two's complement, -x := ~x + 1.
    // Hence, ~x == -x -1 == -(x + 1).
    SignedBigInteger result = plus(SignedBigInteger { 1 });
    result.negate();
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::multiplied_by(UnsignedBigInteger const& other) const
{
    return { unsigned_value().multiplied_by(other), m_sign };
}

FLATTEN SignedDivisionResult SignedBigInteger::divided_by(UnsignedBigInteger const& divisor) const
{
    auto division_result = unsigned_value().divided_by(divisor);
    return {
        { move(division_result.quotient), m_sign },
        { move(division_result.remainder), m_sign },
    };
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_or(SignedBigInteger const& other) const
{
    // See bitwise_and() for derivations.
    if (!is_negative() && !other.is_negative())
        return { unsigned_value().bitwise_or(other.unsigned_value()), false };

    // -A | B == (~A + 1) | B == ~(A - 1) | B. The result is negative, so need to two's complement at the end to move the sign into the m_sign field.
    // That can be simplified to:
    //   -(-A | B) == ~(~(A - 1) | B) + 1 = (A - 1) & ~B + 1
    // This saves one ~.
    if (is_negative() && !other.is_negative()) {
        size_t index = unsigned_value().one_based_index_of_highest_set_bit();
        return { unsigned_value().minus(1).bitwise_and(other.unsigned_value().bitwise_not_fill_to_one_based_index(index)).plus(1), true };
    }

    // -(A | -B) == ~A & (B - 1) + 1
    if (!is_negative() && other.is_negative()) {
        size_t index = other.unsigned_value().one_based_index_of_highest_set_bit();
        return { unsigned_value().bitwise_not_fill_to_one_based_index(index).bitwise_and(other.unsigned_value().minus(1)).plus(1), true };
    }

    return { unsigned_value().minus(1).bitwise_and(other.unsigned_value().minus(1)).plus(1), true };
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_and(SignedBigInteger const& other) const
{
    if (!is_negative() && !other.is_negative())
        return { unsigned_value().bitwise_and(other.unsigned_value()), false };

    // These two just use that -x == ~x + 1 (see below).

    // -A & B == (~A + 1) & B.
    if (is_negative() && !other.is_negative()) {
        size_t index = other.unsigned_value().one_based_index_of_highest_set_bit();
        return { unsigned_value().bitwise_not_fill_to_one_based_index(index).plus(1).bitwise_and(other.unsigned_value()), false };
    }

    // A & -B == A & (~B + 1).
    if (!is_negative() && other.is_negative()) {
        size_t index = unsigned_value().one_based_index_of_highest_set_bit();
        return { unsigned_value().bitwise_and(other.unsigned_value().bitwise_not_fill_to_one_based_index(index).plus(1)), false };
    }

    // Both numbers are negative.
    // x + ~x == 0xff...ff, up to however many bits x is wide.
    // In two's complement, x + ~x + 1 == 0 since the 1 in the overflowing bit position is masked out.
    // Rearranging terms, ~x = -x - 1 (eq1).
    // Substituting x = y - 1, ~(y - 1) == -(y - 1) - 1 == -y +1 -1 == -y, or ~(y - 1) == -y (eq2).
    // Since both numbers are negative, we want to compute -A & -B.
    // Per (eq2):
    //   -A & -B == ~(A - 1) & ~(B - 1)
    // Inverting both sides:
    //   ~(-A & -B) == ~(~(A - 1) & ~(B - 1)) == ~~(A - 1) | ~~(B - 1) == (A - 1) | (B - 1).
    // Applying (q1) on the LHS:
    //   -(-A & -B) - 1 == (A - 1) | (B - 1)
    // Adding 1 on both sides and then multiplying both sides by -1:
    //   -A & -B == -( (A - 1) | (B - 1) + 1)
    // So we can compute the bitwise and by returning a negative number with magnitude (A - 1) | (B - 1) + 1.
    // This is better than the naive (~A + 1) & (~B + 1) because it needs just one O(n) scan for the or instead of 2 for the ~s.
    return { unsigned_value().minus(1).bitwise_or(other.unsigned_value().minus(1)).plus(1), true };
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_xor(SignedBigInteger const& other) const
{
    return bitwise_or(other).minus(bitwise_and(other));
}

bool SignedBigInteger::operator==(UnsignedBigInteger const& other) const
{
    if (m_sign && m_unsigned_data != 0)
        return false;
    return m_unsigned_data == other;
}

bool SignedBigInteger::operator!=(UnsignedBigInteger const& other) const
{
    if (m_sign)
        return true;
    return m_unsigned_data != other;
}

bool SignedBigInteger::operator<(UnsignedBigInteger const& other) const
{
    if (m_sign)
        return true;
    return m_unsigned_data < other;
}

bool SignedBigInteger::operator>(UnsignedBigInteger const& other) const
{
    return *this != other && !(*this < other);
}

FLATTEN SignedBigInteger SignedBigInteger::shift_left(size_t num_bits) const
{
    return SignedBigInteger { m_unsigned_data.shift_left(num_bits), m_sign };
}

FLATTEN SignedBigInteger SignedBigInteger::shift_right(size_t num_bits) const
{
    return SignedBigInteger { m_unsigned_data.shift_right(num_bits), m_sign };
}

FLATTEN SignedBigInteger SignedBigInteger::multiplied_by(SignedBigInteger const& other) const
{
    bool result_sign = m_sign ^ other.m_sign;
    return { m_unsigned_data.multiplied_by(other.m_unsigned_data), result_sign };
}

FLATTEN SignedDivisionResult SignedBigInteger::divided_by(SignedBigInteger const& divisor) const
{
    // Aa / Bb -> (A^B)q, Ar
    bool result_sign = m_sign ^ divisor.m_sign;
    auto unsigned_division_result = m_unsigned_data.divided_by(divisor.m_unsigned_data);
    return {
        { move(unsigned_division_result.quotient), result_sign },
        { move(unsigned_division_result.remainder), m_sign }
    };
}

FLATTEN SignedBigInteger SignedBigInteger::negated_value() const
{
    auto result { *this };
    result.negate();
    return result;
}

u32 SignedBigInteger::hash() const
{
    return m_unsigned_data.hash() * (1 - (2 * m_sign));
}

void SignedBigInteger::set_bit_inplace(size_t bit_index)
{
    m_unsigned_data.set_bit_inplace(bit_index);
}

bool SignedBigInteger::operator==(SignedBigInteger const& other) const
{
    if (is_invalid() != other.is_invalid())
        return false;

    if (m_unsigned_data == 0 && other.m_unsigned_data == 0)
        return true;

    return m_sign == other.m_sign && m_unsigned_data == other.m_unsigned_data;
}

bool SignedBigInteger::operator!=(SignedBigInteger const& other) const
{
    return !(*this == other);
}

bool SignedBigInteger::operator<(SignedBigInteger const& other) const
{
    if (m_sign ^ other.m_sign)
        return m_sign;

    if (m_sign)
        return other.m_unsigned_data < m_unsigned_data;

    return m_unsigned_data < other.m_unsigned_data;
}

bool SignedBigInteger::operator<=(SignedBigInteger const& other) const
{
    return *this < other || *this == other;
}

bool SignedBigInteger::operator>(SignedBigInteger const& other) const
{
    return *this != other && !(*this < other);
}

bool SignedBigInteger::operator>=(SignedBigInteger const& other) const
{
    return !(*this < other);
}

UnsignedBigInteger::CompareResult SignedBigInteger::compare_to_double(double value) const
{
    bool bigint_is_negative = m_sign;

    bool value_is_negative = value < 0;

    if (value_is_negative != bigint_is_negative)
        return bigint_is_negative ? UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt : UnsignedBigInteger::CompareResult::DoubleLessThanBigInt;

    // Now both bigint and value have the same sign, so let's compare our magnitudes.
    auto magnitudes_compare_result = m_unsigned_data.compare_to_double(fabs(value));

    // If our mangnitudes are euqal, then we're equal.
    if (magnitudes_compare_result == UnsignedBigInteger::CompareResult::DoubleEqualsBigInt)
        return UnsignedBigInteger::CompareResult::DoubleEqualsBigInt;

    // If we're negative, revert the comparison result, otherwise return the same result.
    if (value_is_negative) {
        if (magnitudes_compare_result == UnsignedBigInteger::CompareResult::DoubleLessThanBigInt)
            return UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt;
        else
            return UnsignedBigInteger::CompareResult::DoubleLessThanBigInt;
    } else {
        return magnitudes_compare_result;
    }
}

}

ErrorOr<void> AK::Formatter<Crypto::SignedBigInteger>::format(FormatBuilder& fmtbuilder, Crypto::SignedBigInteger const& value)
{
    if (value.is_negative())
        TRY(fmtbuilder.put_string("-"sv));
    return Formatter<Crypto::UnsignedBigInteger>::format(fmtbuilder, value.unsigned_value());
}
