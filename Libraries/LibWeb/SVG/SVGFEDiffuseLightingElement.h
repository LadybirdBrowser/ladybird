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

// https://drafts.fxtf.org/filter-effects/#InterfaceSVGFEDiffuseLightingElement
class SVGFEDiffuseLightingElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEDiffuseLightingElement> {
    WEB_WRAPPABLE(SVGFEDiffuseLightingElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEDiffuseLightingElement);

public:
    virtual ~SVGFEDiffuseLightingElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedNumber> surface_scale();
    GC::Ref<SVGAnimatedNumber> diffuse_constant();
    GC::Ref<SVGAnimatedNumber> kernel_unit_length_x();
    GC::Ref<SVGAnimatedNumber> kernel_unit_length_y();

private:
    SVGFEDiffuseLightingElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedNumber> m_surface_scale;
    GC::Ptr<SVGAnimatedNumber> m_diffuse_constant;
    GC::Ptr<SVGAnimatedNumber> m_kernel_unit_length_x;
    GC::Ptr<SVGAnimatedNumber> m_kernel_unit_length_y;
};

}
