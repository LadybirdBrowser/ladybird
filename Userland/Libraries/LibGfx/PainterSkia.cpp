/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/OwnPtr.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/Path.h>

#include <AK/TypeCasts.h>
#include <core/SkBitmap.h>
#include <core/SkBlurTypes.h>
#include <core/SkCanvas.h>
#include <core/SkColorFilter.h>
#include <core/SkMaskFilter.h>
#include <core/SkPath.h>
#include <core/SkPathBuilder.h>
#include <core/SkRRect.h>
#include <core/SkSurface.h>
#include <effects/SkGradientShader.h>
#include <effects/SkImageFilters.h>
#include <gpu/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <pathops/SkPathOps.h>

namespace Gfx {

struct PainterSkia::Impl {
    NonnullRefPtr<Gfx::Bitmap> gfx_bitmap;
    OwnPtr<SkBitmap> sk_bitmap;
    OwnPtr<SkCanvas> sk_canvas;

    Impl(NonnullRefPtr<Gfx::Bitmap> target_bitmap)
        : gfx_bitmap(move(target_bitmap))
    {
        sk_bitmap = make<SkBitmap>();
        SkImageInfo info = SkImageInfo::Make(gfx_bitmap->width(), gfx_bitmap->height(), kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
        sk_bitmap->installPixels(info, gfx_bitmap->scanline(0), gfx_bitmap->pitch());

        sk_canvas = make<SkCanvas>(*sk_bitmap);
    }

    SkCanvas* canvas() { return sk_canvas; }
};

static constexpr SkRect to_skia_rect(auto const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

static constexpr SkColor to_skia_color(Gfx::Color const& color)
{
    return SkColorSetARGB(color.alpha(), color.red(), color.green(), color.blue());
}

static SkPath to_skia_path(Gfx::Path const& path)
{
    Optional<Gfx::FloatPoint> subpath_start_point;
    Optional<Gfx::FloatPoint> subpath_last_point;
    SkPathBuilder path_builder;
    auto close_subpath_if_needed = [&](auto last_point) {
        if (subpath_start_point == last_point)
            path_builder.close();
    };
    for (auto const& segment : path) {
        auto point = segment.point();
        switch (segment.command()) {
        case Gfx::PathSegment::Command::MoveTo: {
            if (subpath_start_point.has_value() && subpath_last_point.has_value())
                close_subpath_if_needed(subpath_last_point.value());
            subpath_start_point = point;
            path_builder.moveTo({ point.x(), point.y() });
            break;
        }
        case Gfx::PathSegment::Command::LineTo: {
            if (!subpath_start_point.has_value())
                subpath_start_point = Gfx::FloatPoint { 0.0f, 0.0f };
            path_builder.lineTo({ point.x(), point.y() });
            break;
        }
        case Gfx::PathSegment::Command::QuadraticBezierCurveTo: {
            if (!subpath_start_point.has_value())
                subpath_start_point = Gfx::FloatPoint { 0.0f, 0.0f };
            SkPoint pt1 = { segment.through().x(), segment.through().y() };
            SkPoint pt2 = { segment.point().x(), segment.point().y() };
            path_builder.quadTo(pt1, pt2);
            break;
        }
        case Gfx::PathSegment::Command::CubicBezierCurveTo: {
            if (!subpath_start_point.has_value())
                subpath_start_point = Gfx::FloatPoint { 0.0f, 0.0f };
            SkPoint pt1 = { segment.through_0().x(), segment.through_0().y() };
            SkPoint pt2 = { segment.through_1().x(), segment.through_1().y() };
            SkPoint pt3 = { segment.point().x(), segment.point().y() };
            path_builder.cubicTo(pt1, pt2, pt3);
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
        subpath_last_point = point;
    }

    close_subpath_if_needed(subpath_last_point);

    return path_builder.snapshot();
}

static SkPathFillType to_skia_path_fill_type(Gfx::WindingRule winding_rule)
{
    switch (winding_rule) {
    case Gfx::WindingRule::Nonzero:
        return SkPathFillType::kWinding;
    case Gfx::WindingRule::EvenOdd:
        return SkPathFillType::kEvenOdd;
    }
    VERIFY_NOT_REACHED();
}

PainterSkia::PainterSkia(NonnullRefPtr<Gfx::Bitmap> target_bitmap)
    : m_impl(adopt_own(*new Impl { move(target_bitmap) }))
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

static SkSamplingOptions to_skia_sampling_options(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::NearestNeighbor:
        return SkSamplingOptions(SkFilterMode::kNearest);
    case Gfx::ScalingMode::BilinearBlend:
    case Gfx::ScalingMode::SmoothPixels:
        return SkSamplingOptions(SkFilterMode::kLinear);
    case Gfx::ScalingMode::BoxSampling:
        return SkSamplingOptions(SkCubicResampler::Mitchell());
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkColorType to_skia_color_type(Gfx::BitmapFormat format)
{
    switch (format) {
    case Gfx::BitmapFormat::Invalid:
        return kUnknown_SkColorType;
    case Gfx::BitmapFormat::BGRA8888:
    case Gfx::BitmapFormat::BGRx8888:
        return kBGRA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBA8888:
        return kRGBA_8888_SkColorType;
    default:
        return kUnknown_SkColorType;
    }
}

void PainterSkia::draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::Bitmap const& src_bitmap, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, float global_alpha)
{
    SkBitmap sk_bitmap;
    SkImageInfo info = SkImageInfo::Make(src_bitmap.width(), src_bitmap.height(), to_skia_color_type(src_bitmap.format()), kUnpremul_SkAlphaType);
    sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(src_bitmap.scanline(0))), src_bitmap.pitch());

    SkPaint paint;
    paint.setAlpha(static_cast<u8>(global_alpha * 255));

    impl().canvas()->drawImageRect(
        sk_bitmap.asImage(),
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
    if (!thickness)
        return;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setColor(to_skia_color(color));
    auto sk_path = to_skia_path(path);
    impl().canvas()->drawPath(sk_path, paint);
}

static SkPoint to_skia_point(auto const& point)
{
    return SkPoint::Make(point.x(), point.y());
}

static SkPaint to_skia_paint(Gfx::PaintStyle const& style, Gfx::FloatRect const& bounding_rect)
{
    if (is<Gfx::CanvasLinearGradientPaintStyle>(style)) {
        auto const& linear_gradient = static_cast<Gfx::CanvasLinearGradientPaintStyle const&>(style);
        auto const& color_stops = linear_gradient.color_stops();

        SkPaint paint;
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
        return paint;
    }

    if (is<Gfx::CanvasRadialGradientPaintStyle>(style)) {
        auto const& radial_gradient = static_cast<Gfx::CanvasRadialGradientPaintStyle const&>(style);
        auto const& color_stops = radial_gradient.color_stops();

        SkPaint paint;
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

        start_center.translate_by(bounding_rect.location());
        end_center.translate_by(bounding_rect.location());

        auto start_sk_point = to_skia_point(start_center);
        auto end_sk_point = to_skia_point(end_center);

        SkMatrix matrix;
        auto shader = SkGradientShader::MakeTwoPointConical(start_sk_point, start_radius, end_sk_point, end_radius, colors.data(), positions.data(), color_stops.size(), SkTileMode::kClamp, 0, &matrix);
        paint.setShader(shader);
    }
    return {};
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, float thickness, float global_alpha)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!thickness)
        return;

    auto sk_path = to_skia_path(path);
    auto paint = to_skia_paint(paint_style, path.bounding_box());
    paint.setAntiAlias(true);
    paint.setAlphaf(global_alpha);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(thickness);
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

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, float global_alpha, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    auto paint = to_skia_paint(paint_style, path.bounding_box());
    paint.setAntiAlias(true);
    paint.setAlphaf(global_alpha);
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

}
