/*
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/SIMD.h>
#include <AK/SIMDMath.h>

// See the comment in <AK/SIMDMath.h>
#pragma GCC diagnostic ignored "-Wpsabi"

TEST_CASE(exp)
{
    AK::SIMD::f32x4 v = { .2f, .4f, .6f, .8f };
    auto result = AK::SIMD::exp(v);

    EXPECT_APPROXIMATE(result[0], 1.22140276f);
    EXPECT_APPROXIMATE(result[1], 1.49182470f);
    EXPECT_APPROXIMATE(result[2], 1.82211880f);
    EXPECT_APPROXIMATE(result[3], 2.22554093f);
}

TEST_CASE(exp_approximate)
{
    AK::SIMD::f32x4 v = { .2f, .4f, .6f, .8f };
    auto result = AK::SIMD::exp_approximate(v);
    constexpr float accuracy = .001f;

    EXPECT(fabsf(result[0] - 1.22140276f) <= accuracy);
    EXPECT(fabsf(result[1] - 1.49182470f) <= accuracy);
    EXPECT(fabsf(result[2] - 1.82211880f) <= accuracy);
    EXPECT(fabsf(result[3] - 2.22554093f) <= accuracy);
}
