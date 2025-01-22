/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <core/SkBitmap.h>
#include <core/SkBlurTypes.h>
#include <core/SkCanvas.h>
#include <core/SkFont.h>
#include <core/SkMaskFilter.h>
#include <core/SkPath.h>
#include <core/SkPathEffect.h>
#include <core/SkRRect.h>
#include <core/SkSurface.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradientShader.h>
#include <effects/SkImageFilters.h>
#include <effects/SkRuntimeEffect.h>
#include <gpu/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <pathops/SkPathOps.h>

#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/SkiaUtils.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

DisplayListPlayerSkia::DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext> context)
    : m_context(context)
{
}

DisplayListPlayerSkia::DisplayListPlayerSkia()
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

void DisplayListPlayerSkia::flush()
{
    if (m_context)
        m_context->flush_and_submit(&surface().sk_surface());
    surface().flush();
}

void DisplayListPlayerSkia::draw_glyph_run(DrawGlyphRun const& command)
{
    auto const& gfx_font = static_cast<Gfx::ScaledFont const&>(command.glyph_run->font());
    auto sk_font = gfx_font.skia_font(command.scale);

    auto glyph_count = command.glyph_run->glyphs().size();
    Vector<SkGlyphID> glyphs;
    glyphs.ensure_capacity(glyph_count);
    Vector<SkPoint> positions;
    positions.ensure_capacity(glyph_count);
    auto font_ascent = gfx_font.pixel_metrics().ascent;
    for (auto const& glyph : command.glyph_run->glyphs()) {
        auto transformed_glyph = glyph;
        transformed_glyph.position.set_y(glyph.position.y() + font_ascent);
        transformed_glyph.position = transformed_glyph.position.scaled(command.scale);
        auto const& point = transformed_glyph.position;
        glyphs.append(transformed_glyph.glyph_id);
        positions.append(to_skia_point(point));
    }

    SkPaint paint;
    paint.setColor(to_skia_color(command.color));

    auto& canvas = surface().canvas();
    switch (command.orientation) {
    case Gfx::Orientation::Horizontal:
        canvas.drawGlyphs(glyphs.size(), glyphs.data(), positions.data(), to_skia_point(command.translation), sk_font, paint);
        break;
    case Gfx::Orientation::Vertical:
        canvas.save();
        canvas.translate(command.rect.width(), 0);
        canvas.rotate(90, command.rect.top_left().x(), command.rect.top_left().y());
        canvas.drawGlyphs(glyphs.size(), glyphs.data(), positions.data(), to_skia_point(command.translation), sk_font, paint);
        canvas.restore();
        break;
    }
}

