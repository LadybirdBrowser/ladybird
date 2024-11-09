/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedString.h>

namespace Web::SVG {

enum class SupportsXLinkHref {
    No,
    Yes,
};

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGURIReference
template<SupportsXLinkHref supports_xlink_href>
class SVGURIReferenceMixin {
public:
    virtual ~SVGURIReferenceMixin() = default;

    JS::NonnullGCPtr<SVGAnimatedString> href()
    {
        // The href IDL attribute represents the value of the ‘href’ attribute, and, on elements that are defined to support
        // it, the deprecated ‘xlink:href’ attribute. On getting href, an SVGAnimatedString object is returned that:
        //    - reflects the ‘href’ attribute, and
        //    - if the element is defined to support the deprecated ‘xlink:href’ attribute, additionally reflects that
        //      deprecated attribute.
        if (!m_href_animated_string) {
            auto* this_svg_element = dynamic_cast<SVGElement*>(this);
            VERIFY(this_svg_element);
            m_href_animated_string = SVGAnimatedString::create(this_svg_element->realm(), *this_svg_element, AttributeNames::href, supports_xlink_href == SupportsXLinkHref::Yes ? Optional<FlyString> { AttributeNames::xlink_href } : OptionalNone {});
        }
        return *m_href_animated_string;
    }

protected:
    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(m_href_animated_string);
    }

private:
    JS::GCPtr<SVGAnimatedString> m_href_animated_string;
};

}
