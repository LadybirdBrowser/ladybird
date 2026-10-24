/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGTextPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTextPathElement);

SVGTextPathElement::SVGTextPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextContentElement(document, move(qualified_name))
{
}

void SVGTextPathElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::startOffset)
        m_start_offset = parse_number_percentage(value.value_or({}));
}

Optional<Utf16String> SVGTextPathElement::href_attribute_value() const
{
    if (has_attribute(AttributeNames::href))
        return get_attribute(AttributeNames::href);
    return get_attribute(AttributeNames::xlink_href);
}

void SVGTextPathElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

CSS::ElementBoxKind SVGTextPathElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgTextPath;
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPathElement__method
GC::Ref<SVGAnimatedEnumeration> SVGTextPathElement::method()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "align"sv, TEXTPATH_METHODTYPE_ALIGN },
        SVGAnimatedEnumeration::Keyword { "stretch"sv, TEXTPATH_METHODTYPE_STRETCH },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::method, keywords, TEXTPATH_METHODTYPE_ALIGN);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPathElement__spacing
GC::Ref<SVGAnimatedEnumeration> SVGTextPathElement::spacing()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "auto"sv, TEXTPATH_SPACINGTYPE_AUTO },
        SVGAnimatedEnumeration::Keyword { "exact"sv, TEXTPATH_SPACINGTYPE_EXACT },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::spacing, keywords, TEXTPATH_SPACINGTYPE_EXACT);
}

}
