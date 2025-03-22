/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"

namespace Crypto {

/**
 * Complexity: O(N^2) where N is the number of words in the larger number
 * Multiplication method:
 * An integer is equal to the sum of the powers of two
 * according to the indices of its 'on' bits.
 * So to multiple x*y, we go over each '1' bit in x (say the i'th bit),
 * and add y<<i to the result.
 */
FLATTEN void UnsignedBigIntegerAlgorithms::multiply_without_allocation(
    UnsignedBigInteger const& left,
    UnsignedBigInteger const& right,
    UnsignedBigInteger& temp_shift,
    UnsignedBigInteger& output)
{
    output.set_to_0();

    // iterate all bits
    for (size_t word_index = 0; word_index < left.length(); ++word_index) {
        for (size_t bit_index = 0; bit_index < UnsignedBigInteger::BITS_IN_WORD; ++bit_index) {
            // If the bit is off - skip over it
            if (!(left.m_words[word_index] & (1 << bit_index)))
                continue;

            size_t shift_amount = word_index * UnsignedBigInteger::BITS_IN_WORD + bit_index;

            // output += (right << shift_amount);
            shift_left_without_allocation(right, shift_amount, temp_shift);
            add_into_accumulator_without_allocation(output, temp_shift);
        }
    }
}

}
