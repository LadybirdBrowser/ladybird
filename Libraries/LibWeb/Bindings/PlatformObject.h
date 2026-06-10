/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Weakable.h>
#include <LibJS/Runtime/Object.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

WEB_API void cache_global_object_wrapper(JS::Realm&);
WEB_API Wrappable* wrappable_impl_from(JS::Object*);
WEB_API Wrappable const* wrappable_impl_from(JS::Object const*);

enum class NamedPropertyDeletionResult : u8 {
    // If the named property deleter has an identifier, but does not return a boolean.
    // This is done because we don't know the return type of the deleter outside of the IDL generator.
    NotRelevant,
    DidNotFail,
    DidFail,
};

#define WEB_NON_IDL_PLATFORM_OBJECT(class_, base_class) \
    JS_OBJECT(class_, base_class)

#define WEB_PLATFORM_OBJECT(class_, base_class) \
    JS_OBJECT_WITH_CUSTOM_CLASS_NAME(class_, base_class)

// https://webidl.spec.whatwg.org/#dfn-platform-object
class WEB_API PlatformObject : public JS::Object {
    JS_OBJECT(PlatformObject, JS::Object);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~PlatformObject() override;
    virtual void finalize() override;

    JS::Realm& realm() const;

    // https://webidl.spec.whatwg.org/#implements
    [[nodiscard]] bool implements_interface(String const&) const;

    // Only valid on platform objects that are exposed over IDL.
    [[nodiscard]] Bindings::InterfaceName interface_name() const;

    // ^JS::Object
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const&, JS::Value, JS::Value, JS::CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;
    virtual JS::ThrowCompletionOr<GC::RootVector<JS::Value>> internal_own_property_keys() const override;

    JS::ThrowCompletionOr<bool> is_named_property_exposed_on_object(JS::PropertyKey const&) const;

    // https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
    // Platform objects have an extract an origin operation, which returns null unless otherwise specified.
    Optional<URL::Origin> extract_an_origin() const;

protected:
    explicit PlatformObject(JS::Realm&, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    explicit PlatformObject(JS::Object& prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);

    [[nodiscard]] virtual Bindings::Wrappable* wrappable_impl() { return nullptr; }
    [[nodiscard]] virtual Bindings::Wrappable const* wrappable_impl() const { return nullptr; }

    struct LegacyPlatformObjectFlags {
        u16 supports_indexed_properties : 1 = false;
        u16 supports_named_properties : 1 = false;
        u16 has_indexed_property_setter : 1 = false;
        u16 has_named_property_setter : 1 = false;
        u16 has_named_property_deleter : 1 = false;
        u16 has_legacy_unenumerable_named_properties_interface_extended_attribute : 1 = false;
        u16 has_legacy_override_built_ins_interface_extended_attribute : 1 = false;
        u16 has_global_interface_extended_attribute : 1 = false;
        u16 indexed_property_setter_has_identifier : 1 = false;
        u16 named_property_setter_has_identifier : 1 = false;
        u16 named_property_deleter_has_identifier : 1 = false;
    };
    Optional<LegacyPlatformObjectFlags> m_legacy_platform_object_flags = {};

    enum class IgnoreNamedProps {
        No,
        Yes,
    };
    JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> legacy_platform_object_get_own_property(JS::PropertyKey const&, IgnoreNamedProps ignore_named_props) const;

    virtual Optional<JS::Value> item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const;
    virtual JS::Value named_item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const;
    Vector<FlyString> supported_property_names() const;
    bool is_supported_property_name(FlyString const&) const;
    bool is_supported_property_index(u32) const;

    // NOTE: These dispatch to binding-side helpers and crash if the wrapped implementation does not support the hook.
    // NOTE: This is only used if named_property_setter_has_identifier returns false, otherwise set_value_of_named_property is used instead.
    virtual WebIDL::ExceptionOr<void> set_value_of_new_named_property(JS::Realm&, String const&, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_named_property(JS::Realm&, String const&, JS::Value);

    // NOTE: This dispatches to binding-side helpers and crashes if the wrapped implementation does not support the hook.
    // NOTE: This is only used if you make named_property_setter_has_identifier return true, otherwise set_value_of_{new,existing}_named_property is used instead.
    virtual WebIDL::ExceptionOr<void> set_value_of_named_property(JS::Realm&, String const&, JS::Value);

    // NOTE: These dispatch to binding-side helpers and crash if the wrapped implementation does not support the hook.
    // NOTE: This is only used if indexed_property_setter_has_identifier returns false, otherwise set_value_of_indexed_property is used instead.
    virtual WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(JS::Realm&, u32, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(JS::Realm&, u32, JS::Value);

    // NOTE: This dispatches to binding-side helpers and crashes if the wrapped implementation does not support the hook.
    // NOTE: This is only used if indexed_property_setter_has_identifier returns true, otherwise set_value_of_{new,existing}_indexed_property is used instead.
    virtual WebIDL::ExceptionOr<void> set_value_of_indexed_property(JS::Realm&, u32, JS::Value);

    virtual WebIDL::ExceptionOr<NamedPropertyDeletionResult> delete_value(String const&);

    virtual bool eligible_for_own_property_enumeration_fast_path() const override final { return false; }

private:
    friend WEB_API Bindings::Wrappable* wrappable_impl_from(JS::Object*);
    friend WEB_API Bindings::Wrappable const* wrappable_impl_from(JS::Object const*);
    friend WEB_API void cache_global_object_wrapper(JS::Realm&);

    WebIDL::ExceptionOr<void> invoke_indexed_property_setter(JS::PropertyKey const&, JS::Value);
    WebIDL::ExceptionOr<void> invoke_named_property_setter(FlyString const&, JS::Value);
};

}
