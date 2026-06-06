/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/NodeFilter.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NodeFilter);

GC::Ref<NodeFilter> NodeFilter::create(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback)
{
    return realm.create<NodeFilter>(realm, callback);
}

NodeFilter::NodeFilter(JS::Realm&, GC::Ref<WebIDL::CallbackType> callback)
    : m_callback(callback)
{
}

void NodeFilter::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
