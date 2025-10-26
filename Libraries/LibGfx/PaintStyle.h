/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/QuickSort.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Rect.h>

namespace Gfx {

class PaintStyle : public RefCounted<PaintStyle> {
public:
    virtual ~PaintStyle() = default;
    virtual bool is_visible() const { return true; }
};

class SolidColorPaintStyle : public PaintStyle {
public:
    static ErrorOr<NonnullRefPtr<SolidColorPaintStyle>> create(Color color)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) SolidColorPaintStyle(color));
    }

    bool is_visible() const override { return m_color.alpha() > 0; }

    Color const& color() const { return m_color; }

private:
    SolidColorPaintStyle(Color color)
        : m_color(color)
    {
    }

    Color m_color;
};

class GradientPaintStyle : public PaintStyle {
public:
    ErrorOr<void> add_color_stop(float position, Color color, Optional<float> transition_hint = {})
    {
        return add_color_stop(ColorStop { color, position, transition_hint });
    }

    ErrorOr<void> add_color_stop(ColorStop stop, bool sort = true)
    {
        TRY(m_color_stops.try_append(stop));
        if (sort)
            quick_sort(m_color_stops, [](auto& a, auto& b) { return a.position < b.position; });
        return {};
    }

    void set_repeat_length(float repeat_length)
    {
        m_repeat_length = repeat_length;
    }

    ReadonlySpan<ColorStop> color_stops() const { return m_color_stops; }
    void set_color_stops(Vector<ColorStop>&& color_stops) { m_color_stops = move(color_stops); }

    Optional<float> repeat_length() const { return m_repeat_length; }

    bool is_visible() const override
    {
        return any_of(m_color_stops, [](auto& stop) { return stop.color.alpha() > 0; });
    }

private:
    Vector<ColorStop, 4> m_color_stops;
    Optional<float> m_repeat_length;
};

class CanvasPatternPaintStyle : public PaintStyle {
public:
    enum class Repetition : u8 {
        Repeat,
        RepeatX,
        RepeatY,
        NoRepeat
    };

    static ErrorOr<NonnullRefPtr<CanvasPatternPaintStyle>> create(RefPtr<ImmutableBitmap> image, Repetition repetition)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasPatternPaintStyle(image, repetition));
    }

    RefPtr<ImmutableBitmap> image() const { return m_image; }
    Repetition repetition() const { return m_repetition; }
    Optional<AffineTransform> const& transform() const { return m_transform; }
    void set_transform(AffineTransform const& transform) { m_transform = transform; }

private:
    CanvasPatternPaintStyle(RefPtr<ImmutableBitmap> image, Repetition repetition)
        : m_image(image)
        , m_repetition(repetition)
    {
    }

    RefPtr<ImmutableBitmap> m_image;
    Repetition m_repetition { Repetition::Repeat };
    Optional<AffineTransform> m_transform;
};

// The following paint styles implement the gradients required for the HTML canvas.
// These gradients are (unlike CSS ones) not relative to the painted shape, and do not
// support premultiplied alpha.

class CanvasLinearGradientPaintStyle : public GradientPaintStyle {
public:
    static ErrorOr<NonnullRefPtr<CanvasLinearGradientPaintStyle>> create(FloatPoint p0, FloatPoint p1)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasLinearGradientPaintStyle(p0, p1));
    }

    FloatPoint start_point() const { return m_p0; }
    FloatPoint end_point() const { return m_p1; }

private:
    CanvasLinearGradientPaintStyle(FloatPoint p0, FloatPoint p1)
        : m_p0(p0)
        , m_p1(p1)
    {
    }

    FloatPoint m_p0;
    FloatPoint m_p1;
};

class CanvasConicGradientPaintStyle : public GradientPaintStyle {
public:
    static ErrorOr<NonnullRefPtr<CanvasConicGradientPaintStyle>> create(FloatPoint center, float start_angle = 0.0f)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasConicGradientPaintStyle(center, start_angle));
    }

    FloatPoint center() const { return m_center; }
    float start_angle() const { return m_start_angle; }

