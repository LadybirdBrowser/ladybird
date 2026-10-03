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
#include <LibJS/Runtime/HostArray.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/HostModule.h>
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
static_assert(static_cast<int>(ResolvedBinding::BindingName) == JS_RESOLVED_BINDING_BINDING_NAME);
static_assert(static_cast<int>(ResolvedBinding::Namespace) == JS_RESOLVED_BINDING_NAMESPACE);
static_assert(static_cast<int>(ResolvedBinding::Ambiguous) == JS_RESOLVED_BINDING_AMBIGUOUS);
static_assert(static_cast<int>(ResolvedBinding::Null) == JS_RESOLVED_BINDING_NULL);

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

inline JSVM* vm_to_abi(VM& vm)
{
    return reinterpret_cast<JSVM*>(&vm);
}

inline VM& vm_from_abi(JSVM* vm)
{
    return *reinterpret_cast<VM*>(vm);
}

inline JSModule* module_to_abi(Module const* module)
{
    return reinterpret_cast<JSModule*>(const_cast<Module*>(module));
}

inline Module* module_from_abi(JSModule* module)
{
    return reinterpret_cast<Module*>(module);
}

inline JSPromiseCapability* promise_capability_to_abi(GC::Ptr<PromiseCapability> capability)
{
    return reinterpret_cast<JSPromiseCapability*>(capability.ptr());
}

inline GC::Ptr<PromiseCapability> promise_capability_from_abi(JSPromiseCapability* capability)
{
    return reinterpret_cast<PromiseCapability*>(capability);
}

inline u8 resolved_binding_type_to_abi(ResolvedBinding::Type type)
{
    return static_cast<u8>(type);
}

inline ResolvedBinding::Type resolved_binding_type_from_abi(u8 type)
{
    VERIFY(type <= JS_RESOLVED_BINDING_NULL);
    return static_cast<ResolvedBinding::Type>(type);
}

inline Utf16FlyString string_from_abi(u16 const* code_units, size_t length_in_code_units)
{
    return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length_in_code_units });
}

// Strings cross the ABI as 16-bit code units, which an ASCII string only has once widened.
template<typename Callback>
void with_string_as_abi(Utf16FlyString const& string, Callback callback)
{
    auto view = string.view();
    if (!view.has_ascii_storage()) {
        auto code_units = view.utf16_span();
        callback(reinterpret_cast<u16 const*>(code_units.data()), code_units.size());
        return;
    }
    Vector<u16, 64> widened_code_units;
    widened_code_units.ensure_capacity(view.length_in_code_units());
    for (auto ascii_character : view.ascii_span())
        widened_code_units.unchecked_append(static_cast<u8>(ascii_character));
    callback(widened_code_units.data(), widened_code_units.size());
}

inline void append_string_to_abi_sink(JSStringSink& sink, Utf16FlyString const& string)
{
    with_string_as_abi(string, [&](u16 const* code_units, size_t length_in_code_units) {
        sink.append(sink.context, code_units, length_in_code_units);
    });
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

template<typename Traits>
struct HostFunctionHookThunks {
    static HostFunction& host_function_from_abi(JSObject* function)
    {
        return static_cast<HostFunction&>(*object_from_abi(function));
    }

    static JSCompletion call(JSObject* function, JSVM* vm)
    {
        return completion_to_abi(Traits::call(host_function_from_abi(function), vm_from_abi(vm)));
    }

    static JSCompletion construct(JSObject* function, JSVM* vm, JSObject* new_target)
    {
        return completion_to_abi(Traits::construct(host_function_from_abi(function), vm_from_abi(vm), static_cast<FunctionObject&>(*object_from_abi(new_target))));
    }

    static void finalize(JSObject* function)
    {
        Traits::finalize(host_function_from_abi(function));
    }
};

template<typename Traits>
struct HostArrayHookThunks {
    static HostArray& host_array_from_abi(JSObject* array)
    {
        return static_cast<HostArray&>(*object_from_abi(array));
    }

    static JSCompletion set(JSObject* array, JSPropertyKey property_key, JSValue value, JSValue receiver, JSSetCacheMetadata* metadata, u8 phase)
    {
        return completion_to_abi(Traits::set(host_array_from_abi(array), property_key_from_abi(property_key), value_from_abi(value), value_from_abi(receiver), set_cache_metadata_from_abi(metadata), lookup_phase_from_abi(phase)));
    }

    static JSCompletion delete_property(JSObject* array, JSPropertyKey property_key)
    {
        return completion_to_abi(Traits::delete_property(host_array_from_abi(array), property_key_from_abi(property_key)));
    }
};

template<typename Traits>
struct HostModuleHookThunks {
    static HostModule& host_module_from_abi(JSModule* module)
    {
        return static_cast<HostModule&>(*module_from_abi(module));
    }

    static void get_exported_names(JSModule* module, JSStringSink* names)
    {
        for (auto const& name : Traits::get_exported_names(host_module_from_abi(module)))
            append_string_to_abi_sink(*names, name);
    }

    static void resolve_export(JSModule* module, u16 const* export_name, size_t export_name_length, JSResolvedBinding* out)
    {
        auto binding = Traits::resolve_export(host_module_from_abi(module), string_from_abi(export_name, export_name_length));
        out->type = resolved_binding_type_to_abi(binding.type);
        out->module = module_to_abi(binding.module.ptr());
        if (binding.type == ResolvedBinding::BindingName)
            append_string_to_abi_sink(out->binding_name, binding.export_name);
    }

    static JSCompletion initialize_environment(JSModule* module)
    {
        return completion_to_abi(Traits::initialize_environment(host_module_from_abi(module)));
    }

    static JSCompletion execute_module(JSModule* module, JSPromiseCapability* capability)
    {
        return completion_to_abi(Traits::execute_module(host_module_from_abi(module), promise_capability_from_abi(capability)));
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

template<typename Traits>
consteval JSHostFunctionHooks make_host_function_hooks()
{
    using Thunks = HostABI::HostFunctionHookThunks<Traits>;
    JSHostFunctionHooks hooks {};
    if constexpr (requires { &Traits::call; })
        hooks.call = &Thunks::call;
    if constexpr (requires { &Traits::construct; })
        hooks.construct = &Thunks::construct;
    if constexpr (requires { &Traits::finalize; })
        hooks.finalize = &Thunks::finalize;
    return hooks;
}

template<typename Traits>
consteval JSHostArrayHooks make_host_array_hooks()
{
    using Thunks = HostABI::HostArrayHookThunks<Traits>;
    JSHostArrayHooks hooks {};
    if constexpr (requires { &Traits::set; })
        hooks.set = &Thunks::set;
    if constexpr (requires { &Traits::delete_property; })
        hooks.delete_property = &Thunks::delete_property;
    return hooks;
}

// All four module hooks are required.
template<typename Traits>
consteval JSHostModuleHooks make_host_module_hooks()
{
    using Thunks = HostABI::HostModuleHookThunks<Traits>;
    return JSHostModuleHooks {
        .get_exported_names = &Thunks::get_exported_names,
        .resolve_export = &Thunks::resolve_export,
        .initialize_environment = &Thunks::initialize_environment,
        .execute_module = &Thunks::execute_module,
    };
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
