/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(JS_LAYOUT_HOST_OBJECT_HOST_CLASS_SIZE == sizeof(JSHostClass const*));
static_assert(JS_LAYOUT_HOST_OBJECT_WRAPPABLE_SIZE == sizeof(GC::Cell*));
static_assert(JS_LAYOUT_HOST_OBJECT_HOST_DATA_SIZE == sizeof(GC::Cell*));

GC::Ref<HostObject> HostObject::create(Realm& realm, JSHostClass const& host_class, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> wrappable, GC::Ptr<GC::Cell> host_data)
{
    auto* object = js_host_object_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), &host_class, optional_object_to_abi(prototype.ptr()), wrappable.ptr(), host_data.ptr());
    return static_cast<HostObject&>(object_from_abi(object));
}

void HostObject::set_host_data(GC::Ptr<GC::Cell> host_data)
{
    js_host_object_set_host_data(object_to_abi(*this), host_data.ptr());
}

bool is_host_instance_of(Object const& object, JSHostClass const& host_class)
{
    return js_host_object_is_host_instance_of(object_to_abi(object), &host_class);
}

GC::Ptr<GC::Cell> host_data_of(Object const& object)
{
    return static_cast<GC::Cell*>(js_host_object_host_data_of(object_to_abi(object)));
}

}
