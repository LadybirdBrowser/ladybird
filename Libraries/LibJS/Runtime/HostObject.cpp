/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibJS/Runtime/HostArray.h>
#include <LibJS/Runtime/HostClassInternals.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

using namespace HostABI;

GC::Ref<HostObject> HostObject::create(Realm& realm, JSHostClass const& host_class, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> wrappable, GC::Ptr<GC::Cell> host_data)
{
    auto object = realm.heap().allocate_with_descriptor(cell_allocator_for_host_class<HostObject>(host_class), realm, host_class, prototype, wrappable, host_data);
    static_cast<Cell&>(*object).initialize(realm);
    return object;
}

HostObject::HostObject(Realm& realm, JSHostClass const& host_class, GC::Ptr<Object> prototype, GC::Ptr<GC::Cell> wrappable, GC::Ptr<GC::Cell> host_data)
    : Object(realm, prototype)
    , m_host_class(&host_class)
    , m_wrappable(wrappable)
    , m_host_data(host_data)
{
#if !defined(AK_OS_WINDOWS)
    static_assert(sizeof(Object) == JS_HOST_OBJECT_HOST_CLASS_OFFSET);
    static_assert(offsetof(HostObject, m_host_class) == JS_HOST_OBJECT_HOST_CLASS_OFFSET);
    static_assert(offsetof(HostObject, m_wrappable) == JS_HOST_OBJECT_WRAPPABLE_OFFSET);
    static_assert(offsetof(HostObject, m_host_data) == JS_HOST_OBJECT_HOST_DATA_OFFSET);
    static_assert(sizeof(HostObject) == JS_HOST_OBJECT_SIZE);
#endif

    VERIFY(host_class.abi_version == JS_HOST_ABI_VERSION && host_class.kind == JS_HOST_CLASS_OBJECT);
    copy_host_class_flags_into_object(host_class, *this);
}

void HostObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_wrappable);
    visitor.visit(m_host_data);
}

void HostObject::finalize()
{
    Base::finalize();
    if (auto finalize_hook = hooks().finalize)
        finalize_hook(as_abi_object());
}

JSHostObjectHooks const& HostObject::hooks() const
{
    static constexpr JSHostObjectHooks ordinary_hooks {};
    if (!m_host_class->hooks)
        return ordinary_hooks;
    return *static_cast<JSHostObjectHooks const*>(m_host_class->hooks);
}

JSObject* HostObject::as_abi_object() const
{
    return object_to_abi(this);
}

StringView HostObject::class_name() const
{
    return { m_host_class->name, m_host_class->name_length };
}

ThrowCompletionOr<Object*> HostObject::internal_get_prototype_of() const
{
    auto hook = hooks().get_prototype_of;
    if (!hook)
        return Object::internal_get_prototype_of();
    return completion_from_abi<Object*>(hook(as_abi_object()));
}

ThrowCompletionOr<bool> HostObject::internal_set_prototype_of(Object* prototype)
{
    auto hook = hooks().set_prototype_of;
    if (!hook) {
        if (m_host_class->flags & JS_HOST_CLASS_IMMUTABLE_PROTOTYPE)
            return set_immutable_prototype(prototype);
        return Object::internal_set_prototype_of(prototype);
    }
    return completion_from_abi<bool>(hook(as_abi_object(), object_to_abi(prototype)));
}

ThrowCompletionOr<bool> HostObject::internal_is_extensible() const
{
    auto hook = hooks().is_extensible;
    if (!hook)
        return Object::internal_is_extensible();
    return completion_from_abi<bool>(hook(as_abi_object()));
}

ThrowCompletionOr<bool> HostObject::internal_prevent_extensions()
{
    auto hook = hooks().prevent_extensions;
    if (!hook)
        return Object::internal_prevent_extensions();
    return completion_from_abi<bool>(hook(as_abi_object()));
}

ThrowCompletionOr<Optional<PropertyDescriptor>> HostObject::internal_get_own_property(PropertyKey const& property_key) const
{
    auto hook = hooks().get_own_property;
    if (!hook)
        return Object::internal_get_own_property(property_key);
    JSPropertyDescriptor descriptor {};
    TRY(completion_from_abi<void>(hook(as_abi_object(), property_key_to_abi(property_key), &descriptor)));
    return property_descriptor_from_abi(descriptor);
}

