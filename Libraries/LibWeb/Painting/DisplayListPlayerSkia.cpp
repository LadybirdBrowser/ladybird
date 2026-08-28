/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <core/SkBitmap.h>
#include <core/SkBlurTypes.h>
#include <core/SkCanvas.h>
#include <core/SkColorFilter.h>
#include <core/SkColorSpace.h>
#include <core/SkMaskFilter.h>
#include <core/SkPath.h>
#include <core/SkPathEffect.h>
#include <core/SkPicture.h>
#include <core/SkPictureRecorder.h>
#include <core/SkRRect.h>
#include <core/SkSurface.h>
#include <core/SkTextBlob.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradient.h>
#include <effects/SkImageFilters.h>
#include <effects/SkLumaColorFilter.h>
#include <effects/SkRuntimeEffect.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <pathops/SkPathOps.h>

#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibWeb/Painting/CanvasSurfaceRegistry.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

namespace Web::Painting {

DisplayListPlayerSkia::DisplayListPlayerSkia()
    : DisplayListPlayerSkia(Gfx::SkiaBackendContext::the_main_thread_context())
{
}

DisplayListPlayerSkia::DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
    : m_skia_backend_context(move(skia_backend_context))
{
}

DisplayListPlayerSkia::~DisplayListPlayerSkia()
{
}

void DisplayListPlayerSkia::execute(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceStorage const& resource_storage,
    ScrollStateSnapshot const& scroll_state_snapshot,
    RefPtr<Gfx::PaintingSurface> surface,
    CanvasSurfaceRegistry const* canvas_surface_registry,
    CompositedContextResolver const* composited_context_resolver)
{
    TemporaryChange composited_context_resolver_change { m_composited_context_resolver, composited_context_resolver };
    DisplayListPlayer::execute(
        display_list,
        visual_context_tree,
        resource_storage,
        scroll_state_snapshot,
        move(surface),
        canvas_surface_registry);
}

static SkRRect to_skia_rrect(auto const& rect, Gfx::CornerRadii const& corner_radii)
{
    SkRRect rrect;
    SkVector radii[4];
    radii[0].set(corner_radii.top_left.horizontal_radius, corner_radii.top_left.vertical_radius);
    radii[1].set(corner_radii.top_right.horizontal_radius, corner_radii.top_right.vertical_radius);
    radii[2].set(corner_radii.bottom_right.horizontal_radius, corner_radii.bottom_right.vertical_radius);
    radii[3].set(corner_radii.bottom_left.horizontal_radius, corner_radii.bottom_left.vertical_radius);
    rrect.setRectRadii(to_skia_rect(rect), radii);
    return rrect;
}

static void clip_to_rounded_rect(SkCanvas& canvas, auto const& rect, Gfx::CornerRadii const& corner_radii, SkClipOp clip_op)
{
    if (corner_radii.has_any_radius())
        canvas.clipRRect(to_skia_rrect(rect, corner_radii), clip_op, true);
    else
        canvas.clipRect(to_skia_rect(rect), clip_op, true);
}

static SkMatrix to_skia_matrix(Gfx::AffineTransform const& affine_transform)
{
    SkScalar affine[6];
    affine[0] = affine_transform.a();
    affine[1] = affine_transform.b();
    affine[2] = affine_transform.c();
    affine[3] = affine_transform.d();
    affine[4] = affine_transform.e();
    affine[5] = affine_transform.f();

    SkMatrix matrix;
    matrix.setAffine(affine);
    return matrix;
}
static SkM44 to_skia_matrix4x4(Gfx::FloatMatrix4x4 const& matrix)
{
    return SkM44(
        matrix[0, 0],
        matrix[0, 1],
        matrix[0, 2],
        matrix[0, 3],
        matrix[1, 0],
        matrix[1, 1],
        matrix[1, 2],
        matrix[1, 3],
        matrix[2, 0],
        matrix[2, 1],
        matrix[2, 2],
        matrix[2, 3],
        matrix[3, 0],
        matrix[3, 1],
        matrix[3, 2],
        matrix[3, 3]);
}

static Gfx::FloatMatrix4x4 to_gfx_matrix4x4(SkM44 const& matrix)
{
    Gfx::FloatMatrix4x4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            result[row, column] = matrix.rc(row, column);
    }
    return result;
}

void DisplayListPlayerSkia::flush(Gfx::PaintingSurface& surface)
{
    if (auto context = surface.skia_backend_context())
        context->flush_and_submit(&surface.sk_surface());
    surface.flush();
}

void DisplayListPlayerSkia::flush_async(Gfx::PaintingSurface& surface, Function<void()>&& callback)
{
    if (auto context = surface.skia_backend_context())
        context->flush_and_submit_async(&surface.sk_surface(), move(callback));
    else
        callback();
    surface.flush();
}

static void paint_scrollbar_into_surface(Gfx::PaintingSurface& surface, PaintScrollBar const& command)
{
    auto gutter_rect = to_skia_rect(command.gutter_rect);

    auto thumb_rect = to_skia_rect(command.thumb_rect);
    auto radius = thumb_rect.width() / 2;
    auto thumb_rrect = SkRRect::MakeRectXY(thumb_rect, radius, radius);

    auto& canvas = surface.canvas();

    auto gutter_fill_color = command.track_color;
    SkPaint gutter_fill_paint;
    gutter_fill_paint.setColor(to_skia_color(gutter_fill_color));
    canvas.drawRect(gutter_rect, gutter_fill_paint);

    SkPaint thumb_fill_paint;
    thumb_fill_paint.setColor(to_skia_color(command.thumb_color));
    canvas.drawRRect(thumb_rrect, thumb_fill_paint);

    auto stroke_color = command.thumb_color.lightened();
    SkPaint stroke_paint;
    stroke_paint.setStroke(true);
    stroke_paint.setStrokeWidth(1);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setColor(to_skia_color(stroke_color));
    canvas.drawRRect(thumb_rrect, stroke_paint);
}

void DisplayListPlayerSkia::paint_scrollbar(Gfx::PaintingSurface& surface, PaintScrollBar const& command)
{
    paint_scrollbar_into_surface(surface, command);
}

void DisplayListPlayerSkia::play_command(DrawGlyphRun const& command)
{
    auto glyphs = inline_objects<DisplayListGlyph>(command.glyphs);
    if (glyphs.is_empty())
        return;

    auto blob = resource_storage().text_blob(command.font_id, command.scale, glyphs);
    if (!blob)
        return;

    SkPaint paint;
    paint.setColor(to_skia_color(command.color));

    auto& canvas = surface().canvas();
    auto const& translation = command.translation;

    switch (command.orientation) {
    case Gfx::Orientation::Horizontal:
        canvas.drawTextBlob(blob.get(), translation.x(), translation.y(), paint);
        break;
    case Gfx::Orientation::Vertical:
        canvas.save();
        canvas.translate(command.rect.width(), 0);
        canvas.rotate(90, command.rect.top_left().x(), command.rect.top_left().y());
        canvas.drawTextBlob(blob.get(), translation.x(), translation.y(), paint);
        canvas.restore();
        break;
    }
}

