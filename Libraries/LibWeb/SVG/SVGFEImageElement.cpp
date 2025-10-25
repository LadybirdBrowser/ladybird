/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGFEImageElement.h"
#include <LibCore/Timer.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Bindings/SVGFEImageElementPrototype.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Namespace.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEImageElement);

SVGFEImageElement::SVGFEImageElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEImageElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEImageElement);
    Base::initialize(realm);
}

void SVGFEImageElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_resource_request);
}

void SVGFEImageElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href) {
        if (namespace_ == Namespace::XLink && has_attribute_ns({}, name))
            return;

        auto href = value;
        if (!namespace_.has_value() && !href.has_value())
            href = get_attribute_ns(SVG::AttributeNames::href, Namespace::XLink);

        process_href(href);
    }
}

void SVGFEImageElement::process_href(Optional<String> const& href)
{
    if (!href.has_value()) {
        m_href = {};
        return;
    }

    m_href = document().encoding_parse_url(*href);
    if (!m_href.has_value())
        return;

    m_resource_request = HTML::SharedResourceRequest::get_or_create(realm(), document().page(), *m_href);
    m_resource_request->add_callbacks(
        [this, resource_request = GC::Root { m_resource_request }] {
            set_needs_style_update(true);
            if (auto layout_node = this->layout_node())
                layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGImageFilterFetch);
        },
        nullptr);

    if (m_resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(vm(), *m_href, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_resource_request->fetch_resource(realm(), request);
    }
}

RefPtr<Gfx::ImmutableBitmap> SVGFEImageElement::current_image_bitmap(Gfx::IntSize size) const
{
    if (!m_resource_request)
        return {};
    if (auto data = m_resource_request->image_data())
        return data->bitmap(0, size);
    return {};
}

Optional<Gfx::IntRect> SVGFEImageElement::content_rect() const
{
    auto bitmap = current_image_bitmap();
    if (!bitmap)
        return {};
    auto layout_node = this->layout_node();
    if (!layout_node)
        return {};
    auto width = layout_node->computed_values().width().to_px(*layout_node, 0);
    if (width == 0)
        width = bitmap->width();

    auto height = layout_node->computed_values().height().to_px(*layout_node, 0);
    if (height == 0)
        height = bitmap->height();

    auto x = layout_node->computed_values().x().to_px(*layout_node, 0);
    auto y = layout_node->computed_values().y().to_px(*layout_node, 0);
    return Gfx::enclosing_int_rect({ x, y, width, height });
}

}
