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
#include <core/SkTextBlob.h>
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
#include <LibGfx/PainterSkia.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
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
    auto const& font = active_display_list().resource_storage().font(command.font_id);
    auto glyphs = inline_objects<Gfx::DrawGlyph>(command.glyphs);
    if (glyphs.is_empty())
        return;

    auto sk_font = font.skia_font(command.scale);
    SkTextBlobBuilder builder;
    auto const& run = builder.allocRunPos(sk_font, glyphs.size());

    auto font_ascent = font.pixel_metrics().ascent;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        run.glyphs[i] = glyphs[i].glyph_id;
        run.pos[i * 2] = glyphs[i].position.x() * command.scale;
        run.pos[i * 2 + 1] = (glyphs[i].position.y() + font_ascent) * command.scale;
    }

    auto blob = builder.make();
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
    auto frame = active_display_list().resource_storage().external_content_source(command.source_id).current_frame();
    if (!frame.has_value())
        return;
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
    auto frame = active_display_list().resource_storage().video_frame_source(command.source_id).current_frame();
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
    auto const& frame = active_display_list().resource_storage().image_frame(command.frame_id);
    auto image = m_image_cache.image_for_frame(frame);
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
    auto const& frame = active_display_list().resource_storage().image_frame(command.frame_id);
    auto image = m_image_cache.image_for_frame(frame);
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

