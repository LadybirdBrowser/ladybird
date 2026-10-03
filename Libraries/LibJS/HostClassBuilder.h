/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <LibGC/RootVector.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Value.h>

// Builds the C tables of HostObjectABI.h from C++ traits classes. A traits class implements a hook with a static member
// function of the hook's name that takes and returns engine types, and a hook it does not implement stays null. Each
// hook must be a single function, not an overload set. A class is declared in a header and defined once:
//
//     struct LocationTraits {
//         static ThrowCompletionOr<Value> get(HostObject const&, PropertyKey const&, Value receiver, CacheableGetPropertyMetadata*, Object::PropertyLookupPhase);
//     };
//
//     extern JSHostClass const location_class;
//
//     static constexpr JSHostObjectHooks location_hooks = make_host_object_hooks<LocationTraits>();
//     constexpr JSHostClass location_class = make_host_class(JS_HOST_CLASS_OBJECT, "Location"sv, nullptr, &location_hooks, nullptr, 0);

namespace JS::HostABI {

static_assert(sizeof(JSCompletion) == 16);
static_assert(offsetof(JSPropertyDescriptor, property_offset) == 24);
static_assert(offsetof(JSPropertyDescriptor, flags) == 28);
static_assert(sizeof(JSPropertyDescriptor) == 32);
static_assert(offsetof(JSHostClass, flags) == 4);
static_assert(offsetof(JSHostClass, name) == 8);
static_assert(offsetof(JSHostClass, user_data) == 40);
static_assert(sizeof(JSHostClass) == 48);

static_assert(sizeof(JSValue) == sizeof(Value));
static_assert(sizeof(JSPropertyKey) == sizeof(PropertyKey));
static_assert(alignof(JSPropertyKey) == alignof(PropertyKey));
static_assert(offsetof(JSPropertyKey, bits) == 0);
static_assert(to_underlying(Object::PropertyLookupPhase::OwnProperty) == JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY);
static_assert(to_underlying(Object::PropertyLookupPhase::PrototypeChain) == JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN);

inline JSValue value_to_abi(Value value)
{
    return value.encoded();
}

inline Value value_from_abi(JSValue value)
{
    return bit_cast<Value>(value);
}

inline JSObject* object_to_abi(Object const* object)
{
    return reinterpret_cast<JSObject*>(const_cast<Object*>(object));
}

inline Object* object_from_abi(JSObject* object)
{
    return reinterpret_cast<Object*>(object);
}

// The key is borrowed: no reference count changes hands, and the engine's key outlives the hook call.
inline JSPropertyKey property_key_to_abi(PropertyKey const& property_key)
{
    return *reinterpret_cast<JSPropertyKey const*>(&property_key);
}

inline PropertyKey const& property_key_from_abi(JSPropertyKey const& property_key)
{
    return *reinterpret_cast<PropertyKey const*>(&property_key);
}

inline JSGetCacheMetadata* get_cache_metadata_to_abi(CacheableGetPropertyMetadata* metadata)
{
    return reinterpret_cast<JSGetCacheMetadata*>(metadata);
}

inline CacheableGetPropertyMetadata* get_cache_metadata_from_abi(JSGetCacheMetadata* metadata)
{
    return reinterpret_cast<CacheableGetPropertyMetadata*>(metadata);
}

inline JSSetCacheMetadata* set_cache_metadata_to_abi(CacheableSetPropertyMetadata* metadata)
{
    return reinterpret_cast<JSSetCacheMetadata*>(metadata);
}

inline CacheableSetPropertyMetadata* set_cache_metadata_from_abi(JSSetCacheMetadata* metadata)
{
    return reinterpret_cast<CacheableSetPropertyMetadata*>(metadata);
}

inline u8 lookup_phase_to_abi(Object::PropertyLookupPhase phase)
{
    return to_underlying(phase);
}

inline Object::PropertyLookupPhase lookup_phase_from_abi(u8 phase)
{
    VERIFY(phase == JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY || phase == JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN);
    return static_cast<Object::PropertyLookupPhase>(phase);
}

inline JSPropertyDescriptor property_descriptor_to_abi(PropertyDescriptor const& descriptor)
{
    JSPropertyDescriptor abi_descriptor {};
    u16 flags = JS_PD_PRESENT;
    if (descriptor.value.has_value()) {
        flags |= JS_PD_HAS_VALUE;
        abi_descriptor.value = value_to_abi(*descriptor.value);
    }
    if (descriptor.get.has_value()) {
        flags |= JS_PD_HAS_GET;
        abi_descriptor.get = object_to_abi(descriptor.get->ptr());
    }
    if (descriptor.set.has_value()) {
        flags |= JS_PD_HAS_SET;
        abi_descriptor.set = object_to_abi(descriptor.set->ptr());
    }
    if (descriptor.writable.has_value())
        flags |= JS_PD_HAS_WRITABLE | (*descriptor.writable ? JS_PD_WRITABLE : 0);
    if (descriptor.enumerable.has_value())
        flags |= JS_PD_HAS_ENUMERABLE | (*descriptor.enumerable ? JS_PD_ENUMERABLE : 0);
    if (descriptor.configurable.has_value())
        flags |= JS_PD_HAS_CONFIGURABLE | (*descriptor.configurable ? JS_PD_CONFIGURABLE : 0);
    if (descriptor.property_offset.has_value()) {
        flags |= JS_PD_HAS_PROPERTY_OFFSET;
        abi_descriptor.property_offset = *descriptor.property_offset;
    }
    abi_descriptor.flags = flags;
    return abi_descriptor;
}

inline JSPropertyDescriptor property_descriptor_to_abi(Optional<PropertyDescriptor> const& descriptor)
{
    if (!descriptor.has_value())
        return {};
    return property_descriptor_to_abi(*descriptor);
}

// Getters and setters arrive as JSObject pointers, which the engine only ever fills from FunctionObjects.
inline GC::Ptr<FunctionObject> function_from_abi(JSObject* function)
{
    return static_cast<FunctionObject*>(object_from_abi(function));
}

inline Optional<PropertyDescriptor> property_descriptor_from_abi(JSPropertyDescriptor const& abi_descriptor)
{
    auto flags = abi_descriptor.flags;
    if (!(flags & JS_PD_PRESENT))
        return {};
    PropertyDescriptor descriptor;
    if (flags & JS_PD_HAS_VALUE)
        descriptor.value = value_from_abi(abi_descriptor.value);
    if (flags & JS_PD_HAS_GET)
        descriptor.get = function_from_abi(abi_descriptor.get);
    if (flags & JS_PD_HAS_SET)
        descriptor.set = function_from_abi(abi_descriptor.set);
    if (flags & JS_PD_HAS_WRITABLE)
        descriptor.writable = (flags & JS_PD_WRITABLE) != 0;
    if (flags & JS_PD_HAS_ENUMERABLE)
        descriptor.enumerable = (flags & JS_PD_ENUMERABLE) != 0;
    if (flags & JS_PD_HAS_CONFIGURABLE)
        descriptor.configurable = (flags & JS_PD_CONFIGURABLE) != 0;
    if (flags & JS_PD_HAS_PROPERTY_OFFSET)
        descriptor.property_offset = abi_descriptor.property_offset;
    return descriptor;
}

inline JSCompletion normal_completion_to_abi(u64 payload)
{
    return { payload, JS_COMPLETION_NORMAL };
}

inline JSCompletion throw_completion_to_abi(Value error)
{
    return { value_to_abi(error), JS_COMPLETION_THROW };
}

template<typename T>
JSCompletion completion_to_abi(ThrowCompletionOr<T> const& completion)
{
    if (completion.is_error())
        return throw_completion_to_abi(completion.error_value());
    if constexpr (IsSame<T, void>) {
        return normal_completion_to_abi(0);
    } else if constexpr (IsSame<T, bool>) {
        return normal_completion_to_abi(completion.value() ? 1 : 0);
    } else if constexpr (IsSame<T, Value>) {
        return normal_completion_to_abi(value_to_abi(completion.value()));
    } else if constexpr (IsSame<T, Object*>) {
        return normal_completion_to_abi(reinterpret_cast<FlatPtr>(object_to_abi(completion.value())));
    } else {
        static_assert(IsSame<T, GC::Ref<Object>>);
        return normal_completion_to_abi(reinterpret_cast<FlatPtr>(object_to_abi(completion.value().ptr())));
    }
}

template<typename T>
ThrowCompletionOr<T> completion_from_abi(JSCompletion completion)
{
    if (completion.variant == JS_COMPLETION_THROW)
        return Completion { Completion::Type::Throw, value_from_abi(completion.payload) };
    VERIFY(completion.variant == JS_COMPLETION_NORMAL);
    if constexpr (IsSame<T, void>) {
        return {};
    } else if constexpr (IsSame<T, bool>) {
        return completion.payload != 0;
    } else if constexpr (IsSame<T, Value>) {
        return value_from_abi(completion.payload);
    } else if constexpr (IsSame<T, Object*>) {
        return object_from_abi(reinterpret_cast<JSObject*>(static_cast<FlatPtr>(completion.payload)));
    } else {
        static_assert(IsSame<T, GC::Ref<Object>>);
        return GC::Ref { *object_from_abi(reinterpret_cast<JSObject*>(static_cast<FlatPtr>(completion.payload))) };
    }
}

template<typename Traits>
struct HostObjectHookThunks {
    static HostObject& host_object_from_abi(JSObject* object)
    {
        return static_cast<HostObject&>(*object_from_abi(object));
    }

