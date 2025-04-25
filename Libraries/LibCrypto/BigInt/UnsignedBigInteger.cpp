/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <math.h>
#include <tommath.h>

#include <AK/BuiltinWrappers.h>
#include <AK/FloatingPoint.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringBuilder.h>
#include <LibCrypto/BigInt/Tommath.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto {

UnsignedBigInteger::UnsignedBigInteger(u8 const* ptr, size_t length)
{
    MP_MUST(mp_init(&m_mp));
    MP_MUST(mp_from_ubin(&m_mp, ptr, length));
}

UnsignedBigInteger::UnsignedBigInteger(Vector<u32> const& words)
{
    MP_MUST(mp_init(&m_mp));
    MP_MUST(mp_unpack(&m_mp, words.size(), MP_LSB_FIRST, sizeof(u32), MP_NATIVE_ENDIAN, 0, words.data()));
}

UnsignedBigInteger::UnsignedBigInteger(double value)
{
    // Because this is currently only used for LibJS we VERIFY some preconditions
    // also these values don't have a clear BigInteger representation.
    VERIFY(!isnan(value));
    VERIFY(!isinf(value));
    VERIFY(trunc(value) == value);
    VERIFY(value >= 0.0);

    MP_MUST(mp_init(&m_mp));
    MP_MUST(mp_set_double(&m_mp, value));
}

UnsignedBigInteger::UnsignedBigInteger(u64 value)
{
    MP_MUST(mp_init(&m_mp));
    mp_set_u64(&m_mp, value);
}

UnsignedBigInteger::UnsignedBigInteger(UnsignedBigInteger const& other)
{
    MP_MUST(mp_init_copy(&m_mp, &other.m_mp));
}

UnsignedBigInteger& UnsignedBigInteger::operator=(UnsignedBigInteger const& other)
{
    if (this == &other)
        return *this;
    mp_clear(&m_mp);
    MP_MUST(mp_init_copy(&m_mp, &other.m_mp));
    return *this;
}

UnsignedBigInteger::UnsignedBigInteger()
{
    MP_MUST(mp_init(&m_mp));
}

UnsignedBigInteger::~UnsignedBigInteger()
{
    mp_clear(&m_mp);
}

size_t UnsignedBigInteger::export_data(Bytes data) const
{
    size_t written = 0;
    MP_MUST(mp_to_ubin(&m_mp, data.data(), data.size(), &written));
    return written;
}

ErrorOr<UnsignedBigInteger> UnsignedBigInteger::from_base(u16 N, StringView str)
{
    VERIFY(N <= 36);
    if (str.is_empty())
        return UnsignedBigInteger(0);

    auto buffer = TRY(ByteBuffer::create_zeroed(str.length() + 1));

    size_t idx = 0;
    for (auto& c : str) {
        if (c == '_') {
            // Skip underscores
            continue;
        }

        buffer[idx++] = c;
    }

    UnsignedBigInteger result;
    if (mp_read_radix(&result.m_mp, reinterpret_cast<char const*>(buffer.data()), N) != MP_OKAY)
        return Error::from_string_literal("Invalid number");
    return result;
}

ErrorOr<String> UnsignedBigInteger::to_base(u16 N) const
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

u64 UnsignedBigInteger::to_u64() const
{
    return mp_get_u64(&m_mp);
}

