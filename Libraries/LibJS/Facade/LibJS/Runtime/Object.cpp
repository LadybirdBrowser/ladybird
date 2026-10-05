/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(to_underlying(Object::PropertyKind::Key) == JS_PROPERTY_KIND_KEY);
static_assert(to_underlying(Object::PropertyKind::Value) == JS_PROPERTY_KIND_VALUE);
static_assert(to_underlying(Object::PropertyKind::KeyAndValue) == JS_PROPERTY_KIND_KEY_AND_VALUE);
static_assert(to_underlying(Object::IntegrityLevel::Sealed) == JS_INTEGRITY_LEVEL_SEALED);
static_assert(to_underlying(Object::IntegrityLevel::Frozen) == JS_INTEGRITY_LEVEL_FROZEN);
static_assert(Attribute::Writable == JS_ATTRIBUTE_WRITABLE);
static_assert(Attribute::Enumerable == JS_ATTRIBUTE_ENUMERABLE);
static_assert(Attribute::Configurable == JS_ATTRIBUTE_CONFIGURABLE);

// The runtime computes the value of an intrinsic accessor's property by calling it with the object's realm. A Realm&
// is the runtime's realm pointer, and a Value is returned like the JSValue whose bits it has.
static_assert(sizeof(Object::IntrinsicAccessor) == sizeof(JSIntrinsicAccessor));
static_assert(IsTriviallyCopyable<Value> && sizeof(Value) == sizeof(JSValue));

static JSVM* vm_of(Object const& object)
{
    return vm_to_abi(object.vm());
}

GC::Ref<Object> Object::create(Realm& realm, GC::Ptr<Object> prototype)
{
    return object_from_abi(js_object_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), optional_object_to_abi(prototype.ptr())));
}

// 7.3.2 Get ( O, P ), https://tc39.es/ecma262/#sec-get-o-p
ThrowCompletionOr<Value> Object::get(PropertyKey const& property_key) const
{
    return completion_from_abi<Value>(js_object_get(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key)));
}

// 7.3.4 Set ( O, P, V, Throw ), https://tc39.es/ecma262/#sec-set-o-p-v-throw
ThrowCompletionOr<void> Object::set(PropertyKey const& property_key, Value value, ShouldThrowExceptions throw_exceptions)
{
    return completion_from_abi<void>(js_object_set(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value), throw_exceptions == ShouldThrowExceptions::Yes));
}

// 7.3.5 CreateDataProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createdataproperty
ThrowCompletionOr<bool> Object::create_data_property(PropertyKey const& property_key, Value value, Optional<u32>* new_property_offset, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    if (!new_property_offset && !precomputed_get_own_property)
        return completion_from_abi<bool>(js_object_create_data_property(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value)));

    // 1. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
    auto new_descriptor = PropertyDescriptor {
        .value = value,
        .writable = true,
        .enumerable = true,
        .configurable = true,
    };

    // 2. Return ? O.[[DefineOwnProperty]](P, newDesc).
    auto result = internal_define_own_property(property_key, new_descriptor, precomputed_get_own_property);
    if (new_property_offset && new_descriptor.property_offset.has_value())
        *new_property_offset = new_descriptor.property_offset.value();
    return result;
}

// 7.3.7 CreateDataPropertyOrThrow ( O, P, V ), https://tc39.es/ecma262/#sec-createdatapropertyorthrow
ThrowCompletionOr<bool> Object::create_data_property_or_throw(PropertyKey const& property_key, Value value)
{
    return completion_from_abi<bool>(js_object_create_data_property_or_throw(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value)));
}

// 7.3.8 CreateNonEnumerableDataPropertyOrThrow ( O, P, V ), https://tc39.es/ecma262/#sec-createnonenumerabledatapropertyorthrow
void Object::create_non_enumerable_data_property_or_throw(PropertyKey const& property_key, Value value)
{
    VERIFY(!value.is_special_empty_value());

    // 1. Assert: O is an ordinary, extensible object with no non-configurable properties.

    // 2. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
    auto new_description = PropertyDescriptor { .value = value, .writable = true, .enumerable = false, .configurable = true };

    // 3. Perform ! DefinePropertyOrThrow(O, P, newDesc).
    MUST(define_property_or_throw(property_key, new_description));

    // 4. Return unused.
}

