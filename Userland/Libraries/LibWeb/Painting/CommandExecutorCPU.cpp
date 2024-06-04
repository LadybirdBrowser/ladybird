/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filters/StackBlurFilter.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/CommandExecutorCPU.h>
#include <LibWeb/Painting/FilterPainting.h>
#include <LibWeb/Painting/RecordingPainter.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

CommandExecutorCPU::CommandExecutorCPU(Gfx::Bitmap& bitmap, bool enable_affine_command_executor)
    : m_target_bitmap(bitmap)
    , m_enable_affine_command_executor(enable_affine_command_executor)
{
    stacking_contexts.append({ .painter = AK::make<Gfx::Painter>(bitmap),
        .opacity = 1.0f,
        .destination = {},
        .scaling_mode = {} });
}

CommandResult CommandExecutorCPU::draw_glyph_run(DrawGlyphRun const& command)
{
    auto& painter = this->painter();
    auto const& glyphs = command.glyph_run->glyphs();
    for (auto& glyph_or_emoji : glyphs) {
        auto transformed_glyph = glyph_or_emoji;
        transformed_glyph.visit([&](auto& glyph) {
            glyph.position = glyph.position.scaled(command.scale).translated(command.translation);
            glyph.font = glyph.font->with_size(glyph.font->point_size() * static_cast<float>(command.scale));
        });
        if (glyph_or_emoji.has<Gfx::DrawGlyph>()) {
            auto& glyph = transformed_glyph.get<Gfx::DrawGlyph>();
            painter.draw_glyph(glyph.position, glyph.code_point, *glyph.font, command.color);
        } else {
            auto& emoji = transformed_glyph.get<Gfx::DrawEmoji>();
            painter.draw_emoji(emoji.position.to_type<int>(), *emoji.emoji, *emoji.font);
        }
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_text(DrawText const& command)
{
    auto& painter = this->painter();
    painter.draw_text(command.rect, command.raw_text, command.font, command.alignment, command.color, command.elision, command.wrapping);
    return CommandResult::Continue;
}

template<typename Callback>
void apply_clip_paths_to_painter(Gfx::IntRect const& rect, Callback callback, Vector<Gfx::Path> const& clip_paths, Gfx::Painter& target_painter)
{
    // Setup a painter for a background canvas that we will paint to first.
    auto background_canvas = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size()).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*background_canvas);

    // Offset the painter to paint in the correct location.
    painter.translate(-rect.location());

    // Paint the background canvas.
    callback(painter);

    // Apply the clip path to the target painter.
    Gfx::AntiAliasingPainter aa_painter(target_painter);
    for (auto const& clip_path : clip_paths) {
        auto fill_offset = clip_path.bounding_box().location().to_type<int>() - rect.location();
        auto paint_style = Gfx::BitmapPaintStyle::create(*background_canvas, fill_offset).release_value_but_fixme_should_propagate_errors();
        aa_painter.fill_path(clip_path, paint_style);
    }
}

