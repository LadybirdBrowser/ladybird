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

    GC::Ref<SVGAnimatedLength> x()
    {
        return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::X);
    }

    GC::Ref<SVGAnimatedLength> y()
    {
        return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::Y);
    }

    GC::Ref<SVGAnimatedLength> width()
    {
        return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::Width);
    }

    GC::Ref<SVGAnimatedLength> height()
    {
        return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::Height);
    }

    GC::Ref<SVGAnimatedString> result()
    {
        if (!m_result_animated_string)
            m_result_animated_string = SVGAnimatedString::create(this_svg_element()->realm(), *this_svg_element(), AttributeNames::result);

        return *m_result_animated_string;
    }

protected:
    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(m_result_animated_string);
    }

private:
    SVGElement* this_svg_element() { return static_cast<IncludingClass*>(this); }

    GC::Ptr<SVGAnimatedString> m_result_animated_string;
};

}
