/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedNumberList);

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedNumberList
GC::Ref<SVGAnimatedNumberList> SVGAnimatedNumberList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute)
{
    // On getting baseVal or animVal, an SVGNumberList object is returned that reflects the base value of the reflected
    // attribute.
    // NB: List interface objects reflected through the animVal IDL attribute are always read only.
    auto base_val = SVGNumberList::create(element, reflected_attribute, ReadOnlyList::No);
    auto anim_val = SVGNumberList::create(element, move(reflected_attribute), ReadOnlyList::Yes);
    return GC::Heap::the().allocate<SVGAnimatedNumberList>(base_val, anim_val);
}

SVGAnimatedNumberList::SVGAnimatedNumberList(GC::Ref<SVGNumberList> base_val, GC::Ref<SVGNumberList> anim_val)
    : m_base_val(base_val)
    , m_anim_val(anim_val)
{
}

void SVGAnimatedNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

void SVGAnimatedNumberList::synchronize()
{
    m_base_val->synchronize();
    m_anim_val->synchronize();
}

}
