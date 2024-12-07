/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/EnvironmentCoordinate.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ObjectEnvironment.h>

namespace JS {

GC_DEFINE_ALLOCATOR(GlobalEnvironment);

// 9.1.2.5 NewGlobalEnvironment ( G, thisValue ), https://tc39.es/ecma262/#sec-newglobalenvironment
GlobalEnvironment::GlobalEnvironment(Object& global_object, Object& this_value)
    : Environment(nullptr)
    , m_global_this_value(&this_value)
{
    m_object_record = global_object.heap().allocate<ObjectEnvironment>(global_object, ObjectEnvironment::IsWithEnvironment::No, nullptr);
    m_declarative_record = global_object.heap().allocate<DeclarativeEnvironment>();
}

void GlobalEnvironment::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_object_record);
    visitor.visit(m_global_this_value);
    visitor.visit(m_declarative_record);
}

// 9.1.1.4.11 GetThisBinding ( ), https://tc39.es/ecma262/#sec-global-environment-records-getthisbinding
ThrowCompletionOr<Value> GlobalEnvironment::get_this_binding(VM&) const
{
    // 1. Return envRec.[[GlobalThisValue]].
    return m_global_this_value;
}

// 9.1.1.4.1 HasBinding ( N ), https://tc39.es/ecma262/#sec-global-environment-records-hasbinding-n
ThrowCompletionOr<bool> GlobalEnvironment::has_binding(FlyString const& name, Optional<size_t>*) const
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, return true.
    if (MUST(m_declarative_record->has_binding(name)))
        return true;

    // 3. Let ObjRec be envRec.[[ObjectRecord]].
    // 4. Return ? ObjRec.HasBinding(N).
    return m_object_record->has_binding(name);
}

// 9.1.1.4.2 CreateMutableBinding ( N, D ), https://tc39.es/ecma262/#sec-global-environment-records-createmutablebinding-n-d
ThrowCompletionOr<void> GlobalEnvironment::create_mutable_binding(VM& vm, FlyString const& name, bool can_be_deleted)
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, throw a TypeError exception.
    if (MUST(m_declarative_record->has_binding(name)))
        return vm.throw_completion<TypeError>(ErrorType::GlobalEnvironmentAlreadyHasBinding, name);

    // 3. Return ! DclRec.CreateMutableBinding(N, D).
    return MUST(m_declarative_record->create_mutable_binding(vm, name, can_be_deleted));
}

// 9.1.1.4.3 CreateImmutableBinding ( N, S ), https://tc39.es/ecma262/#sec-global-environment-records-createimmutablebinding-n-s
ThrowCompletionOr<void> GlobalEnvironment::create_immutable_binding(VM& vm, FlyString const& name, bool strict)
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, throw a TypeError exception.
    if (MUST(m_declarative_record->has_binding(name)))
        return vm.throw_completion<TypeError>(ErrorType::GlobalEnvironmentAlreadyHasBinding, name);

    // 3. Return ! DclRec.CreateImmutableBinding(N, S).
    return MUST(m_declarative_record->create_immutable_binding(vm, name, strict));
}

// 9.1.1.4.4 InitializeBinding ( N, V, hint ), https://tc39.es/ecma262/#sec-global-environment-records-initializebinding-n-v
ThrowCompletionOr<void> GlobalEnvironment::initialize_binding(VM& vm, FlyString const& name, Value value, InitializeBindingHint hint)
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, then
    if (MUST(m_declarative_record->has_binding(name))) {
        // a. Return ! DclRec.InitializeBinding(N, V, hint).
        return MUST(m_declarative_record->initialize_binding(vm, name, value, hint));
    }

    // 3. Assert: If the binding exists, it must be in the object Environment Record.
    // 4. Assert: hint is normal.
    VERIFY(hint == Environment::InitializeBindingHint::Normal);
    // 5. Let ObjRec be envRec.[[ObjectRecord]].
    // 6. Return ? ObjRec.InitializeBinding(N, V, normal).
    return m_object_record->initialize_binding(vm, name, value, Environment::InitializeBindingHint::Normal);
}

// 9.1.1.4.5 SetMutableBinding ( N, V, S ), https://tc39.es/ecma262/#sec-global-environment-records-setmutablebinding-n-v-s
ThrowCompletionOr<void> GlobalEnvironment::set_mutable_binding(VM& vm, FlyString const& name, Value value, bool strict)
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, then
    if (MUST(m_declarative_record->has_binding(name))) {
        // a. Return ? DclRec.SetMutableBinding(N, V, S).
        return m_declarative_record->set_mutable_binding(vm, name, value, strict);
    }

    // 3. Let ObjRec be envRec.[[ObjectRecord]].
    // 4. Return ? ObjRec.SetMutableBinding(N, V, S).
    return m_object_record->set_mutable_binding(vm, name, value, strict);
}

