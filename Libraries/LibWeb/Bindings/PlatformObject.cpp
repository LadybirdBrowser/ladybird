/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Bindings {

static JS::Realm& wrapper_realm(JS::HostObject const& wrapper)
{
    return wrapper.shape().realm();
}

static Wrappable* wrappable_of(JS::HostObject const& wrapper)
{
    return static_cast<Wrappable*>(wrapper.wrappable().ptr());
}

// Every host object whose class has one of the platform object hook tables is a PlatformObject.
static PlatformObject& as_platform_object(JS::HostObject& wrapper)
{
    return static_cast<PlatformObject&>(wrapper);
}

JS::ThrowCompletionOr<bool> ordinary_define_own_property_and_preserve_wrapper_if_needed(JS::HostObject& object, JS::PropertyKey const& property_name, JS::PropertyDescriptor& property_descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
{
    Optional<JS::PropertyDescriptor> own_property;
    if (!precomputed_get_own_property) {
        own_property = TRY(object.ordinary_get_own_property(property_name));
        precomputed_get_own_property = &own_property;
    }

    bool already_had_own_property = precomputed_get_own_property->has_value();
    auto result = TRY(object.ordinary_define_own_property(property_name, property_descriptor, precomputed_get_own_property));
    if (result && !already_had_own_property && wrapper_realm(object).host_defined()) {
        if (auto* wrappable = wrappable_of(object)) {
            preserve_wrapper(*wrappable, as_platform_object(object));
            // Preservation is one-shot and sticky (append-only per wrapper), so once a
            // non-legacy wrapper has been preserved there is no reason to keep routing its
            // expando writes through the slow add-own-property path: clearing the flag lets
            // subsequent adds use the AddOwnProperty inline cache. Legacy platform objects
            // keep the slow path, since their named-property visibility is impl-data-dependent
            // rather than shape-dependent and must not be bypassed by the cache.
            if (!legacy_platform_object_info_of(object))
                object.clear_requires_slow_add_own_property();
        }
    }

    return result;
}

static JS::ThrowCompletionOr<bool> ordinary_set_prototype_of_and_preserve_wrapper_if_needed(JS::HostObject& wrapper, JS::Object* prototype)
{
    auto* old_prototype = wrapper.shape().prototype();
    auto result = TRY(wrapper.ordinary_set_prototype_of(prototype));
    if (result && old_prototype != wrapper.shape().prototype() && wrapper_realm(wrapper).host_defined()) {
        if (auto* wrappable = wrappable_of(wrapper))
            preserve_wrapper(*wrappable, as_platform_object(wrapper));
    }
    return result;
}

static JS::ThrowCompletionOr<bool> ordinary_prevent_extensions_and_preserve_wrapper_if_needed(JS::HostObject& wrapper)
{
    auto was_extensible = wrapper.extensible();
    auto result = TRY(wrapper.ordinary_prevent_extensions());
    if (result && was_extensible && !wrapper.extensible() && wrapper_realm(wrapper).host_defined()) {
        if (auto* wrappable = wrappable_of(wrapper))
            preserve_wrapper(*wrappable, as_platform_object(wrapper));
    }
    return result;
}

void finalize_platform_object(JS::HostObject& wrapper)
{
    auto* wrappable = wrappable_of(wrapper);
    if (!wrappable)
        return;

    auto& realm = wrapper_realm(wrapper);
    if (realm.host_defined())
        host_defined_wrapper_world(realm).clear_wrapper(*wrappable, as_platform_object(wrapper));
}

#if !defined(AK_OS_WINDOWS)
static_assert(sizeof(PlatformObject) == JS_HOST_OBJECT_SIZE);
#endif

PlatformObject::PlatformObject(JS::Realm& realm, JSHostClass const& host_class)
    : JS::HostObject(realm, host_class, nullptr, nullptr, nullptr)
{
    VERIFY((host_class.flags & platform_object_host_class_flags) == platform_object_host_class_flags);
}

PlatformObject::PlatformObject(JS::Realm& realm, JSHostClass const& host_class, GC::Ref<Bindings::Wrappable> wrappable)
    : JS::HostObject(realm, host_class, nullptr, wrappable, nullptr)
{
    VERIFY((host_class.flags & platform_object_host_class_flags) == platform_object_host_class_flags);
}

PlatformObject::~PlatformObject() = default;

JS::Realm& PlatformObject::realm() const
{
    return shape().realm();
}

bool PlatformObject::implements_interface(String const& interface) const
{
    if (auto const* wrappable = wrappable_impl())
        return wrappable->implements_interface(interface);
    return false;
}

Bindings::InterfaceName PlatformObject::interface_name() const
{
    if (auto const* wrappable = wrappable_impl())
        return wrappable->interface_name();
    VERIFY_NOT_REACHED();
}

Optional<URL::Origin> PlatformObject::extract_an_origin() const
{
    if (auto const* wrappable = wrappable_impl())
        return wrappable->extract_an_origin();
    return {};
}

static Utf16FlyString property_key_to_utf16_fly_string(JS::PropertyKey const& property_key)
{
    return Utf16FlyString { property_key.to_utf16_string() };
}

// The generator only fills in the setters and the deleter of interfaces that declare them, and the legacy platform
// object algorithms only invoke them on such interfaces.
template<typename SpecialOperation>
static SpecialOperation declared_special_operation(SpecialOperation special_operation)
{
    VERIFY(special_operation);
    return special_operation;
}

static Optional<JS::Value> item_value(JS::HostObject const& wrapper, WrapperWorld& wrapper_world, JS::Realm& realm, size_t index)
{
    auto item_value = legacy_platform_object_info_of(wrapper)->item_value;
    if (!item_value)
        return {};
    return item_value(wrapper, wrapper_world, realm, index);
}

static JS::Value named_item_value(JS::HostObject const& wrapper, WrapperWorld& wrapper_world, JS::Realm& realm, Utf16FlyString const& name)
{
    auto named_item_value = legacy_platform_object_info_of(wrapper)->named_item_value;
    if (!named_item_value)
        return JS::js_undefined();
    return named_item_value(wrapper, wrapper_world, realm, name);
}

static Vector<Utf16FlyString> supported_property_names(JS::HostObject const& wrapper)
{
    if (auto const* wrappable = wrappable_of(wrapper))
        return wrappable->supported_property_names();
    return {};
}

static bool is_supported_property_name(JS::HostObject const& wrapper, Utf16FlyString const& name)
{
    if (auto const* wrappable = wrappable_of(wrapper))
        return wrappable->is_supported_property_name(name);
    return supported_property_names(wrapper).contains_slow(name);
}

static bool is_supported_property_index(JS::HostObject const& wrapper, u32 index)
{
    auto& realm = wrapper_realm(wrapper);
    return item_value(wrapper, host_defined_wrapper_world(realm), realm, index).has_value();
}

// https://webidl.spec.whatwg.org/#dfn-named-property-visibility
JS::ThrowCompletionOr<bool> is_named_property_exposed_on_object(JS::HostObject const& wrapper, JS::PropertyKey const& property_key)
{
    // The spec doesn't say anything about the type of the property name here.
    // Numbers can be converted to a string, which is fine and what other engines do.
    // However, since a symbol cannot be converted to a string, it cannot be a supported property name. Return early if it's a symbol.
    if (property_key.is_symbol())
        return false;

    auto const* legacy_info = legacy_platform_object_info_of(wrapper);
    VERIFY(legacy_info);

    // OPTIMIZATION: A stored property on an ordinary prototype masks a named property independently of whether
    //               the name is supported. Check without invoking internal methods so that a collection's built-in
    //               properties do not need its named-property cache. Stop before exotic objects, preserving the
    //               order of their observable operations in the algorithm below.
    if (!legacy_info->has_legacy_override_built_ins_interface_extended_attribute && property_key.is_string()) {
        for (auto const* prototype = wrapper.prototype(); prototype; prototype = prototype->prototype()) {
            if (!prototype->eligible_for_own_property_enumeration_fast_path() || prototype->is_ecmascript_function_object())
                break;
            if (prototype->storage_has(property_key))
                return false;
        }
    }

    // 1. If P is not a supported property name of O, then return false.
    auto property_name = property_key_to_utf16_fly_string(property_key);
    if (!is_supported_property_name(wrapper, property_name))
        return false;

    // 2. If O has an own property named P, then return false.
    // NOTE: This has to be done manually instead of using Object::has_own_property, as that would use the overridden internal_get_own_property.
    auto own_property_named_p = MUST(wrapper.ordinary_get_own_property(property_key));

    if (own_property_named_p.has_value())
        return false;

    // 3. If O implements an interface that has the [LegacyOverrideBuiltIns] extended attribute, then return true.
    if (legacy_info->has_legacy_override_built_ins_interface_extended_attribute)
        return true;

    // 4. Let prototype be O.[[GetPrototypeOf]]().
    auto* prototype = TRY(wrapper.internal_get_prototype_of());

    // 5. While prototype is not null:
    while (prototype) {
        // 1. If prototype is not a named properties object, and prototype has an own property named P, then return false.
        // NB: Window's is the only named properties object.
        if (JS::host_class_of(*prototype) != &window_properties_host_class) {
            bool prototype_has_own_property_named_p = TRY(prototype->has_own_property(property_key));
            if (prototype_has_own_property_named_p)
                return false;
        }

        // 2. Set prototype to prototype.[[GetPrototypeOf]]().
        prototype = TRY(prototype->internal_get_prototype_of());
    }

    // 6. Return true.
    return true;
}

enum class IgnoreNamedProps {
    No,
    Yes,
};

// A property that a legacy platform object exposes for a supported property index or name. Such a property is always
// a configurable data property.
struct LegacyPlatformObjectSpecialProperty {
    JS::Value value;
    bool writable { false };
    bool enumerable { false };
};

// Steps 1 and 2 of https://webidl.spec.whatwg.org/#LegacyPlatformObjectGetOwnProperty, which return the indexed or named
// property named P if O exposes one. Step 3 is left to the callers.
static JS::ThrowCompletionOr<Optional<LegacyPlatformObjectSpecialProperty>> legacy_platform_object_indexed_or_named_property(JS::HostObject const& wrapper, JS::PropertyKey const& property_name, IgnoreNamedProps ignore_named_props)
{
    auto const* legacy_info = legacy_platform_object_info_of(wrapper);
    VERIFY(legacy_info);

    auto& realm = wrapper_realm(wrapper);
    auto& wrapper_world = host_defined_wrapper_world(realm);

    // 1. If O supports indexed properties and P is an array index, then:
    if (legacy_info->supports_indexed_properties && property_name.is_number()) {
        // 1. Let index be the result of calling ToUint32(P).
        u32 index = property_name.as_number();

        // 2. If index is a supported property index, then:
        if (auto maybe_value = item_value(wrapper, wrapper_world, realm, index); maybe_value.has_value()) {
            // 1. Let operation be the operation used to declare the indexed property getter.
            // 2. Let value be an uninitialized variable.
            // 3. If operation was defined without an identifier, then set value to the result of performing the steps listed in the interface description to determine the value of an indexed property with index as the index.
            // 4. Otherwise, operation was defined with an identifier. Set value to the result of performing the method steps of operation with O as this and « index » as the argument values.
            auto value = maybe_value.release_value();

            // 5. Let desc be a newly created Property Descriptor with no fields.
            // 6. Set desc.[[Value]] to the result of converting value to an ECMAScript value.
            // 7. If O implements an interface with an indexed property setter, then set desc.[[Writable]] to true, otherwise set it to false.
            // 8. Set desc.[[Enumerable]] and desc.[[Configurable]] to true.
            // 9. Return desc.
            return LegacyPlatformObjectSpecialProperty {
                .value = value,
                .writable = legacy_info->has_indexed_property_setter,
                .enumerable = true,
            };
        }

        // 3. Set ignoreNamedProps to true.
        ignore_named_props = IgnoreNamedProps::Yes;
    }

    // 2. If O supports named properties and ignoreNamedProps is false, then:
    if (legacy_info->supports_named_properties && ignore_named_props == IgnoreNamedProps::No) {
        // 1. If the result of running the named property visibility algorithm with property name P and object O is true, then:
        if (TRY(is_named_property_exposed_on_object(wrapper, property_name))) {
            auto property_name_utf16 = property_key_to_utf16_fly_string(property_name);

            // 1. Let operation be the operation used to declare the named property getter.
            // 2. Let value be an uninitialized variable.
            // 3. If operation was defined without an identifier, then set value to the result of performing the steps listed in the interface description to determine the value of a named property with P as the name.
            // 4. Otherwise, operation was defined with an identifier. Set value to the result of performing the method steps of operation with O as this and « P » as the argument values.
            auto value = named_item_value(wrapper, wrapper_world, realm, property_name_utf16);

            // 5. Let desc be a newly created Property Descriptor with no fields.
            // 6. Set desc.[[Value]] to the result of converting value to an ECMAScript value.
            // 7. If O implements an interface with a named property setter, then set desc.[[Writable]] to true, otherwise set it to false.
            // 8. If O implements an interface with the [LegacyUnenumerableNamedProperties] extended attribute, then set desc.[[Enumerable]] to false, otherwise set it to true.
            // 9. Set desc.[[Configurable]] to true.
            // 10. Return desc.
            return LegacyPlatformObjectSpecialProperty {
                .value = value,
                .writable = legacy_info->has_named_property_setter,
                .enumerable = !legacy_info->has_legacy_unenumerable_named_properties_interface_extended_attribute,
            };
        }
    }

    return Optional<LegacyPlatformObjectSpecialProperty> {};
}

// https://webidl.spec.whatwg.org/#LegacyPlatformObjectGetOwnProperty
static JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> legacy_platform_object_get_own_property(JS::HostObject const& wrapper, JS::PropertyKey const& property_name, IgnoreNamedProps ignore_named_props)
{
    if (auto property = TRY(legacy_platform_object_indexed_or_named_property(wrapper, property_name, ignore_named_props)); property.has_value()) {
        return JS::PropertyDescriptor {
            .value = property->value,
            .writable = property->writable,
            .enumerable = property->enumerable,
            .configurable = true,
        };
    }

    // 3. Return OrdinaryGetOwnProperty(O, P).
    return TRY(wrapper.ordinary_get_own_property(property_name));
}

// https://webidl.spec.whatwg.org/#invoke-indexed-setter
static WebIDL::ExceptionOr<void> invoke_indexed_property_setter(JS::HostObject& wrapper, JS::PropertyKey const& property_name, JS::Value value)
{
    auto const* legacy_info = legacy_platform_object_info_of(wrapper);
    VERIFY(legacy_info);

    // 1. Let index be the result of calling ? ToUint32(P).
    auto index = property_name.as_number();

    // 2. Let creating be true if index is not a supported property index, and false otherwise.
    bool creating = !is_supported_property_index(wrapper, index);

    // FIXME: We do not have this information at this point, so converting the value is left as an exercise to the inheritor of PlatformObject.
    // 3. Let operation be the operation used to declare the indexed property setter.
    // 4. Let T be the type of the second argument of operation.
    // 5. Let value be the result of converting V to an IDL value of type T.

    // 6. If operation was defined without an identifier, then:
    if (!legacy_info->indexed_property_setter_has_identifier) {
        // 1. If creating is true, then perform the steps listed in the interface description to set the value of a new indexed property with index as the index and value as the value.
        if (creating)
            return declared_special_operation(legacy_info->set_value_of_new_indexed_property)(wrapper, wrapper_realm(wrapper), index, value);

        // 2. Otherwise, creating is false. Perform the steps listed in the interface description to set the value of an existing indexed property with index as the index and value as the value.
        return declared_special_operation(legacy_info->set_value_of_existing_indexed_property)(wrapper, wrapper_realm(wrapper), index, value);
    }

    // 7. Otherwise, operation was defined with an identifier. Perform the method steps of operation with O as this and « index, value » as the argument values.
    return declared_special_operation(legacy_info->set_value_of_indexed_property)(wrapper, wrapper_realm(wrapper), index, value);
}

// https://webidl.spec.whatwg.org/#invoke-named-setter
static WebIDL::ExceptionOr<void> invoke_named_property_setter(JS::HostObject& wrapper, Utf16FlyString const& property_name, JS::Value value)
{
    auto const* legacy_info = legacy_platform_object_info_of(wrapper);
    VERIFY(legacy_info);

    // 1. Let creating be true if P is not a supported property name, and false otherwise.
    bool creating = !is_supported_property_name(wrapper, property_name);

    // FIXME: We do not have this information at this point, so converting the value is left as an exercise to the inheritor of PlatformObject.
    // 2. Let operation be the operation used to declare the indexed property setter.
    // 3. Let T be the type of the second argument of operation.
    // 4. Let value be the result of converting V to an IDL value of type T.

    // 5. If operation was defined without an identifier, then:
    if (!legacy_info->named_property_setter_has_identifier) {
        // 1. If creating is true, then perform the steps listed in the interface description to set the value of a new named property with P as the name and value as the value.
        if (creating)
            return declared_special_operation(legacy_info->set_value_of_new_named_property)(wrapper, wrapper_realm(wrapper), property_name, value);

        // 2. Otherwise, creating is false. Perform the steps listed in the interface description to set the value of an existing named property with P as the name and value as the value.
        return declared_special_operation(legacy_info->set_value_of_existing_named_property)(wrapper, wrapper_realm(wrapper), property_name, value);
    }

    // 6. Otherwise, operation was defined with an identifier. Perform the method steps of operation with O as this and « P, value » as the argument values.
    return declared_special_operation(legacy_info->set_value_of_named_property)(wrapper, wrapper_realm(wrapper), property_name, value);
}

namespace {

struct PlatformObjectTraits {
    static JS::ThrowCompletionOr<bool> set_prototype_of(JS::HostObject& wrapper, JS::Object* prototype)
    {
        return ordinary_set_prototype_of_and_preserve_wrapper_if_needed(wrapper, prototype);
    }

    static JS::ThrowCompletionOr<bool> prevent_extensions(JS::HostObject& wrapper)
    {
        return ordinary_prevent_extensions_and_preserve_wrapper_if_needed(wrapper);
    }

    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostObject& wrapper, JS::PropertyKey const& property_name, JS::PropertyDescriptor& property_descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
    {
        return ordinary_define_own_property_and_preserve_wrapper_if_needed(wrapper, property_name, property_descriptor, precomputed_get_own_property);
    }

    static void finalize(JS::HostObject& wrapper)
    {
        finalize_platform_object(wrapper);
    }
};

struct GlobalPlatformObjectTraits {
    static JS::ThrowCompletionOr<bool> prevent_extensions(JS::HostObject& wrapper)
    {
        return ordinary_prevent_extensions_and_preserve_wrapper_if_needed(wrapper);
    }

    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostObject& wrapper, JS::PropertyKey const& property_name, JS::PropertyDescriptor& property_descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
    {
        return ordinary_define_own_property_and_preserve_wrapper_if_needed(wrapper, property_name, property_descriptor, precomputed_get_own_property);
    }

    static void finalize(JS::HostObject& wrapper)
    {
        finalize_platform_object(wrapper);
    }
};

// https://webidl.spec.whatwg.org/#es-legacy-platform-objects
struct LegacyPlatformObjectTraits {
    // https://webidl.spec.whatwg.org/#legacy-platform-object-set
    static JS::ThrowCompletionOr<bool> set(JS::HostObject& wrapper, JS::PropertyKey const& property_name, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata*, JS::Object::PropertyLookupPhase)
    {
        auto const* legacy_info = legacy_platform_object_info_of(wrapper);
        VERIFY(legacy_info);

        auto& vm = wrapper.vm();

        // 1. If O and Receiver are the same object, then:
        if (receiver.as_if<JS::Object>() == GC::Ptr<JS::Object> { wrapper }) {
            // 1. If O implements an interface with an indexed property setter and P is an array index, then:
            if (legacy_info->has_indexed_property_setter && property_name.is_number()) {
                // 1. Invoke the indexed property setter on O with P and V.
                TRY(WebIDL::throw_dom_exception_if_needed(vm, wrapper_realm(wrapper), [&] { return invoke_indexed_property_setter(wrapper, property_name, value); }));

                // 2. Return true.
                return true;
            }

            // 2. If O implements an interface with a named property setter and P is a String, then:
            // NB: A PropertyKey containing a number is a String (it can only be a String or a Symbol, the number representation is an optimization).
            if (legacy_info->has_named_property_setter && (property_name.is_string() || property_name.is_number())) {
                // 1. Invoke the named property setter on O with P and V.
                TRY(WebIDL::throw_dom_exception_if_needed(vm, wrapper_realm(wrapper), [&] { return invoke_named_property_setter(wrapper, property_key_to_utf16_fly_string(property_name), value); }));

                // 2. Return true.
                return true;
            }
        }

        // 2. Let ownDesc be ? PlatformObjectGetOwnProperty(O, P, true).
        auto own_descriptor = TRY(legacy_platform_object_get_own_property(wrapper, property_name, IgnoreNamedProps::Yes));

        // 3. Perform ? OrdinarySetWithOwnDescriptor(O, P, V, Receiver, ownDesc).
        // NOTE: The spec says "perform" instead of "return", meaning nothing will be returned on this path according to the spec, which isn't possible to do.
        //       Let's treat it as though it says "return" instead of "perform".
        return wrapper.ordinary_set_with_own_descriptor(property_name, value, receiver, own_descriptor);
    }

    // https://webidl.spec.whatwg.org/#legacy-platform-object-defineownproperty
    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostObject& wrapper, JS::PropertyKey const& property_name, JS::PropertyDescriptor& property_descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
    {
        Optional<JS::PropertyDescriptor> get_own_property_result = {};

        auto const* legacy_info = legacy_platform_object_info_of(wrapper);
        VERIFY(legacy_info);

        auto& vm = wrapper.vm();

        // 1. If O supports indexed properties and P is an array index, then:
        if (legacy_info->supports_indexed_properties && property_name.is_number()) {
            // 1. If the result of calling IsDataDescriptor(Desc) is false, then return false.
            if (!property_descriptor.is_data_descriptor())
                return false;

            // 2. If O does not implement an interface with an indexed property setter, then return false.
            if (!legacy_info->has_indexed_property_setter)
                return false;

            // 3. Invoke the indexed property setter on O with P and Desc.[[Value]].
            TRY(WebIDL::throw_dom_exception_if_needed(vm, wrapper_realm(wrapper), [&] { return invoke_indexed_property_setter(wrapper, property_name, property_descriptor.value.value()); }));

            // 4. Return true.
            return true;
        }

        // 2. If O supports named properties, O does not implement an interface with the [Global] extended attribute, P is a String, and P is not an unforgeable property name of O, then:
        // NB: A PropertyKey containing a number is a String (it can only be a String or a Symbol, the number representation is an optimization).
        // FIXME: Check if P is not an unforgeable property name of O
        if (legacy_info->supports_named_properties && !legacy_info->has_global_interface_extended_attribute && (property_name.is_string() || property_name.is_number())) {
            auto const property_name_utf16 = property_key_to_utf16_fly_string(property_name);

            // 1. Let creating be true if P is not a supported property name, and false otherwise.
            bool creating = !is_supported_property_name(wrapper, property_name_utf16);

            // 2. If O implements an interface with the [LegacyOverrideBuiltIns] extended attribute or O does not have an own property named P, then:
            // NOTE: Own property lookup has to be done manually instead of using Object::has_own_property, as that would use the overridden internal_get_own_property.
            if (!legacy_info->has_legacy_override_built_ins_interface_extended_attribute) {
                // AD-HOC: Avoid computing the [[GetOwnProperty]] multiple times.
                if (!precomputed_get_own_property) {
                    get_own_property_result = TRY(wrapper.ordinary_get_own_property(property_name));
                    precomputed_get_own_property = &get_own_property_result;
                }
            }
            if (legacy_info->has_legacy_override_built_ins_interface_extended_attribute || !precomputed_get_own_property->has_value()) {
                // 1. If creating is false and O does not implement an interface with a named property setter, then return false.
                if (!creating && !legacy_info->has_named_property_setter)
                    return false;

                // 2. If O implements an interface with a named property setter, then:
                if (legacy_info->has_named_property_setter) {
                    // 1. If the result of calling IsDataDescriptor(Desc) is false, then return false.
                    if (!property_descriptor.is_data_descriptor())
                        return false;

                    // 2. Invoke the named property setter on O with P and Desc.[[Value]].
                    TRY(WebIDL::throw_dom_exception_if_needed(vm, wrapper_realm(wrapper), [&] { return invoke_named_property_setter(wrapper, property_name_utf16, property_descriptor.value.value()); }));

                    // 3. Return true.
                    return true;
                }
            }
        }

        // 3. Return ! OrdinaryDefineOwnProperty(O, P, Desc).
        return ordinary_define_own_property_and_preserve_wrapper_if_needed(wrapper, property_name, property_descriptor, precomputed_get_own_property);
    }

    // https://webidl.spec.whatwg.org/#legacy-platform-object-delete
    static JS::ThrowCompletionOr<bool> delete_property(JS::HostObject& wrapper, JS::PropertyKey const& property_name)
    {
        auto const* legacy_info = legacy_platform_object_info_of(wrapper);
        VERIFY(legacy_info);

        auto& vm = wrapper.vm();

        // 1. If O supports indexed properties and P is an array index, then:
        if (legacy_info->supports_indexed_properties && property_name.is_number()) {
            // 1. Let index be the result of calling ! ToUint32(P).
            u32 index = property_name.as_number();

            // 2. If index is not a supported property index, then return true.
            if (!is_supported_property_index(wrapper, index))
                return true;

            // 3. Return false.
            return false;
        }

        // 2. If O supports named properties, O does not implement an interface with the [Global] extended attribute and
        //    the result of calling the named property visibility algorithm with property name P and object O is true, then:
        if (legacy_info->supports_named_properties
            && !legacy_info->has_global_interface_extended_attribute
            && TRY(is_named_property_exposed_on_object(wrapper, property_name))) {
            // 1. If O does not implement an interface with a named property deleter, then return false.
            if (!legacy_info->has_named_property_deleter)
                return false;

            // FIXME: It's unfortunate that this is done twice, once in is_named_property_exposed_on_object and here.
            auto property_name_utf16 = property_key_to_utf16_fly_string(property_name);

            // 2. Let operation be the operation used to declare the named property deleter.
            // 3. If operation was defined without an identifier, then:
            //    1. Perform the steps listed in the interface description to delete an existing named property with P as the name.
            //    2. If the steps indicated that the deletion failed, then return false.
            // 4. Otherwise, operation was defined with an identifier:
            //    1. Perform method steps of operation with O as this and « P » as the argument values.
            //    2. If operation was declared with a return type of boolean and the steps returned false, then return false.
            auto did_deletion_fail = TRY(WebIDL::throw_dom_exception_if_needed(vm, wrapper_realm(wrapper), [&] { return declared_special_operation(legacy_info->delete_value)(wrapper, property_name_utf16); }));
            if (!legacy_info->named_property_deleter_has_identifier)
                VERIFY(did_deletion_fail != NamedPropertyDeletionResult::NotRelevant);

            if (did_deletion_fail == NamedPropertyDeletionResult::DidFail)
                return false;

            // 5. Return true.
            return true;
        }

        // 3. If O has an own property with name P, then:
        // NOTE: This has to be done manually instead of using Object::has_own_property, as that would use the overridden internal_get_own_property.
        auto own_property_named_p_descriptor = TRY(wrapper.ordinary_get_own_property(property_name));

        if (own_property_named_p_descriptor.has_value()) {
            // 1. If the property is not configurable, then return false.
            if (!own_property_named_p_descriptor->configurable.value())
                return false;

            // 2. Otherwise, remove the property from O.
            wrapper.storage_delete(property_name);
        }

        // 4. Return true.
        return true;
    }

    static JS::ThrowCompletionOr<bool> set_prototype_of(JS::HostObject& wrapper, JS::Object* prototype)
    {
        return ordinary_set_prototype_of_and_preserve_wrapper_if_needed(wrapper, prototype);
    }

    // https://webidl.spec.whatwg.org/#legacy-platform-object-preventextensions
    static JS::ThrowCompletionOr<bool> prevent_extensions(JS::HostObject&)
    {
        // 1. Return false.
        // Spec Note: Note: this keeps legacy platform objects extensible by making [[PreventExtensions]] fail for them.
        return false;
    }

    static bool is_cacheable_for_inherited_property(JS::HostObject const& wrapper)
    {
        auto const* legacy_info = legacy_platform_object_info_of(wrapper);
        if (!legacy_info->supports_named_properties
            || !legacy_info->has_legacy_override_built_ins_interface_extended_attribute)
            return true;
        return is<DOM::Document>(wrappable_of(wrapper)) && host_defined_wrapper_world(wrapper_realm(wrapper)).is_main_world();
    }

    static void finalize(JS::HostObject& wrapper)
    {
        finalize_platform_object(wrapper);
    }
};

// https://webidl.spec.whatwg.org/#legacy-platform-object-getownproperty
JS::ThrowCompletionOr<void> legacy_platform_object_internal_get_own_property(JS::HostObject const& wrapper, JS::PropertyKey const& property_name, JSPropertyDescriptor& descriptor)
{
    // 1. Return ? PlatformObjectGetOwnProperty(O, P, false).
    if (auto property = TRY(legacy_platform_object_indexed_or_named_property(wrapper, property_name, IgnoreNamedProps::No)); property.has_value()) {
        u16 flags = JS_PD_PRESENT | JS_PD_HAS_VALUE | JS_PD_HAS_WRITABLE | JS_PD_HAS_ENUMERABLE | JS_PD_HAS_CONFIGURABLE | JS_PD_CONFIGURABLE;
        if (property->writable)
            flags |= JS_PD_WRITABLE;
        if (property->enumerable)
            flags |= JS_PD_ENUMERABLE;
        descriptor.value = JS::HostABI::value_to_abi(property->value);
        descriptor.flags = flags;
        return {};
    }

    // NB: This is step 3 of LegacyPlatformObjectGetOwnProperty, OrdinaryGetOwnProperty(O, P).
    descriptor = JS::HostABI::property_descriptor_to_abi(TRY(wrapper.ordinary_get_own_property(property_name)));
    return {};
}

// https://webidl.spec.whatwg.org/#legacy-platform-object-ownpropertykeys
JS::ThrowCompletionOr<void> legacy_platform_object_internal_own_property_keys(JS::HostObject const& wrapper, JSValueSink& keys)
{
    auto const* legacy_info = legacy_platform_object_info_of(wrapper);
    VERIFY(legacy_info);

    auto& vm = wrapper.vm();

    // 1. Let keys be a new empty list of ECMAScript String and Symbol values.
    // NB: keys is the engine's list, which it passes in empty.
    auto append_to_keys = [&](JS::Value key) { keys.append(keys.context, JS::HostABI::value_to_abi(key)); };

    // 2. If O supports indexed properties, then for each index of O’s supported property indices, in ascending numerical order, append ! ToString(index) to keys.
    if (legacy_info->supports_indexed_properties) {
        for (u64 index = 0; index <= NumericLimits<u32>::max(); ++index) {
            if (is_supported_property_index(wrapper, index))
                append_to_keys(JS::PrimitiveString::create_from_unsigned_integer(vm, index));
            else
                break;
        }
    }

    // 3. If O supports named properties, then for each P of O’s supported property names that is visible according to the named property visibility algorithm, append P to keys.
    if (legacy_info->supports_named_properties) {
        for (auto& named_property : supported_property_names(wrapper)) {
            if (TRY(is_named_property_exposed_on_object(wrapper, named_property)))
                append_to_keys(JS::PrimitiveString::create(vm, named_property));
        }
    }

    // 4. For each P of O’s own property keys that is a String, in ascending chronological order of property creation, append P to keys.
    // 5. For each P of O’s own property keys that is a Symbol, in ascending chronological order of property creation, append P to keys.
    // NB: OrdinaryOwnPropertyKeys lists every String key before any Symbol key, but puts array indices first, in ascending
    //     numeric order, rather than in order of creation.
    for (auto key : TRY(wrapper.ordinary_own_property_keys()))
        append_to_keys(key);

    // FIXME: 6. Assert: keys has no duplicate items.

    // 7. Return keys.
    return {};
}

JS::HostObject const& legacy_platform_object_from_abi(JSObject* wrapper)
{
    return static_cast<JS::HostObject const&>(*JS::HostABI::object_from_abi(wrapper));
}

// The legacy [[GetOwnProperty]] and [[OwnPropertyKeys]] write their results straight into the engine's descriptor and
// key list, rather than returning them from traits functions for the hook thunks to copy, because indexed access to
// collections and their enumeration are hot.
consteval JSHostObjectHooks make_legacy_platform_object_hooks()
{
    auto hooks = JS::make_host_object_hooks<LegacyPlatformObjectTraits>();
    hooks.get_own_property = [](JSObject* wrapper, JSPropertyKey property_name, JSPropertyDescriptor* descriptor) {
        return JS::HostABI::completion_to_abi(legacy_platform_object_internal_get_own_property(legacy_platform_object_from_abi(wrapper), JS::HostABI::property_key_from_abi(property_name), *descriptor));
    };
    hooks.own_property_keys = [](JSObject* wrapper, JSValueSink* keys) {
        return JS::HostABI::completion_to_abi(legacy_platform_object_internal_own_property_keys(legacy_platform_object_from_abi(wrapper), *keys));
    };
    return hooks;
}

}

constexpr JSHostObjectHooks platform_object_hooks = JS::make_host_object_hooks<PlatformObjectTraits>();
constexpr JSHostObjectHooks global_platform_object_hooks = JS::make_host_object_hooks<GlobalPlatformObjectTraits>();
constexpr JSHostObjectHooks legacy_platform_object_hooks = make_legacy_platform_object_hooks();

}