static void apply_compositing_and_blending_operator(SkPaint& paint, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    if (compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
        paint.setBlender(Gfx::to_skia_blender(compositing_and_blending_operator));
}

void DisplayListPlayerSkia::play_command(FillRect const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    auto color = command.background_color_animation_frame == NO_FRAME_NODE
        ? command.color
        : active_visual_context_tree().sampled_background_color(command.background_color_animation_frame).value_or(command.color);
    paint.setColor(to_skia_color(color));
    apply_compositing_and_blending_operator(paint, command.compositing_and_blending_operator);
    canvas.drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::play_command(PaintCaret const& command)
{
    if (!caret_is_visible_at_time(command, MonotonicTime::now().nanoseconds()))
        return;

    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(command.rect), paint);
}

void DisplayListPlayerSkia::play_command(DrawCompositedContext const& command)
{
    if (!m_composited_context_resolver)
        return;

    auto composited_context_surface = (*m_composited_context_resolver)(command.child_context_id);
    if (!composited_context_surface)
        return;

    auto image = composited_context_surface->sk_image_snapshot<sk_sp<SkImage>>();
    if (!image)
        return;

    auto dst_rect = to_skia_rect(command.dst_rect);
    SkRect src_rect = SkRect::MakeIWH(image->width(), image->height());
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.drawImageRect(image.get(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void DisplayListPlayerSkia::play_command(DrawCanvas const& command)
{
    auto const* registry = canvas_surface_registry();
    if (!registry)
        return;

    auto* canvas_surface = registry->canvas_surface(command.canvas_id);
    if (!canvas_surface)
        return;

    auto image = canvas_surface->sk_image_snapshot<sk_sp<SkImage>>();
    if (!image)
        return;

    auto dst_rect = to_skia_rect(command.dst_rect);
    SkRect src_rect = SkRect::MakeIWH(image->width(), image->height());
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.drawImageRect(image.get(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void DisplayListPlayerSkia::play_command(DrawVideoFrame const& command)
{
    auto image = resource_storage().skia_image_for_video_sink(command.video_sink_id, m_skia_backend_context);
    if (!image)
        return;

    auto dst_rect = to_skia_rect(command.dst_rect);
    SkRect src_rect = SkRect::MakeIWH(image->width(), image->height());
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.drawImageRect(image.get(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

// Images invert lightness in Oklab, same as solid colors. Skia's high-contrast filter inverts through HSL, which sends
// white to black where a fill of that same white lands on the dark surface color. A page with both would show the seam.
sk_sp<SkColorFilter> force_dark_image_color_filter()
{
    // Compiled once and deliberately leaked: it lives as long as the process, and a static with a destructor here would
    // be an exit-time destructor. The filter takes no uniforms, so one instance serves every draw.
    static auto* filter = [] {
        auto result = SkRuntimeEffect::MakeForColorFilter(SkString(R"(
            const float LIGHTNESS_LIFT = 1.20;
            const float GRAY_BRIGHTNESS_THRESHOLD = 32.0 / 255.0;
            const float GRAY_ADJUSTED_BRIGHTNESS = 18.0 / 255.0;

            float3 to_linear(float3 c) {
                return mix(c / 12.92, pow((c + 0.055) / 1.055, float3(2.4)), step(float3(0.04045), c));
            }

            float3 to_srgb(float3 c) {
                return mix(c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, step(float3(0.0031308), c));
            }

            half4 main(half4 color) {
                // A runtime color filter is handed premultiplied color and must return it that way, but the
                // transfer functions below only make sense on the color as authored.
                if (color.a <= 0.0)
                    return color;
                float3 rgb = float3(color.rgb) / float(color.a);

                float3 lin = to_linear(rgb);
                float3 lms = pow(float3(
                    0.41222146 * lin.r + 0.53633255 * lin.g + 0.051445995 * lin.b,
                    0.2119035  * lin.r + 0.6806995  * lin.g + 0.10739696  * lin.b,
                    0.08830246 * lin.r + 0.28171885 * lin.g + 0.6299787   * lin.b), float3(1.0 / 3.0));

                float l = 0.21045426  * lms.x + 0.7936178   * lms.y - 0.004072047 * lms.z;
                float a = 1.9779985   * lms.x - 2.4285922   * lms.y + 0.4505937   * lms.z;
                float b = 0.025904037 * lms.x + 0.78277177  * lms.y - 0.80867577  * lms.z;

                l = min(LIGHTNESS_LIFT - l, 1.0);

                float3 back = float3(
                    l + 0.39633778  * a + 0.21580376 * b,
                    l - 0.105561346 * a - 0.06385417 * b,
                    l - 0.08948418  * a - 1.2914855  * b);
                back = back * back * back;

                // The lift pushes saturated colors out of sRGB, so this matrix can return negative linear components,
                // and those can't reach the transfer function: mix() evaluates both of its arms, so pow() would see the
                // negative base and yield NaN even though step() then picks the linear arm. This clamp is the
                // per-channel gamut clip; the outer one only guards the transfer function's rounding at 1.0.
                float3 inverted = clamp(to_srgb(clamp(float3(
                     4.0767417    * back.x - 3.3077116 * back.y + 0.23096994 * back.z,
                    -1.268438     * back.x + 2.6097574 * back.y - 0.34131938 * back.z,
                    -0.0041960863 * back.x - 0.7034186 * back.y + 1.7076147  * back.z), 0.0, 1.0)), 0.0, 1.0);

                // Grays landing in the band snap to one dark surface color, rather than a spread of near-blacks.
                float epsilon = 1.0 / 255.0;
                if (abs(inverted.r - inverted.g) <= epsilon && abs(inverted.r - inverted.b) <= epsilon
                    && inverted.r < GRAY_BRIGHTNESS_THRESHOLD && inverted.r > GRAY_ADJUSTED_BRIGHTNESS) {
                    inverted = float3(GRAY_ADJUSTED_BRIGHTNESS);
                }

                return half4(half3(inverted) * color.a, color.a);
            }
        )"));
        VERIFY(result.effect);
        return result.effect->makeColorFilter(nullptr).release();
    }();

    return sk_ref_sp(filter);
}

void DisplayListPlayerSkia::play_command(DrawScaledDecodedImageFrame const& command)
{
    auto image = resource_storage().skia_image_for_image_frame(command.frame_id, m_skia_backend_context);
    if (!image)
        return;

    auto dst_rect = to_skia_rect(command.dst_rect);
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    auto should_force_dark = command.apply_force_dark && resource_storage().image_frame_should_force_dark(command.frame_id);
    if (command.isolated_backdrop_color.has_value()) {
        auto src_rect = command.src_rect.value_or(Gfx::FloatRect { 0, 0, static_cast<float>(image->width()), static_cast<float>(image->height()) });
        SkMatrix matrix;
        matrix.setScale(dst_rect.width() / src_rect.width(), dst_rect.height() / src_rect.height());
        matrix.postTranslate(dst_rect.x() - src_rect.x() * dst_rect.width() / src_rect.width(), dst_rect.y() - src_rect.y() * dst_rect.height() / src_rect.height());
        auto image_shader = image->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, to_skia_sampling_options(command.scaling_mode), matrix);
        // The backdrop arrives already resolved by the recorder, so only the image gets the filter; filtering the
        // blended result would run the backdrop through the image transform on top of its own.
        if (should_force_dark)
            image_shader = image_shader->makeWithColorFilter(force_dark_image_color_filter());
        auto backdrop_shader = SkShaders::Color(to_skia_color(command.isolated_backdrop_color.value()));
        paint.setShader(SkShaders::Blend(Gfx::to_skia_blender(command.compositing_and_blending_operator), move(backdrop_shader), move(image_shader)));
    } else {
        if (should_force_dark)
            paint.setColorFilter(force_dark_image_color_filter());
        if (command.compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
            paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));
    }
    canvas.save();
    canvas.clipRect(dst_rect, true);
    if (command.isolated_backdrop_color.has_value()) {
        canvas.drawRect(dst_rect, paint);
    } else if (command.src_rect.has_value()) {
        auto src_rect = to_skia_rect(command.src_rect.value());
        canvas.drawImageRect(image.get(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
    } else {
        canvas.drawImageRect(image.get(), dst_rect, to_skia_sampling_options(command.scaling_mode), &paint);
    }
    canvas.restore();
}

void DisplayListPlayerSkia::play_command(DrawRepeatedDecodedImageFrame const& command)
{
    auto const& frame = resource_storage().image_frame(command.frame_id);
    auto image = resource_storage().skia_image_for_image_frame(command.frame_id, m_skia_backend_context);
    if (!image)
        return;

    SkMatrix matrix;
    auto dst_rect = command.dst_rect.to_type<float>();
    auto src_size = frame.size().to_type<float>();
    matrix.setScale(dst_rect.width() / src_size.width(), dst_rect.height() / src_size.height());
    matrix.postTranslate(dst_rect.x(), dst_rect.y());
    auto sampling_options = to_skia_sampling_options(command.scaling_mode);

    auto tile_mode_x = command.repeat.x ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto tile_mode_y = command.repeat.y ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto shader = image->makeShader(tile_mode_x, tile_mode_y, sampling_options, matrix);

    SkPaint paint;
    paint.setAntiAlias(true);
    auto should_force_dark = command.apply_force_dark && resource_storage().image_frame_should_force_dark(command.frame_id);
    if (command.isolated_backdrop_color.has_value()) {
        // The backdrop arrives already resolved by the recorder, so only the image gets the filter; filtering the
        // blended result would run the backdrop through the image transform on top of its own.
        if (should_force_dark)
            shader = shader->makeWithColorFilter(force_dark_image_color_filter());
        auto backdrop_shader = SkShaders::Color(to_skia_color(command.isolated_backdrop_color.value()));
        paint.setShader(SkShaders::Blend(Gfx::to_skia_blender(command.compositing_and_blending_operator), move(backdrop_shader), move(shader)));
    } else {
        if (should_force_dark)
            paint.setColorFilter(force_dark_image_color_filter());
        paint.setShader(shader);
    }
    if (!command.isolated_backdrop_color.has_value() && command.compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
        paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));
    auto& canvas = surface().canvas();
    canvas.drawPaint(paint);
}

static void paint_repeated_image(SkCanvas& canvas, SkImage& image, Gfx::IntRect const& dst_rect, Gfx::ScalingMode scaling_mode, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, bool repeat_x, bool repeat_y)
{
    SkMatrix matrix;
    matrix.setTranslate(dst_rect.x(), dst_rect.y());

    auto tile_mode_x = repeat_x ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto tile_mode_y = repeat_y ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto sampling_options = to_skia_sampling_options(scaling_mode);
    auto shader = image.makeShader(tile_mode_x, tile_mode_y, sampling_options, &matrix);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(shader);
    apply_compositing_and_blending_operator(paint, compositing_and_blending_operator);
    canvas.drawPaint(paint);
}

void DisplayListPlayerSkia::play_command(DrawTiledDecodedImageFrame const& command)
{
    auto image = resource_storage().skia_image_for_image_frame(command.frame_id, m_skia_backend_context);
    if (!image)
        return;

    auto sampling_options = to_skia_sampling_options(command.scaling_mode);

    auto scale_x = command.tile_rect.width() / command.src_rect.width();
    auto scale_y = command.tile_rect.height() / command.src_rect.height();
    auto tile_step_width = command.tile_step.width();
    auto tile_step_height = command.tile_step.height();

    SkPictureRecorder tile_recorder;
    auto tile_bounds = SkRect::MakeWH(tile_step_width / scale_x, tile_step_height / scale_y);
    auto* tile_canvas = tile_recorder.beginRecording(tile_bounds);
    SkPaint tile_paint;
    tile_paint.setAntiAlias(true);
    if (command.apply_force_dark && resource_storage().image_frame_should_force_dark(command.frame_id))
        tile_paint.setColorFilter(force_dark_image_color_filter());
    tile_canvas->drawImageRect(
        image.get(),
        to_skia_rect(command.src_rect),
        SkRect::MakeWH(command.src_rect.width(), command.src_rect.height()),
        sampling_options,
        &tile_paint,
        SkCanvas::kStrict_SrcRectConstraint);
    auto tile_picture = tile_recorder.finishRecordingAsPicture();

    SkMatrix matrix;
    matrix.setTranslate(command.tile_rect.x(), command.tile_rect.y());
    matrix.preScale(scale_x, scale_y);

    auto is_single_tile_axis = [](Optional<u32> const& tile_count) {
        return tile_count.has_value() && tile_count.value() == 1;
    };
    auto tile_mode_x = is_single_tile_axis(command.tile_count_x) ? SkTileMode::kDecal : SkTileMode::kRepeat;
    auto tile_mode_y = is_single_tile_axis(command.tile_count_y) ? SkTileMode::kDecal : SkTileMode::kRepeat;
    auto filter_mode = [&] {
        switch (command.scaling_mode) {
        case Gfx::ScalingMode::None:
        case Gfx::ScalingMode::NearestNeighbor:
            return SkFilterMode::kNearest;
        case Gfx::ScalingMode::Bilinear:
        case Gfx::ScalingMode::BilinearMipmap:
            return SkFilterMode::kLinear;
        }
        VERIFY_NOT_REACHED();
    }();

    auto shader = tile_picture->makeShader(tile_mode_x, tile_mode_y, filter_mode, &matrix, &tile_bounds);

    auto pattern_left = command.tile_count_x.has_value() ? command.tile_rect.left() : static_cast<float>(command.clip_rect.left());
    auto pattern_top = command.tile_count_y.has_value() ? command.tile_rect.top() : static_cast<float>(command.clip_rect.top());
    auto pattern_right = command.tile_count_x.has_value()
        ? command.tile_rect.left() + (command.tile_count_x.value() - 1) * command.tile_step.width() + command.tile_rect.width()
        : static_cast<float>(command.clip_rect.right());
    auto pattern_bottom = command.tile_count_y.has_value()
        ? command.tile_rect.top() + (command.tile_count_y.value() - 1) * command.tile_step.height() + command.tile_rect.height()
        : static_cast<float>(command.clip_rect.bottom());
    Gfx::FloatRect pattern_rect { pattern_left, pattern_top, pattern_right - pattern_left, pattern_bottom - pattern_top };
    pattern_rect.intersect(command.clip_rect.to_type<float>());
    if (pattern_rect.is_empty())
        return;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(shader);

    auto& canvas = surface().canvas();
    canvas.drawRect(to_skia_rect(pattern_rect), paint);
}

void DisplayListPlayerSkia::play_command(DrawRepeatedDisplayList const& command)
{
    auto tile_size = command.dst_rect.size();
    if (tile_size.is_empty())
        return;

    if (auto image = resource_storage().cached_skia_image_for_display_list(command.display_list_id, tile_size, m_skia_backend_context)) {
        paint_repeated_image(surface().canvas(), *image, command.dst_rect, command.scaling_mode, command.compositing_and_blending_operator, command.repeat.x, command.repeat.y);
        return;
    }

    auto tile_surface = Gfx::PaintingSurface::create_with_size(tile_size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia_backend_context);
    Gfx::PainterSkia painter { tile_surface };
    painter.clear_rect(tile_surface->rect().to_type<float>(), Gfx::Color::Transparent);
    auto const& tile_display_list = resource_storage().display_list_resource(command.display_list_id);
    execute_display_list_into_surface(*tile_display_list.display_list, tile_display_list.visual_context_tree, *tile_surface);
    auto image = tile_surface->sk_surface().makeImageSnapshot();
    if (!image)
        return;

    resource_storage().set_cached_skia_image_for_display_list(command.display_list_id, tile_size, m_skia_backend_context, image);

    paint_repeated_image(surface().canvas(), *image, command.dst_rect, command.scaling_mode, command.compositing_and_blending_operator, command.repeat.x, command.repeat.y);
}

static SkGradient::Interpolation to_skia_interpolation(Gfx::GradientInterpolationMethod interpolation_method)
{
    SkGradient::Interpolation interpolation;

    if (interpolation_method.type == Gfx::GradientInterpolationMethod::Type::Rectangular) {
        switch (interpolation_method.rectangular_color_space) {
        case Gfx::RectangularColorSpace::Srgb:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kSRGB;
            break;
        case Gfx::RectangularColorSpace::SrgbLinear:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kSRGBLinear;
            break;
        case Gfx::RectangularColorSpace::Lab:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kLab;
            break;
        case Gfx::RectangularColorSpace::Oklab:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kOKLab;
            break;
        case Gfx::RectangularColorSpace::DisplayP3:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kDisplayP3;
            break;
        case Gfx::RectangularColorSpace::A98Rgb:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kA98RGB;
            break;
        case Gfx::RectangularColorSpace::ProphotoRgb:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kProphotoRGB;
            break;
        case Gfx::RectangularColorSpace::Rec2020:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kRec2020;
            break;
        case Gfx::RectangularColorSpace::DisplayP3Linear:
        case Gfx::RectangularColorSpace::XyzD50:
        case Gfx::RectangularColorSpace::XyzD65:
            dbgln("FIXME: Unsupported gradient color space");
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kOKLab;
            break;
        case Gfx::RectangularColorSpace::Xyz:
            VERIFY_NOT_REACHED();
        }
    } else {
        switch (interpolation_method.polar_color_space) {
        case Gfx::PolarColorSpace::Hsl:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kHSL;
            break;
        case Gfx::PolarColorSpace::Hwb:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kHWB;
            break;
        case Gfx::PolarColorSpace::Lch:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kLCH;
            break;
        case Gfx::PolarColorSpace::Oklch:
            interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kOKLCH;
            break;
        }

        switch (interpolation_method.hue_interpolation_method) {
        case Gfx::HueInterpolationMethod::Shorter:
            interpolation.fHueMethod = SkGradient::Interpolation::HueMethod::kShorter;
            break;
        case Gfx::HueInterpolationMethod::Longer:
            interpolation.fHueMethod = SkGradient::Interpolation::HueMethod::kLonger;
            break;
        case Gfx::HueInterpolationMethod::Increasing:
            interpolation.fHueMethod = SkGradient::Interpolation::HueMethod::kIncreasing;
            break;
        case Gfx::HueInterpolationMethod::Decreasing:
            interpolation.fHueMethod = SkGradient::Interpolation::HueMethod::kDecreasing;
            break;
        }
    }

    interpolation.fInPremul = SkGradient::Interpolation::InPremul::kYes;
    return interpolation;
}

ReadonlySpan<Color> DisplayListPlayerSkia::gradient_colors(DisplayListGradientColorStops color_stops) const
{
    return inline_objects<Color>(color_stops.colors);
}

ReadonlySpan<float> DisplayListPlayerSkia::gradient_positions(DisplayListGradientColorStops color_stops) const
{
    return inline_objects<float>(color_stops.positions);
}

static Vector<SkColor4f> to_skia_gradient_colors(ReadonlySpan<Color> color_stop_colors)
{
    Vector<SkColor4f> colors;
    colors.ensure_capacity(color_stop_colors.size());
    for (auto color : color_stop_colors)
        colors.unchecked_append(to_skia_color4f(color));
    return colors;
}

void DisplayListPlayerSkia::play_command(PaintLinearGradient const& command)
{
    auto color_stop_colors = gradient_colors(command.color_stops);
    auto color_stop_positions = gradient_positions(command.color_stops);
    VERIFY(!color_stop_colors.is_empty());

    auto colors = to_skia_gradient_colors(color_stop_colors);

    auto rect = command.gradient_rect.to_type<float>();
    auto length = calculate_gradient_length<float>(rect.size(), command.gradient_angle);

    // Starting and ending points before rotation (0deg / "to top")
    auto rect_center = rect.center();
    auto start = rect_center.translated(0, (.5f - command.first_stop_position) * length);
    auto end = start.translated(0, command.repeat_length * -length);
    Array const points { to_skia_point(start), to_skia_point(end) };

    SkMatrix matrix;
    matrix.setRotate(command.gradient_angle, rect_center.x(), rect_center.y());

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(command.interpolation_method);
    SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { color_stop_positions.data(), color_stop_positions.size() }, SkTileMode::kRepeat, color_space }, interpolation };
    auto shader = SkShaders::LinearGradient(points.data(), gradient, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setShader(shader);
    apply_compositing_and_blending_operator(paint, command.compositing_and_blending_operator);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::play_command(PaintOuterBoxShadow const& command)
{
    auto content_rrect = to_skia_rrect(command.device_content_rect, command.content_corner_radii);

    auto& canvas = surface().canvas();
    canvas.save();
    canvas.clipRRect(content_rrect, SkClipOp::kDifference, true);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, command.blur_radius / 2));
    auto shadow_rounded_rect = to_skia_rrect(command.shadow_rect, command.shadow_corner_radii);
    canvas.drawRRect(shadow_rounded_rect, paint);
    canvas.restore();
}

void DisplayListPlayerSkia::play_command(PaintInnerBoxShadow const& command)
{
    auto outer_rect = to_skia_rrect(command.outer_shadow_rect, command.content_corner_radii);
    auto inner_rect = to_skia_rrect(command.inner_shadow_rect, command.inner_shadow_corner_radii);

    auto outer_path = SkPath::RRect(outer_rect);
    auto inner_path = SkPath::RRect(inner_rect);

    auto result = Op(outer_path, inner_path, SkPathOp::kDifference_SkPathOp);
    if (!result.has_value()) {
        VERIFY_NOT_REACHED();
    }
    auto result_path = *std::move(result);

    auto& canvas = surface().canvas();
    SkPaint path_paint;
    path_paint.setAntiAlias(true);
    path_paint.setColor(to_skia_color(command.color));
    path_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, command.blur_radius / 2));
    canvas.save();
    canvas.clipRRect(to_skia_rrect(command.device_content_rect, command.content_corner_radii), true);
    canvas.drawPath(result_path, path_paint);
    canvas.restore();
}

void DisplayListPlayerSkia::play_command(PaintTextShadow const& command)
{
    auto& canvas = surface().canvas();
    auto blur_image_filter = SkImageFilters::Blur(command.blur_radius / 2, command.blur_radius / 2, nullptr);
    SkPaint blur_paint;
    blur_paint.setImageFilter(blur_image_filter);
    // Skia paints color glyphs with their own colors, ignoring the paint color, so tint the shadow layer with a kSrcIn
    // blend to force a flat silhouette in the shadow color. The glyphs are drawn opaquely below so the shadow color's
    // alpha is applied exactly once.
    blur_paint.setColorFilter(SkColorFilters::Blend(to_skia_color(command.color), SkBlendMode::kSrcIn));
    canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, &blur_paint, nullptr, 0));
    play_command(DrawGlyphRun { .font_id = command.font_id,
        .glyphs = command.glyphs,
        .rect = command.text_rect,
        .glyph_bounding_rect = command.shadow_bounding_rect,
        .translation = command.draw_location + command.text_rect.location().to_type<float>(),
        .scale = command.scale,
        .color = command.color.with_alpha(255),
        .orientation = Gfx::Orientation::Horizontal });
    canvas.restore();
}

void DisplayListPlayerSkia::play_command(FillRectWithRoundedCorners const& command)
{
    auto const& rect = command.rect;

    auto& canvas = surface().canvas();
    SkPaint paint;
    auto color = command.background_color_animation_frame == NO_FRAME_NODE
        ? command.color
        : active_visual_context_tree().sampled_background_color(command.background_color_animation_frame).value_or(command.color);
    paint.setColor(to_skia_color(color));
    paint.setAntiAlias(true);

    auto rounded_rect = to_skia_rrect(rect, command.corner_radii);
    canvas.drawRRect(rounded_rect, paint);
}

static SkTileMode to_skia_tile_mode(DisplayListGradientSpreadMethod spread_method)
{
    switch (spread_method) {
    case DisplayListGradientSpreadMethod::Pad:
        return SkTileMode::kClamp;
    case DisplayListGradientSpreadMethod::Reflect:
        return SkTileMode::kMirror;
    case DisplayListGradientSpreadMethod::Repeat:
        return SkTileMode::kRepeat;
    default:
        VERIFY_NOT_REACHED();
    }
}

template<typename MakeShader>
static SkPaint gradient_paint_style_to_skia_paint(
    DisplayListGradientPaintStyle const& paint_style,
    ReadonlySpan<Color> color_stop_colors,
    ReadonlySpan<float> color_stop_positions,
    Gfx::FloatRect const& bounding_rect,
    MakeShader make_shader)
{
    SkPaint paint;

    VERIFY(color_stop_colors.size() == color_stop_positions.size());

    Vector<SkColor4f> colors;
    colors.ensure_capacity(color_stop_colors.size());
    Vector<SkScalar> positions;
    positions.ensure_capacity(color_stop_positions.size());

    for (auto color : color_stop_colors)
        colors.unchecked_append(to_skia_color4f(color));
    for (auto position : color_stop_positions)
        positions.unchecked_append(position);

    SkMatrix matrix;
    matrix.setTranslate(bounding_rect.x(), bounding_rect.y());
    if (paint_style.gradient_transform.has_value())
        matrix = matrix * to_skia_matrix(paint_style.gradient_transform.value());

    auto tile_mode = to_skia_tile_mode(paint_style.spread_method);

    paint.setShader(make_shader(colors, positions, tile_mode, matrix));
    if (paint_style.color_space == Gfx::InterpolationColorSpace::LinearRGB) {
        paint.setColorFilter(SkColorFilters::LinearToSRGBGamma());
    }

    return paint;
}

SkPaint DisplayListPlayerSkia::paint_style_to_skia_paint(DisplayListPaintStyle const& paint_style, Gfx::FloatRect const& bounding_rect)
{
    auto make_gradient_paint = [&](auto make_shader) {
        return gradient_paint_style_to_skia_paint(
            paint_style.gradient,
            gradient_colors(paint_style.gradient.color_stops),
            gradient_positions(paint_style.gradient.color_stops),
            bounding_rect,
            make_shader);
    };

    switch (paint_style.paint_style_type) {
    case DisplayListPaintStyleType::None:
        return {};
    case DisplayListPaintStyleType::LinearGradient:
        return make_gradient_paint([&](Vector<SkColor4f> const& colors, Vector<SkScalar> const& positions, SkTileMode tile_mode, SkMatrix const& matrix) {
            Array points {
                to_skia_point(paint_style.linear_gradient_start_point),
                to_skia_point(paint_style.linear_gradient_end_point),
            };
            SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { positions.data(), positions.size() }, tile_mode }, {} };
            return SkShaders::LinearGradient(points.data(), gradient, &matrix);
        });
    case DisplayListPaintStyleType::RadialGradient:
        return make_gradient_paint([&](Vector<SkColor4f> const& colors, Vector<SkScalar> const& positions, SkTileMode tile_mode, SkMatrix const& matrix) {
            auto start_center = to_skia_point(paint_style.radial_gradient_start_center);
            auto end_center = to_skia_point(paint_style.radial_gradient_end_center);
            SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { positions.data(), positions.size() }, tile_mode }, {} };
            return SkShaders::TwoPointConicalGradient(start_center, paint_style.radial_gradient_start_radius, end_center, paint_style.radial_gradient_end_radius, gradient, &matrix);
        });
    case DisplayListPaintStyleType::Pattern: {
        auto const& tile_rect = paint_style.pattern_tile_rect;
        auto content_scale = paint_style.pattern_content_scale;
        auto tile_size = Gfx::IntSize(ceilf(tile_rect.width() * content_scale.width()), ceilf(tile_rect.height() * content_scale.height()));
        if (tile_size.is_empty())
            return {};

        auto tile_surface = Gfx::PaintingSurface::create_with_size(tile_size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia_backend_context);

        auto const& tile_display_list = resource_storage().display_list_resource(paint_style.pattern_tile_display_list_id);
        execute_display_list_into_surface(*tile_display_list.display_list, tile_display_list.visual_context_tree, *tile_surface);

        auto image = tile_surface->sk_surface().makeImageSnapshot();

        SkMatrix matrix;
        matrix.setTranslate(tile_rect.x(), tile_rect.y());

        matrix.preScale(tile_rect.width() / tile_size.width(), tile_rect.height() / tile_size.height());
        if (paint_style.pattern_transform.has_value())
            matrix = matrix * to_skia_matrix(paint_style.pattern_transform.value());

        auto sampling = SkSamplingOptions(SkFilterMode::kLinear);
        auto shader = image->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, sampling, &matrix);

        SkPaint paint;
        paint.setShader(shader);
        return paint;
    }
    }
    VERIFY_NOT_REACHED();
}

