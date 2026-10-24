/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#InterfaceSVGFESpotLightElement
class SVGFESpotLightElement final
    : public SVGElement {
    WEB_WRAPPABLE(SVGFESpotLightElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFESpotLightElement);

public:
    virtual ~SVGFESpotLightElement() override = default;

    GC::Ref<SVGAnimatedNumber> x();
    GC::Ref<SVGAnimatedNumber> y();
    GC::Ref<SVGAnimatedNumber> z();
    GC::Ref<SVGAnimatedNumber> points_at_x();
    GC::Ref<SVGAnimatedNumber> points_at_y();
    GC::Ref<SVGAnimatedNumber> points_at_z();
    GC::Ref<SVGAnimatedNumber> specular_exponent();
    GC::Ref<SVGAnimatedNumber> limiting_cone_angle();

private:
    SVGFESpotLightElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedNumber> m_x;
    GC::Ptr<SVGAnimatedNumber> m_y;
    GC::Ptr<SVGAnimatedNumber> m_z;
    GC::Ptr<SVGAnimatedNumber> m_points_at_x;
    GC::Ptr<SVGAnimatedNumber> m_points_at_y;
    GC::Ptr<SVGAnimatedNumber> m_points_at_z;
    GC::Ptr<SVGAnimatedNumber> m_specular_exponent;
    GC::Ptr<SVGAnimatedNumber> m_limiting_cone_angle;
};

}
