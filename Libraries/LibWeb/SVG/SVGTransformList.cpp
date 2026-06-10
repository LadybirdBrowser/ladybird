/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGTransform.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTransformList);

GC::Ref<SVGTransformList> SVGTransformList::create(Vector<GC::Ref<SVGTransform>> items, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGTransformList>(move(items), read_only);
}

GC::Ref<SVGTransformList> SVGTransformList::create(ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGTransformList>(read_only);
}

SVGTransformList::SVGTransformList(Vector<GC::Ref<SVGTransform>> items, ReadOnlyList read_only)
    : SVGList(move(items), read_only)
{
}

SVGTransformList::SVGTransformList(ReadOnlyList read_only)
    : SVGList(read_only)
{
}

void SVGTransformList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
