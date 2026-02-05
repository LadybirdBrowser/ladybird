/*
 * Copyright (c) 2020, Matthew Olsson <matthewcolsson@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGSVGElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGViewElement.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSVGElement);

SVGSVGElement::SVGSVGElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, qualified_name)
{
}

void SVGSVGElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGSVGElement);
    Base::initialize(realm);
    SVGFitToViewBox::initialize(realm);
}

void SVGSVGElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
    visitor.visit(m_active_view_element);
}

GC::Ptr<Layout::Node> SVGSVGElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGSVGBox>(document(), *this, move(style));
}

RefPtr<CSS::StyleValue const> SVGSVGElement::width_style_value_from_attribute() const
{
    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    auto width_attribute = attribute(SVG::AttributeNames::width);
    if (auto width_value = parse_css_value(parsing_context, width_attribute.value_or(String {}), CSS::PropertyID::Width)) {
        return width_value.release_nonnull();
    }
    if (width_attribute == "") {
        // If the `width` attribute is an empty string, it defaults to 100%.
        // This matches WebKit and Blink, but not Firefox. The spec is unclear.
        // FIXME: Figure out what to do here.
        return CSS::PercentageStyleValue::create(CSS::Percentage { 100 });
    }
    return nullptr;
}

RefPtr<CSS::StyleValue const> SVGSVGElement::height_style_value_from_attribute() const
{
    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    auto height_attribute = attribute(SVG::AttributeNames::height);
    if (auto height_value = parse_css_value(parsing_context, height_attribute.value_or(String {}), CSS::PropertyID::Height)) {
        return height_value.release_nonnull();
    }
    if (height_attribute == "") {
        // If the `height` attribute is an empty string, it defaults to 100%.
        // This matches WebKit and Blink, but not Firefox. The spec is unclear.
        // FIXME: Figure out what to do here.
        return CSS::PercentageStyleValue::create(CSS::Percentage { 100 });
    }
    return nullptr;
}

void SVGSVGElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);

    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::width) || name.equals_ignoring_ascii_case(SVG::AttributeNames::height))
        update_fallback_view_box_for_svg_as_image();
}

void SVGSVGElement::children_changed(ChildrenChangedMetadata const*)
{
    // FIXME: Add support for all types of SVG fragment identifier.
    //        See: https://svgwg.org/svg2-draft/linking.html#LinksIntoSVG
    if (auto url = document().url(); url.fragment().has_value()) {
        if (auto referenced_element = get_element_by_id(*url.fragment())) {
            if (auto* view_element = as_if<SVGViewElement>(*referenced_element)) {
                set_active_view_element(*view_element);
                return;
            }
        }
        set_active_view_element({});
    }
}

void SVGSVGElement::update_fallback_view_box_for_svg_as_image()
{
    // AD-HOC: This creates a fallback viewBox for SVGs used as images.
    //         If the <svg> element has width and height, but no viewBox,
    //         we fall back to a synthetic viewBox="0 0 width height".

    Optional<double> width;
    Optional<double> height;

    auto width_attribute = get_attribute_value(SVG::AttributeNames::width);
    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    if (auto width_value = parse_css_value(parsing_context, width_attribute, CSS::PropertyID::Width)) {
        if (width_value->is_length() && width_value->as_length().length().is_absolute())
            width = width_value->as_length().length().absolute_length_to_px().to_double();
    }

    auto height_attribute = get_attribute_value(SVG::AttributeNames::height);
    if (auto height_value = parse_css_value(parsing_context, height_attribute, CSS::PropertyID::Height)) {
        if (height_value->is_length() && height_value->as_length().length().is_absolute())
            height = height_value->as_length().length().absolute_length_to_px().to_double();
    }

    if (width.has_value() && width.value() > 0 && height.has_value() && height.value() > 0) {
        m_fallback_view_box_for_svg_as_image = ViewBox { 0, 0, width.value(), height.value() };
    } else {
        m_fallback_view_box_for_svg_as_image = {};
    }
}

void SVGSVGElement::set_fallback_view_box_for_svg_as_image(Optional<ViewBox> view_box)
{
    m_fallback_view_box_for_svg_as_image = view_box;
}

Optional<ViewBox> SVGSVGElement::active_view_box() const
{
    if (m_active_view_element && m_active_view_element->view_box().has_value())
        return m_active_view_element->view_box().value();

    if (auto view_box = SVGFitToViewBox::view_box(); view_box.has_value())
        return view_box;

    // NOTE: If the parent is a document, we're an <svg> element used as an image.
    if (parent() && parent()->is_document() && m_fallback_view_box_for_svg_as_image.has_value())
        return m_fallback_view_box_for_svg_as_image;

    return {};
}

GC::Ref<SVGAnimatedLength> SVGSVGElement::x() const
{
    return svg_animated_length_for_property(CSS::PropertyID::X);
}

GC::Ref<SVGAnimatedLength> SVGSVGElement::y() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Y);
}

GC::Ref<SVGAnimatedLength> SVGSVGElement::width() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Width);
}

GC::Ref<SVGAnimatedLength> SVGSVGElement::height() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Height);
}

float SVGSVGElement::current_scale() const
{
    dbgln("(STUBBED) SVGSVGElement::current_scale(). Called on: {}", debug_description());
    return 1.0f;
}

void SVGSVGElement::set_current_scale(float)
{
    dbgln("(STUBBED) SVGSVGElement::set_current_scale(). Called on: {}", debug_description());
}

GC::Ref<Geometry::DOMPointReadOnly> SVGSVGElement::current_translate() const
{
    dbgln("(STUBBED) SVGSVGElement::current_translate(). Called on: {}", debug_description());
    return Geometry::DOMPointReadOnly::create(realm());
}

GC::Ref<DOM::NodeList> SVGSVGElement::get_intersection_list(GC::Ref<Geometry::DOMRectReadOnly>, GC::Ptr<SVGElement>) const
{
    dbgln("(STUBBED) SVGSVGElement::get_intersection_list(). Called on: {}", debug_description());
    return DOM::StaticNodeList::create(realm(), {});
}

GC::Ref<DOM::NodeList> SVGSVGElement::get_enclosure_list(GC::Ref<Geometry::DOMRectReadOnly>, GC::Ptr<SVGElement>) const
{
    dbgln("(STUBBED) SVGSVGElement::get_enclosure_list(). Called on: {}", debug_description());
    return DOM::StaticNodeList::create(realm(), {});
}

bool SVGSVGElement::check_intersection(GC::Ref<SVGElement>, GC::Ref<Geometry::DOMRectReadOnly>) const
{
    dbgln("(STUBBED) SVGSVGElement::check_intersection(). Called on: {}", debug_description());
    return false;
}

bool SVGSVGElement::check_enclosure(GC::Ref<SVGElement>, GC::Ref<Geometry::DOMRectReadOnly>) const
{
    dbgln("(STUBBED) SVGSVGElement::check_enclosure(). Called on: {}", debug_description());
    return false;
}

void SVGSVGElement::deselect_all() const
{
    // This is equivalent to calling document.getSelection().removeAllRanges() on the document that this ‘svg’ element is in.
    if (auto selection = document().get_selection())
        selection->remove_all_ranges();
}

GC::Ref<SVGLength> SVGSVGElement::create_svg_length() const
{
    // A new, detached SVGLength object whose value is the unitless <number> 0.
    return SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0, SVGLength::ReadOnly::No);
}

GC::Ref<Geometry::DOMPoint> SVGSVGElement::create_svg_point() const
{
    // A new, detached DOMPoint object whose coordinates are all 0.
    return Geometry::DOMPoint::from_point(vm(), Geometry::DOMPointInit {});
}

GC::Ref<Geometry::DOMMatrix> SVGSVGElement::create_svg_matrix() const
{
    // A new, detached DOMMatrix object representing the identity matrix.
    return Geometry::DOMMatrix::create(realm());
}

GC::Ref<Geometry::DOMRect> SVGSVGElement::create_svg_rect() const
{
    // A new, DOMRect object whose x, y, width and height are all 0.
    return Geometry::DOMRect::construct_impl(realm(), 0, 0, 0, 0).release_value_but_fixme_should_propagate_errors();
}

GC::Ref<SVGTransform> SVGSVGElement::create_svg_transform() const
{
    return SVGTransform::create(realm());
}

CSS::SizeWithAspectRatio SVGSVGElement::negotiate_natural_metrics(SVG::SVGSVGElement const& svg_root)
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS

    CSS::SizeWithAspectRatio natural_metrics;

    // The intrinsic dimensions must also be determined from the width and height sizing properties.
    // If either width or height are not specified, the used value is the initial value 'auto'.
    // 'auto' and percentage lengths must not be used to determine an intrinsic width or intrinsic height.

    if (auto width = svg_root.width_style_value_from_attribute(); width && width->is_length() && width->as_length().length().is_absolute()) {
        natural_metrics.width = width->as_length().length().absolute_length_to_px();
    }

    if (auto height = svg_root.height_style_value_from_attribute(); height && height->is_length() && height->as_length().length().is_absolute()) {
        natural_metrics.height = height->as_length().length().absolute_length_to_px();
    }

    // The intrinsic aspect ratio must be calculated using the following algorithm. If the algorithm returns null, then there is no intrinsic aspect ratio.
    natural_metrics.aspect_ratio = [&]() -> Optional<CSSPixelFraction> {
        // 1. If the width and height sizing properties on the ‘svg’ element are both absolute values:
        if (natural_metrics.width.has_value() && natural_metrics.height.has_value()) {
            if (natural_metrics.width != 0 && natural_metrics.height != 0) {
                // 1. return width / height
                return *natural_metrics.width / *natural_metrics.height;
            }
            return {};
        }

        // 2. If an SVG View is active:
        if (auto active_view_element = svg_root.active_view_element(); active_view_element && active_view_element->view_box().has_value()) {
            // 1. let viewbox be the viewbox defined by the active SVG View
            auto view_box = active_view_element->view_box().value();

            // 2. return viewbox.width / viewbox.height
            if (view_box.width != 0 || view_box.height != 0)
                return view_box.width / view_box.height;

            return {};
        }

        // 3. If the ‘viewBox’ on the ‘svg’ element is correctly specified:
        if (svg_root.view_box().has_value()) {
            // 1. let viewbox be the viewbox defined by the ‘viewBox’ attribute on the ‘svg’ element
            auto const& viewbox = svg_root.view_box().value();

            // 2. return viewbox.width / viewbox.height
            auto viewbox_width = CSSPixels::nearest_value_for(viewbox.width);
            auto viewbox_height = CSSPixels::nearest_value_for(viewbox.height);
            if (viewbox_width != 0 && viewbox_height != 0)
                return viewbox_width / viewbox_height;

            return {};
        }

        // 4. return null
        return {};
    }();

    return natural_metrics;
}

}
