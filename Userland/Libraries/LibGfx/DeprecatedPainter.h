/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Memory.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

ALWAYS_INLINE static Color color_for_format(BitmapFormat format, ARGB32 value)
{
    switch (format) {
    case BitmapFormat::BGRA8888:
        return Color::from_argb(value);
    case BitmapFormat::BGRx8888:
        return Color::from_rgb(value);
    // FIXME: Handle other formats
    default:
        VERIFY_NOT_REACHED();
    }
}

class DeprecatedPainter {
public:
    explicit DeprecatedPainter(Gfx::Bitmap&);
    ~DeprecatedPainter() = default;

    void clear_rect(IntRect const&, Color);
    void fill_rect(IntRect const&, Color);
    void fill_rect(IntRect const&, PaintStyle const&);
    void fill_rect_with_rounded_corners(IntRect const&, Color, int radius);
    void fill_rect_with_rounded_corners(IntRect const&, Color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius);
    void fill_ellipse(IntRect const&, Color);
    void draw_rect(IntRect const&, Color, bool rough = false);
    Optional<Color> get_pixel(IntPoint);
    void draw_line(IntPoint, IntPoint, Color, int thickness = 1, LineStyle style = LineStyle::Solid, Color alternate_color = Color::Transparent);
    void blit(IntPoint, Gfx::Bitmap const&, IntRect const& src_rect, float opacity = 1.0f, bool apply_alpha = true);
    void blit_filtered(IntPoint, Gfx::Bitmap const&, IntRect const& src_rect, Function<Color(Color)> const&, bool apply_alpha = true);

    enum class CornerOrientation {
        TopLeft,
        TopRight,
        BottomRight,
        BottomLeft
    };
    void fill_rounded_corner(IntRect const&, int radius, Color, CornerOrientation);

    static void for_each_line_segment_on_bezier_curve(FloatPoint control_point, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>&);
    static void for_each_line_segment_on_bezier_curve(FloatPoint control_point, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>&&);

    static void for_each_line_segment_on_cubic_bezier_curve(FloatPoint control_point_0, FloatPoint control_point_1, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>&);
    static void for_each_line_segment_on_cubic_bezier_curve(FloatPoint control_point_0, FloatPoint control_point_1, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>&&);

    void stroke_path(DeprecatedPath const&, Color, int thickness);

    void fill_path(DeprecatedPath const&, Color, WindingRule rule = WindingRule::Nonzero);
    void fill_path(DeprecatedPath const&, PaintStyle const& paint_style, float opacity = 1.0f, WindingRule rule = WindingRule::Nonzero);

    void add_clip_rect(IntRect const& rect);

    void translate(int dx, int dy) { state().translation.translate_by({ dx, dy }); }

    IntPoint translation() const { return state().translation; }

    [[nodiscard]] Gfx::Bitmap& target() { return *m_target; }

    void save() { m_state_stack.append(m_state_stack.last()); }
    void restore()
    {
        VERIFY(m_state_stack.size() > 1);
        m_state_stack.take_last();
    }

    IntRect clip_rect() const { return state().clip_rect; }

protected:
    friend AntiAliasingPainter;
    template<unsigned SamplesPerPixel>
    friend class EdgeFlagPathRasterizer;

    IntPoint to_physical(IntPoint p) const { return p.translated(translation()); }
    void set_physical_pixel(u32& pixel, Color);
    void fill_physical_scanline(int y, int x, int width, Color color);
    void blit_with_opacity(IntPoint, Gfx::Bitmap const&, IntRect const& src_rect, float opacity, bool apply_alpha = true);
    void draw_physical_pixel(IntPoint, Color, int thickness = 1);
    void set_physical_pixel(IntPoint, Color color, bool blend);

    struct State {
        IntPoint translation;
        IntRect clip_rect;
    };

    State& state() { return m_state_stack.last(); }
    State const& state() const { return m_state_stack.last(); }

    void fill_physical_rect(IntRect const&, Color);

    NonnullRefPtr<Gfx::Bitmap> m_target;
    Vector<State, 4> m_state_stack;
};

}
