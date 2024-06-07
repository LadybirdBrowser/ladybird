/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/RecordingPainter.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

RecordingPainter::RecordingPainter(CommandList& command_list)
    : m_command_list(command_list)
{
    m_state_stack.append(State());
}

RecordingPainter::~RecordingPainter()
{
    VERIFY(m_corner_clip_state_stack.is_empty());
}

void RecordingPainter::append(Command&& command)
{
    m_command_list.append(move(command), state().scroll_frame_id);
}

void RecordingPainter::sample_under_corners(u32 id, CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    m_corner_clip_state_stack.append({ id, border_rect });
    if (m_corner_clip_state_stack.size() > commands_list().corner_clip_max_depth())
        commands_list().set_corner_clip_max_depth(m_corner_clip_state_stack.size());
    append(SampleUnderCorners {
        id,
        corner_radii,
        border_rect = state().translation.map(border_rect),
        corner_clip });
}

void RecordingPainter::blit_corner_clipping(u32 id)
{
    auto clip_state = m_corner_clip_state_stack.take_last();
    VERIFY(clip_state.id == id);
    append(BlitCornerClipping { id, state().translation.map(clip_state.rect) });
}

void RecordingPainter::fill_rect(Gfx::IntRect const& rect, Color color, Vector<Gfx::Path> const& clip_paths)
{
    if (rect.is_empty())
        return;
    append(FillRect {
        .rect = state().translation.map(rect),
        .color = color,
        .clip_paths = clip_paths,
    });
}

void RecordingPainter::fill_path(FillPathUsingColorParams params)
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

void RecordingPainter::fill_path(FillPathUsingPaintStyleParams params)
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

void RecordingPainter::stroke_path(StrokePathUsingColorParams params)
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

void RecordingPainter::stroke_path(StrokePathUsingPaintStyleParams params)
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

void RecordingPainter::draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness)
{
    if (a_rect.is_empty())
        return;
    append(DrawEllipse {
        .rect = state().translation.map(a_rect),
        .color = color,
        .thickness = thickness,
    });
}

void RecordingPainter::fill_ellipse(Gfx::IntRect const& a_rect, Color color)
{
    if (a_rect.is_empty())
        return;
    append(FillEllipse {
        .rect = state().translation.map(a_rect),
        .color = color,
    });
}

void RecordingPainter::fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data, Vector<Gfx::Path> const& clip_paths)
{
    if (gradient_rect.is_empty())
        return;
    append(PaintLinearGradient {
        .gradient_rect = state().translation.map(gradient_rect),
        .linear_gradient_data = data,
        .clip_paths = clip_paths });
}

void RecordingPainter::fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position, Vector<Gfx::Path> const& clip_paths)
{
    if (rect.is_empty())
        return;
    append(PaintConicGradient {
        .rect = state().translation.map(rect),
        .conic_gradient_data = data,
        .position = position,
        .clip_paths = clip_paths });
}

void RecordingPainter::fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size, Vector<Gfx::Path> const& clip_paths)
{
    if (rect.is_empty())
        return;
    append(PaintRadialGradient {
        .rect = state().translation.map(rect),
        .radial_gradient_data = data,
        .center = center,
        .size = size,
        .clip_paths = clip_paths });
}

void RecordingPainter::draw_rect(Gfx::IntRect const& rect, Color color, bool rough)
{
    if (rect.is_empty())
        return;
    append(DrawRect {
        .rect = state().translation.map(rect),
        .color = color,
        .rough = rough });
}

void RecordingPainter::draw_scaled_bitmap(Gfx::IntRect const& dst_rect, Gfx::Bitmap const& bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode)
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

void RecordingPainter::draw_scaled_immutable_bitmap(Gfx::IntRect const& dst_rect, Gfx::ImmutableBitmap const& bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Vector<Gfx::Path> const& clip_paths)
{
    if (dst_rect.is_empty())
        return;
    append(DrawScaledImmutableBitmap {
        .dst_rect = state().translation.map(dst_rect),
        .bitmap = bitmap,
        .src_rect = src_rect,
        .scaling_mode = scaling_mode,
        .clip_paths = clip_paths,
    });
}

void RecordingPainter::draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness, Gfx::LineStyle style, Color alternate_color)
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

void RecordingPainter::draw_text(Gfx::IntRect const& rect, String raw_text, Gfx::Font const& font, Gfx::TextAlignment alignment, Color color, Gfx::TextElision elision, Gfx::TextWrapping wrapping)
{
    if (rect.is_empty())
        return;
    append(DrawText {
        .rect = state().translation.map(rect),
        .raw_text = move(raw_text),
        .alignment = alignment,
        .color = color,
        .elision = elision,
        .wrapping = wrapping,
        .font = font,
    });
}

void RecordingPainter::draw_signed_distance_field(Gfx::IntRect const& dst_rect, Color color, Gfx::GrayscaleBitmap const& sdf, float smoothing)
{
    if (dst_rect.is_empty())
        return;
    append(DrawSignedDistanceField {
        .rect = state().translation.map(dst_rect),
        .color = color,
        .sdf = sdf,
        .smoothing = smoothing,
    });
}

