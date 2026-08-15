/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/SVG/SVGFEFloodElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFloodElement);

SVGFEFloodElement::SVGFEFloodElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEFloodElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
}

// https://www.w3.org/TR/filter-effects-1/#FloodColorProperty
Gfx::Color SVGFEFloodElement::flood_color()
{
    auto style = computed_style();
    VERIFY(style);
    return style->flood_color();
}

// https://www.w3.org/TR/filter-effects-1/#FloodOpacityProperty
float SVGFEFloodElement::flood_opacity() const
{
    auto style = computed_style();
    VERIFY(style);
    return style->flood_opacity();
}

}