// 7.3.9 DefinePropertyOrThrow ( O, P, desc ), https://tc39.es/ecma262/#sec-definepropertyorthrow
ThrowCompletionOr<void> Object::define_property_or_throw(PropertyKey const& property_key, PropertyDescriptor& property_descriptor)
{
    auto abi_descriptor = property_descriptor_to_abi(property_descriptor);
    auto completion = js_object_define_property_or_throw(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), &abi_descriptor);
    update_property_descriptor_from_abi(property_descriptor, abi_descriptor);
    return completion_from_abi<void>(completion);
}

// 7.3.12 HasProperty ( O, P ), https://tc39.es/ecma262/#sec-hasproperty
ThrowCompletionOr<bool> Object::has_property(PropertyKey const& property_key) const
{
    return completion_from_abi<bool>(js_object_has_property(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key)));
}

// 7.3.13 HasOwnProperty ( O, P ), https://tc39.es/ecma262/#sec-hasownproperty
ThrowCompletionOr<bool> Object::has_own_property(PropertyKey const& property_key) const
{
    return completion_from_abi<bool>(js_object_has_own_property(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key)));
}

// 7.3.16 SetIntegrityLevel ( O, level ), https://tc39.es/ecma262/#sec-setintegritylevel
ThrowCompletionOr<bool> Object::set_integrity_level(IntegrityLevel level)
{
    return completion_from_abi<bool>(js_object_set_integrity_level(vm_of(*this), object_to_abi(*this), to_underlying(level)));
}

// 7.3.23 EnumerableOwnProperties ( O, kind ), https://tc39.es/ecma262/#sec-enumerableownproperties
ThrowCompletionOr<GC::RootVector<Value>> Object::enumerable_own_property_names(PropertyKind kind) const
{
    return collect_values_from_abi([&](JSValueSink* sink) {
        return js_object_enumerable_own_property_names(vm_of(*this), object_to_abi(*this), to_underlying(kind), sink);
    });
}

ThrowCompletionOr<Object*> Object::internal_get_prototype_of() const
{
    return optional_object_completion_from_abi(js_object_internal_get_prototype_of(vm_of(*this), object_to_abi(*this)));
}

ThrowCompletionOr<bool> Object::internal_set_prototype_of(Object* prototype)
{
    return completion_from_abi<bool>(js_object_internal_set_prototype_of(vm_of(*this), object_to_abi(*this), optional_object_to_abi(prototype)));
}

static ThrowCompletionOr<Optional<PropertyDescriptor>> own_property_from_abi(JSPropertyDescriptor const& abi_descriptor, JSCompletion completion)
{
    TRY(completion_from_abi<void>(completion));
    return property_descriptor_from_abi(abi_descriptor);
}

ThrowCompletionOr<Optional<PropertyDescriptor>> Object::internal_get_own_property(PropertyKey const& property_key) const
{
    JSPropertyDescriptor abi_descriptor {};
    auto completion = js_object_internal_get_own_property(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), &abi_descriptor);
    return own_property_from_abi(abi_descriptor, completion);
}

using DefineOwnPropertyThroughABI = JSCompletion (*)(JSVM*, JSObject*, JSPropertyKey const*, JSPropertyDescriptor*, JSPropertyDescriptor const*);

static ThrowCompletionOr<bool> define_own_property_through_abi(DefineOwnPropertyThroughABI define_own_property, Object& object, PropertyKey const& property_key, PropertyDescriptor& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    auto abi_descriptor = property_descriptor_to_abi(property_descriptor);
    PrecomputedOwnPropertyForABI precomputed { precomputed_get_own_property };
    auto completion = define_own_property(vm_of(object), object_to_abi(object), property_key_to_abi(property_key), &abi_descriptor, precomputed.pointer());
    update_property_descriptor_from_abi(property_descriptor, abi_descriptor);
    return completion_from_abi<bool>(completion);
}

