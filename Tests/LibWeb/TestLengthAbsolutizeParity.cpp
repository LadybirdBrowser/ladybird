/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/CSS/StyleComputeFFI.h>

// The Rust style computation core resolves lengths independently of the C++
// implementation in Length. This compares both across every supported unit so
// they cannot drift apart.

namespace Web::CSS {

static Length::ResolutionContext make_context(bool* did_resolve_viewport_relative_length)
{
    Gfx::FontPixelMetrics const pixel_metrics { .x_height = 8, .advance_of_ascii_zero = 7.5, .ascent = 11, .descent = 4 };
    Gfx::FontPixelMetrics const root_pixel_metrics { .x_height = 10, .advance_of_ascii_zero = 9, .ascent = 14, .descent = 5 };
    Length::ResolutionContext context {
        .viewport_rect = { 0, 0, 800, 600 },
        .font_metrics = { CSSPixels(16), pixel_metrics, CSSPixels(19) },
        .root_font_metrics = { CSSPixels(20), root_pixel_metrics, CSSPixels(24) },
        .font_metrics_depend_on_viewport_metrics = false,
        .root_font_metrics_depend_on_viewport_metrics = true,
    };
    context.set_did_resolve_viewport_relative_length(*did_resolve_viewport_relative_length);
    return context;
}

TEST_CASE(rust_length_resolution_matches_cpp)
{
    static constexpr Array units = {
        LengthUnit::Px, LengthUnit::Cm, LengthUnit::Mm, LengthUnit::Q, LengthUnit::In,
        LengthUnit::Pt, LengthUnit::Pc, LengthUnit::Em, LengthUnit::Rem, LengthUnit::Ex,
        LengthUnit::Rex, LengthUnit::Cap, LengthUnit::Rcap, LengthUnit::Ch, LengthUnit::Rch,
        LengthUnit::Ic, LengthUnit::Ric, LengthUnit::Lh, LengthUnit::Rlh, LengthUnit::Vw,
        LengthUnit::Vh, LengthUnit::Vi, LengthUnit::Vb, LengthUnit::Vmin, LengthUnit::Vmax,
        LengthUnit::Svw, LengthUnit::Svh, LengthUnit::Lvw, LengthUnit::Lvh, LengthUnit::Dvw,
        LengthUnit::Dvh, LengthUnit::Svmin, LengthUnit::Lvmax, LengthUnit::Dvmin, LengthUnit::Dvmax
    };
    static constexpr Array values = { -2.5, 0.0, 0.125, 1.0, 33.3333, 1000.0 };

    for (auto unit : units) {
        for (auto value : values) {
            bool cpp_resolved_viewport = false;
            auto cpp_context = make_context(&cpp_resolved_viewport);
            auto ffi_context = to_ffi_length_resolution_context(cpp_context);

            auto rust_result = invoke_rust_absolutize_length(value, to_underlying(unit), &ffi_context);
            EXPECT(rust_result.handled);

            Length length { value, unit };
            auto cpp_px = length.to_px_without_rounding(cpp_context);

            EXPECT_EQ(rust_result.px, cpp_px);
            EXPECT_EQ(rust_result.resolved_viewport_relative_length, cpp_resolved_viewport);
            EXPECT_EQ(rust_result.changed, unit != LengthUnit::Px);
        }
    }
}

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
            EXPECT_EQ(rust_css_pixels_multiply(left, right), cpp_product.raw_value());

            if (right != 0) {
                CSSPixels cpp_quotient = CSSPixelFraction(CSSPixels::from_raw(left), CSSPixels::from_raw(right));
                EXPECT_EQ(rust_css_pixels_divide_as_fraction(left, right), cpp_quotient.raw_value());
            }
        }
        auto value = CSSPixels::from_raw(left).to_double();
        for (auto factor : { 0.71, 1.0 / 0.71, 1.5, -2.25, 0.015625, 1000000.0 }) {
            EXPECT_EQ(rust_css_pixels_nearest_value_for(value * factor), CSSPixels::nearest_value_for(value * factor).raw_value());
            EXPECT_EQ(rust_css_pixels_scaled(left, factor), CSSPixels::from_raw(left).scaled(factor).raw_value());
        }
    }
}

TEST_CASE(container_relative_units_fall_back_to_cpp)
{
    bool unused = false;
    auto cpp_context = make_context(&unused);
    auto ffi_context = to_ffi_length_resolution_context(cpp_context);
    auto result = invoke_rust_absolutize_length(1.0, to_underlying(LengthUnit::Cqw), &ffi_context);
    EXPECT(!result.handled);
}

}
