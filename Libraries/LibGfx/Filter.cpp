/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filter.h>
#include <LibGfx/FilterImpl.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaUtils.h>
#include <core/SkBlendMode.h>
#include <core/SkColorFilter.h>
#include <core/SkScalar.h>
#include <effects/SkColorMatrix.h>
#include <effects/SkImageFilters.h>
#include <effects/SkPerlinNoiseShader.h>

namespace Gfx {

using Impl = FilterImpl;

Filter::Filter(Filter const& other)
    : m_impl(other.m_impl->clone())
{
}

Filter& Filter::operator=(Filter const& other)
{
    if (this != &other) {
        m_impl = other.m_impl->clone();
    }
    return *this;
}

Filter::~Filter() = default;

Filter::Filter(NonnullOwnPtr<FilterImpl>&& impl)
    : m_impl(impl->clone())
{
}

FilterImpl const& Filter::impl() const
{
    return *m_impl;
}

Filter Filter::arithmetic(Optional<Filter const&> background, Optional<Filter const&> foreground, float k1, float k2, float k3, float k4)
{
    sk_sp<SkImageFilter> background_skia = background.has_value() ? background->m_impl->filter : nullptr;
    sk_sp<SkImageFilter> foreground_skia = foreground.has_value() ? foreground->m_impl->filter : nullptr;

    auto filter = SkImageFilters::Arithmetic(
        SkFloatToScalar(k1), SkFloatToScalar(k2), SkFloatToScalar(k3), SkFloatToScalar(k4), false, move(background_skia), move(foreground_skia));
    return Filter(Impl::create(filter));
}

Filter Filter::compose(Filter const& outer, Filter const& inner)
{
    auto inner_skia = inner.m_impl->filter;
    auto outer_skia = outer.m_impl->filter;

    auto filter = SkImageFilters::Compose(outer_skia, inner_skia);
    return Filter(Impl::create(filter));
}

Filter Filter::blend(Optional<Filter const&> background, Optional<Filter const&> foreground, Gfx::CompositingAndBlendingOperator mode)
{
    sk_sp<SkImageFilter> background_skia = background.has_value() ? background->m_impl->filter : nullptr;
    sk_sp<SkImageFilter> foreground_skia = foreground.has_value() ? foreground->m_impl->filter : nullptr;

    auto filter = SkImageFilters::Blend(to_skia_blender(mode), background_skia, foreground_skia);
    return Filter(Impl::create(filter));
}

Filter Filter::blur(float radius_x, float radius_y, Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

    auto filter = SkImageFilters::Blur(radius_x, radius_y, input_skia);
    return Filter(Impl::create(filter));
}

Filter Filter::flood(Gfx::Color color, float opacity)
{
    auto color_skia = to_skia_color(color);
    color_skia = SkColorSetA(color_skia, static_cast<u8>(opacity * 255));

    return Filter(Impl::create(SkImageFilters::Shader(SkShaders::Color(color_skia))));
}

Filter Filter::drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color,
    Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;
    auto shadow_color = to_skia_color(color);

    auto filter = SkImageFilters::DropShadow(offset_x, offset_y, radius, radius, shadow_color, input_skia);
    return Filter(Impl::create(filter));
}

Filter Filter::color(ColorFilterType type, float amount, Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

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
    case Gfx::ColorFilterType::Brightness: {
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

    return Filter(Impl::create(SkImageFilters::ColorFilter(color_filter, input_skia)));
}

Filter Filter::color_matrix(float matrix[20], Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

    return Filter(Impl::create(SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix), input_skia)));
}

