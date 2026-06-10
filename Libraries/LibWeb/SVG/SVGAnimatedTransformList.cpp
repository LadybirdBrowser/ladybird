/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedTransformList);

GC::Ref<SVGAnimatedTransformList> SVGAnimatedTransformList::create(GC::Ref<SVGTransformList> base_val, GC::Ref<SVGTransformList> anim_val)
{
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

}
