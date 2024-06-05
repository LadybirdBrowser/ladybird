/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Memory.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/GrayscaleBitmap.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextAlignment.h>
#include <LibGfx/TextDirection.h>
#include <LibGfx/TextElision.h>
#include <LibGfx/TextWrapping.h>
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

class Painter {
public:
    static constexpr int LINE_SPACING = 4;

    explicit Painter(Gfx::Bitmap&);
    ~Painter() = default;

    void clear_rect(IntRect const&, Color);
    void fill_rect(IntRect const&, Color);
    void fill_rect(IntRect const&, PaintStyle const&);
    void fill_rect_with_gradient(Orientation, IntRect const&, Color gradient_start, Color gradient_end);
    void fill_rect_with_gradient(IntRect const&, Color gradient_start, Color gradient_end);
    void fill_rect_with_linear_gradient(IntRect const&, ReadonlySpan<ColorStop>, float angle, Optional<float> repeat_length = {});
    void fill_rect_with_conic_gradient(IntRect const&, ReadonlySpan<ColorStop>, IntPoint center, float start_angle, Optional<float> repeat_length = {});
    void fill_rect_with_radial_gradient(IntRect const&, ReadonlySpan<ColorStop>, IntPoint center, IntSize size, Optional<float> repeat_length = {}, Optional<float> rotation_angle = {});
    void fill_rect_with_rounded_corners(IntRect const&, Color, int radius);
    void fill_rect_with_rounded_corners(IntRect const&, Color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius);
    void fill_ellipse(IntRect const&, Color);
    void draw_rect(IntRect const&, Color, bool rough = false);
    void draw_scaled_bitmap(IntRect const& dst_rect, Gfx::Bitmap const&, IntRect const& src_rect, float opacity = 1.0f, ScalingMode = ScalingMode::NearestNeighbor);
    void draw_scaled_bitmap(IntRect const& dst_rect, Gfx::Bitmap const&, FloatRect const& src_rect, float opacity = 1.0f, ScalingMode = ScalingMode::NearestNeighbor);
    void draw_scaled_bitmap_with_transform(IntRect const& dst_rect, Gfx::Bitmap const&, FloatRect const& src_rect, Gfx::AffineTransform const&, float opacity = 1.0f, ScalingMode = ScalingMode::NearestNeighbor);
    void set_pixel(IntPoint, Color, bool blend = false);
    void set_pixel(int x, int y, Color color, bool blend = false) { set_pixel({ x, y }, color, blend); }
    Optional<Color> get_pixel(IntPoint);
    ErrorOr<NonnullRefPtr<Bitmap>> get_region_bitmap(IntRect const&, BitmapFormat format, Optional<IntRect&> actual_region = {});
    void draw_line(IntPoint, IntPoint, Color, int thickness = 1, LineStyle style = LineStyle::Solid, Color alternate_color = Color::Transparent);
    void draw_triangle_wave(IntPoint, IntPoint, Color color, int amplitude, int thickness = 1);
    void blit(IntPoint, Gfx::Bitmap const&, IntRect const& src_rect, float opacity = 1.0f, bool apply_alpha = true);
    void blit_filtered(IntPoint, Gfx::Bitmap const&, IntRect const& src_rect, Function<Color(Color)> const&, bool apply_alpha = true);
    void draw_text(FloatRect const&, StringView, Font const&, TextAlignment = TextAlignment::TopLeft, Color = Color::Black, TextElision = TextElision::None, TextWrapping = TextWrapping::DontWrap);
    void draw_text(Function<void(FloatRect const&, Utf8CodePointIterator&)>, FloatRect const&, StringView, Font const&, TextAlignment = TextAlignment::TopLeft, TextElision = TextElision::None, TextWrapping = TextWrapping::DontWrap);
    void draw_text(Function<void(FloatRect const&, Utf8CodePointIterator&)>, FloatRect const&, Utf8View const&, Font const&, TextAlignment = TextAlignment::TopLeft, TextElision = TextElision::None, TextWrapping = TextWrapping::DontWrap);
    void draw_text(IntRect const&, StringView, Font const&, TextAlignment = TextAlignment::TopLeft, Color = Color::Black, TextElision = TextElision::None, TextWrapping = TextWrapping::DontWrap);
    void draw_text(Function<void(FloatRect const&, Utf8CodePointIterator&)>, IntRect const&, StringView, Font const&, TextAlignment = TextAlignment::TopLeft, TextElision = TextElision::None, TextWrapping = TextWrapping::DontWrap);
    void draw_text(Function<void(FloatRect const&, Utf8CodePointIterator&)>, IntRect const&, Utf8View const&, Font const&, TextAlignment = TextAlignment::TopLeft, TextElision = TextElision::None, TextWrapping = TextWrapping::DontWrap);
    void draw_emoji(IntPoint, Gfx::Bitmap const&, Font const&);
    void draw_glyph(FloatPoint, u32, Font const&, Color);
    void draw_glyph_or_emoji(FloatPoint, Utf8CodePointIterator&, Font const&, Color);
    void draw_signed_distance_field(IntRect const& dst_rect, Color, Gfx::GrayscaleBitmap const&, float smoothing);

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

    void stroke_path(Path const&, Color, int thickness);

    void fill_path(Path const&, Color, WindingRule rule = WindingRule::Nonzero);
    void fill_path(Path const&, PaintStyle const& paint_style, float opacity = 1.0f, WindingRule rule = WindingRule::Nonzero);

    void add_clip_rect(IntRect const& rect);
    void clear_clip_rect();

    void translate(int dx, int dy) { translate({ dx, dy }); }
    void translate(IntPoint delta) { state().translation.translate_by(delta); }

    IntPoint translation() const { return state().translation; }

    Gfx::Bitmap* target() { return m_target.ptr(); }

    void save() { m_state_stack.append(m_state_stack.last()); }
    void restore()
    {
        VERIFY(m_state_stack.size() > 1);
        m_state_stack.take_last();
    }

    IntRect clip_rect() const { return state().clip_rect; }

protected:
    friend GradientLine;
    friend AntiAliasingPainter;
    template<unsigned SamplesPerPixel>
    friend class EdgeFlagPathRasterizer;

    IntRect to_physical(IntRect const& r) const { return r.translated(translation()); }
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

    IntRect m_clip_origin;
    NonnullRefPtr<Gfx::Bitmap> m_target;
    Vector<State, 4> m_state_stack;

private:
    Vector<DirectionalRun> split_text_into_directional_runs(Utf8View const&, TextDirection initial_direction);
    bool text_contains_bidirectional_text(Utf8View const&, TextDirection);
    template<typename DrawGlyphFunction>
    void do_draw_text(FloatRect const&, Utf8View const& text, Font const&, TextAlignment, TextElision, TextWrapping, DrawGlyphFunction);
};

class PainterStateSaver {
public:
    explicit PainterStateSaver(Painter&);
    ~PainterStateSaver();

private:
    Painter& m_painter;
};

ByteString parse_ampersand_string(StringView, Optional<size_t>* underline_offset = nullptr);

}
