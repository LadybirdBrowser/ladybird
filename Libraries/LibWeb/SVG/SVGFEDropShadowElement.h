/*
 * Copyright (c) 2026, Shannon Booth <shannon@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFECompositeElement
class SVGFEDropShadowElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEDropShadowElement> {
    WEB_PLATFORM_OBJECT(SVGFEDropShadowElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEDropShadowElement);

public:
    virtual ~SVGFEDropShadowElement() override = default;

    GC::Ref<SVGAnimatedString> in1();

    GC::Ref<SVGAnimatedNumber> dx();
    GC::Ref<SVGAnimatedNumber> dy();

    GC::Ref<SVGAnimatedNumber> std_deviation_x();
    GC::Ref<SVGAnimatedNumber> std_deviation_y();

    void set_std_deviation(float std_deviation_x, float std_deviation_y);

    Gfx::Color flood_color();
    float flood_opacity() const;

private:
    SVGFEDropShadowElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;

    GC::Ptr<SVGAnimatedNumber> m_dx;
    GC::Ptr<SVGAnimatedNumber> m_dy;
    GC::Ptr<SVGAnimatedNumber> m_std_deviation_x;
    GC::Ptr<SVGAnimatedNumber> m_std_deviation_y;
};

}