void RecordingPainter::draw_text_run(Gfx::IntPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale)
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

void RecordingPainter::add_clip_rect(Gfx::IntRect const& rect)
{
    auto prev_clip_rect = state().clip_rect;
    if (!state().clip_rect.has_value()) {
        state().clip_rect = state().translation.map(rect);
    } else {
        state().clip_rect->intersect(state().translation.map(rect));
    }

    if (prev_clip_rect != state().clip_rect)
        append(SetClipRect { .rect = *state().clip_rect });
}

void RecordingPainter::translate(int dx, int dy)
{
    m_state_stack.last().translation.translate(dx, dy);
}

void RecordingPainter::translate(Gfx::IntPoint delta)
{
    m_state_stack.last().translation.translate(delta.to_type<float>());
}

void RecordingPainter::save()
{
    m_state_stack.append(m_state_stack.last());
}

void RecordingPainter::restore()
{
    auto prev_clip_rect = state().clip_rect;

    VERIFY(m_state_stack.size() > 1);
    m_state_stack.take_last();

    if (state().clip_rect != prev_clip_rect) {
        if (state().clip_rect.has_value())
            append(SetClipRect { .rect = *state().clip_rect });
        else
            append(ClearClipRect {});
    }
}

void RecordingPainter::push_stacking_context(PushStackingContextParams params)
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
        .mask = params.mask });
    m_state_stack.append(State());
}

void RecordingPainter::pop_stacking_context()
{
    m_state_stack.take_last();
    append(PopStackingContext {});
}

void RecordingPainter::apply_backdrop_filter(Gfx::IntRect const& backdrop_region, BorderRadiiData const& border_radii_data, CSS::ResolvedBackdropFilter const& backdrop_filter)
{
    if (backdrop_region.is_empty())
        return;
    append(ApplyBackdropFilter {
        .backdrop_region = state().translation.map(backdrop_region),
        .border_radii_data = border_radii_data,
        .backdrop_filter = backdrop_filter,
    });
}

void RecordingPainter::paint_outer_box_shadow_params(PaintBoxShadowParams params)
{
    params.device_content_rect = state().translation.map(params.device_content_rect);
    append(PaintOuterBoxShadow { .box_shadow_params = params });
}

void RecordingPainter::paint_inner_box_shadow_params(PaintBoxShadowParams params)
{
    append(PaintInnerBoxShadow { .box_shadow_params = params });
}

void RecordingPainter::paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Span<Gfx::DrawGlyphOrEmoji const> glyph_run, Color color, int fragment_baseline, Gfx::IntPoint draw_location)
{
    append(PaintTextShadow {
        .blur_radius = blur_radius,
        .shadow_bounding_rect = bounding_rect,
        .text_rect = text_rect,
        .glyph_run = Vector<Gfx::DrawGlyphOrEmoji> { glyph_run },
        .color = color,
        .fragment_baseline = fragment_baseline,
        .draw_location = state().translation.map(draw_location) });
}

void RecordingPainter::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, Gfx::AntiAliasingPainter::CornerRadius top_left_radius, Gfx::AntiAliasingPainter::CornerRadius top_right_radius, Gfx::AntiAliasingPainter::CornerRadius bottom_right_radius, Gfx::AntiAliasingPainter::CornerRadius bottom_left_radius, Vector<Gfx::Path> const& clip_paths)
{
    if (rect.is_empty())
        return;

    if (!top_left_radius && !top_right_radius && !bottom_right_radius && !bottom_left_radius) {
        fill_rect(rect, color, clip_paths);
        return;
    }

    append(FillRectWithRoundedCorners {
        .rect = state().translation.map(rect),
        .color = color,
        .top_left_radius = top_left_radius,
        .top_right_radius = top_right_radius,
        .bottom_left_radius = bottom_left_radius,
        .bottom_right_radius = bottom_right_radius,
        .clip_paths = clip_paths,
    });
}

void RecordingPainter::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius, Vector<Gfx::Path> const& clip_paths)
{
    if (a_rect.is_empty())
        return;
    fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius, clip_paths);
}

void RecordingPainter::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius, Vector<Gfx::Path> const& clip_paths)
{
    if (a_rect.is_empty())
        return;
    fill_rect_with_rounded_corners(a_rect, color,
        { top_left_radius, top_left_radius },
        { top_right_radius, top_right_radius },
        { bottom_right_radius, bottom_right_radius },
        { bottom_left_radius, bottom_left_radius },
        clip_paths);
}

void RecordingPainter::draw_triangle_wave(Gfx::IntPoint a_p1, Gfx::IntPoint a_p2, Color color, int amplitude, int thickness = 1)
{
    append(DrawTriangleWave {
        .p1 = state().translation.map(a_p1),
        .p2 = state().translation.map(a_p2),
        .color = color,
        .amplitude = amplitude,
        .thickness = thickness });
}

}
