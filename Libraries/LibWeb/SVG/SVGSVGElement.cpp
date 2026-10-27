/*
 * Copyright (c) 2020, Matthew Olsson <matthewcolsson@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SMILTimeContainer.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGViewElement.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSVGElement);

SVGSVGElement::SVGSVGElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, qualified_name)
{
}

void SVGSVGElement::initialize_element()
{
    SVGFitToViewBox::initialize_fit_to_view_box(*this);
}

void SVGSVGElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
    visitor.visit(m_current_translate);
    visitor.visit(m_active_view_element);
    visitor.visit(m_time_container);
}

void SVGSVGElement::inserted()
{
    Base::inserted();
    create_time_container_if_outermost();
}

void SVGSVGElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);

    // A time container follows the document timeline of the document it was created for, so an adopted svg element
    // creates a new one once it is inserted.
    m_time_container = nullptr;
}

void SVGSVGElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
    create_time_container_if_outermost();
}

void SVGSVGElement::create_time_container_if_outermost()
{
    // The outermost svg element of an SVG document fragment defines its timeline, which it keeps when it is removed or
    // moved and inserted again. The animations inside it update after it, as it comes first in tree order.
    if (m_time_container || !is_connected() || owner_svg_element())
        return;
    m_time_container = SMILTimeContainer::create(document());
    document().begin_smil_time_container_once_loaded(*m_time_container);
}

CSS::ElementBoxKind SVGSVGElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgSvg;
}

Optional<CSS::Length> SVGSVGElement::width_attribute_length() const
{
    if (m_cached_width_attribute_length.has_value())
        return *m_cached_width_attribute_length;

    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    auto width_attribute = attribute(SVG::AttributeNames::width);

    // NB: If the `width` attribute is an empty string, WebKit and Blink treat it as 100% while Firefox does not (the
    //     spec is unclear); a percentage never contributes here either way, so nothing distinguishes it from any other
    //     non-<length> value.
    Optional<CSS::Length> result;
    if (auto width_value = parse_css_value(parsing_context, width_attribute.value_or({}), CSS::PropertyID::Width)) {
        if (width_value->is_length())
            result = width_value->as_length().length();
    }

    m_cached_width_attribute_length = result;
    return result;
}

Optional<CSS::Length> SVGSVGElement::height_attribute_length() const
{
    if (m_cached_height_attribute_length.has_value())
        return *m_cached_height_attribute_length;

    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    auto height_attribute = attribute(SVG::AttributeNames::height);

    // NB: See the `width` note above; an empty string `height` likewise never contributes a <length>.
    Optional<CSS::Length> result;
    if (auto height_value = parse_css_value(parsing_context, height_attribute.value_or({}), CSS::PropertyID::Height)) {
        if (height_value->is_length())
            result = height_value->as_length().length();
    }

    m_cached_height_attribute_length = result;
    return result;
}

void SVGSVGElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);

    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::width)) {
        m_cached_width_attribute_length = {};
        update_fallback_view_box_for_svg_as_image();
    }
    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::height)) {
        m_cached_height_attribute_length = {};
        update_fallback_view_box_for_svg_as_image();
    }
}

void SVGSVGElement::children_changed(ChildrenChangedMetadata const&)
{
    // FIXME: Add support for all types of SVG fragment identifier.
    //        See: https://svgwg.org/svg2-draft/linking.html#LinksIntoSVG
    if (auto url = document().url(); url.fragment().has_value()) {
        if (auto referenced_element = get_element_by_id(decode_fragment_identifier(*url.fragment()))) {
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

    auto resolution_context = CSS::Length::ResolutionContext::for_document(document());

    if (auto width_length = width_attribute_length(); width_length.has_value())
        width = width_length->to_px(resolution_context).to_double();

    if (auto height_length = height_attribute_length(); height_length.has_value())
        height = height_length->to_px(resolution_context).to_double();

    if (width.has_value() && width.value() > 0 && height.has_value() && height.value() > 0) {
        m_fallback_view_box_for_svg_as_image = ViewBox { 0, 0, width.value(), height.value() };
    } else {
        m_fallback_view_box_for_svg_as_image = {};
    }
}

// The active view element is the one input to the published attribute facts that no attribute of this element names,
// so changing it republishes them.
void SVGSVGElement::set_active_view_element(GC::Ptr<SVGViewElement> view_element)
{
    if (m_active_view_element == view_element)
        return;
    m_active_view_element = view_element;
    publish_svg_attribute_facts();
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

float SVGSVGElement::current_scale() const
{
    dbgln("(STUBBED) SVGSVGElement::current_scale(). Called on: {}", debug_description());
    return 1.0f;
}

void SVGSVGElement::set_current_scale(float)
{
    dbgln("(STUBBED) SVGSVGElement::set_current_scale(). Called on: {}", debug_description());
}

// https://svgwg.org/svg2-draft/struct.html#__svg__SVGSVGElement__currentTranslate
// On getting currentTranslate, the SVGSVGElement object's current translate point object is returned.
GC::Ref<Geometry::DOMPointReadOnly> SVGSVGElement::current_translate() const
{
    if (!m_current_translate)
        m_current_translate = Geometry::DOMPointReadOnly::create();
    return *m_current_translate;
}

GC::Ref<DOM::NodeList> SVGSVGElement::get_intersection_list(GC::Ref<Geometry::DOMRectReadOnly>, GC::Ptr<SVGElement>) const
{
    dbgln("(STUBBED) SVGSVGElement::get_intersection_list(). Called on: {}", debug_description());
    return DOM::StaticNodeList::create(Vector<GC::RawRef<DOM::Node>> {});
}

GC::Ref<DOM::NodeList> SVGSVGElement::get_enclosure_list(GC::Ref<Geometry::DOMRectReadOnly>, GC::Ptr<SVGElement>) const
{
    dbgln("(STUBBED) SVGSVGElement::get_enclosure_list(). Called on: {}", debug_description());
    return DOM::StaticNodeList::create(Vector<GC::RawRef<DOM::Node>> {});
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

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__pauseAnimations
void SVGSVGElement::pause_animations()
{
    // When pauseAnimations() is called, the following steps are run:
    // 1. If the current ‘svg’ element is not the outermost svg element, then return.
    if (owner_svg_element())
        return;

    // 2. Pause the timeline that controls SVG animations for the SVG document fragment that the ‘svg’ element is in.
    // NB: An svg element that has never been connected has no timeline.
    if (m_time_container)
        m_time_container->pause();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__unpauseAnimations
void SVGSVGElement::unpause_animations()
{
    // When unpauseAnimations() is called, the following steps are run:
    // 1. If the current ‘svg’ element is not the outermost svg element, then return.
    if (owner_svg_element())
        return;

    // 2. Unpause the timeline that controls SVG animations for the SVG document fragment that the ‘svg’ element is in.
    if (m_time_container)
        m_time_container->unpause();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__animationsPaused
bool SVGSVGElement::animations_paused()
{
    // When animationsPaused() is called, true is returned if the timeline that controls SVG animations for the SVG
    // document fragment that the ‘svg’ element in has been paused by pauseAnimations; otherwise, false is returned.
    auto time_container = outermost_svg_element()->time_container();
    return time_container && time_container->is_paused();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__getCurrentTime
float SVGSVGElement::get_current_time()
{
    // When getCurrentTime() is called, the following steps are run:
    // 1. If the timeline for the current SVG document fragment has not yet begun, then return 0.
    auto time_container = outermost_svg_element()->time_container();
    if (!time_container || !time_container->has_begun())
        return 0;

    // 2. Otherwise, the timeline for the current SVG document fragment has begun. Return the current time in seconds
    //    relative to the start time of the timeline.
    return static_cast<float>(time_container->current_time_in_milliseconds() / 1000.);
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__setCurrentTime
void SVGSVGElement::set_current_time(float seconds)
{
    // When setCurrentTime(seconds) is called, the following steps are run:
    // 1. If the current ‘svg’ element is not the outermost svg element, then return.
    if (owner_svg_element() || !m_time_container)
        return;

    // 2. If the timeline for the current SVG document fragment has begun, then the timeline's current time (in seconds,
    //    relative to the start time of the timeline) is set to seconds.
    auto milliseconds = static_cast<double>(seconds) * 1000.;
    if (m_time_container->has_begun()) {
        m_time_container->seek_to_time_in_milliseconds(milliseconds);
        return;
    }

    // 3. Otherwise, the timeline has not begun. seconds is recorded on the current SVG document fragment as the time to
    //    be seeked to once the timeline has begun. If multiple calls to setCurrentTime are made before the timeline has
    //    begun, the most recent call's time will be used to seek the timeline once it begins.
    m_time_container->record_time_to_seek_to_once_begun_in_milliseconds(milliseconds);
}

// https://w3c.github.io/svgwg/svg2-draft/struct.html#__svg__SVGSVGElement__createSVGNumber
GC::Ref<SVGNumber> SVGSVGElement::create_svg_number() const
{
    return SVGNumber::create(0, SVGNumber::ReadOnly::No);
}

// https://svgwg.org/svg2-draft/struct.html#__svg__SVGSVGElement__createSVGAngle
GC::Ref<SVGAngle> SVGSVGElement::create_svg_angle() const
{
    // A new, detached SVGAngle object whose value is the unitless <number> 0.
    return SVGAngle::create_detached({}, SVGAngle::ReadOnly::No);
}

GC::Ref<SVGLength> SVGSVGElement::create_svg_length() const
{
    // A new, detached SVGLength object whose value is the unitless <number> 0.
    return SVGLength::create_detached(document().relevant_settings_object().realm(), SVGLengthValue::number(0), SVGLength::ReadOnly::No);
}

GC::Ref<Geometry::DOMPoint> SVGSVGElement::create_svg_point() const
{
    // A new, detached DOMPoint object whose coordinates are all 0.
    return Geometry::DOMPoint::create();
}

GC::Ref<Geometry::DOMMatrix> SVGSVGElement::create_svg_matrix() const
{
    // A new, detached DOMMatrix object representing the identity matrix.
    return Geometry::DOMMatrix::create();
}

GC::Ref<Geometry::DOMRect> SVGSVGElement::create_svg_rect() const
{
    // A new, DOMRect object whose x, y, width and height are all 0.
    return Geometry::DOMRect::create(0, 0, 0, 0);
}

GC::Ref<SVGTransform> SVGSVGElement::create_svg_transform() const
{
    // A new, detached SVGTransform object whose value is matrix(1, 0, 0, 1, 0, 0).
    return SVGTransform::create();
}

// https://svgwg.org/svg2-draft/struct.html#__svg__SVGSVGElement__createSVGTransformFromMatrix
WebIDL::ExceptionOr<GC::Ref<SVGTransform>> SVGSVGElement::create_svg_transform_from_matrix(Bindings::DOMMatrix2DInit const& matrix) const
{
    // Its behavior is the same as the createSVGTransformFromMatrix method on SVGTransformList.
    // https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransformList__createSVGTransformFromMatrix
    // 1. Let transform be a newly created SVGTransform object that is detached.
    auto transform = SVGTransform::create();

    // 2. Follow the steps that would be run if the setMatrix method on transform were called, passing matrix as its
    //    argument.
    TRY(transform->set_matrix(matrix));

    // 3. Return transform.
    return transform;
}

// https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
Optional<CSSPixelFraction> SVGSVGElement::view_box_natural_aspect_ratio(SVGSVGElement const& svg_root)
{
    // 2. If an SVG View is active:
    if (auto active_view_element = svg_root.active_view_element(); active_view_element && active_view_element->view_box().has_value()) {
        // 1. let viewbox be the viewbox defined by the active SVG View
        auto view_box = active_view_element->view_box().value();

        // 2. return viewbox.width / viewbox.height
        if (view_box.width != 0 && view_box.height != 0)
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
}

}
