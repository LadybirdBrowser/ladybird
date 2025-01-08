/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"
#include <AK/NumericLimits.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

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
FLATTEN ErrorOr<void> UnsignedBigIntegerAlgorithms::bitwise_not_fill_to_one_based_index_without_allocation(
    UnsignedBigInteger const& right,
    size_t index,
    UnsignedBigInteger& output)
{
    // If the value is invalid, the output value is invalid as well.
    if (right.is_invalid()) {
        output.invalidate();
        return {};
    }

    if (index == 0) {
        output.set_to_0();
        return {};
    }
    size_t size = (index + UnsignedBigInteger::BITS_IN_WORD - 1) / UnsignedBigInteger::BITS_IN_WORD;

    TRY(output.m_words.try_resize_and_keep_capacity(size));
    VERIFY(size > 0);
    for (size_t i = 0; i < size - 1; ++i)
        output.m_words[i] = ~(i < right.length() ? right.words()[i] : 0);

    index -= (size - 1) * UnsignedBigInteger::BITS_IN_WORD;
    auto last_word_index = size - 1;
    auto last_word = last_word_index < right.length() ? right.words()[last_word_index] : 0;

    output.m_words[last_word_index] = (NumericLimits<UnsignedBigInteger::Word>::max() >> (UnsignedBigInteger::BITS_IN_WORD - index)) & ~last_word;

    return {};
}

FLATTEN void UnsignedBigIntegerAlgorithms::shift_left_without_allocation(
    UnsignedBigInteger const& number,
    size_t num_bits,
    UnsignedBigInteger& output)
{
    MUST(try_shift_left_without_allocation(number, num_bits, output));
}

/**
 * Complexity : O(N) where N is the number of words in the number
 */
FLATTEN ErrorOr<void> UnsignedBigIntegerAlgorithms::try_shift_left_without_allocation(
    UnsignedBigInteger const& number,
    size_t num_bits,
    UnsignedBigInteger& output)
{
    size_t const bit_shift = num_bits % UnsignedBigInteger::BITS_IN_WORD;
    size_t const bit_shift_complement = UnsignedBigInteger::BITS_IN_WORD - bit_shift;

    size_t const zero_based_index_of_highest_set_bit_in_hiword = (number.one_based_index_of_highest_set_bit() - 1) % UnsignedBigInteger::BITS_IN_WORD;

    // true if the high word is a result of the bit_shift
    bool const hiword_shift = (bit_shift + zero_based_index_of_highest_set_bit_in_hiword) >= UnsignedBigInteger::BITS_IN_WORD;
    size_t const word_shift = num_bits / UnsignedBigInteger::BITS_IN_WORD;

    TRY(try_shift_left_by_n_words(number, word_shift + (hiword_shift ? 1 : 0), output));

    if (bit_shift == 0) // shifting left by an exact number of words)
        return {};

    UnsignedBigInteger::Word carry = 0;
    for (size_t i = 0; i < number.length(); ++i) {
        size_t const output_index = i + word_shift;

        output.m_words[output_index] = (number.m_words.at(i) << bit_shift) | carry;
        carry = (number.m_words.at(i) >> bit_shift_complement);
    }

    if (hiword_shift)
        output.m_words[output.length() - 1] = carry;

    return {};
}

/**
 * Complexity : O(N) where N is the number of words in the number
 */
FLATTEN void UnsignedBigIntegerAlgorithms::shift_right_without_allocation(
    UnsignedBigInteger const& number,
    size_t num_bits,
    UnsignedBigInteger& output)
{
    size_t const bit_shift = num_bits % UnsignedBigInteger::BITS_IN_WORD;
    size_t const bit_shift_complement = UnsignedBigInteger::BITS_IN_WORD - bit_shift;
    size_t const zero_based_index_of_highest_set_bit_in_hiword = (number.one_based_index_of_highest_set_bit() - 1) % UnsignedBigInteger::BITS_IN_WORD;

    // true if the high word will be zeroed as a result of the shift
    bool const hiword_zero = (bit_shift > zero_based_index_of_highest_set_bit_in_hiword);
    size_t const word_shift = num_bits / UnsignedBigInteger::BITS_IN_WORD + (hiword_zero ? 1 : 0);

    if (word_shift >= number.length()) { // all non-zero digits have been shifted right; result is zero
        output.set_to_0();
        return;
    }

    shift_right_by_n_words(number, word_shift, output);

    if (bit_shift == 0) // shifting right by an exact number of words)
        return;

    size_t const output_length = output.length();
    size_t number_index = number.length() - 1;

    UnsignedBigInteger::Word carry = 0;

    if (hiword_zero) {
        carry = number.words().at(number_index) << bit_shift_complement;
        --number_index;
    }

    for (size_t i = 0; i < output_length; ++i) {
        size_t const output_index = output_length - i - 1; // downto index 0

        output.m_words[output_index] = ((number.m_words.at(number_index) >> bit_shift)) | carry;
        carry = (number.m_words.at(number_index) << bit_shift_complement);
        --number_index;
    }
}

void UnsignedBigIntegerAlgorithms::shift_left_by_n_words(
    UnsignedBigInteger const& number,
    size_t number_of_words,
    UnsignedBigInteger& output)
{
    MUST(try_shift_left_by_n_words(number, number_of_words, output));
}

ErrorOr<void> UnsignedBigIntegerAlgorithms::try_shift_left_by_n_words(
    UnsignedBigInteger const& number,
    size_t number_of_words,
    UnsignedBigInteger& output)
{
    // shifting left by N words means just inserting N zeroes to the beginning of the words vector
    output.set_to_0();
    TRY(output.m_words.try_resize_and_keep_capacity(number_of_words + number.length()));

    __builtin_memset(output.m_words.data(), 0, number_of_words * sizeof(unsigned));
    __builtin_memcpy(&output.m_words.data()[number_of_words], number.m_words.data(), number.m_words.size() * sizeof(unsigned));

    return {};
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