    static JSCompletion get_prototype_of(JSObject* object)
    {
        return completion_to_abi(Traits::get_prototype_of(host_object_from_abi(object)));
    }

    static JSCompletion set_prototype_of(JSObject* object, JSObject* prototype)
    {
        return completion_to_abi(Traits::set_prototype_of(host_object_from_abi(object), object_from_abi(prototype)));
    }

    static JSCompletion is_extensible(JSObject* object)
    {
        return completion_to_abi(Traits::is_extensible(host_object_from_abi(object)));
    }

    static JSCompletion prevent_extensions(JSObject* object)
    {
        return completion_to_abi(Traits::prevent_extensions(host_object_from_abi(object)));
    }

    static JSCompletion get_own_property(JSObject* object, JSPropertyKey property_key, JSPropertyDescriptor* out)
    {
        auto result = Traits::get_own_property(host_object_from_abi(object), property_key_from_abi(property_key));
        if (result.is_error())
            return throw_completion_to_abi(result.error_value());
        *out = property_descriptor_to_abi(result.value());
        return normal_completion_to_abi(0);
    }

    static JSCompletion define_own_property(JSObject* object, JSPropertyKey property_key, JSPropertyDescriptor* abi_descriptor, JSPropertyDescriptor const* abi_precomputed_get_own_property)
    {
        auto descriptor = property_descriptor_from_abi(*abi_descriptor).release_value();
        Optional<PropertyDescriptor> precomputed_get_own_property;
        if (abi_precomputed_get_own_property)
            precomputed_get_own_property = property_descriptor_from_abi(*abi_precomputed_get_own_property);
        auto result = Traits::define_own_property(host_object_from_abi(object), property_key_from_abi(property_key), descriptor, abi_precomputed_get_own_property ? &precomputed_get_own_property : nullptr);
        *abi_descriptor = property_descriptor_to_abi(descriptor);
        return completion_to_abi(result);
    }

