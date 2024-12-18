/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibGfx/Filter.h>
#include <LibGfx/SkiaUtils.h>
#include <core/SkColorFilter.h>
#include <core/SkImageFilter.h>
#include <effects/SkImageFilters.h>

namespace Gfx {

SkPath to_skia_path(Path const& path)
{
    return static_cast<PathImplSkia const&>(path.impl()).sk_path();
}

sk_sp<SkImageFilter> to_skia_image_filter(Gfx::Filter const& filter)
{
    // See: https://drafts.fxtf.org/filter-effects-1/#supported-filter-functions
    return filter.visit(
        [&](Gfx::BlurFilter blur_filter) {
            return SkImageFilters::Blur(blur_filter.radius, blur_filter.radius, nullptr);
        },
        [&](Gfx::ColorFilter color_filter) {
            sk_sp<SkColorFilter> skia_color_filter;
            float amount = color_filter.amount;

            // Matrices are taken from https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation
            switch (color_filter.type) {
            case ColorFilter::Type::Grayscale: {
                float matrix[20] = {
                    0.2126f + 0.7874f * (1 - amount), 0.7152f - 0.7152f * (1 - amount), 0.0722f - 0.0722f * (1 - amount), 0, 0,
                    0.2126f - 0.2126f * (1 - amount), 0.7152f + 0.2848f * (1 - amount), 0.0722f - 0.0722f * (1 - amount), 0, 0,
                    0.2126f - 0.2126f * (1 - amount), 0.7152f - 0.7152f * (1 - amount), 0.0722f + 0.9278f * (1 - amount), 0, 0,
                    0, 0, 0, 1, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilter::Type::Brightness: {
                float matrix[20] = {
                    amount, 0, 0, 0, 0,
                    0, amount, 0, 0, 0,
                    0, 0, amount, 0, 0,
                    0, 0, 0, 1, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
                break;
            }
            case Gfx::ColorFilter::Type::Contrast: {
                float intercept = -(0.5f * amount) + 0.5f;
                float matrix[20] = {
                    amount, 0, 0, 0, intercept,
                    0, amount, 0, 0, intercept,
                    0, 0, amount, 0, intercept,
                    0, 0, 0, 1, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
                break;
            }
            case Gfx::ColorFilter::Type::Invert: {
                float matrix[20] = {
                    1 - 2 * amount, 0, 0, 0, amount,
                    0, 1 - 2 * amount, 0, 0, amount,
                    0, 0, 1 - 2 * amount, 0, amount,
                    0, 0, 0, 1, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilter::Type::Opacity: {
                float matrix[20] = {
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, amount, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilter::Type::Sepia: {
                float matrix[20] = {
                    0.393f + 0.607f * (1 - amount), 0.769f - 0.769f * (1 - amount), 0.189f - 0.189f * (1 - amount), 0, 0,
                    0.349f - 0.349f * (1 - amount), 0.686f + 0.314f * (1 - amount), 0.168f - 0.168f * (1 - amount), 0, 0,
                    0.272f - 0.272f * (1 - amount), 0.534f - 0.534f * (1 - amount), 0.131f + 0.869f * (1 - amount), 0, 0,
                    0, 0, 0, 1, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilter::Type::Saturate: {
                float matrix[20] = {
                    0.213f + 0.787f * amount, 0.715f - 0.715f * amount, 0.072f - 0.072f * amount, 0, 0,
                    0.213f - 0.213f * amount, 0.715f + 0.285f * amount, 0.072f - 0.072f * amount, 0, 0,
                    0.213f - 0.213f * amount, 0.715f - 0.715f * amount, 0.072f + 0.928f * amount, 0, 0,
                    0, 0, 0, 1, 0
                };
                skia_color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
                break;
            }
            default:
                VERIFY_NOT_REACHED();
            }

            return SkImageFilters::ColorFilter(skia_color_filter, nullptr);
        },
        [&](Gfx::HueRotateFilter hue_rotate_filter) {
            float radians = AK::to_radians(hue_rotate_filter.angle_degrees);

            auto cosA = cos(radians);
            auto sinA = sin(radians);

            auto a00 = 0.213f + cosA * 0.787f - sinA * 0.213f;
            auto a01 = 0.715f - cosA * 0.715f - sinA * 0.715f;
            auto a02 = 0.072f - cosA * 0.072f + sinA * 0.928f;
            auto a10 = 0.213f - cosA * 0.213f + sinA * 0.143f;
            auto a11 = 0.715f + cosA * 0.285f + sinA * 0.140f;
            auto a12 = 0.072f - cosA * 0.072f - sinA * 0.283f;
            auto a20 = 0.213f - cosA * 0.213f - sinA * 0.787f;
            auto a21 = 0.715f - cosA * 0.715f + sinA * 0.715f;
            auto a22 = 0.072f + cosA * 0.928f + sinA * 0.072f;

            float matrix[20] = {
                a00, a01, a02, 0, 0,
                a10, a11, a12, 0, 0,
                a20, a21, a22, 0, 0,
                0, 0, 0, 1, 0
            };

            auto filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
            return SkImageFilters::ColorFilter(filter, nullptr);
        },
        [&](Gfx::DropShadowFilter drop_shadow_filter) {
            auto shadow_color = to_skia_color(drop_shadow_filter.color);
            return SkImageFilters::DropShadow(drop_shadow_filter.offset_x, drop_shadow_filter.offset_y, drop_shadow_filter.radius, drop_shadow_filter.radius, shadow_color, nullptr);
        });
}

}
