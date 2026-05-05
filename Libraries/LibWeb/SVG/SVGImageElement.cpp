/*
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGImageElement.h"
#include <LibCore/Timer.h>
#include <LibGC/Heap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/Bindings/SVGImageElement.h>
#include <LibWeb/CSS/PropertyID.h>
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
    m_animation_timer = Core::Timer::create();
    m_animation_timer->on_timeout = [this] { animate(); };
}

SVGImageElement::~SVGImageElement() = default;

void SVGImageElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGImageElement);
    Base::initialize(realm);
}

void SVGImageElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    image_provider_visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_resource_request);
}

void SVGImageElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href) {
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
    return svg_animated_length_for_property(CSS::PropertyID::X);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__y
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::y()
{
    return svg_animated_length_for_property(CSS::PropertyID::Y);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__width
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::width()
{
    return svg_animated_length_for_property(CSS::PropertyID::Width);
}

// https://svgwg.org/svg2-draft/embedded.html#__svg__SVGImageElement__height
GC::Ref<SVG::SVGAnimatedLength> SVGImageElement::height()
{
    return svg_animated_length_for_property(CSS::PropertyID::Height);
}

Gfx::FloatRect SVGImageElement::bounding_box() const
{
    auto node = unsafe_layout_node();
    if (!node)
        return {};

    CSSPixels viewport_width = 0;
    CSSPixels viewport_height = 0;
    if (auto viewport = const_cast<SVGImageElement*>(this)->viewport_element()) {
        if (auto viewport_node = viewport->unsafe_layout_node()) {
            viewport_width = viewport_node->computed_values().width().to_px(*viewport_node, 0);
            viewport_height = viewport_node->computed_values().height().to_px(*viewport_node, 0);
        }
    }

    Optional<float> width;
    if (!node->computed_values().width().is_auto())
        width = node->computed_values().width().to_px(*node, viewport_width).to_float();

    Optional<float> height;
    if (!node->computed_values().height().is_auto())
        height = node->computed_values().height().to_px(*node, viewport_height).to_float();

    if (!height.has_value() && width.has_value() && intrinsic_aspect_ratio().has_value())
        height = width.value() / intrinsic_aspect_ratio().value().to_float();

    if (!width.has_value() && height.has_value() && intrinsic_aspect_ratio().has_value())
        width = height.value() * intrinsic_aspect_ratio().value().to_float();

    if (!width.has_value() && intrinsic_width().has_value())
        width = intrinsic_width()->to_float();

    if (!height.has_value() && intrinsic_height().has_value())
        height = intrinsic_height()->to_float();

    return {
        node->computed_values().x().to_px(*node, viewport_width).to_float(),
        node->computed_values().y().to_px(*node, viewport_height).to_float(),
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
    m_resource_request = HTML::SharedResourceRequest::get_or_create(realm(), document().page(), url);
    m_resource_request->add_callbacks(
        [this, resource_request = GC::Root { m_resource_request }] {
            m_load_event_delayer.clear();
            auto image_data = resource_request->image_data();
            if (image_data->is_animated() && image_data->frame_count() > 1) {
                m_current_frame_index = 0;
                m_animation_timer->set_interval(image_data->frame_duration(0));
                m_animation_timer->start();
            }
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

GC::Ptr<Layout::Node> SVGImageElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGImageBox>(document(), *this, move(style));
}

bool SVGImageElement::is_image_available() const
{
    return m_resource_request && m_resource_request->image_data();
}

Optional<CSSPixels> SVGImageElement::intrinsic_width() const
{
    if (!m_resource_request)
        return {};
    if (auto image_data = m_resource_request->image_data())
        return image_data->intrinsic_width();
    return {};
}

Optional<CSSPixels> SVGImageElement::intrinsic_height() const
{
    if (!m_resource_request)
        return {};
    if (auto image_data = m_resource_request->image_data())
        return image_data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> SVGImageElement::intrinsic_aspect_ratio() const
{
    if (!m_resource_request)
        return {};
    if (auto image_data = m_resource_request->image_data())
        return image_data->intrinsic_aspect_ratio();
    return {};
}

RefPtr<Gfx::DecodedImageFrame> SVGImageElement::default_image_frame_sized(Gfx::IntSize size) const
{
    if (!m_resource_request)
        return {};
    if (auto data = m_resource_request->image_data())
        return data->frame(0, size);
    return {};
}

RefPtr<Gfx::DecodedImageFrame> SVGImageElement::current_image_frame_sized(Gfx::IntSize size) const
{
    if (!m_resource_request)
        return {};
    if (auto data = m_resource_request->image_data())
        return data->frame(m_current_frame_index, size);
    return {};
}

void SVGImageElement::animate()
{
    auto image_data = m_resource_request->image_data();
    if (!image_data) {
        return;
    }

    m_current_frame_index = (m_current_frame_index + 1) % image_data->frame_count();
    auto current_frame_duration = image_data->frame_duration(m_current_frame_index);

    if (current_frame_duration != m_animation_timer->interval()) {
        m_animation_timer->restart(current_frame_duration);
    }

    if (m_current_frame_index == image_data->frame_count() - 1) {
        ++m_loops_completed;
        if (m_loops_completed > 0 && m_loops_completed == image_data->loop_count()) {
            m_animation_timer->stop();
        }
    }

    if (paintable())
        paintable()->set_needs_repaint();
}

GC::Ptr<HTML::DecodedImageData> SVGImageElement::decoded_image_data() const
{
    if (!m_resource_request)
        return nullptr;
    return m_resource_request->image_data();
}

}
