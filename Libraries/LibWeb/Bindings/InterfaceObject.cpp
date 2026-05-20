/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/InterfaceObject.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(InterfacePrototypeObject);
GC_DEFINE_ALLOCATOR(InterfaceConstructor);

static FlyString fly_string_from_metadata_name(StringView name)
{
    return FlyString::from_utf8_without_validation(name.bytes());
}

static Utf16FlyString utf16_fly_string_from_metadata_name(StringView name)
{
    return Utf16FlyString::from_utf8_without_validation(name);
}

static String string_from_metadata_name(StringView name)
{
    return MUST(String::from_utf8(name));
}

InterfacePrototypeObject::InterfacePrototypeObject(JS::Realm& realm, InterfaceObjectMetadata const& metadata)
    : Object(realm, metadata.ensure_parent_prototype ? nullptr : realm.intrinsics().object_prototype().ptr())
    , m_metadata(metadata)
{
}

void InterfacePrototypeObject::initialize(JS::Realm& realm)
{
    auto& vm = realm.vm();

    if (m_metadata.initialize_prototype) {
        m_metadata.initialize_prototype(realm, *this);
    } else if (m_metadata.ensure_parent_prototype) {
        set_prototype(&m_metadata.ensure_parent_prototype(realm));
        define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, string_from_metadata_name(m_metadata.namespaced_name)), JS::Attribute::Configurable);
    } else {
        set_prototype(realm.intrinsics().object_prototype());
        define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, string_from_metadata_name(m_metadata.namespaced_name)), JS::Attribute::Configurable);
    }

    Base::initialize(realm);
}

void InterfacePrototypeObject::define_unforgeable_attributes(JS::Realm& realm, JS::Object& object)
{
    if (m_metadata.define_unforgeable_attributes)
        m_metadata.define_unforgeable_attributes(realm, object);
}

JS::ThrowCompletionOr<bool> InterfacePrototypeObject::internal_set_prototype_of(JS::Object* prototype)
{
    if (m_metadata.has_immutable_prototype)
        return set_immutable_prototype(prototype);
    return Base::internal_set_prototype_of(prototype);
}

InterfaceConstructor::InterfaceConstructor(JS::Realm& realm, InterfaceObjectMetadata const& metadata)
    : NativeFunction(utf16_fly_string_from_metadata_name(metadata.name), realm.intrinsics().function_prototype())
    , m_metadata(metadata)
{
}

JS::ThrowCompletionOr<JS::Value> InterfaceConstructor::call()
{
    return vm().throw_completion<JS::TypeError>(JS::ErrorType::ConstructorWithoutNew, m_metadata.namespaced_name);
}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> InterfaceConstructor::construct([[maybe_unused]] JS::FunctionObject& new_target)
{
    if (m_metadata.construct)
        return m_metadata.construct(*this, new_target);
    return vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, m_metadata.namespaced_name);
}

void InterfaceConstructor::initialize(JS::Realm& realm)
{
    auto& vm = this->vm();

    Base::initialize(realm);

    if (m_metadata.initialize_constructor) {
        m_metadata.initialize_constructor(realm, *this);
        return;
    }

    if (m_metadata.ensure_parent_constructor)
        set_prototype(&m_metadata.ensure_parent_constructor(realm));
    define_direct_property(vm.names.length, JS::Value(0), JS::Attribute::Configurable);
    define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, string_from_metadata_name(m_metadata.name)), JS::Attribute::Configurable);
    define_direct_property(vm.names.prototype, &host_defined_intrinsics(realm).existing_web_prototype(fly_string_from_metadata_name(m_metadata.namespaced_name)), 0);
}

}
