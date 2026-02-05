/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD
#define SK_SUPPORT_UNSPANNED_APIS

#include <AK/GenericShorthands.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <LibGfx/Filter.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkCanvas.h>
#include <core/SkImage.h>
#include <core/SkPath.h>
#include <core/SkPathEffect.h>
#include <effects/SkBlurMaskFilter.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradientShader.h>

namespace Gfx {

struct PainterSkia::Impl {
    RefPtr<Gfx::PaintingSurface> painting_surface;

    Impl(Gfx::PaintingSurface& surface)
        : painting_surface(surface)
    {
    }

    template<typename Callback>
    void with_canvas(Callback&& callback)
    {
        painting_surface->lock_context();
        auto& canvas = painting_surface->canvas();
        callback(canvas);
        painting_surface->unlock_context();
    }
};

static void apply_paint_style(SkPaint& paint, PaintStyle const& style)
{
    if (auto const& solid_color = as_if<SolidColorPaintStyle>(style)) {
        paint.setColor(to_skia_color(solid_color->color()));
    } else if (auto const& linear_gradient = as_if<Gfx::CanvasLinearGradientPaintStyle>(style)) {
        auto const& color_stops = linear_gradient->color_stops();

        Vector<SkColor> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.append(to_skia_color(color_stop.color));
            positions.append(color_stop.position);
        }

        Array points { to_skia_point(linear_gradient->start_point()), to_skia_point(linear_gradient->end_point()) };

        SkMatrix matrix;
        auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), color_stops.size(), SkTileMode::kClamp, 0, &matrix);
        paint.setShader(shader);
    } else if (auto const* radial_gradient = as_if<CanvasRadialGradientPaintStyle>(style)) {
        auto const& color_stops = radial_gradient->color_stops();

        Vector<SkColor> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.append(to_skia_color(color_stop.color));
            positions.append(color_stop.position);
        }

        auto start_center = radial_gradient->start_center();
        auto end_center = radial_gradient->end_center();
        auto start_radius = radial_gradient->start_radius();
        auto end_radius = radial_gradient->end_radius();

        auto start_sk_point = to_skia_point(start_center);
        auto end_sk_point = to_skia_point(end_center);

        SkMatrix matrix;
        auto shader = SkGradientShader::MakeTwoPointConical(start_sk_point, start_radius, end_sk_point, end_radius, colors.data(), positions.data(), color_stops.size(), SkTileMode::kClamp, 0, &matrix);
        paint.setShader(shader);
    } else if (auto const* canvas_pattern = as_if<CanvasPatternPaintStyle>(style)) {
        auto image = canvas_pattern->image();
        if (!image)
            return;
        auto const* sk_image = image->sk_image();

        auto repetition = canvas_pattern->repetition();
        auto repeat_x = first_is_one_of(repetition, CanvasPatternPaintStyle::Repetition::Repeat, CanvasPatternPaintStyle::Repetition::RepeatX);
        auto repeat_y = first_is_one_of(repetition, CanvasPatternPaintStyle::Repetition::Repeat, CanvasPatternPaintStyle::Repetition::RepeatY);

        // FIXME: Implement sampling configuration.
        SkSamplingOptions sk_sampling_options { SkFilterMode::kLinear };
        Optional<SkMatrix> transformation_matrix;

        if (canvas_pattern->transform().has_value()) {
            auto const& transform = canvas_pattern->transform().value();
            transformation_matrix = SkMatrix::MakeAll(
                transform.a(), transform.c(), transform.e(),
                transform.b(), transform.d(), transform.f(),
                0, 0, 1);
        }
        auto shader = sk_image->makeShader(
            repeat_x ? SkTileMode::kRepeat : SkTileMode::kDecal,
            repeat_y ? SkTileMode::kRepeat : SkTileMode::kDecal,
            sk_sampling_options, transformation_matrix.has_value() ? &transformation_matrix.value() : nullptr);
        paint.setShader(move(shader));
    } else {
        dbgln("FIXME: Unsupported PaintStyle");
    }
}

static void apply_filter(SkPaint& paint, Gfx::Filter const& filter)
{
    paint.setImageFilter(to_skia_image_filter(filter));
}

