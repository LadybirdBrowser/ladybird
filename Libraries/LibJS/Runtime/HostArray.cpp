/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/HostArray.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<HostArray> HostArray::create(Realm& realm, JSHostClass const& host_class, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> host_data)
{
    auto* array = js_host_array_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), &host_class, optional_object_to_abi(prototype.ptr()), host_data.ptr());
    return static_cast<HostArray&>(object_from_abi(array));
}

JSHostClass const& HostArray::host_class() const
{
    auto const* host_class = host_class_of(*this);
    VERIFY(host_class);
    return *host_class;
}

GC::Ptr<GC::Cell> HostArray::host_data() const
{
    return host_data_of(*this);
}

void HostArray::set_host_data(GC::Ptr<GC::Cell> host_data)
{
    js_host_object_set_host_data(object_to_abi(*this), host_data.ptr());
}

ThrowCompletionOr<bool> HostArray::array_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase)
{
    return completion_from_abi<bool>(js_host_array_array_set(vm_to_abi(vm()), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value), value_to_abi(receiver), set_cache_metadata_to_abi(cacheable_metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> HostArray::array_delete(PropertyKey const& property_key)
{
    return completion_from_abi<bool>(js_host_array_array_delete(vm_to_abi(vm()), object_to_abi(*this), property_key_to_abi(property_key)));
}

}
