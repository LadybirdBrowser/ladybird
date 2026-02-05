/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/DOM/IDLEventListener.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(IDLEventListener);

GC::Ref<IDLEventListener> IDLEventListener::create(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback)
{
    return realm.create<IDLEventListener>(realm, move(callback));
}

IDLEventListener::IDLEventListener(JS::Realm& realm, GC::Ref<WebIDL::CallbackType> callback)
    : JS::Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
    , m_callback(move(callback))
{
}

void IDLEventListener::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
