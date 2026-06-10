/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/IDLEventListener.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(IDLEventListener);

GC::Ref<IDLEventListener> IDLEventListener::create(GC::Ref<WebIDL::CallbackType> callback)
{
    return GC::Heap::the().allocate<IDLEventListener>(move(callback));
}

IDLEventListener::IDLEventListener(GC::Ref<WebIDL::CallbackType> callback)
    : m_callback(move(callback))
{
}

void IDLEventListener::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