CommandResult CommandExecutorCPU::fill_rect(FillRect const& command)
{
    auto paint_op = [&](Gfx::Painter& painter) {
        painter.fill_rect(command.rect, command.color);
    };
    if (command.clip_paths.is_empty()) {
        paint_op(painter());
    } else {
        apply_clip_paths_to_painter(command.rect, paint_op, command.clip_paths, painter());
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_scaled_bitmap(DrawScaledBitmap const& command)
{
    auto& painter = this->painter();
    painter.draw_scaled_bitmap(command.dst_rect, command.bitmap, command.src_rect, 1, command.scaling_mode);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const& command)
{
    auto paint_op = [&](Gfx::Painter& painter) {
        painter.draw_scaled_bitmap(command.dst_rect, command.bitmap->bitmap(), command.src_rect, 1, command.scaling_mode);
    };
    if (command.clip_paths.is_empty()) {
        paint_op(painter());
    } else {
        apply_clip_paths_to_painter(command.dst_rect, paint_op, command.clip_paths, painter());
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::set_clip_rect(SetClipRect const& command)
{
    auto& painter = this->painter();
    painter.clear_clip_rect();
    painter.add_clip_rect(command.rect);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::clear_clip_rect(ClearClipRect const&)
{
    painter().clear_clip_rect();
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::push_stacking_context(PushStackingContext const& command)
{
    // FIXME: This extracts the affine 2D part of the full transformation matrix.
    // Use the whole matrix when we get better transformation support in LibGfx or use LibGL for drawing the bitmap
    auto affine_transform = Gfx::extract_2d_affine_transform(command.transform.matrix);

    if (m_enable_affine_command_executor && command.opacity == 1.0f && !affine_transform.is_identity_or_translation()) {
        auto offset = command.is_fixed_position ? Gfx::IntPoint {} : painter().translation();
        auto full_transform = Gfx::AffineTransform {}
                                  .set_translation((command.post_transform_translation + offset).to_type<float>())
                                  .translate(command.transform.origin)
                                  .multiply(affine_transform)
                                  .translate(-command.transform.origin);
        m_affine_command_executor = AffineCommandExecutorCPU(m_target_bitmap, full_transform, painter().clip_rect());
        return CommandResult::ContinueWithNestedExecutor;
    }

    painter().save();
    if (command.is_fixed_position)
        painter().translate(-painter().translation());

    if (command.mask.has_value()) {
        // TODO: Support masks and other stacking context features at the same time.
        // Note: Currently only SVG masking is implemented (which does not use CSS transforms anyway).
        auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, command.mask->mask_bitmap->size());
        if (bitmap_or_error.is_error())
            return CommandResult::Continue;
        auto bitmap = bitmap_or_error.release_value();
        stacking_contexts.append(StackingContext {
            .painter = AK::make<Gfx::Painter>(bitmap),
            .opacity = 1,
            .destination = command.source_paintable_rect.translated(command.post_transform_translation),
            .scaling_mode = Gfx::Painter::ScalingMode::None,
            .mask = command.mask });
        painter().translate(-command.source_paintable_rect.location());
        return CommandResult::Continue;
    }

    if (command.opacity == 1.0f && affine_transform.is_identity_or_translation()) {
        // OPTIMIZATION: This is a simple translation use previous stacking context's painter.
        painter().translate(affine_transform.translation().to_rounded<int>() + command.post_transform_translation);
        stacking_contexts.append(StackingContext {
            .painter = MaybeOwned(painter()),
            .opacity = 1,
            .destination = {},
            .scaling_mode = {} });
        return CommandResult::Continue;
    }

    auto& current_painter = this->painter();
    auto source_rect = command.source_paintable_rect.to_type<float>().translated(-command.transform.origin);
    auto transformed_destination_rect = affine_transform.map(source_rect).translated(command.transform.origin);
    auto destination_rect = transformed_destination_rect.to_rounded<int>();

    // FIXME: We should find a way to scale the paintable, rather than paint into a separate bitmap,
    // then scale it. This snippet now copies the background at the destination, then scales it down/up
    // to the size of the source (which could add some artefacts, though just scaling the bitmap already does that).
    // We need to copy the background at the destination because a bunch of our rendering effects now rely on
    // being able to sample the painter (see border radii, shadows, filters, etc).
    Gfx::FloatPoint destination_clipped_fixup {};
    auto try_get_scaled_destination_bitmap = [&]() -> ErrorOr<NonnullRefPtr<Gfx::Bitmap>> {
        Gfx::IntRect actual_destination_rect;
        auto bitmap = TRY(current_painter.get_region_bitmap(destination_rect, Gfx::BitmapFormat::BGRA8888, actual_destination_rect));
        // get_region_bitmap() may clip to a smaller region if the requested rect goes outside the painter, so we need to account for that.
        destination_clipped_fixup = Gfx::FloatPoint { destination_rect.location() - actual_destination_rect.location() };
        destination_rect = actual_destination_rect;
        if (source_rect.size() != transformed_destination_rect.size()) {
            auto sx = static_cast<float>(source_rect.width()) / transformed_destination_rect.width();
            auto sy = static_cast<float>(source_rect.height()) / transformed_destination_rect.height();
            bitmap = TRY(bitmap->scaled(sx, sy));
            destination_clipped_fixup.scale_by(sx, sy);
        }
        return bitmap;
    };

    auto bitmap_or_error = try_get_scaled_destination_bitmap();
    if (bitmap_or_error.is_error()) {
        // NOTE: If the creation of the bitmap fails, we need to skip all painting commands that belong to this stacking context.
        //       We don't interrupt the execution of painting commands because get_region_bitmap() returns an error if the requested
        //       region is outside of the viewport (mmap fails to allocate a zero-size region), which means we can safely proceed
        //       with execution of commands outside of this stacking context.
        // FIXME: Change the get_region_bitmap() API to return ErrorOr<Optional<Bitmap>> and exit the execution of commands here
        //        if we run out of memory.
        painter().restore();
        return CommandResult::SkipStackingContext;
    }

    auto bitmap = bitmap_or_error.release_value();
    stacking_contexts.append(StackingContext {
        .painter = AK::make<Gfx::Painter>(bitmap),
        .opacity = command.opacity,
        .destination = destination_rect.translated(command.post_transform_translation),
        .scaling_mode = CSS::to_gfx_scaling_mode(command.image_rendering, destination_rect, destination_rect) });
    painter().translate(-command.source_paintable_rect.location() + destination_clipped_fixup.to_type<int>());

    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::pop_stacking_context(PopStackingContext const&)
{
    ScopeGuard restore_painter = [&] {
        painter().restore();
    };
    auto stacking_context = stacking_contexts.take_last();
    // Stacking contexts that don't own their painter are simple translations, and don't need to blit anything back.
    if (stacking_context.painter.is_owned()) {
        auto bitmap = stacking_context.painter->target();
        if (stacking_context.mask.has_value())
            bitmap->apply_mask(*stacking_context.mask->mask_bitmap, stacking_context.mask->mask_kind);
        auto destination_rect = stacking_context.destination;
        if (destination_rect.size() == bitmap->size()) {
            painter().blit(destination_rect.location(), *bitmap, bitmap->rect(), stacking_context.opacity);
        } else {
            painter().draw_scaled_bitmap(destination_rect, *bitmap, bitmap->rect(), stacking_context.opacity, stacking_context.scaling_mode);
        }
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::paint_linear_gradient(PaintLinearGradient const& command)
{
    auto const& linear_gradient_data = command.linear_gradient_data;
    auto paint_op = [&](Gfx::Painter& painter) {
        painter.fill_rect_with_linear_gradient(
            command.gradient_rect, linear_gradient_data.color_stops.list,
            linear_gradient_data.gradient_angle, linear_gradient_data.color_stops.repeat_length);
    };
    if (command.clip_paths.is_empty()) {
        paint_op(painter());
    } else {
        apply_clip_paths_to_painter(command.gradient_rect, paint_op, command.clip_paths, painter());
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::paint_outer_box_shadow(PaintOuterBoxShadow const& command)
{
    auto& painter = this->painter();
    Web::Painting::paint_outer_box_shadow(painter, command.outer_box_shadow_params);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::paint_inner_box_shadow(PaintInnerBoxShadow const& command)
{
    auto& painter = this->painter();
    Web::Painting::paint_inner_box_shadow(painter, command.outer_box_shadow_params);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::paint_text_shadow(PaintTextShadow const& command)
{
    // FIXME: Figure out the maximum bitmap size for all shadows and then allocate it once and reuse it?
    auto maybe_shadow_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, command.shadow_bounding_rect.size());
    if (maybe_shadow_bitmap.is_error()) {
        dbgln("Unable to allocate temporary bitmap {} for text-shadow rendering: {}", command.shadow_bounding_rect.size(), maybe_shadow_bitmap.error());
        return CommandResult::Continue;
    }
    auto shadow_bitmap = maybe_shadow_bitmap.release_value();

    Gfx::Painter shadow_painter { *shadow_bitmap };
    // FIXME: "Spread" the shadow somehow.
    Gfx::IntPoint const baseline_start(command.text_rect.x(), command.text_rect.y() + command.fragment_baseline);
    shadow_painter.translate(baseline_start);
    for (auto const& glyph_or_emoji : command.glyph_run) {
        if (glyph_or_emoji.has<Gfx::DrawGlyph>()) {
            auto const& glyph = glyph_or_emoji.get<Gfx::DrawGlyph>();
            shadow_painter.draw_glyph(glyph.position, glyph.code_point, *glyph.font, command.color);
        } else {
            auto const& emoji = glyph_or_emoji.get<Gfx::DrawEmoji>();
            shadow_painter.draw_emoji(emoji.position.to_type<int>(), *emoji.emoji, *emoji.font);
        }
    }

    // Blur
    Gfx::StackBlurFilter filter(*shadow_bitmap);
    filter.process_rgba(command.blur_radius, command.color);

    painter().blit(command.draw_location, *shadow_bitmap, command.shadow_bounding_rect);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::fill_rect_with_rounded_corners(FillRectWithRoundedCorners const& command)
{
    auto paint_op = [&](Gfx::Painter& painter) {
        Gfx::AntiAliasingPainter aa_painter(painter);
        aa_painter.fill_rect_with_rounded_corners(
            command.rect,
            command.color,
            command.top_left_radius,
            command.top_right_radius,
            command.bottom_right_radius,
            command.bottom_left_radius);
    };
    if (command.clip_paths.is_empty()) {
        paint_op(painter());
    } else {
        apply_clip_paths_to_painter(command.rect, paint_op, command.clip_paths, painter());
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::fill_path_using_color(FillPathUsingColor const& command)
{
    Gfx::AntiAliasingPainter aa_painter(painter());
    aa_painter.translate(command.aa_translation);
    aa_painter.fill_path(command.path, command.color, command.winding_rule);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::fill_path_using_paint_style(FillPathUsingPaintStyle const& command)
{
    Gfx::AntiAliasingPainter aa_painter(painter());
    aa_painter.translate(command.aa_translation);
    aa_painter.fill_path(command.path, command.paint_style, command.opacity, command.winding_rule);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::stroke_path_using_color(StrokePathUsingColor const& command)
{
    Gfx::AntiAliasingPainter aa_painter(painter());
    aa_painter.translate(command.aa_translation);
    aa_painter.stroke_path(command.path, command.color, command.thickness);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::stroke_path_using_paint_style(StrokePathUsingPaintStyle const& command)
{
    Gfx::AntiAliasingPainter aa_painter(painter());
    aa_painter.translate(command.aa_translation);
    aa_painter.stroke_path(command.path, command.paint_style, command.thickness, command.opacity);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_ellipse(DrawEllipse const& command)
{
    Gfx::AntiAliasingPainter aa_painter(painter());
    aa_painter.draw_ellipse(command.rect, command.color, command.thickness);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::fill_ellipse(FillEllipse const& command)
{
    Gfx::AntiAliasingPainter aa_painter(painter());
    aa_painter.fill_ellipse(command.rect, command.color);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_line(DrawLine const& command)
{
    if (command.style == Gfx::Painter::LineStyle::Dotted) {
        Gfx::AntiAliasingPainter aa_painter(painter());
        aa_painter.draw_line(command.from, command.to, command.color, command.thickness, command.style, command.alternate_color);
    } else {
        painter().draw_line(command.from, command.to, command.color, command.thickness, command.style, command.alternate_color);
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_signed_distance_field(DrawSignedDistanceField const& command)
{
    painter().draw_signed_distance_field(command.rect, command.color, command.sdf, command.smoothing);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::apply_backdrop_filter(ApplyBackdropFilter const& command)
{
    auto& painter = this->painter();

    // This performs the backdrop filter operation: https://drafts.fxtf.org/filter-effects-2/#backdrop-filter-operation

    // Note: The region bitmap can be smaller than the backdrop_region if it's at the edge of canvas.
    // Note: This is in DevicePixels, but we use an IntRect because `get_region_bitmap()` below writes to it.

    // FIXME: Go through the steps to find the "Backdrop Root Image"
    // https://drafts.fxtf.org/filter-effects-2/#BackdropRoot

    // 1. Copy the Backdrop Root Image into a temporary buffer, such as a raster image. Call this buffer T’.
    Gfx::IntRect actual_region {};
    auto maybe_backdrop_bitmap = painter.get_region_bitmap(command.backdrop_region, Gfx::BitmapFormat::BGRA8888, actual_region);
    if (actual_region.is_empty())
        return CommandResult::Continue;
    if (maybe_backdrop_bitmap.is_error()) {
        dbgln("Failed get region bitmap for backdrop-filter");
        return CommandResult::Continue;
    }
    auto backdrop_bitmap = maybe_backdrop_bitmap.release_value();

    // 2. Apply the backdrop-filter’s filter operations to the entire contents of T'.
    apply_filter_list(*backdrop_bitmap, command.backdrop_filter.filters);

    // FIXME: 3. If element B has any transforms (between B and the Backdrop Root), apply the inverse of those transforms to the contents of T’.

    // 4. Apply a clip to the contents of T’, using the border box of element B, including border-radius if specified. Note that the children of B are not considered for the sizing or location of this clip.
    // FIXME: 5. Draw all of element B, including its background, border, and any children elements, into T’.
    // FXIME: 6. If element B has any transforms, effects, or clips, apply those to T’.

    // 7. Composite the contents of T’ into element B’s parent, using source-over compositing.
    painter.blit(actual_region.location(), *backdrop_bitmap, backdrop_bitmap->rect());
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_rect(DrawRect const& command)
{
    painter().draw_rect(command.rect, command.color, command.rough);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::paint_radial_gradient(PaintRadialGradient const& command)
{
    auto paint_op = [&](Gfx::Painter& painter) {
        painter.fill_rect_with_radial_gradient(command.rect, command.radial_gradient_data.color_stops.list, command.center, command.size, command.radial_gradient_data.color_stops.repeat_length);
    };
    if (command.clip_paths.is_empty()) {
        paint_op(painter());
    } else {
        apply_clip_paths_to_painter(command.rect, paint_op, command.clip_paths, painter());
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::paint_conic_gradient(PaintConicGradient const& command)
{
    auto paint_op = [&](Gfx::Painter& painter) {
        painter.fill_rect_with_conic_gradient(command.rect, command.conic_gradient_data.color_stops.list, command.position, command.conic_gradient_data.start_angle, command.conic_gradient_data.color_stops.repeat_length);
    };
    if (command.clip_paths.is_empty()) {
        paint_op(painter());
    } else {
        apply_clip_paths_to_painter(command.rect, paint_op, command.clip_paths, painter());
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::draw_triangle_wave(DrawTriangleWave const& command)
{
    painter().draw_triangle_wave(command.p1, command.p2, command.color, command.amplitude, command.thickness);
    return CommandResult::Continue;
}

void CommandExecutorCPU::prepare_to_execute(size_t corner_clip_max_depth)
{
    m_corner_clippers_stack.ensure_capacity(corner_clip_max_depth);
}

CommandResult CommandExecutorCPU::sample_under_corners(SampleUnderCorners const& command)
{
    auto clipper = BorderRadiusCornerClipper::create(command.corner_radii, command.border_rect.to_type<DevicePixels>(), command.corner_clip).release_value();
    clipper->sample_under_corners(painter());
    m_corner_clippers_stack.append(clipper);
    return CommandResult::Continue;
}

CommandResult CommandExecutorCPU::blit_corner_clipping(BlitCornerClipping const&)
{
    auto clipper = m_corner_clippers_stack.take_last();
    clipper->blit_corner_clipping(painter());
    return CommandResult::Continue;
}

bool CommandExecutorCPU::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    return !painter().clip_rect().intersects(rect.translated(painter().translation()));
}

}