void DisplayListPlayerSkia::fill_rect(FillRect const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::draw_painting_surface(DrawPaintingSurface const& command)
{
    auto src_rect = to_skia_rect(command.src_rect);
    auto dst_rect = to_skia_rect(command.dst_rect);
    auto& sk_surface = command.surface->sk_surface();
    auto& canvas = surface().canvas();
    auto image = sk_surface.makeImageSnapshot();
    SkPaint paint;
    canvas.drawImageRect(image, src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void DisplayListPlayerSkia::draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const& command)
{
    auto src_rect = to_skia_rect(command.src_rect);
    auto dst_rect = to_skia_rect(command.dst_rect);
    auto& canvas = surface().canvas();
    SkPaint paint;
    canvas.drawImageRect(command.bitmap->sk_image(), src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void DisplayListPlayerSkia::draw_repeated_immutable_bitmap(DrawRepeatedImmutableBitmap const& command)
{
    SkMatrix matrix;
    auto dst_rect = command.dst_rect.to_type<float>();
    auto src_size = command.bitmap->size().to_type<float>();
    matrix.setScale(dst_rect.width() / src_size.width(), dst_rect.height() / src_size.height());
    matrix.postTranslate(dst_rect.x(), dst_rect.y());
    auto sampling_options = to_skia_sampling_options(command.scaling_mode);

    auto tile_mode_x = command.repeat.x ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto tile_mode_y = command.repeat.y ? SkTileMode::kRepeat : SkTileMode::kDecal;
    auto shader = command.bitmap->sk_image()->makeShader(tile_mode_x, tile_mode_y, sampling_options, matrix);

    SkPaint paint;
    paint.setShader(shader);
    auto& canvas = surface().canvas();
    canvas.drawPaint(paint);
}

void DisplayListPlayerSkia::add_clip_rect(AddClipRect const& command)
{
    auto& canvas = surface().canvas();
    auto const& rect = command.rect;
    canvas.clipRect(to_skia_rect(rect));
}

void DisplayListPlayerSkia::save(Save const&)
{
    auto& canvas = surface().canvas();
    canvas.save();
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

void DisplayListPlayerSkia::push_stacking_context(PushStackingContext const& command)
{
    auto& canvas = surface().canvas();

    auto affine_transform = Gfx::extract_2d_affine_transform(command.transform.matrix);
    auto new_transform = Gfx::AffineTransform {}
                             .translate(command.transform.origin)
                             .multiply(affine_transform)
                             .translate(-command.transform.origin);
    auto matrix = to_skia_matrix(new_transform);

    if (command.opacity < 1 || command.compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal || command.isolate) {
        auto source_paintable_rect = to_skia_rect(command.source_paintable_rect);
        SkRect dest;
        matrix.mapRect(&dest, source_paintable_rect);

        SkPaint paint;
        paint.setAlphaf(command.opacity);
        paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));
        canvas.saveLayer(&dest, &paint);
    } else {
        canvas.save();
    }

    if (command.clip_path.has_value())
        canvas.clipPath(to_skia_path(command.clip_path.value()), true);

    canvas.concat(matrix);
}

void DisplayListPlayerSkia::pop_stacking_context(PopStackingContext const&)
{
    surface().canvas().restore();
}

static ColorStopList replace_transition_hints_with_normal_color_stops(ColorStopList const& color_stop_list)
{
    ColorStopList stops_with_replaced_transition_hints;

    auto const& first_color_stop = color_stop_list.first();
    // First color stop in the list should never have transition hint value
    VERIFY(!first_color_stop.transition_hint.has_value());
    stops_with_replaced_transition_hints.empend(first_color_stop.color, first_color_stop.position);

    // This loop replaces transition hints with five regular points, calculated using the
    // formula defined in the spec. After rendering using linear interpolation, this will
    // produce a result close enough to that obtained if the color of each point were calculated
    // using the non-linear formula from the spec.
    for (size_t i = 1; i < color_stop_list.size(); i++) {
        auto const& color_stop = color_stop_list[i];
        if (!color_stop.transition_hint.has_value()) {
            stops_with_replaced_transition_hints.empend(color_stop.color, color_stop.position);
            continue;
        }

        auto const& previous_color_stop = color_stop_list[i - 1];
        auto const& next_color_stop = color_stop_list[i];

        auto distance_between_stops = next_color_stop.position - previous_color_stop.position;
        auto transition_hint = color_stop.transition_hint.value();

        Array transition_hint_relative_sampling_positions {
            transition_hint * 0.33f,
            transition_hint * 0.66f,
            transition_hint,
            transition_hint + (1.f - transition_hint) * 0.33f,
            transition_hint + (1.f - transition_hint) * 0.66f,
        };

        for (auto const& transition_hint_relative_sampling_position : transition_hint_relative_sampling_positions) {
            auto position = previous_color_stop.position + transition_hint_relative_sampling_position * distance_between_stops;
            auto value = color_stop_step(previous_color_stop, next_color_stop, position);
            auto color = previous_color_stop.color.interpolate(next_color_stop.color, value);
            stops_with_replaced_transition_hints.empend(color, position);
        }

        stops_with_replaced_transition_hints.empend(color_stop.color, color_stop.position);
    }

    return stops_with_replaced_transition_hints;
}

static ColorStopList expand_repeat_length(ColorStopList const& color_stop_list, float repeat_length)
{
    // https://drafts.csswg.org/css-images/#repeating-gradients
    // When rendered, however, the color-stops are repeated infinitely in both directions, with their
    // positions shifted by multiples of the difference between the last specified color-stop’s position
    // and the first specified color-stop’s position. For example, repeating-linear-gradient(red 10px, blue 50px)
    // is equivalent to linear-gradient(..., red -30px, blue 10px, red 10px, blue 50px, red 50px, blue 90px, ...).

    auto first_stop_position = color_stop_list.first().position;
    int const negative_repeat_count = AK::ceil(first_stop_position / repeat_length);
    int const positive_repeat_count = AK::ceil((1.0f - first_stop_position) / repeat_length);

    ColorStopList color_stop_list_with_expanded_repeat = color_stop_list;

    auto get_color_between_stops = [](float position, auto const& current_stop, auto const& previous_stop) {
        auto distance_between_stops = current_stop.position - previous_stop.position;
        auto percentage = (position - previous_stop.position) / distance_between_stops;
        return previous_stop.color.interpolate(current_stop.color, percentage);
    };

    for (auto repeat_count = 1; repeat_count <= negative_repeat_count; repeat_count++) {
        for (auto stop : color_stop_list.in_reverse()) {
            stop.position += repeat_length * static_cast<float>(-repeat_count);
            if (stop.position < 0) {
                stop.color = get_color_between_stops(0.0f, stop, color_stop_list_with_expanded_repeat.first());
                color_stop_list_with_expanded_repeat.prepend(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.prepend(stop);
        }
    }

    for (auto repeat_count = 0; repeat_count < positive_repeat_count; repeat_count++) {
        for (auto stop : color_stop_list) {
            stop.position += repeat_length * static_cast<float>(repeat_count);
            if (stop.position > 1) {
                stop.color = get_color_between_stops(1.0f, stop, color_stop_list_with_expanded_repeat.last());
                color_stop_list_with_expanded_repeat.append(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.append(stop);
        }
    }

    return color_stop_list_with_expanded_repeat;
}

void DisplayListPlayerSkia::paint_linear_gradient(PaintLinearGradient const& command)
{
    auto const& linear_gradient_data = command.linear_gradient_data;
    auto color_stop_list = linear_gradient_data.color_stops.list;
    auto const& repeat_length = linear_gradient_data.color_stops.repeat_length;
    VERIFY(!color_stop_list.is_empty());
    if (repeat_length.has_value())
        color_stop_list = expand_repeat_length(color_stop_list, *repeat_length);

    auto stops_with_replaced_transition_hints = replace_transition_hints_with_normal_color_stops(color_stop_list);

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    for (size_t stop_index = 0; stop_index < stops_with_replaced_transition_hints.size(); stop_index++) {
        auto const& stop = stops_with_replaced_transition_hints[stop_index];
        if (stop_index > 0 && stop == stops_with_replaced_transition_hints[stop_index - 1])
            continue;
        colors.append(to_skia_color4f(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.gradient_rect;
    auto length = calculate_gradient_length<int>(rect.size(), linear_gradient_data.gradient_angle);
    auto bottom = rect.center().translated(0, -length / 2);
    auto top = rect.center().translated(0, length / 2);

    Array points {
        to_skia_point(top),
        to_skia_point(bottom),
    };

    auto center = to_skia_rect(rect).center();
    SkMatrix matrix;
    matrix.setRotate(linear_gradient_data.gradient_angle, center.x(), center.y());

    auto color_space = SkColorSpace::MakeSRGB();
    SkGradientShader::Interpolation interpolation = {};
    interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGB;
    interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
    auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), color_space, positions.data(), positions.size(), SkTileMode::kClamp, interpolation, &matrix);

    SkPaint paint;
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
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

void DisplayListPlayerSkia::paint_outer_box_shadow(PaintOuterBoxShadow const& command)
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
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(color));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    auto shadow_rounded_rect = to_skia_rrect(shadow_rect, corner_radii);
    canvas.drawRRect(shadow_rounded_rect, paint);
    canvas.restore();
}

void DisplayListPlayerSkia::paint_inner_box_shadow(PaintInnerBoxShadow const& command)
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
    path_paint.setAntiAlias(true);
    path_paint.setColor(to_skia_color(color));
    path_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    canvas.save();
    canvas.clipRRect(to_skia_rrect(device_content_rect, corner_radii), true);
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
    draw_glyph_run({
        .glyph_run = command.glyph_run,
        .scale = command.glyph_run_scale,
        .rect = command.text_rect,
        .translation = command.draw_location + command.text_rect.location().to_type<float>(),
        .color = command.color,
    });
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

void DisplayListPlayerSkia::fill_path_using_color(FillPathUsingColor const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    auto path = to_skia_path(command.path);
    path.setFillType(to_skia_path_fill_type(command.winding_rule));
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    canvas.drawPath(path, paint);
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

static SkPaint paint_style_to_skia_paint(Painting::SVGGradientPaintStyle const& paint_style, Gfx::FloatRect bounding_rect)
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

    return paint;
}

void DisplayListPlayerSkia::fill_path_using_paint_style(FillPathUsingPaintStyle const& command)
{
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    path.setFillType(to_skia_path_fill_type(command.winding_rule));
    auto paint = paint_style_to_skia_paint(*command.paint_style, command.bounding_rect().to_type<float>());
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::stroke_path_using_color(StrokePathUsingColor const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return;

    // FIXME: Use .cap_style, .join_style, .miter_limit, .dash_array, .dash_offset.
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    canvas.drawPath(path, paint);
}

void DisplayListPlayerSkia::stroke_path_using_paint_style(StrokePathUsingPaintStyle const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return;

    // FIXME: Use .cap_style, .join_style, .miter_limit, .dash_array, .dash_offset.
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    auto paint = paint_style_to_skia_paint(*command.paint_style, command.bounding_rect().to_type<float>());
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    surface().canvas().drawPath(path, paint);
}

void DisplayListPlayerSkia::draw_ellipse(DrawEllipse const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return;

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
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return;

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
    canvas.clipRect(rect);
    ScopeGuard guard = [&] { canvas.restore(); };

    for (auto const& filter : command.backdrop_filter) {
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
    auto const& radial_gradient_data = command.radial_gradient_data;

    auto color_stop_list = radial_gradient_data.color_stops.list;
    VERIFY(!color_stop_list.is_empty());
    auto const& repeat_length = radial_gradient_data.color_stops.repeat_length;
    if (repeat_length.has_value())
        color_stop_list = expand_repeat_length(color_stop_list, repeat_length.value());

    auto stops_with_replaced_transition_hints = replace_transition_hints_with_normal_color_stops(color_stop_list);

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    for (size_t stop_index = 0; stop_index < stops_with_replaced_transition_hints.size(); stop_index++) {
        auto const& stop = stops_with_replaced_transition_hints[stop_index];
        if (stop_index > 0 && stop == stops_with_replaced_transition_hints[stop_index - 1])
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
    matrix.setScale(size.width() / size.height(), 1.0f, center.x(), center.y());

    SkTileMode tile_mode = SkTileMode::kClamp;
    if (repeat_length.has_value())
        tile_mode = SkTileMode::kRepeat;

    auto color_space = SkColorSpace::MakeSRGB();
    SkGradientShader::Interpolation interpolation = {};
    interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGB;
    interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
    auto shader = SkGradientShader::MakeRadial(center, size.height(), colors.data(), color_space, positions.data(), positions.size(), tile_mode, interpolation, &matrix);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::paint_conic_gradient(PaintConicGradient const& command)
{
    auto const& conic_gradient_data = command.conic_gradient_data;

    auto color_stop_list = conic_gradient_data.color_stops.list;
    auto const& repeat_length = conic_gradient_data.color_stops.repeat_length;
    if (repeat_length.has_value())
        color_stop_list = expand_repeat_length(color_stop_list, repeat_length.value());

    VERIFY(!color_stop_list.is_empty());
    auto stops_with_replaced_transition_hints = replace_transition_hints_with_normal_color_stops(color_stop_list);

    Vector<SkColor4f> colors;
    Vector<SkScalar> positions;
    for (size_t stop_index = 0; stop_index < stops_with_replaced_transition_hints.size(); stop_index++) {
        auto const& stop = stops_with_replaced_transition_hints[stop_index];
        if (stop_index > 0 && stop == stops_with_replaced_transition_hints[stop_index - 1])
            continue;
        colors.append(to_skia_color4f(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.rect;
    auto center = command.position.translated(rect.location()).to_type<float>();

    SkMatrix matrix;
    matrix.setRotate(-90 + conic_gradient_data.start_angle, center.x(), center.y());
    auto color_space = SkColorSpace::MakeSRGB();
    SkGradientShader::Interpolation interpolation = {};
    interpolation.fColorSpace = SkGradientShader::Interpolation::ColorSpace::kSRGB;
    interpolation.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
    auto shader = SkGradientShader::MakeSweep(center.x(), center.y(), colors.data(), color_space, positions.data(), positions.size(), SkTileMode::kRepeat, 0, 360, interpolation, &matrix);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);
}

void DisplayListPlayerSkia::draw_triangle_wave(DrawTriangleWave const&)
{
}

void DisplayListPlayerSkia::add_rounded_rect_clip(AddRoundedRectClip const& command)
{
    auto rounded_rect = to_skia_rrect(command.border_rect, command.corner_radii);
    auto& canvas = surface().canvas();
    auto clip_op = command.corner_clip == CornerClip::Inside ? SkClipOp::kDifference : SkClipOp::kIntersect;
    canvas.clipRRect(rounded_rect, clip_op, true);
}

void DisplayListPlayerSkia::add_mask(AddMask const& command)
{
    auto const& rect = command.rect;
    if (rect.is_empty())
        return;

    auto mask_surface = Gfx::PaintingSurface::create_with_size(m_context, rect.size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);

    NonnullRefPtr old_surface = surface();
    set_surface(mask_surface);
    execute(*command.display_list);
    set_surface(old_surface);

    SkMatrix mask_matrix;
    mask_matrix.setTranslate(rect.x(), rect.y());
    auto image = mask_surface->sk_surface().makeImageSnapshot();
    auto shader = image->makeShader(SkSamplingOptions(), mask_matrix);
    surface().canvas().clipShader(shader);
}

void DisplayListPlayerSkia::paint_nested_display_list(PaintNestedDisplayList const& command)
{
    auto& canvas = surface().canvas();
    canvas.translate(command.rect.x(), command.rect.y());
    execute(*command.display_list);
}

void DisplayListPlayerSkia::paint_scrollbar(PaintScrollBar const& command)
{
    auto rect = to_skia_rect(command.rect);
    auto radius = rect.width() / 2;
    auto rrect = SkRRect::MakeRectXY(rect, radius, radius);

    auto& canvas = surface().canvas();

    auto fill_color = Color(Color::NamedColor::DarkGray).with_alpha(128);
    SkPaint fill_paint;
    fill_paint.setColor(to_skia_color(fill_color));
    canvas.drawRRect(rrect, fill_paint);

    auto stroke_color = Color(Color::NamedColor::LightGray).with_alpha(128);
    SkPaint stroke_paint;
    stroke_paint.setStroke(true);
    stroke_paint.setStrokeWidth(1);
    stroke_paint.setColor(to_skia_color(stroke_color));
    canvas.drawRRect(rrect, stroke_paint);
}

void DisplayListPlayerSkia::apply_opacity(ApplyOpacity const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAlphaf(command.opacity);
    canvas.saveLayer(nullptr, &paint);
}

void DisplayListPlayerSkia::apply_composite_and_blending_operator(ApplyCompositeAndBlendingOperator const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setBlender(Gfx::to_skia_blender(command.compositing_and_blending_operator));
    canvas.saveLayer(nullptr, &paint);
}

void DisplayListPlayerSkia::apply_filters(ApplyFilters const& command)
{
    if (command.filter.is_empty()) {
        return;
    }
    sk_sp<SkImageFilter> image_filter;
    auto append_filter = [&image_filter](auto new_filter) {
        if (image_filter)
            image_filter = SkImageFilters::Compose(new_filter, image_filter);
        else
            image_filter = new_filter;
    };

    // Apply filters in order
    for (auto filter : command.filter) {
        append_filter(to_skia_image_filter(filter));
    }

    SkPaint paint;
    paint.setImageFilter(image_filter);
    auto& canvas = surface().canvas();
    canvas.saveLayer(nullptr, &paint);
}

void DisplayListPlayerSkia::apply_transform(ApplyTransform const& command)
{
    auto affine_transform = Gfx::extract_2d_affine_transform(command.matrix);
    auto new_transform = Gfx::AffineTransform {}
                             .translate(command.origin)
                             .multiply(affine_transform)
                             .translate(-command.origin);
    auto matrix = to_skia_matrix(new_transform);
    surface().canvas().concat(matrix);
}

void DisplayListPlayerSkia::apply_mask_bitmap(ApplyMaskBitmap const& command)
{
    auto& canvas = surface().canvas();

    auto const* mask_image = command.bitmap->sk_image();

    char const* sksl_shader = nullptr;
    if (command.kind == Gfx::Bitmap::MaskKind::Luminance) {
        sksl_shader = R"(
                uniform shader mask_image;
                half4 main(float2 coord) {
                    half4 color = mask_image.eval(coord);
                    half luminance = 0.2126 * color.b + 0.7152 * color.g + 0.0722 * color.r;
                    return half4(0.0, 0.0, 0.0, color.a * luminance);
                }
            )";
    } else if (command.kind == Gfx::Bitmap::MaskKind::Alpha) {
        sksl_shader = R"(
                uniform shader mask_image;
                half4 main(float2 coord) {
                    half4 color = mask_image.eval(coord);
                    return half4(0.0, 0.0, 0.0, color.a);
                }
            )";
    } else {
        VERIFY_NOT_REACHED();
    }

    auto [effect, error] = SkRuntimeEffect::MakeForShader(SkString(sksl_shader));
    if (!effect) {
        dbgln("SkSL error: {}", error.c_str());
        VERIFY_NOT_REACHED();
    }

    SkMatrix mask_matrix;
    auto mask_position = command.origin;
    mask_matrix.setTranslate(mask_position.x(), mask_position.y());

    SkRuntimeShaderBuilder builder(effect);
    builder.child("mask_image") = mask_image->makeShader(SkSamplingOptions(), mask_matrix);
    canvas.clipShader(builder.makeShader());
}

bool DisplayListPlayerSkia::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    return surface().canvas().quickReject(to_skia_rect(rect));
}

}
