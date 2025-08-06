/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#svgfemergenodeelement
class SVGFEMergeNodeElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGFEMergeNodeElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEMergeNodeElement);

public:
    virtual ~SVGFEMergeNodeElement() override = default;

    GC::Ref<SVGAnimatedString> in1();

private:
    SVGFEMergeNodeElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
};

}
