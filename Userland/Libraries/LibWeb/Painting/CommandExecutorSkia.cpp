/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

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
#include <pathops/SkPathOps.h>

#include <LibGfx/Filters/StackBlurFilter.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/CommandExecutorSkia.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

class CommandExecutorSkia::SkiaSurface {
public:
    SkCanvas& canvas() const { return *surface->getCanvas(); }

    SkiaSurface(sk_sp<SkSurface> surface)
        : surface(move(surface))
    {
    }

private:
    sk_sp<SkSurface> surface;
};

static SkRect to_skia_rect(auto const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

static SkColor to_skia_color(Gfx::Color const& color)
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
        subpath_last_point = point;
        switch (segment.command()) {
        case Gfx::PathSegment::Command::MoveTo: {
            if (subpath_start_point.has_value())
                close_subpath_if_needed(subpath_start_point.value());
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

static SkBitmap to_skia_bitmap(Gfx::Bitmap const& bitmap)
{
    SkColorType color_type = to_skia_color_type(bitmap.format());
    SkImageInfo image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, kUnpremul_SkAlphaType);
    SkBitmap sk_bitmap;
    sk_bitmap.setInfo(image_info);

    if (!sk_bitmap.installPixels(image_info, (void*)bitmap.begin(), bitmap.width() * 4)) {
        VERIFY_NOT_REACHED();
    }

    return sk_bitmap;
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

#define APPLY_PATH_CLIP_IF_NEEDED                                  \
    ScopeGuard restore_path_clip { [&] {                           \
        if (command.clip_paths.size() > 0)                         \
            surface().canvas().restore();                          \
    } };                                                           \
    if (command.clip_paths.size() > 0) {                           \
        surface().canvas().save();                                 \
        for (auto const& path : command.clip_paths)                \
            surface().canvas().clipPath(to_skia_path(path), true); \
    }

CommandExecutorSkia::CommandExecutorSkia(Gfx::Bitmap& bitmap)
{
    VERIFY(bitmap.format() == Gfx::BitmapFormat::BGRA8888);
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap.begin(), bitmap.width() * 4);
    VERIFY(surface);
    m_surface = make<SkiaSurface>(surface);
}

CommandExecutorSkia::~CommandExecutorSkia() = default;

CommandExecutorSkia::SkiaSurface& CommandExecutorSkia::surface() const
{
    return static_cast<SkiaSurface&>(*m_surface);
}

CommandResult CommandExecutorSkia::draw_glyph_run(DrawGlyphRun const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColorFilter(SkColorFilters::Blend(to_skia_color(command.color), SkBlendMode::kSrcIn));
    auto const& glyphs = command.glyph_run->glyphs();
    for (auto const& glyph_or_emoji : glyphs) {
        auto transformed_glyph = glyph_or_emoji;
        transformed_glyph.visit([&](auto& glyph) {
            glyph.position = glyph.position.scaled(command.scale).translated(command.translation);
            glyph.font = glyph.font->with_size(glyph.font->point_size() * static_cast<float>(command.scale));
        });
        if (transformed_glyph.has<Gfx::DrawGlyph>()) {
            auto& glyph = transformed_glyph.get<Gfx::DrawGlyph>();
            auto const& point = glyph.position;
            auto const& code_point = glyph.code_point;
            auto top_left = point + Gfx::FloatPoint(glyph.font->glyph_left_bearing(code_point), 0);
            auto glyph_position = Gfx::GlyphRasterPosition::get_nearest_fit_for(top_left);
            auto maybe_font_glyph = glyph.font->glyph(code_point, glyph_position.subpixel_offset);
            if (!maybe_font_glyph.has_value())
                continue;
            if (maybe_font_glyph->is_color_bitmap()) {
                TODO();
            } else {
                auto sk_bitmap = to_skia_bitmap(*maybe_font_glyph->bitmap());
                auto sk_image = SkImages::RasterFromBitmap(sk_bitmap);
                auto const& blit_position = glyph_position.blit_position;
                canvas.drawImage(sk_image, blit_position.x(), blit_position.y(), SkSamplingOptions(), &paint);
            }
        }
    }
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_text(DrawText const&)
{
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::fill_rect(FillRect const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_scaled_bitmap(DrawScaledBitmap const& command)
{
    auto src_rect = to_skia_rect(command.src_rect);
    auto dst_rect = to_skia_rect(command.dst_rect);
    auto bitmap = to_skia_bitmap(command.bitmap);
    auto image = SkImages::RasterFromBitmap(bitmap);
    auto& canvas = surface().canvas();
    SkPaint paint;
    canvas.drawImageRect(image, src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto src_rect = to_skia_rect(command.src_rect);
    auto dst_rect = to_skia_rect(command.dst_rect);
    auto bitmap = to_skia_bitmap(command.bitmap->bitmap());
    auto image = SkImages::RasterFromBitmap(bitmap);
    auto& canvas = surface().canvas();
    SkPaint paint;
    canvas.drawImageRect(image, src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::add_clip_rect(AddClipRect const& command)
{
    auto& canvas = surface().canvas();
    auto const& rect = command.rect;
    canvas.clipRect(to_skia_rect(rect));
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::save(Save const&)
{
    auto& canvas = surface().canvas();
    canvas.save();
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::restore(Restore const&)
{
    auto& canvas = surface().canvas();
    canvas.restore();
    return CommandResult::Continue;
}

static SkBitmap alpha_mask_from_bitmap(Gfx::Bitmap const& bitmap, Gfx::Bitmap::MaskKind kind)
{
    SkBitmap alpha_mask;
    alpha_mask.allocPixels(SkImageInfo::MakeA8(bitmap.width(), bitmap.height()));
    for (int y = 0; y < bitmap.height(); y++) {
        for (int x = 0; x < bitmap.width(); x++) {
            if (kind == Gfx::Bitmap::MaskKind::Luminance) {
                auto color = bitmap.get_pixel(x, y);
                *alpha_mask.getAddr8(x, y) = color.alpha() * color.luminosity() / 255;
            } else {
                VERIFY(kind == Gfx::Bitmap::MaskKind::Alpha);
                auto color = bitmap.get_pixel(x, y);
                *alpha_mask.getAddr8(x, y) = color.alpha();
            }
        }
    }
    return alpha_mask;
}

CommandResult CommandExecutorSkia::push_stacking_context(PushStackingContext const& command)
{
    auto& canvas = surface().canvas();

    auto affine_transform = Gfx::extract_2d_affine_transform(command.transform.matrix);
    auto new_transform = Gfx::AffineTransform {}
                             .set_translation(command.post_transform_translation.to_type<float>())
                             .translate(command.transform.origin)
                             .multiply(affine_transform)
                             .translate(-command.transform.origin);
    auto matrix = to_skia_matrix(new_transform);

    if (command.opacity < 1) {
        auto source_paintable_rect = to_skia_rect(command.source_paintable_rect);
        SkRect dest;
        matrix.mapRect(&dest, source_paintable_rect);
        canvas.saveLayerAlphaf(&dest, command.opacity);
    } else {
        canvas.save();
    }

    if (command.mask.has_value()) {
        auto alpha_mask = alpha_mask_from_bitmap(*command.mask.value().mask_bitmap, command.mask.value().mask_kind);
        SkMatrix mask_matrix;
        auto mask_position = command.source_paintable_rect.location();
        mask_matrix.setTranslate(mask_position.x(), mask_position.y());
        auto shader = alpha_mask.makeShader(SkSamplingOptions(), mask_matrix);
        canvas.clipShader(shader);
    }

    if (command.is_fixed_position) {
        // FIXME: Resetting matrix is not correct when element is nested in a transformed stacking context
        canvas.resetMatrix();
    }
    canvas.concat(matrix);

    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::pop_stacking_context(PopStackingContext const&)
{
    surface().canvas().restore();
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::paint_linear_gradient(PaintLinearGradient const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& linear_gradient_data = command.linear_gradient_data;

    // FIXME: Account for repeat length
    Vector<SkColor> colors;
    colors.ensure_capacity(linear_gradient_data.color_stops.list.size());
    Vector<SkScalar> positions;
    positions.ensure_capacity(linear_gradient_data.color_stops.list.size());
    auto const& list = linear_gradient_data.color_stops.list;
    for (auto const& color_stop : linear_gradient_data.color_stops.list) {
        // FIXME: Account for ColorStop::transition_hint
        colors.append(to_skia_color(color_stop.color));
        positions.append(color_stop.position);
    }

    auto const& rect = command.gradient_rect;
    auto length = calculate_gradient_length<int>(rect.size(), linear_gradient_data.gradient_angle);
    auto bottom = rect.center().translated(0, -length / 2);
    auto top = rect.center().translated(0, length / 2);

    Array<SkPoint, 2> points;
    points[0] = SkPoint::Make(top.x(), top.y());
    points[1] = SkPoint::Make(bottom.x(), bottom.y());

    auto center = to_skia_rect(rect).center();
    SkMatrix matrix;
    matrix.setRotate(linear_gradient_data.gradient_angle, center.x(), center.y());

    auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), list.size(), SkTileMode::kClamp, 0, &matrix);

    SkPaint paint;
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);

    return CommandResult::Continue;
}

static void add_spread_distance_to_border_radius(int& border_radius, int spread_distance)
{
    if (border_radius == 0 || spread_distance == 0)
        return;

    // https://drafts.csswg.org/css-backgrounds/#shadow-shape
    // To preserve the box’s shape when spread is applied, the corner radii of the shadow are also increased (decreased,
    // for inner shadows) from the border-box (padding-box) radii by adding (subtracting) the spread distance (and flooring
    // at zero). However, in order to create a sharper corner when the border radius is small (and thus ensure continuity
    // between round and sharp corners), when the border radius is less than the spread distance (or in the case of an inner
    // shadow, less than the absolute value of a negative spread distance), the spread distance is first multiplied by the
    // proportion 1 + (r-1)^3, where r is the ratio of the border radius to the spread distance, in calculating the corner
    // radii of the spread shadow shape.
    if (border_radius > AK::abs(spread_distance)) {
        border_radius += spread_distance;
    } else {
        auto r = (float)border_radius / AK::abs(spread_distance);
        border_radius += spread_distance * (1 + AK::pow(r - 1, 3.0f));
    }
}

CommandResult CommandExecutorSkia::paint_outer_box_shadow(PaintOuterBoxShadow const& command)
{
    auto const& outer_box_shadow_params = command.box_shadow_params;
    auto const& color = outer_box_shadow_params.color;
    auto const& spread_distance = outer_box_shadow_params.spread_distance;
    auto const& blur_radius = outer_box_shadow_params.blur_radius;

    auto content_rrect = to_skia_rrect(outer_box_shadow_params.device_content_rect, outer_box_shadow_params.corner_radii);

    auto shadow_rect = outer_box_shadow_params.device_content_rect;
    shadow_rect.inflate(spread_distance, spread_distance, spread_distance, spread_distance);
    auto offset_x = outer_box_shadow_params.offset_x;
    auto offset_y = outer_box_shadow_params.offset_y;
    shadow_rect.translate_by(offset_x, offset_y);

    auto add_spread_distance_to_corner_radius = [&](auto& corner_radius) {
        add_spread_distance_to_border_radius(corner_radius.horizontal_radius, spread_distance);
        add_spread_distance_to_border_radius(corner_radius.vertical_radius, spread_distance);
    };

    auto corner_radii = outer_box_shadow_params.corner_radii;
    add_spread_distance_to_corner_radius(corner_radii.top_left);
    add_spread_distance_to_corner_radius(corner_radii.top_right);
    add_spread_distance_to_corner_radius(corner_radii.bottom_right);
    add_spread_distance_to_corner_radius(corner_radii.bottom_left);

    auto& canvas = surface().canvas();
    canvas.save();
    canvas.clipRRect(content_rrect, SkClipOp::kDifference, true);
    SkPaint paint;
    paint.setColor(to_skia_color(color));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    auto shadow_rounded_rect = to_skia_rrect(shadow_rect, corner_radii);
    canvas.drawRRect(shadow_rounded_rect, paint);
    canvas.restore();

    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::paint_inner_box_shadow(PaintInnerBoxShadow const& command)
{
    auto const& outer_box_shadow_params = command.box_shadow_params;
    auto color = outer_box_shadow_params.color;
    auto device_content_rect = outer_box_shadow_params.device_content_rect;
    auto offset_x = outer_box_shadow_params.offset_x;
    auto offset_y = outer_box_shadow_params.offset_y;
    auto blur_radius = outer_box_shadow_params.blur_radius;
    auto spread_distance = outer_box_shadow_params.spread_distance;
    auto const& corner_radii = outer_box_shadow_params.corner_radii;

    auto outer_shadow_rect = device_content_rect.translated({ offset_x, offset_y });
    auto inner_shadow_rect = outer_shadow_rect.inflated(-spread_distance, -spread_distance, -spread_distance, -spread_distance);
    outer_shadow_rect.inflate(
        blur_radius + offset_y,
        blur_radius + abs(offset_x),
        blur_radius + abs(offset_y),
        blur_radius + offset_x);

    auto inner_rect_corner_radii = corner_radii;

    auto add_spread_distance_to_corner_radius = [&](auto& corner_radius) {
        add_spread_distance_to_border_radius(corner_radius.horizontal_radius, -spread_distance);
        add_spread_distance_to_border_radius(corner_radius.vertical_radius, -spread_distance);
    };

    add_spread_distance_to_corner_radius(inner_rect_corner_radii.top_left);
    add_spread_distance_to_corner_radius(inner_rect_corner_radii.top_right);
    add_spread_distance_to_corner_radius(inner_rect_corner_radii.bottom_right);
    add_spread_distance_to_corner_radius(inner_rect_corner_radii.bottom_left);

    auto outer_rect = to_skia_rrect(outer_shadow_rect, corner_radii);
    auto inner_rect = to_skia_rrect(inner_shadow_rect, inner_rect_corner_radii);

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
    path_paint.setColor(to_skia_color(color));
    path_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    canvas.save();
    canvas.clipRRect(to_skia_rrect(device_content_rect, corner_radii), true);
    canvas.drawPath(result_path, path_paint);
    canvas.restore();

    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::paint_text_shadow(PaintTextShadow const&)
{
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::fill_rect_with_rounded_corners(FillRectWithRoundedCorners const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& rect = command.rect;

    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColor(to_skia_color(command.color));

    SkRRect rounded_rect;
    SkVector radii[4];
    radii[0].set(command.top_left_radius.horizontal_radius, command.top_left_radius.vertical_radius);
    radii[1].set(command.top_right_radius.horizontal_radius, command.top_right_radius.vertical_radius);
    radii[2].set(command.bottom_right_radius.horizontal_radius, command.bottom_right_radius.vertical_radius);
    radii[3].set(command.bottom_left_radius.horizontal_radius, command.bottom_left_radius.vertical_radius);
    rounded_rect.setRectRadii(to_skia_rect(rect), radii);
    canvas.drawRRect(rounded_rect, paint);

    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::fill_path_using_color(FillPathUsingColor const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    auto path = to_skia_path(command.path);
    path.setFillType(to_skia_path_fill_type(command.winding_rule));
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    canvas.drawPath(path, paint);
    return CommandResult::Continue;
}

SkPaint paint_style_to_skia_paint(Painting::SVGGradientPaintStyle const& paint_style, Gfx::FloatRect bounding_rect)
{
    SkPaint paint;
    if (is<SVGLinearGradientPaintStyle>(paint_style)) {
        auto const& linear_gradient_paint_style = static_cast<SVGLinearGradientPaintStyle const&>(paint_style);

        SkMatrix matrix;
        auto scale = linear_gradient_paint_style.scale();
        auto start_point = linear_gradient_paint_style.start_point().scaled(scale);
        auto end_point = linear_gradient_paint_style.end_point().scaled(scale);

        start_point.translate_by(bounding_rect.location());
        end_point.translate_by(bounding_rect.location());

        Array<SkPoint, 2> points;
        points[0] = SkPoint::Make(start_point.x(), start_point.y());
        points[1] = SkPoint::Make(end_point.x(), end_point.y());

        auto const& color_stops = linear_gradient_paint_style.color_stops();

        Vector<SkColor> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());

        for (auto const& color_stop : linear_gradient_paint_style.color_stops()) {
            colors.append(to_skia_color(color_stop.color));
            positions.append(color_stop.position);
        }

        auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), color_stops.size(), SkTileMode::kClamp, 0, &matrix);
        paint.setShader(shader);
    } else if (is<SVGRadialGradientPaintStyle>(paint_style)) {
        // TODO:
    }

    return paint;
}

CommandResult CommandExecutorSkia::fill_path_using_paint_style(FillPathUsingPaintStyle const& command)
{
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    path.setFillType(to_skia_path_fill_type(command.winding_rule));
    auto paint = paint_style_to_skia_paint(*command.paint_style, command.bounding_rect().to_type<float>());
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    surface().canvas().drawPath(path, paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::stroke_path_using_color(StrokePathUsingColor const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    canvas.drawPath(path, paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::stroke_path_using_paint_style(StrokePathUsingPaintStyle const& command)
{
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    auto paint = paint_style_to_skia_paint(*command.paint_style, command.bounding_rect().to_type<float>());
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    surface().canvas().drawPath(path, paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_ellipse(DrawEllipse const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    canvas.drawOval(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::fill_ellipse(FillEllipse const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    canvas.drawOval(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_line(DrawLine const& command)
{
    auto from = SkPoint::Make(command.from.x(), command.from.y());
    auto to = SkPoint::Make(command.to.x(), command.to.y());
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    canvas.drawLine(from, to, paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::apply_backdrop_filter(ApplyBackdropFilter const& command)
{
    auto& canvas = surface().canvas();

    auto rect = to_skia_rect(command.backdrop_region);
    canvas.save();
    canvas.clipRect(rect);
    ScopeGuard guard = [&] { canvas.restore(); };

    for (auto const& filter_function : command.backdrop_filter.filters) {
        // See: https://drafts.fxtf.org/filter-effects-1/#supported-filter-functions
        filter_function.visit(
            [&](CSS::ResolvedBackdropFilter::Blur const& blur_filter) {
                auto blur_image_filter = SkImageFilters::Blur(blur_filter.radius, blur_filter.radius, nullptr);
                canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, blur_image_filter.get(), 0));
                canvas.restore();
            },
            [&](CSS::ResolvedBackdropFilter::ColorOperation const& color) {
                auto amount = clamp(color.amount, 0.0f, 1.0f);

                // Matrices are taken from https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation
                sk_sp<SkColorFilter> color_filter;
                switch (color.operation) {
                case CSS::Filter::Color::Operation::Grayscale: {
                    float matrix[20] = {
                        0.2126f + 0.7874f * (1 - amount), 0.7152f - 0.7152f * (1 - amount), 0.0722f - 0.0722f * (1 - amount), 0, 0,
                        0.2126f - 0.2126f * (1 - amount), 0.7152f + 0.2848f * (1 - amount), 0.0722f - 0.0722f * (1 - amount), 0, 0,
                        0.2126f - 0.2126f * (1 - amount), 0.7152f - 0.7152f * (1 - amount), 0.0722f + 0.9278f * (1 - amount), 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Brightness: {
                    float matrix[20] = {
                        amount, 0, 0, 0, 0,
                        0, amount, 0, 0, 0,
                        0, 0, amount, 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Contrast: {
                    float intercept = -(0.5f * amount) + 0.5f;
                    float matrix[20] = {
                        amount, 0, 0, 0, intercept,
                        0, amount, 0, 0, intercept,
                        0, 0, amount, 0, intercept,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Invert: {
                    float matrix[20] = {
                        1 - 2 * amount, 0, 0, 0, amount,
                        0, 1 - 2 * amount, 0, 0, amount,
                        0, 0, 1 - 2 * amount, 0, amount,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Opacity: {
                    float matrix[20] = {
                        1, 0, 0, 0, 0,
                        0, 1, 0, 0, 0,
                        0, 0, 1, 0, 0,
                        0, 0, 0, amount, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Sepia: {
                    float matrix[20] = {
                        0.393f + 0.607f * (1 - amount), 0.769f - 0.769f * (1 - amount), 0.189f - 0.189f * (1 - amount), 0, 0,
                        0.349f - 0.349f * (1 - amount), 0.686f + 0.314f * (1 - amount), 0.168f - 0.168f * (1 - amount), 0, 0,
                        0.272f - 0.272f * (1 - amount), 0.534f - 0.534f * (1 - amount), 0.131f + 0.869f * (1 - amount), 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Saturate: {
                    float matrix[20] = {
                        0.213f + 0.787f * amount, 0.715f - 0.715f * amount, 0.072f - 0.072f * amount, 0, 0,
                        0.213f - 0.213f * amount, 0.715f + 0.285f * amount, 0.072f - 0.072f * amount, 0, 0,
                        0.213f - 0.213f * amount, 0.715f - 0.715f * amount, 0.072f + 0.928f * amount, 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                default:
                    VERIFY_NOT_REACHED();
                }

                auto image_filter = SkImageFilters::ColorFilter(color_filter, nullptr);
                canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
                canvas.restore();
            },
            [&](CSS::ResolvedBackdropFilter::HueRotate const& hue_rotate) {
                float radians = AK::to_radians(hue_rotate.angle_degrees);

                auto cosA = cos(radians);
                auto sinA = sin(radians);

                auto a00 = 0.213f + cosA * 0.787f - sinA * 0.213f;
                auto a01 = 0.715f - cosA * 0.715f - sinA * 0.715f;
                auto a02 = 0.072f - cosA * 0.072f + sinA * 0.928f;
                auto a10 = 0.213f - cosA * 0.213f + sinA * 0.143f;
                auto a11 = 0.715f + cosA * 0.285f + sinA * 0.140f;
                auto a12 = 0.072f - cosA * 0.072f - sinA * 0.283f;
                auto a20 = 0.213f - cosA * 0.213f - sinA * 0.787f;
                auto a21 = 0.715f - cosA * 0.715f + sinA * 0.715f;
                auto a22 = 0.072f + cosA * 0.928f + sinA * 0.072f;

                float matrix[20] = {
                    a00, a01, a02, 0, 0,
                    a10, a11, a12, 0, 0,
                    a20, a21, a22, 0, 0,
                    0, 0, 0, 1, 0
                };

                auto color_filter = SkColorFilters::Matrix(matrix);
                auto image_filter = SkImageFilters::ColorFilter(color_filter, nullptr);
                canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
                canvas.restore();
            },
            [&](CSS::ResolvedBackdropFilter::DropShadow const&) {
                dbgln("TODO: Implement drop-shadow() filter function!");
            });
    }

    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_rect(DrawRect const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1);
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::paint_radial_gradient(PaintRadialGradient const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& linear_gradient_data = command.radial_gradient_data;

    // FIXME: Account for repeat length
    Vector<SkColor> colors;
    colors.ensure_capacity(linear_gradient_data.color_stops.list.size());
    Vector<SkScalar> positions;
    positions.ensure_capacity(linear_gradient_data.color_stops.list.size());
    auto const& list = linear_gradient_data.color_stops.list;
    for (auto const& color_stop : linear_gradient_data.color_stops.list) {
        // FIXME: Account for ColorStop::transition_hint
        colors.append(to_skia_color(color_stop.color));
        positions.append(color_stop.position);
    }

    auto const& rect = command.rect;
    auto center = SkPoint::Make(command.center.x(), command.center.y());
    auto radius = command.size.height();
    auto shader = SkGradientShader::MakeRadial(center, radius, colors.data(), positions.data(), list.size(), SkTileMode::kClamp, 0);

    SkPaint paint;
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);

    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::paint_conic_gradient(PaintConicGradient const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::draw_triangle_wave(DrawTriangleWave const&)
{
    return CommandResult::Continue;
}

void CommandExecutorSkia::prepare_to_execute(size_t)
{
}

CommandResult CommandExecutorSkia::sample_under_corners(SampleUnderCorners const& command)
{
    auto rounded_rect = to_skia_rrect(command.border_rect, command.corner_radii);
    auto& canvas = surface().canvas();
    canvas.save();
    auto clip_op = command.corner_clip == CornerClip::Inside ? SkClipOp::kDifference : SkClipOp::kIntersect;
    canvas.clipRRect(rounded_rect, clip_op, true);
    return CommandResult::Continue;
}

CommandResult CommandExecutorSkia::blit_corner_clipping(BlitCornerClipping const&)
{
    auto& canvas = surface().canvas();
    canvas.restore();
    return CommandResult::Continue;
}

bool CommandExecutorSkia::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    return surface().canvas().quickReject(to_skia_rect(rect));
}

}