private:
    CanvasConicGradientPaintStyle(FloatPoint center, float start_angle)
        : m_center(center)
        , m_start_angle(start_angle)
    {
    }

    FloatPoint m_center;
    float m_start_angle { 0.0f };
};

class CanvasRadialGradientPaintStyle : public GradientPaintStyle {
public:
    static ErrorOr<NonnullRefPtr<CanvasRadialGradientPaintStyle>> create(FloatPoint start_center, float start_radius, FloatPoint end_center, float end_radius)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasRadialGradientPaintStyle(start_center, start_radius, end_center, end_radius));
    }

    Gfx::FloatPoint start_center() const { return m_start_center; }
    float start_radius() const { return m_start_radius; }
    Gfx::FloatPoint end_center() const { return m_end_center; }
    float end_radius() const { return m_end_radius; }

private:
    CanvasRadialGradientPaintStyle(FloatPoint start_center, float start_radius, FloatPoint end_center, float end_radius)
        : m_start_center(start_center)
        , m_start_radius(start_radius)
        , m_end_center(end_center)
        , m_end_radius(end_radius)
    {
    }

    FloatPoint m_start_center;
    float m_start_radius { 0.0f };
    FloatPoint m_end_center;
    float m_end_radius { 0.0f };
};

// The following paint styles implement the gradients required for SVGs

class SVGGradientPaintStyle : public GradientPaintStyle {
public:
    void set_gradient_transform(Gfx::AffineTransform transform);

    enum class SpreadMethod {
        Pad,
        Repeat,
        Reflect
    };

    void set_spread_method(SpreadMethod spread_method)
    {
        m_spread_method = spread_method;
    }

protected:
    Optional<AffineTransform> const& scale_adjusted_inverse_gradient_transform() const { return m_inverse_transform; }
    float gradient_transform_scale() const { return m_scale; }
    SpreadMethod spread_method() const { return m_spread_method; }

private:
    Optional<AffineTransform> m_inverse_transform {};
    float m_scale = 1.0f;
    SpreadMethod m_spread_method { SpreadMethod::Pad };
};

class SVGLinearGradientPaintStyle : public SVGGradientPaintStyle {
public:
    static ErrorOr<NonnullRefPtr<SVGLinearGradientPaintStyle>> create(FloatPoint p0, FloatPoint p1)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) SVGLinearGradientPaintStyle(p0, p1));
    }

    void set_start_point(FloatPoint start_point)
    {
        m_p0 = start_point;
    }

    void set_end_point(FloatPoint end_point)
    {
        m_p1 = end_point;
    }

    SVGLinearGradientPaintStyle(FloatPoint p0, FloatPoint p1)
        : m_p0(p0)
        , m_p1(p1)
    {
    }

private:
    FloatPoint m_p0;
    FloatPoint m_p1;
};

class SVGRadialGradientPaintStyle final : public SVGGradientPaintStyle {
public:
    static ErrorOr<NonnullRefPtr<SVGRadialGradientPaintStyle>> create(FloatPoint start_center, float start_radius, FloatPoint end_center, float end_radius)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) SVGRadialGradientPaintStyle(start_center, start_radius, end_center, end_radius));
    }

    void set_start_center(FloatPoint start_center)
    {
        m_start_center = start_center;
    }

    void set_start_radius(float start_radius)
    {
        m_start_radius = start_radius;
    }

    void set_end_center(FloatPoint end_center)
    {
        m_end_center = end_center;
    }

    void set_end_radius(float end_radius)
    {
        m_end_radius = end_radius;
    }

    SVGRadialGradientPaintStyle(FloatPoint start_center, float start_radius, FloatPoint end_center, float end_radius)
        : m_start_center(start_center)
        , m_start_radius(start_radius)
        , m_end_center(end_center)
        , m_end_radius(end_radius)
    {
    }

private:
    FloatPoint m_start_center;
    float m_start_radius { 0.0f };
    FloatPoint m_end_center;
    float m_end_radius { 0.0f };
};

}
