/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/CallbackType.h>

#include "XPathNSResolver.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathNSResolver);

GC::Ref<XPathNSResolver> XPathNSResolver::create(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback)
{
    return realm.create<XPathNSResolver>(realm, callback);
}

XPathNSResolver::XPathNSResolver(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback)
    : JS::Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
    , m_callback(callback)
{
}

void XPathNSResolver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
