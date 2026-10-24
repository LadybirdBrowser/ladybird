/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#InterfaceSVGFEPointLightElement
class SVGFEPointLightElement final
    : public SVGElement {
    WEB_WRAPPABLE(SVGFEPointLightElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEPointLightElement);

public:
    virtual ~SVGFEPointLightElement() override = default;

    GC::Ref<SVGAnimatedNumber> x();
    GC::Ref<SVGAnimatedNumber> y();
    GC::Ref<SVGAnimatedNumber> z();

private:
    SVGFEPointLightElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedNumber> m_x;
    GC::Ptr<SVGAnimatedNumber> m_y;
    GC::Ptr<SVGAnimatedNumber> m_z;
};

}
