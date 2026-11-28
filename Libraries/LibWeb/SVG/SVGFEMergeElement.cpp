/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGFEMergeElement.h>

namespace Web::SVG {

SVGFEMergeElement::SVGFEMergeElement(DOM::RustMadePrefix prefix)
    : SVGElement(prefix)
{
}

void SVGFEMergeElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
}

}