static SkPaint to_skia_paint(Gfx::PaintStyle const& style, Optional<Gfx::Filter const&> filter)
{
    SkPaint paint;

    apply_paint_style(paint, style);

    if (filter.has_value())
        apply_filter(paint, move(filter.value()));

    return paint;
}

PainterSkia::PainterSkia(NonnullRefPtr<Gfx::PaintingSurface> painting_surface)
    : m_impl(adopt_own(*new Impl { move(painting_surface) }))
{
    m_impl->with_canvas([this](auto& canvas) {
        m_initial_save_count = canvas.save();
    });
}

PainterSkia::~PainterSkia() = default;

void PainterSkia::clear_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    impl().with_canvas([&](auto& canvas) {
        canvas.save();
        canvas.clipRect(to_skia_rect(rect));
        canvas.clear(to_skia_color(color));
        canvas.restore();
    });
}

void PainterSkia::fill_rect(Gfx::FloatRect const& rect, Color color)
{
    SkPaint paint;
    paint.setColor(to_skia_color(color));
    impl().with_canvas([&](auto& canvas) {
        canvas.drawRect(to_skia_rect(rect), paint);
    });
}

void PainterSkia::draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::ImmutableBitmap const& src_bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Optional<Gfx::Filter> filter, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    SkPaint paint;

    if (filter.has_value())
        apply_filter(paint, filter.value());

    paint.setAlpha(static_cast<u8>(global_alpha * 255));
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));

    impl().with_canvas([&](auto& canvas) {
        canvas.drawImageRect(
            src_bitmap.sk_image(),
            to_skia_rect(src_rect),
            to_skia_rect(dst_rect),
            to_skia_sampling_options(scaling_mode),
            &paint,
            SkCanvas::kStrict_SrcRectConstraint);
    });
}

void PainterSkia::set_transform(Gfx::AffineTransform const& transform)
{
    auto matrix = SkMatrix::MakeAll(
        transform.a(), transform.c(), transform.e(),
        transform.b(), transform.d(), transform.f(),
        0, 0, 1);

    impl().with_canvas([&](auto& canvas) {
        canvas.setMatrix(matrix);
    });
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::Color color, float thickness)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setColor(to_skia_color(color));
    auto sk_path = to_skia_path(path);
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::Color color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle cap_style, Gfx::Path::JoinStyle join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setColor(to_skia_color(color));
    paint.setStrokeCap(to_skia_cap(cap_style));
    paint.setStrokeJoin(to_skia_join(join_style));
    paint.setStrokeMiter(miter_limit);
    paint.setPathEffect(SkDashPathEffect::Make(dash_array.data(), dash_array.size(), dash_offset));
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    auto sk_path = to_skia_path(path);
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> filter, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    auto sk_path = to_skia_path(path);
    auto paint = to_skia_paint(paint_style, filter);
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> filter, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const& cap_style, Gfx::Path::JoinStyle const& join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    auto sk_path = to_skia_path(path);
    auto paint = to_skia_paint(paint_style, filter);
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setStrokeCap(to_skia_cap(cap_style));
    paint.setStrokeJoin(to_skia_join(join_style));
    paint.setStrokeMiter(miter_limit);
    paint.setPathEffect(SkDashPathEffect::Make(dash_array.data(), dash_array.size(), dash_offset));
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule)
{
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(color));
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    paint.setColor(to_skia_color(color));
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> filter, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    auto paint = to_skia_paint(paint_style, filter);
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    impl().with_canvas([&](auto& canvas) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::save()
{
    impl().with_canvas([&](auto& canvas) {
        canvas.save();
    });
}

void PainterSkia::restore()
{
    impl().with_canvas([&](auto& canvas) {
        canvas.restore();
    });
}

void PainterSkia::clip(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    impl().with_canvas([&](auto& canvas) {
        canvas.clipPath(sk_path, SkClipOp::kIntersect, true);
    });
}

void PainterSkia::reset()
{
    impl().with_canvas([&](auto& canvas) {
        canvas.restoreToCount(m_initial_save_count);
    });
}

}
