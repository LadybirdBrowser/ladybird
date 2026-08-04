/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumberList);

GC::Ref<SVGNumberList> SVGNumberList::create(Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGNumberList>(move(items), read_only);
}

GC::Ref<SVGNumberList> SVGNumberList::create(ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGNumberList>(read_only);
}

SVGNumberList::SVGNumberList(Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
    : SVGList(move(items), read_only)
{
}

SVGNumberList::SVGNumberList(ReadOnlyList read_only)
    : SVGList(read_only)
{
}

void SVGNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
