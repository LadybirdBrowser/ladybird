/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/ClipFrame.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/ShouldAntiAlias.h>

namespace Web::Painting {

struct StackingContextTransform {
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix;

    StackingContextTransform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 matrix, float scale);

    [[nodiscard]] bool is_identity() const { return matrix.is_identity(); }
};

class WEB_API DisplayListRecorder {
    AK_MAKE_NONCOPYABLE(DisplayListRecorder);
    AK_MAKE_NONMOVABLE(DisplayListRecorder);

public:
    void fill_rect(Gfx::IntRect const& rect, Color color);

    struct FillPathParams {
        Gfx::Path path;
        float opacity = 1.0f;
        PaintStyleOrColor paint_style_or_color;
        Gfx::WindingRule winding_rule = Gfx::WindingRule::EvenOdd;
        ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };
    };
    void fill_path(FillPathParams params);

    struct StrokePathParams {
        Gfx::Path::CapStyle cap_style;
        Gfx::Path::JoinStyle join_style;
        float miter_limit;
        Vector<float> dash_array;
        float dash_offset;
        Gfx::Path path;
        float opacity = 1.0f;
        PaintStyleOrColor paint_style_or_color;
        float thickness;
        ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };
    };
    void stroke_path(StrokePathParams);

    void draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness);

    void fill_ellipse(Gfx::IntRect const& a_rect, Color color);

    void fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data);
    void fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position);
    void fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size);

    void draw_rect(Gfx::IntRect const& rect, Color color, bool rough = false);

    void draw_painting_surface(Gfx::IntRect const& dst_rect, NonnullRefPtr<Gfx::PaintingSurface> const&, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode = Gfx::ScalingMode::NearestNeighbor);
    void draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::ImmutableBitmap const& bitmap, Gfx::ScalingMode scaling_mode = Gfx::ScalingMode::NearestNeighbor);

    void draw_repeated_immutable_bitmap(Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y);

    void draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness = 1, Gfx::LineStyle style = Gfx::LineStyle::Solid, Color alternate_color = Color::Transparent);

    void draw_text(Gfx::IntRect const&, Utf16String const&, Gfx::Font const&, Gfx::TextAlignment, Color);

    // Streamlined text drawing routine that does no wrapping/elision/alignment.
    void draw_glyph_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Gfx::Orientation);

    void add_clip_rect(Gfx::IntRect const& rect);

    void translate(Gfx::IntPoint delta);

    void push_scroll_frame_id(Optional<i32> id);
    void pop_scroll_frame_id();

    void push_clip_frame(RefPtr<ClipFrame const> const&);
    void pop_clip_frame();

    void save();
    void save_layer();
    void restore();

    struct PushStackingContextParams {
        float opacity;
        Gfx::CompositingAndBlendingOperator compositing_and_blending_operator;
        bool isolate;
        StackingContextTransform transform;
        Optional<Gfx::Path> clip_path = {};
        Optional<Gfx::IntRect> bounding_rect {};

        bool has_effect() const { return opacity != 1.0f || compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal || isolate || clip_path.has_value() || !transform.is_identity(); }
    };
    void push_stacking_context(PushStackingContextParams const& params);
    void pop_stacking_context();

    void paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect);

    void add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip);
    void add_mask(RefPtr<DisplayList> display_list, Gfx::IntRect rect);

    void apply_backdrop_filter(Gfx::IntRect const& backdrop_region, BorderRadiiData const& border_radii_data, Gfx::Filter const& backdrop_filter);

    void paint_outer_box_shadow(PaintBoxShadowParams params);
    void paint_inner_box_shadow(PaintBoxShadowParams params);
    void paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const&, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location);

    void fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadii const&);
    void fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius);
    void fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius);

    void paint_scrollbar(int scroll_frame_id, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, CSSPixelFraction scroll_size, Color thumb_color, Color track_color, bool vertical);

    void apply_opacity(float opacity);
    void apply_compositing_and_blending_operator(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator);
    void apply_filter(Gfx::Filter const& filter);
    void apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 const&);
    void apply_mask_bitmap(Gfx::IntPoint origin, Gfx::ImmutableBitmap const&, Gfx::Bitmap::MaskKind);

    DisplayListRecorder(DisplayList&);
    ~DisplayListRecorder();

    int m_save_nesting_level { 0 };

private:
    Vector<Optional<i32>> m_scroll_frame_id_stack;
    Vector<RefPtr<ClipFrame const>> m_clip_frame_stack;
    Vector<size_t> m_push_sc_index_stack;
    DisplayList& m_display_list;
};

class DisplayListRecorderStateSaver {
public:
    explicit DisplayListRecorderStateSaver(DisplayListRecorder& recorder)
        : m_recorder(recorder)
    {
        m_recorder.save();
    }

    ~DisplayListRecorderStateSaver()
    {
        m_recorder.restore();
    }

private:
    DisplayListRecorder& m_recorder;
};

}
