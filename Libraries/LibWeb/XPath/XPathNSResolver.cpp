/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>

#include "XPathNSResolver.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathNSResolver);

GC::Ref<XPathNSResolver> XPathNSResolver::create(GC::Ref<WebIDL::CallbackType> callback)
{
    return GC::Heap::the().allocate<XPathNSResolver>(move(callback));
}

XPathNSResolver::XPathNSResolver(GC::Ref<WebIDL::CallbackType> callback)
    : m_callback(move(callback))
{
}

void XPathNSResolver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
