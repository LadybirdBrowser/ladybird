/*
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Diagnostics.h>
#include <AK/UFixedBigInt.h>

namespace AK::Detail {

template<size_t dividend_bit_size, size_t divisor_bit_size, bool restore_remainder>
constexpr void div_mod_internal(
    StaticStorage<false, dividend_bit_size> const& operand1,
    StaticStorage<false, divisor_bit_size> const& operand2,
    StaticStorage<false, dividend_bit_size>& quotient,
    StaticStorage<false, divisor_bit_size>& remainder)
{
    using Ops = StorageOperations<>;

    size_t dividend_len = operand1.size(), divisor_len = operand2.size();
    while (divisor_len > 0 && !operand2[divisor_len - 1])
        --divisor_len;
    while (dividend_len > 0 && !operand1[dividend_len - 1])
        --dividend_len;

    // FIXME: Should raise SIGFPE instead
    VERIFY(divisor_len); // VERIFY(divisor != 0)

    // Fast paths
    if (divisor_len == 1 && operand2[0] == 1) { // divisor == 1
        quotient = operand1;
        if constexpr (restore_remainder)
            Ops::set(0, remainder);
        return;
    }

    if (dividend_len < divisor_len) { // dividend < divisor
        Ops::set(0, quotient);
        if constexpr (restore_remainder)
            Ops::copy(operand1, remainder);
        return;
    }

    if (divisor_len == 1 && dividend_len == 1) { // NativeWord / NativeWord
        Ops::set(operand1[0] / operand2[0], quotient);
        if constexpr (restore_remainder)
            Ops::set(operand1[0] % operand2[0], remainder);
        return;
    }

    if (divisor_len == 1) { // BigInt by NativeWord
        auto u = (static_cast<NativeDoubleWord>(operand1[dividend_len - 1]) << native_word_size) + operand1[dividend_len - 2];
        auto divisor = operand2[0];

        auto top = u / divisor;
        quotient[dividend_len - 1] = static_cast<NativeWord>(top >> native_word_size);
        quotient[dividend_len - 2] = static_cast<NativeWord>(top);

        auto carry = static_cast<NativeWord>(u % divisor);
        for (size_t i = dividend_len - 2; i--;)
            quotient[i] = div_mod_words(operand1[i], carry, divisor, carry);
        for (size_t i = dividend_len; i < quotient.size(); ++i)
            quotient[i] = 0;
        if constexpr (restore_remainder)
            Ops::set(carry, remainder);
        return;
    }

    // Knuth's algorithm D
    StaticStorage<false, dividend_bit_size + native_word_size> dividend;
    Ops::copy(operand1, dividend);
    auto divisor = operand2;

    Ops::div_mod_internal<restore_remainder>(dividend, divisor, quotient, remainder, dividend_len, divisor_len);
}

}
