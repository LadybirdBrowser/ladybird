/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLengthList);

GC::Ref<SVGLengthList> SVGLengthList::create(Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGLengthList>(move(items), read_only);
}

GC::Ref<SVGLengthList> SVGLengthList::create(ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGLengthList>(read_only);
}

SVGLengthList::SVGLengthList(Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
    : SVGList(move(items), read_only)
{
}

SVGLengthList::SVGLengthList(ReadOnlyList read_only)
    : SVGList(read_only)
{
}

void SVGLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}
