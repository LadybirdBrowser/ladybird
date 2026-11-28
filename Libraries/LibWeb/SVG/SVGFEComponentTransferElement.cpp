/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGFEComponentTransferElement.h>

namespace Web::SVG {

SVGFEComponentTransferElement::SVGFEComponentTransferElement(DOM::RustMadePrefix prefix)
    : SVGElement(prefix)
{
}

void SVGFEComponentTransferElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
}

GC::Ref<SVGAnimatedString> SVGFEComponentTransferElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, {}, {} });
    return *m_in1;
}

}