Filter Filter::color_table(Optional<ReadonlyBytes> a, Optional<ReadonlyBytes> r, Optional<ReadonlyBytes> g,
    Optional<ReadonlyBytes> b, Optional<Filter const&> input)
{
    VERIFY(!a.has_value() || a->size() == 256);
    VERIFY(!r.has_value() || r->size() == 256);
    VERIFY(!g.has_value() || g->size() == 256);
    VERIFY(!b.has_value() || b->size() == 256);

    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

    auto* a_table = a.has_value() ? a->data() : nullptr;
    auto* r_table = r.has_value() ? r->data() : nullptr;
    auto* g_table = g.has_value() ? g->data() : nullptr;
    auto* b_table = b.has_value() ? b->data() : nullptr;

    // Color tables are applied in linear space by default, so we need to convert twice.
    // FIXME: support sRGB space as well (i.e. don't perform these conversions).
    auto srgb_to_linear = SkImageFilters::ColorFilter(SkColorFilters::SRGBToLinearGamma(), input_skia);
    auto color_table = SkImageFilters::ColorFilter(SkColorFilters::TableARGB(a_table, r_table, g_table, b_table), srgb_to_linear);
    auto linear_to_srgb = SkImageFilters::ColorFilter(SkColorFilters::LinearToSRGBGamma(), color_table);
    return Filter(Impl::create(linear_to_srgb));
}

Filter Filter::saturate(float value, Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

    SkColorMatrix matrix;
    matrix.setSaturation(value);

    return Filter(Impl::create(SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix), input_skia)));
}

Filter Filter::hue_rotate(float angle_degrees, Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

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

    auto color_filter = SkColorFilters::Matrix(matrix, SkColorFilters::Clamp::kNo);
    return Filter(Impl::create(SkImageFilters::ColorFilter(color_filter, input_skia)));
}

Filter Filter::image(Gfx::ImmutableBitmap const& bitmap, Gfx::IntRect const& src_rect, Gfx::IntRect const& dest_rect, Gfx::ScalingMode scaling_mode)
{
    auto skia_src_rect = to_skia_rect(src_rect);
    auto skia_dest_rect = to_skia_rect(dest_rect);
    auto sampling_options = to_skia_sampling_options(scaling_mode);

    return Filter(Impl::create(SkImageFilters::Image(sk_ref_sp(bitmap.sk_image()), skia_src_rect, skia_dest_rect, sampling_options)));
}

Filter Filter::merge(Vector<Optional<Filter>> const& inputs)
{
    Vector<sk_sp<SkImageFilter>> skia_filters;
    skia_filters.ensure_capacity(inputs.size());
    for (auto& filter : inputs)
        skia_filters.unchecked_append(filter.has_value() ? filter->m_impl->filter : nullptr);

    return Filter(Impl::create(SkImageFilters::Merge(skia_filters.data(), skia_filters.size())));
}

Filter Filter::erode(float radius_x, float radius_y, Optional<Filter> const& input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;
    return Filter(Impl::create(SkImageFilters::Erode(radius_x, radius_y, input_skia)));
}

Filter Filter::dilate(float radius_x, float radius_y, Optional<Filter> const& input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;
    return Filter(Impl::create(SkImageFilters::Dilate(radius_x, radius_y, input_skia)));
}

Filter Filter::offset(float dx, float dy, Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;
    return Filter(Impl::create(SkImageFilters::Offset(dx, dy, input_skia)));
}

Filter Filter::turbulence(TurbulenceType turbulence_type, float base_frequency_x, float base_frequency_y, i32 num_octaves, float seed, Gfx::IntSize const& tile_stitch_size)
{
    sk_sp<SkShader> turbulence_shader = [&] {
        auto skia_size = SkISize::Make(tile_stitch_size.width(), tile_stitch_size.height());
        switch (turbulence_type) {
        case TurbulenceType::Turbulence:
            return SkShaders::MakeTurbulence(base_frequency_x, base_frequency_y, num_octaves, seed, &skia_size);
        case TurbulenceType::FractalNoise:
            return SkShaders::MakeFractalNoise(base_frequency_x, base_frequency_y, num_octaves, seed, &skia_size);
        }
        VERIFY_NOT_REACHED();
    }();

    return Filter(Impl::create(SkImageFilters::Shader(move(turbulence_shader))));
}

}
