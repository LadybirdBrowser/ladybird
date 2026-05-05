/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define SK_SUPPORT_UNSPANNED_APIS

#include <core/SkBitmap.h>
#include <core/SkBlurTypes.h>
#include <core/SkCanvas.h>
#include <core/SkColorFilter.h>
#include <core/SkColorSpace.h>
#include <core/SkFont.h>
#include <core/SkMaskFilter.h>
#include <core/SkPath.h>
#include <core/SkPathEffect.h>
#include <core/SkRRect.h>
#include <core/SkSurface.h>
#include <core/SkYUVAPixmaps.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradientShader.h>
#include <effects/SkImageFilters.h>
#include <effects/SkLumaColorFilter.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <pathops/SkPathOps.h>

#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/VideoFrameSource.h>

namespace Web::Painting {

DisplayListPlayerSkia::DisplayListPlayerSkia()
    : DisplayListPlayerSkia(Gfx::SkiaBackendContext::the_main_thread_context())
{
}

DisplayListPlayerSkia::DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
    : m_skia_backend_context(move(skia_backend_context))
    , m_image_cache(m_skia_backend_context)
{
}

DisplayListPlayerSkia::~DisplayListPlayerSkia()
{
}

static SkRRect to_skia_rrect(auto const& rect, CornerRadii const& corner_radii)
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

void DisplayListPlayerSkia::flush()
{
    if (auto context = surface().skia_backend_context())
        context->flush_and_submit(&surface().sk_surface());
    surface().flush();
    m_image_cache.prune();
}

void DisplayListPlayerSkia::draw_glyph_run(DrawGlyphRun const& command)
{
    auto* blob = command.glyph_run->cached_skia_text_blob();
    if (!blob)
        return;

    SkPaint paint;
    paint.setColor(to_skia_color(command.color));

    auto& canvas = surface().canvas();
    auto const& translation = command.translation;

    switch (command.orientation) {
    case Gfx::Orientation::Horizontal:
        canvas.drawTextBlob(blob, translation.x(), translation.y(), paint);
        break;
    case Gfx::Orientation::Vertical:
        canvas.save();
        canvas.translate(command.rect.width(), 0);
        canvas.rotate(90, command.rect.top_left().x(), command.rect.top_left().y());
        canvas.drawTextBlob(blob, translation.x(), translation.y(), paint);
        canvas.restore();
        break;
    }
}

void DisplayListPlayerSkia::fill_rect(FillRect const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::draw_external_content(DrawExternalContent const& command)
{
    auto bitmap = command.source->current_bitmap();
    if (!bitmap)
        return;
    auto source_bitmap = bitmap->bitmap();
    if (!source_bitmap)
        return;
    auto frame = Gfx::DecodedImageFrame::create(*source_bitmap);
    auto image = m_image_cache.image_for_frame(*frame);
    if (!image)
        return;
    auto dst_rect = to_skia_rect(command.dst_rect);
    SkRect src_rect = SkRect::MakeIWH(image->width(), image->height());
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.drawImageRect(image.get(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void DisplayListPlayerSkia::draw_video_frame_source(DrawVideoFrameSource const& command)
{
    auto frame = command.source->current_frame();
    if (!frame)
        return;

    sk_sp<SkImage> image;
    auto* gr_context = m_skia_backend_context ? m_skia_backend_context->sk_context() : nullptr;
    if (gr_context) {
        image = SkImages::TextureFromYUVAPixmaps(
            gr_context,
            frame->yuv_data().make_pixmaps(),
            skgpu::Mipmapped::kNo,
            false,
            frame->color_space().color_space<sk_sp<SkColorSpace>>());
    }

    RefPtr<Gfx::Bitmap> converted_bitmap;
    if (!image) {
        auto bitmap_or_error = frame->yuv_data().to_bitmap();
        if (bitmap_or_error.is_error()) {
            dbgln("Could not convert video frame to bitmap: {}", bitmap_or_error.release_error());
            return;
        }
        converted_bitmap = bitmap_or_error.release_value();
        auto raster_image = Gfx::sk_image_from_bitmap(*converted_bitmap, frame->color_space());
        if (gr_context) {
            image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
            if (!image)
                image = move(raster_image);
        } else {
            image = move(raster_image);
        }
    }

    auto dst_rect = to_skia_rect(command.dst_rect);
    SkRect src_rect = SkRect::MakeIWH(image->width(), image->height());
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.drawImageRect(image.get(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void DisplayListPlayerSkia::draw_scaled_decoded_image_frame(DrawScaledDecodedImageFrame const& command)
{
    auto image = m_image_cache.image_for_frame(*command.frame);
    if (!image)
        return;

    auto dst_rect = to_skia_rect(command.dst_rect);
    auto clip_rect = to_skia_rect(command.clip_rect);
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.save();
    canvas.clipRect(clip_rect, true);
    canvas.drawImageRect(image.get(), dst_rect, to_skia_sampling_options(command.scaling_mode), &paint);
    canvas.restore();
}

void DisplayListPlayerSkia::draw_repeated_decoded_image_frame(DrawRepeatedDecodedImageFrame const& command)
{
    auto image = m_image_cache.image_for_frame(*command.frame);
    if (!image)
        return;

    SkMatrix matrix;
    auto dst_rect = command.dst_rect.to_type<float>();
    auto src_size = command.frame->size().to_type<float>();
    matrix.setScale(dst_rect.width() / src_size.width(), dst_rect.height() / src_size.height());
    matrix.postTranslate(dst_rect.x(), dst_rect.y());
    auto sampling_options = to_skia_sampling_options(command.scaling_mode);

    auto tile_mode_x = command.repeat.x ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto tile_mode_y = command.repeat.y ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto shader = image->makeShader(tile_mode_x, tile_mode_y, sampling_options, matrix);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(shader);
    auto& canvas = surface().canvas();
    canvas.drawPaint(paint);
}

void DisplayListPlayerSkia::add_clip_rect(AddClipRect const& command)
{
    auto& canvas = surface().canvas();
    auto const& rect = command.rect;
    canvas.clipRect(to_skia_rect(rect), true);
}

void DisplayListPlayerSkia::save(Save const&)
{
    auto& canvas = surface().canvas();
    canvas.save();
}

void DisplayListPlayerSkia::save_layer(SaveLayer const&)
{
    auto& canvas = surface().canvas();
    canvas.saveLayer(nullptr, nullptr);
}

void DisplayListPlayerSkia::restore(Restore const&)
{
    auto& canvas = surface().canvas();
    canvas.restore();
}

void DisplayListPlayerSkia::translate(Translate const& command)
{
    auto& canvas = surface().canvas();
    canvas.translate(command.delta.x(), command.delta.y());
}

static SkGradientShader::Interpolation to_skia_interpolation(CSS::ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method)
{
    SkGradientShader::Interpolation interpolation;

    interpolation_method.visit(
        [&](CSS::RectangularColorSpace color_space) {
            switch (color_space) {
            case CSS::RectangularColorSpace::Srgb:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGB;
                break;
            case CSS::RectangularColorSpace::SrgbLinear:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGBLinear;
                break;
            case CSS::RectangularColorSpace::Lab:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kLab;
                break;
            case CSS::RectangularColorSpace::Oklab:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLab;
                break;
            case CSS::RectangularColorSpace::DisplayP3:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kDisplayP3;
                break;
            case CSS::RectangularColorSpace::A98Rgb:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kA98RGB;
                break;
            case CSS::RectangularColorSpace::ProphotoRgb:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kProphotoRGB;
                break;
            case CSS::RectangularColorSpace::Rec2020:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kRec2020;
                break;
            case CSS::RectangularColorSpace::DisplayP3Linear:
            case CSS::RectangularColorSpace::XyzD50:
            case CSS::RectangularColorSpace::XyzD65:
                dbgln("FIXME: Unsupported gradient color space");
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLab;
                break;
            case CSS::RectangularColorSpace::Xyz:
                // NB: Xyz should have been canonicalized to XyzD65 at parse time
                VERIFY_NOT_REACHED();
            }
        },
        [&](CSS::ColorInterpolationMethodStyleValue::PolarColorInterpolationMethod const& color_space) {
            switch (color_space.color_space) {
            case CSS::PolarColorSpace::Hsl:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kHSL;
                break;
            case CSS::PolarColorSpace::Hwb:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kHWB;
                break;
            case CSS::PolarColorSpace::Lch:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kLCH;
                break;
            case CSS::PolarColorSpace::Oklch:
                interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLCH;
                break;
            }

            switch (color_space.hue_interpolation_method) {
            case CSS::HueInterpolationMethod::Shorter:
                interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kShorter;
                break;
            case CSS::HueInterpolationMethod::Longer:
                interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kLonger;
                break;
            case CSS::HueInterpolationMethod::Increasing:
                interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kIncreasing;
                break;
            case CSS::HueInterpolationMethod::Decreasing:
                interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kDecreasing;
                break;
            }
        });

    interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;

    return interpolation;
}

void DisplayListPlayerSkia::paint_linear_gradient(PaintLinearGradient const& command)
{
    auto const& linear_gradient_data = command.linear_gradient_data;
    auto const& color_stop_list = linear_gradient_data.color_stops.list;
    auto const& repeat_length = linear_gradient_data.color_stops.repeat_length;
    VERIFY(!color_stop_list.is_empty());

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    auto const first_position = repeat_length.has_value() ? color_stop_list.first().position : 0.f;
    for (size_t stop_index = 0; stop_index < color_stop_list.size(); stop_index++) {
        auto const& stop = color_stop_list[stop_index];
        if (stop_index > 0 && stop == color_stop_list[stop_index - 1])
            continue;

        colors.append(to_skia_color4f(stop.color));
        positions.append((stop.position - first_position) / repeat_length.value_or(1));
    }

    auto rect = command.gradient_rect.to_type<float>();
    auto length = calculate_gradient_length<float>(rect.size(), linear_gradient_data.gradient_angle);

    // Starting and ending points before rotation (0deg / "to top")
    auto rect_center = rect.center();
    auto start = rect_center.translated(0, (.5f - first_position) * length);
    auto end = start.translated(0, repeat_length.value_or(1) * -length);
    Array const points { to_skia_point(start), to_skia_point(end) };

    SkMatrix matrix;
    matrix.setRotate(linear_gradient_data.gradient_angle, rect_center.x(), rect_center.y());

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(linear_gradient_data.interpolation_method);
    auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), color_space, positions.data(), positions.size(), SkTileMode::kRepeat, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::paint_outer_box_shadow(PaintOuterBoxShadow const& command)
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

void DisplayListPlayerSkia::paint_inner_box_shadow(PaintInnerBoxShadow const& command)
{
    auto outer_rect = to_skia_rrect(command.outer_shadow_rect, command.content_corner_radii);
    auto inner_rect = to_skia_rrect(command.inner_shadow_rect, command.inner_shadow_corner_radii);

    SkPath outer_path;
    outer_path.addRRect(outer_rect);
    SkPath inner_path;
    inner_path.addRRect(inner_rect);

    SkPath result_path;
    if (!Op(outer_path, inner_path, SkPathOp::kDifference_SkPathOp, &result_path)) {
        VERIFY_NOT_REACHED();
    }

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

void DisplayListPlayerSkia::paint_text_shadow(PaintTextShadow const& command)
{
    auto& canvas = surface().canvas();
    auto blur_image_filter = SkImageFilters::Blur(command.blur_radius / 2, command.blur_radius / 2, nullptr);
    SkPaint blur_paint;
    blur_paint.setImageFilter(blur_image_filter);
    canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, &blur_paint, nullptr, 0));
    draw_glyph_run({ .glyph_run = command.glyph_run,
        .rect = command.text_rect,
        .translation = command.draw_location + command.text_rect.location().to_type<float>(),
        .color = command.color });
    canvas.restore();
}

void DisplayListPlayerSkia::fill_rect_with_rounded_corners(FillRectWithRoundedCorners const& command)
{
    auto const& rect = command.rect;

    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColor(to_skia_color(command.color));
    paint.setAntiAlias(true);

    auto rounded_rect = to_skia_rrect(rect, command.corner_radii);
    canvas.drawRRect(rounded_rect, paint);
}

static SkTileMode to_skia_tile_mode(SVGLinearGradientPaintStyle::SpreadMethod spread_method)
{
    switch (spread_method) {
    case SVGLinearGradientPaintStyle::SpreadMethod::Pad:
        return SkTileMode::kClamp;
    case SVGLinearGradientPaintStyle::SpreadMethod::Reflect:
        return SkTileMode::kMirror;
    case SVGLinearGradientPaintStyle::SpreadMethod::Repeat:
        return SkTileMode::kRepeat;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkPaint gradient_paint_style_to_skia_paint(Painting::SVGGradientPaintStyle const& paint_style, Gfx::FloatRect const& bounding_rect)
{
    SkPaint paint;

    auto const& color_stops = paint_style.color_stops();

    Vector<SkColor> colors;
    colors.ensure_capacity(color_stops.size());
    Vector<SkScalar> positions;
    positions.ensure_capacity(color_stops.size());

    for (auto const& color_stop : color_stops) {
        colors.append(to_skia_color(color_stop.color));
        positions.append(color_stop.position);
    }

    SkMatrix matrix;
    matrix.setTranslate(bounding_rect.x(), bounding_rect.y());
    if (auto gradient_transform = paint_style.gradient_transform(); gradient_transform.has_value())
        matrix = matrix * to_skia_matrix(gradient_transform.value());

    auto tile_mode = to_skia_tile_mode(paint_style.spread_method());

    sk_sp<SkShader> shader;
    if (is<SVGLinearGradientPaintStyle>(paint_style)) {
        auto const& linear_gradient_paint_style = static_cast<SVGLinearGradientPaintStyle const&>(paint_style);

        Array points {
            to_skia_point(linear_gradient_paint_style.start_point()),
            to_skia_point(linear_gradient_paint_style.end_point()),
        };
        shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), color_stops.size(), tile_mode, 0, &matrix);
    } else if (is<SVGRadialGradientPaintStyle>(paint_style)) {
        auto const& radial_gradient_paint_style = static_cast<SVGRadialGradientPaintStyle const&>(paint_style);

        auto start_center = to_skia_point(radial_gradient_paint_style.start_center());
        auto end_center = to_skia_point(radial_gradient_paint_style.end_center());

        auto start_radius = radial_gradient_paint_style.start_radius();
        auto end_radius = radial_gradient_paint_style.end_radius();

        shader = SkGradientShader::MakeTwoPointConical(start_center, start_radius, end_center, end_radius, colors.data(), positions.data(), color_stops.size(), tile_mode, 0, &matrix);
    }
    paint.setShader(shader);
    if (paint_style.color_space() == Gfx::InterpolationColorSpace::LinearRGB) {
        paint.setColorFilter(SkColorFilters::LinearToSRGBGamma());
    }

    return paint;
}

SkPaint DisplayListPlayerSkia::paint_style_to_skia_paint(Painting::SVGPaintServerPaintStyle const& paint_style, Gfx::FloatRect const& bounding_rect)
{
    if (auto const* gradient = as_if<SVGGradientPaintStyle>(paint_style))
        return gradient_paint_style_to_skia_paint(*gradient, bounding_rect);

    if (auto const* pattern = as_if<SVGPatternPaintStyle>(paint_style)) {
        auto const& tile_rect = pattern->tile_rect();
        auto tile_size = Gfx::IntSize(ceilf(tile_rect.width()), ceilf(tile_rect.height()));
        if (tile_size.is_empty())
            return {};

        auto tile_surface = Gfx::PaintingSurface::create_with_size(tile_size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia_backend_context);

        execute_display_list_into_surface(*pattern->tile_display_list(), *tile_surface);

        auto image = tile_surface->sk_surface().makeImageSnapshot();

        SkMatrix matrix;
        matrix.setTranslate(tile_rect.x(), tile_rect.y());

        matrix.preScale(tile_rect.width() / tile_size.width(), tile_rect.height() / tile_size.height());
        if (auto transform = pattern->pattern_transform(); transform.has_value())
            matrix = matrix * to_skia_matrix(transform.value());

        auto sampling = SkSamplingOptions(SkFilterMode::kLinear);
        auto shader = image->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, sampling, &matrix);

        SkPaint paint;
        paint.setShader(shader);
        return paint;
    }

    return {};
}

void DisplayListPlayerSkia::fill_path(FillPath const& command)
{
    auto path = to_skia_path(command.path);
    path.setFillType(to_skia_path_fill_type(command.winding_rule));

    SkPaint paint;
    if (command.paint_style_or_color.has<PaintStyle>()) {
        auto const& paint_style = command.paint_style_or_color.get<PaintStyle>();
        paint = paint_style_to_skia_paint(*paint_style, command.bounding_rect().to_type<float>());
        paint.setAlphaf(command.opacity);
    } else {
        auto const& color = command.paint_style_or_color.get<Color>();
        paint.setColor(to_skia_color(color));
    }
    paint.setAntiAlias(command.should_anti_alias == ShouldAntiAlias::Yes);
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::stroke_path(StrokePath const& command)
{
    auto path = to_skia_path(command.path);
    SkPaint paint;
    if (command.paint_style_or_color.has<PaintStyle>()) {
        auto const& paint_style = command.paint_style_or_color.get<PaintStyle>();
        paint = paint_style_to_skia_paint(*paint_style, command.bounding_rect().to_type<float>());
        paint.setAlphaf(command.opacity);
    } else {
        auto const& color = command.paint_style_or_color.get<Color>();
        paint.setColor(to_skia_color(color));
    }
    paint.setAntiAlias(command.should_anti_alias == ShouldAntiAlias::Yes);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setStrokeCap(to_skia_cap(command.cap_style));
    paint.setStrokeJoin(to_skia_join(command.join_style));
    paint.setStrokeMiter(command.miter_limit);
    paint.setPathEffect(SkDashPathEffect::Make(command.dash_array.data(), command.dash_array.size(), command.dash_offset));
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::draw_ellipse(DrawEllipse const& command)
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

void DisplayListPlayerSkia::fill_ellipse(FillEllipse const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    canvas.drawOval(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::draw_line(DrawLine const& command)
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
        paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));
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
        paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));

        auto direction = to - from;
        direction.normalize();
        from -= direction * (command.thickness / 2.0f);
        to += direction * (command.thickness / 2.0f);
        break;
    }
    }

    canvas.drawLine(from, to, paint);
}

void DisplayListPlayerSkia::apply_backdrop_filter(ApplyBackdropFilter const& command)
{
    auto& canvas = surface().canvas();

    auto rect = to_skia_rect(command.backdrop_region);
    canvas.save();
    canvas.clipRect(rect, true);
    ScopeGuard guard = [&] { canvas.restore(); };

    if (command.backdrop_filter.has_value()) {
        auto image_filter = to_skia_image_filter(command.backdrop_filter.value());
        canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
        canvas.restore();
    }
}

void DisplayListPlayerSkia::draw_rect(DrawRect const& command)
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

void DisplayListPlayerSkia::paint_radial_gradient(PaintRadialGradient const& command)
{
    auto const& radial_gradient_data = command.radial_gradient_data;
    auto const& color_stop_list = radial_gradient_data.color_stops.list;
    VERIFY(!color_stop_list.is_empty());

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    for (size_t stop_index = 0; stop_index < color_stop_list.size(); stop_index++) {
        auto const& stop = color_stop_list[stop_index];
        if (stop_index > 0 && stop == color_stop_list[stop_index - 1])
            continue;
        colors.append(to_skia_color4f(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.rect;
    auto center = to_skia_point(command.center.translated(command.rect.location()));

    auto const size = command.size.to_type<float>();
    SkMatrix matrix;
    // Skia does not support specifying of horizontal and vertical radius's separately,
    // so instead we apply scale matrix
    auto const aspect_ratio = size.width() / size.height();
    auto const sx = isinf(aspect_ratio) ? 1.0f : aspect_ratio;
    matrix.setScale(sx, 1.0f, center.x(), center.y());

    SkTileMode tile_mode = radial_gradient_data.color_stops.repeating ? SkTileMode::kRepeat : SkTileMode::kClamp;

    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(radial_gradient_data.interpolation_method);
    auto shader = SkGradientShader::MakeRadial(center, size.height(), colors.data(), color_space, positions.data(), positions.size(), tile_mode, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::paint_conic_gradient(PaintConicGradient const& command)
{
    auto const& conic_gradient_data = command.conic_gradient_data;
    auto const& color_stop_list = conic_gradient_data.color_stops.list;
    VERIFY(!color_stop_list.is_empty());

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    for (size_t stop_index = 0; stop_index < color_stop_list.size(); stop_index++) {
        auto const& stop = color_stop_list[stop_index];
        if (stop_index > 0 && stop == color_stop_list[stop_index - 1])
            continue;
        colors.append(to_skia_color4f(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.rect;
    auto center = command.position.translated(rect.location()).to_type<float>();

    SkMatrix matrix;
    matrix.setRotate(-90 + conic_gradient_data.start_angle, center.x(), center.y());
    auto color_space = SkColorSpace::MakeSRGB();
    auto interpolation = to_skia_interpolation(conic_gradient_data.interpolation_method);
    auto shader = SkGradientShader::MakeSweep(center.x(), center.y(), colors.data(), color_space, positions.data(), positions.size(), SkTileMode::kRepeat, 0, 360, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::add_rounded_rect_clip(AddRoundedRectClip const& command)
{
    auto rounded_rect = to_skia_rrect(command.border_rect, command.corner_radii);
    auto& canvas = surface().canvas();
    auto clip_op = command.corner_clip == CornerClip::Inside ? SkClipOp::kDifference : SkClipOp::kIntersect;
    canvas.clipRRect(rounded_rect, clip_op, true);
}

void DisplayListPlayerSkia::paint_nested_display_list(PaintNestedDisplayList const& command)
{
    auto& canvas = surface().canvas();
    canvas.translate(command.rect.x(), command.rect.y());
    ScrollStateSnapshot scroll_state_snapshot;
    execute_impl(*command.display_list, scroll_state_snapshot);
}

void DisplayListPlayerSkia::paint_scrollbar(PaintScrollBar const& command)
{
    auto gutter_rect = to_skia_rect(command.gutter_rect);

    auto thumb_rect = to_skia_rect(command.thumb_rect);
    auto radius = thumb_rect.width() / 2;
    auto thumb_rrect = SkRRect::MakeRectXY(thumb_rect, radius, radius);

    auto& canvas = surface().canvas();

    auto gutter_fill_color = command.track_color;
    SkPaint gutter_fill_paint;
    gutter_fill_paint.setColor(to_skia_color(gutter_fill_color));
    canvas.drawRect(gutter_rect, gutter_fill_paint);

    auto thumb_fill_color = command.thumb_color;
    if (command.gutter_rect.is_empty() && thumb_fill_color == CSS::InitialValues::scrollbar_color().thumb_color)
        thumb_fill_color = thumb_fill_color.with_alpha(128);

    SkPaint thumb_fill_paint;
    thumb_fill_paint.setColor(to_skia_color(thumb_fill_color));
    canvas.drawRRect(thumb_rrect, thumb_fill_paint);

    auto stroke_color = thumb_fill_color.lightened();
    SkPaint stroke_paint;
    stroke_paint.setStroke(true);
    stroke_paint.setStrokeWidth(1);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setColor(to_skia_color(stroke_color));
    canvas.drawRRect(thumb_rrect, stroke_paint);
}

void DisplayListPlayerSkia::apply_effects(ApplyEffects const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;

    if (command.opacity < 1.0f)
        paint.setAlphaf(command.opacity);

    if (command.compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
        paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));

    if (command.filter.has_value())
        paint.setImageFilter(to_skia_image_filter(command.filter.value()));

    if (command.mask_kind.has_value() && command.mask_kind.value() == Gfx::MaskKind::Luminance)
        paint.setColorFilter(SkLumaColorFilter::Make());

    canvas.saveLayer(nullptr, &paint);
}

void DisplayListPlayerSkia::apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 const& matrix)
{
    auto new_transform = Gfx::translation_matrix(Vector3<float>(origin.x(), origin.y(), 0));
    new_transform = new_transform * matrix;
    new_transform = new_transform * Gfx::translation_matrix(Vector3<float>(-origin.x(), -origin.y(), 0));
    auto skia_matrix = to_skia_matrix4x4(new_transform);
    surface().canvas().concat(skia_matrix);
}

void DisplayListPlayerSkia::add_clip_path(Gfx::Path const& path)
{
    auto& canvas = surface().canvas();
    canvas.clipPath(to_skia_path(path), true);
}

bool DisplayListPlayerSkia::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    return surface().canvas().quickReject(to_skia_rect(rect));
}

}
