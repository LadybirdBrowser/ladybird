/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColorFilter.h>
#include <core/SkImage.h>
#include <core/SkImageInfo.h>
#include <core/SkPaint.h>
#include <core/SkSamplingOptions.h>

// The lightness lift pushes saturated colors out of sRGB, so the Oklab round trip hands the transfer function negative
// linear components, which the shader has to clamp before pow() sees them. This pins the contract every backend has to
// meet: finite, in-range color for every input. The float target matters because an 8-bit one squashes a NaN to 0 or
// 255, hiding exactly what's being checked.
static void draw_through_force_dark_filter(SkColor color, float (&rgba)[4])
{
    SkBitmap source;
    source.allocN32Pixels(1, 1);
    source.eraseColor(color);
    source.setImmutable();

    SkBitmap target;
    target.allocPixels(SkImageInfo::Make(1, 1, kRGBA_F32_SkColorType, kPremul_SkAlphaType));
    SkCanvas canvas(target);
    SkPaint paint;
    paint.setColorFilter(Web::Painting::force_dark_image_color_filter());
    canvas.drawImage(source.asImage(), 0, 0, SkSamplingOptions {}, &paint);

    auto const* pixel = static_cast<float const*>(target.getAddr(0, 0));
    for (int i = 0; i < 4; ++i)
        rgba[i] = pixel[i];
}

TEST_CASE(force_dark_image_filter_keeps_out_of_gamut_colors_finite)
{
    for (auto color : { SK_ColorRED, SK_ColorGREEN, SK_ColorBLUE, SK_ColorYELLOW, SK_ColorCYAN, SK_ColorMAGENTA }) {
        float rgba[4];
        draw_through_force_dark_filter(color, rgba);
        for (auto channel : rgba) {
            EXPECT(!__builtin_isnan(channel));
            EXPECT(channel >= 0.0f && channel <= 1.0f);
        }
    }
}

// Pure red is the worked example: after the lift its linear green and blue come out negative, and the clamp pins both
// to exactly zero, while red stays a bright red and alpha rides through untouched.
TEST_CASE(force_dark_image_filter_clamps_pure_red_to_the_gamut_edge)
{
    float rgba[4];
    draw_through_force_dark_filter(SK_ColorRED, rgba);
    EXPECT(rgba[0] > 0.5f);
    EXPECT_EQ(rgba[1], 0.0f);
    EXPECT_EQ(rgba[2], 0.0f);
    EXPECT_EQ(rgba[3], 1.0f);
}
