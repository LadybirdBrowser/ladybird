/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#InterfaceSVGFEDistantLightElement
class SVGFEDistantLightElement final
    : public SVGElement {
    WEB_WRAPPABLE(SVGFEDistantLightElement, SVGElement);

public:
    virtual ~SVGFEDistantLightElement() override = default;

    GC::Ref<SVGAnimatedNumber> azimuth();
    GC::Ref<SVGAnimatedNumber> elevation();

private:
    explicit SVGFEDistantLightElement(DOM::RustMadePrefix);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedNumber> m_azimuth;
    GC::Ptr<SVGAnimatedNumber> m_elevation;
};

}