double UnsignedBigInteger::to_double(RoundingMode rounding_mode) const
{
    // Check if we need to truncate
    auto bitlen = mp_count_bits(&m_mp);
    if (bitlen <= 53)
        return mp_get_double(&m_mp);

    if (rounding_mode == RoundingMode::RoundTowardZero) {
        UnsignedBigInteger shifted;

        // Truncate the lower bits
        auto shift = bitlen - 53;
        MP_MUST(mp_div_2d(&m_mp, shift, &shifted.m_mp, nullptr));

        // Convert to double
        return ldexp(mp_get_double(&shifted.m_mp), shift);
    }

    if (rounding_mode == RoundingMode::IEEERoundAndTiesToEvenMantissa) {
        UnsignedBigInteger shifted, remainder, half, lsb;

        // Get top 53 bits (truncated)
        auto shift = bitlen - 53;
        MP_MUST(mp_div_2d(&m_mp, shift, &shifted.m_mp, &remainder.m_mp));

        // Check if remainder == 2^(shift - 1)
        MP_MUST(mp_2expt(&half.m_mp, shift - 1));
        int cmp = mp_cmp(&remainder.m_mp, &half.m_mp);
        if (cmp < 0) {
            // Round down (truncate)
        } else if (cmp > 0) {
            // Round up
            MP_MUST(mp_add_d(&shifted.m_mp, 1, &shifted.m_mp));
        } else {
            // Exactly halfway, check even
            MP_MUST(mp_mod_2d(&shifted.m_mp, 1, &lsb.m_mp));
            if (!mp_iszero(&lsb.m_mp)) {
                // It's odd, round to even
                MP_MUST(mp_add_d(&shifted.m_mp, 1, &shifted.m_mp));
            }
        }

        // Convert to double
        return ldexp(mp_get_double(&shifted.m_mp), shift);
    }

    VERIFY_NOT_REACHED();
}

Vector<u32> UnsignedBigInteger::words() const
{
    auto count = mp_pack_count(&m_mp, 0, sizeof(u32));
    Vector<u32> result;
    result.resize(count);

    size_t written = 0;
    MP_MUST(mp_pack(result.data(), count, &written, MP_LSB_FIRST, sizeof(u32), MP_NATIVE_ENDIAN, 0, &m_mp));

    result.resize(written);
    return result;
}

void UnsignedBigInteger::set_to_0()
{
    mp_zero(&m_mp);
    m_hash = {};
}

void UnsignedBigInteger::set_to(u64 other)
{
    mp_set_u64(&m_mp, other);
    m_hash = {};
}

void UnsignedBigInteger::set_to(UnsignedBigInteger const& other)
{
    MP_MUST(mp_copy(&other.m_mp, &m_mp));
    m_hash = {};
}

bool UnsignedBigInteger::is_zero() const
{
    return mp_iszero(&m_mp);
}

bool UnsignedBigInteger::is_odd() const
{
    return mp_isodd(&m_mp);
}

size_t UnsignedBigInteger::byte_length() const
{
    return mp_ubin_size(&m_mp);
}

