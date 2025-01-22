/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Quad.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(SVGPathPaintable);

GC::Ref<SVGPathPaintable> SVGPathPaintable::create(Layout::SVGGraphicsBox const& layout_box)
{
    return layout_box.heap().allocate<SVGPathPaintable>(layout_box);
}

SVGPathPaintable::SVGPathPaintable(Layout::SVGGraphicsBox const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

Layout::SVGGraphicsBox const& SVGPathPaintable::layout_box() const
{
    return static_cast<Layout::SVGGraphicsBox const&>(layout_node());
}

TraversalDecision SVGPathPaintable::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (!computed_path().has_value())
        return TraversalDecision::Continue;
    auto transformed_bounding_box = computed_transforms().svg_to_css_pixels_transform().map_to_quad(computed_path()->bounding_box());
    if (!transformed_bounding_box.contains(position.to_type<float>()))
        return TraversalDecision::Continue;
    return SVGGraphicsPaintable::hit_test(position, type, callback);
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

void SVGPathPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible() || !computed_path().has_value())
        return;

    SVGGraphicsPaintable::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto& graphics_element = layout_box().dom_node();

    auto const* svg_node = layout_box().first_ancestor_of_type<Layout::SVGSVGBox>();
    auto svg_element_rect = svg_node->paintable_box()->absolute_rect();

    DisplayListRecorderStateSaver save_painter { context.display_list_recorder() };

    auto offset = context.rounded_device_point(svg_element_rect.location()).to_type<int>().to_type<float>();
    auto maybe_view_box = svg_node->dom_node().view_box();

    auto paint_transform = computed_transforms().svg_to_device_pixels_transform(context);
    auto path = computed_path()->copy_transformed(paint_transform);

    // Fills are computed as though all subpaths are closed (https://svgwg.org/svg2-draft/painting.html#FillProperties)
    auto closed_path = [&] {
        // We need to fill the path before applying the stroke, however the filled
        // path must be closed, whereas the stroke path may not necessary be closed.
        // Copy the path and close it for filling, but use the previous path for stroke
        auto copy = path;
        copy.close_all_subpaths();
        return copy;
    };

    auto svg_viewport = [&] {
        if (maybe_view_box.has_value())
            return Gfx::FloatRect { maybe_view_box->min_x, maybe_view_box->min_y, maybe_view_box->width, maybe_view_box->height };
        return Gfx::FloatRect { {}, svg_element_rect.size().to_type<float>() };
    }();

    if (context.draw_svg_geometry_for_clip_path()) {
        // https://drafts.fxtf.org/css-masking/#ClipPathElement:
        // The raw geometry of each child element exclusive of rendering properties such as fill, stroke, stroke-width
        // within a clipPath conceptually defines a 1-bit mask (with the possible exception of anti-aliasing along
        // the edge of the geometry) which represents the silhouette of the graphics associated with that element.
        context.display_list_recorder().fill_path({
            .path = closed_path(),
            .color = Color::Black,
            .winding_rule = to_gfx_winding_rule(graphics_element.clip_rule().value_or(SVG::ClipRule::Nonzero)),
            .translation = offset,
        });
        return;
    }

    SVG::SVGPaintContext paint_context {
        .viewport = svg_viewport,
        .path_bounding_box = computed_path()->bounding_box(),
        .paint_transform = paint_transform,
    };

    auto fill_opacity = graphics_element.fill_opacity().value_or(1);
    auto winding_rule = to_gfx_winding_rule(graphics_element.fill_rule().value_or(SVG::FillRule::Nonzero));
    if (auto paint_style = graphics_element.fill_paint_style(paint_context); paint_style.has_value()) {
        context.display_list_recorder().fill_path({
            .path = closed_path(),
            .paint_style = *paint_style,
            .winding_rule = winding_rule,
            .opacity = fill_opacity,
            .translation = offset,
        });
    } else if (auto fill_color = graphics_element.fill_color(); fill_color.has_value()) {
        context.display_list_recorder().fill_path({
            .path = closed_path(),
            .color = fill_color->with_opacity(fill_opacity),
            .winding_rule = winding_rule,
            .translation = offset,
        });
    }

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

    CSS::CalculationResolutionContext calculation_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(layout_node()),
    };
    auto miter_limit = graphics_element.stroke_miterlimit().value_or(CSS::InitialValues::stroke_miterlimit()).resolved(calculation_context).value_or(0);

    auto stroke_opacity = graphics_element.stroke_opacity().value_or(1);

    // Note: This is assuming .x_scale() == .y_scale() (which it does currently).
    auto viewbox_scale = paint_transform.x_scale();
    float stroke_thickness = graphics_element.stroke_width().value_or(1) * viewbox_scale;
    auto stroke_dasharray = graphics_element.stroke_dasharray();
    for (auto& value : stroke_dasharray)
        value *= viewbox_scale;
    float stroke_dashoffset = graphics_element.stroke_dashoffset().value_or(0) * viewbox_scale;

    if (auto paint_style = graphics_element.stroke_paint_style(paint_context); paint_style.has_value()) {
        context.display_list_recorder().stroke_path({
            .cap_style = cap_style,
            .join_style = join_style,
            .miter_limit = static_cast<float>(miter_limit),
            .dash_array = stroke_dasharray,
            .dash_offset = stroke_dashoffset,
            .path = path,
            .paint_style = *paint_style,
            .thickness = stroke_thickness,
            .opacity = stroke_opacity,
            .translation = offset,
        });
    } else if (auto stroke_color = graphics_element.stroke_color(); stroke_color.has_value()) {
        context.display_list_recorder().stroke_path({
            .cap_style = cap_style,
            .join_style = join_style,
            .miter_limit = static_cast<float>(miter_limit),
            .dash_array = stroke_dasharray,
            .dash_offset = stroke_dashoffset,
            .path = path,
            .color = stroke_color->with_opacity(stroke_opacity),
            .thickness = stroke_thickness,
            .translation = offset,
        });
    }
}

}