ThrowCompletionOr<bool> Object::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    return define_own_property_through_abi(js_object_internal_define_own_property, *this, property_key, property_descriptor, precomputed_get_own_property);
}

ThrowCompletionOr<Value> Object::internal_get(PropertyKey const& property_key, Value receiver, CacheableGetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    return completion_from_abi<Value>(js_object_internal_get(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(receiver), get_cache_metadata_to_abi(cacheable_metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> Object::internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase)
{
    return completion_from_abi<bool>(js_object_internal_set(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value), value_to_abi(receiver), set_cache_metadata_to_abi(cacheable_metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> Object::internal_delete(PropertyKey const& property_key)
{
    return completion_from_abi<bool>(js_object_internal_delete(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key)));
}

ThrowCompletionOr<GC::RootVector<Value>> Object::internal_own_property_keys() const
{
    return collect_values_from_abi([&](JSValueSink* sink) {
        return js_object_internal_own_property_keys(vm_of(*this), object_to_abi(*this), sink);
    });
}

ThrowCompletionOr<Value> Object::internal_get_as_prototype_of(PropertyKey const& property_key, Value receiver, CacheableGetPropertyMetadata* metadata_for_caller) const
{
    return completion_from_abi<Value>(js_object_internal_get_as_prototype_of(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(receiver), get_cache_metadata_to_abi(metadata_for_caller)));
}

ThrowCompletionOr<Object*> Object::ordinary_get_prototype_of() const
{
    return optional_object_completion_from_abi(js_object_ordinary_get_prototype_of(vm_of(*this), object_to_abi(*this)));
}

ThrowCompletionOr<bool> Object::ordinary_set_prototype_of(Object* prototype)
{
    return completion_from_abi<bool>(js_object_ordinary_set_prototype_of(vm_of(*this), object_to_abi(*this), optional_object_to_abi(prototype)));
}

ThrowCompletionOr<bool> Object::ordinary_prevent_extensions()
{
    return completion_from_abi<bool>(js_object_ordinary_prevent_extensions(vm_of(*this), object_to_abi(*this)));
}

ThrowCompletionOr<Optional<PropertyDescriptor>> Object::ordinary_get_own_property(PropertyKey const& property_key) const
{
    JSPropertyDescriptor abi_descriptor {};
    auto completion = js_object_ordinary_get_own_property(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), &abi_descriptor);
    return own_property_from_abi(abi_descriptor, completion);
}

ThrowCompletionOr<bool> Object::ordinary_define_own_property(PropertyKey const& property_key, PropertyDescriptor& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    return define_own_property_through_abi(js_object_ordinary_define_own_property, *this, property_key, property_descriptor, precomputed_get_own_property);
}

ThrowCompletionOr<Value> Object::ordinary_get(PropertyKey const& property_key, Value receiver, CacheableGetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    return completion_from_abi<Value>(js_object_ordinary_get(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(receiver), get_cache_metadata_to_abi(cacheable_metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> Object::ordinary_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase)
{
    return completion_from_abi<bool>(js_object_ordinary_set(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value), value_to_abi(receiver), set_cache_metadata_to_abi(cacheable_metadata), lookup_phase_to_abi(phase)));
}

ThrowCompletionOr<bool> Object::ordinary_delete(PropertyKey const& property_key)
{
    return completion_from_abi<bool>(js_object_ordinary_delete(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key)));
}

ThrowCompletionOr<GC::RootVector<Value>> Object::ordinary_own_property_keys() const
{
    return collect_values_from_abi([&](JSValueSink* sink) {
        return js_object_ordinary_own_property_keys(vm_of(*this), object_to_abi(*this), sink);
    });
}

void Object::clear_requires_slow_add_own_property()
{
    js_object_clear_requires_slow_add_own_property(object_to_abi(*this));
}

// 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc ), https://tc39.es/ecma262/#sec-ordinarysetwithowndescriptor
ThrowCompletionOr<bool> Object::ordinary_set_with_own_descriptor(PropertyKey const& property_key, Value value, Value receiver, Optional<PropertyDescriptor> own_descriptor, CacheableSetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase)
{
    auto abi_own_descriptor = optional_property_descriptor_to_abi(own_descriptor);
    return completion_from_abi<bool>(js_object_ordinary_set_with_own_descriptor(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value), value_to_abi(receiver), &abi_own_descriptor, set_cache_metadata_to_abi(cacheable_metadata), lookup_phase_to_abi(phase)));
}

// 10.4.7.2 SetImmutablePrototype ( O, V ), https://tc39.es/ecma262/#sec-set-immutable-prototype
ThrowCompletionOr<bool> Object::set_immutable_prototype(Object* prototype)
{
    return completion_from_abi<bool>(js_object_set_immutable_prototype(vm_of(*this), object_to_abi(*this), optional_object_to_abi(prototype)));
}

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
Optional<Completion> Object::enumerate_object_properties(Function<Optional<Completion>(Value)> callback) const
{
    struct Enumeration {
        Function<Optional<Completion>(Value)>& callback;
        Optional<Completion> completion_the_callback_stopped_with;
    };
    Enumeration enumeration { callback, {} };

    auto completion = js_object_enumerate_object_properties(
        vm_of(*this), object_to_abi(*this),
        [](void* context, JSValue key) -> bool {
            auto& enumeration = *static_cast<Enumeration*>(context);
            enumeration.completion_the_callback_stopped_with = enumeration.callback(value_from_abi(key));
            return enumeration.completion_the_callback_stopped_with.has_value();
        },
        &enumeration);
    if (completion.variant == JS_COMPLETION_THROW)
        return completion_from_abi<void>(completion).release_error();
    return enumeration.completion_the_callback_stopped_with;
}

bool Object::storage_has(PropertyKey const& property_key) const
{
    return js_object_storage_has(object_to_abi(*this), property_key_to_abi(property_key));
}

void Object::storage_delete(PropertyKey const& property_key)
{
    js_object_storage_delete(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key));
}

Value Object::get_without_side_effects(PropertyKey const& property_key) const
{
    return value_from_abi(js_object_get_without_side_effects(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key)));
}

void Object::define_direct_property(PropertyKey const& property_key, Value value, PropertyAttributes attributes)
{
    js_object_define_direct_property(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), value_to_abi(value), attributes.bits());
}

void Object::define_unimplemented_property(Utf16FlyString const& property_name)
{
    js_object_define_unimplemented_property(vm_of(*this), object_to_abi(*this), utf16_view_to_abi(property_name.view()));
}

void Object::define_direct_accessor(PropertyKey const& property_key, GC::Ptr<FunctionObject> getter, GC::Ptr<FunctionObject> setter, PropertyAttributes attributes)
{
    js_object_define_direct_accessor(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), optional_object_to_abi(getter.ptr()), optional_object_to_abi(setter.ptr()), attributes.bits());
}

