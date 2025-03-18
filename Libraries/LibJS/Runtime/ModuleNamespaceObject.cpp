/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/ModuleNamespaceObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ModuleNamespaceObject);

ModuleNamespaceObject::ModuleNamespaceObject(Realm& realm, Module* module, Vector<FlyString> exports)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype(), MayInterfereWithIndexedPropertyAccess::Yes)
    , m_module(module)
    , m_exports(move(exports))
{
    // Note: We just perform step 6 of 10.4.6.12 ModuleNamespaceCreate ( module, exports ), https://tc39.es/ecma262/#sec-modulenamespacecreate
    // 6. Let sortedExports be a List whose elements are the elements of exports ordered as if an Array of the same values had been sorted using %Array.prototype.sort% using undefined as comparefn.
    quick_sort(m_exports, [&](FlyString const& lhs, FlyString const& rhs) {
        return lhs.bytes_as_string_view() < rhs.bytes_as_string_view();
    });
}

void ModuleNamespaceObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 28.3.1 @@toStringTag, https://tc39.es/ecma262/#sec-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Module"_string), 0);
}

// 10.4.6.1 [[GetPrototypeOf]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-getprototypeof
ThrowCompletionOr<Object*> ModuleNamespaceObject::internal_get_prototype_of() const
{
    // 1. Return null.
    return nullptr;
}

// 10.4.6.2 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-setprototypeof-v
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_set_prototype_of(Object* prototype)
{
    // 1. Return ! SetImmutablePrototype(O, V).
    return MUST(set_immutable_prototype(prototype));
}

// 10.4.6.3 [[IsExtensible]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-isextensible
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_is_extensible() const
{
    // 1. Return false.
    return false;
}

// 10.4.6.4 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-preventextensions
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_prevent_extensions()
{
    // 1. Return true.
    return true;
}

// 10.4.6.5 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-getownproperty-p
ThrowCompletionOr<Optional<PropertyDescriptor>> ModuleNamespaceObject::internal_get_own_property(PropertyKey const& property_key) const
{
    // 1. If Type(P) is Symbol, return OrdinaryGetOwnProperty(O, P).
    if (property_key.is_symbol())
        return Object::internal_get_own_property(property_key);

    // 2. Let exports be O.[[Exports]].
    // 3. If P is not an element of exports, return undefined.
    auto export_element = m_exports.find(property_key.to_string());
    if (export_element.is_end())
        return Optional<PropertyDescriptor> {};

    // 4. Let value be ? O.[[Get]](P, O).
    auto value = TRY(internal_get(property_key, this));

    // 5. Return PropertyDescriptor { [[Value]]: value, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: false }.
    return PropertyDescriptor { .value = value, .writable = true, .enumerable = true, .configurable = false };
}

// 10.4.6.6 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-defineownproperty-p-desc
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor const& descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    // 1. If Type(P) is Symbol, return ! OrdinaryDefineOwnProperty(O, P, Desc).
    if (property_key.is_symbol())
        return MUST(Object::internal_define_own_property(property_key, descriptor, precomputed_get_own_property));

    // 2. Let current be ? O.[[GetOwnProperty]](P).
    auto current = TRY(internal_get_own_property(property_key));

    // 3. If current is undefined, return false.
    if (!current.has_value())
        return false;

    // 4. If Desc has a [[Configurable]] field and Desc.[[Configurable]] is true, return false.
    if (descriptor.configurable.has_value() && descriptor.configurable.value())
        return false;

    // 5. If Desc has an [[Enumerable]] field and Desc.[[Enumerable]] is false, return false.
    if (descriptor.enumerable.has_value() && !descriptor.enumerable.value())
        return false;

    // 6. If IsAccessorDescriptor(Desc) is true, return false.
    if (descriptor.is_accessor_descriptor())
        return false;

    // 7. If Desc has a [[Writable]] field and Desc.[[Writable]] is false, return false.
    if (descriptor.writable.has_value() && !descriptor.writable.value())
        return false;

    // 8. If Desc has a [[Value]] field, return SameValue(Desc.[[Value]], current.[[Value]]).
    if (descriptor.value.has_value())
        return same_value(descriptor.value.value(), current->value.value());

    // 9. Return true.
    return true;
}

