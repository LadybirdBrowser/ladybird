/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintStyle.h>

namespace Web::Painting {

class SVGPaintStyle : public AtomicRefCounted<SVGPaintStyle> {
public:
    virtual ~SVGPaintStyle() = default;
};

struct ColorStop {
    Color color;
    float position = AK::NaN<float>;
    Optional<float> transition_hint = {};
};

class SVGGradientPaintStyle : public SVGPaintStyle {
public:
    enum class SpreadMethod {
        Pad,
        Repeat,
        Reflect
    };

    Optional<Gfx::AffineTransform> const& gradient_transform() const { return m_gradient_transform; }
    void set_gradient_transform(Gfx::AffineTransform transform) { m_gradient_transform = transform; }

    SpreadMethod spread_method() const { return m_spread_method; }
    void set_spread_method(SpreadMethod spread_method) { m_spread_method = spread_method; }

    void add_color_stop(float position, Color color, Optional<float> transition_hint = {})
    {
        return add_color_stop(ColorStop { color, position, transition_hint });
    }

    void add_color_stop(ColorStop stop, bool sort = true)
    {
        m_color_stops.append(stop);
        if (sort)
            quick_sort(m_color_stops, [](auto& a, auto& b) { return a.position < b.position; });
    }

    ReadonlySpan<ColorStop> color_stops() const { return m_color_stops; }
    Optional<float> repeat_length() const { return m_repeat_length; }

    virtual ~SVGGradientPaintStyle() { }

protected:
    Vector<ColorStop, 4> m_color_stops;
    Optional<float> m_repeat_length;

    Optional<Gfx::AffineTransform> m_gradient_transform {};
    SpreadMethod m_spread_method { SpreadMethod::Pad };
};

class SVGLinearGradientPaintStyle final : public SVGGradientPaintStyle {
public:
    static NonnullRefPtr<SVGLinearGradientPaintStyle> create(Gfx::FloatPoint start_point, Gfx::FloatPoint end_point)
    {
        return adopt_ref(*new SVGLinearGradientPaintStyle(start_point, end_point));
    }

    Gfx::FloatPoint start_point() const { return m_start_point; }
    Gfx::FloatPoint end_point() const { return m_end_point; }

    void set_start_point(Gfx::FloatPoint start_point) { m_start_point = start_point; }
    void set_end_point(Gfx::FloatPoint end_point) { m_end_point = end_point; }

private:
    SVGLinearGradientPaintStyle(Gfx::FloatPoint start_point, Gfx::FloatPoint end_point)
        : m_start_point(start_point)
        , m_end_point(end_point)
    {
    }

    Gfx::FloatPoint m_start_point;
    Gfx::FloatPoint m_end_point;
};

class SVGRadialGradientPaintStyle final : public SVGGradientPaintStyle {
public:
    static NonnullRefPtr<SVGRadialGradientPaintStyle> create(Gfx::FloatPoint start_center, float start_radius, Gfx::FloatPoint end_center, float end_radius)
    {
        return adopt_ref(*new SVGRadialGradientPaintStyle(start_center, start_radius, end_center, end_radius));
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
    SVGRadialGradientPaintStyle(Gfx::FloatPoint start_center, float start_radius, Gfx::FloatPoint end_center, float end_radius)
        : m_start_center(start_center)
        , m_start_radius(start_radius)
        , m_end_center(end_center)
        , m_end_radius(end_radius)
    {
    }

    Gfx::FloatPoint m_start_center;
    float m_start_radius { 0.0f };
    Gfx::FloatPoint m_end_center;
    float m_end_radius { 0.0f };
};

class SVGPatternPaintStyle final : public SVGPaintStyle {
public:
    static NonnullRefPtr<SVGPatternPaintStyle> create(NonnullRefPtr<Gfx::ImmutableBitmap const> tile_bitmap, Gfx::AffineTransform device_space_matrix, bool repeat_x, bool repeat_y)
    {
        return adopt_ref(*new SVGPatternPaintStyle(move(tile_bitmap), device_space_matrix, repeat_x, repeat_y));
    }

    Gfx::ImmutableBitmap const& tile_bitmap() const { return *m_tile_bitmap; }
    Gfx::AffineTransform const& device_space_matrix() const { return m_device_space_matrix; }
    bool repeat_x() const { return m_repeat_x; }
    bool repeat_y() const { return m_repeat_y; }

private:
    SVGPatternPaintStyle(NonnullRefPtr<Gfx::ImmutableBitmap const> tile_bitmap, Gfx::AffineTransform device_space_matrix, bool repeat_x, bool repeat_y)
        : m_tile_bitmap(move(tile_bitmap))
        , m_device_space_matrix(device_space_matrix)
        , m_repeat_x(repeat_x)
        , m_repeat_y(repeat_y)
    {
    }

    NonnullRefPtr<Gfx::ImmutableBitmap const> m_tile_bitmap;
    Gfx::AffineTransform m_device_space_matrix;
    bool m_repeat_x { true };
    bool m_repeat_y { true };
};

}
