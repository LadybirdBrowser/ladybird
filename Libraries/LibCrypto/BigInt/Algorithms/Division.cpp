/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"
#include <AK/BigIntBase.h>
#include <AK/BuiltinWrappers.h>

namespace Crypto {

using AK::Detail::div_mod_words;
using AK::Detail::dword;

/**
 * Complexity: O(N^2) where N is the number of words in the larger number
 * Division method:
 * Knuth's Algorithm D, see UFixedBigIntDivision.h for more details
 */
FLATTEN void UnsignedBigIntegerAlgorithms::divide_without_allocation(
    UnsignedBigInteger const& numerator,
    UnsignedBigInteger const& denominator,
    UnsignedBigInteger& quotient,
    UnsignedBigInteger& remainder)
{
    size_t dividend_len = numerator.trimmed_length();
    size_t divisor_len = denominator.trimmed_length();

    VERIFY(divisor_len != 0);

    // Fast paths
    // Division by 1
    if (divisor_len == 1 && denominator.m_words[0] == 1) {
        quotient.set_to(numerator);
        remainder.set_to_0();
        return;
    }

    if (dividend_len < divisor_len) {
        quotient.set_to_0();
        remainder.set_to(numerator);
        return;
    }

    if (divisor_len == 1 && dividend_len == 1) {
        quotient.set_to(numerator.m_words[0] / denominator.m_words[0]);
        remainder.set_to(numerator.m_words[0] % denominator.m_words[0]);
        return;
    }
    // Division by Word
    if (divisor_len == 1) {
        quotient.resize_with_leading_zeros(dividend_len);
        remainder.resize_with_leading_zeros(1);

        // FIXME: Use a "DoubleWord" to allow increasing the Word size of
        //        BigInt in the future
        static_assert(UnsignedBigInteger::BITS_IN_WORD == 32);
        auto u = dword(numerator.m_words[dividend_len - 2], numerator.m_words[dividend_len - 1]);
        auto divisor = denominator.m_words[0];

        auto top = u / divisor;
        quotient.m_words[dividend_len - 1] = top >> UnsignedBigInteger::BITS_IN_WORD;
        quotient.m_words[dividend_len - 2] = static_cast<UnsignedBigInteger::Word>(top);

        auto carry = static_cast<UnsignedBigInteger::Word>(u % divisor);
        for (size_t i = dividend_len - 2; i-- != 0;)
            quotient.m_words[i] = div_mod_words(numerator.m_words[i], carry, divisor, carry);
        remainder.m_words[0] = carry;
        return;
    }

    // Knuth's algorithm D
    auto dividend = numerator;
    dividend.resize_with_leading_zeros(dividend_len + 1);
    auto divisor = denominator;

    quotient.resize_with_leading_zeros(dividend_len - divisor_len + 1);
    remainder.resize_with_leading_zeros(divisor_len);

    Ops::div_mod_internal<true>(
        dividend.words_span(), divisor.words_span(),
        quotient.words_span(), remainder.words_span(),
        dividend_len, divisor_len);
}

/**
 * Complexity : O(N) where N is the number of digits in the numerator
 * Division method :
 * Starting from the most significant one, for each half-word of the numerator, combine it
 * with the existing remainder if any, divide the combined number as a u32 operation and
 * update the quotient / remainder as needed.
 */
FLATTEN void UnsignedBigIntegerAlgorithms::divide_u16_without_allocation(
    UnsignedBigInteger const& numerator,
    UnsignedBigInteger::Word denominator,
    UnsignedBigInteger& quotient,
    UnsignedBigInteger& remainder)
{
    VERIFY(denominator < (1 << 16));
    UnsignedBigInteger::Word remainder_word = 0;
    auto numerator_length = numerator.trimmed_length();
    quotient.set_to_0();
    quotient.m_words.resize(numerator_length);
    for (int word_index = numerator_length - 1; word_index >= 0; --word_index) {
        auto word_high = numerator.m_words[word_index] >> 16;
        auto word_low = numerator.m_words[word_index] & ((1 << 16) - 1);

        auto number_to_divide_high = (remainder_word << 16) | word_high;
        auto quotient_high = number_to_divide_high / denominator;
        remainder_word = number_to_divide_high % denominator;

        auto number_to_divide_low = remainder_word << 16 | word_low;
        auto quotient_low = number_to_divide_low / denominator;
        remainder_word = number_to_divide_low % denominator;

        quotient.m_words[word_index] = (quotient_high << 16) | quotient_low;
    }
    remainder.set_to(remainder_word);
}

}
