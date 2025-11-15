/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEComponentTransferElement
class SVGFEComponentTransferElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEComponentTransferElement> {
    WEB_PLATFORM_OBJECT(SVGFEComponentTransferElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEComponentTransferElement);

public:
    virtual ~SVGFEComponentTransferElement() override = default;

    GC::Ref<SVGAnimatedString> in1();

private:
    SVGFEComponentTransferElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
};

}
