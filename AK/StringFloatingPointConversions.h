/*
 * Copyright (c) 2022, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Types.h>

namespace AK {

struct FloatingPointExponentialForm {
    bool sign;
    u64 fraction;
    i32 exponent;
};

/// This function finds the representation of `value' in the form of
/// `(-1) ^ sign * fraction * 10 ^ exponent', such that (applying in the order of enumeration):
///
///  1. sign is either 0 or 1, fraction is a non-negative number, exponent is an integer.
///  2. For +0, it is {.sign = 0, .fraction = 0, .exponent = 0},
///     for -0, is {.sign = 1, .fraction = 0, .exponent = 0},
///     for +inf, -inf, and NaN is undefined.
///  3. `(-1) ^ sign * fraction * 10 ^ exponent', computed with an infinite precision, rounds to
///     `value' in the half to even mode.
///  4. len(str(fraction)) is minimal.
///  5. `abs((-1) ^ sign * fraction * 10 ^ exponent - value)' is minimal.
///  6. fraction is even.
template<FloatingPoint T>
FloatingPointExponentialForm convert_floating_point_to_decimal_exponential_form(T value);

}

#if USING_AK_GLOBALLY
using AK::convert_floating_point_to_decimal_exponential_form;
#endif
