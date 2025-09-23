/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

StackingContextTransform::StackingContextTransform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 matrix, float scale)
{
    this->origin = origin.scaled(scale);
    matrix[0, 3] *= scale;
    matrix[1, 3] *= scale;
    matrix[2, 3] *= scale;
    this->matrix = matrix;
}

DisplayListRecorder::DisplayListRecorder(DisplayList& command_list)
    : m_display_list(command_list)
{
}

DisplayListRecorder::~DisplayListRecorder() = default;

template<typename T>
consteval static int command_nesting_level_change(T const& command)
{
    if constexpr (requires { command.nesting_level_change; })
        return command.nesting_level_change;
    return 0;
}

#define APPEND(...)                                                          \
    do {                                                                     \
        auto command = __VA_ARGS__;                                          \
        Optional<i32> _scroll_frame_id;                                      \
        if (!m_scroll_frame_id_stack.is_empty())                             \
            _scroll_frame_id = m_scroll_frame_id_stack.last();               \
        RefPtr<ClipFrame const> _clip_frame;                                 \
        if (!m_clip_frame_stack.is_empty())                                  \
            _clip_frame = m_clip_frame_stack.last();                         \
        m_save_nesting_level += command_nesting_level_change(command);       \
        m_display_list.append(move(command), _scroll_frame_id, _clip_frame); \
    } while (false)

void DisplayListRecorder::paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    APPEND(PaintNestedDisplayList { move(display_list), rect });
}

void DisplayListRecorder::add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    APPEND(AddRoundedRectClip { corner_radii, border_rect, corner_clip });
}

void DisplayListRecorder::add_mask(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    if (rect.is_empty())
        return;
    APPEND(AddMask { move(display_list), rect });
}

void DisplayListRecorder::fill_rect(Gfx::IntRect const& rect, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(FillRect { rect, color });
}

void DisplayListRecorder::fill_path(FillPathParams params)
{
    if (params.paint_style_or_color.has<Gfx::Color>() && params.paint_style_or_color.get<Gfx::Color>().alpha() == 0)
        return;
    auto path_bounding_rect = params.path.bounding_box();
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    APPEND(FillPath {
        .path_bounding_rect = path_bounding_int_rect,
        .path = move(params.path),
        .opacity = params.opacity,
        .paint_style_or_color = params.paint_style_or_color,
        .winding_rule = params.winding_rule,
        .should_anti_alias = params.should_anti_alias });
}

void DisplayListRecorder::stroke_path(StrokePathParams params)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!params.thickness)
        return;
    if (params.paint_style_or_color.has<Gfx::Color>() && params.paint_style_or_color.get<Gfx::Color>().alpha() == 0)
        return;
    auto path_bounding_rect = params.path.bounding_box();
    // Increase path bounding box by `thickness` to account for stroke.
    path_bounding_rect.inflate(params.thickness, params.thickness);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    APPEND(StrokePath {
        .cap_style = params.cap_style,
        .join_style = params.join_style,
        .miter_limit = params.miter_limit,
        .dash_array = move(params.dash_array),
        .dash_offset = params.dash_offset,
        .path_bounding_rect = path_bounding_int_rect,
        .path = move(params.path),
        .opacity = params.opacity,
        .paint_style_or_color = params.paint_style_or_color,
        .thickness = params.thickness,
        .should_anti_alias = params.should_anti_alias });
}

void DisplayListRecorder::draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness)
{
    if (a_rect.is_empty() || color.alpha() == 0 || !thickness)
        return;
    APPEND(DrawEllipse {
        .rect = a_rect,
        .color = color,
        .thickness = thickness,
    });
}

void DisplayListRecorder::fill_ellipse(Gfx::IntRect const& a_rect, Color color)
{
    if (a_rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(FillEllipse { a_rect, color });
}

void DisplayListRecorder::fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data)
{
    if (gradient_rect.is_empty())
        return;
    APPEND(PaintLinearGradient { gradient_rect, data });
}

