/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGGraphicsElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

SVGGraphicsElement::SVGGraphicsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

Optional<Painting::PaintStyle> SVGGraphicsElement::svg_paint_computed_value_to_gfx_paint_style(SVGPaintContext const& paint_context, Optional<CSS::SVGPaint> const& paint_value, DisplayListRecordingContext* recording_context) const
{
    // FIXME: This entire function is an ad-hoc hack:
    if (!paint_value.has_value() || !paint_value->is_url())
        return {};
    if (auto gradient = try_resolve_url_to<SVG::SVGGradientElement const>(paint_value->as_url())) {
        return gradient->to_gfx_paint_style(paint_context);
    }
    if (auto pattern = try_resolve_url_to<SVG::SVGPatternElement const>(paint_value->as_url())) {
        if (recording_context && layout_node())
            return pattern->to_gfx_paint_style(paint_context, *recording_context, *layout_node());
    }
    return {};
}

// NB: SVG property accessors below are called during painting.
Optional<Painting::PaintStyle> SVGGraphicsElement::fill_paint_style(SVGPaintContext const& paint_context, DisplayListRecordingContext* recording_context) const
{
    if (!unsafe_layout_node())
        return {};
    return svg_paint_computed_value_to_gfx_paint_style(paint_context, unsafe_layout_node()->fill(), recording_context);
}

Optional<Painting::PaintStyle> SVGGraphicsElement::stroke_paint_style(SVGPaintContext const& paint_context, DisplayListRecordingContext* recording_context) const
{
    if (!unsafe_layout_node())
        return {};
    return svg_paint_computed_value_to_gfx_paint_style(paint_context, unsafe_layout_node()->stroke(), recording_context);
}

GC::Ptr<DOM::Element> SVGGraphicsElement::resolve_url_to_element(CSS::URL const& url) const
{
    // FIXME: Complete and use the entire URL, not just the fragment.
    if (auto fragment_offset = url.url().find_byte_offset('#'); fragment_offset.has_value()) {
        auto fragment_string = MUST(url.url().substring_from_byte_offset_with_shared_superstring(fragment_offset.value() + 1));
        return resolve_fragment_identifier_to_element(decode_fragment_identifier(fragment_string));
    }

    return {};
}

GC::Ptr<DOM::Element> SVGGraphicsElement::resolve_url_to_element(Utf16String const& url_string) const
{
    auto url = document().encoding_parse_url(url_string);
    if (!url.has_value() || !url->fragment().has_value())
        return {};
    return resolve_fragment_identifier_to_element(decode_fragment_identifier(*url->fragment()));
}

GC::Ptr<DOM::Element> SVGGraphicsElement::resolve_fragment_identifier_to_element(Utf16String const& fragment) const
{
    if (auto element = document().get_element_by_id(fragment))
        return element;

    auto containing_shadow = containing_shadow_root();
    if (containing_shadow) {
        if (auto element = containing_shadow->get_element_by_id(fragment))
            return element;
    }

    return {};
}

GC::Ptr<SVG::SVGMaskElement const> SVGGraphicsElement::mask() const
{
    // NB: unsafe_layout_node() because this is called during painting to resolve SVG references.
    auto const& mask_reference = unsafe_layout_node()->mask();
    if (!mask_reference.has_value())
        return {};
    return try_resolve_url_to<SVG::SVGMaskElement const>(mask_reference->url());
}

GC::Ptr<SVG::SVGClipPathElement const> SVGGraphicsElement::clip_path() const
{
    // NB: unsafe_layout_node() because this is called during painting to resolve SVG references.
    auto const& clip_path_reference = unsafe_layout_node()->clip_path();
    if (!clip_path_reference.has_value() || !clip_path_reference->is_url())
        return {};
    return try_resolve_url_to<SVG::SVGClipPathElement const>(clip_path_reference->url());
}

GC::Ptr<SVG::SVGPatternElement const> SVGGraphicsElement::fill_pattern() const
{
    if (!unsafe_layout_node())
        return {};
    auto const& fill = unsafe_layout_node()->fill();
    if (!fill.has_value() || !fill->is_url())
        return {};
    return try_resolve_url_to<SVG::SVGPatternElement const>(fill->as_url());
}

