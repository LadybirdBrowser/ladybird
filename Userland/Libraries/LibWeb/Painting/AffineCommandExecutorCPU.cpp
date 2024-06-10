/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/AffineCommandExecutorCPU.h>

namespace Web::Painting {

// This executor is hopes to handle (at least) 2D CSS transforms. All commands
// implemented here are required to support affine transformations, if that is
// not possible the implementation should say in CommandExecutorCPU. Note: The
// transform can be assumed to be non-identity or translation, so there's no
// need to add fast paths here (those will be handled in the normal executor).

static Gfx::Path rect_path(Gfx::FloatRect const& rect)
{
    Gfx::Path path;
    path.move_to({ rect.x(), rect.y() });
    path.line_to({ rect.x() + rect.width(), rect.y() });
    path.line_to({ rect.x() + rect.width(), rect.y() + rect.height() });
    path.line_to({ rect.x(), rect.y() + rect.height() });
    path.close();
    return path;
}

AffineCommandExecutorCPU::AffineCommandExecutorCPU(Gfx::Bitmap& bitmap, Gfx::AffineTransform transform, Gfx::IntRect clip)
    : m_painter(bitmap)
{
    auto clip_quad = Gfx::AffineTransform {}.map_to_quad(clip.to_type<float>());
    m_stacking_contexts.append(StackingContext { transform, clip_quad, clip_quad.bounding_rect() });
}

CommandResult AffineCommandExecutorCPU::draw_glyph_run(DrawGlyphRun const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_text(DrawText const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::fill_rect(FillRect const& command)
{
    // FIXME: Somehow support clip_paths?
    auto path = rect_path(command.rect.to_type<float>()).copy_transformed(stacking_context().transform);
    aa_painter().fill_path(path, command.color, Gfx::WindingRule::EvenOdd);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_scaled_bitmap(DrawScaledBitmap const& command)
{
    m_painter.draw_scaled_bitmap_with_transform(command.dst_rect, command.bitmap, command.src_rect.to_type<float>(), stacking_context().transform, 1.0f, command.scaling_mode);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const& command)
{
    m_painter.draw_scaled_bitmap_with_transform(command.dst_rect, command.bitmap->bitmap(), command.src_rect.to_type<float>(), stacking_context().transform, 1.0f, command.scaling_mode);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::save(Save const&)
{
    m_painter.save();
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::restore(Restore const&)
{
    m_painter.restore();
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::add_clip_rect(AddClipRect const&)
{
    // FIXME: Implement. The plan here is to implement https://en.wikipedia.org/wiki/Sutherland%E2%80%93Hodgman_algorithm
    // within the rasterizer (which should work as the clip quadrilateral will always be convex).
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::push_stacking_context(PushStackingContext const& command)
{
    // FIXME: Support opacity.
    // FIXME: Support masks.
    // Note: Image rendering is not relevant as this does not transform via a bitmap.
    // Note: `position: fixed` does not apply when CSS transforms are involved.

    // FIXME: Attempt to support 3D transforms... Somehow?
    auto affine_transform = Gfx::extract_2d_affine_transform(command.transform.matrix);
    auto new_transform = Gfx::AffineTransform {}
                             .set_translation(command.post_transform_translation.to_type<float>())
                             .translate(command.transform.origin)
                             .multiply(affine_transform)
                             .translate(-command.transform.origin);

    auto const& current_stacking_context = stacking_context();
    m_stacking_contexts.append(StackingContext {
        .transform = Gfx::AffineTransform(current_stacking_context.transform).multiply(new_transform),
        .clip = current_stacking_context.clip,
        .clip_bounds = current_stacking_context.clip_bounds });
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::pop_stacking_context(PopStackingContext const&)
{
    m_stacking_contexts.take_last();
    if (m_stacking_contexts.size() == 0)
        return CommandResult::ContinueWithParentExecutor;
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::paint_linear_gradient(PaintLinearGradient const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::paint_outer_box_shadow(PaintOuterBoxShadow const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::paint_inner_box_shadow(PaintInnerBoxShadow const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::paint_text_shadow(PaintTextShadow const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::fill_rect_with_rounded_corners(FillRectWithRoundedCorners const& command)
{
    Gfx::Path path;

    auto x = command.rect.x();
    auto y = command.rect.y();
    auto width = command.rect.width();
    auto height = command.rect.height();

    if (command.top_left_radius)
        path.move_to({ x + command.top_left_radius.horizontal_radius, y });
    else
        path.move_to({ x, y });

    if (command.top_right_radius) {
        path.horizontal_line_to(x + width - command.top_right_radius.horizontal_radius);
        path.elliptical_arc_to({ x + width, y + command.top_right_radius.horizontal_radius }, { command.top_right_radius.horizontal_radius, command.top_right_radius.vertical_radius }, 0, false, true);
    } else {
        path.horizontal_line_to(x + width);
    }

    if (command.bottom_right_radius) {
        path.vertical_line_to(y + height - command.bottom_right_radius.vertical_radius);
        path.elliptical_arc_to({ x + width - command.bottom_right_radius.horizontal_radius, y + height }, { command.bottom_right_radius.horizontal_radius, command.bottom_right_radius.vertical_radius }, 0, false, true);
    } else {
        path.vertical_line_to(y + height);
    }

    if (command.bottom_left_radius) {
        path.horizontal_line_to(x + command.bottom_left_radius.horizontal_radius);
        path.elliptical_arc_to({ x, y + height - command.bottom_left_radius.vertical_radius }, { command.bottom_left_radius.horizontal_radius, command.bottom_left_radius.vertical_radius }, 0, false, true);
    } else {
        path.horizontal_line_to(x);
    }

    if (command.top_left_radius) {
        path.vertical_line_to(y + command.top_left_radius.vertical_radius);
        path.elliptical_arc_to({ x + command.top_left_radius.horizontal_radius, y }, { command.top_left_radius.horizontal_radius, command.top_left_radius.vertical_radius }, 0, false, true);
    } else {
        path.vertical_line_to(y);
    }

    path = path.copy_transformed(stacking_context().transform);
    aa_painter().fill_path(path, command.color, Gfx::WindingRule::EvenOdd);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::fill_path_using_color(FillPathUsingColor const& command)
{
    auto path_transform = Gfx::AffineTransform(stacking_context().transform).multiply(Gfx::AffineTransform {}.set_translation(command.aa_translation));
    aa_painter().fill_path(command.path.copy_transformed(path_transform), command.color, command.winding_rule);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::fill_path_using_paint_style(FillPathUsingPaintStyle const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::stroke_path_using_color(StrokePathUsingColor const& command)
{
    auto path_transform = Gfx::AffineTransform(stacking_context().transform).multiply(Gfx::AffineTransform {}.set_translation(command.aa_translation));
    aa_painter().stroke_path(command.path.copy_transformed(path_transform), command.color, command.thickness);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::stroke_path_using_paint_style(StrokePathUsingPaintStyle const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_ellipse(DrawEllipse const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::fill_ellipse(FillEllipse const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_line(DrawLine const& command)
{
    // FIXME: Implement other line styles.
    Gfx::Path path;
    path.move_to(command.from.to_type<float>());
    path.line_to(command.to.to_type<float>());
    aa_painter().stroke_path(path, command.color, command.thickness);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::apply_backdrop_filter(ApplyBackdropFilter const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_rect(DrawRect const& command)
{
    auto path = rect_path(command.rect.to_type<float>()).copy_transformed(stacking_context().transform);
    aa_painter().stroke_path(path, command.color, 1);
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::paint_radial_gradient(PaintRadialGradient const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::paint_conic_gradient(PaintConicGradient const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::draw_triangle_wave(DrawTriangleWave const&)
{
    // FIXME: Implement.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::sample_under_corners(SampleUnderCorners const&)
{
    // FIXME: Implement? -- Likely not a good approach for transforms.
    return CommandResult::Continue;
}

CommandResult AffineCommandExecutorCPU::blit_corner_clipping(BlitCornerClipping const&)
{
    // FIXME: Implement? -- Likely not a good approach for transforms.
    return CommandResult::Continue;
}

bool AffineCommandExecutorCPU::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    auto const& current_stacking_context = stacking_context();
    auto transformed_rect = current_stacking_context.transform.map(rect.to_type<float>());
    return transformed_rect.intersected(current_stacking_context.clip_bounds).is_empty();
}

}
