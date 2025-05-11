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

struct FilterImpl;

class Filter {
public:
    Filter(Filter const&);
    Filter& operator=(Filter const&);

    ~Filter();

    static Filter compose(Filter const& outer, Filter const& inner);
    static Filter blend(Filter const& background, Filter const& foreground, CompositingAndBlendingOperator mode);
    static Filter flood(Gfx::Color color, float opacity);
    static Filter drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input = {});
    static Filter blur(float radius, Optional<Filter const&> input = {});
    static Filter color(ColorFilterType type, float amount, Optional<Filter const&> input = {});
    static Filter color_matrix(float matrix[20], Optional<Filter const&> input = {});
    static Filter saturate(float value, Optional<Filter const&> input = {});
    static Filter hue_rotate(float angle_degrees, Optional<Filter const&> input = {});

    FilterImpl const& impl() const;

private:
    Filter(NonnullOwnPtr<FilterImpl>&&);
    NonnullOwnPtr<FilterImpl> m_impl;
};

}
