/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/UIEvents/TouchList.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(TouchList);

GC::Ref<TouchList> TouchList::create(GC::ConservativeVector<GC::Ref<Touch>> touches)
{
    return GC::Heap::the().allocate<TouchList>(move(touches));
}

TouchList::TouchList(GC::ConservativeVector<GC::Ref<Touch>> touches)
    : m_touches(move(touches))
{
}

void TouchList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_touches);
}

}
