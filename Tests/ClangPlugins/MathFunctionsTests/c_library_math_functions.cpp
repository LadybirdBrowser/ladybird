/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/Math.h>
#include <cmath>
#include <math.h>

double c_library(double x, float y)
{
    // expected-error@+1 {{'sin' is computed differently by the C library of each platform. Use AK::sin from AK/Math.h instead}}
    auto a = sin(x);
    // expected-error@+1 {{'cbrtf' is computed differently by the C library of each platform. Use AK::cbrt from AK/Math.h instead}}
    auto b = cbrtf(y);
    // expected-error@+1 {{'pow' is computed differently by the C library of each platform. Use AK::pow from AK/Math.h instead}}
    auto c = std::pow(x, x);
    // expected-error@+1 {{'hypot' is computed differently by the C library of each platform. Use AK::hypot from AK/Math.h instead}}
    auto d = std::hypot(y, y);
    // expected-error@+1 {{'__builtin_atan2' is computed differently by the C library of each platform. Use AK::atan2 from AK/Math.h instead}}
    auto e = __builtin_atan2(x, x);
    // expected-error@+1 {{'acosh' is computed differently by the C library of each platform. Use AK::acosh from AK/Math.h instead}}
    double (*f)(double) = acosh;
    return a + b + c + d + e + f(x);
}

static double log(char const*)
{
    return 0;
}

double own_function_with_a_c_library_name()
{
    return log("message");
}

double ak(double x, float y)
{
    float sin_value;
    float cos_value;
    AK::sincos(y, sin_value, cos_value);
    return AK::sin(x) + AK::pow(y, y) + AK::hypot(x, x) + AK::exp(x) + sin_value + cos_value
        + AK::sqrt(x) + std::sqrt(x) + fabs(x);
}
