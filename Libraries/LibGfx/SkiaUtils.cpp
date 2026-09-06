/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/SkiaUtils.h>
#include <RustFFI.h>
#include <core/SkBitmap.h>
#include <core/SkBlender.h>
#include <core/SkColorFilter.h>
#include <core/SkColorSpace.h>
#include <core/SkData.h>
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

namespace {

// Reads a serialized filter graph node by node and builds the SkImageFilter tree as it goes. The
// layout is defined by the filter module of libgfx_rust, whose codec is the other reader of it.
class SkiaFilterReader {
public:
    SkiaFilterReader(ReadonlyBytes bytes, Function<DecodedImageFrame const&(u64)> const& image_frame)
        : m_bytes(bytes)
        , m_stream(bytes)
        , m_image_frame(image_frame)
    {
    }

    sk_sp<SkImageFilter> read_graph()
    {
        auto image_filter = filter();
        VERIFY(m_stream.is_eof());
        return image_filter;
    }

private:
    template<typename T>
    T value()
    {
        return MUST(m_stream.read_value<T>());
    }

    SkColor color()
    {
        return to_skia_color(Color::from_bgra(value<u32>()));
    }

    SkRect rect()
    {
        auto x = value<i32>();
        auto y = value<i32>();
        auto width = value<i32>();
        auto height = value<i32>();
        return to_skia_rect(IntRect { x, y, width, height });
    }

    SkISize size()
    {
        auto width = value<i32>();
        auto height = value<i32>();
        return SkISize::Make(width, height);
    }

    Optional<ReadonlyBytes> optional_color_table()
    {
        if (!value<bool>())
            return {};
        auto size = value<u32>();
        VERIFY(size == 256);
        auto table = m_bytes.slice(m_stream.offset(), size);
        MUST(m_stream.discard(size));
        return table;
    }

    sk_sp<SkImageFilter> optional_filter()
    {
        if (!value<bool>())
            return nullptr;
        return filter();
    }

    sk_sp<SkImageFilter> filter();