void DisplayListRecorder::fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position)
{
    if (rect.is_empty())
        return;
    APPEND(PaintConicGradient {
        .rect = rect,
        .conic_gradient_data = data,
        .position = position });
}

void DisplayListRecorder::fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size)
{
    if (rect.is_empty())
        return;
    APPEND(PaintRadialGradient {
        .rect = rect,
        .radial_gradient_data = data,
        .center = center,
        .size = size });
}

void DisplayListRecorder::draw_rect(Gfx::IntRect const& rect, Color color, bool rough)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    APPEND(DrawRect {
        .rect = rect,
        .color = color,
        .rough = rough });
}

void DisplayListRecorder::draw_painting_surface(Gfx::IntRect const& dst_rect, NonnullRefPtr<Gfx::PaintingSurface> surface, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    APPEND(DrawPaintingSurface {
        .dst_rect = dst_rect,
        .surface = surface,
        .src_rect = src_rect,
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::ImmutableBitmap const& bitmap, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    APPEND(DrawScaledImmutableBitmap {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .bitmap = bitmap,
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_repeated_immutable_bitmap(Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y)
{
    APPEND(DrawRepeatedImmutableBitmap {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .bitmap = move(bitmap),
        .scaling_mode = scaling_mode,
        .repeat = { repeat_x, repeat_y },
    });
}

void DisplayListRecorder::draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness, Gfx::LineStyle style, Color alternate_color)
{
    if (color.alpha() == 0 || !thickness)
        return;
    APPEND(DrawLine {
        .color = color,
        .from = from,
        .to = to,
        .thickness = thickness,
        .style = style,
        .alternate_color = alternate_color,
    });
}

void DisplayListRecorder::draw_text(Gfx::IntRect const& rect, Utf16String const& raw_text, Gfx::Font const& font, Gfx::TextAlignment alignment, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;

    auto glyph_run = Gfx::shape_text({}, 0, raw_text.utf16_view(), font, Gfx::GlyphRun::TextType::Ltr, {});
    float baseline_x = 0;
    if (alignment == Gfx::TextAlignment::CenterLeft) {
        baseline_x = rect.x();
    } else if (alignment == Gfx::TextAlignment::Center) {
        baseline_x = static_cast<float>(rect.x()) + (static_cast<float>(rect.width()) - glyph_run->width()) / 2.0f;
    } else if (alignment == Gfx::TextAlignment::CenterRight) {
        baseline_x = static_cast<float>(rect.right()) - glyph_run->width();
    } else {
        // Unimplemented alignment.
        TODO();
    }
    auto metrics = font.pixel_metrics();
    float baseline_y = static_cast<float>(rect.y()) + metrics.ascent + (static_cast<float>(rect.height()) - (metrics.ascent + metrics.descent)) / 2.0f;
    draw_glyph_run({ baseline_x, baseline_y }, *glyph_run, color, rect, 1.0, Orientation::Horizontal);
}

void DisplayListRecorder::draw_glyph_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Orientation orientation)
{
    if (color.alpha() == 0)
        return;
    APPEND(DrawGlyphRun {
        .glyph_run = glyph_run,
        .scale = scale,
        .rect = rect,
        .translation = baseline_start,
        .color = color,
        .orientation = orientation,
        .bounding_rectangle = glyph_run.bounding_rect().scaled(scale).translated(baseline_start).to_type<int>(),
    });
}

void DisplayListRecorder::add_clip_rect(Gfx::IntRect const& rect)
{
    APPEND(AddClipRect { rect });
}

void DisplayListRecorder::translate(Gfx::IntPoint delta)
{
    APPEND(Translate { delta });
}

void DisplayListRecorder::save()
{
    APPEND(Save {});
}

void DisplayListRecorder::save_layer()
{
    APPEND(SaveLayer {});
}

void DisplayListRecorder::restore()
{
    APPEND(Restore {});
}

void DisplayListRecorder::push_scroll_frame_id(Optional<i32> id)
{
    m_scroll_frame_id_stack.append(id);
}

void DisplayListRecorder::pop_scroll_frame_id()
{
    (void)m_scroll_frame_id_stack.take_last();
}

void DisplayListRecorder::push_clip_frame(RefPtr<ClipFrame const> clip_frame)
{
    m_clip_frame_stack.append(clip_frame);
}

void DisplayListRecorder::pop_clip_frame()
{
    (void)m_clip_frame_stack.take_last();
}

void DisplayListRecorder::push_stacking_context(PushStackingContextParams params)
{
    APPEND(PushStackingContext {
        .opacity = params.opacity,
        .compositing_and_blending_operator = params.compositing_and_blending_operator,
        .isolate = params.isolate,
        .transform = params.transform,
        .clip_path = params.clip_path });
    m_clip_frame_stack.append({});
}

void DisplayListRecorder::pop_stacking_context()
{
    APPEND(PopStackingContext {});
    (void)m_clip_frame_stack.take_last();
}

void DisplayListRecorder::apply_backdrop_filter(Gfx::IntRect const& backdrop_region, BorderRadiiData const& border_radii_data, Gfx::Filter const& backdrop_filter)
{
    if (backdrop_region.is_empty())
        return;
    APPEND(ApplyBackdropFilter {
        .backdrop_region = backdrop_region,
        .border_radii_data = border_radii_data,
        .backdrop_filter = backdrop_filter,
    });
}

void DisplayListRecorder::paint_outer_box_shadow(PaintBoxShadowParams params)
{
    APPEND(PaintOuterBoxShadow { .box_shadow_params = params });
}

void DisplayListRecorder::paint_inner_box_shadow(PaintBoxShadowParams params)
{
    APPEND(PaintInnerBoxShadow { .box_shadow_params = params });
}

void DisplayListRecorder::paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const& glyph_run, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location)
{
    APPEND(PaintTextShadow {
        .glyph_run = glyph_run,
        .glyph_run_scale = glyph_run_scale,
        .shadow_bounding_rect = bounding_rect,
        .text_rect = text_rect,
        .draw_location = draw_location,
        .blur_radius = blur_radius,
        .color = color });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadii const& corner_radii)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;

    if (!corner_radii.has_any_radius()) {
        fill_rect(rect, color);
        return;
    }

    APPEND(FillRectWithRoundedCorners {
        .rect = rect,
        .color = color,
        .corner_radii = corner_radii,
    });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius)
{
    fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius);
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius)
{
    fill_rect_with_rounded_corners(a_rect, color,
        { { top_left_radius, top_left_radius },
            { top_right_radius, top_right_radius },
            { bottom_right_radius, bottom_right_radius },
            { bottom_left_radius, bottom_left_radius } });
}