static SkGradientShader::Interpolation to_skia_interpolation(Gfx::GradientInterpolationMethod interpolation_method)
{
    SkGradientShader::Interpolation interpolation;

    if (interpolation_method.type == Gfx::GradientInterpolationMethod::Type::Rectangular) {
        switch (interpolation_method.rectangular_color_space) {
        case Gfx::RectangularColorSpace::Srgb:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGB;
            break;
        case Gfx::RectangularColorSpace::SrgbLinear:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGBLinear;
            break;
        case Gfx::RectangularColorSpace::Lab:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kLab;
            break;
        case Gfx::RectangularColorSpace::Oklab:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLab;
            break;
        case Gfx::RectangularColorSpace::DisplayP3:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kDisplayP3;
            break;
        case Gfx::RectangularColorSpace::A98Rgb:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kA98RGB;
            break;
        case Gfx::RectangularColorSpace::ProphotoRgb:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kProphotoRGB;
            break;
        case Gfx::RectangularColorSpace::Rec2020:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kRec2020;
            break;
        case Gfx::RectangularColorSpace::DisplayP3Linear:
        case Gfx::RectangularColorSpace::XyzD50:
        case Gfx::RectangularColorSpace::XyzD65:
            dbgln("FIXME: Unsupported gradient color space");
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLab;
            break;
        case Gfx::RectangularColorSpace::Xyz:
            VERIFY_NOT_REACHED();
        }
    } else {
        switch (interpolation_method.polar_color_space) {
        case Gfx::PolarColorSpace::Hsl:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kHSL;
            break;
        case Gfx::PolarColorSpace::Hwb:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kHWB;
            break;
        case Gfx::PolarColorSpace::Lch:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kLCH;
            break;
        case Gfx::PolarColorSpace::Oklch:
            interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kOKLCH;
            break;
        }

        switch (interpolation_method.hue_interpolation_method) {
        case Gfx::HueInterpolationMethod::Shorter:
            interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kShorter;
            break;
        case Gfx::HueInterpolationMethod::Longer:
            interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kLonger;
            break;
        case Gfx::HueInterpolationMethod::Increasing:
            interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kIncreasing;
            break;
        case Gfx::HueInterpolationMethod::Decreasing:
            interpolation.fHueMethod = SkGradientShader::Interpolation::HueMethod::kDecreasing;
            break;
        }
    }

    interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
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

void DisplayListPlayerSkia::paint_linear_gradient(PaintLinearGradient const& command)
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
    auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), color_space, color_stop_positions.data(), color_stop_positions.size(), SkTileMode::kRepeat, interpolation, &matrix);

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
    draw_glyph_run({ .font_id = command.font_id,
        .glyphs = command.glyphs,
        .rect = command.text_rect,
        .glyph_bounding_rect = command.shadow_bounding_rect,
        .translation = command.draw_location + command.text_rect.location().to_type<float>(),
        .scale = command.scale,
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

    Vector<SkColor> colors;
    colors.ensure_capacity(color_stop_colors.size());
    Vector<SkScalar> positions;
    positions.ensure_capacity(color_stop_positions.size());

    for (auto color : color_stop_colors)
        colors.unchecked_append(to_skia_color(color));
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

    switch (paint_style.type) {
    case DisplayListPaintStyleType::None:
        return {};
    case DisplayListPaintStyleType::LinearGradient:
        return make_gradient_paint([&](Vector<SkColor> const& colors, Vector<SkScalar> const& positions, SkTileMode tile_mode, SkMatrix const& matrix) {
            Array points {
                to_skia_point(paint_style.linear_gradient_start_point),
                to_skia_point(paint_style.linear_gradient_end_point),
            };
            return SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), colors.size(), tile_mode, 0, &matrix);
        });
    case DisplayListPaintStyleType::RadialGradient:
        return make_gradient_paint([&](Vector<SkColor> const& colors, Vector<SkScalar> const& positions, SkTileMode tile_mode, SkMatrix const& matrix) {
            auto start_center = to_skia_point(paint_style.radial_gradient_start_center);
            auto end_center = to_skia_point(paint_style.radial_gradient_end_center);
            return SkGradientShader::MakeTwoPointConical(start_center, paint_style.radial_gradient_start_radius, end_center, paint_style.radial_gradient_end_radius, colors.data(), positions.data(), colors.size(), tile_mode, 0, &matrix);
        });
    case DisplayListPaintStyleType::Pattern: {
        auto const& tile_rect = paint_style.pattern_tile_rect;
        auto tile_size = Gfx::IntSize(ceilf(tile_rect.width()), ceilf(tile_rect.height()));
        if (tile_size.is_empty())
            return {};

        auto tile_surface = Gfx::PaintingSurface::create_with_size(tile_size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia_backend_context);

        auto const& tile_display_list = active_display_list().resource_storage().display_list(paint_style.pattern_tile_display_list_id);
        execute_display_list_into_surface(tile_display_list, *tile_surface);

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

void DisplayListPlayerSkia::fill_path(FillPath const& command)
{
    auto path = Gfx::to_skia_path(path_from_data(command.path_data));
    path.setFillType(to_skia_path_fill_type(command.winding_rule));

    SkPaint paint;
    if (command.paint_kind == PathPaintKind::PaintStyle) {
        paint = paint_style_to_skia_paint(command.paint_style, command.bounding_rect().to_type<float>());
        paint.setAlphaf(command.opacity);
    } else {
        paint.setColor(to_skia_color(command.color));
    }
    paint.setAntiAlias(command.should_anti_alias == Gfx::ShouldAntiAlias::Yes);
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::stroke_path(StrokePath const& command)
{
    auto path = Gfx::to_skia_path(path_from_data(command.path_data));
    SkPaint paint;
    if (command.paint_kind == PathPaintKind::PaintStyle) {
        paint = paint_style_to_skia_paint(command.paint_style, command.bounding_rect().to_type<float>());
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
    paint.setPathEffect(SkDashPathEffect::Make(dash_array.data(), dash_array.size(), command.dash_offset));
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

    if (command.has_backdrop_filter) {
        auto filter = Gfx::deserialize_filter(inline_data(command.backdrop_filter_data), [&](u64 image_id) {
            return active_display_list().resource_storage().image_frame(ImageFrameResourceId { image_id });
        });
        auto image_filter = to_skia_image_filter(filter);
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
    auto shader = SkGradientShader::MakeRadial(center, size.height(), colors.data(), color_space, color_stop_positions.data(), color_stop_positions.size(), tile_mode, interpolation, &matrix);

    SkPaint paint;
    paint.setDither(true);
    paint.setAntiAlias(true);
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::paint_conic_gradient(PaintConicGradient const& command)
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
    auto shader = SkGradientShader::MakeSweep(center.x(), center.y(), colors.data(), color_space, color_stop_positions.data(), color_stop_positions.size(), SkTileMode::kRepeat, 0, 360, interpolation, &matrix);

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
    auto clip_op = command.corner_clip == Gfx::CornerClip::Inside ? SkClipOp::kDifference : SkClipOp::kIntersect;
    canvas.clipRRect(rounded_rect, clip_op, true);
}

void DisplayListPlayerSkia::paint_nested_display_list(PaintNestedDisplayList const& command)
{
    auto& canvas = surface().canvas();
    canvas.translate(command.rect.x(), command.rect.y());
    ScrollStateSnapshot scroll_state_snapshot;
    auto command_bytes = inline_data(command.command_bytes);
    auto& nested_display_list = active_display_list().resource_storage().display_list(command.display_list_id);
    execute_nested_display_list(nested_display_list, scroll_state_snapshot, command_bytes);
}

void DisplayListPlayerSkia::compositor_scroll_node(CompositorScrollNode const&)
{
}

void DisplayListPlayerSkia::compositor_sticky_area(CompositorStickyArea const&)
{
}

void DisplayListPlayerSkia::compositor_wheel_hit_test_target(CompositorWheelHitTestTarget const&)
{
}

void DisplayListPlayerSkia::compositor_main_thread_wheel_event_region(CompositorMainThreadWheelEventRegion const&)
{
}

void DisplayListPlayerSkia::compositor_viewport_scrollbar(CompositorViewportScrollbar const&)
{
}

void DisplayListPlayerSkia::compositor_blocking_wheel_event_region(CompositorBlockingWheelEventRegion const&)
{
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

void DisplayListPlayerSkia::apply_effects(ApplyEffects const& command, Gfx::Filter const* filter)
{
    auto& canvas = surface().canvas();
    SkPaint paint;

    if (command.opacity < 1.0f)
        paint.setAlphaf(command.opacity);

    if (command.compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
        paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));

    Optional<Gfx::Filter> deserialized_filter;
    if (command.has_filter) {
        if (!filter) {
            deserialized_filter = Gfx::deserialize_filter(inline_data(command.filter_data), [&](u64 image_id) {
                return active_display_list().resource_storage().image_frame(ImageFrameResourceId { image_id });
            });
            filter = &deserialized_filter.value();
        }
        paint.setImageFilter(to_skia_image_filter(*filter));
    }

    if (command.has_mask_kind && command.mask_kind == Gfx::MaskKind::Luminance)
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