    ReadonlyBytes m_bytes;
    FixedMemoryStream m_stream;
    Function<DecodedImageFrame const&(u64)> const& m_image_frame;
};

sk_sp<SkImageFilter> SkiaFilterReader::filter()
{
    using FilterOperationType = FFI::FilterOperationType;
    switch (value<FilterOperationType>()) {
    case FilterOperationType::Arithmetic: {
        auto background = optional_filter();
        auto foreground = optional_filter();
        auto k1 = value<float>();
        auto k2 = value<float>();
        auto k3 = value<float>();
        auto k4 = value<float>();
        static constexpr bool enforce_premultiplied_color = true;
        return SkImageFilters::Arithmetic(
            SkFloatToScalar(k1),
            SkFloatToScalar(k2),
            SkFloatToScalar(k3),
            SkFloatToScalar(k4),
            enforce_premultiplied_color,
            move(background),
            move(foreground));
    }
    case FilterOperationType::Compose: {
        auto outer = filter();
        auto inner = filter();
        return SkImageFilters::Compose(outer, inner);
    }
    case FilterOperationType::Blend: {
        auto background = optional_filter();
        auto foreground = optional_filter();
        auto mode = value<CompositingAndBlendingOperator>();
        return SkImageFilters::Blend(to_skia_blender(mode), background, foreground);
    }
    case FilterOperationType::Flood: {
        auto color = this->color();
        auto opacity = value<float>();
        color = SkColorSetA(color, static_cast<u8>(opacity * 255));
        return SkImageFilters::Shader(SkShaders::Color(color));
    }
    case FilterOperationType::DisplacementMap: {
        auto color = optional_filter();
        auto displacement = optional_filter();
        auto scale = value<float>();
        auto x_channel_selector = value<FFI::ChannelSelector>();
        auto y_channel_selector = value<FFI::ChannelSelector>();
        auto convert_channel_selector = [](FFI::ChannelSelector channel_selector) {
            switch (channel_selector) {
            case FFI::ChannelSelector::Red:
                return SkColorChannel::kR;
            case FFI::ChannelSelector::Green:
                return SkColorChannel::kG;
            case FFI::ChannelSelector::Blue:
                return SkColorChannel::kB;
            case FFI::ChannelSelector::Alpha:
                return SkColorChannel::kA;
            }
            VERIFY_NOT_REACHED();
        };
        return SkImageFilters::DisplacementMap(
            convert_channel_selector(x_channel_selector),
            convert_channel_selector(y_channel_selector),
            scale,
            displacement,
            color);
    }
    case FilterOperationType::DropShadow: {
        auto offset_x = value<float>();
        auto offset_y = value<float>();
        auto radius = value<float>();
        auto color = this->color();
        auto input = optional_filter();
        return SkImageFilters::DropShadow(offset_x, offset_y, radius, radius, color, input);
    }
    case FilterOperationType::Blur: {
        auto radius_x = value<float>();
        auto radius_y = value<float>();
        auto input = optional_filter();
        return SkImageFilters::Blur(radius_x, radius_y, input);
    }
    case FilterOperationType::ColorFilter: {
        auto type = value<ColorFilterType>();
        auto amount = value<float>();
        auto input = optional_filter();
        sk_sp<SkColorFilter> color_filter;

        // Matrices are taken from https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation
        switch (type) {
        case ColorFilterType::Grayscale: {
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
        case ColorFilterType::Brightness: {
            float matrix[20] = {
                amount, 0, 0, 0, 0,
                0, amount, 0, 0, 0,
                0, 0, amount, 0, 0,
                0, 0, 0, 1, 0
            };
            color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
            break;
        }
        case ColorFilterType::Contrast: {
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
        case ColorFilterType::Invert: {
            float matrix[20] = {
                1 - 2 * amount, 0, 0, 0, amount,
                0, 1 - 2 * amount, 0, 0, amount,
                0, 0, 1 - 2 * amount, 0, amount,
                0, 0, 0, 1, 0
            };
            color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
            break;
        }
        case ColorFilterType::Opacity: {
            float matrix[20] = {
                1, 0, 0, 0, 0,
                0, 1, 0, 0, 0,
                0, 0, 1, 0, 0,
                0, 0, 0, amount, 0
            };
            color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kYes);
            break;
        }
        case ColorFilterType::Sepia: {
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
        case ColorFilterType::Saturate: {
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
    }
    case FilterOperationType::ColorMatrix: {
        float matrix[20];
        for (auto& entry : matrix)
            entry = value<float>();
        auto input = optional_filter();
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix), input);
    }
    case FilterOperationType::ColorTable: {
        auto a = optional_color_table();
        auto r = optional_color_table();
        auto g = optional_color_table();
        auto b = optional_color_table();
        auto input = optional_filter();
        auto* a_table = a.has_value() ? a->data() : nullptr;
        auto* r_table = r.has_value() ? r->data() : nullptr;
        auto* g_table = g.has_value() ? g->data() : nullptr;
        auto* b_table = b.has_value() ? b->data() : nullptr;

        // NB: The color space in which the table is applied is determined by the color-interpolation-filters
        //     property and handled by the filter graph, so the table is applied directly here.
        return SkImageFilters::ColorFilter(SkColorFilters::TableARGB(a_table, r_table, g_table, b_table), input);
    }
    case FilterOperationType::Saturate: {
        auto saturation = value<float>();
        auto input = optional_filter();
        SkColorMatrix matrix;
        matrix.setSaturation(saturation);
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix), input);
    }
    case FilterOperationType::HueRotate: {
        auto angle_degrees = value<float>();
        auto input = optional_filter();
        float radians = AK::to_radians(angle_degrees);
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
    }
    case FilterOperationType::Image: {
        auto const& frame = m_image_frame(value<u64>());
        auto src_rect = rect();
        auto dest_rect = rect();
        auto sampling_options = to_skia_sampling_options(value<ScalingMode>());
        auto image = sk_image_from_bitmap(frame.bitmap(), frame.color_space());
        return SkImageFilters::Image(move(image), src_rect, dest_rect, sampling_options);
    }
    case FilterOperationType::Merge: {
        auto count = value<u32>();
        Vector<sk_sp<SkImageFilter>> filters;
        filters.ensure_capacity(count);
        for (u32 i = 0; i < count; ++i)
            filters.unchecked_append(optional_filter());
        return SkImageFilters::Merge(filters.data(), filters.size());
    }
    case FilterOperationType::Offset: {
        auto dx = value<float>();
        auto dy = value<float>();
        auto input = optional_filter();
        return SkImageFilters::Offset(dx, dy, input);
    }
    case FilterOperationType::Erode: {
        auto radius_x = value<float>();
        auto radius_y = value<float>();
        auto input = optional_filter();
        return SkImageFilters::Erode(radius_x, radius_y, input);
    }
    case FilterOperationType::Dilate: {
        auto radius_x = value<float>();
        auto radius_y = value<float>();
        auto input = optional_filter();
        return SkImageFilters::Dilate(radius_x, radius_y, input);
    }
    case FilterOperationType::Turbulence: {
        auto turbulence_type = value<FFI::TurbulenceType>();
        auto base_frequency_x = value<float>();
        auto base_frequency_y = value<float>();
        auto num_octaves = value<i32>();
        auto seed = value<float>();
        auto tile_stitch_size = size();
        sk_sp<SkShader> turbulence_shader = [&] {
            switch (turbulence_type) {
            case FFI::TurbulenceType::Turbulence:
                return SkShaders::MakeTurbulence(base_frequency_x, base_frequency_y, num_octaves, seed, &tile_stitch_size);
            case FFI::TurbulenceType::FractalNoise:
                return SkShaders::MakeFractalNoise(base_frequency_x, base_frequency_y, num_octaves, seed, &tile_stitch_size);
            }
            VERIFY_NOT_REACHED();
        }();
        return SkImageFilters::Shader(move(turbulence_shader));
    }
    case FilterOperationType::ColorSpaceConversion: {
        auto source_color_space = value<InterpolationColorSpace>();
        auto destination_color_space = value<InterpolationColorSpace>();
        auto input = optional_filter();
        if (source_color_space == destination_color_space)
            return input;

        sk_sp<SkColorFilter> color_space_filter;
        switch (destination_color_space) {
        case InterpolationColorSpace::LinearRGB:
            color_space_filter = SkColorFilters::SRGBToLinearGamma();
            break;
        case InterpolationColorSpace::SRGB:
            color_space_filter = SkColorFilters::LinearToSRGBGamma();
            break;
        }
        return SkImageFilters::ColorFilter(move(color_space_filter), move(input));
    }
    }
    VERIFY_NOT_REACHED();
}

}

sk_sp<SkImageFilter> to_skia_image_filter(ReadonlyBytes serialized_filter, Function<DecodedImageFrame const&(u64)> const& image_frame)
{
    return SkiaFilterReader { serialized_filter, image_frame }.read_graph();
}

sk_sp<SkImageFilter> to_skia_image_filter(Gfx::Filter const& filter)
{
    return to_skia_image_filter(filter.serialized_bytes(), [](u64) -> DecodedImageFrame const& {
        // A graph carried as a Gfx::Filter comes from CSS filter functions, which draw no images.
        VERIFY_NOT_REACHED();
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

sk_sp<SkImage> sk_image_adopting_bitmap(NonnullRefPtr<Bitmap> bitmap, ColorSpace const& color_space)
{
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->format(), bitmap->alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    auto row_bytes = bitmap->pitch();
    auto byte_count = bitmap->size_in_bytes();
    auto* pixels = bitmap->scanline_u8(0);
    auto data = SkData::MakeWithProc(pixels, byte_count, [](void const*, void* context) { static_cast<Bitmap*>(context)->unref(); }, &bitmap.leak_ref());
    return SkImages::RasterFromData(info, move(data), row_bytes);
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
