/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostArray.h>
#include <LibJS/Runtime/HostClassInternals.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

using namespace HostABI;

GC::Ref<HostArray> HostArray::create(Realm& realm, JSHostClass const& host_class, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> host_data)
{
    if (!prototype)
        prototype = realm.intrinsics().array_prototype();
    auto array = realm.heap().allocate_with_descriptor(cell_allocator_for_host_class<HostArray>(host_class), realm, host_class, *prototype, host_data);
    static_cast<Cell&>(*array).initialize(realm);
    return array;
}

HostArray::HostArray(Realm& realm, JSHostClass const& host_class, Object& prototype, GC::Ptr<GC::Cell> host_data)
    : Array(realm, prototype)
    , m_host_class(&host_class)
    , m_host_data(host_data)
{
    VERIFY(host_class.abi_version == JS_HOST_ABI_VERSION && host_class.kind == JS_HOST_CLASS_ARRAY);
    copy_host_class_flags_into_object(host_class, *this);
}

void HostArray::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host_data);
}

JSHostArrayHooks const& HostArray::hooks() const
{
    static constexpr JSHostArrayHooks array_hooks {};
    if (!m_host_class->hooks)
        return array_hooks;
    return *static_cast<JSHostArrayHooks const*>(m_host_class->hooks);
}

StringView HostArray::class_name() const
{
    return { m_host_class->name, m_host_class->name_length };
}

ThrowCompletionOr<bool> HostArray::internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* metadata, PropertyLookupPhase phase)
{
    auto hook = hooks().set;
    if (!hook)
        return array_set(property_key, value, receiver, metadata, phase);
    return completion_from_abi<bool>(hook(object_to_abi(this), property_key_to_abi(property_key), value_to_abi(value), value_to_abi(receiver), set_cache_metadata_to_abi(metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> HostArray::internal_delete(PropertyKey const& property_key)
{
    auto hook = hooks().delete_property;
    if (!hook)
        return array_delete(property_key);
    return completion_from_abi<bool>(hook(object_to_abi(this), property_key_to_abi(property_key)));
}

ThrowCompletionOr<bool> HostArray::array_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* metadata, PropertyLookupPhase phase)
{
    return Array::internal_set(property_key, value, receiver, metadata, phase);
}

ThrowCompletionOr<bool> HostArray::array_delete(PropertyKey const& property_key)
{
    return Array::internal_delete(property_key);
}

}