void Object::define_direct_cached_accessor(PropertyKey const& property_key, GC::Ptr<FunctionObject> getter, GC::Ptr<FunctionObject> setter, PropertyAttributes attributes)
{
    js_object_define_direct_cached_accessor(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), optional_object_to_abi(getter.ptr()), optional_object_to_abi(setter.ptr()), attributes.bits());
}

void Object::clear_cached_accessor_value(PropertyKey const& property_key)
{
    js_object_clear_cached_accessor_value(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key));
}

void Object::define_intrinsic_accessor(PropertyKey const& property_key, PropertyAttributes attributes, IntrinsicAccessor accessor)
{
    VERIFY(accessor);
    js_object_define_intrinsic_accessor(vm_of(*this), object_to_abi(*this), property_key_to_abi(property_key), attributes.bits(), bit_cast<JSIntrinsicAccessor>(accessor));
}

void Object::define_native_function(Realm& realm, PropertyKey const& property_key, NativeFunctionPointer native_function, i32 length, PropertyAttributes attributes)
{
    js_object_define_native_function(vm_of(*this), object_to_abi(*this), cell_to_abi<JSRealm>(realm), property_key_to_abi(property_key), native_function_to_abi(native_function), length, attributes.bits());
}

void Object::define_native_function(Realm& realm, PropertyKey const& property_key, Function<ThrowCompletionOr<Value>(VM&)> native_function, i32 length, PropertyAttributes attributes)
{
    auto function = NativeFunction::create(realm, move(native_function), length, property_key, &realm);
    define_direct_property(property_key, function, attributes);
}

