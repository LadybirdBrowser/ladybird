/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedTransformList);

// https://svgwg.org/svg2-draft/coords.html#InterfaceSVGAnimatedTransformList
GC::Ref<SVGAnimatedTransformList> SVGAnimatedTransformList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute)
{
    // On getting baseVal or animVal, an SVGTransformList object is returned that reflects the given presentation
    // attribute.
    // NB: List interface objects reflected through the animVal IDL attribute are always read only.
    auto base_val = SVGTransformList::create(element, reflected_attribute, ReadOnlyList::No);
    auto anim_val = SVGTransformList::create(element, move(reflected_attribute), ReadOnlyList::Yes);
    return GC::Heap::the().allocate<SVGAnimatedTransformList>(base_val, anim_val);
}

SVGAnimatedTransformList::SVGAnimatedTransformList(GC::Ref<SVGTransformList> base_val, GC::Ref<SVGTransformList> anim_val)
    : m_base_val(base_val)
    , m_anim_val(anim_val)
{
}

void SVGAnimatedTransformList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

void SVGAnimatedTransformList::synchronize()
{
    m_base_val->synchronize();
    m_anim_val->synchronize();
}

}