Gfx::Path DisplayListPlayerSkia::path_from_data(DisplayListDataSpan path_data) const
{
    auto bytes = inline_data(path_data);
    return Gfx::Path::from_serialized_bytes(bytes);
}

void DisplayListPlayerSkia::play_command(FillPath const& command)
{
    auto path = Gfx::to_skia_path(path_from_data(command.path_data));
    path.setFillType(to_skia_path_fill_type(command.winding_rule));

    SkPaint paint;
    if (command.paint_kind == PathPaintKind::PaintStyle) {
        paint = paint_style_to_skia_paint(command.paint_style, command.path_bounding_rect);
        paint.setAlphaf(command.opacity);
    } else {
        paint.setColor(to_skia_color(command.color));
    }
    paint.setAntiAlias(command.should_anti_alias == Gfx::ShouldAntiAlias::Yes);
    apply_compositing_and_blending_operator(paint, command.compositing_and_blending_operator);
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::play_command(StrokePath const& command)
{
    auto path = Gfx::to_skia_path(path_from_data(command.path_data));
    SkPaint paint;
    if (command.paint_kind == PathPaintKind::PaintStyle) {
        paint = paint_style_to_skia_paint(command.paint_style, command.path_bounding_rect);
        paint.setAlphaf(command.opacity);
    } else {
        paint.setColor(to_skia_color(command.color));
    }
    paint.setAntiAlias(command.should_anti_alias == Gfx::ShouldAntiAlias::Yes);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setStrokeCap(to_skia_cap(command.cap_style));
    paint.setStrokeJoin(to_skia_join(command.join_style));
    paint.setStrokeMiter(command.miter_limit);
    auto dash_array = inline_objects<float>(command.dash_array);
    paint.setPathEffect(SkDashPathEffect::Make({ dash_array.data(), dash_array.size() }, command.dash_offset));
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::play_command(DrawEllipse const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    canvas.drawOval(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::play_command(DrawLine const& command)
{
    auto from = to_skia_point(command.from);
    auto to = to_skia_point(command.to);
    auto& canvas = surface().canvas();

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));

    switch (command.style) {
    case Gfx::LineStyle::Solid:
        break;
    case Gfx::LineStyle::Dotted: {
        auto length = command.to.distance_from(command.from);
        auto dot_count = floor(length / (static_cast<float>(command.thickness) * 2));
        auto interval = length / dot_count;
        SkScalar intervals[] = { 0, interval };
        paint.setPathEffect(SkDashPathEffect::Make(intervals, 0));
        paint.setStrokeCap(SkPaint::Cap::kRound_Cap);

        // NOTE: As Skia doesn't render a dot exactly at the end of a line, we need
        //       to extend it by less then an interval.
        auto direction = to - from;
        direction.normalize();
        to += direction * (interval / 2.0f);
        break;
    }
    case Gfx::LineStyle::Dashed: {
        auto length = command.to.distance_from(command.from) + command.thickness;
        auto dash_count = floor(length / static_cast<float>(command.thickness) / 4) * 2 + 1;
        auto interval = length / dash_count;
        SkScalar intervals[] = { interval, interval };
        paint.setPathEffect(SkDashPathEffect::Make(intervals, 0));

        auto direction = to - from;
        direction.normalize();
        from -= direction * (command.thickness / 2.0f);
        to += direction * (command.thickness / 2.0f);
        break;
    }
    }

    canvas.drawLine(from, to, paint);
}

void DisplayListPlayerSkia::play_command(ApplyBackdropFilter const& command)
{
    auto& canvas = surface().canvas();

    canvas.save();
    clip_to_rounded_rect(canvas, command.backdrop_region, command.corner_radii, SkClipOp::kIntersect);
    ScopeGuard guard = [&] { canvas.restore(); };

    if (command.has_backdrop_filter) {
        auto filter = Gfx::deserialize_filter(inline_data(command.backdrop_filter_data), [&](u64 image_id) {
            return resource_storage().image_frame(ImageFrameResourceId { image_id });
        });
        auto image_filter = to_skia_image_filter(filter);
        canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
        canvas.restore();
    }
}

void DisplayListPlayerSkia::play_command(DrawRect const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1);
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::play_command(PaintRadialGradient const& command)
{
    auto color_stop_colors = gradient_colors(command.color_stops);
    auto color_stop_positions = gradient_positions(command.color_stops);
    VERIFY(!color_stop_colors.is_empty());

    auto colors = to_skia_gradient_colors(color_stop_colors);

    auto const& rect = command.rect;
    auto center = to_skia_point(command.center.translated(command.rect.location()));

    auto const size = command.size.to_type<float>();
    SkMatrix matrix;
    // Skia does not support specifying of horizontal and vertical radius's separately,
    // so instead we apply scale matrix
    auto const aspect_ratio = size.width() / size.height();
    auto const sx = isinf(aspect_ratio) ? 1.0f : aspect_ratio;
    matrix.setScale(sx, 1.0f, center.x(), center.y());

    SkTileMode tile_mode = command.color_stops.repeating ? SkTileMode::kRepeat : SkTileMode::kClamp;

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(command.interpolation_method);
    SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { color_stop_positions.data(), color_stop_positions.size() }, tile_mode, color_space }, interpolation };
    auto shader = SkShaders::RadialGradient(center, size.height(), gradient, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    apply_compositing_and_blending_operator(paint, command.compositing_and_blending_operator);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::play_command(PaintConicGradient const& command)
{
    auto color_stop_colors = gradient_colors(command.color_stops);
    auto color_stop_positions = gradient_positions(command.color_stops);
    VERIFY(!color_stop_colors.is_empty());

    auto colors = to_skia_gradient_colors(color_stop_colors);

    auto const& rect = command.rect;
    auto center = command.position.translated(rect.location()).to_type<float>();

    SkMatrix matrix;
    matrix.setRotate(-90 + command.start_angle, center.x(), center.y());
    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(command.interpolation_method);
    SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { color_stop_positions.data(), color_stop_positions.size() }, SkTileMode::kRepeat, color_space }, interpolation };
    auto shader = SkShaders::SweepGradient(to_skia_point(center), 0, 360, gradient, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    apply_compositing_and_blending_operator(paint, command.compositing_and_blending_operator);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::play_command(DrawIsolatedDisplayList const& command)
{
    auto& canvas = surface().canvas();
    canvas.save();
    canvas.clipRect(to_skia_rect(command.rect), true);
    SkPaint group_paint;
    if (command.compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
        group_paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));
    canvas.saveLayer(nullptr, &group_paint);
    play_command(PaintNestedDisplayList { command.display_list_id, command.rect, command.list_size });
    if (command.mask_display_list_id.value() != 0) {
        SkPaint mask_paint;
        mask_paint.setBlender(Gfx::to_skia_blender(Gfx::CompositingAndBlendingOperator::DestinationIn));
        if (command.mask_kind == Gfx::MaskKind::Luminance)
            mask_paint.setColorFilter(SkLumaColorFilter::Make());
        canvas.saveLayer(nullptr, &mask_paint);
        play_command(PaintNestedDisplayList { command.mask_display_list_id, command.rect, command.list_size });
        canvas.restore();
    }
    canvas.restore();
    canvas.restore();
}

void DisplayListPlayerSkia::play_command(PaintNestedDisplayList const& command)
{
    auto& canvas = surface().canvas();
    auto const& nested_display_list = resource_storage().display_list_resource(command.display_list_id);

    // Nested display lists (used for SVG images and mask contents) are immutable and are replayed with identical
    // commands on every compositor frame between content updates; some carry huge command streams (thousands of
    // paths, dozens of layers) and can be far larger than the viewport. Rasterize a list into an offscreen surface
    // at list resolution and reuse the snapshot, instead of re-encoding the entire vector command stream on every
    // frame. Each list keeps a small set of rasters in the per-client resource storage, keyed by display list id
    // and removed together with the display list itself; the same list can be painted at several places in one
    // frame (repeated SVG images, atlases), and each place then hits its own raster. A placement matching the list
    // resolution at near-integer device offsets blits the raster pixel-identically to replaying the commands; any
    // other axis-aligned placement resamples it through the placement matrix with linear filtering. Lists that are
    // painted for the first time, whose blending could read page content painted underneath them (see
    // should_cache_nested_display_list_raster), or that sit under a rotated/skewed/mirrored transform replay
    // directly.
    auto total_matrix = canvas.getTotalMatrix();
    auto list_bounds = Gfx::IntRect { {}, command.list_size };
    if (m_skia_backend_context && total_matrix.isScaleTranslate() && total_matrix.getScaleX() > 0 && total_matrix.getScaleY() > 0
        && !command.rect.is_empty() && !list_bounds.is_empty()) {
        auto device_rect = total_matrix.mapRect(to_skia_rect(command.rect));
        constexpr float pixel_exact_epsilon = 1.f / 64.f;
        auto snapped_device_rect = SkRect::MakeLTRB(
            SkScalarRoundToScalar(device_rect.left()), SkScalarRoundToScalar(device_rect.top()),
            SkScalarRoundToScalar(device_rect.right()), SkScalarRoundToScalar(device_rect.bottom()));
        bool draws_pixel_exact = fabsf(device_rect.left() - snapped_device_rect.left()) <= pixel_exact_epsilon
            && fabsf(device_rect.top() - snapped_device_rect.top()) <= pixel_exact_epsilon
            && fabsf(snapped_device_rect.width() - static_cast<float>(command.list_size.width())) <= pixel_exact_epsilon
            && fabsf(snapped_device_rect.height() - static_cast<float>(command.list_size.height())) <= pixel_exact_epsilon;
        auto placement_device_rect = draws_pixel_exact ? snapped_device_rect : device_rect;
        auto placement = SkMatrix::RectToRect(SkRect::MakeWH(command.list_size.width(), command.list_size.height()), placement_device_rect);
        SkMatrix inverse_placement;
        if (!placement_device_rect.isEmpty() && placement.invert(&inverse_placement)) {
            auto visible_device_rect = SkRect::Make(canvas.getDeviceClipBounds());
            if (!visible_device_rect.intersect(placement_device_rect))
                return;
            auto visible_in_list_space = inverse_placement.mapRect(visible_device_rect);
            auto visible_rect_in_list_space = Gfx::enclosing_int_rect(
                Gfx::FloatRect { visible_in_list_space.left(), visible_in_list_space.top(), visible_in_list_space.width(), visible_in_list_space.height() })
                                                  .intersected(list_bounds);
            if (visible_rect_in_list_space.is_empty())
                return;

            Gfx::IntRect raster_rect;
            auto cached_image = resource_storage().cached_nested_display_list_raster(command.display_list_id, m_skia_backend_context, visible_rect_in_list_space, raster_rect);
            if (!cached_image && resource_storage().should_cache_nested_display_list_raster(command.display_list_id)) {
                // Small lists are rasterized whole, so lists painted as many little slices (atlases, repeated
                // images) hit one raster for every slice. Larger lists are rasterized at the visible portion.
                constexpr size_t max_full_list_raster_bytes = 16 * MiB;
                constexpr int max_full_list_raster_dimension = 16384;
                auto full_list_bytes = static_cast<size_t>(list_bounds.width()) * list_bounds.height() * 4;
                bool rasterize_whole_list = full_list_bytes <= max_full_list_raster_bytes
                    && list_bounds.width() <= max_full_list_raster_dimension
                    && list_bounds.height() <= max_full_list_raster_dimension;
                auto raster_candidate_rect = rasterize_whole_list ? list_bounds : visible_rect_in_list_space;
                constexpr size_t max_raster_bytes = 128 * MiB;
                if (static_cast<size_t>(raster_candidate_rect.width()) * raster_candidate_rect.height() * 4 <= max_raster_bytes) {
                    auto offscreen_surface = Gfx::PaintingSurface::create_with_size(
                        raster_candidate_rect.size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia_backend_context);
                    offscreen_surface->canvas().clear(SK_ColorTRANSPARENT);
                    offscreen_surface->canvas().translate(-raster_candidate_rect.x(), -raster_candidate_rect.y());
                    execute_display_list_into_surface(
                        *nested_display_list.display_list, nested_display_list.visual_context_tree, *offscreen_surface);
                    offscreen_surface->canvas().resetMatrix();
                    auto image = offscreen_surface->sk_surface().makeImageSnapshot();
                    if (image) {
                        resource_storage().add_cached_nested_display_list_raster(command.display_list_id, m_skia_backend_context, raster_candidate_rect, image);
                        cached_image = move(image);
                        raster_rect = raster_candidate_rect;
                    }
                }
            }
            if (cached_image) {
                auto source_rect = visible_rect_in_list_space.translated(-raster_rect.location());
                auto destination_rect = placement.mapRect(to_skia_rect(visible_rect_in_list_space));
                auto sampling = draws_pixel_exact ? SkSamplingOptions() : SkSamplingOptions(SkFilterMode::kLinear);
                canvas.save();
                canvas.setMatrix(SkMatrix::I());
                canvas.drawImageRect(cached_image.get(), to_skia_rect(source_rect), destination_rect,
                    sampling, nullptr, SkCanvas::kStrict_SrcRectConstraint);
                canvas.restore();
                return;
            }
        }
    }

    canvas.save();
    // Axis-aligned content edges snap to the device pixel grid, the way integer-rect drawing
    // commands land there by construction — otherwise float rounding in the transform chain
    // leaves single-pixel antialiasing slivers along edges meant to be pixel-exact. The snapped
    // placement replaces the canvas matrix outright so no further float composition disturbs it.
    if (auto device_rect = total_matrix.mapRect(to_skia_rect(command.rect));
        total_matrix.isScaleTranslate() && total_matrix.getScaleX() > 0 && total_matrix.getScaleY() > 0 && !list_bounds.is_empty()
        && roundf(device_rect.right()) > roundf(device_rect.left()) && roundf(device_rect.bottom()) > roundf(device_rect.top())) {
        auto snapped_device_rect = SkRect::MakeLTRB(
            roundf(device_rect.left()), roundf(device_rect.top()), roundf(device_rect.right()), roundf(device_rect.bottom()));
        canvas.setMatrix(SkMatrix::RectToRect(SkRect::MakeWH(command.list_size.width(), command.list_size.height()), snapped_device_rect));
        canvas.clipRect(SkRect::MakeWH(command.list_size.width(), command.list_size.height()));
    } else {
        canvas.clipRect(to_skia_rect(command.rect));
        canvas.translate(command.rect.x(), command.rect.y());
        if (!list_bounds.is_empty() && !command.rect.is_empty())
            canvas.scale(command.rect.width() / command.list_size.width(), command.rect.height() / command.list_size.height());
    }
    ScrollStateSnapshot scroll_state_snapshot;
    execute_nested_display_list(*nested_display_list.display_list, nested_display_list.visual_context_tree, scroll_state_snapshot);
    canvas.restore();
}

void DisplayListPlayerSkia::play_command(CompositorScrollNode const&)
{
}

void DisplayListPlayerSkia::play_command(CompositorWheelHitTestTarget const&)
{
}

void DisplayListPlayerSkia::play_command(CompositorWheelHitTestTargetWithCornerRadii const&)
{
}

void DisplayListPlayerSkia::play_command(CompositorMainThreadWheelEventRegion const&)
{
}

void DisplayListPlayerSkia::play_command(CompositorViewportScrollbar const&)
{
}

void DisplayListPlayerSkia::play_command(CompositorBlockingWheelEventRegion const&)
{
}

void DisplayListPlayerSkia::play_command(PaintScrollBar const& command)
{
    paint_scrollbar_into_surface(surface(), command);
}

void DisplayListPlayerSkia::push_clip(ReplayClip const& clip)
{
    auto& canvas = surface().canvas();
    canvas.save();
    clip_to_rounded_rect(canvas, clip.rect, clip.corner_radii, clip.mode == ClipMode::Difference ? SkClipOp::kDifference : SkClipOp::kIntersect);
}

void DisplayListPlayerSkia::push_clip_path(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    surface().canvas().save();
    clip_path(path, winding_rule, true);
}

void DisplayListPlayerSkia::push_layer(ReplayLayer const& layer)
{
    auto& canvas = surface().canvas();
    SkPaint paint;

    if (layer.opacity < 1.0f)
        paint.setAlphaf(layer.opacity);

    if (layer.blend_mode != Gfx::CompositingAndBlendingOperator::Normal)
        paint.setBlender(Gfx::to_skia_blender(layer.blend_mode));

    if (layer.filter_bytes_size)
        paint.setImageFilter(to_skia_image_filter(layer_filter(layer)));

    canvas.saveLayer(nullptr, &paint);
}

void DisplayListPlayerSkia::push_mask(ReplayMask const& mask)
{
    auto& canvas = surface().canvas();
    canvas.save();
    canvas.clipRect(to_skia_rect(mask.rect.to_type<float>()), true);
    canvas.saveLayer(nullptr, nullptr);
}

void DisplayListPlayerSkia::pop_mask(ReplayMask const& mask, Optional<DisplayListResourceId> mask_content)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setBlender(Gfx::to_skia_blender(Gfx::CompositingAndBlendingOperator::DestinationIn));
    if (mask.kind == Gfx::MaskKind::Luminance)
        paint.setColorFilter(SkLumaColorFilter::Make());
    canvas.saveLayer(nullptr, &paint);
    if (mask_content.has_value()) {
        play_command(PaintNestedDisplayList {
            .display_list_id = *mask_content,
            .rect = mask.rect.to_type<float>(),
            .list_size = mask.rect.size(),
        });
    }
    canvas.restore();
    canvas.restore();
    canvas.restore();
}

void DisplayListPlayerSkia::pop()
{
    surface().canvas().restore();
}

void DisplayListPlayerSkia::push_device_space_plane_clip(Gfx::Path const& path)
{
    auto& canvas = surface().canvas();
    canvas.save();
    canvas.setMatrix(SkM44());
    clip_path(path, Gfx::WindingRule::Nonzero, false);
}

void DisplayListPlayerSkia::set_matrix(Gfx::FloatMatrix4x4 const& matrix)
{
    surface().canvas().setMatrix(to_skia_matrix4x4(matrix));
}

Gfx::FloatMatrix4x4 DisplayListPlayerSkia::canvas_matrix() const
{
    return to_gfx_matrix4x4(surface().canvas().getLocalToDevice());
}

void DisplayListPlayerSkia::clip_path(Gfx::Path const& path, Gfx::WindingRule winding_rule, bool anti_aliased)
{
    auto& canvas = surface().canvas();
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    canvas.clipPath(sk_path, anti_aliased);
}

bool DisplayListPlayerSkia::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    return surface().canvas().quickReject(to_skia_rect(rect));
}

}
