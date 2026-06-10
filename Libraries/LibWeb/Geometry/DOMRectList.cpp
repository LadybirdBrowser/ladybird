/*
 * Copyright (c) 2022, DerpyCrabs <derpycrabs@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRectList);

GC::Ref<DOMRectList> DOMRectList::create(Vector<GC::Root<DOMRect>> rect_handles)
{
    Vector<GC::Ref<DOMRect>> rects;
    for (auto& rect : rect_handles)
        rects.append(*rect);
    return GC::Heap::the().allocate<DOMRectList>(move(rects));
}

DOMRectList::DOMRectList(Vector<GC::Ref<DOMRect>> rects)
    : m_rects(move(rects))
{
}

DOMRectList::~DOMRectList() = default;

void DOMRectList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rects);
}

// https://drafts.fxtf.org/geometry-1/#dom-domrectlist-length
u32 DOMRectList::length() const
{
    return m_rects.size();
}

// https://drafts.fxtf.org/geometry-1/#dom-domrectlist-item
DOMRect const* DOMRectList::item(u32 index) const
{
    // The item(index) method, when invoked, must return null when
    // index is greater than or equal to the number of DOMRect objects associated with the DOMRectList.
    // Otherwise, the DOMRect object at index must be returned. Indices are zero-based.
    if (index >= m_rects.size())
        return nullptr;
    return m_rects[index];
}

}
