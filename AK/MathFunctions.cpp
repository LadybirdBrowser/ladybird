/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define LIBC_MATH (LIBC_MATH_NO_ERRNO | LIBC_MATH_NO_EXCEPT | LIBC_MATH_ASSUME_ROUND_NEAREST_ONLY)
#define LIBC_NAMESPACE __llvm_libc_ladybird

#include <AK/MathFunctions.h>
#include <shared/math.h>

#define AK_DEFINE_UNARY_MATH_FUNCTION(function)                                               \
    double ladybird_math_##function(double x) { return LIBC_NAMESPACE::shared::function(x); } \
    float ladybird_math_##function##f(float x) { return LIBC_NAMESPACE::shared::function##f(x); }

#define AK_DEFINE_BINARY_MATH_FUNCTION(function)                                                           \
    double ladybird_math_##function(double x, double y) { return LIBC_NAMESPACE::shared::function(x, y); } \
    float ladybird_math_##function##f(float x, float y) { return LIBC_NAMESPACE::shared::function##f(x, y); }

extern "C" {

AK_ENUMERATE_UNARY_MATH_FUNCTIONS(AK_DEFINE_UNARY_MATH_FUNCTION)
AK_ENUMERATE_BINARY_MATH_FUNCTIONS(AK_DEFINE_BINARY_MATH_FUNCTION)

void ladybird_math_sincos(double x, double* sin_value, double* cos_value)
{
    LIBC_NAMESPACE::shared::sincos(x, sin_value, cos_value);
}

void ladybird_math_sincosf(float x, float* sin_value, float* cos_value)
{
    LIBC_NAMESPACE::shared::sincosf(x, sin_value, cos_value);
}
}
