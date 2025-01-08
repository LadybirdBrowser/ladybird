/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigInteger.h"
#include <AK/BuiltinWrappers.h>
#include <AK/CharacterTypes.h>
#include <AK/FloatingPoint.h>
#include <AK/StringBuilder.h>
#include <AK/StringHash.h>
#include <LibCrypto/BigInt/Algorithms/UnsignedBigIntegerAlgorithms.h>
#include <math.h>

namespace Crypto {

UnsignedBigInteger::UnsignedBigInteger(u8 const* ptr, size_t length)
{
    m_words.resize_and_keep_capacity((length + sizeof(u32) - 1) / sizeof(u32));
    size_t in = length, out = 0;
    while (in >= sizeof(u32)) {
        in -= sizeof(u32);
        u32 word = ((u32)ptr[in] << 24) | ((u32)ptr[in + 1] << 16) | ((u32)ptr[in + 2] << 8) | (u32)ptr[in + 3];
        m_words[out++] = word;
    }
    if (in > 0) {
        u32 word = 0;
        for (size_t i = 0; i < in; i++) {
            word <<= 8;
            word |= (u32)ptr[i];
        }
        m_words[out++] = word;
    }
}

UnsignedBigInteger::UnsignedBigInteger(double value)
{
    // Because this is currently only used for LibJS we VERIFY some preconditions
    // also these values don't have a clear BigInteger representation.
    VERIFY(!isnan(value));
    VERIFY(!isinf(value));
    VERIFY(trunc(value) == value);
    VERIFY(value >= 0.0);

    if (value <= NumericLimits<u32>::max()) {
        m_words.append(static_cast<u32>(value));
        return;
    }

    FloatExtractor<double> extractor;
    extractor.d = value;
    VERIFY(!extractor.sign);

    i32 real_exponent = extractor.exponent - extractor.exponent_bias;
    VERIFY(real_exponent > 0);

    // Ensure we have enough space, we will need 2^exponent bits, so round up in words
    auto word_index = (real_exponent + BITS_IN_WORD) / BITS_IN_WORD;
    m_words.resize_and_keep_capacity(word_index);

    // Now we just need to put the mantissa with explicit 1 bit at the top at the proper location
    u64 raw_mantissa = extractor.mantissa | (1ull << extractor.mantissa_bits);
    VERIFY((raw_mantissa & 0xfff0000000000000) == 0x0010000000000000);
    // Shift it so the bits we need are at the top
    raw_mantissa <<= 64 - extractor.mantissa_bits - 1;

    // The initial bit needs to be exactly aligned with exponent, this is 1-indexed
    auto top_word_bit_offset = real_exponent % BITS_IN_WORD + 1;

    auto top_word_bits_from_mantissa = raw_mantissa >> (64 - top_word_bit_offset);
    VERIFY(top_word_bits_from_mantissa <= NumericLimits<Word>::max());
    m_words[word_index - 1] = top_word_bits_from_mantissa;

    --word_index;
    // Shift used bits away
    raw_mantissa <<= top_word_bit_offset;
    i32 bits_in_mantissa = extractor.mantissa_bits + 1 - top_word_bit_offset;
    // Now just put everything at the top of the next words

    constexpr auto to_word_shift = 64 - BITS_IN_WORD;

    while (word_index > 0 && bits_in_mantissa > 0) {
        VERIFY((raw_mantissa >> to_word_shift) <= NumericLimits<Word>::max());
        m_words[word_index - 1] = raw_mantissa >> to_word_shift;
        raw_mantissa <<= to_word_shift;

        bits_in_mantissa -= BITS_IN_WORD;
        --word_index;
    }

    VERIFY(m_words.size() > word_index);
    VERIFY((m_words.size() - word_index) <= 3);

    // No bits left, otherwise we would have to round
    VERIFY(raw_mantissa == 0);
}

UnsignedBigInteger UnsignedBigInteger::create_invalid()
{
    UnsignedBigInteger invalid(0);
    invalid.invalidate();
    return invalid;
}

size_t UnsignedBigInteger::export_data(Bytes data, bool remove_leading_zeros) const
{
    size_t word_count = trimmed_length();
    size_t out = 0;
    if (word_count > 0) {
        ssize_t leading_zeros = -1;
        if (remove_leading_zeros) {
            UnsignedBigInteger::Word word = m_words[word_count - 1];
            for (size_t i = 0; i < sizeof(u32); i++) {
                u8 byte = (u8)(word >> ((sizeof(u32) - i - 1) * 8));
                data[out++] = byte;
                if (leading_zeros < 0 && byte != 0)
                    leading_zeros = (int)i;
            }
        }
        for (size_t i = word_count - (remove_leading_zeros ? 1 : 0); i > 0; i--) {
            auto word = m_words[i - 1];
            data[out++] = (u8)(word >> 24);
            data[out++] = (u8)(word >> 16);
            data[out++] = (u8)(word >> 8);
            data[out++] = (u8)word;
        }
        if (leading_zeros > 0)
            out -= leading_zeros;
    }
    return out;
}

ErrorOr<UnsignedBigInteger> UnsignedBigInteger::from_base(u16 N, StringView str)
{
    VERIFY(N <= 36);
    UnsignedBigInteger result;
    UnsignedBigInteger base { N };

    for (auto const& c : str) {
        if (c == '_')
            continue;
        if (!is_ascii_base36_digit(c))
            return Error::from_string_literal("Invalid Base36 digit");
        auto digit = parse_ascii_base36_digit(c);
        if (digit >= N)
            return Error::from_string_literal("Base36 digit out of range");

        result = result.multiplied_by(base).plus(digit);
    }
    return result;
}

ErrorOr<String> UnsignedBigInteger::to_base(u16 N) const
{
    VERIFY(N <= 36);
    if (*this == UnsignedBigInteger { 0 })
        return "0"_string;

    StringBuilder builder;
    UnsignedBigInteger temp(*this);
    UnsignedBigInteger quotient;
    UnsignedBigInteger remainder;

    while (temp != UnsignedBigInteger { 0 }) {
        UnsignedBigIntegerAlgorithms::divide_u16_without_allocation(temp, N, quotient, remainder);
        VERIFY(remainder.words()[0] < N);
        TRY(builder.try_append(to_ascii_base36_digit(remainder.words()[0])));
        temp.set_to(quotient);
    }

    return TRY(builder.to_string()).reverse();
}

ByteString UnsignedBigInteger::to_base_deprecated(u16 N) const
{
    return MUST(to_base(N)).to_byte_string();
}

u64 UnsignedBigInteger::to_u64() const
{
    static_assert(sizeof(Word) == 4);
    if (!length())
        return 0;
    u64 value = m_words[0];
    if (length() > 1)
        value |= static_cast<u64>(m_words[1]) << 32;
    return value;
}

double UnsignedBigInteger::to_double(UnsignedBigInteger::RoundingMode rounding_mode) const
{
    VERIFY(!is_invalid());
    auto highest_bit = one_based_index_of_highest_set_bit();
    if (highest_bit == 0)
        return 0;
    --highest_bit;

    using Extractor = FloatExtractor<double>;

    // Simple case if less than 2^53 since those number are all exactly representable in doubles
    if (highest_bit < Extractor::mantissa_bits + 1)
        return static_cast<double>(to_u64());

    // If it uses too many bit to represent in a double return infinity
    if (highest_bit > Extractor::exponent_bias)
        return __builtin_huge_val();

    // Otherwise we have to take the top 53 bits, use those as the mantissa,
    // and the amount of bits as the exponent. Note that the mantissa has an implicit top bit of 1
    // so we have to ignore the very top bit.

    // Since we extract at most 53 bits it will take at most 3 words
    static_assert(BITS_IN_WORD * 3 >= (Extractor::mantissa_bits + 1));
    constexpr auto bits_in_u64 = 64;
    static_assert(bits_in_u64 > Extractor::mantissa_bits + 1);

    auto bits_to_read = min(static_cast<size_t>(Extractor::mantissa_bits), highest_bit);

    auto last_word_index = trimmed_length();
    VERIFY(last_word_index > 0);

    // Note that highest bit is 0-indexed at this point.
    auto highest_bit_index_in_top_word = highest_bit % BITS_IN_WORD;

    // Shift initial word until highest bit is just beyond top of u64.
    u64 mantissa = m_words[last_word_index - 1];
    if (highest_bit_index_in_top_word != 0)
        mantissa <<= (bits_in_u64 - highest_bit_index_in_top_word);
    else
        mantissa = 0;

    auto bits_written = highest_bit_index_in_top_word;

    --last_word_index;

    Optional<Word> dropped_bits_for_rounding;
    u8 bits_dropped_from_final_word = 0;

    if (bits_written < bits_to_read && last_word_index > 0) {
        // Second word can always just cleanly be shifted up to the final bit of the first word
        // since the first has at most BIT_IN_WORD - 1, 31
        u64 next_word = m_words[last_word_index - 1];
        VERIFY((mantissa & (next_word << (bits_in_u64 - bits_written - BITS_IN_WORD))) == 0);
        mantissa |= next_word << (bits_in_u64 - bits_written - BITS_IN_WORD);
        bits_written += BITS_IN_WORD;
        --last_word_index;

        if (bits_written > bits_to_read) {
            bits_dropped_from_final_word = bits_written - bits_to_read;
            dropped_bits_for_rounding = m_words[last_word_index] & ((1 << bits_dropped_from_final_word) - 1);
        } else if (bits_written < bits_to_read && last_word_index > 0) {
            // The final word has to be shifted down first to discard any excess bits.
            u64 final_word = m_words[last_word_index - 1];
            --last_word_index;

            auto bits_to_write = bits_to_read - bits_written;

            bits_dropped_from_final_word = BITS_IN_WORD - bits_to_write;
            dropped_bits_for_rounding = final_word & ((1 << bits_dropped_from_final_word) - 1u);
            final_word >>= bits_dropped_from_final_word;

            // Then move the bits right up to the lowest bits of the second word
            VERIFY((mantissa & (final_word << (bits_in_u64 - bits_written - bits_to_write))) == 0);
            mantissa |= final_word << (bits_in_u64 - bits_written - bits_to_write);
        }
    }

    // Now the mantissa should be complete so shift it down
    mantissa >>= bits_in_u64 - Extractor::mantissa_bits;

    if (rounding_mode == RoundingMode::IEEERoundAndTiesToEvenMantissa) {
        bool round_up = false;

        if (bits_dropped_from_final_word == 0) {
            if (last_word_index > 0) {
                Word next_word = m_words[last_word_index - 1];
                last_word_index--;
                if ((next_word & 0x80000000) != 0) {
                    // next top bit set check for any other bits
                    if ((next_word ^ 0x80000000) != 0) {
                        round_up = true;
                    } else {
                        while (last_word_index > 0) {
                            if (m_words[last_word_index - 1] != 0) {
                                round_up = true;
                                break;
                            }
                        }

                        // All other bits are 0 which is a tie thus round to even exponent
                        // Since we are halfway, if exponent ends with 1 we round up, if 0 we round down
                        round_up = (mantissa & 1) != 0;
                    }
                } else {
                    round_up = false;
                }
            } else {
                // If there are no words left the rest is implicitly 0 so just round down
                round_up = false;
            }

        } else {
            VERIFY(dropped_bits_for_rounding.has_value());
            VERIFY(bits_dropped_from_final_word >= 1);

            // In this case the top bit comes form the dropped bits
            auto top_bit_extractor = 1u << (bits_dropped_from_final_word - 1u);
            if ((*dropped_bits_for_rounding & top_bit_extractor) != 0) {
                // Possible tie again, if any other bit is set we round up
                if ((*dropped_bits_for_rounding ^ top_bit_extractor) != 0) {
                    round_up = true;
                } else {
                    while (last_word_index > 0) {
                        if (m_words[last_word_index - 1] != 0) {
                            round_up = true;
                            break;
                        }
                    }

                    round_up = (mantissa & 1) != 0;
                }
            } else {
                round_up = false;
            }
        }

        if (round_up) {
            ++mantissa;
            if ((mantissa & (1ull << Extractor::mantissa_bits)) != 0) {
                // we overflowed the mantissa
                mantissa = 0;
                highest_bit++;

                // In which case it is possible we have to round to infinity
                if (highest_bit > Extractor::exponent_bias)
                    return __builtin_huge_val();
            }
        }
    } else {
        VERIFY(rounding_mode == RoundingMode::RoundTowardZero);
    }

    Extractor extractor;
    extractor.exponent = highest_bit + extractor.exponent_bias;

    VERIFY((mantissa & 0xfff0000000000000) == 0);
    extractor.mantissa = mantissa;

    return extractor.d;
}

void UnsignedBigInteger::set_to_0()
{
    m_words.clear_with_capacity();
    m_is_invalid = false;
    m_cached_trimmed_length = {};
    m_cached_hash = 0;
}

void UnsignedBigInteger::set_to(UnsignedBigInteger::Word other)
{
    m_is_invalid = false;
    m_words.resize_and_keep_capacity(1);
    m_words[0] = other;
    m_cached_trimmed_length = {};
    m_cached_hash = 0;
}

void UnsignedBigInteger::set_to(UnsignedBigInteger const& other)
{
    m_is_invalid = other.m_is_invalid;
    m_words.resize_and_keep_capacity(other.m_words.size());
    __builtin_memcpy(m_words.data(), other.m_words.data(), other.m_words.size() * sizeof(u32));
    m_cached_trimmed_length = {};
    m_cached_hash = 0;
}

bool UnsignedBigInteger::is_zero() const
{
    for (size_t i = 0; i < length(); ++i) {
        if (m_words[i] != 0)
            return false;
    }

    return true;
}

size_t UnsignedBigInteger::trimmed_length() const
{
    if (!m_cached_trimmed_length.has_value()) {
        size_t num_leading_zeroes = 0;
        for (int i = length() - 1; i >= 0; --i, ++num_leading_zeroes) {
            if (m_words[i] != 0)
                break;
        }
        m_cached_trimmed_length = length() - num_leading_zeroes;
    }
    return m_cached_trimmed_length.value();
}

void UnsignedBigInteger::clamp_to_trimmed_length()
{
    auto length = trimmed_length();
    if (m_words.size() > length)
        m_words.resize(length);
}

void UnsignedBigInteger::resize_with_leading_zeros(size_t new_length)
{
    size_t old_length = length();
    if (old_length < new_length) {
        m_words.resize_and_keep_capacity(new_length);
        __builtin_memset(&m_words.data()[old_length], 0, (new_length - old_length) * sizeof(u32));
    }
}

size_t UnsignedBigInteger::one_based_index_of_highest_set_bit() const
{
    size_t number_of_words = trimmed_length();
    size_t index = 0;
    if (number_of_words > 0) {
        index += (number_of_words - 1) * BITS_IN_WORD;
        index += BITS_IN_WORD - count_leading_zeroes(m_words[number_of_words - 1]);
    }
    return index;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::plus(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;

    UnsignedBigIntegerAlgorithms::add_without_allocation(*this, other, result);

    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::minus(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;

    UnsignedBigIntegerAlgorithms::subtract_without_allocation(*this, other, result);

    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_or(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;

    UnsignedBigIntegerAlgorithms::bitwise_or_without_allocation(*this, other, result);

    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_and(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;

    UnsignedBigIntegerAlgorithms::bitwise_and_without_allocation(*this, other, result);

    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_xor(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;

    UnsignedBigIntegerAlgorithms::bitwise_xor_without_allocation(*this, other, result);

    return result;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::bitwise_not_fill_to_one_based_index(size_t size) const
{
    return MUST(try_bitwise_not_fill_to_one_based_index(size));
}

FLATTEN ErrorOr<UnsignedBigInteger> UnsignedBigInteger::try_bitwise_not_fill_to_one_based_index(size_t size) const
{
    UnsignedBigInteger result;

    TRY(UnsignedBigIntegerAlgorithms::bitwise_not_fill_to_one_based_index_without_allocation(*this, size, result));

    return result;
}

FLATTEN ErrorOr<UnsignedBigInteger> UnsignedBigInteger::try_shift_left(size_t num_bits) const
{
    UnsignedBigInteger output;

    TRY(UnsignedBigIntegerAlgorithms::try_shift_left_without_allocation(*this, num_bits, output));

    return output;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::shift_left(size_t num_bits) const
{
    return MUST(try_shift_left(num_bits));
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::shift_right(size_t num_bits) const
{
    UnsignedBigInteger output;

    UnsignedBigIntegerAlgorithms::shift_right_without_allocation(*this, num_bits, output);

    return output;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::as_n_bits(size_t n) const
{
    if (auto const num_bits = one_based_index_of_highest_set_bit(); n >= num_bits)
        return *this;

    UnsignedBigInteger output;
    output.set_to(*this);

    auto const word_index = n / BITS_IN_WORD;

    auto const bits_to_keep = n % BITS_IN_WORD;
    auto const bits_to_discard = BITS_IN_WORD - bits_to_keep;

    output.m_words.resize(word_index + 1);

    auto const last_word = output.m_words[word_index];
    Word new_last_word = 0;

    // avoid UB from a 32 bit shift on a u32
    if (bits_to_keep != 0)
        new_last_word = last_word << bits_to_discard >> bits_to_discard;

    output.m_words[word_index] = new_last_word;

    return output;
}

FLATTEN UnsignedBigInteger UnsignedBigInteger::multiplied_by(UnsignedBigInteger const& other) const
{
    UnsignedBigInteger result;
    UnsignedBigInteger temp_shift;

    UnsignedBigIntegerAlgorithms::multiply_without_allocation(*this, other, temp_shift, result);

    return result;
}

FLATTEN UnsignedDivisionResult UnsignedBigInteger::divided_by(UnsignedBigInteger const& divisor) const
{
    UnsignedBigInteger quotient;
    UnsignedBigInteger remainder;

    // If we actually have a u16-compatible divisor, short-circuit to the
    // less computationally-intensive "divide_u16_without_allocation" method.
    if (divisor.trimmed_length() == 1 && divisor.m_words[0] < (1 << 16)) {
        UnsignedBigIntegerAlgorithms::divide_u16_without_allocation(*this, divisor.m_words[0], quotient, remainder);
        return UnsignedDivisionResult { quotient, remainder };
    }

    UnsignedBigIntegerAlgorithms::divide_without_allocation(*this, divisor, quotient, remainder);

    return UnsignedDivisionResult { quotient, remainder };
}

u32 UnsignedBigInteger::hash() const
{
    if (m_cached_hash != 0)
        return m_cached_hash;

    return m_cached_hash = string_hash((char const*)m_words.data(), sizeof(Word) * m_words.size());
}

void UnsignedBigInteger::set_bit_inplace(size_t bit_index)
{
    size_t const word_index = bit_index / UnsignedBigInteger::BITS_IN_WORD;
    size_t const inner_word_index = bit_index % UnsignedBigInteger::BITS_IN_WORD;

    m_words.ensure_capacity(word_index + 1);

    for (size_t i = length(); i <= word_index; ++i) {
        m_words.unchecked_append(0);
    }
    m_words[word_index] |= (1 << inner_word_index);

    m_cached_trimmed_length = {};
    m_cached_hash = 0;
}

bool UnsignedBigInteger::operator==(UnsignedBigInteger const& other) const
{
    if (is_invalid() != other.is_invalid())
        return false;

    auto length = trimmed_length();

    if (length != other.trimmed_length())
        return false;

    return !__builtin_memcmp(m_words.data(), other.words().data(), length * (BITS_IN_WORD / 8));
}

bool UnsignedBigInteger::operator!=(UnsignedBigInteger const& other) const
{
    return !(*this == other);
}

bool UnsignedBigInteger::operator<(UnsignedBigInteger const& other) const
{
    auto length = trimmed_length();
    auto other_length = other.trimmed_length();

    if (length < other_length) {
        return true;
    }

    if (length > other_length) {
        return false;
    }

    if (length == 0) {
        return false;
    }
    for (int i = length - 1; i >= 0; --i) {
        if (m_words[i] == other.m_words[i])
            continue;
        return m_words[i] < other.m_words[i];
    }
    return false;
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

    // Now we shift value to the left virtually, with `exponent - bias` steps
    // we then pretend both it and the big int are extended with virtual zeros.
    auto next_bigint_word = (BITS_IN_WORD - 1 + bigint_bits_needed) / BITS_IN_WORD;

    VERIFY(next_bigint_word == trimmed_length());

    auto msb_in_top_word_index = (bigint_bits_needed - 1) % BITS_IN_WORD;
    VERIFY(msb_in_top_word_index == (BITS_IN_WORD - count_leading_zeroes(words()[next_bigint_word - 1]) - 1));

    // We will keep the bits which are still valid in the mantissa at the top of mantissa bits.
    mantissa_bits <<= 64 - (extractor.mantissa_bits + 1);

    auto bits_left_in_mantissa = static_cast<size_t>(extractor.mantissa_bits) + 1;

    auto get_next_value_bits = [&](size_t num_bits) -> Word {
        VERIFY(num_bits < 63);
        VERIFY(bits_left_in_mantissa > 0);
        if (num_bits > bits_left_in_mantissa)
            num_bits = bits_left_in_mantissa;

        bits_left_in_mantissa -= num_bits;

        u64 extracted_bits = mantissa_bits & (((1ull << num_bits) - 1) << (64 - num_bits));
        // Now shift the bits down to put the most significant bit on the num_bits position
        // this means the rest will be "virtual" zeros.
        extracted_bits >>= 32;

        // Now shift away the used bits and fit the result into a Word.
        mantissa_bits <<= num_bits;

        VERIFY(extracted_bits <= NumericLimits<Word>::max());
        return static_cast<Word>(extracted_bits);
    };

    auto bits_in_next_bigint_word = msb_in_top_word_index + 1;

    while (next_bigint_word > 0 && bits_left_in_mantissa > 0) {
        Word bigint_word = words()[next_bigint_word - 1];
        Word double_word = get_next_value_bits(bits_in_next_bigint_word);

        // For the first bit we have to align it with the top bit of bigint
        // and for all the other cases bits_in_next_bigint_word is 32 so this does nothing.
        double_word >>= 32 - bits_in_next_bigint_word;

        if (bigint_word < double_word)
            return CompareResult::DoubleGreaterThanBigInt;

        if (bigint_word > double_word)
            return CompareResult::DoubleLessThanBigInt;

        --next_bigint_word;
        bits_in_next_bigint_word = BITS_IN_WORD;
    }

    // If there are still bits left in bigint than any non zero bit means it has greater value.
    if (next_bigint_word > 0) {
        VERIFY(bits_left_in_mantissa == 0);
        while (next_bigint_word > 0) {
            if (words()[next_bigint_word - 1] != 0)
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
    if (value.is_invalid())
        return fmtbuilder.put_string("invalid"sv);

    StringBuilder builder;
    for (int i = value.length() - 1; i >= 0; --i)
        TRY(builder.try_appendff("{}|", value.words()[i]));

    return Formatter<StringView>::format(fmtbuilder, builder.string_view());
}
