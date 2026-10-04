/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16FlyString.h>
#include <AK/Weakable.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

enum class NamedPropertyDeletionResult : u8 {
    // If the named property deleter has an identifier, but does not return a boolean.
    // This is done because we don't know the return type of the deleter outside of the IDL generator.
    NotRelevant,
    DidNotFail,
    DidFail,
};

// The engine flags of every platform object's host class.
constexpr u32 platform_object_host_class_flags = JS_HOST_CLASS_IS_PLATFORM_OBJECT
    | JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY
    | JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH;

// What the internal methods of a legacy platform object need to know about the interfaces it implements, accumulated
// over its interface's inheritance chain. A wrapper's host class points at it through its user data, which is null
// for wrappers that are not legacy platform objects.
//
// The functions perform the special operations of the interfaces on the wrapped implementation object. Each one comes
// from the nearest interface in the chain that declares the operation, and is null if none does.
struct LegacyPlatformObjectInfo {
    using IndexedPropertyGetter = Optional<JS::Value> (*)(JS::HostObject const& wrapper, WrapperWorld&, JS::Realm&, size_t index);
    using NamedPropertyGetter = JS::Value (*)(JS::HostObject const& wrapper, WrapperWorld&, JS::Realm&, Utf16FlyString const& name);
    using IndexedPropertySetter = WebIDL::ExceptionOr<void> (*)(JS::HostObject& wrapper, JS::Realm&, u32 index, JS::Value);
    using NamedPropertySetter = WebIDL::ExceptionOr<void> (*)(JS::HostObject& wrapper, JS::Realm&, Utf16FlyString const& name, JS::Value);
    using NamedPropertyDeleter = WebIDL::ExceptionOr<NamedPropertyDeletionResult> (*)(JS::HostObject& wrapper, Utf16FlyString const& name);

    bool supports_indexed_properties { false };
    bool supports_named_properties { false };
    bool has_indexed_property_setter { false };
    bool has_named_property_setter { false };
    bool has_named_property_deleter { false };
    bool has_legacy_unenumerable_named_properties_interface_extended_attribute { false };
    bool has_legacy_override_built_ins_interface_extended_attribute { false };
    bool has_global_interface_extended_attribute { false };
    bool indexed_property_setter_has_identifier { false };
    bool named_property_setter_has_identifier { false };
    bool named_property_deleter_has_identifier { false };

    // Returns no value for an index that is not a supported property index.
    IndexedPropertyGetter item_value { nullptr };
    NamedPropertyGetter named_item_value { nullptr };

    // The steps to set the value of a new or an existing indexed property, for an indexed property setter declared
    // without an identifier, and the method steps of one declared with an identifier.
    IndexedPropertySetter set_value_of_new_indexed_property { nullptr };
    IndexedPropertySetter set_value_of_existing_indexed_property { nullptr };
    IndexedPropertySetter set_value_of_indexed_property { nullptr };

    // The same for the named property setter.
    NamedPropertySetter set_value_of_new_named_property { nullptr };
    NamedPropertySetter set_value_of_existing_named_property { nullptr };
    NamedPropertySetter set_value_of_named_property { nullptr };

    NamedPropertyDeleter delete_value { nullptr };
};

[[nodiscard]] inline LegacyPlatformObjectInfo const* legacy_platform_object_info_of(JS::HostObject const& wrapper)
{
    return static_cast<LegacyPlatformObjectInfo const*>(wrapper.host_class().user_data);
}

template<typename Implementation>
[[nodiscard]] Implementation& wrapped_implementation_of(JS::HostObject const& wrapper)
{
    return static_cast<Implementation&>(*static_cast<Wrappable*>(wrapper.wrappable().ptr()));
}

// The internal methods of wrapper host classes. Wrappers of [Global] interfaces and of interfaces without special
// operations have the ordinary internal methods, but preserve the wrapper once script gives it state of its own: a new
// own property, another prototype or non-extensibility. A [Global] wrapper's prototype is immutable through its host
// class flags instead. Legacy platform objects have the internal methods that WebIDL defines for them. A DOMException
// wrapper is an ordinary wrapper that also exposes the error data of its implementation object.
//
// Outside LibWeb, their addresses are not constant expressions on Windows, where the tables are imported from a DLL.
extern WEB_API JSHostObjectHooks const platform_object_hooks;
extern WEB_API JSHostObjectHooks const global_platform_object_hooks;
extern WEB_API JSHostObjectHooks const legacy_platform_object_hooks;
extern JSHostObjectHooks const dom_exception_wrapper_hooks;

// The finalize hook of every wrapper host class calls this to remove the wrapper from its world.
void finalize_platform_object(JS::HostObject& wrapper);

// https://webidl.spec.whatwg.org/#dfn-platform-object
// Only host classes give objects the platform object flag, so every platform object is a host object: a wrapper, whose
// wrappable slot holds its implementation object, or a WindowProxy, whose wrappable slot is null.
[[nodiscard]] inline JS::HostObject* as_platform_object(JS::Object& object)
{
    if (!object.is_platform_object())
        return nullptr;
    return static_cast<JS::HostObject*>(&object);
}

[[nodiscard]] inline JS::HostObject const* as_platform_object(JS::Object const& object)
{
    if (!object.is_platform_object())
        return nullptr;
    return static_cast<JS::HostObject const*>(&object);
}

// https://webidl.spec.whatwg.org/#dfn-named-property-visibility
// The wrapper must be a legacy platform object or the wrapper of a [Global] interface.
WEB_API JS::ThrowCompletionOr<bool> is_named_property_exposed_on_object(JS::HostObject const& wrapper, JS::PropertyKey const&);

// Defines the property via OrdinaryDefineOwnProperty and, if that created a new own property,
// preserves the object's wrapper so the expando stays alive as long as the wrappable does.
// Wrapper host classes with a custom [[DefineOwnProperty]] must route their ordinary path through this.
WEB_API JS::ThrowCompletionOr<bool> ordinary_define_own_property_and_preserve_wrapper_if_needed(JS::HostObject& wrapper, JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>* precomputed_get_own_property);

}
