/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"
#include <AK/BigIntBase.h>
#include <AK/BuiltinWrappers.h>
#include <AK/NumericLimits.h>

namespace Crypto {

/**
 * Complexity: O(N) where N is the number of words in the shorter value
 * Method:
 * Apply <op> word-wise until words in the shorter value are used up
 * then copy the rest of the words verbatim from the longer value.
 */
FLATTEN void UnsignedBigIntegerAlgorithms::bitwise_or_without_allocation(
    UnsignedBigInteger const& left,
    UnsignedBigInteger const& right,
    UnsignedBigInteger& output)
{
    // If either of the BigInts are invalid, the output is just the other one.
    if (left.is_invalid()) {
        output.set_to(right);
        return;
    }
    if (right.is_invalid()) {
        output.set_to(left);
        return;
    }

    UnsignedBigInteger const *shorter, *longer;
    if (left.length() < right.length()) {
        shorter = &left;
        longer = &right;
    } else {
        shorter = &right;
        longer = &left;
    }

    output.m_words.resize_and_keep_capacity(longer->length());

    size_t longer_offset = longer->length() - shorter->length();
    for (size_t i = 0; i < shorter->length(); ++i)
        output.m_words[i] = longer->words()[i] | shorter->words()[i];

    __builtin_memcpy(output.m_words.data() + shorter->length(), longer->words().data() + shorter->length(), sizeof(u32) * longer_offset);
}

/**
 * Complexity: O(N) where N is the number of words in the shorter value
 * Method:
 * Apply 'and' word-wise until words in the shorter value are used up
 * and zero the rest.
 */
FLATTEN void UnsignedBigIntegerAlgorithms::bitwise_and_without_allocation(
    UnsignedBigInteger const& left,
    UnsignedBigInteger const& right,
    UnsignedBigInteger& output)
{
    // If either of the BigInts are invalid, the output is just the other one.
    if (left.is_invalid()) {
        output.set_to(right);
        return;
    }
    if (right.is_invalid()) {
        output.set_to(left);
        return;
    }

    UnsignedBigInteger const *shorter, *longer;
    if (left.length() < right.length()) {
        shorter = &left;
        longer = &right;
    } else {
        shorter = &right;
        longer = &left;
    }

    output.m_words.resize_and_keep_capacity(longer->length());

    size_t longer_offset = longer->length() - shorter->length();
    for (size_t i = 0; i < shorter->length(); ++i)
        output.m_words[i] = longer->words()[i] & shorter->words()[i];

    __builtin_memset(output.m_words.data() + shorter->length(), 0, sizeof(u32) * longer_offset);
}

/**
 * Complexity: O(N) where N is the number of words in the shorter value
 * Method:
 * Apply 'xor' word-wise until words in the shorter value are used up
 * and copy the rest.
 */
FLATTEN void UnsignedBigIntegerAlgorithms::bitwise_xor_without_allocation(
    UnsignedBigInteger const& left,
    UnsignedBigInteger const& right,
    UnsignedBigInteger& output)
{
    // If either of the BigInts are invalid, the output is just the other one.
    if (left.is_invalid()) {
        output.set_to(right);
        return;
    }
    if (right.is_invalid()) {
        output.set_to(left);
        return;
    }

    UnsignedBigInteger const *shorter, *longer;
    if (left.length() < right.length()) {
        shorter = &left;
        longer = &right;
    } else {
        shorter = &right;
        longer = &left;
    }

    output.m_words.resize_and_keep_capacity(longer->length());

    size_t longer_offset = longer->length() - shorter->length();
    for (size_t i = 0; i < shorter->length(); ++i)
        output.m_words[i] = longer->words()[i] ^ shorter->words()[i];

    __builtin_memcpy(output.m_words.data() + shorter->length(), longer->words().data() + shorter->length(), sizeof(u32) * longer_offset);
}

/**
 * Complexity: O(N) where N is the number of words
 */
FLATTEN void UnsignedBigIntegerAlgorithms::bitwise_not_fill_to_one_based_index_without_allocation(
    UnsignedBigInteger const& right,
    size_t index,
    UnsignedBigInteger& output)
{
    // If the value is invalid, the output value is invalid as well.
    if (right.is_invalid()) {
        output.invalidate();
        return;
    }

    if (index == 0) {
        output.set_to_0();
        return;
    }
    size_t size = (index + UnsignedBigInteger::BITS_IN_WORD - 1) / UnsignedBigInteger::BITS_IN_WORD;

    output.m_words.resize_and_keep_capacity(size);
    VERIFY(size > 0);
    for (size_t i = 0; i < size - 1; ++i)
        output.m_words[i] = ~(i < right.length() ? right.words()[i] : 0);

    index -= (size - 1) * UnsignedBigInteger::BITS_IN_WORD;
    auto last_word_index = size - 1;
    auto last_word = last_word_index < right.length() ? right.words()[last_word_index] : 0;

    output.m_words[last_word_index] = (NumericLimits<UnsignedBigInteger::Word>::max() >> (UnsignedBigInteger::BITS_IN_WORD - index)) & ~last_word;
}

/**
 * Complexity : O(N + num_bits % 8) where N is the number of words in the number
 * Shift method :
 * Start by shifting by whole words in num_bits (by putting missing words at the start),
 * then shift the number's words two by two by the remaining amount of bits.
 */
FLATTEN void UnsignedBigIntegerAlgorithms::shift_left_without_allocation(
    UnsignedBigInteger const& number,
    size_t num_bits,
    UnsignedBigInteger& temp_result,
    UnsignedBigInteger& temp_plus,
    UnsignedBigInteger& output)
{
    // We can only do shift operations on individual words
    // where the shift amount is <= size of word (32).
    // But we do know how to shift by a multiple of word size (e.g 64=32*2)
    // So we first shift the result by how many whole words fit in 'num_bits'
    shift_left_by_n_words(number, num_bits / UnsignedBigInteger::BITS_IN_WORD, temp_result);

    output.set_to(temp_result);

    // And now we shift by the leftover amount of bits
    num_bits %= UnsignedBigInteger::BITS_IN_WORD;

    if (num_bits == 0) {
        return;
    }

    for (size_t i = 0; i < temp_result.length(); ++i) {
        u32 current_word_of_temp_result = shift_left_get_one_word(temp_result, num_bits, i);
        output.m_words[i] = current_word_of_temp_result;
    }

    // Shifting the last word can produce a carry
    u32 carry_word = shift_left_get_one_word(temp_result, num_bits, temp_result.length());
    if (carry_word != 0) {

        // output += (carry_word << temp_result.length())
        // FIXME : Using temp_plus this way to transform carry_word into a bigint is not
        //         efficient nor pretty. Maybe we should have an "add_with_shift" method ?
        temp_plus.set_to_0();
        temp_plus.m_words.append(carry_word);
        shift_left_by_n_words(temp_plus, temp_result.length(), temp_result);
        add_into_accumulator_without_allocation(output, temp_result);
    }
}

FLATTEN void UnsignedBigIntegerAlgorithms::shift_right_without_allocation(
    UnsignedBigInteger const& number,
    size_t num_bits,
    UnsignedBigInteger& output)
{
    output.m_words.resize_and_keep_capacity(number.length() - (num_bits / UnsignedBigInteger::BITS_IN_WORD));
    Ops::shift_right(number.words_span(), num_bits, output.words_span());
}

void UnsignedBigIntegerAlgorithms::shift_left_by_n_words(
    UnsignedBigInteger const& number,
    size_t number_of_words,
    UnsignedBigInteger& output)
{
    // shifting left by N words means just inserting N zeroes to the beginning of the words vector
    output.set_to_0();
    output.m_words.resize_and_keep_capacity(number_of_words + number.length());

    __builtin_memset(output.m_words.data(), 0, number_of_words * sizeof(unsigned));
    __builtin_memcpy(&output.m_words.data()[number_of_words], number.m_words.data(), number.m_words.size() * sizeof(unsigned));
}

void UnsignedBigIntegerAlgorithms::shift_right_by_n_words(
    UnsignedBigInteger const& number,
    size_t number_of_words,
    UnsignedBigInteger& output)
{
    // shifting right by N words means just not copying the first words
    output.set_to_0();
    output.m_words.resize_and_keep_capacity(number.length() - number_of_words);
    __builtin_memcpy(output.m_words.data(), &number.m_words.data()[number_of_words], (number.m_words.size() - number_of_words) * sizeof(unsigned));
}

/**
 * Returns the word at a requested index in the result of a shift operation
 */
ALWAYS_INLINE UnsignedBigInteger::Word UnsignedBigIntegerAlgorithms::shift_left_get_one_word(
    UnsignedBigInteger const& number,
    size_t num_bits,
    size_t result_word_index)
{
    // "<= length()" (rather than length() - 1) is intentional,
    // The result index of length() is used when calculating the carry word
    VERIFY(result_word_index <= number.length());
    VERIFY(num_bits <= UnsignedBigInteger::BITS_IN_WORD);
    u32 result = 0;

    // we need to check for "num_bits != 0" since shifting right by 32 is apparently undefined behavior!
    if (result_word_index > 0 && num_bits != 0) {
        result += number.m_words[result_word_index - 1] >> (UnsignedBigInteger::BITS_IN_WORD - num_bits);
    }
    if (result_word_index < number.length() && num_bits < 32) {
        result += number.m_words[result_word_index] << num_bits;
    }
    return result;
}

}
