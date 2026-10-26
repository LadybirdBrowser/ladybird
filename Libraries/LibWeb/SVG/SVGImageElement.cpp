/*
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGImageElement.h"
#include <LibGC/Heap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/Bindings/SVGImageElement.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/PaintTiming/PaintTiming.h>
#include <LibWeb/Painting/PaintFacts.h>
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

void SVGImageElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_resource_request);
    visitor.visit(m_preserve_aspect_ratio);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__preserveAspectRatio
GC::Ref<SVGAnimatedPreserveAspectRatio> SVGImageElement::preserve_aspect_ratio_for_bindings()
{
    if (!m_preserve_aspect_ratio)
        m_preserve_aspect_ratio = SVGAnimatedPreserveAspectRatio::create(*this);
    return *m_preserve_aspect_ratio;
}

void SVGImageElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);

    if (m_load_event_delayer.has_value())
        m_load_event_delayer.emplace(document());

    // NB: See process_the_url().
    if (old_document.is_temporary_document_for_fragment_parsing()) {
        auto href = get_attribute_ns(Optional<Utf16FlyString> {}, SVG::AttributeNames::href);
        if (!href.has_value())
            href = get_attribute_ns(Namespace::XLink, SVG::AttributeNames::href);
        process_the_url(href);
    }
}

void SVGImageElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href) {
        // https://svgwg.org/svg2-draft/linking.html#XLinkRefAttrs
        // For backwards compatibility, elements with an ‘href’ attribute also recognize an ‘href’ attribute in the
        // XLink namespace. If the element is in the XLink namespace, it does not recognize an ‘href’ attribute in the
        // SVG namespace. When the ‘href’ attribute is present in both the XLink namespace and without a namespace, the
        // value of the attribute without a namespace shall be used. The attribute in the XLink namespace shall be ignored.
        if (namespace_ == Namespace::XLink && has_attribute_ns(Optional<Utf16FlyString> {}, name))
            return;

        auto href = value;
        if (!namespace_.has_value() && !href.has_value())
            href = get_attribute_ns(Namespace::XLink, SVG::AttributeNames::href);

        process_the_url(href);
    }
}

// https://www.w3.org/TR/SVG2/linking.html#processingURL
void SVGImageElement::process_the_url(Optional<Utf16String> const& href)
{
    // AD-HOC: An element in a temporary document for fragment parsing does not fetch anything, so that markup parsed
    //         into such a document, for example by the Sanitizer API, causes no network requests for elements that
    //         are never inserted anywhere. The URL is processed once the element is adopted into another document.
    if (document().is_temporary_document_for_fragment_parsing())
        return;

    if (!href.has_value()) {
        m_href = {};
        abandon_the_current_fetch();
        return;
    }

    m_href = document().encoding_parse_url(*href);
    if (!m_href.has_value()) {
        abandon_the_current_fetch();
        return;
    }

    fetch_the_document(*m_href);
}

void SVGImageElement::abandon_the_current_fetch()
{
    // AD-HOC: Without a URL to fetch, the element has no image request. An earlier request that is still loading must
    //         not fire events or keep delaying the load event, and an image that has already loaded is no longer the
    //         element's image.
    ++m_current_fetch_id;
    m_load_event_delayer.clear();
    if (!m_resource_request)
        return;
    unregister_with_decoded_image_data_if_needed();
    m_resource_request = nullptr;
    CSS::record_element_replaced_content_input(*this);
    Painting::push_replaced_image_paint_facts(*this, *this);
    set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGImageElementFetchTheDocument);
}

// https://svgwg.org/svg2-draft/linking.html#processingURL-fetch
void SVGImageElement::fetch_the_document(URL::URL const& url)
{
    // NB: Replacing the delayer releases the one held for the previous request, so the document only waits for the
    //     current request.
    m_load_event_delayer.emplace(document());
    unregister_with_decoded_image_data_if_needed();
    auto resource_request = HTML::SharedResourceRequest::get_or_create(document(), url, HTML::CORSSettingAttribute::NoCORS);
    m_resource_request = resource_request;
    auto fetch_id = ++m_current_fetch_id;
    CSS::record_element_replaced_content_input(*this);

    // AD-HOC: A request that finishes after href has changed is no longer the element's image request. Its completion
    //         must not fire events or release the load event delayer of the current request. Shared resource requests
    //         are shared per URL and CORS mode, so each fetch gets its own ID instead of comparing requests.
    resource_request->add_callbacks(
        [this, fetch_id] {
            if (fetch_id != m_current_fetch_id)
                return;

            m_load_event_delayer.clear();
            register_with_decoded_image_data_if_needed();
            CSS::record_element_replaced_content_input(*this);
            Painting::push_replaced_image_paint_facts(*this, *this);
            set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGImageElementFetchTheDocument);

            // https://w3c.github.io/paint-timing/#sec-modifications-dom
            // When an Element element's associated image request has become completely available, run the algorithm
            // to process an image that finished loading passing in element and its associated image request as
            // inputs.
            PaintTiming::process_an_image_that_finished_loading(*this, PaintTiming::image_request_info(Utf16String::from_utf8(m_resource_request->url().serialize()), m_resource_request->image_data()));

            dispatch_event(DOM::Event::create(HTML::EventNames::load,
                HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this))));
        },
        [this, fetch_id] {
            if (fetch_id != m_current_fetch_id)
                return;

            m_load_event_delayer.clear();

            dispatch_event(DOM::Event::create(HTML::EventNames::error,
                HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this))));
        },
        [this, fetch_id] {
            if (fetch_id != m_current_fetch_id)
                return;

            m_load_event_delayer.clear();
        });

    // NB: A request that has already completed runs its callbacks right away, and an event handler can change href.
    //     Then this fetch is no longer the current one.
    if (fetch_id != m_current_fetch_id)
        return;

    if (resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        resource_request->fetch_resource(request);
    }
}

CSS::ElementBoxKind SVGImageElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgImage;
}

GC::Ptr<HTML::DecodedImageData> SVGImageElement::decoded_image_data() const
{
    if (!m_resource_request)
        return nullptr;
    return m_resource_request->image_data();
}

void SVGImageElement::decoded_image_data_did_update()
{
    // An SVG image works out its natural size again after it redraws itself or changes color scheme.
    CSS::record_element_replaced_content_input(*this);
    Painting::push_replaced_image_paint_facts(*this, *this);
}

}
