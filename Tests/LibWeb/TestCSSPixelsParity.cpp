/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCompositing/RustFFI.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web {

TEST_CASE(css_pixels_arithmetic_matches_cpp)
{
    // Deterministic pseudo-random raw values plus rounding edge cases.
    Vector<i32> raw_values { 0, 1, -1, 31, 32, 33, 63, 64, 65, -32, -64, 1024, -1024, 65535, -65535, 1 << 20 };
    u32 state = 0x12345678;
    for (int i = 0; i < 64; ++i) {
        state = state * 1664525u + 1013904223u;
        raw_values.append(static_cast<i32>(state) >> 8);
    }

    for (auto left : raw_values) {
        for (auto right : raw_values) {
            auto cpp_product = CSSPixels::from_raw(left) * CSSPixels::from_raw(right);
            EXPECT_EQ(Compositing::RustFFI::rust_css_pixels_multiply(left, right), cpp_product.raw_value());

            if (right != 0) {
                CSSPixels cpp_quotient = CSSPixelFraction(CSSPixels::from_raw(left), CSSPixels::from_raw(right));
                EXPECT_EQ(Compositing::RustFFI::rust_css_pixels_divide_as_fraction(left, right), cpp_quotient.raw_value());
            }
        }
        auto value = CSSPixels::from_raw(left).to_double();
        for (auto factor : { 0.71, 1.0 / 0.71, 1.5, -2.25, 0.015625, 1000000.0 }) {
            EXPECT_EQ(Compositing::RustFFI::rust_css_pixels_nearest_value_for(value * factor), CSSPixels::nearest_value_for(value * factor).raw_value());
            EXPECT_EQ(Compositing::RustFFI::rust_css_pixels_scaled(left, factor), CSSPixels::from_raw(left).scaled(factor).raw_value());
        }
    }
}

}
