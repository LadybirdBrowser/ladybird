/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextAlignment.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/Command.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

class DisplayListRecorder {
    AK_MAKE_NONCOPYABLE(DisplayListRecorder);
    AK_MAKE_NONMOVABLE(DisplayListRecorder);

public:
    void fill_rect(Gfx::IntRect const& rect, Color color);

    struct FillPathUsingColorParams {
        Gfx::Path path;
        Gfx::Color color;
        Gfx::WindingRule winding_rule = Gfx::WindingRule::EvenOdd;
        Optional<Gfx::FloatPoint> translation = {};
    };
    void fill_path(FillPathUsingColorParams params);

    struct FillPathUsingPaintStyleParams {
        Gfx::Path path;
        PaintStyle paint_style;
        Gfx::WindingRule winding_rule = Gfx::WindingRule::EvenOdd;
        float opacity;
        Optional<Gfx::FloatPoint> translation = {};
    };
    void fill_path(FillPathUsingPaintStyleParams params);

    struct StrokePathUsingColorParams {
        Gfx::Path::CapStyle cap_style;
        Gfx::Path::JoinStyle join_style;
        float miter_limit;
        Vector<float> dash_array;
        float dash_offset;
        Gfx::Path path;
        Gfx::Color color;
        float thickness;
        Optional<Gfx::FloatPoint> translation = {};
    };
    void stroke_path(StrokePathUsingColorParams params);

    struct StrokePathUsingPaintStyleParams {
        Gfx::Path::CapStyle cap_style;
        Gfx::Path::JoinStyle join_style;
        float miter_limit;
        Vector<float> dash_array;
        float dash_offset;
        Gfx::Path path;
        PaintStyle paint_style;
        float thickness;
        float opacity;
        Optional<Gfx::FloatPoint> translation = {};
    };
    void stroke_path(StrokePathUsingPaintStyleParams params);

    void draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness);

    void fill_ellipse(Gfx::IntRect const& a_rect, Color color);

    void fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data);
    void fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position);
    void fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size);

    void draw_rect(Gfx::IntRect const& rect, Color color, bool rough = false);

    void draw_painting_surface(Gfx::IntRect const& dst_rect, NonnullRefPtr<Gfx::PaintingSurface>, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode = Gfx::ScalingMode::NearestNeighbor);
    void draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::ImmutableBitmap const& bitmap, Gfx::ScalingMode scaling_mode = Gfx::ScalingMode::NearestNeighbor);

    void draw_repeated_immutable_bitmap(Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, NonnullRefPtr<Gfx::ImmutableBitmap> bitmap, Gfx::ScalingMode scaling_mode, DrawRepeatedImmutableBitmap::Repeat);

    void draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness = 1, Gfx::LineStyle style = Gfx::LineStyle::Solid, Color alternate_color = Color::Transparent);

    void draw_text(Gfx::IntRect const&, String, Gfx::Font const&, Gfx::TextAlignment, Color);

    // Streamlined text drawing routine that does no wrapping/elision/alignment.
    void draw_text_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Gfx::Orientation);

    void add_clip_rect(Gfx::IntRect const& rect);

    void translate(Gfx::IntPoint delta);

    void push_scroll_frame_id(Optional<i32> id);
    void pop_scroll_frame_id();

    void save();
    void restore();

    struct PushStackingContextParams {
        float opacity;
        Gfx::CompositingAndBlendingOperator compositing_and_blending_operator;
        bool isolate;
        bool is_fixed_position;
        Gfx::IntRect source_paintable_rect;
        StackingContextTransform transform;
        Optional<Gfx::Path> clip_path = {};
    };
    void push_stacking_context(PushStackingContextParams params);
    void pop_stacking_context();

    void paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect);

    void add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip);
    void add_mask(RefPtr<DisplayList> display_list, Gfx::IntRect rect);

    void apply_backdrop_filter(Gfx::IntRect const& backdrop_region, BorderRadiiData const& border_radii_data, Vector<Gfx::Filter> const& backdrop_filter);

    void paint_outer_box_shadow_params(PaintBoxShadowParams params);
    void paint_inner_box_shadow_params(PaintBoxShadowParams params);
    void paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const&, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location);

    void fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadius top_left_radius, CornerRadius top_right_radius, CornerRadius bottom_right_radius, CornerRadius bottom_left_radius);
    void fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius);
    void fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius);

    void draw_triangle_wave(Gfx::IntPoint a_p1, Gfx::IntPoint a_p2, Color color, int amplitude, int thickness);

    void paint_scrollbar(int scroll_frame_id, Gfx::IntRect, CSSPixelFraction scroll_size, bool vertical);

    void apply_opacity(float opacity);
    void apply_compositing_and_blending_operator(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);
    void apply_filters(Vector<Gfx::Filter> filter);
    void apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4);
    void apply_mask_bitmap(Gfx::IntPoint origin, Gfx::ImmutableBitmap const&, Gfx::Bitmap::MaskKind);

    DisplayListRecorder(DisplayList&);
    ~DisplayListRecorder();

    DisplayList& display_list() { return m_command_list; }

    void append(Command&& command);

private:
    Vector<Optional<i32>> m_scroll_frame_id_stack;
    DisplayList& m_command_list;
};

class DisplayListRecorderStateSaver {
public:
    explicit DisplayListRecorderStateSaver(DisplayListRecorder& painter)
        : m_painter(painter)
    {
        m_painter.save();
    }

    ~DisplayListRecorderStateSaver()
    {
        m_painter.restore();
    }

private:
    DisplayListRecorder& m_painter;
};

}
