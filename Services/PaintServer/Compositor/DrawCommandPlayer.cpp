/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/SkiaUtils.h>
#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibPaintServer/Debug.h>
#include <LibPaintServer/Policy.h>
#include <PaintServer/Compositor/DrawCommandPlayer.h>

#include <core/SkBlurTypes.h>
#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkColorFilter.h>
#include <core/SkColorSpace.h>
#include <core/SkData.h>
#include <core/SkFont.h>
#include <core/SkImageFilter.h>
#include <core/SkImageInfo.h>
#include <core/SkM44.h>
#include <core/SkMaskFilter.h>
#include <core/SkMatrix.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>
#include <core/SkPicture.h>
#include <core/SkPictureRecorder.h>
#include <core/SkRRect.h>
#include <core/SkSerialProcs.h>
#include <core/SkTextBlob.h>
#include <core/SkTypeface.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradientShader.h>
#include <effects/SkImageFilters.h>
#include <effects/SkLumaColorFilter.h>
#include <math.h>
#include <pathops/SkPathOps.h>

namespace PaintServer {

static SkColor to_sk_color(Gfx::Color color);
static SkRect to_sk_rect(Gfx::FloatRect const& rect);
static SkPoint to_sk_point(Gfx::FloatPoint const& point);
static SkColor4f to_sk_color4f(Gfx::Color color);
static SkRRect to_sk_rrect(FillRectWithRoundedCornersCommand const& command);
static SkRRect to_sk_rrect(Gfx::FloatRect const& rect, WireCornerRadii const& radii);
static bool has_any_radius(WireCornerRadii const& radii);
static bool is_valid_compositing_and_blending_operator(u32 raw_value);
static SkM44 to_skia_matrix4x4(ApplyTransformCommand const& command);
static SkGradientShader::Interpolation to_skia_interpolation(GradientColorSpace color_space, GradientHueMethod hue_method);
static SkMatrix to_sk_matrix(Gfx::AffineTransform const& transform);
static SkTileMode to_sk_tile_mode(SVGSpreadMethod spread_method);
static ErrorOr<SkPath> decode_sk_path(ReadonlyBytes bytes);
static ErrorOr<sk_sp<SkImageFilter>> decode_image_filter(DrawContext const&, ReadonlyBytes filter_bytes, StringView error_source);
static SkPaint s_debug_paint = SkPaint(SkColor4f(1.0f, 0.0f, 1.0f, 0.5f));
static void log_canvas_state(char const* label, SkCanvas const& canvas);

DrawCommandPlayer::DrawCommandPlayer(DrawContext const& draw_context, SkCanvas& canvas)
    : m_draw_context(draw_context)
    , m_canvas(canvas)
    , m_initial_save_count(canvas.getSaveCount())
{
}

ErrorOr<void> DrawCommandPlayer::apply(DrawCommandView const& command_view)
{
    auto dispatch_with_policy = [&](auto&& callback) -> ErrorOr<void> {
        auto result = callback();
        if (!result.is_error())
            return {};
        dbgln("Failed to render draw-list command of type {}: {}", command_type_name(command_view.type), result.error());
        if (is_logging_enabled())
            maybe_draw_magenta_rect(command_view);
        return result.release_error();
    };
    switch (command_view.type) {
    case CommandType::ClearRect:
        return dispatch_with_policy([&] { return apply_clear_rect(command_view); });
    case CommandType::FillRect:
        return dispatch_with_policy([&] { return apply_fill_rect(command_view); });
    case CommandType::FillRectWithRoundedCorners:
        return dispatch_with_policy([&] { return apply_fill_rect_with_rounded_corners(command_view); });
    case CommandType::DrawScaledImage:
        return dispatch_with_policy([&] { return apply_draw_scaled_image(command_view); });
    case CommandType::DrawExternalContent:
        return dispatch_with_policy([&] { return apply_draw_external_content(command_view); });
    case CommandType::DrawRect:
        return dispatch_with_policy([&] { return apply_draw_rect(command_view); });
    case CommandType::DrawLine:
        return dispatch_with_policy([&] { return apply_draw_line(command_view); });
    case CommandType::DrawEllipse:
        return dispatch_with_policy([&] { return apply_draw_ellipse(command_view); });
    case CommandType::FillEllipse:
        return dispatch_with_policy([&] { return apply_fill_ellipse(command_view); });
    case CommandType::DrawGlyphRun:
        return dispatch_with_policy([&] { return apply_draw_glyph_run(command_view); });
    case CommandType::Save:
        return dispatch_with_policy([&] { return apply_save(command_view); });
    case CommandType::SaveLayer:
        return dispatch_with_policy([&] { return apply_save_layer(command_view); });
    case CommandType::Restore:
        return dispatch_with_policy([&] { return apply_restore(command_view); });
    case CommandType::ResetCanvasState:
        return dispatch_with_policy([&] { return apply_reset_canvas_state(command_view); });
    case CommandType::Translate:
        return dispatch_with_policy([&] { return apply_translate(command_view); });
    case CommandType::AddClipRect:
        return dispatch_with_policy([&] { return apply_add_clip_rect(command_view); });
    case CommandType::AddClipPath:
        return dispatch_with_policy([&] { return apply_add_clip_path(command_view); });
    case CommandType::ApplyEffects:
        return dispatch_with_policy([&] { return apply_effects(command_view); });
    case CommandType::ApplyBackdropFilter:
        return dispatch_with_policy([&] { return apply_backdrop_filter(command_view); });
    case CommandType::SetTransform:
        return dispatch_with_policy([&] { return apply_set_transform(command_view); });
    case CommandType::ApplyTransform:
        return dispatch_with_policy([&] { return apply_transform(command_view); });
    case CommandType::AddRoundedRectClip:
        return dispatch_with_policy([&] { return apply_add_rounded_rect_clip(command_view); });
    case CommandType::PaintLinearGradient:
        return dispatch_with_policy([&] { return apply_paint_linear_gradient(command_view); });
    case CommandType::PaintRadialGradient:
        return dispatch_with_policy([&] { return apply_paint_radial_gradient(command_view); });
    case CommandType::PaintConicGradient:
        return dispatch_with_policy([&] { return apply_paint_conic_gradient(command_view); });
    case CommandType::PaintOuterBoxShadow:
        return dispatch_with_policy([&] { return apply_paint_outer_box_shadow(command_view); });
    case CommandType::PaintInnerBoxShadow:
        return dispatch_with_policy([&] { return apply_paint_inner_box_shadow(command_view); });
    case CommandType::PaintTextShadow:
        return dispatch_with_policy([&] { return apply_paint_text_shadow(command_view); });
    case CommandType::FillPath:
        return dispatch_with_policy([&] { return apply_fill_path(command_view); });
    case CommandType::StrokePath:
        return dispatch_with_policy([&] { return apply_stroke_path(command_view); });
    case CommandType::DrawRepeatedImage:
        return dispatch_with_policy([&] { return apply_draw_repeated_image(command_view); });
    case CommandType::PaintScrollBar:
        return dispatch_with_policy([&] { return apply_paint_scroll_bar(command_view); });
    default:
        break;
    }
    dbgln("Unknown draw-list command type: {}", static_cast<u32>(command_view.type));
    maybe_draw_magenta_rect(command_view);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_paint_scroll_bar(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<PaintScrollBarCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("PaintScrollBar: frame_id={} vertical={} scrollbar={}x{}@{},{} gutter={}x{}@{},{} thumb={}x{}@{},{} scroll_size={}",
            command.scroll_frame_id,
            command.vertical,
            command.scrollbar_rect.width(),
            command.scrollbar_rect.height(),
            command.scrollbar_rect.x(),
            command.scrollbar_rect.y(),
            command.gutter_rect.width(),
            command.gutter_rect.height(),
            command.gutter_rect.x(),
            command.gutter_rect.y(),
            command.thumb_rect.width(),
            command.thumb_rect.height(),
            command.thumb_rect.x(),
            command.thumb_rect.y(),
            command.scroll_size);
    }
    Gfx::FloatRect thumb_rect = command.thumb_rect;
    if (m_draw_context.scroll_offset_resolver) {
        Gfx::FloatPoint scroll_offset = m_draw_context.scroll_offset_resolver->operator()(command.scroll_frame_id);
        if (command.vertical)
            thumb_rect.translate_by(0, -scroll_offset.y() * command.scroll_size);
        else
            thumb_rect.translate_by(-scroll_offset.x() * command.scroll_size, 0);
    }

    if (!command.gutter_rect.is_empty()) {
        SkPaint gutter_fill_paint;
        gutter_fill_paint.setColor(to_sk_color(command.track_color));
        m_canvas.drawRect(to_sk_rect(command.gutter_rect), gutter_fill_paint);
    }

    if (thumb_rect.is_empty())
        return {};

    f32 radius = thumb_rect.width() / 2.0f;
    SkRect sk_thumb_rect = to_sk_rect(thumb_rect);
    SkRRect thumb_rrect = SkRRect::MakeRectXY(sk_thumb_rect, radius, radius);

    SkPaint thumb_fill_paint;
    thumb_fill_paint.setColor(to_sk_color(command.thumb_color));
    m_canvas.drawRRect(thumb_rrect, thumb_fill_paint);

    Gfx::Color stroke_color = command.thumb_color.lightened();
    SkPaint stroke_paint;
    stroke_paint.setStyle(SkPaint::kStroke_Style);
    stroke_paint.setStrokeWidth(1);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setColor(to_sk_color(stroke_color));
    m_canvas.drawRRect(thumb_rrect, stroke_paint);

    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_clear_rect(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<ClearRectCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("ClearRect: rect={}x{}@{},{}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y());
    if (command.rect.is_empty())
        return {};

    m_canvas.save();
    m_canvas.clipRect(to_sk_rect(command.rect), true);
    m_canvas.clear(to_sk_color(command.color));
    m_canvas.restore();
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_fill_rect(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<FillRectCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("FillRect: rect={}x{}@{},{}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y());
    if (command.rect.is_empty())
        return {};

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_sk_color(command.color));
    m_canvas.drawRect(to_sk_rect(command.rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_fill_rect_with_rounded_corners(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<FillRectWithRoundedCornersCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("FillRectWithRoundedCorners: rect={}x{}@{},{}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y());
    if (command.rect.is_empty())
        return {};

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_sk_color(command.color));

    m_canvas.drawRRect(to_sk_rrect(command), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_draw_scaled_image(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<DrawScaledImageCommand>(command_view));
    ReadonlyBytes filter_bytes = TRY(command_trailing_bytes<DrawScaledImageCommand>(command_view, command.filter_byte_count));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("DrawScaledImage: {} image_id={} dst={}x{}@{},{} clip={}x{}@{},{}",
            command.image_resource_id,
            command.image_id,
            command.dst_rect.width(),
            command.dst_rect.height(),
            command.dst_rect.x(),
            command.dst_rect.y(),
            command.clip_rect.width(),
            command.clip_rect.height(),
            command.clip_rect.x(),
            command.clip_rect.y());
    }
    if (command.image_resource_id == 0 && command.image_id == 0)
        return Error::from_string_literal("DrawScaledImage command has invalid resource id");
    if (command.dst_rect.is_empty() || command.clip_rect.is_empty())
        return {};
    if (!m_draw_context.draw_scaled_image_painter)
        return Error::from_string_literal("DrawScaledImage command has no resource handler");

    sk_sp<SkImageFilter> image_filter;
    if (!filter_bytes.is_empty())
        image_filter = TRY(decode_image_filter(m_draw_context, filter_bytes, "DrawScaledImage"sv));

    return (*m_draw_context.draw_scaled_image_painter)(command, m_canvas, image_filter);
}

ErrorOr<void> DrawCommandPlayer::apply_draw_external_content(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<DrawExternalContentCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("DrawExternalContent: {} image_id={} dst={}x{}@{},{} clip={}x{}@{},{} scaling_mode={}",
            command.image_resource_id,
            command.image_id,
            command.dst_rect.width(),
            command.dst_rect.height(),
            command.dst_rect.x(),
            command.dst_rect.y(),
            command.clip_rect.width(),
            command.clip_rect.height(),
            command.clip_rect.x(),
            command.clip_rect.y(),
            command.scaling_mode);
    }
    if (command.image_resource_id == 0 && command.image_id == 0)
        return Error::from_string_literal("DrawExternalContent command has invalid resource id");
    if (command.dst_rect.is_empty() || command.clip_rect.is_empty())
        return {};
    if (!m_draw_context.draw_external_content_painter)
        return Error::from_string_literal("DrawExternalContent command has no resource handler");

    return (*m_draw_context.draw_external_content_painter)(command, m_canvas);
}

ErrorOr<void> DrawCommandPlayer::apply_draw_rect(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<DrawRectCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("DrawRect: rect={}x{}@{},{} thickness={}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y(), command.thickness);
    if (command.rect.is_empty())
        return {};
    if (!isfinite(command.thickness) || command.thickness <= 0.0f)
        return {};

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_sk_color(command.color));
    m_canvas.drawRect(to_sk_rect(command.rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_draw_ellipse(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<DrawEllipseCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("DrawEllipse: rect={}x{}@{},{} thickness={}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y(), command.thickness);
    if (command.rect.is_empty())
        return {};
    if (!isfinite(command.thickness) || command.thickness <= 0.0f)
        return {};

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_sk_color(command.color));
    m_canvas.drawOval(to_sk_rect(command.rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_draw_line(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<DrawLineCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("DrawLine: from=({}, {}) to=({}, {}) thickness={} style={}", command.from.x(), command.from.y(), command.to.x(), command.to.y(), command.thickness, command.style);
    if (!isfinite(command.thickness) || command.thickness <= 0.0f)
        return {};

    f32 from_x = command.from.x();
    f32 from_y = command.from.y();
    f32 to_x = command.to.x();
    f32 to_y = command.to.y();

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_sk_color(command.color));

    auto style = static_cast<WireLineStyle>(command.style);
    switch (style) {
    case WireLineStyle::Solid:
        break;
    case WireLineStyle::Dotted: {
        f32 dx = to_x - from_x;
        f32 dy = to_y - from_y;
        f32 length = hypotf(dx, dy);
        if (!isfinite(length) || length <= 0.0f)
            break;

        f32 dot_count = floorf(length / (command.thickness * 2.0f));
        if (!isfinite(dot_count) || dot_count < 1.0f)
            dot_count = 1.0f;

        f32 interval = length / dot_count;
        if (!isfinite(interval) || interval <= 0.0f)
            break;

        paint.setStrokeCap(SkPaint::kRound_Cap);
        SkScalar intervals[] = { 0.0f, interval };
        paint.setPathEffect(SkDashPathEffect::Make(SkSpan<SkScalar const>(intervals, 2), 0));

        f32 inv_len = 1.0f / length;
        to_x += (dx * inv_len) * (interval / 2.0f);
        to_y += (dy * inv_len) * (interval / 2.0f);
        break;
    }
    case WireLineStyle::Dashed: {
        SkScalar intervals[] = { command.thickness * 3.0f, command.thickness * 3.0f };
        paint.setPathEffect(SkDashPathEffect::Make(SkSpan<SkScalar const>(intervals, 2), 0));
        break;
    }
    }

    m_canvas.drawLine(from_x, from_y, to_x, to_y, paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_fill_ellipse(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<FillEllipseCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("FillEllipse: rect={}x{}@{},{}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y());
    if (command.rect.is_empty())
        return {};

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_sk_color(command.color));
    m_canvas.drawOval(to_sk_rect(command.rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_save(DrawCommandView const& command_view)
{
    (void)TRY(decode_fixed_size_command<SaveCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("Save");
    m_canvas.save();
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_save_layer(DrawCommandView const& command_view)
{
    (void)TRY(decode_fixed_size_command<SaveLayerCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("SaveLayer");
    m_canvas.saveLayer(nullptr, nullptr);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_restore(DrawCommandView const& command_view)
{
    (void)TRY(decode_fixed_size_command<RestoreCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("Restore: save_count={}", m_canvas.getSaveCount());
    if (m_canvas.getSaveCount() <= m_initial_save_count)
        return {};
    m_canvas.restore();
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_reset_canvas_state(DrawCommandView const& command_view)
{
    (void)TRY(decode_fixed_size_command<ResetCanvasStateCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("ResetCanvasState: save_count={}", m_canvas.getSaveCount());
    if (m_initial_save_count <= 1) {
        m_canvas.restoreToCount(1);
        return {};
    }
    m_canvas.restoreToCount(m_initial_save_count - 1);
    m_canvas.save();
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_translate(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<TranslateCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("Translate: delta=({}, {})", command.delta.x(), command.delta.y());
    m_canvas.translate(command.delta.x(), command.delta.y());
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_add_clip_rect(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<AddClipRectCommand>(command_view));
    m_canvas.clipRect(to_sk_rect(command.rect));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("AddClipRect: rect={}x{}@{},{}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y());
        log_canvas_state("AddClipRect state", m_canvas);
    }
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_add_clip_path(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<AddClipPathCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("AddClipPath: bounds={}x{}@{},{} fill_rule={} path_bytes={}", command.bounding_rect.width(), command.bounding_rect.height(), command.bounding_rect.x(), command.bounding_rect.y(), command.fill_rule, command.path_byte_count);
    if (command.bounding_rect.is_empty())
        return {};

    ReadonlyBytes path_bytes = TRY(command_trailing_bytes<AddClipPathCommand>(command_view, command.path_byte_count));
    SkPath path = TRY(decode_sk_path(path_bytes));
    path.setFillType(Gfx::to_skia_path_fill_type(static_cast<Gfx::WindingRule>(command.fill_rule)));
    m_canvas.clipPath(path, true);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_effects(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<ApplyEffectsCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("ApplyEffects: opacity={} blend_op={} has_mask_kind={} mask_kind={} filter_bytes={}", command.opacity, command.compositing_and_blending_operator, command.has_mask_kind, command.mask_kind, command.filter_byte_count);

    if (command.filter_byte_count > MAX_APPLY_EFFECTS_FILTER_BYTES)
        return Error::from_string_literal("ApplyEffects filter payload too large");

    ReadonlyBytes filter_bytes = TRY(command_trailing_bytes<ApplyEffectsCommand>(command_view, command.filter_byte_count));

    SkPaint paint;

    if (isfinite(command.opacity) && command.opacity < 1.0f)
        paint.setAlphaf(clamp(command.opacity, 0.0f, 1.0f));

    if (is_valid_compositing_and_blending_operator(command.compositing_and_blending_operator)) {
        auto op = static_cast<Gfx::CompositingAndBlendingOperator>(command.compositing_and_blending_operator);
        if (op != Gfx::CompositingAndBlendingOperator::Normal)
            paint.setBlender(Gfx::to_skia_blender(op));
    }

    if (!filter_bytes.is_empty())
        paint.setImageFilter(TRY(decode_image_filter(m_draw_context, filter_bytes, "DrawCommandPlayer"sv)));

    if (command.has_mask_kind != 0) {
        auto mask_kind = static_cast<Gfx::MaskKind>(command.mask_kind);
        if (mask_kind == Gfx::MaskKind::Luminance)
            paint.setColorFilter(SkLumaColorFilter::Make());
    }

    m_canvas.saveLayer(nullptr, &paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_backdrop_filter(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<ApplyBackdropFilterCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("ApplyBackdropFilter: region={}x{}@{},{} filter_bytes={}", command.backdrop_region.width(), command.backdrop_region.height(), command.backdrop_region.x(), command.backdrop_region.y(), command.filter_byte_count);

    if (command.filter_byte_count > MAX_APPLY_EFFECTS_FILTER_BYTES)
        return Error::from_string_literal("ApplyBackdropFilter filter payload too large");

    ReadonlyBytes filter_bytes = TRY(command_trailing_bytes<ApplyBackdropFilterCommand>(command_view, command.filter_byte_count));

    SkRect backdrop_rect = to_sk_rect(command.backdrop_region);
    m_canvas.save();

    if (has_any_radius(command.corner_radii)) {
        SkRRect rounded_backdrop = to_sk_rrect(command.backdrop_region, command.corner_radii);
        m_canvas.clipRRect(rounded_backdrop, true);
    } else {
        m_canvas.clipRect(backdrop_rect, true);
    }

    if (!filter_bytes.is_empty()) {
        sk_sp<SkImageFilter> image_filter = TRY(decode_image_filter(m_draw_context, filter_bytes, "DrawCommandPlayer"sv));
        m_canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
    }

    if (!filter_bytes.is_empty())
        m_canvas.restore();
    m_canvas.restore();
    return {};
}

bool has_any_radius(WireCornerRadii const& radii)
{
    return radii.top_left.horizontal_radius != 0.0f
        || radii.top_left.vertical_radius != 0.0f
        || radii.top_right.horizontal_radius != 0.0f
        || radii.top_right.vertical_radius != 0.0f
        || radii.bottom_right.horizontal_radius != 0.0f
        || radii.bottom_right.vertical_radius != 0.0f
        || radii.bottom_left.horizontal_radius != 0.0f
        || radii.bottom_left.vertical_radius != 0.0f;
}

ErrorOr<sk_sp<SkImageFilter>> decode_image_filter(DrawContext const& draw_context, ReadonlyBytes filter_bytes, StringView error_source)
{
    if (filter_bytes.is_empty())
        return Error::from_string_literal("Serialized image filter payload is empty");

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    auto deserialize_filter_image_from_data = [](sk_sp<SkData> data, std::optional<SkAlphaType>, void* context) -> sk_sp<SkImage> {
        if (!data || data->size() != sizeof(SerializedFilterImageReference) || !context)
            return nullptr;

        auto const& image_reference = *static_cast<SerializedFilterImageReference const*>(data->data());
        auto const* image_resolver = static_cast<DrawImageResolver const*>(context);
        if (!image_resolver)
            return nullptr;
        return image_resolver->operator()(image_reference.image_resource_id, image_reference.image_id);
    };

    SkDeserialProcs deserial_procs;
    deserial_procs.fImageCtx = const_cast<DrawImageResolver*>(draw_context.image_resolver);
    deserial_procs.fImageDataProc = deserialize_filter_image_from_data;

    sk_sp<SkImageFilter> filter = SkImageFilter::Deserialize(filter_bytes.data(), filter_bytes.size(), &deserial_procs);
    if (!filter) {
        if (is_logging_enabled(LOG_RESOURCE))
            dbgln("{}: filter deserialization failed payload_size={}", error_source, filter_bytes.size());
        return Error::from_string_literal("Serialized image filter deserialization failed");
    }
    return filter;
}

ErrorOr<void> DrawCommandPlayer::apply_transform(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<ApplyTransformCommand>(command_view));
    auto origin = command.origin;

    SkM44 transform = SkM44::Translate(origin.x(), origin.y(), 0.0f) * to_skia_matrix4x4(command) * SkM44::Translate(-origin.x(), -origin.y(), 0.0f);
    m_canvas.concat(transform);
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("ApplyTransform: origin=({}, {}) matrix=[[{}, {}, {}, {}], [{}, {}, {}, {}], [{}, {}, {}, {}], [{}, {}, {}, {}]]",
            origin.x(),
            origin.y(),
            command.matrix[0, 0], command.matrix[0, 1], command.matrix[0, 2], command.matrix[0, 3],
            command.matrix[1, 0], command.matrix[1, 1], command.matrix[1, 2], command.matrix[1, 3],
            command.matrix[2, 0], command.matrix[2, 1], command.matrix[2, 2], command.matrix[2, 3],
            command.matrix[3, 0], command.matrix[3, 1], command.matrix[3, 2], command.matrix[3, 3]);
        log_canvas_state("ApplyTransform state", m_canvas);
    }
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_set_transform(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<SetTransformCommand>(command_view));
    auto const& transform = command.transform;
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("SetTransform: [{}, {}, {}, {}, {}, {}]", transform.a(), transform.b(), transform.c(), transform.d(), transform.e(), transform.f());
    SkMatrix matrix = SkMatrix::MakeAll(
        transform.a(), transform.c(), transform.e(),
        transform.b(), transform.d(), transform.f(),
        0, 0, 1);
    m_canvas.setMatrix(matrix);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_add_rounded_rect_clip(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<AddRoundedRectClipCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("AddRoundedRectClip: rect={}x{}@{},{} corner_clip={}", command.border_rect.width(), command.border_rect.height(), command.border_rect.x(), command.border_rect.y(), command.corner_clip);
    if (command.border_rect.is_empty())
        return {};

    SkRRect rounded_rect = to_sk_rrect(command.border_rect, command.corner_radii);
    bool inside = command.corner_clip == to_underlying(WireCornerClip::Inside);
    SkClipOp clip_op = inside ? SkClipOp::kDifference : SkClipOp::kIntersect;
    m_canvas.clipRRect(rounded_rect, clip_op, true);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_draw_glyph_run(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<DrawGlyphRunCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("DrawGlyphRun: font_resource_id={} glyph_count={} text_rect={}x{}@{},{} visual_bounds={}x{}@{},{} translation=({}, {}) orientation={}",
            command.font_resource_id,
            command.glyph_count,
            command.text_rect.width(),
            command.text_rect.height(),
            command.text_rect.x(),
            command.text_rect.y(),
            command.visual_bounds.width(),
            command.visual_bounds.height(),
            command.visual_bounds.x(),
            command.visual_bounds.y(),
            command.translation.x(),
            command.translation.y(),
            command.orientation);
    }
    if (command.font_resource_id == 0)
        return Error::from_string_literal("DrawGlyphRun has invalid font resource id");
    if (command.glyph_count == 0)
        return {};

    auto glyphs = TRY(command_trailing_span<DrawGlyphRunCommand, Glyph>(command_view, command.glyph_count));
    return draw_glyph_run(command, glyphs);
}

ErrorOr<void> DrawCommandPlayer::draw_glyph_run(DrawGlyphRunCommand const& header, ReadonlySpan<Glyph const> glyphs)
{
    bool vertical = header.orientation != 0;

    auto* typeface = m_draw_context.font_cache.lookup(m_draw_context.surface_id, header.font_resource_id);
    if (!typeface)
        return {};

    f32 scale = header.device_pixels_per_css_pixel;
    SkFont sk_font(sk_ref_sp(typeface), header.font_pixel_size * scale);
    sk_font.setSubpixel(true);

    SkTextBlobBuilder builder;
    auto const& run = builder.allocRunPos(sk_font, static_cast<int>(header.glyph_count));

    for (u32 i = 0; i < header.glyph_count; ++i) {
        auto const& wire_glyph = glyphs[i];
        run.glyphs[i] = wire_glyph.glyph_id;
        size_t const pos_index = static_cast<size_t>(i) * 2;
        run.pos[pos_index] = wire_glyph.x * scale;
        run.pos[pos_index + 1] = ((wire_glyph.y + header.font_ascent) * scale);
    }

    auto blob = builder.make();
    if (!blob)
        return {};

    SkPaint paint;
    paint.setColor(to_sk_color(header.color));

    if (vertical) {
        m_canvas.save();
        m_canvas.translate(header.text_rect.width(), 0);
        m_canvas.rotate(90, header.text_rect.x(), header.text_rect.y());
    }

    m_canvas.drawTextBlob(blob.get(), header.translation.x(), header.translation.y(), paint);

    if (vertical)
        m_canvas.restore();

    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_paint_linear_gradient(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<PaintLinearGradientCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("PaintLinearGradient: rect={}x{}@{},{} angle={} repeat_length={} has_repeat_length={} stop_count={}", command.gradient_rect.width(), command.gradient_rect.height(), command.gradient_rect.x(), command.gradient_rect.y(), command.gradient_angle, command.repeat_length, command.has_repeat_length, command.stop_count);
    if (command.stop_count == 0)
        return {};

    if (command.gradient_rect.is_empty())
        return {};

    auto stops = TRY(command_trailing_span<PaintLinearGradientCommand, WireColorStop>(command_view, command.stop_count));

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    colors.ensure_capacity(command.stop_count);
    positions.ensure_capacity(command.stop_count);

    auto const& first_stop = stops[0];
    f32 const first_position = command.has_repeat_length != 0 ? first_stop.position : 0.0f;
    f32 const repeat_length = command.has_repeat_length != 0 ? command.repeat_length : 1.0f;
    if (!isfinite(repeat_length) || repeat_length == 0.0f)
        return Error::from_string_literal("PaintLinearGradient has invalid repeat_length");

    for (auto const& stop : stops) {
        if (!isfinite(stop.position))
            continue;

        colors.append(to_sk_color4f(stop.color));
        positions.append((stop.position - first_position) / repeat_length);
    }

    if (colors.is_empty())
        return {};

    auto rect = command.gradient_rect;
    f32 length = Gfx::calculate_gradient_length<float>(rect.size(), command.gradient_angle);

    auto rect_center = rect.center();
    auto start = rect_center.translated(0.0f, (0.5f - first_position) * length);
    auto end = start.translated(0.0f, repeat_length * -length);
    SkPoint points[2] { to_sk_point(start), to_sk_point(end) };

    SkMatrix matrix;
    matrix.setRotate(command.gradient_angle, rect_center.x(), rect_center.y());

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(static_cast<GradientColorSpace>(command.color_space), static_cast<GradientHueMethod>(command.hue_method));
    auto shader = SkGradientShader::MakeLinear(points, colors.data(), color_space, positions.data(), static_cast<int>(positions.size()), SkTileMode::kRepeat, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setShader(shader);
    paint.setAntiAlias(true);
    m_canvas.drawRect(to_sk_rect(rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_paint_radial_gradient(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<PaintRadialGradientCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("PaintRadialGradient: rect={}x{}@{},{} center=({}, {}) size={}x{} repeating={} stop_count={}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y(), command.center.x(), command.center.y(), command.size.width(), command.size.height(), command.repeating, command.stop_count);
    if (command.stop_count == 0)
        return {};

    if (command.rect.is_empty())
        return {};

    auto stops = TRY(command_trailing_span<PaintRadialGradientCommand, WireColorStop>(command_view, command.stop_count));

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    colors.ensure_capacity(command.stop_count);
    positions.ensure_capacity(command.stop_count);

    Optional<WireColorStop> previous_stop;
    for (auto const& stop : stops) {
        if (previous_stop.has_value() && previous_stop->color == stop.color && previous_stop->position == stop.position)
            continue;
        previous_stop = stop;

        colors.append(to_sk_color4f(stop.color));
        positions.append(stop.position);
    }

    if (colors.is_empty())
        return {};

    auto center = to_sk_point(command.center);
    auto size = command.size;

    SkMatrix matrix;
    f32 const aspect_ratio = size.width() / size.height();
    f32 const sx = isinf(aspect_ratio) ? 1.0f : aspect_ratio;
    matrix.setScale(sx, 1.0f, center.x(), center.y());

    SkTileMode tile_mode = command.repeating != 0 ? SkTileMode::kRepeat : SkTileMode::kClamp;

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(static_cast<GradientColorSpace>(command.color_space), static_cast<GradientHueMethod>(command.hue_method));
    auto shader = SkGradientShader::MakeRadial(center, size.height(), colors.data(), color_space, positions.data(), static_cast<int>(positions.size()), tile_mode, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    m_canvas.drawRect(to_sk_rect(command.rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_paint_conic_gradient(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<PaintConicGradientCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("PaintConicGradient: rect={}x{}@{},{} center=({}, {}) start_angle={} stop_count={}", command.rect.width(), command.rect.height(), command.rect.x(), command.rect.y(), command.position.x(), command.position.y(), command.start_angle, command.stop_count);
    if (command.stop_count == 0)
        return {};

    if (command.rect.is_empty())
        return {};

    auto stops = TRY(command_trailing_span<PaintConicGradientCommand, WireColorStop>(command_view, command.stop_count));

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    colors.ensure_capacity(command.stop_count);
    positions.ensure_capacity(command.stop_count);

    Optional<WireColorStop> previous_stop;
    for (auto const& stop : stops) {
        if (previous_stop.has_value() && previous_stop->color == stop.color && previous_stop->position == stop.position)
            continue;
        previous_stop = stop;

        colors.append(to_sk_color4f(stop.color));
        positions.append(stop.position);
    }

    if (colors.is_empty())
        return {};

    auto center = command.position;

    SkMatrix matrix;
    matrix.setRotate(-90 + command.start_angle, center.x(), center.y());

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(static_cast<GradientColorSpace>(command.color_space), static_cast<GradientHueMethod>(command.hue_method));
    auto shader = SkGradientShader::MakeSweep(center.x(), center.y(), colors.data(), color_space, positions.data(), static_cast<int>(positions.size()), SkTileMode::kRepeat, 0, 360, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    m_canvas.drawRect(to_sk_rect(command.rect), paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_draw_repeated_image(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<DrawRepeatedImageCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("DrawRepeatedImage: {} image_id={} dst={}x{}@{},{} clip={}x{}@{},{} repeat_x={} repeat_y={} scaling_mode={}",
            command.image_resource_id,
            command.image_id,
            command.dst_rect.width(),
            command.dst_rect.height(),
            command.dst_rect.x(),
            command.dst_rect.y(),
            command.clip_rect.width(),
            command.clip_rect.height(),
            command.clip_rect.x(),
            command.clip_rect.y(),
            command.repeat_x,
            command.repeat_y,
            command.scaling_mode);
    }
    if (command.image_resource_id == 0 && command.image_id == 0)
        return Error::from_string_literal("DrawRepeatedImage command has invalid resource id");
    if (command.dst_rect.is_empty() || command.clip_rect.is_empty())
        return {};
    if (!m_draw_context.draw_repeated_image_painter)
        return Error::from_string_literal("DrawRepeatedImage command has no resource handler");

    return (*m_draw_context.draw_repeated_image_painter)(command, m_canvas);
}

ErrorOr<void> DrawCommandPlayer::apply_paint_outer_box_shadow(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<PaintOuterBoxShadowCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("PaintOuterBoxShadow: content={}x{}@{},{} shadow={}x{}@{},{} blur_radius={}", command.device_content_rect.width(), command.device_content_rect.height(), command.device_content_rect.x(), command.device_content_rect.y(), command.shadow_rect.width(), command.shadow_rect.height(), command.shadow_rect.x(), command.shadow_rect.y(), command.blur_radius);
    if (command.device_content_rect.is_empty())
        return {};

    SkRRect content_rrect = to_sk_rrect(command.device_content_rect, command.content_corner_radii);

    m_canvas.save();
    m_canvas.clipRRect(content_rrect, SkClipOp::kDifference, true);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_sk_color(command.color));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, command.blur_radius / 2.0f));

    SkRRect shadow_rrect = to_sk_rrect(command.shadow_rect, command.shadow_corner_radii);
    m_canvas.drawRRect(shadow_rrect, paint);

    m_canvas.restore();
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_paint_inner_box_shadow(DrawCommandView const& command_view)
{
    auto command = TRY(decode_fixed_size_command<PaintInnerBoxShadowCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING))
        dbgln("PaintInnerBoxShadow: content={}x{}@{},{} outer={}x{}@{},{} inner={}x{}@{},{} blur_radius={}", command.device_content_rect.width(), command.device_content_rect.height(), command.device_content_rect.x(), command.device_content_rect.y(), command.outer_shadow_rect.width(), command.outer_shadow_rect.height(), command.outer_shadow_rect.x(), command.outer_shadow_rect.y(), command.inner_shadow_rect.width(), command.inner_shadow_rect.height(), command.inner_shadow_rect.x(), command.inner_shadow_rect.y(), command.blur_radius);
    if (command.device_content_rect.is_empty())
        return {};

    SkRRect outer_rect = to_sk_rrect(command.outer_shadow_rect, command.content_corner_radii);
    SkRRect inner_rect = to_sk_rrect(command.inner_shadow_rect, command.inner_shadow_corner_radii);

    SkPath outer_path;
    outer_path.addRRect(outer_rect);
    SkPath inner_path;
    inner_path.addRRect(inner_rect);

    SkPath result_path;
    if (!Op(outer_path, inner_path, SkPathOp::kDifference_SkPathOp, &result_path))
        return Error::from_string_literal("PaintInnerBoxShadow path op failed");

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_sk_color(command.color));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, command.blur_radius / 2.0f));

    m_canvas.save();
    m_canvas.clipRRect(to_sk_rrect(command.device_content_rect, command.content_corner_radii), true);
    m_canvas.drawPath(result_path, paint);
    m_canvas.restore();
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_paint_text_shadow(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<PaintTextShadowCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("PaintTextShadow: font_resource_id={} glyph_count={} visual_bounds={}x{}@{},{} translation=({}, {}) blur_radius={} orientation={}",
            command.font_resource_id,
            command.glyph_count,
            command.visual_bounds.width(),
            command.visual_bounds.height(),
            command.visual_bounds.x(),
            command.visual_bounds.y(),
            command.translation.x(),
            command.translation.y(),
            command.blur_radius,
            command.orientation);
    }
    if (command.font_resource_id == 0)
        return Error::from_string_literal("PaintTextShadow has invalid font resource id");
    if (command.glyph_count == 0)
        return {};

    auto glyphs = TRY(command_trailing_span<PaintTextShadowCommand, Glyph>(command_view, command.glyph_count));

    auto blur_image_filter = SkImageFilters::Blur(command.blur_radius / 2.0f, command.blur_radius / 2.0f, nullptr);
    SkPaint blur_paint;
    blur_paint.setImageFilter(blur_image_filter);

    m_canvas.saveLayer(nullptr, &blur_paint);

    DrawGlyphRunCommand draw_header;
    draw_header.font_resource_id = command.font_resource_id;
    draw_header.font_pixel_size = command.font_pixel_size;
    draw_header.font_ascent = command.font_ascent;
    draw_header.device_pixels_per_css_pixel = command.device_pixels_per_css_pixel;
    draw_header.color = command.color;
    draw_header.translation = command.translation;
    draw_header.text_rect = command.text_rect;
    draw_header.visual_bounds = command.visual_bounds;
    draw_header.orientation = command.orientation;
    draw_header.glyph_count = command.glyph_count;

    auto result = draw_glyph_run(draw_header, glyphs);
    m_canvas.restore();
    return result;
}

static ErrorOr<SkPath> decode_sk_path(ReadonlyBytes bytes)
{
    SkPath path;
    size_t const bytes_read = path.readFromMemory(bytes.data(), bytes.size());
    if (bytes_read != bytes.size())
        return Error::from_string_literal("Path payload deserialization failed");
    return path;
}

static ErrorOr<sk_sp<SkPicture>> record_pattern_picture(DrawContext const& draw_context, ReadonlyBytes payload, Gfx::IntSize picture_size)
{
    SkPictureRecorder recorder;
    SkCanvas* canvas = recorder.beginRecording(SkRect::MakeWH(static_cast<SkScalar>(picture_size.width()), static_cast<SkScalar>(picture_size.height())));
    if (!canvas)
        return Error::from_string_literal("Pattern paint recorder canvas is unavailable");

    DrawCommandPlayer player(draw_context, *canvas);
    Cursor cursor(payload);
    for (;;) {
        auto maybe_command_or_error = cursor.next();
        if (maybe_command_or_error.is_error())
            return maybe_command_or_error.release_error();
        auto maybe_command = maybe_command_or_error.release_value();
        if (!maybe_command.has_value())
            break;
        TRY(player.apply(maybe_command.value()));
    }

    auto picture = recorder.finishRecordingAsPicture();
    if (!picture)
        return Error::from_string_literal("Pattern paint picture recording failed");
    return picture;
}

static Optional<SkRect> bounded_pattern_tile_rect(SkMatrix const& local_matrix, Gfx::FloatRect const& bounding_rect, Gfx::IntSize picture_size)
{
    SkMatrix inverse;
    if (!local_matrix.invert(&inverse))
        return {};

    SkRect picture_bounds = SkRect::MakeWH(static_cast<SkScalar>(picture_size.width()), static_cast<SkScalar>(picture_size.height()));
    SkRect picture_space_bounds;
    inverse.mapRect(&picture_space_bounds, to_sk_rect(bounding_rect));
    if (picture_space_bounds.width() > picture_bounds.width() || picture_space_bounds.height() > picture_bounds.height())
        return {};
    if (!picture_space_bounds.intersect(picture_bounds))
        return {};
    return picture_space_bounds;
}

static ErrorOr<SkPaint> decode_path_paint(u8 paint_style_type, Gfx::Color const& color, ReadonlyBytes paint_payload, Gfx::FloatRect const& bounding_rect, DrawContext const& draw_context)
{
    SkPaint paint;

    switch (static_cast<PaintStyleType>(paint_style_type)) {
    case PaintStyleType::SolidColor:
        paint.setColor(to_sk_color(color));
        return paint;
    case PaintStyleType::SVGLinearGradient: {
        auto header = TRY(read_command_struct<SVGLinearGradientPayload>(paint_payload));
        auto stops = TRY(command_trailing_span<SVGLinearGradientPayload, WireColorStop>(paint_payload, header.stop_count));

        Vector<SkColor> colors;
        Vector<SkScalar> positions;
        colors.ensure_capacity(stops.size());
        positions.ensure_capacity(stops.size());
        for (auto const& stop : stops) {
            colors.append(to_sk_color(stop.color));
            positions.append(stop.position);
        }

        SkMatrix matrix;
        matrix.setTranslate(bounding_rect.x(), bounding_rect.y());
        if (header.has_gradient_transform != 0)
            matrix = matrix * to_sk_matrix(header.gradient_transform);

        SkPoint points[] { to_sk_point(header.start_point), to_sk_point(header.end_point) };
        paint.setShader(SkGradientShader::MakeLinear(points, colors.data(), positions.data(), static_cast<int>(positions.size()), to_sk_tile_mode(static_cast<SVGSpreadMethod>(header.spread_method)), 0, &matrix));
        if (static_cast<Gfx::InterpolationColorSpace>(header.color_space) == Gfx::InterpolationColorSpace::LinearRGB)
            paint.setColorFilter(SkColorFilters::LinearToSRGBGamma());
        return paint;
    }
    case PaintStyleType::SVGRadialGradient: {
        auto header = TRY(read_command_struct<SVGRadialGradientPayload>(paint_payload));
        auto stops = TRY(command_trailing_span<SVGRadialGradientPayload, WireColorStop>(paint_payload, header.stop_count));

        Vector<SkColor> colors;
        Vector<SkScalar> positions;
        colors.ensure_capacity(stops.size());
        positions.ensure_capacity(stops.size());
        for (auto const& stop : stops) {
            colors.append(to_sk_color(stop.color));
            positions.append(stop.position);
        }

        SkMatrix matrix;
        matrix.setTranslate(bounding_rect.x(), bounding_rect.y());
        if (header.has_gradient_transform != 0)
            matrix = matrix * to_sk_matrix(header.gradient_transform);

        paint.setShader(SkGradientShader::MakeTwoPointConical(to_sk_point(header.start_center), header.start_radius, to_sk_point(header.end_center), header.end_radius, colors.data(), positions.data(), static_cast<int>(positions.size()), to_sk_tile_mode(static_cast<SVGSpreadMethod>(header.spread_method)), 0, &matrix));
        if (static_cast<Gfx::InterpolationColorSpace>(header.color_space) == Gfx::InterpolationColorSpace::LinearRGB)
            paint.setColorFilter(SkColorFilters::LinearToSRGBGamma());
        return paint;
    }
    case PaintStyleType::SVGPattern: {
        auto header = TRY(read_command_struct<SVGPatternPayload>(paint_payload));
        if (paint_payload.size() - sizeof(header) != header.draw_list_byte_count)
            return Error::from_string_literal("Pattern paint draw-list payload size mismatch");

        Gfx::IntSize const picture_size { ceilf(header.tile_rect.width()), ceilf(header.tile_rect.height()) };
        if (picture_size.is_empty())
            return Error::from_string_literal("Pattern paint tile rect is empty");

        ReadonlyBytes const draw_list_bytes = paint_payload.slice(sizeof(header), header.draw_list_byte_count);
        sk_sp<SkPicture> picture = TRY(record_pattern_picture(draw_context, draw_list_bytes, picture_size));

        SkMatrix matrix;
        matrix.setTranslate(header.tile_rect.x(), header.tile_rect.y());
        matrix.preScale(header.tile_rect.width() / static_cast<f32>(picture_size.width()), header.tile_rect.height() / static_cast<f32>(picture_size.height()));
        if (header.has_pattern_transform != 0)
            matrix = matrix * to_sk_matrix(header.pattern_transform);

        Optional<SkRect> tile_rect = bounded_pattern_tile_rect(matrix, bounding_rect, picture_size);
        paint.setShader(picture->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkFilterMode::kLinear, &matrix, tile_rect.has_value() ? &tile_rect.value() : nullptr));
        return paint;
    }
    case PaintStyleType::CanvasLinearGradient: {
        auto header = TRY(read_command_struct<CanvasLinearGradientPayload>(paint_payload));
        auto stops = TRY(command_trailing_span<CanvasLinearGradientPayload, WireColorStop>(paint_payload, header.stop_count));

        Vector<SkColor> colors;
        Vector<SkScalar> positions;
        colors.ensure_capacity(stops.size());
        positions.ensure_capacity(stops.size());
        for (auto const& stop : stops) {
            colors.append(to_sk_color(stop.color));
            positions.append(stop.position);
        }

        SkPoint points[] { to_sk_point(header.start_point), to_sk_point(header.end_point) };
        paint.setShader(SkGradientShader::MakeLinear(points, colors.data(), positions.data(), static_cast<int>(positions.size()), SkTileMode::kClamp));
        return paint;
    }
    case PaintStyleType::CanvasRadialGradient: {
        auto header = TRY(read_command_struct<CanvasRadialGradientPayload>(paint_payload));
        auto stops = TRY(command_trailing_span<CanvasRadialGradientPayload, WireColorStop>(paint_payload, header.stop_count));

        Vector<SkColor> colors;
        Vector<SkScalar> positions;
        colors.ensure_capacity(stops.size());
        positions.ensure_capacity(stops.size());
        for (auto const& stop : stops) {
            colors.append(to_sk_color(stop.color));
            positions.append(stop.position);
        }

        paint.setShader(SkGradientShader::MakeTwoPointConical(to_sk_point(header.start_center), header.start_radius, to_sk_point(header.end_center), header.end_radius, colors.data(), positions.data(), static_cast<int>(positions.size()), SkTileMode::kClamp));
        return paint;
    }
    case PaintStyleType::CanvasConicGradient: {
        auto header = TRY(read_command_struct<CanvasConicGradientPayload>(paint_payload));
        auto stops = TRY(command_trailing_span<CanvasConicGradientPayload, WireColorStop>(paint_payload, header.stop_count));

        Vector<SkColor> colors;
        Vector<SkScalar> positions;
        colors.ensure_capacity(stops.size());
        positions.ensure_capacity(stops.size());
        for (auto const& stop : stops) {
            colors.append(to_sk_color(stop.color));
            positions.append(stop.position);
        }

        SkMatrix matrix;
        matrix.setRotate(AK::to_degrees(header.start_angle), header.center.x(), header.center.y());
        paint.setShader(SkGradientShader::MakeSweep(header.center.x(), header.center.y(), colors.data(), positions.data(), static_cast<int>(positions.size()), SkTileMode::kClamp, 0, 360, 0, &matrix));
        return paint;
    }
    case PaintStyleType::CanvasPattern: {
        auto header = TRY(read_command_struct<CanvasPatternPayload>(paint_payload));
        if (!draw_context.image_resolver)
            return Error::from_string_literal("Canvas pattern has no image resolver");

        auto sk_image = draw_context.image_resolver->operator()(header.image_resource_id, header.image_id);
        if (!sk_image)
            return Error::from_string_literal("Canvas pattern image is unavailable");

        SkTileMode tile_mode_x = header.repeat_x != 0 ? SkTileMode::kRepeat : SkTileMode::kDecal;
        SkTileMode tile_mode_y = header.repeat_y != 0 ? SkTileMode::kRepeat : SkTileMode::kDecal;
        Optional<SkMatrix> matrix;
        if (header.has_transform != 0)
            matrix = to_sk_matrix(header.transform);
        paint.setShader(sk_image->makeShader(tile_mode_x, tile_mode_y, SkSamplingOptions { SkFilterMode::kLinear }, matrix.has_value() ? &matrix.value() : nullptr));
        return paint;
    }
    }

    return Error::from_string_literal("Unsupported path paint type");
}

ErrorOr<void> DrawCommandPlayer::apply_fill_path(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<FillPathCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("FillPath: path_bounds={}x{}@{},{} paint_style_type={} opacity={} anti_alias={} winding_rule={}",
            command.path_bounding_rect.width(),
            command.path_bounding_rect.height(),
            command.path_bounding_rect.x(),
            command.path_bounding_rect.y(),
            command.paint_style_type,
            command.opacity,
            command.should_anti_alias,
            command.winding_rule);
    }
    ReadonlyBytes path_bytes = TRY(command_trailing_bytes_slice<FillPathCommand>(command_view, 0, command.path_byte_count));
    ReadonlyBytes filter_bytes = TRY(command_trailing_bytes_slice<FillPathCommand>(command_view, command.path_byte_count, command.filter_byte_count));
    ReadonlyBytes paint_bytes = TRY(command_trailing_bytes_slice<FillPathCommand>(command_view, command.path_byte_count + command.filter_byte_count, command.paint_style_byte_count));
    SkPath path = TRY(decode_sk_path(path_bytes));
    if (is_logging_enabled(LOG_DRAWING)) {
        SkRect path_bounds = path.getBounds();
        dbgln("FillPath decoded bounds={}x{}@{},{}",
            path_bounds.width(),
            path_bounds.height(),
            path_bounds.x(),
            path_bounds.y());
        log_canvas_state("FillPath state", m_canvas);
    }
    path.setFillType(Gfx::to_skia_path_fill_type(static_cast<Gfx::WindingRule>(command.winding_rule)));

    SkPaint paint = TRY(decode_path_paint(command.paint_style_type, command.color, paint_bytes, command.path_bounding_rect, m_draw_context));
    paint.setAntiAlias(command.should_anti_alias != 0);
    paint.setAlphaf(paint.getAlphaf() * command.opacity);
    if (is_valid_compositing_and_blending_operator(command.compositing_and_blending_operator)) {
        auto op = static_cast<Gfx::CompositingAndBlendingOperator>(command.compositing_and_blending_operator);
        if (op != Gfx::CompositingAndBlendingOperator::Normal)
            paint.setBlender(Gfx::to_skia_blender(op));
    }
    if (!filter_bytes.is_empty())
        paint.setImageFilter(TRY(decode_image_filter(m_draw_context, filter_bytes, "FillPath"sv)));
    if (command.blur_radius != 0.0f)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, command.blur_radius / 2.0f));
    m_canvas.drawPath(path, paint);
    return {};
}

ErrorOr<void> DrawCommandPlayer::apply_stroke_path(DrawCommandView const& command_view)
{
    auto command = TRY(decode_variable_size_command<StrokePathCommand>(command_view));
    if (is_logging_enabled(LOG_DRAWING)) {
        dbgln("StrokePath: path_bounds={}x{}@{},{} paint_style_type={} opacity={} thickness={} anti_alias={} dash_count={}",
            command.path_bounding_rect.width(),
            command.path_bounding_rect.height(),
            command.path_bounding_rect.x(),
            command.path_bounding_rect.y(),
            command.paint_style_type,
            command.opacity,
            command.thickness,
            command.should_anti_alias,
            command.dash_count);
    }
    size_t dash_bytes_size = static_cast<size_t>(command.dash_count) * sizeof(float);
    ReadonlySpan<float const> dash_array {};
    if (command.dash_count != 0)
        dash_array = TRY(command_trailing_span_slice<StrokePathCommand, float>(command_view.bytes, 0, command.dash_count));
    ReadonlyBytes path_bytes = TRY(command_trailing_bytes_slice<StrokePathCommand>(command_view, dash_bytes_size, command.path_byte_count));
    ReadonlyBytes filter_bytes = TRY(command_trailing_bytes_slice<StrokePathCommand>(command_view, dash_bytes_size + command.path_byte_count, command.filter_byte_count));
    ReadonlyBytes paint_bytes = TRY(command_trailing_bytes_slice<StrokePathCommand>(command_view, dash_bytes_size + command.path_byte_count + command.filter_byte_count, command.paint_style_byte_count));
    SkPath path = TRY(decode_sk_path(path_bytes));

    SkPaint paint = TRY(decode_path_paint(command.paint_style_type, command.color, paint_bytes, command.path_bounding_rect, m_draw_context));
    paint.setAntiAlias(command.should_anti_alias != 0);
    paint.setAlphaf(paint.getAlphaf() * command.opacity);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setStrokeCap(Gfx::to_skia_cap(static_cast<Gfx::Path::CapStyle>(command.cap_style)));
    paint.setStrokeJoin(Gfx::to_skia_join(static_cast<Gfx::Path::JoinStyle>(command.join_style)));
    paint.setStrokeMiter(command.miter_limit);
    if (is_valid_compositing_and_blending_operator(command.compositing_and_blending_operator)) {
        auto op = static_cast<Gfx::CompositingAndBlendingOperator>(command.compositing_and_blending_operator);
        if (op != Gfx::CompositingAndBlendingOperator::Normal)
            paint.setBlender(Gfx::to_skia_blender(op));
    }
    if (!filter_bytes.is_empty())
        paint.setImageFilter(TRY(decode_image_filter(m_draw_context, filter_bytes, "StrokePath"sv)));
    if (command.blur_radius != 0.0f)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, command.blur_radius / 2.0f));
    if (!dash_array.is_empty())
        paint.setPathEffect(SkDashPathEffect::Make(SkSpan<SkScalar const>(dash_array.data(), dash_array.size()), command.dash_offset));
    m_canvas.drawPath(path, paint);
    return {};
}

void DrawCommandPlayer::maybe_draw_magenta_rect(DrawCommandView const& command_view)
{
#define DRAW_FALLBACK_FOR_FIXED_SIZE(command_type, command_struct, rect_member)                                                            \
    case CommandType::command_type:                                                                                                        \
        if (command_view.bytes.size() == sizeof(command_struct))                                                                           \
            m_canvas.drawRect(to_sk_rect(reinterpret_cast<command_struct const*>(command_view.bytes.data())->rect_member), s_debug_paint); \
        return;

#define DRAW_FALLBACK_FOR_VARIABLE_SIZE(command_type, command_struct, rect_member)                                                         \
    case CommandType::command_type:                                                                                                        \
        if (command_view.bytes.size() >= sizeof(command_struct))                                                                           \
            m_canvas.drawRect(to_sk_rect(reinterpret_cast<command_struct const*>(command_view.bytes.data())->rect_member), s_debug_paint); \
        return;

    switch (command_view.type) {
        DRAW_FALLBACK_FOR_FIXED_SIZE(ClearRect, ClearRectCommand, rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(FillRect, FillRectCommand, rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(FillRectWithRoundedCorners, FillRectWithRoundedCornersCommand, rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(DrawRect, DrawRectCommand, rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(DrawEllipse, DrawEllipseCommand, rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(FillEllipse, FillEllipseCommand, rect)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(DrawScaledImage, DrawScaledImageCommand, dst_rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(PaintOuterBoxShadow, PaintOuterBoxShadowCommand, device_content_rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(PaintInnerBoxShadow, PaintInnerBoxShadowCommand, device_content_rect)
        DRAW_FALLBACK_FOR_FIXED_SIZE(DrawRepeatedImage, DrawRepeatedImageCommand, clip_rect)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(AddClipPath, AddClipPathCommand, bounding_rect)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(PaintTextShadow, PaintTextShadowCommand, visual_bounds)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(FillPath, FillPathCommand, path_bounding_rect)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(StrokePath, StrokePathCommand, path_bounding_rect)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(DrawGlyphRun, DrawGlyphRunCommand, visual_bounds)
        DRAW_FALLBACK_FOR_VARIABLE_SIZE(PaintLinearGradient, PaintLinearGradientCommand, gradient_rect)
    default:
        return;
    }
#undef DRAW_FALLBACK_FOR_VARIABLE_SIZE
#undef DRAW_FALLBACK_FOR_FIXED_SIZE
}

static SkColor to_sk_color(Gfx::Color color)
{
    return SkColorSetARGB(color.alpha(), color.red(), color.green(), color.blue());
}

static SkRect to_sk_rect(Gfx::FloatRect const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

static SkPoint to_sk_point(Gfx::FloatPoint const& point)
{
    return SkPoint::Make(point.x(), point.y());
}

static SkColor4f to_sk_color4f(Gfx::Color color)
{
    return SkColor4f {
        static_cast<float>(color.red()) / 255.0f,
        static_cast<float>(color.green()) / 255.0f,
        static_cast<float>(color.blue()) / 255.0f,
        static_cast<float>(color.alpha()) / 255.0f,
    };
}

static SkRRect to_sk_rrect(FillRectWithRoundedCornersCommand const& command)
{
    SkVector radii[4];
    radii[0].set(command.top_left.horizontal_radius > 0.0f ? command.top_left.horizontal_radius : 0.0f, command.top_left.vertical_radius > 0.0f ? command.top_left.vertical_radius : 0.0f);
    radii[1].set(command.top_right.horizontal_radius > 0.0f ? command.top_right.horizontal_radius : 0.0f, command.top_right.vertical_radius > 0.0f ? command.top_right.vertical_radius : 0.0f);
    radii[2].set(command.bottom_right.horizontal_radius > 0.0f ? command.bottom_right.horizontal_radius : 0.0f, command.bottom_right.vertical_radius > 0.0f ? command.bottom_right.vertical_radius : 0.0f);
    radii[3].set(command.bottom_left.horizontal_radius > 0.0f ? command.bottom_left.horizontal_radius : 0.0f, command.bottom_left.vertical_radius > 0.0f ? command.bottom_left.vertical_radius : 0.0f);

    SkRRect rounded_rect;
    rounded_rect.setRectRadii(to_sk_rect(command.rect), radii);
    return rounded_rect;
}

static SkRRect to_sk_rrect(Gfx::FloatRect const& rect, WireCornerRadii const& radii)
{
    SkVector sk_radii[4];
    sk_radii[0].set(radii.top_left.horizontal_radius > 0.0f ? radii.top_left.horizontal_radius : 0.0f, radii.top_left.vertical_radius > 0.0f ? radii.top_left.vertical_radius : 0.0f);
    sk_radii[1].set(radii.top_right.horizontal_radius > 0.0f ? radii.top_right.horizontal_radius : 0.0f, radii.top_right.vertical_radius > 0.0f ? radii.top_right.vertical_radius : 0.0f);
    sk_radii[2].set(radii.bottom_right.horizontal_radius > 0.0f ? radii.bottom_right.horizontal_radius : 0.0f, radii.bottom_right.vertical_radius > 0.0f ? radii.bottom_right.vertical_radius : 0.0f);
    sk_radii[3].set(radii.bottom_left.horizontal_radius > 0.0f ? radii.bottom_left.horizontal_radius : 0.0f, radii.bottom_left.vertical_radius > 0.0f ? radii.bottom_left.vertical_radius : 0.0f);

    SkRRect rounded_rect;
    rounded_rect.setRectRadii(to_sk_rect(rect), sk_radii);
    return rounded_rect;
}

static bool is_valid_compositing_and_blending_operator(u32 raw_value)
{
    return raw_value >= to_underlying(Gfx::CompositingAndBlendingOperator::Normal)
        && raw_value <= to_underlying(Gfx::CompositingAndBlendingOperator::PlusLighter);
}

static SkM44 to_skia_matrix4x4(ApplyTransformCommand const& command)
{
    return SkM44(
        command.matrix[0, 0], command.matrix[0, 1], command.matrix[0, 2], command.matrix[0, 3],
        command.matrix[1, 0], command.matrix[1, 1], command.matrix[1, 2], command.matrix[1, 3],
        command.matrix[2, 0], command.matrix[2, 1], command.matrix[2, 2], command.matrix[2, 3],
        command.matrix[3, 0], command.matrix[3, 1], command.matrix[3, 2], command.matrix[3, 3]);
}

static SkGradientShader::Interpolation to_skia_interpolation(GradientColorSpace color_space, GradientHueMethod hue_method)
{
    SkGradientShader::Interpolation interpolation;

    switch (color_space) {
    case GradientColorSpace::OKLab:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLab;
        break;
    case GradientColorSpace::sRGB:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGB;
        break;
    case GradientColorSpace::sRGBLinear:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGBLinear;
        break;
    case GradientColorSpace::Lab:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kLab;
        break;
    case GradientColorSpace::DisplayP3:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kDisplayP3;
        break;
    case GradientColorSpace::A98RGB:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kA98RGB;
        break;
    case GradientColorSpace::ProPhotoRGB:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kProphotoRGB;
        break;
    case GradientColorSpace::Rec2020:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kRec2020;
        break;
    case GradientColorSpace::HSL:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kHSL;
        break;
    case GradientColorSpace::HWB:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kHWB;
        break;
    case GradientColorSpace::LCH:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kLCH;
        break;
    case GradientColorSpace::OKLCH:
        interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLCH;
        break;
    }

    switch (hue_method) {
    case GradientHueMethod::Shorter:
        interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kShorter;
        break;
    case GradientHueMethod::Longer:
        interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kLonger;
        break;
    case GradientHueMethod::Increasing:
        interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kIncreasing;
        break;
    case GradientHueMethod::Decreasing:
        interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kDecreasing;
        break;
    }

    interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
    return interpolation;
}

static SkMatrix to_sk_matrix(Gfx::AffineTransform const& transform)
{
    return SkMatrix::MakeAll(
        transform.a(), transform.c(), transform.e(),
        transform.b(), transform.d(), transform.f(),
        0.0f, 0.0f, 1.0f);
}

static SkTileMode to_sk_tile_mode(SVGSpreadMethod spread_method)
{
    switch (spread_method) {
    case SVGSpreadMethod::Pad:
        return SkTileMode::kClamp;
    case SVGSpreadMethod::Repeat:
        return SkTileMode::kRepeat;
    case SVGSpreadMethod::Reflect:
        return SkTileMode::kMirror;
    }
    VERIFY_NOT_REACHED();
}

static void log_canvas_state(char const* label, SkCanvas const& canvas)
{
    SkM44 const matrix = canvas.getLocalToDevice();
    SkRect const local_clip = canvas.getLocalClipBounds();
    SkIRect const device_clip = canvas.getDeviceClipBounds();
    dbgln("{}: save_count={} matrix=[[{}, {}, {}, {}], [{}, {}, {}, {}], [{}, {}, {}, {}], [{}, {}, {}, {}]] local_clip={}x{}@{},{} device_clip={}x{}@{},{}",
        label,
        canvas.getSaveCount(),
        matrix.rc(0, 0), matrix.rc(0, 1), matrix.rc(0, 2), matrix.rc(0, 3),
        matrix.rc(1, 0), matrix.rc(1, 1), matrix.rc(1, 2), matrix.rc(1, 3),
        matrix.rc(2, 0), matrix.rc(2, 1), matrix.rc(2, 2), matrix.rc(2, 3),
        matrix.rc(3, 0), matrix.rc(3, 1), matrix.rc(3, 2), matrix.rc(3, 3),
        local_clip.width(),
        local_clip.height(),
        local_clip.x(),
        local_clip.y(),
        device_clip.width(),
        device_clip.height(),
        device_clip.x(),
        device_clip.y());
}

}
