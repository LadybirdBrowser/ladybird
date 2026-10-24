/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedLengthList);

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedLengthList
GC::Ref<SVGAnimatedLengthList> SVGAnimatedLengthList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, SVGLength::Directionality directionality)
{
    // On getting baseVal or animVal, an SVGLengthList object is returned that reflects the base value of the reflected
    // attribute.
    // NB: List interface objects reflected through the animVal IDL attribute are always read only.
    auto base_val = SVGLengthList::create(element, reflected_attribute, directionality, ReadOnlyList::No);
    auto anim_val = SVGLengthList::create(element, move(reflected_attribute), directionality, ReadOnlyList::Yes);
    return GC::Heap::the().allocate<SVGAnimatedLengthList>(base_val, anim_val);
}

SVGAnimatedLengthList::SVGAnimatedLengthList(GC::Ref<SVGLengthList> base_val, GC::Ref<SVGLengthList> anim_val)
    : m_base_val(base_val)
    , m_anim_val(anim_val)
{
}

void SVGAnimatedLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

void SVGAnimatedLengthList::synchronize()
{
    m_base_val->synchronize();
    m_anim_val->synchronize();
}

}
