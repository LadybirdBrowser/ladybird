/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Math.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/FilterImpl.h>
#include <LibGfx/SkiaUtils.h>
#include <core/SkBitmap.h>
#include <core/SkBlender.h>
#include <core/SkColorFilter.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkImageFilter.h>
#include <core/SkString.h>
#include <effects/SkColorMatrix.h>
#include <effects/SkImageFilters.h>
#include <effects/SkPerlinNoiseShader.h>
#include <effects/SkRuntimeEffect.h>

namespace Gfx {

SkPath to_skia_path(Path const& path)
{
    return static_cast<PathImplSkia const&>(path.impl()).sk_path();
}

sk_sp<SkImageFilter> to_skia_image_filter(Gfx::Filter const& filter)
{
    auto to_optional_skia_image_filter = [](Optional<Gfx::Filter> const& input) -> sk_sp<SkImageFilter> {
        if (!input.has_value())
            return nullptr;
        return to_skia_image_filter(input.value());
    };

    return filter.impl().operation.visit(
        [&](FilterImpl::Arithmetic const& op) -> sk_sp<SkImageFilter> {
            auto background = to_optional_skia_image_filter(op.background);
            auto foreground = to_optional_skia_image_filter(op.foreground);
            return SkImageFilters::Arithmetic(
                SkFloatToScalar(op.k1),
                SkFloatToScalar(op.k2),
                SkFloatToScalar(op.k3),
                SkFloatToScalar(op.k4),
                false,
                move(background),
                move(foreground));
        },
        [&](FilterImpl::Compose const& op) -> sk_sp<SkImageFilter> {
            auto outer = to_skia_image_filter(op.outer);
            auto inner = to_skia_image_filter(op.inner);
            return SkImageFilters::Compose(outer, inner);
        },
        [&](FilterImpl::Blend const& op) -> sk_sp<SkImageFilter> {
            auto background = to_optional_skia_image_filter(op.background);
            auto foreground = to_optional_skia_image_filter(op.foreground);
            return SkImageFilters::Blend(to_skia_blender(op.mode), background, foreground);
        },
        [&](FilterImpl::Flood const& op) -> sk_sp<SkImageFilter> {
            auto color = to_skia_color(op.color);
            color = SkColorSetA(color, static_cast<u8>(op.opacity * 255));
            return SkImageFilters::Shader(SkShaders::Color(color));
        },
        [&](FilterImpl::DisplacementMap const& op) -> sk_sp<SkImageFilter> {
            auto color = to_optional_skia_image_filter(op.color);
            auto displacement = to_optional_skia_image_filter(op.displacement);
            auto convert_channel_selector = [](ChannelSelector channel_selector) {
                switch (channel_selector) {
                case ChannelSelector::Red:
                    return SkColorChannel::kR;
                case ChannelSelector::Green:
                    return SkColorChannel::kG;
                case ChannelSelector::Blue:
                    return SkColorChannel::kB;
                case ChannelSelector::Alpha:
                    return SkColorChannel::kA;
                }
                VERIFY_NOT_REACHED();
            };
            return SkImageFilters::DisplacementMap(
                convert_channel_selector(op.x_channel_selector),
                convert_channel_selector(op.y_channel_selector),
                op.scale,
                displacement,
                color);
        },
        [&](FilterImpl::DropShadow const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            return SkImageFilters::DropShadow(op.offset_x, op.offset_y, op.radius, op.radius, to_skia_color(op.color), input);
        },
        [&](FilterImpl::Blur const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            return SkImageFilters::Blur(op.radius_x, op.radius_y, input);
        },
        [&](FilterImpl::ColorFilter const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            sk_sp<SkColorFilter> color_filter;

            // Matrices are taken from https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation
            switch (op.type) {
            case ColorFilterType::Grayscale: {
                auto amount = op.amount;
                float matrix[20] = {
                    0.2126f + 0.7874f * (1 - amount), 0.7152f - 0.7152f * (1 - amount),
                    0.0722f - 0.0722f * (1 - amount), 0, 0,
                    0.2126f - 0.2126f * (1 - amount), 0.7152f + 0.2848f * (1 - amount),
                    0.0722f - 0.0722f * (1 - amount), 0, 0,
                    0.2126f - 0.2126f * (1 - amount), 0.7152f - 0.7152f * (1 - amount),
                    0.0722f + 0.9278f * (1 - amount), 0, 0,
                    0, 0, 0, 1, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilterType::Brightness: {
                auto amount = op.amount;
                float matrix[20] = {
                    amount, 0, 0, 0, 0,
                    0, amount, 0, 0, 0,
                    0, 0, amount, 0, 0,
                    0, 0, 0, 1, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
                break;
            }
            case Gfx::ColorFilterType::Contrast: {
                auto amount = op.amount;
                float intercept = -(0.5f * amount) + 0.5f;
                float matrix[20] = {
                    amount, 0, 0, 0, intercept,
                    0, amount, 0, 0, intercept,
                    0, 0, amount, 0, intercept,
                    0, 0, 0, 1, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
                break;
            }
            case Gfx::ColorFilterType::Invert: {
                auto amount = op.amount;
                float matrix[20] = {
                    1 - 2 * amount, 0, 0, 0, amount,
                    0, 1 - 2 * amount, 0, 0, amount,
                    0, 0, 1 - 2 * amount, 0, amount,
                    0, 0, 0, 1, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilterType::Opacity: {
                auto amount = op.amount;
                float matrix[20] = {
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, amount, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilterType::Sepia: {
                auto amount = op.amount;
                float matrix[20] = {
                    0.393f + 0.607f * (1 - amount), 0.769f - 0.769f * (1 - amount), 0.189f - 0.189f * (1 - amount), 0,
                    0,
                    0.349f - 0.349f * (1 - amount), 0.686f + 0.314f * (1 - amount), 0.168f - 0.168f * (1 - amount), 0,
                    0,
                    0.272f - 0.272f * (1 - amount), 0.534f - 0.534f * (1 - amount), 0.131f + 0.869f * (1 - amount), 0,
                    0,
                    0, 0, 0, 1, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
                break;
            }
            case Gfx::ColorFilterType::Saturate: {
                auto amount = op.amount;
                float matrix[20] = {
                    0.213f + 0.787f * amount, 0.715f - 0.715f * amount, 0.072f - 0.072f * amount, 0, 0,
                    0.213f - 0.213f * amount, 0.715f + 0.285f * amount, 0.072f - 0.072f * amount, 0, 0,
                    0.213f - 0.213f * amount, 0.715f - 0.715f * amount, 0.072f + 0.928f * amount, 0, 0,
                    0, 0, 0, 1, 0
                };
                color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
                break;
            }
            default:
                VERIFY_NOT_REACHED();
            }

            return SkImageFilters::ColorFilter(color_filter, input);
        },
        [&](FilterImpl::ColorMatrix const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            return SkImageFilters::ColorFilter(SkColorFilters::Matrix(op.matrix.data()), input);
        },
        [&](FilterImpl::ColorTable const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            auto* a_table = op.a.has_value() ? op.a->data() : nullptr;
            auto* r_table = op.r.has_value() ? op.r->data() : nullptr;
            auto* g_table = op.g.has_value() ? op.g->data() : nullptr;
            auto* b_table = op.b.has_value() ? op.b->data() : nullptr;

            // Color tables are applied in linear space by default, so we need to convert twice.
            // FIXME: support sRGB space as well (i.e. don't perform these conversions).
            auto srgb_to_linear = SkImageFilters::ColorFilter(SkColorFilters::SRGBToLinearGamma(), input);
            auto color_table = SkImageFilters::ColorFilter(SkColorFilters::TableARGB(a_table, r_table, g_table, b_table), srgb_to_linear);
            return SkImageFilters::ColorFilter(SkColorFilters::LinearToSRGBGamma(), color_table);
        },
        [&](FilterImpl::Saturate const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            SkColorMatrix matrix;
            matrix.setSaturation(op.value);
            return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix), input);
        },
        [&](FilterImpl::HueRotate const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            float radians = AK::to_radians(op.angle_degrees);
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
            return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo), input);
        },
        [&](FilterImpl::Image const& op) -> sk_sp<SkImageFilter> {
            auto skia_src_rect = to_skia_rect(op.src_rect);
            auto skia_dest_rect = to_skia_rect(op.dest_rect);
            auto sampling_options = to_skia_sampling_options(op.scaling_mode);
            auto image = sk_image_from_bitmap(op.frame.bitmap(), op.frame.color_space());
            return SkImageFilters::Image(move(image), skia_src_rect, skia_dest_rect, sampling_options);
        },
        [&](FilterImpl::Merge const& op) -> sk_sp<SkImageFilter> {
            Vector<sk_sp<SkImageFilter>> filters;
            filters.ensure_capacity(op.inputs.size());
            for (auto const& input : op.inputs)
                filters.unchecked_append(to_optional_skia_image_filter(input));
            return SkImageFilters::Merge(filters.data(), filters.size());
        },
        [&](FilterImpl::Offset const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            return SkImageFilters::Offset(op.dx, op.dy, input);
        },
        [&](FilterImpl::Erode const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            return SkImageFilters::Erode(op.radius_x, op.radius_y, input);
        },
        [&](FilterImpl::Dilate const& op) -> sk_sp<SkImageFilter> {
            auto input = to_optional_skia_image_filter(op.input);
            return SkImageFilters::Dilate(op.radius_x, op.radius_y, input);
        },
        [&](FilterImpl::Turbulence const& op) -> sk_sp<SkImageFilter> {
            sk_sp<SkShader> turbulence_shader = [&] {
                auto skia_size = SkISize::Make(op.tile_stitch_size.width(), op.tile_stitch_size.height());
                switch (op.turbulence_type) {
                case TurbulenceType::Turbulence:
                    return SkShaders::MakeTurbulence(op.base_frequency_x, op.base_frequency_y, op.num_octaves, op.seed, &skia_size);
                case TurbulenceType::FractalNoise:
                    return SkShaders::MakeFractalNoise(op.base_frequency_x, op.base_frequency_y, op.num_octaves, op.seed, &skia_size);
                }
                VERIFY_NOT_REACHED();
            }();
            return SkImageFilters::Shader(move(turbulence_shader));
        });
}

sk_sp<SkImage> sk_image_from_bitmap(Bitmap const& bitmap, ColorSpace const& color_space)
{
    auto info = SkImageInfo::Make(bitmap.width(), bitmap.height(), to_skia_color_type(bitmap.format()), to_skia_alpha_type(bitmap.format(), bitmap.alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    SkBitmap sk_bitmap;
    sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap.scanline(0))), bitmap.pitch());
    sk_bitmap.setImmutable();
    return sk_bitmap.asImage();
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
