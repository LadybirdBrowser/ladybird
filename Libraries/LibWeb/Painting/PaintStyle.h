/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

struct GradientPaintStyle {
    enum class SpreadMethod {
        Pad,
        Repeat,
        Reflect
    };

    Optional<Gfx::AffineTransform> const& gradient_transform() const { return m_gradient_transform; }
    void set_gradient_transform(Gfx::AffineTransform transform) { m_gradient_transform = transform; }

    SpreadMethod spread_method() const { return m_spread_method; }
    void set_spread_method(SpreadMethod spread_method) { m_spread_method = spread_method; }

    void add_color_stop(float position, Color color)
    {
        m_color_stop_positions.append(position);
        m_color_stop_colors.append(color);
    }

    ReadonlySpan<Color> color_stop_colors() const { return m_color_stop_colors; }
    ReadonlySpan<float> color_stop_positions() const { return m_color_stop_positions; }

    Gfx::InterpolationColorSpace color_space() const { return m_color_space; }
    void set_color_space(Gfx::InterpolationColorSpace color_space) { m_color_space = color_space; }

private:
    Vector<Color, 4> m_color_stop_colors;
    Vector<float, 4> m_color_stop_positions;

    Optional<Gfx::AffineTransform> m_gradient_transform {};
    SpreadMethod m_spread_method { SpreadMethod::Pad };
    Gfx::InterpolationColorSpace m_color_space { Gfx::InterpolationColorSpace::SRGB };
};

struct LinearGradientPaintStyle final : public GradientPaintStyle {
    LinearGradientPaintStyle(Gfx::FloatPoint start_point, Gfx::FloatPoint end_point)
        : m_start_point(start_point)
        , m_end_point(end_point)
    {
    }

    Gfx::FloatPoint start_point() const { return m_start_point; }
    Gfx::FloatPoint end_point() const { return m_end_point; }

    void set_start_point(Gfx::FloatPoint start_point) { m_start_point = start_point; }
    void set_end_point(Gfx::FloatPoint end_point) { m_end_point = end_point; }

private:
    Gfx::FloatPoint m_start_point;
    Gfx::FloatPoint m_end_point;
};

struct RadialGradientPaintStyle final : public GradientPaintStyle {
    RadialGradientPaintStyle(Gfx::FloatPoint start_center, float start_radius, Gfx::FloatPoint end_center, float end_radius)
        : m_start_center(start_center)
        , m_start_radius(start_radius)
        , m_end_center(end_center)
        , m_end_radius(end_radius)
    {
    }

    Gfx::FloatPoint start_center() const { return m_start_center; }
    float start_radius() const { return m_start_radius; }
    Gfx::FloatPoint end_center() const { return m_end_center; }
    float end_radius() const { return m_end_radius; }

    void set_start_center(Gfx::FloatPoint start_center) { m_start_center = start_center; }
    void set_start_radius(float start_radius) { m_start_radius = start_radius; }
    void set_end_center(Gfx::FloatPoint end_center) { m_end_center = end_center; }
    void set_end_radius(float end_radius) { m_end_radius = end_radius; }

private:
    Gfx::FloatPoint m_start_center;
    float m_start_radius { 0.0f };
    Gfx::FloatPoint m_end_center;
    float m_end_radius { 0.0f };
};

struct PatternPaintStyle final {
    PatternPaintStyle(NonnullRefPtr<DisplayList> tile_display_list, Gfx::FloatRect tile_rect, Optional<Gfx::AffineTransform> pattern_transform);
    ~PatternPaintStyle();

    NonnullRefPtr<DisplayList> const& tile_display_list() const { return m_tile_display_list; }
    Gfx::FloatRect const& tile_rect() const { return m_tile_rect; }
    Optional<Gfx::AffineTransform> const& pattern_transform() const { return m_pattern_transform; }

private:
    NonnullRefPtr<DisplayList> m_tile_display_list;
    Gfx::FloatRect m_tile_rect;
    Optional<Gfx::AffineTransform> m_pattern_transform;
};

using PaintStyle = Variant<LinearGradientPaintStyle, RadialGradientPaintStyle, PatternPaintStyle>;
using PaintStyleOrColor = Variant<PaintStyle, Gfx::Color>;

}