// 9.1.1.4.6 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-global-environment-records-getbindingvalue-n-s
ThrowCompletionOr<Value> GlobalEnvironment::get_binding_value(VM& vm, FlyString const& name, bool strict)
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, then
    Optional<size_t> index;
    if (MUST(m_declarative_record->has_binding(name, &index))) {
        // a. Return ? DclRec.GetBindingValue(N, S).
        if (index.has_value())
            return m_declarative_record->get_binding_value_direct(vm, index.value());
        return m_declarative_record->get_binding_value(vm, name, strict);
    }

    // 3. Let ObjRec be envRec.[[ObjectRecord]].
    // 4. Return ? ObjRec.GetBindingValue(N, S).
    return m_object_record->get_binding_value(vm, name, strict);
}

// 9.1.1.4.7 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-global-environment-records-deletebinding-n
ThrowCompletionOr<bool> GlobalEnvironment::delete_binding(VM& vm, FlyString const& name)
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. If ! DclRec.HasBinding(N) is true, then
    if (MUST(m_declarative_record->has_binding(name))) {
        // a. Return ! DclRec.DeleteBinding(N).
        return MUST(m_declarative_record->delete_binding(vm, name));
    }

    // 3. Let ObjRec be envRec.[[ObjectRecord]].
    // 4. Let globalObject be ObjRec.[[BindingObject]].

    // 5. Let existingProp be ? HasOwnProperty(globalObject, N).
    bool existing_prop = TRY(m_object_record->binding_object().has_own_property(name));

    // 6. If existingProp is true, then
    if (existing_prop) {
        // a. Let status be ? ObjRec.DeleteBinding(N).
        bool status = TRY(m_object_record->delete_binding(vm, name));

        // b. If status is true, then
        if (status) {
            // i. Let varNames be envRec.[[VarNames]].
            // ii. If N is an element of varNames, remove that element from the varNames.
            m_var_names.remove_all_matching([&](auto& entry) { return entry == name; });
        }

        // c. Return status.
        return status;
    }

    // 7. Return true.
    return true;
}

// 9.1.1.4.12 HasVarDeclaration ( N ), https://tc39.es/ecma262/#sec-hasvardeclaration
bool GlobalEnvironment::has_var_declaration(FlyString const& name) const
{
    // 1. Let varDeclaredNames be envRec.[[VarNames]].
    // 2. If varDeclaredNames contains N, return true.
    // 3. Return false.
    return m_var_names.contains_slow(name);
}

// 9.1.1.4.13 HasLexicalDeclaration ( N ), https://tc39.es/ecma262/#sec-haslexicaldeclaration
bool GlobalEnvironment::has_lexical_declaration(FlyString const& name) const
{
    // 1. Let DclRec be envRec.[[DeclarativeRecord]].
    // 2. Return ! DclRec.HasBinding(N).
    return MUST(m_declarative_record->has_binding(name));
}

// 9.1.1.4.14 HasRestrictedGlobalProperty ( N ), https://tc39.es/ecma262/#sec-hasrestrictedglobalproperty
ThrowCompletionOr<bool> GlobalEnvironment::has_restricted_global_property(FlyString const& name) const
{
    // 1. Let ObjRec be envRec.[[ObjectRecord]].
    // 2. Let globalObject be ObjRec.[[BindingObject]].
    auto& global_object = m_object_record->binding_object();

    // 3. Let existingProp be ? globalObject.[[GetOwnProperty]](N).
    auto existing_prop = TRY(global_object.internal_get_own_property(name));

    // 4. If existingProp is undefined, return false.
    if (!existing_prop.has_value())
        return false;

    // 5. If existingProp.[[Configurable]] is true, return false.
    if (*existing_prop->configurable)
        return false;

    // 6. Return true.
    return true;
}

// 9.1.1.4.15 CanDeclareGlobalVar ( N ), https://tc39.es/ecma262/#sec-candeclareglobalvar
ThrowCompletionOr<bool> GlobalEnvironment::can_declare_global_var(FlyString const& name) const
{
    // 1. Let ObjRec be envRec.[[ObjectRecord]].
    // 2. Let globalObject be ObjRec.[[BindingObject]].
    auto& global_object = m_object_record->binding_object();

    // 3. Let hasProperty be ? HasOwnProperty(globalObject, N).
    bool has_property = TRY(global_object.has_own_property(name));

    // 4. If hasProperty is true, return true.
    if (has_property)
        return true;

    // 5. Return ? IsExtensible(globalObject).
    return global_object.is_extensible();
}

