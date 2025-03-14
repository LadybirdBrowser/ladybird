/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGfx/Filter.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/SkiaUtils.h>

#include <AK/TypeCasts.h>
#include <core/SkBlender.h>
#include <core/SkCanvas.h>
#include <core/SkPath.h>
#include <effects/SkBlurMaskFilter.h>
#include <effects/SkGradientShader.h>

namespace Gfx {

struct PainterSkia::Impl {
    RefPtr<Gfx::PaintingSurface> painting_surface;

    Impl(Gfx::PaintingSurface& surface)
        : painting_surface(surface)
    {
    }

    SkCanvas* canvas() const
    {
        return &painting_surface->canvas();
    }
};

static void apply_paint_style(SkPaint& paint, Gfx::PaintStyle const& style)
{
    if (is<Gfx::SolidColorPaintStyle>(style)) {
        auto const& solid_color = static_cast<Gfx::SolidColorPaintStyle const&>(style);
        auto color = solid_color.sample_color(Gfx::IntPoint(0, 0));

        paint.setColor(to_skia_color(color));
    } else if (is<Gfx::CanvasLinearGradientPaintStyle>(style)) {
        auto const& linear_gradient = static_cast<Gfx::CanvasLinearGradientPaintStyle const&>(style);
        auto const& color_stops = linear_gradient.color_stops();

        Vector<SkColor> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.append(to_skia_color(color_stop.color));
            positions.append(color_stop.position);
        }

        Array<SkPoint, 2> points;
        points[0] = to_skia_point(linear_gradient.start_point());
        points[1] = to_skia_point(linear_gradient.end_point());

        SkMatrix matrix;
        auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), color_stops.size(), SkTileMode::kClamp, 0, &matrix);
        paint.setShader(shader);
    } else if (is<Gfx::CanvasRadialGradientPaintStyle>(style)) {
        auto const& radial_gradient = static_cast<Gfx::CanvasRadialGradientPaintStyle const&>(style);
        auto const& color_stops = radial_gradient.color_stops();

        Vector<SkColor> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.append(to_skia_color(color_stop.color));
            positions.append(color_stop.position);
        }

        auto start_center = radial_gradient.start_center();
        auto end_center = radial_gradient.end_center();
        auto start_radius = radial_gradient.start_radius();
        auto end_radius = radial_gradient.end_radius();

        auto start_sk_point = to_skia_point(start_center);
        auto end_sk_point = to_skia_point(end_center);

        SkMatrix matrix;
        auto shader = SkGradientShader::MakeTwoPointConical(start_sk_point, start_radius, end_sk_point, end_radius, colors.data(), positions.data(), color_stops.size(), SkTileMode::kClamp, 0, &matrix);
        paint.setShader(shader);
    }
}

static void apply_filters(SkPaint& paint, ReadonlySpan<Gfx::Filter> filters)
{
    for (auto const& filter : filters) {
        paint.setImageFilter(to_skia_image_filter(filter));
    }
}

static SkPaint to_skia_paint(Gfx::PaintStyle const& style, ReadonlySpan<Gfx::Filter> filters)
{
    SkPaint paint;

    apply_paint_style(paint, style);
    apply_filters(paint, filters);

    return paint;
}

PainterSkia::PainterSkia(NonnullRefPtr<Gfx::PaintingSurface> painting_surface)
    : m_impl(adopt_own(*new Impl { move(painting_surface) }))
{
}

PainterSkia::~PainterSkia() = default;

void PainterSkia::clear_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    SkPaint paint;
    paint.setColor(to_skia_color(color));
    paint.setBlendMode(SkBlendMode::kClear);
    impl().canvas()->drawRect(to_skia_rect(rect), paint);
}

void PainterSkia::fill_rect(Gfx::FloatRect const& rect, Color color)
{
    SkPaint paint;
    paint.setColor(to_skia_color(color));
    impl().canvas()->drawRect(to_skia_rect(rect), paint);
}

void PainterSkia::draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::ImmutableBitmap const& src_bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, ReadonlySpan<Gfx::Filter> filters, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    SkPaint paint;
    apply_filters(paint, filters);
    paint.setAlpha(static_cast<u8>(global_alpha * 255));
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));

    impl().canvas()->drawImageRect(
        src_bitmap.sk_image(),
        to_skia_rect(src_rect),
        to_skia_rect(dst_rect),
        to_skia_sampling_options(scaling_mode),
        &paint,
        SkCanvas::kStrict_SrcRectConstraint);
}

void PainterSkia::set_transform(Gfx::AffineTransform const& transform)
{
    auto matrix = SkMatrix::MakeAll(
        transform.a(), transform.c(), transform.e(),
        transform.b(), transform.d(), transform.f(),
        0, 0, 1);

    impl().canvas()->setMatrix(matrix);
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
    impl().canvas()->drawPath(sk_path, paint);
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::Color color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
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
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    auto sk_path = to_skia_path(path);
    impl().canvas()->drawPath(sk_path, paint);
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, ReadonlySpan<Gfx::Filter> filters, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    auto sk_path = to_skia_path(path);
    auto paint = to_skia_paint(paint_style, filters);
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    impl().canvas()->drawPath(sk_path, paint);
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, ReadonlySpan<Gfx::Filter> filters, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const& cap_style, Gfx::Path::JoinStyle const& join_style)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    auto sk_path = to_skia_path(path);
    auto paint = to_skia_paint(paint_style, filters);
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setStrokeCap(to_skia_cap(cap_style));
    paint.setStrokeJoin(to_skia_join(join_style));
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    impl().canvas()->drawPath(sk_path, paint);
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule)
{
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(color));
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    impl().canvas()->drawPath(sk_path, paint);
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
    impl().canvas()->drawPath(sk_path, paint);
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, ReadonlySpan<Gfx::Filter> filters, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    auto paint = to_skia_paint(paint_style, filters);
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setBlender(to_skia_blender(compositing_and_blending_operator));
    impl().canvas()->drawPath(sk_path, paint);
}

void PainterSkia::save()
{
    impl().canvas()->save();
}

void PainterSkia::restore()
{
    impl().canvas()->restore();
}

void PainterSkia::clip(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    impl().canvas()->clipPath(sk_path, SkClipOp::kIntersect, true);
}

}
