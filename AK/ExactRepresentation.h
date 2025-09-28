/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>

namespace AK {

constexpr __uint128_t int_with_n_1s(size_t n)
{
    if (n == 0)
        return 0;

    __uint128_t out = 1;
    for (size_t i = 1; i < n; i++) {
        out = out << 1;
        out |= 1;
    }
    return out;
}

template<FloatingPoint F, Integral I>
constexpr bool has_exact_representation(I value)
{
    constexpr size_t mantissa_length = NumericLimits<F>::mantissa_length();

    if constexpr ((mantissa_length >= (sizeof(I) * 8) && NumericLimits<I>::is_signed()) || (mantissa_length > (sizeof(I) * 8))) {
        return true;
    }

    constexpr __uint128_t max_representable_value_no_exp = int_with_n_1s(mantissa_length);

    __uint128_t value_in_processing;
    if constexpr (NumericLimits<I>::is_signed()) {
        if (value == NumericLimits<I>::min()) {
            // hacky fix for the fact that in a signed integer
            // the minimum value of an integer has no positive counterpart
            value_in_processing = AK::abs(value / 2);
        } else {
            value_in_processing = AK::abs(value);
        }
    } else {
        value_in_processing = value;
    }

    while (true) {
        if ((value_in_processing & ~max_representable_value_no_exp) == 0) {
            return true;
        }

        if (value_in_processing % 2 == 0) {
            value_in_processing = value_in_processing >> 1;
        } else {
            return false;
        }
    }
}

}
