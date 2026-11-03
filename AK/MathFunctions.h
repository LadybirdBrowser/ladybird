/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// NB: The Rust ak crate compiles AK/MathFunctions.cpp without the rest of AK, so this includes nothing.

#define AK_ENUMERATE_UNARY_MATH_FUNCTIONS(X) \
    X(acos)                                  \
    X(asin)                                  \
    X(atan)                                  \
    X(cbrt)                                  \
    X(cos)                                   \
    X(cosh)                                  \
    X(exp)                                   \
    X(exp2)                                  \
    X(expm1)                                 \
    X(log)                                   \
    X(log10)                                 \
    X(log1p)                                 \
    X(log2)                                  \
    X(sin)                                   \
    X(sinh)                                  \
    X(tan)                                   \
    X(tanh)

#define AK_ENUMERATE_BINARY_MATH_FUNCTIONS(X) \
    X(atan2)                                  \
    X(hypot)                                  \
    X(pow)

#define AK_DECLARE_UNARY_MATH_FUNCTION(function) \
    double ladybird_math_##function(double x);   \
    float ladybird_math_##function##f(float x);

#define AK_DECLARE_BINARY_MATH_FUNCTION(function)        \
    double ladybird_math_##function(double x, double y); \
    float ladybird_math_##function##f(float x, float y);

extern "C" {

AK_ENUMERATE_UNARY_MATH_FUNCTIONS(AK_DECLARE_UNARY_MATH_FUNCTION)
AK_ENUMERATE_BINARY_MATH_FUNCTIONS(AK_DECLARE_BINARY_MATH_FUNCTION)

void ladybird_math_sincos(double x, double* sin_value, double* cos_value);
void ladybird_math_sincosf(float x, float* sin_value, float* cos_value);
}

#undef AK_DECLARE_BINARY_MATH_FUNCTION
#undef AK_DECLARE_UNARY_MATH_FUNCTION
