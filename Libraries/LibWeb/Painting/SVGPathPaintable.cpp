/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Quad.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
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

void SVGPathPaintable::reset_for_relayout()
{
    SVGGraphicsPaintable::reset_for_relayout();
    m_computed_path.clear();
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

void SVGPathPaintable::resolve_paint_properties()
{
    Base::resolve_paint_properties();

    auto& graphics_element = dom_node();
    m_stroke_thickness = graphics_element.stroke_width().value_or(1);
    m_stroke_dasharray = graphics_element.stroke_dasharray();
    m_stroke_dashoffset = graphics_element.stroke_dashoffset().value_or(0);
}

void SVGPathPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible() || !computed_path().has_value())
        return;

    SVGGraphicsPaintable::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto& graphics_element = dom_node();

    auto const* svg_node = layout_box().first_ancestor_of_type<Layout::SVGSVGBox>();
    auto svg_element_rect = svg_node->paintable_box()->absolute_rect();

    auto offset = context.rounded_device_point(svg_element_rect.location()).to_type<int>().to_type<float>();
    auto maybe_view_box = svg_node->dom_node().view_box();

    auto paint_transform = computed_transforms().svg_to_device_pixels_transform(context);
    auto path = computed_path()->copy_transformed(paint_transform);
    path.offset(offset);

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
            .path = path,
            .paint_style_or_color = Gfx::Color(Color::Black),
            .winding_rule = to_gfx_winding_rule(graphics_element.clip_rule().value_or(SVG::ClipRule::Nonzero)),
            .should_anti_alias = should_anti_alias(),
        });
        return;
    }

    SVG::SVGPaintContext paint_context {
        .viewport = svg_viewport,
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

        // Note: This is assuming .x_scale() == .y_scale() (which it does currently).
        auto viewbox_scale = paint_transform.x_scale();
        float stroke_thickness = m_stroke_thickness * viewbox_scale;
        auto stroke_dasharray = m_stroke_dasharray;
        for (auto& value : stroke_dasharray)
            value *= viewbox_scale;
        float stroke_dashoffset = m_stroke_dashoffset * viewbox_scale;

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

}
