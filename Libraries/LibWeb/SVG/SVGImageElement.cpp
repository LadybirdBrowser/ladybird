/*
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGImageElement.h"
#include <LibGC/Heap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/Bindings/SVGImageElement.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
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

Gfx::FloatRect SVGImageElement::bounding_box(CSSPixelSize viewport_size) const
{
    auto computed_values = this->computed_style();
    VERIFY(computed_values);

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#Placement
    // Computation of automatically-sized values follows the Default Sizing Algorithm defined for replaced elements in
    // CSS layout [css-images-3]. In particular, when the referenced resource does not have an intrinsic size (such as
    // image types with no defined dimensions), it is assumed to have a width of 300px and a height of 150px.
    auto specified_width = computed_values->width().is_length_percentage() ? computed_values->width().to_px(viewport_size.width()) : Optional<CSSPixels> {};
    auto specified_height = computed_values->height().is_length_percentage() ? computed_values->height().to_px(viewport_size.height()) : Optional<CSSPixels> {};

    CSS::SizeWithAspectRatio intrinsic_size_with_aspect_ratio { this->intrinsic_width(), this->intrinsic_height(), this->intrinsic_aspect_ratio() };

    CSSPixelSize default_size {};

    if (decoded_image_data())
        default_size = CSSPixelSize { 300, 150 };

    auto sizing = CSS::run_default_sizing_algorithm(specified_width, specified_height, intrinsic_size_with_aspect_ratio, default_size);

    return {
        computed_values->x().to_px(viewport_size.width()).to_float(),
        computed_values->y().to_px(viewport_size.height()).to_float(),
        sizing.width().to_float(),
        sizing.height().to_float()
    };
}

// https://www.w3.org/TR/SVG2/linking.html#processingURL
void SVGImageElement::process_the_url(Optional<Utf16String> const& href)
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
    m_resource_request = HTML::SharedResourceRequest::get_or_create(document(), url);
    m_resource_request->add_callbacks(
        [this, resource_request = GC::Root { m_resource_request }] {
            m_load_event_delayer.clear();
            register_with_decoded_image_data_if_needed();
            document().style_computer().style_engine().record_element_style_input_change(style_node_id());
            set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGImageElementFetchTheDocument);

            dispatch_event(DOM::Event::create(HTML::EventNames::load,
                HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this))));
        },
        [this] {
            m_load_event_delayer.clear();

            dispatch_event(DOM::Event::create(HTML::EventNames::error,
                HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this))));
        });

    if (m_resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_resource_request->fetch_resource(request);
    }
}

RefPtr<Layout::Node> SVGImageElement::create_layout_node(CSS::LayoutStyle style)
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
