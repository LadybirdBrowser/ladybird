/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#svgfemergeelement
class SVGFEMergeElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEMergeElement> {
    WEB_WRAPPABLE(SVGFEMergeElement, SVGElement);

public:
    virtual ~SVGFEMergeElement() override = default;

private:
    explicit SVGFEMergeElement(DOM::RustMadePrefix);
    virtual void visit_edges(Cell::Visitor&) override;
};

}
