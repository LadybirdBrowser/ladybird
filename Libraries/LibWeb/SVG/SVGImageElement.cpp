/*
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGImageElement.h"
#include <LibGC/Heap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/Bindings/SVGImageElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGImageElement);

SVGImageElement::SVGImageElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGImageElement::~SVGImageElement() = default;

void SVGImageElement::finalize()
{
    Base::finalize();
    unregister_with_decoded_image_data_if_needed();
}

void SVGImageElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGImageElement);
    Base::initialize(realm);
}

void SVGImageElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_resource_request);
}

void SVGImageElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);

    if (m_load_event_delayer.has_value())
        m_load_event_delayer.emplace(document());
}

void SVGImageElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::x) {
        m_x = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y) {
        m_y = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::width) {
        m_width = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::height) {
        m_height = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::href) {
        // https://svgwg.org/svg2-draft/linking.html#XLinkRefAttrs
        // For backwards compatibility, elements with an ‘href’ attribute also recognize an ‘href’ attribute in the
        // XLink namespace. If the element is in the XLink namespace, it does not recognize an ‘href’ attribute in the
        // SVG namespace. When the ‘href’ attribute is present in both the XLink namespace and without a namespace, the
        // value of the attribute without a namespace shall be used. The attribute in the XLink namespace shall be ignored.
        if (namespace_ == Namespace::XLink && has_attribute_ns({}, name))
            return;

        auto href = value;
        if (!namespace_.has_value() && !href.has_value())
            href = get_attribute_ns(SVG::AttributeNames::href, Namespace::XLink);

        process_the_url(href);
    }
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__x
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::x()
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto value = m_x.value_or(NumberPercentage::create_number(0)).value();
    auto base_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__y
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::y()
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto value = m_y.value_or(NumberPercentage::create_number(0)).value();
    auto base_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__width
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::width()
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto value = m_width.has_value() ? m_width->value() : intrinsic_width().value_or(0).to_double();
    auto base_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__height
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::height()
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto value = m_height.has_value() ? m_height->value() : intrinsic_height().value_or(0).to_double();
    auto base_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, value, SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

Gfx::FloatRect SVGImageElement::bounding_box(CSSPixelSize viewport_size) const
{
    Optional<float> width;
    if (m_width.has_value())
        width = m_width->resolve_relative_to(viewport_size.width().to_float());

    Optional<float> height;
    if (m_height.has_value())
        height = m_height->resolve_relative_to(viewport_size.height().to_float());

    if (!height.has_value() && width.has_value() && intrinsic_aspect_ratio().has_value())
        height = width.value() / intrinsic_aspect_ratio().value().to_float();

    if (!width.has_value() && height.has_value() && intrinsic_aspect_ratio().has_value())
        width = height.value() * intrinsic_aspect_ratio().value().to_float();

    if (!width.has_value() && intrinsic_width().has_value())
        width = intrinsic_width()->to_float();

    if (!height.has_value() && intrinsic_height().has_value())
        height = intrinsic_height()->to_float();

    return {
        m_x.value_or(NumberPercentage::create_number(0)).resolve_relative_to(viewport_size.width().to_float()),
        m_y.value_or(NumberPercentage::create_number(0)).resolve_relative_to(viewport_size.height().to_float()),
        width.value_or(0.0f),
        height.value_or(0.0f),
    };
}

// https://www.w3.org/TR/SVG2/linking.html#processingURL
void SVGImageElement::process_the_url(Optional<String> const& href)
{
    if (!href.has_value()) {
        m_href = {};
        return;
    }

    m_href = document().encoding_parse_url(*href);
    if (!m_href.has_value())
        return;

    fetch_the_document(*m_href);
}

// https://svgwg.org/svg2-draft/linking.html#processingURL-fetch
void SVGImageElement::fetch_the_document(URL::URL const& url)
{
    m_load_event_delayer.emplace(document());
    unregister_with_decoded_image_data_if_needed();
    m_resource_request = HTML::SharedResourceRequest::get_or_create(realm(), document().page(), url);
    m_resource_request->add_callbacks(
        [this, resource_request = GC::Root { m_resource_request }] {
            m_load_event_delayer.clear();
            register_with_decoded_image_data_if_needed();
            set_needs_style_update(true);
            set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGImageElementFetchTheDocument);

            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
        },
        [this] {
            m_load_event_delayer.clear();

            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
        });

    if (m_resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(vm(), url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_resource_request->fetch_resource(realm(), request);
    }
}

RefPtr<Layout::Node> SVGImageElement::create_layout_node(CSS::ComputedProperties const& style)
{
    return make_ref_counted<Layout::SVGImageBox>(document(), *this, style);
}

GC::Ptr<HTML::DecodedImageData> SVGImageElement::decoded_image_data() const
{
    if (!m_resource_request)
        return nullptr;
    return m_resource_request->image_data();
}

}
