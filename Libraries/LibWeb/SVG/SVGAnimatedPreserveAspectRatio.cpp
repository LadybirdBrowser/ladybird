/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedPreserveAspectRatio.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedPreserveAspectRatio);

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGAnimatedPreserveAspectRatio__baseVal
GC::Ref<SVGAnimatedPreserveAspectRatio> SVGAnimatedPreserveAspectRatio::create(GC::Ref<SVGElement> element)
{
    // On getting baseVal or animVal, an SVGPreserveAspectRatio object is returned that reflects the base value of the
    // ‘preserveAspectRatio’ attribute on the SVG element that the object with the reflecting IDL attribute of type
    // SVGAnimatedPreserveAspectRatio was obtained from.
    auto base_val = SVGPreserveAspectRatio::create(element, SVGPreserveAspectRatio::ReadOnly::No);
    auto anim_val = SVGPreserveAspectRatio::create(element, SVGPreserveAspectRatio::ReadOnly::Yes);
    return GC::Heap::the().allocate<SVGAnimatedPreserveAspectRatio>(base_val, anim_val);
}

SVGAnimatedPreserveAspectRatio::SVGAnimatedPreserveAspectRatio(GC::Ref<SVGPreserveAspectRatio> base_val, GC::Ref<SVGPreserveAspectRatio> anim_val)
    : m_base_val(base_val)
    , m_anim_val(anim_val)
{
}

SVGAnimatedPreserveAspectRatio::~SVGAnimatedPreserveAspectRatio() = default;

void SVGAnimatedPreserveAspectRatio::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

}
