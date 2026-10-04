/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGFEImageElement.h"
#include <LibWeb/Bindings/SVGFEImageElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Namespace.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEImageElement);

SVGFEImageElement::SVGFEImageElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEImageElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_resource_request);
}

void SVGFEImageElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href) {
        if (namespace_ == Namespace::XLink && has_attribute_ns(Optional<Utf16FlyString> {}, name))
            return;

        auto href = value;
        if (!namespace_.has_value() && !href.has_value())
            href = get_attribute_ns(Namespace::XLink, SVG::AttributeNames::href);

        process_href(href);
    }
}

void SVGFEImageElement::process_href(Optional<Utf16String> const& href)
{
    if (!href.has_value()) {
        m_href = {};
        return;
    }

    m_href = document().encoding_parse_url(*href);
    if (!m_href.has_value())
        return;

    m_resource_request = HTML::SharedResourceRequest::get_or_create(document(), *m_href);
    m_resource_request->add_callbacks(
        [this, resource_request = GC::Root { m_resource_request }] {
            document().note_svg_paint_resources_changed();
            document().schedule_full_accumulated_visual_context_rebuild(Layout::RustFFI::VisualContextUpdateScope::EveryBox);
            document().set_needs_repaint(Badge<SVGFEImageElement> {}, InvalidateDisplayList::PaintCommands);
        },
        nullptr);

    if (m_resource_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(*m_href, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document().relevant_settings_object());
        m_resource_request->fetch_resource(request);
    }
}

GC::Ptr<HTML::DecodedImageData> SVGFEImageElement::image_data() const
{
    if (!m_resource_request)
        return {};
    return m_resource_request->image_data();
}

}
