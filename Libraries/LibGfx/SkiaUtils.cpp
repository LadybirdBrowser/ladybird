/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibGfx/Filter.h>
#include <LibGfx/SkiaUtils.h>
#include <core/SkBlender.h>
#include <core/SkColorFilter.h>
#include <core/SkImageFilter.h>
#include <core/SkString.h>
#include <effects/SkImageFilters.h>
#include <effects/SkRuntimeEffect.h>

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

sk_sp<SkBlender> to_skia_blender(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    switch (compositing_and_blending_operator) {
    case CompositingAndBlendingOperator::Normal:
        return SkBlender::Mode(SkBlendMode::kSrcOver);
    case CompositingAndBlendingOperator::Multiply:
        return SkBlender::Mode(SkBlendMode::kMultiply);
    case CompositingAndBlendingOperator::Screen:
        return SkBlender::Mode(SkBlendMode::kScreen);
    case CompositingAndBlendingOperator::Overlay:
        return SkBlender::Mode(SkBlendMode::kOverlay);
    case CompositingAndBlendingOperator::Darken:
        return SkBlender::Mode(SkBlendMode::kDarken);
    case CompositingAndBlendingOperator::Lighten:
        return SkBlender::Mode(SkBlendMode::kLighten);
    case CompositingAndBlendingOperator::ColorDodge:
        return SkBlender::Mode(SkBlendMode::kColorDodge);
    case CompositingAndBlendingOperator::ColorBurn:
        return SkBlender::Mode(SkBlendMode::kColorBurn);
    case CompositingAndBlendingOperator::HardLight:
        return SkBlender::Mode(SkBlendMode::kHardLight);
    case CompositingAndBlendingOperator::SoftLight:
        return SkBlender::Mode(SkBlendMode::kSoftLight);
    case CompositingAndBlendingOperator::Difference:
        return SkBlender::Mode(SkBlendMode::kDifference);
    case CompositingAndBlendingOperator::Exclusion:
        return SkBlender::Mode(SkBlendMode::kExclusion);
    case CompositingAndBlendingOperator::Hue:
        return SkBlender::Mode(SkBlendMode::kHue);
    case CompositingAndBlendingOperator::Saturation:
        return SkBlender::Mode(SkBlendMode::kSaturation);
    case CompositingAndBlendingOperator::Color:
        return SkBlender::Mode(SkBlendMode::kColor);
    case CompositingAndBlendingOperator::Luminosity:
        return SkBlender::Mode(SkBlendMode::kLuminosity);
    case CompositingAndBlendingOperator::Clear:
        return SkBlender::Mode(SkBlendMode::kClear);
    case CompositingAndBlendingOperator::Copy:
        return SkBlender::Mode(SkBlendMode::kSrc);
    case CompositingAndBlendingOperator::SourceOver:
        return SkBlender::Mode(SkBlendMode::kSrcOver);
    case CompositingAndBlendingOperator::DestinationOver:
        return SkBlender::Mode(SkBlendMode::kDstOver);
    case CompositingAndBlendingOperator::SourceIn:
        return SkBlender::Mode(SkBlendMode::kSrcIn);
    case CompositingAndBlendingOperator::DestinationIn:
        return SkBlender::Mode(SkBlendMode::kDstIn);
    case CompositingAndBlendingOperator::SourceOut:
        return SkBlender::Mode(SkBlendMode::kSrcOut);
    case CompositingAndBlendingOperator::DestinationOut:
        return SkBlender::Mode(SkBlendMode::kDstOut);
    case CompositingAndBlendingOperator::SourceATop:
        return SkBlender::Mode(SkBlendMode::kSrcATop);
    case CompositingAndBlendingOperator::DestinationATop:
        return SkBlender::Mode(SkBlendMode::kDstATop);
    case CompositingAndBlendingOperator::Xor:
        return SkBlender::Mode(SkBlendMode::kXor);
    case CompositingAndBlendingOperator::Lighter:
        return SkBlender::Mode(SkBlendMode::kPlus);
    case CompositingAndBlendingOperator::PlusDarker:
        // https://drafts.fxtf.org/compositing/#porterduffcompositingoperators_plus_darker
        // FIXME: This does not match the spec, however it looks like Safari, the only popular browser supporting this operator.
        return SkRuntimeEffect::MakeForBlender(SkString(R"(
            vec4 main(vec4 source, vec4 destination) {
                return saturate(saturate(destination.a + source.a) - saturate(destination.a - destination) - saturate(source.a - source));
            }
        )"))
            .effect->makeBlender(nullptr);
    case CompositingAndBlendingOperator::PlusLighter:
        // https://drafts.fxtf.org/compositing/#porterduffcompositingoperators_plus_lighter
        return SkRuntimeEffect::MakeForBlender(SkString(R"(
            vec4 main(vec4 source, vec4 destination) {
                return saturate(source + destination);
            }
        )"))
            .effect->makeBlender(nullptr);
    default:
        VERIFY_NOT_REACHED();
    }
}

}
