/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <math.h>
#include <tommath.h>

#include <AK/StringBuilder.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/Tommath.h>

namespace Crypto {

SignedBigInteger::SignedBigInteger(UnsignedBigInteger&& unsigned_data, bool sign)
{
    MP_MUST(mp_init_copy(&m_mp, &unsigned_data.m_mp));
    if (sign) {
        MP_MUST(mp_neg(&m_mp, &m_mp));
    }
}

SignedBigInteger::SignedBigInteger(u8 const* ptr, size_t length)
{
    MP_MUST(mp_init(&m_mp));
    MP_MUST(mp_from_sbin(&m_mp, ptr, length));
}

SignedBigInteger::SignedBigInteger(UnsignedBigInteger const& unsigned_data)
{
    MP_MUST(mp_init_copy(&m_mp, &unsigned_data.m_mp));
}

SignedBigInteger::SignedBigInteger(double value)
{
    MP_MUST(mp_init(&m_mp));
    MP_MUST(mp_set_double(&m_mp, value));
}

SignedBigInteger::SignedBigInteger(i64 value)
{
    MP_MUST(mp_init(&m_mp));
    mp_set_i64(&m_mp, value);
}

SignedBigInteger::SignedBigInteger(SignedBigInteger const& other)
{
    MP_MUST(mp_init_copy(&m_mp, &other.m_mp));
}

SignedBigInteger& SignedBigInteger::operator=(SignedBigInteger const& other)
{
    if (this == &other)
        return *this;
    mp_clear(&m_mp);
    MP_MUST(mp_init_copy(&m_mp, &other.m_mp));
    return *this;
}

SignedBigInteger::SignedBigInteger()
{
    MP_MUST(mp_init(&m_mp));
}

SignedBigInteger::~SignedBigInteger()
{
    mp_clear(&m_mp);
}

size_t SignedBigInteger::export_data(Bytes data) const
{
    size_t written = 0;
    MP_MUST(mp_to_sbin(&m_mp, data.data(), data.size(), &written));
    return written;
}

ErrorOr<SignedBigInteger> SignedBigInteger::from_base(u16 N, StringView str)
{
    VERIFY(N <= 36);
    if (str.is_empty())
        return SignedBigInteger(0);

    auto buffer = TRY(ByteBuffer::create_zeroed(str.length() + 1));

    size_t idx = 0;
    for (auto& c : str) {
        if (c == '_') {
            // Skip underscores
            continue;
        }

        buffer[idx++] = c;
    }

    SignedBigInteger result;
    if (mp_read_radix(&result.m_mp, reinterpret_cast<char const*>(buffer.data()), N) != MP_OKAY)
        return Error::from_string_literal("Invalid number");
    return result;
}

ErrorOr<String> SignedBigInteger::to_base(u16 N) const
{
    VERIFY(N <= 36);
    if (is_zero())
        return "0"_string;

    int size = 0;
    MP_MUST(mp_radix_size(&m_mp, N, &size));
    auto buffer = TRY(ByteBuffer::create_zeroed(size));

    size_t written = 0;
    MP_MUST(mp_to_radix(&m_mp, reinterpret_cast<char*>(buffer.data()), size, &written, N));

    return StringView(buffer.bytes().slice(0, written - 1)).to_ascii_lowercase_string();
}

u64 SignedBigInteger::to_u64() const
{
    return mp_get_u64(&m_mp);
}

double SignedBigInteger::to_double(UnsignedBigInteger::RoundingMode rounding_mode) const
{
    int sign = mp_isneg(&m_mp) ? -1 : 1;
    return unsigned_value().to_double(rounding_mode) * sign;
}

UnsignedBigInteger SignedBigInteger::unsigned_value() const
{
    UnsignedBigInteger result;
    MP_MUST(mp_abs(&m_mp, &result.m_mp));
    return result;
}

bool SignedBigInteger::is_negative() const
{
    return mp_isneg(&m_mp);
}

bool SignedBigInteger::is_zero() const
{
    return mp_iszero(&m_mp);
}

void SignedBigInteger::negate()
{
    MP_MUST(mp_neg(&m_mp, &m_mp));
    m_hash = {};
}

void SignedBigInteger::set_to_0()
{
    mp_zero(&m_mp);
    m_hash = {};
}

void SignedBigInteger::set_to(i64 other)
{
    mp_set_i64(&m_mp, other);
    m_hash = {};
}

void SignedBigInteger::set_to(SignedBigInteger const& other)
{
    MP_MUST(mp_copy(&other.m_mp, &m_mp));
    m_hash = {};
}

size_t SignedBigInteger::byte_length() const
{
    return mp_sbin_size(&m_mp);
}

FLATTEN SignedBigInteger SignedBigInteger::plus(SignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_add(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::minus(SignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_sub(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::plus(UnsignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_add(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::minus(UnsignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_sub(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_not() const
{
    SignedBigInteger result;
    MP_MUST(mp_complement(&m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::multiplied_by(UnsignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_mul(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedDivisionResult SignedBigInteger::divided_by(UnsignedBigInteger const& divisor) const
{
    SignedBigInteger quotient;
    SignedBigInteger remainder;
    MP_MUST(mp_div(&m_mp, &divisor.m_mp, &quotient.m_mp, &remainder.m_mp));
    return SignedDivisionResult { quotient, remainder };
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_or(SignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_or(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_and(SignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_and(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::bitwise_xor(SignedBigInteger const& other) const
{
    return bitwise_or(other).minus(bitwise_and(other));
}

bool SignedBigInteger::operator==(UnsignedBigInteger const& other) const
{
    return mp_cmp(&m_mp, &other.m_mp) == MP_EQ;
}

bool SignedBigInteger::operator!=(UnsignedBigInteger const& other) const
{
    return !(*this == other);
}

bool SignedBigInteger::operator<(UnsignedBigInteger const& other) const
{
    return mp_cmp(&m_mp, &other.m_mp) == MP_LT;
}

bool SignedBigInteger::operator>(UnsignedBigInteger const& other) const
{
    return *this != other && !(*this < other);
}

FLATTEN ErrorOr<SignedBigInteger> SignedBigInteger::shift_left(size_t num_bits) const
{
    SignedBigInteger result;
    MP_TRY(mp_mul_2d(&m_mp, num_bits, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::shift_right(size_t num_bits) const
{
    SignedBigInteger result;
    MP_MUST(mp_div_2d(&m_mp, num_bits, &result.m_mp, nullptr));
    return result;
}

FLATTEN ErrorOr<SignedBigInteger> SignedBigInteger::mod_power_of_two(size_t power_of_two) const
{
    if (power_of_two == 0)
        return SignedBigInteger(0);

    // If the number is positive and smaller than the modulus, we can just return it.
    if (!is_negative() && static_cast<size_t>(m_mp.used * MP_DIGIT_BIT) <= power_of_two)
        return *this;

    // If the power of two overflows the int type, we don't have enough memory to compute it.
    if (power_of_two > NumericLimits<int>::max())
        return Error::from_errno(ENOMEM);

    SignedBigInteger result;
    MP_MUST(mp_mod_2d(&m_mp, power_of_two, &result.m_mp));
    if (!result.is_negative())
        return result;

    // If the result is negative, we need to add the modulus to it.
    UnsignedBigInteger modulus;
    MP_TRY(mp_2expt(&modulus.m_mp, power_of_two));
    MP_MUST(mp_add(&result.m_mp, &modulus.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::multiplied_by(SignedBigInteger const& other) const
{
    SignedBigInteger result;
    MP_MUST(mp_mul(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN SignedDivisionResult SignedBigInteger::divided_by(SignedBigInteger const& divisor) const
{
    SignedBigInteger quotient;
    SignedBigInteger remainder;
    MP_MUST(mp_div(&m_mp, &divisor.m_mp, &quotient.m_mp, &remainder.m_mp));
    return SignedDivisionResult { quotient, remainder };
}

FLATTEN SignedBigInteger SignedBigInteger::pow(u32 exponent) const
{
    SignedBigInteger result;
    MP_MUST(mp_expt_n(&m_mp, exponent, &result.m_mp));
    return result;
}

FLATTEN SignedBigInteger SignedBigInteger::negated_value() const
{
    auto result { *this };
    result.negate();
    return result;
}

u32 SignedBigInteger::hash() const
{
    if (m_hash.has_value())
        return *m_hash;

    auto buffer = MUST(ByteBuffer::create_zeroed(byte_length()));
    auto length = export_data(buffer);
    m_hash = string_hash(reinterpret_cast<char const*>(buffer.data()), length);
    return *m_hash;
}

bool SignedBigInteger::operator==(SignedBigInteger const& other) const
{
    return mp_cmp(&m_mp, &other.m_mp) == MP_EQ;
}

bool SignedBigInteger::operator!=(SignedBigInteger const& other) const
{
    return !(*this == other);
}

bool SignedBigInteger::operator<(SignedBigInteger const& other) const
{
    return mp_cmp(&m_mp, &other.m_mp) == MP_LT;
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
    bool bigint_is_negative = is_negative();

    bool value_is_negative = value < 0;

    if (value_is_negative != bigint_is_negative)
        return bigint_is_negative ? UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt : UnsignedBigInteger::CompareResult::DoubleLessThanBigInt;

    // Now both bigint and value have the same sign, so let's compare our magnitudes.
    auto magnitudes_compare_result = unsigned_value().compare_to_double(fabs(value));

    // If our magnitudes are equal, then we're equal.
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
