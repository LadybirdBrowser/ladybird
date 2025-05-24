/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>

namespace Gfx {

enum class ColorFilterType {
    Brightness,
    Contrast,
    Grayscale,
    Invert,
    Opacity,
    Saturate,
    Sepia
};

class FilterImpl {
public:
    virtual ~FilterImpl();
    virtual NonnullOwnPtr<FilterImpl> clone() const = 0;
};

class FilterImplFactory {
public:
    virtual ~FilterImplFactory();

    virtual NonnullOwnPtr<FilterImpl> create_compose(FilterImpl const& outer, FilterImpl const& inner) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_blur(float radius, Optional<FilterImpl const&> input) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<FilterImpl const&> input) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_flood(Gfx::Color color, float opacity) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_blend(FilterImpl const& first, FilterImpl const& second, CompositingAndBlendingOperator mode) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_color(ColorFilterType type, float amount, Optional<FilterImpl const&> input) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_color_matrix(float matrix[20], Optional<FilterImpl const&> input) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_saturate(float value, Optional<FilterImpl const&> input) = 0;
    virtual NonnullOwnPtr<FilterImpl> create_hue_rotate(float angle_degrees, Optional<FilterImpl const&> input) = 0;

    static FilterImplFactory& instance();
};

class Filter {
public:
    Filter(Filter const& other)
        : m_impl(other.impl().clone())
    {
    }

    Filter& operator=(Filter const& other)
    {
        if (this != &other)
            m_impl = other.impl().clone();
        return *this;
    }

    Filter(Filter&& other) = default;
    Filter& operator=(Filter&& other) = default;

    static Filter compose(Filter const& outer, Filter const& inner);
    static Filter blend(Filter const& background, Filter const& foreground, CompositingAndBlendingOperator mode);
    static Filter flood(Gfx::Color color, float opacity);
    static Filter drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input = {});
    static Filter blur(float radius, Optional<Filter const&> input = {});
    static Filter color(ColorFilterType type, float amount, Optional<Filter const&> input = {});
    static Filter color_matrix(float matrix[20], Optional<Filter const&> input = {});
    static Filter saturate(float value, Optional<Filter const&> input = {});
    static Filter hue_rotate(float angle_degrees, Optional<Filter const&> input = {});

    FilterImpl& impl() { return *m_impl; }
    FilterImpl const& impl() const { return *m_impl; }

private:
    explicit Filter(NonnullOwnPtr<FilterImpl>);

    NonnullOwnPtr<FilterImpl> m_impl;
};

}
