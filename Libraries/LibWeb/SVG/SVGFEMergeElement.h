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
    WEB_PLATFORM_OBJECT(SVGFEMergeElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEMergeElement);

public:
    virtual ~SVGFEMergeElement() override = default;

private:
    SVGFEMergeElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
