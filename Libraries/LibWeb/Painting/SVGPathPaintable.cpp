/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Quad.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGUseElement.h>
#include <LibWeb/SVG/TagNames.h>

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

void SVGPathPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible() || !computed_path().has_value())
        return;

    SVGGraphicsPaintable::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto& graphics_element = layout_box().dom_node();

    auto const* svg_node = layout_box().first_ancestor_of_type<Layout::SVGSVGBox>();
    auto svg_element_rect = svg_node->paintable_box()->absolute_rect();

    auto offset = context.rounded_device_point(svg_element_rect.location()).to_type<int>().to_type<float>();
    auto maybe_view_box = svg_node->dom_node().view_box();

    auto paint_transform = computed_transforms().svg_to_device_pixels_transform(context);
    auto path = computed_path()->copy_transformed(paint_transform);
    path.offset(offset);

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
            .paint_style_or_color = Gfx::Color(Color::Black),
            .winding_rule = to_gfx_winding_rule(graphics_element.clip_rule().value_or(SVG::ClipRule::Nonzero)),
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
            .opacity = fill_opacity,
            .paint_style_or_color = *paint_style,
            .winding_rule = winding_rule,
        });
    } else {
        // MVP pattern fill handling: url(#pattern...) with a single <image> child (or <use>-><image>)
        auto const& fill = layout_box().computed_values().fill();
        if (fill.has_value() && fill->is_url()) {
            auto const& url = fill->as_url();
            Optional<FlyString> fragment;
            if (auto fragment_offset = url.url().find_byte_offset('#'); fragment_offset.has_value())
                fragment = MUST(url.url().substring_from_byte_offset_with_shared_superstring(fragment_offset.value() + 1));
            if (fragment.has_value()) {
                if (auto target = graphics_element.document().get_element_by_id(*fragment); target && target->local_name().equals_ignoring_ascii_case(SVG::TagNames::pattern)) {
                    dbgln("[SVGPathPaintable] FILL pattern detected id='{}'", *fragment);
                    // MVP: Support userSpaceOnUse only and a single <image> content (or <use>-><image>)
                    auto resolve_use_to_image = [&](DOM::Element& elem) -> SVG::SVGImageElement* {
                        if (is<SVG::SVGImageElement>(elem))
                            return &static_cast<SVG::SVGImageElement&>(elem);
                        if (is<SVG::SVGUseElement>(elem)) {
                            auto* use = static_cast<SVG::SVGUseElement*>(&elem);
                            if (auto instance = use->instance_root(); instance && is<SVG::SVGImageElement>(*instance))
                                return &static_cast<SVG::SVGImageElement&>(*instance);
                        }
                        return nullptr;
                    };
                    SVG::SVGImageElement* image_elem = nullptr;
                    for (auto* child = target->template first_child_of_type<DOM::Element>(); child; child = child->next_element_sibling()) {
                        if ((image_elem = resolve_use_to_image(*child)))
                            break;
                    }

                    if (!image_elem) {
                        dbgln("[SVGPathPaintable] pattern MVP: unsupported content (no <image>) -> fallback");
                    } else {
                        // Parse minimal attributes from pattern: x,y,width,height, patternUnits/contentUnits, patternTransform (translate/scale only)
                        auto x_attr = target->get_attribute_value(SVG::AttributeNames::x);
                        auto y_attr = target->get_attribute_value(SVG::AttributeNames::y);
                        auto w_attr = target->get_attribute_value(SVG::AttributeNames::width);
                        auto h_attr = target->get_attribute_value(SVG::AttributeNames::height);
                        auto units_attr = target->get_attribute_value(SVG::AttributeNames::patternUnits);
                        auto content_units_attr = target->get_attribute_value(SVG::AttributeNames::patternContentUnits);
                        auto transform_attr = target->get_attribute_value(SVG::AttributeNames::patternTransform);

                        auto x = SVG::AttributeParser::parse_coordinate(x_attr).value_or(0);
                        auto y = SVG::AttributeParser::parse_coordinate(y_attr).value_or(0);
                        auto width = SVG::AttributeParser::parse_positive_length(w_attr).value_or(0);
                        auto height = SVG::AttributeParser::parse_positive_length(h_attr).value_or(0);

                        // Spec default: patternUnits defaults to objectBoundingBox when omitted.
                        auto units = SVG::AttributeParser::parse_units(units_attr).value_or(SVG::SVGUnits::ObjectBoundingBox);
                        auto content_units = SVG::AttributeParser::parse_units(content_units_attr).value_or(SVG::SVGUnits::UserSpaceOnUse);

                        if (width <= 0 || height <= 0) {
                            dbgln("[SVGPathPaintable] pattern MVP: non-positive size -> fallback");
                        } else {
                            // Compute tile rect in user space, supporting patternUnits=userSpaceOnUse|objectBoundingBox
                            auto bbox_user = paint_context.path_bounding_box;
                            Gfx::FloatRect tile_rect_user {};
                            if (units == SVG::SVGUnits::UserSpaceOnUse) {
                                tile_rect_user = { x, y, width, height };
                            } else if (units == SVG::SVGUnits::ObjectBoundingBox) {
                                tile_rect_user = { bbox_user.x() + x * bbox_user.width(), bbox_user.y() + y * bbox_user.height(), width * bbox_user.width(), height * bbox_user.height() };
                            } else {
                                dbgln("[SVGPathPaintable] pattern MVP: unsupported patternUnits -> fallback");
                                goto no_fill;
                            }

                            // patternContentUnits: support userSpaceOnUse and objectBoundingBox identically for image content by scaling to tile rect.
                            if (!(content_units == SVG::SVGUnits::UserSpaceOnUse || content_units == SVG::SVGUnits::ObjectBoundingBox)) {
                                dbgln("[SVGPathPaintable] pattern MVP: unsupported patternContentUnits -> fallback");
                                goto no_fill;
                            }

                            // Handle a very small subset of patternTransform: translate and uniform scale.
                            float pt_tx = 0.0f;
                            float pt_ty = 0.0f;
                            float pt_sx = 1.0f;
                            float pt_sy = 1.0f;
                            bool unsupported_transform = false;
                            if (!transform_attr.is_empty()) {
                                if (auto transform_list = SVG::AttributeParser::parse_transform(transform_attr); transform_list.has_value()) {
                                    for (auto const& t : *transform_list) {
                                        t.operation.visit(
                                            [&](SVG::Transform::Translate const& tr) {
                                                pt_tx += tr.x;
                                                pt_ty += tr.y;
                                            },
                                            [&](SVG::Transform::Scale const& sc) {
                                                pt_sx *= sc.x;
                                                pt_sy *= sc.y;
                                            },
                                            [&](auto const&) {
                                                unsupported_transform = true;
                                            });
                                        if (unsupported_transform)
                                            break;
                                    }
                                }
                            }
                            if (unsupported_transform) {
                                dbgln("[SVGPathPaintable] pattern MVP: unsupported patternTransform -> fallback");
                                goto no_fill;
                            }

                            // Apply translate to tile rect in user space, and scale to tile size.
                            tile_rect_user.set_x(tile_rect_user.x() + pt_tx);
                            tile_rect_user.set_y(tile_rect_user.y() + pt_ty);
                            tile_rect_user.set_size({ tile_rect_user.width() * pt_sx, tile_rect_user.height() * pt_sy });

                            // Map tile rect into device pixels using the SVG paint transform and layout offset.
                            auto device_tile_rect_f = paint_transform.map(tile_rect_user);
                            device_tile_rect_f.translate_by(offset.x(), offset.y());
                            auto device_tile_rect = enclosing_int_rect(device_tile_rect_f);

                            if (device_tile_rect.is_empty()) {
                                dbgln("[SVGPathPaintable] pattern MVP: device tile rect is empty -> fallback");
                                goto no_fill;
                            }

                            // Acquire bitmap from SVGImageElement, scaled to the device tile size.
                            auto requested_size = device_tile_rect.size();
                            auto tile_bitmap = image_elem->current_image_bitmap(requested_size);
                            if (tile_bitmap) {
                                auto clip_rect = enclosing_int_rect(path.bounding_box());
                                DisplayListRecorder::PushStackingContextParams params {
                                    .opacity = fill_opacity,
                                    .compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::Normal,
                                    .isolate = false,
                                    .transform = { .origin = {}, .matrix = Gfx::FloatMatrix4x4::identity() },
                                    .clip_path = closed_path(),
                                };
                                context.display_list_recorder().push_stacking_context(params);
                                context.display_list_recorder().draw_repeated_immutable_bitmap(device_tile_rect, clip_rect, tile_bitmap.release_nonnull(), Gfx::ScalingMode::BilinearBlend, true, true);
                                context.display_list_recorder().pop_stacking_context();
                                goto after_fill;
                            } else {
                                dbgln("[SVGPathPaintable] pattern MVP: image bitmap unavailable -> fallback");
                            }
                        }
                    no_fill:;
                    }
                }
            }
        }
        // Pattern not handled; fall back to color if available
        if (auto fill_color = graphics_element.fill_color(); fill_color.has_value()) {
            dbgln("[SVGPathPaintable] using COLOR for FILL {}", fill_color->to_string());
            context.display_list_recorder().fill_path({
                .path = closed_path(),
                .paint_style_or_color = fill_color->with_opacity(fill_opacity),
                .winding_rule = winding_rule,
            });
        } else {
            dbgln("[SVGPathPaintable] no FILL applied");
        }
    }

after_fill:

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
            .opacity = stroke_opacity,
            .paint_style_or_color = *paint_style,
            .thickness = stroke_thickness,
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
        });
    }
}

}
