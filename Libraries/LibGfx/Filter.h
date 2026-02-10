/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>

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

enum class TurbulenceType {
    FractalNoise,
    Turbulence,
};

struct FilterImpl;

class Filter {
public:
    Filter(Filter const&);
    Filter& operator=(Filter const&);

    ~Filter();

    static Filter arithmetic(Optional<Filter const&> background, Optional<Filter const&> foreground, float k1, float k2, float k3, float k4);
    static Filter compose(Filter const& outer, Filter const& inner);
    static Filter blend(Optional<Filter const&> background, Optional<Filter const&> foreground, CompositingAndBlendingOperator mode);
    static Filter flood(Gfx::Color color, float opacity);
    static Filter drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input = {});
    static Filter blur(float radius_x, float radius_y, Optional<Filter const&> input = {});
    static Filter color(ColorFilterType type, float amount, Optional<Filter const&> input = {});
    static Filter color_matrix(float matrix[20], Optional<Filter const&> input = {});
    static Filter color_table(Optional<ReadonlyBytes> a, Optional<ReadonlyBytes> r, Optional<ReadonlyBytes> g, Optional<ReadonlyBytes> b, Optional<Filter const&> input = {});
    static Filter saturate(float value, Optional<Filter const&> input = {});
    static Filter hue_rotate(float angle_degrees, Optional<Filter const&> input = {});
    static Filter image(Gfx::ImmutableBitmap const& bitmap, Gfx::IntRect const& src_rect, Gfx::IntRect const& dest_rect, Gfx::ScalingMode scaling_mode);
    static Filter merge(Vector<Optional<Filter>> const&);
    static Filter offset(float dx, float dy, Optional<Filter const&> input = {});
    static Filter erode(float radius_x, float radius_y, Optional<Filter> const& input = {});
    static Filter dilate(float radius_x, float radius_y, Optional<Filter> const& input = {});
    static Filter turbulence(TurbulenceType turbulence_type, float base_frequency_x, float base_frequency_y, i32 num_octaves, float seed, Gfx::IntSize const& tile_stitch_size);

    FilterImpl const& impl() const;

private:
    Filter(NonnullOwnPtr<FilterImpl>&&);
    NonnullOwnPtr<FilterImpl> m_impl;
};

}