void Object::define_native_accessor(Realm& realm, PropertyKey const& property_key, NativeFunctionPointer getter, NativeFunctionPointer setter, PropertyAttributes attributes)
{
    js_object_define_native_accessor(vm_of(*this), object_to_abi(*this), cell_to_abi<JSRealm>(realm), property_key_to_abi(property_key), getter ? native_function_to_abi(getter) : nullptr, setter ? native_function_to_abi(setter) : nullptr, attributes.bits());
}

void Object::define_native_accessor(Realm& realm, PropertyKey const& property_key, Function<ThrowCompletionOr<Value>(VM&)> getter, Function<ThrowCompletionOr<Value>(VM&)> setter, PropertyAttributes attributes)
{
    GC::Ptr<FunctionObject> getter_function;
    if (getter)
        getter_function = NativeFunction::create(realm, move(getter), 0, property_key, &realm, "get"sv);
    GC::Ptr<FunctionObject> setter_function;
    if (setter)
        setter_function = NativeFunction::create(realm, move(setter), 1, property_key, &realm, "set"sv);
    define_direct_accessor(property_key, getter_function, setter_function, attributes);
}

bool Object::is_date() const
{
    return is_of_engine_class_or_subclass(JS_LAYOUT_CLASS_ID_DATE);
}

ErrorData* Object::error_data()
{
    return reinterpret_cast<ErrorData*>(const_cast<JSErrorData*>(js_error_data_of(object_to_abi(*this))));
}

ErrorData const* Object::error_data() const
{
    return reinterpret_cast<ErrorData const*>(js_error_data_of(object_to_abi(*this)));
}

bool Object::eligible_for_own_property_enumeration_fast_path() const
{
    return js_object_eligible_for_own_property_enumeration_fast_path(object_to_abi(*this));
}

bool Object::has_parameter_map() const
{
    return js_object_has_parameter_map(object_to_abi(*this));
}

u32 Object::indexed_array_like_size() const
{
    return js_array_indexed_array_like_size(object_to_abi(*this));
}

void Object::indexed_append(Value value, PropertyAttributes attributes)
{
    VERIFY(attributes == default_attributes);
    js_array_indexed_append(object_to_abi(*this), value_to_abi(value));
}

ValueAndAttributes Object::indexed_take_first()
{
    return ValueAndAttributes { .value = value_from_abi(js_array_indexed_take_first(object_to_abi(*this))) };
}

void Object::convert_to_prototype_if_needed()
{
    js_object_convert_to_prototype_if_needed(vm_of(*this), object_to_abi(*this));
}

void Object::invalidate_property_lookup_caches()
{
    js_object_invalidate_property_lookup_caches(vm_of(*this), object_to_abi(*this));
}

void Object::set_prototype(GC::Ptr<Object> prototype)
{
    js_object_set_prototype(vm_of(*this), object_to_abi(*this), optional_object_to_abi(prototype.ptr()));
}

StringView Object::class_name() const
{
    size_t length = 0;
    auto const* characters = js_object_class_name(object_to_abi(*this), &length);
    return { reinterpret_cast<char const*>(characters), length };
}

JSHostClass const* host_class_of(Object const& object)
{
    return js_host_object_host_class_of(object_to_abi(object));
}

}