// 10.4.6.7 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-hasproperty-p
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_has_property(PropertyKey const& property_key) const
{
    // 1. If Type(P) is Symbol, return ! OrdinaryHasProperty(O, P).
    if (property_key.is_symbol())
        return MUST(Object::internal_has_property(property_key));

    // 2. Let exports be O.[[Exports]].
    // 3. If P is an element of exports, return true.
    auto export_element = m_exports.find(property_key.to_string());
    if (!export_element.is_end())
        return true;

    // 4. Return false.
    return false;
}

// 10.4.6.8 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-get-p-receiver
ThrowCompletionOr<Value> ModuleNamespaceObject::internal_get(PropertyKey const& property_key, Value receiver, CacheablePropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    auto& vm = this->vm();

    // 1. If Type(P) is Symbol, then
    if (property_key.is_symbol()) {
        // a. Return ! OrdinaryGet(O, P, Receiver).
        return MUST(Object::internal_get(property_key, receiver, cacheable_metadata, phase));
    }

    // 2. Let exports be O.[[Exports]].
    // 3. If P is not an element of exports, return undefined.
    auto export_element = m_exports.find(property_key.to_string());
    if (export_element.is_end())
        return js_undefined();

    // 4. Let m be O.[[Module]].
    // 5. Let binding be ! m.ResolveExport(P).
    auto binding = MUST(m_module->resolve_export(vm, property_key.to_string()));

    // 6. Assert: binding is a ResolvedBinding Record.
    VERIFY(binding.is_valid());

    // 7. Let targetModule be binding.[[Module]].
    auto target_module = binding.module;

    // 8. Assert: targetModule is not undefined.
    VERIFY(target_module);

    // 9. If binding.[[BindingName]] is namespace, then
    if (binding.is_namespace()) {
        // a. Return ? GetModuleNamespace(targetModule).
        return TRY(target_module->get_module_namespace(vm));
    }

    // 10. Let targetEnv be targetModule.[[Environment]].
    auto target_environment = target_module->environment();

    // 11. If targetEnv is empty, throw a ReferenceError exception.
    if (!target_environment)
        return vm.throw_completion<ReferenceError>(ErrorType::ModuleNoEnvironment);

    // 12. Return ? targetEnv.GetBindingValue(binding.[[BindingName]], true).
    return target_environment->get_binding_value(vm, binding.export_name, true);
}

// 10.4.6.9 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-set-p-v-receiver
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_set(PropertyKey const&, Value, Value, CacheablePropertyMetadata*)
{
    // 1. Return false.
    return false;
}

// 10.4.6.10 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-delete-p
ThrowCompletionOr<bool> ModuleNamespaceObject::internal_delete(PropertyKey const& property_key)
{
    // 1. If Type(P) is Symbol, then
    if (property_key.is_symbol()) {
        // a. Return ! OrdinaryDelete(O, P).
        return MUST(Object::internal_delete(property_key));
    }

    // 2. Let exports be O.[[Exports]].
    // 3. If P is an element of exports, return false.
    auto export_element = m_exports.find(property_key.to_string());
    if (!export_element.is_end())
        return false;

    // 4. Return true.
    return true;
}

// 10.4.6.11 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-ownpropertykeys
ThrowCompletionOr<GC::RootVector<Value>> ModuleNamespaceObject::internal_own_property_keys() const
{
    // 1. Let exports be O.[[Exports]].
    // NOTE: We only add the exports after we know the size of symbolKeys
    GC::RootVector<Value> exports { vm().heap() };

    // 2. Let symbolKeys be OrdinaryOwnPropertyKeys(O).
    auto symbol_keys = MUST(Object::internal_own_property_keys());

    // 3. Return the list-concatenation of exports and symbolKeys.
    exports.ensure_capacity(m_exports.size() + symbol_keys.size());
    for (auto const& export_name : m_exports)
        exports.unchecked_append(PrimitiveString::create(vm(), export_name));
    exports.extend(symbol_keys);

    return exports;
}

void ModuleNamespaceObject::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_module);
}

}
