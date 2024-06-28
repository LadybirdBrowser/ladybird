/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAccelGfx/GlyphAtlas.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListPlayerGPU.h>

namespace Web::Painting {

DisplayListPlayerGPU::DisplayListPlayerGPU(AccelGfx::Context& context, Gfx::Bitmap& bitmap)
    : m_target_bitmap(bitmap)
    , m_context(context)
{
    m_context.activate();
    auto canvas = AccelGfx::Canvas::create(bitmap.size());
    auto painter = AccelGfx::Painter::create(m_context, canvas);
    m_stacking_contexts.append({ .canvas = canvas,
        .painter = move(painter),
        .opacity = 1.0f,
        .destination = {},
        .transform = {} });
}

DisplayListPlayerGPU::~DisplayListPlayerGPU()
{
    m_context.activate();
    VERIFY(m_stacking_contexts.size() == 1);
    painter().flush(m_target_bitmap);
}

CommandResult DisplayListPlayerGPU::draw_glyph_run(DrawGlyphRun const& command)
{
    Vector<Gfx::DrawGlyphOrEmoji> transformed_glyph_run;
    auto const& glyphs = command.glyph_run->glyphs();
    transformed_glyph_run.ensure_capacity(glyphs.size());
    for (auto& glyph : glyphs) {
        auto transformed_glyph = glyph;
        transformed_glyph.visit([&](auto& glyph) {
            glyph.position = glyph.position.scaled(command.scale).translated(command.translation);
            glyph.font = glyph.font->with_size(glyph.font->point_size() * static_cast<float>(command.scale));
        });
        transformed_glyph_run.append(transformed_glyph);
    }
    painter().draw_glyph_run(transformed_glyph_run, command.color);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::fill_rect(FillRect const& command)
{
    // FIXME: Support clip paths
    painter().fill_rect(command.rect, command.color);
    return CommandResult::Continue;
}

static AccelGfx::Painter::ScalingMode to_accelgfx_scaling_mode(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::NearestNeighbor:
    case Gfx::ScalingMode::BoxSampling:
    case Gfx::ScalingMode::SmoothPixels:
    case Gfx::ScalingMode::None:
        return AccelGfx::Painter::ScalingMode::NearestNeighbor;
    case Gfx::ScalingMode::BilinearBlend:
        return AccelGfx::Painter::ScalingMode::Bilinear;
    default:
        VERIFY_NOT_REACHED();
    }
}

CommandResult DisplayListPlayerGPU::draw_scaled_bitmap(DrawScaledBitmap const& command)
{
    painter().draw_scaled_bitmap(command.dst_rect, command.bitmap, command.src_rect, to_accelgfx_scaling_mode(command.scaling_mode));
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const& command)
{
    // TODO: Support clip paths
    painter().draw_scaled_immutable_bitmap(command.dst_rect, command.bitmap, command.src_rect, to_accelgfx_scaling_mode(command.scaling_mode));
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::save(Save const&)
{
    painter().save();
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::restore(Restore const&)
{
    painter().restore();
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::add_clip_rect(AddClipRect const& command)
{
    painter().set_clip_rect(command.rect);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::push_stacking_context(PushStackingContext const& command)
{
    if (command.source_paintable_rect.is_empty())
        return CommandResult::SkipStackingContext;

    m_stacking_contexts.last().stacking_context_depth++;
    painter().save();
    if (command.is_fixed_position) {
        auto const& translation = painter().transform().translation();
        painter().translate(-translation);
    }

    auto stacking_context_transform = Gfx::extract_2d_affine_transform(command.transform.matrix);

    Gfx::AffineTransform inverse_origin_translation;
    inverse_origin_translation.translate(-command.transform.origin);
    Gfx::AffineTransform origin_translation;
    origin_translation.translate(command.transform.origin);

    Gfx::AffineTransform final_transform = origin_translation;
    final_transform.multiply(stacking_context_transform);
    final_transform.multiply(inverse_origin_translation);
    if (command.opacity < 1 || !stacking_context_transform.is_identity_or_translation()) {
        // If, due to layout mistakes, we encounter an excessively large rectangle here, it must be skipped to prevent
        // framebuffer allocation failure.
        if (command.source_paintable_rect.width() > 10000 || command.source_paintable_rect.height() > 10000) {
            dbgln("FIXME: Skipping stacking context with excessively large paintable rect: {}", command.source_paintable_rect);
            return CommandResult::SkipStackingContext;
        }

        auto canvas = AccelGfx::Canvas::create(command.source_paintable_rect.size());
        auto painter = AccelGfx::Painter::create(m_context, canvas);
        painter->translate(-command.source_paintable_rect.location().to_type<float>());
        painter->clear(Color::Transparent);
        m_stacking_contexts.append({ .canvas = canvas,
            .painter = move(painter),
            .opacity = command.opacity,
            .destination = command.source_paintable_rect,
            .transform = final_transform });
    } else {
        painter().translate(stacking_context_transform.translation() + command.post_transform_translation.to_type<float>());
        m_stacking_contexts.append({ .canvas = {},
            .painter = MaybeOwned(painter()),
            .opacity = command.opacity,
            .destination = {},
            .transform = final_transform });
    }
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::pop_stacking_context(PopStackingContext const&)
{
    auto stacking_context = m_stacking_contexts.take_last();
    VERIFY(stacking_context.stacking_context_depth == 0);
    if (stacking_context.painter.is_owned()) {
        painter().blit_canvas(stacking_context.destination, *stacking_context.canvas, stacking_context.opacity, stacking_context.transform);
    }
    painter().restore();
    m_stacking_contexts.last().stacking_context_depth--;
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::paint_linear_gradient(PaintLinearGradient const& command)
{
    // FIXME: Support clip paths
    auto const& linear_gradient_data = command.linear_gradient_data;
    painter().fill_rect_with_linear_gradient(command.gradient_rect, linear_gradient_data.color_stops.list, linear_gradient_data.gradient_angle, linear_gradient_data.color_stops.repeat_length);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::paint_outer_box_shadow(PaintOuterBoxShadow const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::paint_inner_box_shadow(PaintInnerBoxShadow const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::paint_text_shadow(PaintTextShadow const& command)
{
    auto text_shadow_canvas = AccelGfx::Canvas::create(command.shadow_bounding_rect.size());
    auto text_shadow_painter = AccelGfx::Painter::create(m_context, text_shadow_canvas);
    text_shadow_painter->clear(command.color.with_alpha(0));

    Gfx::FloatRect const shadow_location { command.draw_location, command.shadow_bounding_rect.size() };
    Gfx::IntPoint const baseline_start(command.text_rect.x(), command.text_rect.y() + command.fragment_baseline);
    text_shadow_painter->translate(baseline_start.to_type<float>());
    text_shadow_painter->draw_glyph_run(command.glyph_run->glyphs(), command.color);
    if (command.blur_radius == 0) {
        painter().blit_canvas(shadow_location, *text_shadow_canvas);
        return CommandResult::Continue;
    }

    auto horizontal_blur_canvas = AccelGfx::Canvas::create(command.shadow_bounding_rect.size());
    auto horizontal_blur_painter = AccelGfx::Painter::create(m_context, horizontal_blur_canvas);
    horizontal_blur_painter->clear(command.color.with_alpha(0));
    horizontal_blur_painter->blit_blurred_canvas(command.shadow_bounding_rect.to_type<float>(), *text_shadow_canvas, command.blur_radius, AccelGfx::Painter::BlurDirection::Horizontal);
    painter().blit_blurred_canvas(shadow_location, *horizontal_blur_canvas, command.blur_radius, AccelGfx::Painter::BlurDirection::Vertical);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::fill_rect_with_rounded_corners(FillRectWithRoundedCorners const& command)
{
    // FIXME: Support clip paths
    painter().fill_rect_with_rounded_corners(
        command.rect, command.color,
        { static_cast<float>(command.top_left_radius.horizontal_radius), static_cast<float>(command.top_left_radius.vertical_radius) },
        { static_cast<float>(command.top_right_radius.horizontal_radius), static_cast<float>(command.top_right_radius.vertical_radius) },
        { static_cast<float>(command.bottom_left_radius.horizontal_radius), static_cast<float>(command.bottom_left_radius.vertical_radius) },
        { static_cast<float>(command.bottom_right_radius.horizontal_radius), static_cast<float>(command.bottom_right_radius.vertical_radius) });
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::fill_path_using_color(FillPathUsingColor const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::fill_path_using_paint_style(FillPathUsingPaintStyle const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::stroke_path_using_color(StrokePathUsingColor const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::stroke_path_using_paint_style(StrokePathUsingPaintStyle const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::draw_ellipse(DrawEllipse const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::fill_ellipse(FillEllipse const& command)
{
    auto horizontal_radius = static_cast<float>(command.rect.width() / 2);
    auto vertical_radius = static_cast<float>(command.rect.height() / 2);
    painter().fill_rect_with_rounded_corners(
        command.rect, command.color,
        { horizontal_radius, vertical_radius },
        { horizontal_radius, vertical_radius },
        { horizontal_radius, vertical_radius },
        { horizontal_radius, vertical_radius });
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::draw_line(DrawLine const& command)
{
    // FIXME: Pass line style and alternate color once AccelGfx::Painter supports it
    painter().draw_line(command.from, command.to, command.thickness, command.color);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::apply_backdrop_filter(ApplyBackdropFilter const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::draw_rect(DrawRect const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::paint_radial_gradient(PaintRadialGradient const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::paint_conic_gradient(PaintConicGradient const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::draw_triangle_wave(DrawTriangleWave const&)
{
    // FIXME
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::sample_under_corners(SampleUnderCorners const& command)
{
    m_corner_clippers.resize(command.id + 1);
    m_corner_clippers[command.id] = make<BorderRadiusCornerClipper>();
    auto& corner_clipper = *m_corner_clippers[command.id];

    auto const& top_left = command.corner_radii.top_left;
    auto const& top_right = command.corner_radii.top_right;
    auto const& bottom_right = command.corner_radii.bottom_right;
    auto const& bottom_left = command.corner_radii.bottom_left;

    auto sampling_config = calculate_border_radius_sampling_config(command.corner_radii, command.border_rect);
    auto const& page_locations = sampling_config.page_locations;
    auto const& bitmap_locations = sampling_config.bitmap_locations;

    auto top_left_corner_size = Gfx::IntSize { top_left.horizontal_radius, top_left.vertical_radius };
    auto top_right_corner_size = Gfx::IntSize { top_right.horizontal_radius, top_right.vertical_radius };
    auto bottom_right_corner_size = Gfx::IntSize { bottom_right.horizontal_radius, bottom_right.vertical_radius };
    auto bottom_left_corner_size = Gfx::IntSize { bottom_left.horizontal_radius, bottom_left.vertical_radius };

    corner_clipper.page_top_left_rect = { page_locations.top_left, top_left_corner_size };
    corner_clipper.page_top_right_rect = { page_locations.top_right, top_right_corner_size };
    corner_clipper.page_bottom_right_rect = { page_locations.bottom_right, bottom_right_corner_size };
    corner_clipper.page_bottom_left_rect = { page_locations.bottom_left, bottom_left_corner_size };

    corner_clipper.sample_canvas_top_left_rect = { bitmap_locations.top_left, top_left_corner_size };
    corner_clipper.sample_canvas_top_right_rect = { bitmap_locations.top_right, top_right_corner_size };
    corner_clipper.sample_canvas_bottom_right_rect = { bitmap_locations.bottom_right, bottom_right_corner_size };
    corner_clipper.sample_canvas_bottom_left_rect = { bitmap_locations.bottom_left, bottom_left_corner_size };

    corner_clipper.corners_sample_canvas = AccelGfx::Canvas::create(sampling_config.corners_bitmap_size);
    auto corner_painter = AccelGfx::Painter::create(m_context, *corner_clipper.corners_sample_canvas);
    corner_painter->clear(Color::White);

    corner_painter->fill_rect_with_rounded_corners(
        Gfx::IntRect { { 0, 0 }, sampling_config.corners_bitmap_size },
        Color::Transparent,
        { static_cast<float>(top_left.horizontal_radius), static_cast<float>(top_left.vertical_radius) },
        { static_cast<float>(top_right.horizontal_radius), static_cast<float>(top_right.vertical_radius) },
        { static_cast<float>(bottom_right.horizontal_radius), static_cast<float>(bottom_right.vertical_radius) },
        { static_cast<float>(bottom_left.horizontal_radius), static_cast<float>(bottom_left.vertical_radius) },
        AccelGfx::Painter::BlendingMode::AlphaOverride);

    auto const& target_canvas = painter().canvas();
    if (!corner_clipper.sample_canvas_top_left_rect.is_empty())
        corner_painter->blit_canvas(corner_clipper.sample_canvas_top_left_rect, target_canvas, painter().transform().map(corner_clipper.page_top_left_rect), 1.0f, {}, AccelGfx::Painter::BlendingMode::AlphaPreserve);
    if (!corner_clipper.sample_canvas_top_right_rect.is_empty())
        corner_painter->blit_canvas(corner_clipper.sample_canvas_top_right_rect, target_canvas, painter().transform().map(corner_clipper.page_top_right_rect), 1.0f, {}, AccelGfx::Painter::BlendingMode::AlphaPreserve);
    if (!corner_clipper.sample_canvas_bottom_right_rect.is_empty())
        corner_painter->blit_canvas(corner_clipper.sample_canvas_bottom_right_rect, target_canvas, painter().transform().map(corner_clipper.page_bottom_right_rect), 1.0f, {}, AccelGfx::Painter::BlendingMode::AlphaPreserve);
    if (!corner_clipper.sample_canvas_bottom_left_rect.is_empty())
        corner_painter->blit_canvas(corner_clipper.sample_canvas_bottom_left_rect, target_canvas, painter().transform().map(corner_clipper.page_bottom_left_rect), 1.0f, {}, AccelGfx::Painter::BlendingMode::AlphaPreserve);

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerGPU::blit_corner_clipping(BlitCornerClipping const& command)
{
    auto const& corner_clipper = *m_corner_clippers[command.id];
    auto const& corner_sample_canvas = *corner_clipper.corners_sample_canvas;
    if (!corner_clipper.sample_canvas_top_left_rect.is_empty())
        painter().blit_canvas(corner_clipper.page_top_left_rect, corner_sample_canvas, corner_clipper.sample_canvas_top_left_rect);
    if (!corner_clipper.sample_canvas_top_right_rect.is_empty())
        painter().blit_canvas(corner_clipper.page_top_right_rect, corner_sample_canvas, corner_clipper.sample_canvas_top_right_rect);
    if (!corner_clipper.sample_canvas_bottom_right_rect.is_empty())
        painter().blit_canvas(corner_clipper.page_bottom_right_rect, corner_sample_canvas, corner_clipper.sample_canvas_bottom_right_rect);
    if (!corner_clipper.sample_canvas_bottom_left_rect.is_empty())
        painter().blit_canvas(corner_clipper.page_bottom_left_rect, corner_sample_canvas, corner_clipper.sample_canvas_bottom_left_rect);

    m_corner_clippers[command.id].clear();

    return CommandResult::Continue;
}

bool DisplayListPlayerGPU::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    auto translation = painter().transform().translation().to_type<int>();
    return !painter().clip_rect().intersects(rect.translated(translation));
}

void DisplayListPlayerGPU::prepare_glyph_texture(HashMap<Gfx::Font const*, HashTable<u32>> const& unique_glyphs)
{
    AccelGfx::GlyphAtlas::the().update(unique_glyphs);
}

void DisplayListPlayerGPU::prepare_to_execute([[maybe_unused]] size_t corner_clip_max_depth)
{
    m_context.activate();
}

void DisplayListPlayerGPU::update_immutable_bitmap_texture_cache(HashMap<u32, Gfx::ImmutableBitmap const*>& immutable_bitmaps)
{
    painter().update_immutable_bitmap_texture_cache(immutable_bitmaps);
}

}