    static JSCompletion has_property(JSObject* object, JSPropertyKey property_key)
    {
        return completion_to_abi(Traits::has_property(host_object_from_abi(object), property_key_from_abi(property_key)));
    }

    static JSCompletion get(JSObject* object, JSPropertyKey property_key, JSValue receiver, JSGetCacheMetadata* metadata, u8 phase)
    {
        return completion_to_abi(Traits::get(host_object_from_abi(object), property_key_from_abi(property_key), value_from_abi(receiver), get_cache_metadata_from_abi(metadata), lookup_phase_from_abi(phase)));
    }

    static JSCompletion set(JSObject* object, JSPropertyKey property_key, JSValue value, JSValue receiver, JSSetCacheMetadata* metadata, u8 phase)
    {
        return completion_to_abi(Traits::set(host_object_from_abi(object), property_key_from_abi(property_key), value_from_abi(value), value_from_abi(receiver), set_cache_metadata_from_abi(metadata), lookup_phase_from_abi(phase)));
    }

    static JSCompletion delete_property(JSObject* object, JSPropertyKey property_key)
    {
        return completion_to_abi(Traits::delete_property(host_object_from_abi(object), property_key_from_abi(property_key)));
    }

    static JSCompletion own_property_keys(JSObject* object, JSValueSink* keys)
    {
        auto result = Traits::own_property_keys(host_object_from_abi(object));
        if (result.is_error())
            return throw_completion_to_abi(result.error_value());
        for (auto key : result.value())
            keys->append(keys->context, value_to_abi(key));
        return normal_completion_to_abi(0);
    }