void DisplayListRecorder::paint_scrollbar(int scroll_frame_id, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, CSSPixelFraction scroll_size, Color thumb_color, Color track_color, bool vertical)
{
    APPEND(PaintScrollBar {
        .scroll_frame_id = scroll_frame_id,
        .gutter_rect = gutter_rect,
        .thumb_rect = thumb_rect,
        .scroll_size = scroll_size,
        .thumb_color = thumb_color,
        .track_color = track_color,
        .vertical = vertical });
}

void DisplayListRecorder::apply_opacity(float opacity)
{
    APPEND(ApplyOpacity { .opacity = opacity });
}

void DisplayListRecorder::apply_compositing_and_blending_operator(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    APPEND(ApplyCompositeAndBlendingOperator { .compositing_and_blending_operator = compositing_and_blending_operator });
}

void DisplayListRecorder::apply_filter(Gfx::Filter filter)
{
    APPEND(ApplyFilter { .filter = move(filter) });
}

void DisplayListRecorder::apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 matrix)
{
    APPEND(ApplyTransform {
        .origin = origin,
        .matrix = matrix,
    });
}

void DisplayListRecorder::apply_mask_bitmap(Gfx::IntPoint origin, Gfx::ImmutableBitmap const& bitmap, Gfx::Bitmap::MaskKind kind)
{
    APPEND(ApplyMaskBitmap {
        .origin = origin,
        .bitmap = bitmap,
        .kind = kind,
    });
}

}