// 9.1.1.4.16 CanDeclareGlobalFunction ( N ), https://tc39.es/ecma262/#sec-candeclareglobalfunction
ThrowCompletionOr<bool> GlobalEnvironment::can_declare_global_function(FlyString const& name) const
{
    // 1. Let ObjRec be envRec.[[ObjectRecord]].
    // 2. Let globalObject be ObjRec.[[BindingObject]].
    auto& global_object = m_object_record->binding_object();

    // 3. Let existingProp be ? globalObject.[[GetOwnProperty]](N).
    auto existing_prop = TRY(global_object.internal_get_own_property(name));

    // 4. If existingProp is undefined, return ? IsExtensible(globalObject).
    if (!existing_prop.has_value())
        return TRY(global_object.is_extensible());

    // 5. If existingProp.[[Configurable]] is true, return true.
    if (*existing_prop->configurable)
        return true;

    // 6. If IsDataDescriptor(existingProp) is true and existingProp has attribute values { [[Writable]]: true, [[Enumerable]]: true }, return true.
    if (existing_prop->is_data_descriptor() && *existing_prop->writable && *existing_prop->enumerable)
        return true;

    // 7. Return false.
    return false;
}

// 9.1.1.4.17 CreateGlobalVarBinding ( N, D ), https://tc39.es/ecma262/#sec-createglobalvarbinding
ThrowCompletionOr<void> GlobalEnvironment::create_global_var_binding(FlyString const& name, bool can_be_deleted)
{
    auto& vm = this->vm();

    // 1. Let ObjRec be envRec.[[ObjectRecord]].
    // 2. Let globalObject be ObjRec.[[BindingObject]].
    auto& global_object = m_object_record->binding_object();

    // 3. Let hasProperty be ? HasOwnProperty(globalObject, N).
    auto has_property = TRY(global_object.has_own_property(name));

    // 4. Let extensible be ? IsExtensible(globalObject).
    auto extensible = TRY(global_object.is_extensible());

    // 5. If hasProperty is false and extensible is true, then
    if (!has_property && extensible) {
        // a. Perform ? ObjRec.CreateMutableBinding(N, D).
        TRY(m_object_record->create_mutable_binding(vm, name, can_be_deleted));

        // b. Perform ? ObjRec.InitializeBinding(N, undefined, normal).
        TRY(m_object_record->initialize_binding(vm, name, js_undefined(), Environment::InitializeBindingHint::Normal));
    }

    // 6. Let varDeclaredNames be envRec.[[VarNames]].
    // 7. If varDeclaredNames does not contain N, then
    if (!m_var_names.contains_slow(name)) {
        // a. Append N to varDeclaredNames.
        m_var_names.append(name);
    }

    // 8. Return unused.
    return {};
}

// 9.1.1.4.18 CreateGlobalFunctionBinding ( N, V, D ), https://tc39.es/ecma262/#sec-createglobalfunctionbinding
ThrowCompletionOr<void> GlobalEnvironment::create_global_function_binding(FlyString const& name, Value value, bool can_be_deleted)
{
    // 1. Let ObjRec be envRec.[[ObjectRecord]].
    // 2. Let globalObject be ObjRec.[[BindingObject]].
    auto& global_object = m_object_record->binding_object();

    // 3. Let existingProp be ? globalObject.[[GetOwnProperty]](N).
    auto existing_prop = TRY(global_object.internal_get_own_property(name));

    PropertyDescriptor desc;

    // 4. If existingProp is undefined or existingProp.[[Configurable]] is true, then
    if (!existing_prop.has_value() || *existing_prop->configurable) {
        //     a. Let desc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: D }.
        desc = { .value = value, .writable = true, .enumerable = true, .configurable = can_be_deleted };
    }
    // 5. Else,
    else {
        // a. Let desc be the PropertyDescriptor { [[Value]]: V }.
        desc = { .value = value };
    }

    // 6. Perform ? DefinePropertyOrThrow(globalObject, N, desc).
    TRY(global_object.define_property_or_throw(name, desc));

    // 7. Perform ? Set(globalObject, N, V, false).
    TRY(global_object.set(name, value, Object::ShouldThrowExceptions::Yes));

    // 8. Let varDeclaredNames be envRec.[[VarNames]].
    // 9. If varDeclaredNames does not contain N, then
    if (!m_var_names.contains_slow(name)) {
        // a. Append N to varDeclaredNames.
        m_var_names.append(name);
    }

    // 10. Return unused.
    return {};
}

}
