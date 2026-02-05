/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#svgfeoffsetelement
class SVGFEOffsetElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEOffsetElement> {
    WEB_PLATFORM_OBJECT(SVGFEOffsetElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEOffsetElement);

public:
    virtual ~SVGFEOffsetElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedNumber> dx();
    GC::Ref<SVGAnimatedNumber> dy();

private:
    SVGFEOffsetElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedNumber> m_dx;
    GC::Ptr<SVGAnimatedNumber> m_dy;
};

}