ThrowCompletionOr<bool> HostObject::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor& descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    auto hook = hooks().define_own_property;
    if (!hook)
        return Object::internal_define_own_property(property_key, descriptor, precomputed_get_own_property);

    auto abi_descriptor = property_descriptor_to_abi(descriptor);
    JSPropertyDescriptor abi_precomputed_get_own_property {};
    if (precomputed_get_own_property)
        abi_precomputed_get_own_property = property_descriptor_to_abi(*precomputed_get_own_property);
    auto completion = hook(as_abi_object(), property_key_to_abi(property_key), &abi_descriptor, precomputed_get_own_property ? &abi_precomputed_get_own_property : nullptr);
    if (auto descriptor_written_back_by_hook = property_descriptor_from_abi(abi_descriptor); descriptor_written_back_by_hook.has_value())
        descriptor = descriptor_written_back_by_hook.release_value();
    return completion_from_abi<bool>(completion);
}

ThrowCompletionOr<bool> HostObject::internal_has_property(PropertyKey const& property_key) const
{
    auto hook = hooks().has_property;
    if (!hook)
        return Object::internal_has_property(property_key);
    return completion_from_abi<bool>(hook(as_abi_object(), property_key_to_abi(property_key)));
}

ThrowCompletionOr<Value> HostObject::internal_get(PropertyKey const& property_key, Value receiver, CacheableGetPropertyMetadata* metadata, PropertyLookupPhase phase) const
{
    auto hook = hooks().get;
    if (!hook)
        return Object::internal_get(property_key, receiver, metadata, phase);
    return completion_from_abi<Value>(hook(as_abi_object(), property_key_to_abi(property_key), value_to_abi(receiver), get_cache_metadata_to_abi(metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> HostObject::internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* metadata, PropertyLookupPhase phase)
{
    auto hook = hooks().set;
    if (!hook)
        return Object::internal_set(property_key, value, receiver, metadata, phase);
    return completion_from_abi<bool>(hook(as_abi_object(), property_key_to_abi(property_key), value_to_abi(value), value_to_abi(receiver), set_cache_metadata_to_abi(metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> HostObject::internal_delete(PropertyKey const& property_key)
{
    auto hook = hooks().delete_property;
    if (!hook)
        return Object::internal_delete(property_key);
    return completion_from_abi<bool>(hook(as_abi_object(), property_key_to_abi(property_key)));
}

ThrowCompletionOr<GC::RootVector<Value>> HostObject::internal_own_property_keys() const
{
    auto hook = hooks().own_property_keys;
    if (!hook)
        return Object::internal_own_property_keys();

    GC::RootVector<Value> keys;
    JSValueSink sink {
        .context = &keys,
        .append = [](void* context, JSValue key) { static_cast<GC::RootVector<Value>*>(context)->append(value_from_abi(key)); },
    };
    TRY(completion_from_abi<void>(hook(as_abi_object(), &sink)));
    return keys;
}

bool HostObject::is_cacheable_for_property_absence() const
{
    return !(m_host_class->flags & JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE);
}

bool HostObject::is_cacheable_for_inherited_property() const
{
    auto hook = hooks().is_cacheable_for_inherited_property;
    if (!hook)
        return Object::is_cacheable_for_inherited_property();
    return hook(as_abi_object());
}

bool HostObject::eligible_for_own_property_enumeration_fast_path() const
{
    if (m_host_class->flags & JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH)
        return false;
    // The fast paths read keys, attributes and values straight from the shape and storage and follow the shape's
    // prototype, so they would bypass these hooks.
    auto const& hooks = this->hooks();
    return !hooks.get_prototype_of
        && !hooks.get_own_property
        && !hooks.has_property
        && !hooks.get
        && !hooks.own_property_keys;
}

ErrorData* HostObject::error_data()
{
    auto hook = hooks().error_data;
    if (!hook)
        return nullptr;
    return static_cast<ErrorData*>(hook(as_abi_object()));
}

ErrorData const* HostObject::error_data() const
{
    return const_cast<HostObject&>(*this).error_data();
}

bool is_host_instance_of(Object const& object, JSHostClass const& host_class)
{
    for (auto const* candidate = host_class_of(object); candidate; candidate = candidate->parent) {
        if (candidate == &host_class)
            return true;
    }
    return false;
}

GC::Ptr<GC::Cell> host_data_of(Object const& object)
{
    auto const* host_class = host_class_of(object);
    if (!host_class)
        return nullptr;
    switch (host_class->kind) {
    case JS_HOST_CLASS_OBJECT:
        return static_cast<HostObject const&>(object).host_data();
    case JS_HOST_CLASS_FUNCTION:
        return static_cast<HostFunction const&>(object).host_data();
    case JS_HOST_CLASS_ARRAY:
        return static_cast<HostArray const&>(object).host_data();
    default:
        VERIFY_NOT_REACHED();
    }
}

}
