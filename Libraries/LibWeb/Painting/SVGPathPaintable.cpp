/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Quad.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::Painting {

NonnullRefPtr<SVGPathPaintable> SVGPathPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGPathPaintable(layout_box));
}

SVGPathPaintable::SVGPathPaintable(Layout::Box const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

Optional<CSSPixelRect> SVGPathPaintable::clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const
{
    if (!contributes_to_clip_path() || !computed_path().has_value())
        return {};

    auto path = computed_path()->copy_transformed(additional_transform);
    return path.bounding_box().to_type<CSSPixels>();
}

static Gfx::WindingRule to_gfx_winding_rule(SVG::FillRule fill_rule)
{
    switch (fill_rule) {
    case SVG::FillRule::Nonzero:
        return Gfx::WindingRule::Nonzero;
    case SVG::FillRule::Evenodd:
        return Gfx::WindingRule::EvenOdd;
    default:
        VERIFY_NOT_REACHED();
    }
}

void SVGPathPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!computed_path().has_value())
        return;

    if (context.draw_svg_geometry_for_clip_path()) {
        if (!contributes_to_clip_path())
            return;
    } else if (!is_visible()) {
        return;
    }

    SVGGraphicsPaintable::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto& graphics_element = dom_node();

    // Content below the viewport transform node records in user units scaled by the device pixel
    // ratio; the visual context tree applies the viewport and element transforms at replay.
    auto device_scale = static_cast<float>(context.device_pixels_per_css_pixel());
    auto paint_transform = Gfx::AffineTransform {}.scale(device_scale, device_scale);
    auto path = computed_path()->copy_transformed(paint_transform);

    if (context.draw_svg_geometry_for_clip_path()) {
        // https://drafts.fxtf.org/css-masking/#ClipPathElement:
        // The raw geometry of each child element exclusive of rendering properties such as fill, stroke, stroke-width
        // within a clipPath conceptually defines a 1-bit mask (with the possible exception of anti-aliasing along
        // the edge of the geometry) which represents the silhouette of the graphics associated with that element.
        context.display_list_recorder().fill_path({
            .path = path,
            .paint_style_or_color = Gfx::Color(Color::Black),
            .winding_rule = to_gfx_winding_rule(graphics_element.clip_rule().value_or(SVG::ClipRule::Nonzero)),
            .should_anti_alias = should_anti_alias(),
        });
        return;
    }

    Gfx::FloatRect viewport_rect {};
    if (auto const* viewport_paintable = nearest_svg_viewport_paintable_of(layout_node()))
        viewport_rect = svg_viewport_user_rect(*viewport_paintable);
    SVG::SVGPaintContext paint_context {
        .viewport = viewport_rect,
        .path_bounding_box = computed_path()->bounding_box(),
        .paint_transform = paint_transform,
    };

    auto paint_fill = [&] {
        auto fill_opacity = graphics_element.fill_opacity().value_or(1);
        auto winding_rule = to_gfx_winding_rule(graphics_element.fill_rule().value_or(SVG::FillRule::Nonzero));
        if (auto paint_style = graphics_element.fill_paint_style(paint_context, &context); paint_style.has_value()) {
            context.display_list_recorder().fill_path({
                .path = path,
                .opacity = fill_opacity,
                .paint_style_or_color = *paint_style,
                .winding_rule = winding_rule,
                .should_anti_alias = should_anti_alias(),
            });
        } else if (auto fill_color = graphics_element.fill_color(); fill_color.has_value()) {
            context.display_list_recorder().fill_path({
                .path = path,
                .paint_style_or_color = fill_color->with_opacity(fill_opacity),
                .winding_rule = winding_rule,
                .should_anti_alias = should_anti_alias(),
            });
        }
    };

    auto paint_stroke = [&] {
        Gfx::Path::CapStyle cap_style;
        switch (graphics_element.stroke_linecap().value_or(CSS::InitialValues::stroke_linecap())) {
        case CSS::StrokeLinecap::Butt:
            cap_style = Gfx::Path::CapStyle::Butt;
            break;
        case CSS::StrokeLinecap::Round:
            cap_style = Gfx::Path::CapStyle::Round;
            break;
        case CSS::StrokeLinecap::Square:
            cap_style = Gfx::Path::CapStyle::Square;
            break;
        }

        Gfx::Path::JoinStyle join_style;
        switch (graphics_element.stroke_linejoin().value_or(CSS::InitialValues::stroke_linejoin())) {
        case CSS::StrokeLinejoin::Miter:
            join_style = Gfx::Path::JoinStyle::Miter;
            break;
        case CSS::StrokeLinejoin::Round:
            join_style = Gfx::Path::JoinStyle::Round;
            break;
        case CSS::StrokeLinejoin::Bevel:
            join_style = Gfx::Path::JoinStyle::Bevel;
            break;
        }

        auto miter_limit = graphics_element.stroke_miterlimit().value_or(0);
        auto stroke_opacity = graphics_element.stroke_opacity().value_or(1);

        // https://svgwg.org/svg2-draft/painting.html#PaintingVectorEffects
        // With the non-scaling-stroke vector effect, stroke outline shall be calculated in the "host" coordinate space instead of user coordinate system.
        // NB: A scalar cannot compensate a non-uniform accumulated scale exactly; the geometric
        //     mean spreads the residual error over both axes.
        auto stroke_scale = device_scale;
        if (layout_node().vector_effect() == CSS::VectorEffect::NonScalingStroke) {
            auto accumulated_scale = context.display_list_recorder().visual_context_tree().accumulated_2d_scale(
                context.accumulated_visual_context_index_of(*this), ScrollStateSnapshot {}, AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);
            auto accumulated_scale_area = accumulated_scale.width() * accumulated_scale.height();
            if (accumulated_scale_area > 0)
                stroke_scale = device_scale / sqrtf(accumulated_scale_area);
        }
        float stroke_thickness = graphics_element.stroke_width().value_or(1) * stroke_scale;
        auto stroke_dasharray = graphics_element.stroke_dasharray();
        for (auto& value : stroke_dasharray)
            value *= stroke_scale;
        float stroke_dashoffset = graphics_element.stroke_dashoffset().value_or(0) * stroke_scale;

        if (auto paint_style = graphics_element.stroke_paint_style(paint_context, &context); paint_style.has_value()) {
            context.display_list_recorder().stroke_path({
                .cap_style = cap_style,
                .join_style = join_style,
                .miter_limit = static_cast<float>(miter_limit),
                .dash_array = stroke_dasharray,
                .dash_offset = stroke_dashoffset,
                .path = path,
                .opacity = stroke_opacity,
                .paint_style_or_color = *paint_style,
                .thickness = stroke_thickness,
                .should_anti_alias = should_anti_alias(),
            });
        } else if (auto stroke_color = graphics_element.stroke_color(); stroke_color.has_value()) {
            context.display_list_recorder().stroke_path({
                .cap_style = cap_style,
                .join_style = join_style,
                .miter_limit = static_cast<float>(miter_limit),
                .dash_array = stroke_dasharray,
                .dash_offset = stroke_dashoffset,
                .path = path,
                .paint_style_or_color = stroke_color->with_opacity(stroke_opacity),
                .thickness = stroke_thickness,
                .should_anti_alias = should_anti_alias(),
            });
        }
    };

    for (auto paint_order : graphics_element.paint_order()) {
        switch (paint_order) {
        case CSS::PaintOrder::Fill:
            paint_fill();
            break;
        case CSS::PaintOrder::Stroke:
            paint_stroke();
            break;
        case CSS::PaintOrder::Markers:
            // FIXME: Implement marker painting
            break;
        }
    }
}

void SVGPathPaintable::record_hit_test_items(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (phase != PaintPhase::Foreground)
        return;

    auto* hit_test_display_list = context.hit_test_display_list();
    if (!hit_test_display_list)
        return;

    if (!computed_path().has_value())
        return;

    if (layout_node().visibility() != CSS::Visibility::Visible || !visible_for_hit_testing())
        return;

    auto& graphics_element = dom_node();

    // FIXME: Hit test the stroked region of paths without a fill, rather than treating them as non-hittable.
    if (!graphics_element.fill_color().has_value())
        return;

    // The path records raw in user units; hit-test points inverse-map through the visual context
    // chain into the same space. The item rect rounds outward so fixed-point quantization can
    // never reject a valid hit.
    auto path = *computed_path();
    auto bounding_box = Gfx::enclosing_int_rect(path.bounding_box()).to_type<CSSPixels>();
    if (bounding_box.is_empty())
        return;

    auto winding_rule = to_gfx_winding_rule(graphics_element.fill_rule().value_or(SVG::FillRule::Nonzero));
    hit_test_display_list->append_svg_path(const_cast<SVGPathPaintable&>(*this), move(path), winding_rule, bounding_box, accumulated_visual_context_index());
}

}
