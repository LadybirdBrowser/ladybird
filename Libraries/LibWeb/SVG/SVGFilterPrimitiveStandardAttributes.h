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

    GC::Ref<SVGAnimatedLength> x();
    GC::Ref<SVGAnimatedLength> y();
    GC::Ref<SVGAnimatedLength> width();
    GC::Ref<SVGAnimatedLength> height();
    GC::Ref<SVGAnimatedString> result();

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
