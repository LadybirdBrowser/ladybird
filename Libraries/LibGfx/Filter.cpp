/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filter.h>
#include <LibGfx/FilterImpl.h>
#include <LibGfx/SkiaUtils.h>
#include <core/SkBlendMode.h>
#include <core/SkColorFilter.h>
#include <effects/SkColorMatrix.h>
#include <effects/SkImageFilters.h>

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

Filter Filter::compose(Filter const& outer, Filter const& inner)
{
    auto inner_skia = inner.m_impl->filter;
    auto outer_skia = outer.m_impl->filter;

    auto filter = SkImageFilters::Compose(outer_skia, inner_skia);
    return Filter(Impl::create(filter));
}

Filter Filter::blend(Filter const& background, Filter const& foreground, Gfx::CompositingAndBlendingOperator mode)
{
    auto filter = SkImageFilters::Blend(to_skia_blender(mode), background.m_impl->filter, foreground.m_impl->filter);
    return Filter(Impl::create(filter));
}

Filter Filter::blur(float radius, Optional<Filter const&> input)
{
    sk_sp<SkImageFilter> input_skia = input.has_value() ? input->m_impl->filter : nullptr;

    auto filter = SkImageFilters::Blur(radius, radius, input_skia);
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

}