GC::Ptr<SVG::SVGPatternElement const> SVGGraphicsElement::stroke_pattern() const
{
    if (!unsafe_layout_node())
        return {};
    auto const& stroke = unsafe_layout_node()->stroke();
    if (!stroke.has_value() || !stroke->is_url())
        return {};
    return try_resolve_url_to<SVG::SVGPatternElement const>(stroke->as_url());
}

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list)
{
    Gfx::AffineTransform affine_transform;
    for (auto& transform : transform_list) {
        transform.operation.visit(
            [&](Transform::Translate const& translate) {
                affine_transform.multiply(Gfx::AffineTransform {}.translate({ translate.x, translate.y }));
            },
            [&](Transform::Scale const& scale) {
                affine_transform.multiply(Gfx::AffineTransform {}.scale({ scale.x, scale.y }));
            },
            [&](Transform::Rotate const& rotate) {
                Gfx::AffineTransform translate_transform;
                affine_transform.multiply(
                    Gfx::AffineTransform {}
                        .translate({ rotate.x, rotate.y })
                        .rotate_radians(AK::to_radians(rotate.a))
                        .translate({ -rotate.x, -rotate.y }));
            },
            [&](Transform::SkewX const& skew_x) {
                affine_transform.multiply(Gfx::AffineTransform {}.skew_radians(AK::to_radians(skew_x.a), 0));
            },
            [&](Transform::SkewY const& skew_y) {
                affine_transform.multiply(Gfx::AffineTransform {}.skew_radians(0, AK::to_radians(skew_y.a)));
            },
            [&](Transform::Matrix const& matrix) {
                affine_transform.multiply(Gfx::AffineTransform {
                    matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f });
            });
    }
    return affine_transform;
}

