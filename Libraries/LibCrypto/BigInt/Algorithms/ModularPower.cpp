/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"

namespace Crypto {

void UnsignedBigIntegerAlgorithms::destructive_modular_power_without_allocation(
    UnsignedBigInteger& ep,
    UnsignedBigInteger& base,
    UnsignedBigInteger const& m,
    UnsignedBigInteger& temp_1,
    UnsignedBigInteger& temp_multiply,
    UnsignedBigInteger& temp_quotient,
    UnsignedBigInteger& temp_remainder,
    UnsignedBigInteger& exp)
{
    exp.set_to(1);
    while (!(ep < 1)) {
        if (ep.words()[0] % 2 == 1) {
            // exp = (exp * base) % m;
            multiply_without_allocation(exp, base, temp_1, temp_multiply);
            divide_without_allocation(temp_multiply, m, temp_quotient, temp_remainder);
            exp.set_to(temp_remainder);
        }

        // ep = ep / 2;
        ep.set_to(ep.shift_right(1));

        // base = (base * base) % m;
        multiply_without_allocation(base, base, temp_1, temp_multiply);
        divide_without_allocation(temp_multiply, m, temp_quotient, temp_remainder);
        base.set_to(temp_remainder);

        // Note that not clamping here would cause future calculations (multiply, specifically) to allocate even more unused space
        // which would then persist through the temp bigints, and significantly slow down later loops.
        // To avoid that, we can clamp to a specific max size, or just clamp to the min needed amount of space.
        ep.clamp_to_trimmed_length();
        exp.clamp_to_trimmed_length();
        base.clamp_to_trimmed_length();
    }
}

/**
 * Compute (1/value) % 2^32.
 * This needs an odd input value
 * Algorithm from: Dumas, J.G. "On Newton–Raphson Iteration for Multiplicative Inverses Modulo Prime Powers".
 */
ALWAYS_INLINE static u32 inverse_wrapped(u32 value)
{
    VERIFY(value & 1);

    u64 b = static_cast<u64>(value);
    u64 k0 = (2 - b);
    u64 t = (b - 1);
    size_t i = 1;
    while (i < 32) {
        t = t * t;
        k0 = k0 * (t + 1);
        i <<= 1;
    }
    return static_cast<u32>(-k0);
}

/**
 * Computes z = x * y + c. z_carry contains the top bits, z contains the bottom bits.
 */
ALWAYS_INLINE static void linear_multiplication_with_carry(u32 x, u32 y, u32 c, u32& z_carry, u32& z)
{
    u64 result = static_cast<u64>(x) * static_cast<u64>(y) + static_cast<u64>(c);
    z_carry = static_cast<u32>(result >> 32);
    z = static_cast<u32>(result);
}

/**
 * Computes z = a + b. z_carry contains the top bit (1 or 0), z contains the bottom bits.
 */
ALWAYS_INLINE static void addition_with_carry(u32 a, u32 b, u32& z_carry, u32& z)
{
    u64 result = static_cast<u64>(a) + static_cast<u64>(b);
    z_carry = static_cast<u32>(result >> 32);
    z = static_cast<u32>(result);
}

/**
 * Computes a montgomery "fragment" for y_i. This computes "z[i] += x[i] * y_i" for all words while rippling the carry, and returns the carry.
 * Algorithm from: Gueron, "Efficient Software Implementations of Modular Exponentiation". (https://eprint.iacr.org/2011/239.pdf)
 */
UnsignedBigInteger::Word UnsignedBigIntegerAlgorithms::montgomery_fragment(UnsignedBigInteger& z, size_t offset_in_z, UnsignedBigInteger const& x, UnsignedBigInteger::Word y_digit, size_t num_words)
{
    UnsignedBigInteger::Word carry { 0 };
    for (size_t i = 0; i < num_words; ++i) {
        UnsignedBigInteger::Word a_carry;
        UnsignedBigInteger::Word a;
        linear_multiplication_with_carry(x.m_words[i], y_digit, z.m_words[offset_in_z + i], a_carry, a);
        UnsignedBigInteger::Word b_carry;
        UnsignedBigInteger::Word b;
        addition_with_carry(a, carry, b_carry, b);
        z.m_words[offset_in_z + i] = b;
        carry = a_carry + b_carry;
    }
    return carry;
}

/**
 * Computes the "almost montgomery" product : x * y * 2 ^ (-num_words * BITS_IN_WORD) % modulo
 * [Note : that means that the result z satisfies z * 2^(num_words * BITS_IN_WORD) % modulo = x * y % modulo]
 * assuming :
 *  - x, y and modulo are all already padded to num_words
 *  - k = inverse_wrapped(modulo) (optimization to not recompute K each time)
 * Algorithm from: Gueron, "Efficient Software Implementations of Modular Exponentiation". (https://eprint.iacr.org/2011/239.pdf)
 */
void UnsignedBigIntegerAlgorithms::almost_montgomery_multiplication_without_allocation(
    UnsignedBigInteger const& x,
    UnsignedBigInteger const& y,
    UnsignedBigInteger const& modulo,
    UnsignedBigInteger& z,
    UnsignedBigInteger::Word k,
    size_t num_words,
    UnsignedBigInteger& result)
{
    VERIFY(x.length() >= num_words);
    VERIFY(y.length() >= num_words);
    VERIFY(modulo.length() >= num_words);

    z.set_to(0);
    z.resize_with_leading_zeros(num_words * 2);

    UnsignedBigInteger::Word previous_double_carry { 0 };
    for (size_t i = 0; i < num_words; ++i) {
        // z[i->num_words+i] += x * y_i
        UnsignedBigInteger::Word carry_1 = montgomery_fragment(z, i, x, y.m_words[i], num_words);
        // z[i->num_words+i] += modulo * (z_i * k)
        UnsignedBigInteger::Word t = z.m_words[i] * k;
        UnsignedBigInteger::Word carry_2 = montgomery_fragment(z, i, modulo, t, num_words);

        // Compute the carry by combining all of the carries of the previous computations
        // Put it "right after" the range that we computed above
        UnsignedBigInteger::Word temp_carry = previous_double_carry + carry_1;
        UnsignedBigInteger::Word overall_carry = temp_carry + carry_2;
        z.m_words[num_words + i] = overall_carry;

        // Detect if there was a "double carry" for this word by checking if our carry results are smaller than their components
        previous_double_carry = (temp_carry < carry_1 || overall_carry < carry_2) ? 1 : 0;
    }

    if (previous_double_carry == 0) {
        // Return the top num_words bytes of Z, which contains our result.
        shift_right_by_n_words(z, num_words, result);
        result.resize_with_leading_zeros(num_words);
        return;
    }

    // We have a carry, so we're "one bigger" than we need to be.
    // Subtract the modulo from the result (the top half of z), and write it to the bottom half of Z since we have space.
    // (With carry, of course.)
    UnsignedBigInteger::Word c { 0 };
    for (size_t i = 0; i < num_words; ++i) {
        UnsignedBigInteger::Word z_digit = z.m_words[num_words + i];
        UnsignedBigInteger::Word modulo_digit = modulo.m_words[i];
        UnsignedBigInteger::Word new_z_digit = z_digit - modulo_digit - c;
        z.m_words[i] = new_z_digit;
        // Detect if the subtraction underflowed - from "Hacker's Delight"
        c = ((modulo_digit & ~z_digit) | ((modulo_digit | ~z_digit) & new_z_digit)) >> (UnsignedBigInteger::BITS_IN_WORD - 1);
    }

    // Return the bottom num_words bytes of Z (with the carry bit handled)
    z.m_words.resize(num_words);
    result.set_to(z);
    result.resize_with_leading_zeros(num_words);
}

/**
 * Complexity: still O(N^3) with N the number of words in the largest word, but less complex than the classical mod power.
 * Note: the montgomery multiplications requires an inverse modulo over 2^32, which is only defined for odd numbers.
 */
void UnsignedBigIntegerAlgorithms::montgomery_modular_power_with_minimal_allocations(
    UnsignedBigInteger const& base,
    UnsignedBigInteger const& exponent,
    UnsignedBigInteger const& modulo,
    UnsignedBigInteger& temp_z,
    UnsignedBigInteger& rr,
    UnsignedBigInteger& one,
    UnsignedBigInteger& z,
    UnsignedBigInteger& zz,
    UnsignedBigInteger& x,
    UnsignedBigInteger& temp_extra,
    UnsignedBigInteger& result)
{
    VERIFY(modulo.is_odd());

    // Note: While this is a constexpr variable for clarity and could be changed in theory,
    // various optimized parts of the algorithm rely on this value being exactly 4.
    constexpr size_t window_size = 4;

    size_t num_words = modulo.trimmed_length();
    UnsignedBigInteger::Word k = inverse_wrapped(modulo.m_words[0]);

    one.set_to(1);

    // rr = ( 2 ^ (2 * modulo.length() * BITS_IN_WORD) ) % modulo
    shift_left_by_n_words(one, 2 * num_words, x);
    divide_without_allocation(x, modulo, temp_extra, rr);
    rr.resize_with_leading_zeros(num_words);

    // x = base [% modulo, if x doesn't already fit in modulo's words]
    x.set_to(base);
    if (x.trimmed_length() > num_words)
        divide_without_allocation(base, modulo, temp_extra, x);
    x.resize_with_leading_zeros(num_words);

    one.set_to(1);
    one.resize_with_leading_zeros(num_words);

    // Compute the montgomery powers from 0 to 2^window_size. powers[i] = x^i
    UnsignedBigInteger powers[1 << window_size];
    almost_montgomery_multiplication_without_allocation(one, rr, modulo, temp_z, k, num_words, powers[0]);
    almost_montgomery_multiplication_without_allocation(x, rr, modulo, temp_z, k, num_words, powers[1]);
    for (size_t i = 2; i < (1 << window_size); ++i)
        almost_montgomery_multiplication_without_allocation(powers[i - 1], powers[1], modulo, temp_z, k, num_words, powers[i]);

    z.set_to(powers[0]);
    z.resize_with_leading_zeros(num_words);
    zz.set_to(0);
    zz.resize_with_leading_zeros(num_words);

    ssize_t exponent_length = exponent.trimmed_length();
    for (ssize_t word_in_exponent = exponent_length - 1; word_in_exponent >= 0; --word_in_exponent) {
        UnsignedBigInteger::Word exponent_word = exponent.m_words[word_in_exponent];
        size_t bit_in_word = 0;
        while (bit_in_word < UnsignedBigInteger::BITS_IN_WORD) {
            if (word_in_exponent != exponent_length - 1 || bit_in_word != 0) {
                almost_montgomery_multiplication_without_allocation(z, z, modulo, temp_z, k, num_words, zz);
                almost_montgomery_multiplication_without_allocation(zz, zz, modulo, temp_z, k, num_words, z);
                almost_montgomery_multiplication_without_allocation(z, z, modulo, temp_z, k, num_words, zz);
                almost_montgomery_multiplication_without_allocation(zz, zz, modulo, temp_z, k, num_words, z);
            }
            auto power_index = exponent_word >> (UnsignedBigInteger::BITS_IN_WORD - window_size);
            auto& power = powers[power_index];
            almost_montgomery_multiplication_without_allocation(z, power, modulo, temp_z, k, num_words, zz);

            swap(z, zz);

            // Move to the next window
            exponent_word <<= window_size;
            bit_in_word += window_size;
        }
    }

    almost_montgomery_multiplication_without_allocation(z, one, modulo, temp_z, k, num_words, zz);

    if (zz < modulo) {
        result.set_to(zz);
        result.clamp_to_trimmed_length();
        return;
    }

    // Note : Since we were using "almost montgomery" multiplications, we aren't guaranteed to be under the modulo already.
    // So, if we're here, we need to respect the modulo.
    // We can, however, start by trying to subtract the modulo, just in case we're close.
    subtract_without_allocation(zz, modulo, result);

    if (modulo < zz) {
        // Note: This branch shouldn't happen in theory (as noted in https://github.com/rust-num/num-bigint/blob/master/src/biguint/monty.rs#L210)
        // Let's dbgln the values we used. That way, if we hit this branch, we can contribute these values for test cases.
        dbgln("Encountered the modulo branch during a montgomery modular power. Params : {} - {} - {}", base, exponent, modulo);
        // We just clobber all the other temporaries that we don't need for the division.
        // This is wasteful, but we're on the edgiest of cases already.
        divide_without_allocation(zz, modulo, temp_extra, result);
    }

    result.clamp_to_trimmed_length();
}

}
