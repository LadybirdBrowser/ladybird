/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>

#include "XPathNSResolver.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathNSResolver);

GC::Ref<XPathNSResolver> XPathNSResolver::create(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback)
{
    return realm.create<XPathNSResolver>(move(callback));
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
