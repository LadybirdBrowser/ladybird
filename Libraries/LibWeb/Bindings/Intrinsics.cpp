/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Forward.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/InterfaceObject.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(Intrinsics);

void Intrinsics::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_namespaces);
    visitor.visit(m_prototypes);
    visitor.visit(m_constructors);
    visitor.visit(m_realm);
    visitor.visit(m_unforgeable_functions);
}

Intrinsics& host_defined_intrinsics(JS::Realm& realm)
{
    ASSERT(realm.host_defined());
    return host_defined_of(realm).intrinsics;
}

GC::Ref<JS::NativeFunction> Intrinsics::ensure_web_unforgeable_function(
    Utf16FlyString const& interface_name,
    Utf16FlyString const& attribute_name,
    Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> behaviour,
    UnforgeableKey::Type type)
{
    UnforgeableKey key { interface_name, attribute_name, type };
    if (auto it = m_unforgeable_functions.find(key); it != m_unforgeable_functions.end())
        return *it->value;

    auto function = JS::NativeFunction::create(*m_realm, move(behaviour), type == UnforgeableKey::Type::Setter ? 1 : 0, attribute_name, m_realm, type == UnforgeableKey::Type::Setter ? "set"sv : "get"sv);
    m_unforgeable_functions.set(key, *function);
    return *function;
}

void Intrinsics::create_web_prototype_and_constructor(JS::Realm& realm, InterfaceObjectMetadata const& metadata)
{
    auto prototype = JS::HostObject::create(realm, metadata.prototype_host_class, nullptr);
    metadata.initialize_prototype(realm, prototype);
    m_prototypes.set(Utf16FlyString::from_utf16(metadata.utf16_namespaced_name), prototype);

    create_web_constructor(realm, metadata, prototype);
}

void Intrinsics::create_web_constructor(JS::Realm& realm, InterfaceObjectMetadata const& metadata, JS::Object& prototype)
{
    auto& vm = realm.vm();

    auto constructor = JS::HostFunction::create_without_own_properties(realm, metadata.constructor_host_class, Utf16FlyString::from_utf16(metadata.utf16_name));
    metadata.initialize_constructor(realm, constructor);
    m_constructors.set(Utf16FlyString::from_utf16(metadata.utf16_namespaced_name), constructor);

    prototype.define_direct_property(vm.names.constructor, constructor.ptr(), JS::Attribute::Writable | JS::Attribute::Configurable);
}

// https://webidl.spec.whatwg.org/#legacy-factory-functions
void Intrinsics::create_legacy_factory_function(JS::Realm& realm, InterfaceObjectMetadata const& metadata)
{
    auto& vm = realm.vm();

    // Legacy factory functions have never had a NativeFunction name, so Function.prototype.toString() renders them as
    // "function () { [native code] }" and call stacks show them unnamed.
    auto legacy_factory_function = JS::HostFunction::create_without_own_properties(realm, metadata.constructor_host_class, {});
    legacy_factory_function->define_direct_property(vm.names.length, JS::Value(metadata.function_length), JS::Attribute::Configurable);
    legacy_factory_function->define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, metadata.utf16_name), JS::Attribute::Configurable);
    legacy_factory_function->define_direct_property(vm.names.prototype, &metadata.ensure_interface_prototype_object(realm), 0);
    m_constructors.set(Utf16FlyString::from_utf16(metadata.utf16_name), legacy_factory_function);
}

}

namespace AK {

unsigned Traits<Web::Bindings::UnforgeableKey>::hash(Web::Bindings::UnforgeableKey const& key)
{
    return pair_int_hash(pair_int_hash(key.attribute_name.hash(), key.interface_name.hash()), to_underlying(key.type));
}

}
