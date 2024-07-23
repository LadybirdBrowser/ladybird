/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

DisplayListRecorder::DisplayListRecorder(DisplayList& command_list)
    : m_command_list(command_list)
{
    m_state_stack.append(State());
}

DisplayListRecorder::~DisplayListRecorder() = default;

void DisplayListRecorder::append(Command&& command)
{
    m_command_list.append(move(command), state().scroll_frame_id);
}

void DisplayListRecorder::add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    append(AddRoundedRectClip {
        corner_radii,
        border_rect = state().translation.map(border_rect),
        corner_clip });
}

void DisplayListRecorder::fill_rect(Gfx::IntRect const& rect, Color color, RefPtr<DisplayList> text_clip)
{
    if (rect.is_empty())
        return;
    append(FillRect {
        .rect = state().translation.map(rect),
        .color = color,
        .text_clip = move(text_clip),
    });
}

void DisplayListRecorder::fill_path(FillPathUsingColorParams params)
{
    auto aa_translation = state().translation.map(params.translation.value_or(Gfx::FloatPoint {}));
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation).to_type<int>();
    if (path_bounding_rect.is_empty())
        return;
    append(FillPathUsingColor {
        .path_bounding_rect = path_bounding_rect,
        .path = params.path,
        .color = params.color,
        .winding_rule = params.winding_rule,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::fill_path(FillPathUsingPaintStyleParams params)
{
    auto aa_translation = state().translation.map(params.translation.value_or(Gfx::FloatPoint {}));
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation).to_type<int>();
    if (path_bounding_rect.is_empty())
        return;
    append(FillPathUsingPaintStyle {
        .path_bounding_rect = path_bounding_rect,
        .path = params.path,
        .paint_style = params.paint_style,
        .winding_rule = params.winding_rule,
        .opacity = params.opacity,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::stroke_path(StrokePathUsingColorParams params)
{
    auto aa_translation = state().translation.map(params.translation.value_or(Gfx::FloatPoint {}));
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation).to_type<int>();
    // Increase path bounding box by `thickness` to account for stroke.
    path_bounding_rect.inflate(params.thickness, params.thickness);
    if (path_bounding_rect.is_empty())
        return;
    append(StrokePathUsingColor {
        .path_bounding_rect = path_bounding_rect,
        .path = params.path,
        .color = params.color,
        .thickness = params.thickness,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::stroke_path(StrokePathUsingPaintStyleParams params)
{
    auto aa_translation = state().translation.map(params.translation.value_or(Gfx::FloatPoint {}));
    auto path_bounding_rect = params.path.bounding_box().translated(aa_translation).to_type<int>();
    // Increase path bounding box by `thickness` to account for stroke.
    path_bounding_rect.inflate(params.thickness, params.thickness);
    if (path_bounding_rect.is_empty())
        return;
    append(StrokePathUsingPaintStyle {
        .path_bounding_rect = path_bounding_rect,
        .path = params.path,
        .paint_style = params.paint_style,
        .thickness = params.thickness,
        .opacity = params.opacity,
        .aa_translation = aa_translation,
    });
}

void DisplayListRecorder::draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness)
{
    if (a_rect.is_empty())
        return;
    append(DrawEllipse {
        .rect = state().translation.map(a_rect),
        .color = color,
        .thickness = thickness,
    });
}

void DisplayListRecorder::fill_ellipse(Gfx::IntRect const& a_rect, Color color)
{
    if (a_rect.is_empty())
        return;
    append(FillEllipse {
        .rect = state().translation.map(a_rect),
        .color = color,
    });
}

void DisplayListRecorder::fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data, RefPtr<DisplayList> text_clip)
{
    if (gradient_rect.is_empty())
        return;
    append(PaintLinearGradient {
        .gradient_rect = state().translation.map(gradient_rect),
        .linear_gradient_data = data,
        .text_clip = move(text_clip) });
}

void DisplayListRecorder::fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position, RefPtr<DisplayList> text_clip)
{
    if (rect.is_empty())
        return;
    append(PaintConicGradient {
        .rect = state().translation.map(rect),
        .conic_gradient_data = data,
        .position = position,
        .text_clip = move(text_clip) });
}

void DisplayListRecorder::fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size, RefPtr<DisplayList> text_clip)
{
    if (rect.is_empty())
        return;
    append(PaintRadialGradient {
        .rect = state().translation.map(rect),
        .radial_gradient_data = data,
        .center = center,
        .size = size,
        .text_clip = move(text_clip) });
}

void DisplayListRecorder::draw_rect(Gfx::IntRect const& rect, Color color, bool rough)
{
    if (rect.is_empty())
        return;
    append(DrawRect {
        .rect = state().translation.map(rect),
        .color = color,
        .rough = rough });
}

void DisplayListRecorder::draw_scaled_bitmap(Gfx::IntRect const& dst_rect, Gfx::Bitmap const& bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    append(DrawScaledBitmap {
        .dst_rect = state().translation.map(dst_rect),
        .bitmap = bitmap,
        .src_rect = src_rect,
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::ImmutableBitmap const& bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, RefPtr<DisplayList> text_clip)
{
    if (dst_rect.is_empty())
        return;
    append(DrawScaledImmutableBitmap {
        .dst_rect = state().translation.map(dst_rect),
        .bitmap = bitmap,
        .src_rect = src_rect,
        .scaling_mode = scaling_mode,
        .text_clip = move(text_clip),
    });
}

void DisplayListRecorder::draw_repeated_immutable_bitmap(Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, NonnullRefPtr<Gfx::ImmutableBitmap> bitmap, Gfx::ScalingMode scaling_mode, DrawRepeatedImmutableBitmap::Repeat repeat, RefPtr<DisplayList> text_clip)
{
    append(DrawRepeatedImmutableBitmap {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .bitmap = move(bitmap),
        .scaling_mode = scaling_mode,
        .repeat = repeat,
        .text_clip = move(text_clip),
    });
}

void DisplayListRecorder::draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness, Gfx::LineStyle style, Color alternate_color)
{
    append(DrawLine {
        .color = color,
        .from = state().translation.map(from),
        .to = state().translation.map(to),
        .thickness = thickness,
        .style = style,
        .alternate_color = alternate_color,
    });
}

void DisplayListRecorder::draw_text(Gfx::IntRect const& rect, String raw_text, Gfx::Font const& font, Gfx::TextAlignment alignment, Color color)
{
    if (rect.is_empty())
        return;

    auto glyph_run = adopt_ref(*new Gfx::GlyphRun({}, font));
    float glyph_run_width = 0;
    Gfx::for_each_glyph_position(
        { 0, 0 }, raw_text.code_points(), font, [&](Gfx::DrawGlyphOrEmoji const& glyph_or_emoji) {
            glyph_run->append(glyph_or_emoji);
            return IterationDecision::Continue;
        },
        Gfx::IncludeLeftBearing::No, glyph_run_width);

    float baseline_x = 0;
    if (alignment == Gfx::TextAlignment::CenterLeft) {
        baseline_x = rect.x();
    } else if (alignment == Gfx::TextAlignment::Center) {
        baseline_x = static_cast<float>(rect.x()) + (static_cast<float>(rect.width()) - glyph_run_width) / 2.0f;
    } else if (alignment == Gfx::TextAlignment::CenterRight) {
        baseline_x = static_cast<float>(rect.right()) - glyph_run_width;
    } else {
        // Unimplemented alignment.
        TODO();
    }
    auto metrics = font.pixel_metrics();
    float baseline_y = static_cast<float>(rect.y()) + metrics.ascent + (static_cast<float>(rect.height()) - (metrics.ascent + metrics.descent)) / 2.0f;
    draw_text_run(Gfx::IntPoint(roundf(baseline_x), roundf(baseline_y)), *glyph_run, color, rect, 1.0);
}

void DisplayListRecorder::draw_text_run(Gfx::IntPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale)
{
    if (rect.is_empty())
        return;
    auto transformed_baseline_start = state().translation.map(baseline_start).to_type<float>();
    append(DrawGlyphRun {
        .glyph_run = glyph_run,
        .color = color,
        .rect = state().translation.map(rect),
        .translation = transformed_baseline_start,
        .scale = scale,
    });
}

void DisplayListRecorder::add_clip_rect(Gfx::IntRect const& rect)
{
    append(AddClipRect { .rect = state().translation.map(rect) });
}

void DisplayListRecorder::translate(int dx, int dy)
{
    m_state_stack.last().translation.translate(dx, dy);
}

void DisplayListRecorder::translate(Gfx::IntPoint delta)
{
    m_state_stack.last().translation.translate(delta.to_type<float>());
}

void DisplayListRecorder::save()
{
    append(Save {});
    m_state_stack.append(m_state_stack.last());
}

void DisplayListRecorder::restore()
{
    append(Restore {});

    VERIFY(m_state_stack.size() > 1);
    m_state_stack.take_last();
}

void DisplayListRecorder::push_stacking_context(PushStackingContextParams params)
{
    append(PushStackingContext {
        .opacity = params.opacity,
        .is_fixed_position = params.is_fixed_position,
        .source_paintable_rect = params.source_paintable_rect,
        // No translations apply to fixed-position stacking contexts.
        .post_transform_translation = params.is_fixed_position
            ? Gfx::IntPoint {}
            : state().translation.translation().to_rounded<int>(),
        .image_rendering = params.image_rendering,
        .transform = {
            .origin = params.transform.origin,
            .matrix = params.transform.matrix,
        },
        .mask = move(params.mask) });
    m_state_stack.append(State());
}

void DisplayListRecorder::pop_stacking_context()
{
    m_state_stack.take_last();
    append(PopStackingContext {});
}

void DisplayListRecorder::apply_backdrop_filter(Gfx::IntRect const& backdrop_region, BorderRadiiData const& border_radii_data, CSS::ResolvedBackdropFilter const& backdrop_filter)
{
    if (backdrop_region.is_empty())
        return;
    append(ApplyBackdropFilter {
        .backdrop_region = state().translation.map(backdrop_region),
        .border_radii_data = border_radii_data,
        .backdrop_filter = backdrop_filter,
    });
}

void DisplayListRecorder::paint_outer_box_shadow_params(PaintBoxShadowParams params)
{
    params.device_content_rect = state().translation.map(params.device_content_rect);
    append(PaintOuterBoxShadow { .box_shadow_params = params });
}

void DisplayListRecorder::paint_inner_box_shadow_params(PaintBoxShadowParams params)
{
    append(PaintInnerBoxShadow { .box_shadow_params = params });
}

void DisplayListRecorder::paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const& glyph_run, double glyph_run_scale, Color color, Gfx::IntPoint draw_location)
{
    append(PaintTextShadow {
        .blur_radius = blur_radius,
        .shadow_bounding_rect = bounding_rect,
        .text_rect = text_rect,
        .glyph_run = glyph_run,
        .glyph_run_scale = glyph_run_scale,
        .color = color,
        .draw_location = state().translation.map(draw_location) });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, Gfx::AntiAliasingPainter::CornerRadius top_left_radius, Gfx::AntiAliasingPainter::CornerRadius top_right_radius, Gfx::AntiAliasingPainter::CornerRadius bottom_right_radius, Gfx::AntiAliasingPainter::CornerRadius bottom_left_radius, RefPtr<DisplayList> text_clip)
{
    if (rect.is_empty())
        return;

    if (!top_left_radius && !top_right_radius && !bottom_right_radius && !bottom_left_radius) {
        fill_rect(rect, color, text_clip);
        return;
    }

    append(FillRectWithRoundedCorners {
        .rect = state().translation.map(rect),
        .color = color,
        .corner_radii = {
            .top_left = top_left_radius,
            .top_right = top_right_radius,
            .bottom_right = bottom_right_radius,
            .bottom_left = bottom_left_radius,
        },
        .text_clip = text_clip,
    });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius, RefPtr<DisplayList> text_clip)
{
    if (a_rect.is_empty())
        return;
    fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius, move(text_clip));
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius, RefPtr<DisplayList> text_clip)
{
    if (a_rect.is_empty())
        return;
    fill_rect_with_rounded_corners(a_rect, color,
        { top_left_radius, top_left_radius },
        { top_right_radius, top_right_radius },
        { bottom_right_radius, bottom_right_radius },
        { bottom_left_radius, bottom_left_radius },
        move(text_clip));
}

void DisplayListRecorder::draw_triangle_wave(Gfx::IntPoint a_p1, Gfx::IntPoint a_p2, Color color, int amplitude, int thickness = 1)
{
    append(DrawTriangleWave {
        .p1 = state().translation.map(a_p1),
        .p2 = state().translation.map(a_p2),
        .color = color,
        .amplitude = amplitude,
        .thickness = thickness });
}

}
