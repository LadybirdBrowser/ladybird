/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/NodeFilter.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NodeFilter);

GC::Ref<NodeFilter> NodeFilter::create(GC::Ref<WebIDL::CallbackType> callback)
{
    return GC::Heap::the().allocate<NodeFilter>(callback);
}

NodeFilter::NodeFilter(GC::Ref<WebIDL::CallbackType> callback)
    : m_callback(callback)
{
}

void NodeFilter::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
