/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<HostFunction> HostFunction::create(Realm& realm, JSHostClass const& host_class, Utf16FlyString name, i32 length, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> host_data)
{
    auto* function = js_host_function_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), &host_class, utf16_view_to_abi(name.view()), length, optional_object_to_abi(prototype.ptr()), host_data.ptr());
    return static_cast<HostFunction&>(object_from_abi(function));
}

GC::Ref<HostFunction> HostFunction::create_without_own_properties(Realm& realm, JSHostClass const& host_class, Utf16FlyString name, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> host_data)
{
    auto* function = js_host_function_create_without_own_properties(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), &host_class, utf16_view_to_abi(name.view()), optional_object_to_abi(prototype.ptr()), host_data.ptr());
    return static_cast<HostFunction&>(object_from_abi(function));
}

JSHostClass const& HostFunction::host_class() const
{
    auto const* host_class = host_class_of(*this);
    VERIFY(host_class);
    return *host_class;
}

GC::Ptr<GC::Cell> HostFunction::host_data() const
{
    return host_data_of(*this);
}

void HostFunction::set_host_data(GC::Ptr<GC::Cell> host_data)
{
    js_host_object_set_host_data(object_to_abi(*this), host_data.ptr());
}

}