    static bool is_cacheable_for_inherited_property(JSObject* object)
    {
        return Traits::is_cacheable_for_inherited_property(host_object_from_abi(object));
    }

    static void* error_data(JSObject* object)
    {
        return Traits::error_data(host_object_from_abi(object));
    }

    static void finalize(JSObject* object)
    {
        Traits::finalize(host_object_from_abi(object));
    }
};

}

namespace JS {

template<typename Traits>
consteval JSHostObjectHooks make_host_object_hooks()
{
    using Thunks = HostABI::HostObjectHookThunks<Traits>;
    JSHostObjectHooks hooks {};
    if constexpr (requires { &Traits::get_prototype_of; })
        hooks.get_prototype_of = &Thunks::get_prototype_of;
    if constexpr (requires { &Traits::set_prototype_of; })
        hooks.set_prototype_of = &Thunks::set_prototype_of;
    if constexpr (requires { &Traits::is_extensible; })
        hooks.is_extensible = &Thunks::is_extensible;
    if constexpr (requires { &Traits::prevent_extensions; })
        hooks.prevent_extensions = &Thunks::prevent_extensions;
    if constexpr (requires { &Traits::get_own_property; })
        hooks.get_own_property = &Thunks::get_own_property;
    if constexpr (requires { &Traits::define_own_property; })
        hooks.define_own_property = &Thunks::define_own_property;
    if constexpr (requires { &Traits::has_property; })
        hooks.has_property = &Thunks::has_property;
    if constexpr (requires { &Traits::get; })
        hooks.get = &Thunks::get;
    if constexpr (requires { &Traits::set; })
        hooks.set = &Thunks::set;
    if constexpr (requires { &Traits::delete_property; })
        hooks.delete_property = &Thunks::delete_property;
    if constexpr (requires { &Traits::own_property_keys; })
        hooks.own_property_keys = &Thunks::own_property_keys;
    if constexpr (requires { &Traits::is_cacheable_for_inherited_property; })
        hooks.is_cacheable_for_inherited_property = &Thunks::is_cacheable_for_inherited_property;
    if constexpr (requires { &Traits::error_data; })
        hooks.error_data = &Thunks::error_data;
    if constexpr (requires { &Traits::finalize; })
        hooks.finalize = &Thunks::finalize;
    return hooks;
}

consteval JSHostClass make_host_class(u8 kind, StringView name, JSHostClass const* parent, void const* hooks, void const* user_data, u32 flags)
{
    constexpr u32 flags_copied_into_objects = JS_HOST_CLASS_IS_PLATFORM_OBJECT
        | JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY
        | JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS
        | JS_HOST_CLASS_IS_HTMLDDA
        | JS_HOST_CLASS_IS_GLOBAL_OBJECT;
    constexpr u32 host_object_flags = JS_HOST_CLASS_IMMUTABLE_PROTOTYPE
        | JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE
        | JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH;

    u32 allowed_flags = 0;
    switch (kind) {
    case JS_HOST_CLASS_OBJECT:
        allowed_flags = flags_copied_into_objects | host_object_flags;
        break;
    case JS_HOST_CLASS_FUNCTION:
        allowed_flags = flags_copied_into_objects | JS_HOST_CLASS_HAS_CONSTRUCTOR;
        break;
    case JS_HOST_CLASS_ARRAY:
        allowed_flags = flags_copied_into_objects;
        break;
    case JS_HOST_CLASS_MODULE:
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    VERIFY(!(flags & ~allowed_flags));

    return JSHostClass {
        .abi_version = JS_HOST_ABI_VERSION,
        .kind = kind,
        .reserved = 0,
        .flags = flags,
        .name = name.characters_without_null_termination(),
        .name_length = name.length(),
        .parent = parent,
        .hooks = hooks,
        .user_data = user_data,
    };
}

}