static FillRule to_svg_fill_rule(CSS::FillRule fill_rule)
{
    switch (fill_rule) {
    case CSS::FillRule::Nonzero:
        return FillRule::Nonzero;
    case CSS::FillRule::Evenodd:
        return FillRule::Evenodd;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<FillRule> SVGGraphicsElement::fill_rule() const
{
    if (!unsafe_layout_node())
        return {};
    return to_svg_fill_rule(unsafe_layout_node()->fill_rule());
}

Optional<ClipRule> SVGGraphicsElement::clip_rule() const
{
    if (!unsafe_layout_node())
        return {};
    return to_svg_fill_rule(unsafe_layout_node()->clip_rule());
}

Optional<Gfx::Color> SVGGraphicsElement::fill_color() const
{
    if (!unsafe_layout_node())
        return {};

    auto paint = unsafe_layout_node()->fill();
    if (!paint.has_value())
        return {};

    if (paint->is_url())
        return paint->fallback_color();

    return paint->as_color();
}

Optional<Gfx::Color> SVGGraphicsElement::stroke_color() const
{
    if (!unsafe_layout_node())
        return {};

    auto paint = unsafe_layout_node()->stroke();
    if (!paint.has_value())
        return {};

    if (paint->is_url())
        return paint->fallback_color();

    return paint->as_color();
}

Optional<float> SVGGraphicsElement::fill_opacity() const
{
    if (!unsafe_layout_node())
        return {};
    return unsafe_layout_node()->fill_opacity();
}

CSS::PaintOrderList SVGGraphicsElement::paint_order() const
{
    if (!unsafe_layout_node())
        return CSS::InitialValues::paint_order();
    return unsafe_layout_node()->paint_order();
}

Optional<CSS::StrokeLinecap> SVGGraphicsElement::stroke_linecap() const
{
    if (!unsafe_layout_node())
        return {};
    return unsafe_layout_node()->stroke_linecap();
}

Optional<CSS::StrokeLinejoin> SVGGraphicsElement::stroke_linejoin() const
{
    if (!unsafe_layout_node())
        return {};
    return unsafe_layout_node()->stroke_linejoin();
}

Optional<double> SVGGraphicsElement::stroke_miterlimit() const
{
    if (!unsafe_layout_node())
        return {};
    return unsafe_layout_node()->stroke_miterlimit();
}

Optional<float> SVGGraphicsElement::stroke_opacity() const
{
    if (!unsafe_layout_node())
        return {};
    return unsafe_layout_node()->stroke_opacity();
}

float SVGGraphicsElement::resolve_relative_to_viewport_size(CSS::LengthPercentage const& length_percentage) const
{
    // Resolved relative to the "Scaled viewport size": https://www.w3.org/TR/2017/WD-fill-stroke-3-20170413/#scaled-viewport-size
    // FIXME: The spec formula is the normalized diagonal sqrt((width² + height²) / 2); this keeps
    //        the historical (width + height) / 2 approximation.
    // NB: Resolution happens during layout, so only the viewBox and computed style are available,
    //     not committed viewport geometry.
    CSSPixels viewport_width = 0;
    CSSPixels viewport_height = 0;
    auto resolve_viewport_size_from = [&](SVGElement const& viewport_element, Optional<ViewBox> const& view_box) {
        if (view_box.has_value()) {
            viewport_width = CSSPixels::nearest_value_for(view_box->width);
            viewport_height = CSSPixels::nearest_value_for(view_box->height);
        } else if (auto viewport_layout_node = viewport_element.unsafe_layout_node()) {
            viewport_width = viewport_layout_node->width().to_px(0);
            viewport_height = viewport_layout_node->height().to_px(0);
        }
    };
    // <symbol> instances establish nested viewports; percentages inside one resolve against it,
    // not the enclosing <svg>.
    for (auto* ancestor = first_flat_tree_ancestor_of_type<SVGElement>(); ancestor; ancestor = ancestor->first_flat_tree_ancestor_of_type<SVGElement>()) {
        if (auto* svg_svg_element = as_if<SVGSVGElement>(*ancestor)) {
            resolve_viewport_size_from(*svg_svg_element, svg_svg_element->active_view_box());
            break;
        }
        if (auto* symbol_element = as_if<SVGSymbolElement>(*ancestor)) {
            resolve_viewport_size_from(*symbol_element, symbol_element->view_box());
            break;
        }
    }
    auto scaled_viewport_size = (viewport_width + viewport_height) * CSSPixels(0.5);
    return length_percentage.to_px(scaled_viewport_size).to_double();
}

Vector<float> SVGGraphicsElement::stroke_dasharray() const
{
    if (!unsafe_layout_node())
        return {};

    Vector<float> dasharray;
    for (auto const& value : unsafe_layout_node()->stroke_dasharray()) {
        value.visit(
            [&](CSS::LengthPercentage const& length_percentage) {
                dasharray.append(resolve_relative_to_viewport_size(length_percentage));
            },
            [&](float number) {
                dasharray.append(number);
            });
    }

    // https://svgwg.org/svg2-draft/painting.html#StrokeDashing
    // If the list has an odd number of values, then it is repeated to yield an even number of values.
    if (dasharray.size() % 2 == 1)
        dasharray.extend(dasharray);

    // If any value in the list is negative, the <dasharray> value is invalid. If all of the values in the list are zero, then the stroke is rendered as a solid line without any dashing.
    bool all_zero = true;
    for (auto& value : dasharray) {
        if (value < 0)
            return {};
        if (value != 0)
            all_zero = false;
    }
    if (all_zero)
        return {};

    return dasharray;
}

Optional<float> SVGGraphicsElement::stroke_dashoffset() const
{
    if (!unsafe_layout_node())
        return {};
    return resolve_relative_to_viewport_size(unsafe_layout_node()->stroke_dashoffset());
}

Optional<float> SVGGraphicsElement::stroke_width() const
{
    if (!unsafe_layout_node())
        return {};
    return resolve_relative_to_viewport_size(unsafe_layout_node()->stroke_width());
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGraphicsElement__getBBox
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> SVGGraphicsElement::get_b_box(Bindings::SVGBoundingBoxOptions const&)
{
    // The getBBox method is used to compute the bounding box of the current element.  When the getBBox(options) method
    // is called, the bounding box algorithm is invoked for the current element, with fill, stroke, markers and clipped
    // members of the options dictionary argument used to control which parts of the element are included in the
    // bounding box, using the element's user coordinate system as the coordinate system to return the bounding box in.
    // A newly created DOMRect object that defines the computed bounding box is returned.
    // If the element's geometric attributes are missing or have invalid values (e.g. negative width or height), such
    // that the element is not rendered, then a DOMRect with x, y, width and height all set to 0 is returned.

    // FIXME: It should be possible to compute this without layout updates. The bounding box is within the
    //        SVG coordinate space (before any viewbox or other transformations), so it should be possible to
    //        calculate this from SVG geometry without a full layout tree (at least for simple cases).
    //        See: https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
    document().update_layout_if_needed_for_node(*this, DOM::UpdateLayoutReason::SVGGraphicsElementGetBBox);
    if (!layout_node())
        return Geometry::DOMRect::create();
    auto owner_svg_element = this->owner_svg_element();

    // The outermost svg element has no ancestor svg element to measure against; per the bounding box
    // algorithm its bounding box is the union of its children's bounding boxes in its own user
    // coordinate system, with each child's own transforms kept.
    if (!owner_svg_element) {
        if (!is<SVGSVGElement>(*this))
            return Geometry::DOMRect::create();
        auto self_paintable = paintable_box();
        if (!self_paintable)
            return Geometry::DOMRect::create();
        Gfx::FloatRect united_rect;
        self_paintable->for_each_child([&](Painting::Paintable const& child) {
            auto child_rect = child.layout_node().used_svg_element_transform().map(child.absolute_rect().to_type<float>());
            united_rect.unite(child_rect);
            return IterationDecision::Continue;
        });
        if (united_rect.is_empty())
            return Geometry::DOMRect::create();
        return Geometry::DOMRect::create(united_rect);
    }

    auto self_paintable = paintable_box();
    if (!owner_svg_element->paintable_box() || !self_paintable) {
        // Throw only for non-rendered *graphics* elements where geometry isn't computable
        // (e.g. elements inside <marker>, <pattern>, etc.).
        if (is<SVGSVGElement>(*this))
            return Geometry::DOMRect::create();
        return WebIDL::InvalidStateError::create(
            HTML::relevant_realm(*this),
            "Element is not rendered and geometry is not computable"_utf16);
    }

    // A path-like element's bounding box covers its geometry alone; the committed content rect is
    // inflated by the visible stroke width, so take the unstroked path bounds directly.
    if (auto const* path_paintable = as_if<Painting::SVGPathPaintable>(*self_paintable); path_paintable && path_paintable->computed_path().has_value())
        return Geometry::DOMRect::create(path_paintable->computed_path()->bounding_box());

    auto rect = self_paintable->absolute_rect().to_type<float>();
    // An element with a non-positive geometry dimension is not rendered and
    // therefore contributes an empty bounding box, regardless of its
    // positioning rectangle's origin.
    if (rect.width() <= 0 || rect.height() <= 0)
        return Geometry::DOMRect::create();
    return Geometry::DOMRect::create(rect);
}

GC::Ref<SVGAnimatedTransformList> SVGGraphicsElement::transform() const
{
    dbgln("(STUBBED) SVGGraphicsElement::transform(). Called on: {}", debug_description());
    auto base_val = SVGTransformList::create(ReadOnlyList::Yes);
    auto anim_val = SVGTransformList::create(ReadOnlyList::Yes);
    return SVGAnimatedTransformList::create(base_val, anim_val);
}

GC::Ptr<Geometry::DOMMatrix> SVGGraphicsElement::get_screen_ctm()
{
    dbgln("(STUBBED) SVGGraphicsElement::get_screen_ctm(). Called on: {}", debug_description());
    return Geometry::DOMMatrix::create();
}

GC::Ptr<Geometry::DOMMatrix> SVGGraphicsElement::get_ctm()
{
    dbgln("(STUBBED) SVGGraphicsElement::get_ctm(). Called on: {}", debug_description());
    return Geometry::DOMMatrix::create();
}

}
