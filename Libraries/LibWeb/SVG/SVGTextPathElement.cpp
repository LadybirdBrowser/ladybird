/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/Bindings/SVGTextPathElement.h>
#include <LibWeb/Layout/SVGTextPathBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGTextPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTextPathElement);

SVGTextPathElement::SVGTextPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextContentElement(document, move(qualified_name))
{
}

void SVGTextPathElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::startOffset)
        m_start_offset = AttributeParser::parse_number_percentage(value.value_or(String {}));
}

GC::Ptr<SVGGeometryElement const> SVGTextPathElement::path_or_shape() const
{
    auto href = has_attribute(AttributeNames::href) ? get_attribute(AttributeNames::href) : get_attribute(AttributeNames::xlink_href);
    if (!href.has_value())
        return {};
    return try_resolve_url_to<SVGGeometryElement const>(*href);
}

// https://svgwg.org/svg2-draft/text.html#TextPathElementStartOffsetAttribute
float SVGTextPathElement::start_offset_for_path_length(float path_length) const
{
    if (!m_start_offset.has_value())
        return 0;
    return m_start_offset->resolve_relative_to(path_length);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPathElement__startOffset
GC::Ref<SVGAnimatedLength> SVGTextPathElement::start_offset() const
{
    auto base_length = SVGLength::create(realm(), 0, m_start_offset.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_start_offset.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

void SVGTextPathElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextPathElement);
    Base::initialize(realm);
}

void SVGTextPathElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

RefPtr<Layout::Node> SVGTextPathElement::create_layout_node(CSS::ComputedProperties const& style)
{
    return make_ref_counted<Layout::SVGTextPathBox>(document(), *this, style);
}

};
