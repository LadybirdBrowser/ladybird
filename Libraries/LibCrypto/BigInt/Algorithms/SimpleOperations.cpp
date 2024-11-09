/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"

namespace Crypto {

/**
 * Complexity: O(N) where N is the number of words in the larger number
 */
void UnsignedBigIntegerAlgorithms::add_without_allocation(
    UnsignedBigInteger const& left,
    UnsignedBigInteger const& right,
    UnsignedBigInteger& output)
{
    UnsignedBigInteger const* const longer = (left.length() > right.length()) ? &left : &right;
    UnsignedBigInteger const* const shorter = (longer == &right) ? &left : &right;

    output.set_to(*longer);
    add_into_accumulator_without_allocation(output, *shorter);
}

/**
 * Complexity: O(N) where N is the number of words in the larger number
 */
void UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(UnsignedBigInteger& accumulator, UnsignedBigInteger const& value)
{
    auto value_length = value.trimmed_length();

    // If needed, resize the accumulator so it can fit the value.
    accumulator.resize_with_leading_zeros(value_length);
    auto final_length = accumulator.length();

    // Add the words of the value into the accumulator, rippling any carry as we go
    UnsignedBigInteger::Word last_carry_for_word = 0;
    for (size_t i = 0; i < value_length; ++i) {
        UnsignedBigInteger::Word current_carry_for_word = 0;
        if (Checked<UnsignedBigInteger::Word>::addition_would_overflow(value.m_words[i], accumulator.m_words[i])) {
            current_carry_for_word = 1;
        }
        UnsignedBigInteger::Word word_addition_result = value.m_words[i] + accumulator.m_words[i];
        if (Checked<UnsignedBigInteger::Word>::addition_would_overflow(word_addition_result, last_carry_for_word)) {
            current_carry_for_word = 1;
        }
        word_addition_result += last_carry_for_word;
        last_carry_for_word = current_carry_for_word;
        accumulator.m_words[i] = word_addition_result;
    }

    // Ripple the carry over the remaining words in the accumulator until either there is no carry left or we run out of words
    while (last_carry_for_word && final_length > value_length) {
        UnsignedBigInteger::Word current_carry_for_word = 0;
        if (Checked<UnsignedBigInteger::Word>::addition_would_overflow(accumulator.m_words[value_length], last_carry_for_word)) {
            current_carry_for_word = 1;
        }
        accumulator.m_words[value_length] += last_carry_for_word;
        last_carry_for_word = current_carry_for_word;
        value_length++;
    }

    if (last_carry_for_word) {
        // Note : The accumulator couldn't add the carry directly, so we reached its end
        accumulator.m_words.append(last_carry_for_word);
    }
}

/**
 * Complexity: O(N) where N is the number of words in the larger number
 */
void UnsignedBigIntegerAlgorithms::subtract_without_allocation(
    UnsignedBigInteger const& left,
    UnsignedBigInteger const& right,
    UnsignedBigInteger& output)
{
    if (left < right) {
        output.invalidate();
        return;
    }

    u8 borrow = 0;
    auto own_length = left.length();
    auto other_length = right.length();

    output.set_to_0();
    output.m_words.resize_and_keep_capacity(own_length);

    for (size_t i = 0; i < own_length; ++i) {
        u32 other_word = (i < other_length) ? right.m_words[i] : 0;
        i64 temp = static_cast<i64>(left.m_words[i]) - static_cast<i64>(other_word) - static_cast<i64>(borrow);
        // If temp < 0, we had an underflow
        borrow = (temp >= 0) ? 0 : 1;
        if (temp < 0) {
            temp += (UINT32_MAX + 1);
        }
        output.m_words[i] = temp;
    }

    // This assertion should not fail, because we verified that *this>=other at the beginning of the function
    VERIFY(borrow == 0);
}

}
