/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

template<typename IncludingClass>
class SVGFilterPrimitiveStandardAttributes {
public:
    virtual ~SVGFilterPrimitiveStandardAttributes() = default;

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterprimitivestandardattributes-x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, CSS::PercentageStyleValue::create(CSS::Percentage { 0 }));

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterprimitivestandardattributes-y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, CSS::PercentageStyleValue::create(CSS::Percentage { 0 }));

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterprimitivestandardattributes-width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, CSS::PercentageStyleValue::create(CSS::Percentage { 100 }));

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterprimitivestandardattributes-height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, CSS::PercentageStyleValue::create(CSS::Percentage { 100 }));

    GC::Ref<SVGAnimatedString> result();

protected:
    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(m_result_animated_string);
    }

private:
    SVGElement* this_svg_element() { return static_cast<IncludingClass*>(this); }

    GC::Ref<SVGAnimatedLength> svg_animated_length_for_attribute(Utf16FlyString const& attribute_name, SVGLength::Directionality directionality, NonnullRefPtr<CSS::StyleValue const>&& default_value)
    {
        return this_svg_element()->svg_animated_length_for_attribute(attribute_name, directionality, move(default_value));
    }

    GC::Ptr<SVGAnimatedString> m_result_animated_string;
};

}
