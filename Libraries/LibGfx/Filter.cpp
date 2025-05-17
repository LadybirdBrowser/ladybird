/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filter.h>
#include <LibGfx/FilterSkia.h>

namespace Gfx {

FilterImpl::~FilterImpl() = default;

FilterImplFactory::~FilterImplFactory() = default;

FilterImplFactory& FilterImplFactory::instance()
{
    static FilterImplFactorySkia factory;
    return factory;
}

Filter::Filter(NonnullOwnPtr<FilterImpl> impl)
    : m_impl(move(impl))
{
}

Filter Filter::compose(Filter const& outer, Filter const& inner)
{
    return Filter(FilterImplFactory::instance().create_compose(*outer.m_impl, *inner.m_impl));
}

Filter Filter::blend(Filter const& background, Filter const& foreground, CompositingAndBlendingOperator mode)
{
    return Filter(FilterImplFactory::instance().create_blend(*background.m_impl, *foreground.m_impl, mode));
}

Filter Filter::flood(Gfx::Color color, float opacity)
{
    return Filter(FilterImplFactory::instance().create_flood(color, opacity));
}

Filter Filter::drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input)
{
    Optional<FilterImpl const&> input_impl;

    if (input.has_value()) {
        input_impl = static_cast<FilterImpl const&>(*input.value().m_impl);
    }

    return Filter(FilterImplFactory::instance().create_drop_shadow(offset_x, offset_y, radius, color, input_impl));
}

Filter Filter::blur(float radius, Optional<Filter const&> input)
{
    Optional<FilterImpl const&> input_impl;

    if (input.has_value()) {
        input_impl = static_cast<FilterImpl const&>(*input.value().m_impl);
    }

    return Filter(FilterImplFactory::instance().create_blur(radius, input_impl));
}

Filter Filter::color(ColorFilterType type, float amount, Optional<Filter const&> input)
{
    Optional<FilterImpl const&> input_impl;

    if (input.has_value()) {
        input_impl = static_cast<FilterImpl const&>(*input.value().m_impl);
    }

    return Filter(FilterImplFactory::instance().create_color(type, amount, input_impl));
}

Filter Filter::color_matrix(float matrix[20], Optional<Filter const&> input)
{
    Optional<FilterImpl const&> input_impl;

    if (input.has_value()) {
        input_impl = static_cast<FilterImpl const&>(*input.value().m_impl);
    }

    return Filter(FilterImplFactory::instance().create_color_matrix(matrix, input_impl));
}

Filter Filter::saturate(float value, Optional<Filter const&> input)
{
    Optional<FilterImpl const&> input_impl;

    if (input.has_value()) {
        input_impl = static_cast<FilterImpl const&>(*input.value().m_impl);
    }

    return Filter(FilterImplFactory::instance().create_saturate(value, input_impl));
}

Filter Filter::hue_rotate(float angle_degrees, Optional<Filter const&> input)
{
    Optional<FilterImpl const&> input_impl;

    if (input.has_value()) {
        input_impl = static_cast<FilterImpl const&>(*input.value().m_impl);
    }

    return Filter(FilterImplFactory::instance().create_hue_rotate(angle_degrees, input_impl));
}

}
