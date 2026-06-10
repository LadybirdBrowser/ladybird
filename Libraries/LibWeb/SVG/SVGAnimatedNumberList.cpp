/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedNumberList);

GC::Ref<SVGAnimatedNumberList> SVGAnimatedNumberList::create(GC::Ref<SVGNumberList> base_val)
{
    return GC::Heap::the().allocate<SVGAnimatedNumberList>(base_val);
}

SVGAnimatedNumberList::SVGAnimatedNumberList(GC::Ref<SVGNumberList> base_val)
    : m_base_val(base_val)
{
}

void SVGAnimatedNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
}

}
