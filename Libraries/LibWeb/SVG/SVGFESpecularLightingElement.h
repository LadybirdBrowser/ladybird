/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#InterfaceSVGFESpecularLightingElement
class SVGFESpecularLightingElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFESpecularLightingElement> {
    WEB_WRAPPABLE(SVGFESpecularLightingElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFESpecularLightingElement);

public:
    virtual ~SVGFESpecularLightingElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedNumber> surface_scale();
    GC::Ref<SVGAnimatedNumber> specular_constant();
    GC::Ref<SVGAnimatedNumber> specular_exponent();
    GC::Ref<SVGAnimatedNumber> kernel_unit_length_x();
    GC::Ref<SVGAnimatedNumber> kernel_unit_length_y();

private:
    SVGFESpecularLightingElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedNumber> m_surface_scale;
    GC::Ptr<SVGAnimatedNumber> m_specular_constant;
    GC::Ptr<SVGAnimatedNumber> m_specular_exponent;
    GC::Ptr<SVGAnimatedNumber> m_kernel_unit_length_x;
    GC::Ptr<SVGAnimatedNumber> m_kernel_unit_length_y;
};

}