size_t UnsignedBigInteger::one_based_index_of_highest_set_bit() const
{
    return mp_count_bits(&m_mp);
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::plus(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_add(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN ErrorOr<UnsignedBigInteger> UnsignedBigInteger::minus(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_sub(&m_mp, &other.m_mp, &result.m_mp));
    if (mp_isneg(&result.m_mp))
        return Error::from_string_literal("Substraction produced a negative result");
    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_or(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_or(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_and(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_and(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_xor(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_xor(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN ErrorOr<UnsignedBigInteger> UnsignedBigInteger::bitwise_not_fill_to_one_based_index(size_t index) const
{
    if (index == 0)
        return UnsignedBigInteger(0);
    if (index > NumericLimits<int>::max())
        return Error::from_errno(ENOMEM);

    UnsignedBigInteger result, mask, temp;

    MP_TRY(mp_2expt(&mask.m_mp, index));
    MP_TRY(mp_sub_d(&mask.m_mp, 1, &mask.m_mp));

    MP_TRY(mp_and(&mask.m_mp, &m_mp, &temp.m_mp));
    MP_TRY(mp_xor(&temp.m_mp, &mask.m_mp, &result.m_mp));

    return result;
}

FLATTEN ErrorOr<UnsignedBigInteger> UnsignedBigInteger::shift_left(size_t num_bits) const
{
    UnsignedBigInteger result;
    MP_TRY(mp_mul_2d(&m_mp, num_bits, &result.m_mp));
    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::shift_right(size_t num_bits) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_div_2d(&m_mp, num_bits, &result.m_mp, nullptr));
    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::multiplied_by(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_mul(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

FLATTEN UnsignedDivisionResult UnsignedBigInteger::divided_by(UnsignedBigInteger const& divisor) const
{
    UnsignedBigInteger quotient;
    UnsignedBigInteger remainder;
    MP_MUST(mp_div(&m_mp, &divisor.m_mp, &quotient.m_mp, &remainder.m_mp));
    return UnsignedDivisionResult { quotient, remainder };
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::pow(u32 exponent) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_expt_n(&m_mp, exponent, &result.m_mp));
    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::gcd(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    MP_MUST(mp_gcd(&m_mp, &other.m_mp, &result.m_mp));
    return result;
}

u32 UnsignedBigInteger::hash() const
{
    if (m_hash.has_value())
        return *m_hash;

    auto buffer = MUST(ByteBuffer::create_zeroed(byte_length()));
    auto length = export_data(buffer);
    m_hash = string_hash(reinterpret_cast<char const*>(buffer.data()), length);
    return *m_hash;
}

bool UnsignedBigInteger::operator==(UnsignedBigInteger const& other) const
{
    return mp_cmp(&m_mp, &other.m_mp) == MP_EQ;
}

bool UnsignedBigInteger::operator!=(UnsignedBigInteger const& other) const
{
    return !(*this == other);
}

bool UnsignedBigInteger::operator<(UnsignedBigInteger const& other) const
{
    return mp_cmp(&m_mp, &other.m_mp) == MP_LT;
}

bool UnsignedBigInteger::operator<=(UnsignedBigInteger const& other) const
{
    return *this < other || *this == other;
}

bool UnsignedBigInteger::operator>(UnsignedBigInteger const& other) const
{
    return *this != other && !(*this < other);
}

bool UnsignedBigInteger::operator>=(UnsignedBigInteger const& other) const
{
    return *this > other || *this == other;
}

UnsignedBigInteger::CompareResult UnsignedBigInteger::compare_to_double(double value) const
{
    VERIFY(!isnan(value));

    if (isinf(value)) {
        bool is_positive_infinity = __builtin_isinf_sign(value) > 0;
        return is_positive_infinity ? CompareResult::DoubleGreaterThanBigInt : CompareResult::DoubleLessThanBigInt;
    }

    bool value_is_negative = value < 0;

    if (value_is_negative)
        return CompareResult::DoubleLessThanBigInt;

    // Value is zero.
    if (value == 0.0) {
        VERIFY(!value_is_negative);
        // Either we are also zero or value is certainly less than us.
        return is_zero() ? CompareResult::DoubleEqualsBigInt : CompareResult::DoubleLessThanBigInt;
    }

    // If value is not zero but we are, value must be greater.
    if (is_zero())
        return CompareResult::DoubleGreaterThanBigInt;

    FloatExtractor<double> extractor;
    extractor.d = value;

    // Value cannot be negative at this point.
    VERIFY(extractor.sign == 0);
    // Exponent cannot be all set, as then we must be NaN or infinity.
    VERIFY(extractor.exponent != (1 << extractor.exponent_bits) - 1);

    i32 real_exponent = extractor.exponent - extractor.exponent_bias;
    if (real_exponent < 0) {
        // value is less than 1, and we cannot be zero so value must be less.
        return CompareResult::DoubleLessThanBigInt;
    }

    u64 bigint_bits_needed = one_based_index_of_highest_set_bit();
    VERIFY(bigint_bits_needed > 0);

    // Double value is `-1^sign (1.mantissa) * 2^(exponent - bias)` so we need
    // `exponent - bias + 1` bit to represent doubles value,
    // for example `exponent - bias` = 3, sign = 0 and mantissa = 0 we get
    // `-1^0 * 2^3 * 1 = 8` which needs 4 bits to store 8 (0b1000).
    u32 double_bits_needed = real_exponent + 1;

    // If we need more bits to represent us, we must be of greater value.
    if (bigint_bits_needed > double_bits_needed)
        return CompareResult::DoubleLessThanBigInt;
    // If we need less bits to represent us, we must be of less value.
    if (bigint_bits_needed < double_bits_needed)
        return CompareResult::DoubleGreaterThanBigInt;

    u64 mantissa_bits = extractor.mantissa;

    // We add the bit which represents the 1. of the double value calculation.
    constexpr u64 mantissa_extended_bit = 1ull << extractor.mantissa_bits;

    mantissa_bits |= mantissa_extended_bit;

    constexpr u32 bits_in_word = sizeof(u32) * 8;

    // Now we shift value to the left virtually, with `exponent - bias` steps
    // we then pretend both it and the big int are extended with virtual zeros.
    auto next_bigint_word = (bits_in_word - 1 + bigint_bits_needed) / bits_in_word;

    auto words = this->words();
    VERIFY(next_bigint_word == words.size());

    auto msb_in_top_word_index = (bigint_bits_needed - 1) % bits_in_word;
    VERIFY(msb_in_top_word_index == (bits_in_word - count_leading_zeroes(words[next_bigint_word - 1]) - 1));

    // We will keep the bits which are still valid in the mantissa at the top of mantissa bits.
    mantissa_bits <<= 64 - (extractor.mantissa_bits + 1);

    auto bits_left_in_mantissa = static_cast<size_t>(extractor.mantissa_bits) + 1;

    auto get_next_value_bits = [&](size_t num_bits) -> u32 {
        VERIFY(num_bits < 63);
        VERIFY(bits_left_in_mantissa > 0);
        if (num_bits > bits_left_in_mantissa)
            num_bits = bits_left_in_mantissa;

        bits_left_in_mantissa -= num_bits;

        u64 extracted_bits = mantissa_bits & (((1ull << num_bits) - 1) << (64 - num_bits));
        // Now shift the bits down to put the most significant bit on the num_bits position
        // this means the rest will be "virtual" zeros.
        extracted_bits >>= bits_in_word;

        // Now shift away the used bits and fit the result into a Word.
        mantissa_bits <<= num_bits;

        VERIFY(extracted_bits <= NumericLimits<u32>::max());
        return static_cast<u32>(extracted_bits);
    };

    auto bits_in_next_bigint_word = msb_in_top_word_index + 1;

    while (next_bigint_word > 0 && bits_left_in_mantissa > 0) {
        u32 bigint_word = words[next_bigint_word - 1];
        u32 double_word = get_next_value_bits(bits_in_next_bigint_word);

        // For the first bit we have to align it with the top bit of bigint
        // and for all the other cases bits_in_next_bigint_word is 32 so this does nothing.
        double_word >>= bits_in_word - bits_in_next_bigint_word;

        if (bigint_word < double_word)
            return CompareResult::DoubleGreaterThanBigInt;

        if (bigint_word > double_word)
            return CompareResult::DoubleLessThanBigInt;

        --next_bigint_word;
        bits_in_next_bigint_word = bits_in_word;
    }

    // If there are still bits left in bigint than any non zero bit means it has greater value.
    if (next_bigint_word > 0) {
        VERIFY(bits_left_in_mantissa == 0);
        while (next_bigint_word > 0) {
            if (words[next_bigint_word - 1] != 0)
                return CompareResult::DoubleLessThanBigInt;
            --next_bigint_word;
        }
    } else if (bits_left_in_mantissa > 0) {
        VERIFY(next_bigint_word == 0);
        // Similarly if there are still any bits set in the mantissa it has greater value.
        if (mantissa_bits != 0)
            return CompareResult::DoubleGreaterThanBigInt;
    }

    // Otherwise if both don't have bits left or the rest of the bits are zero they are equal.
    return CompareResult::DoubleEqualsBigInt;
}

}

ErrorOr<void> AK::Formatter<Crypto::UnsignedBigInteger>::format(FormatBuilder& fmtbuilder, Crypto::UnsignedBigInteger const& value)
{
    return Formatter<StringView>::format(fmtbuilder, TRY(value.to_base(10)));
}
