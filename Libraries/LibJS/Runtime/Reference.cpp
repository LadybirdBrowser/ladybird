/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Reference.h>

namespace JS {

// 6.2.4.6 PutValue ( V, W ), https://tc39.es/ecma262/#sec-putvalue
ThrowCompletionOr<void> Reference::put_value(VM& vm, Value value)
{
    // 1. ReturnIfAbrupt(V).
    // 2. ReturnIfAbrupt(W).

    // 3. If V is not a Reference Record, throw a ReferenceError exception.
    if (!is_valid_reference())
        return vm.throw_completion<ReferenceError>(ErrorType::InvalidLeftHandAssignment);

    // 4. If IsUnresolvableReference(V) is true, then
    if (is_unresolvable()) {
        // a. If V.[[Strict]] is true, throw a ReferenceError exception.
        if (m_strict)
            return throw_reference_error(vm);

        // b. Let globalObj be GetGlobalObject().
        auto& global_object = vm.get_global_object();

        // c. Perform ? Set(globalObj, V.[[ReferencedName]], W, false).
        TRY(global_object.set(name(), value, Object::ShouldThrowExceptions::No));

        // Return unused.
        return {};
    }

    // 5. If IsPropertyReference(V) is true, then
    if (is_property_reference()) {
        // a. Let baseObj be ? ToObject(V.[[Base]]).
        auto base_obj = TRY(m_base_value.to_object(vm));

        // b. If IsPrivateReference(V) is true, then
        if (is_private_reference()) {
            // i. Return ? PrivateSet(baseObj, V.[[ReferencedName]], W).
            return base_obj->private_set(private_name(), value);
        }

        // c. Let succeeded be ? baseObj.[[Set]](V.[[ReferencedName]], W, GetThisValue(V)).
        auto succeeded = TRY(base_obj->internal_set(name(), value, get_this_value()));

        // d. If succeeded is false and V.[[Strict]] is true, throw a TypeError exception.
        if (!succeeded && m_strict)
            return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name(), m_base_value.to_string_without_side_effects());

        // e. Return unused.
        return {};
    }

    // 6. Else,
    // a. Let base be V.[[Base]].

    // b. Assert: base is an Environment Record.
    VERIFY(m_base_type == BaseType::Environment);
    VERIFY(m_base_environment);

    // c. Return ? base.SetMutableBinding(V.[[ReferencedName]], W, V.[[Strict]]) (see 9.1).
    if (m_environment_coordinate.has_value())
        return static_cast<DeclarativeEnvironment*>(m_base_environment)->set_mutable_binding_direct(vm, m_environment_coordinate->index, value, m_strict);
    else
        return m_base_environment->set_mutable_binding(vm, name().as_string(), value, m_strict);
}

Completion Reference::throw_reference_error(VM& vm) const
{
    if (is_private_reference())
        return vm.throw_completion<ReferenceError>(ErrorType::ReferenceUnresolvable);
    else
        return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, name().to_string_or_symbol().to_display_string());
}

// 6.2.4.5 GetValue ( V ), https://tc39.es/ecma262/#sec-getvalue
ThrowCompletionOr<Value> Reference::get_value(VM& vm) const
{
    auto& realm = *vm.current_realm();

    // 1. ReturnIfAbrupt(V).
    // 2. If V is not a Reference Record, return V.

    // 3. If IsUnresolvableReference(V) is true, throw a ReferenceError exception.
    if (!is_valid_reference() || is_unresolvable())
        return throw_reference_error(vm);

    // 4. If IsPropertyReference(V) is true, then
    if (is_property_reference()) {
        // a. Let baseObj be ? ToObject(V.[[Base]]).
        // NOTE: Deferred as an optimization; we might not actually need to create an object.

        // b. If IsPrivateReference(V) is true, then
        if (is_private_reference()) {
            // FIXME: We need to be able to specify the receiver for this
            // if we want to use it in error messages in future
            // as things currently stand this does the "wrong thing" but
            // the error is unobservable

            auto base_obj = TRY(m_base_value.to_object(vm));

            // i. Return ? PrivateGet(baseObj, V.[[ReferencedName]]).
            return base_obj->private_get(private_name());
        }

        // OPTIMIZATION: For various primitives we can avoid actually creating a new object for them.
        GC::Ptr<Object> base_obj;
        if (m_base_value.is_string()) {
            auto string_value = TRY(m_base_value.as_string().get(vm, name()));
            if (string_value.has_value())
                return *string_value;
            base_obj = realm.intrinsics().string_prototype();
        } else if (m_base_value.is_number())
            base_obj = realm.intrinsics().number_prototype();
        else if (m_base_value.is_boolean())
            base_obj = realm.intrinsics().boolean_prototype();
        else if (m_base_value.is_bigint())
            base_obj = realm.intrinsics().bigint_prototype();
        else if (m_base_value.is_symbol())
            base_obj = realm.intrinsics().symbol_prototype();
        else
            base_obj = TRY(m_base_value.to_object(vm));

        // c. Return ? baseObj.[[Get]](V.[[ReferencedName]], GetThisValue(V)).
        return base_obj->internal_get(name(), get_this_value());
    }

    // 5. Else,
    // a. Let base be V.[[Base]].

    // b. Assert: base is an Environment Record.
    VERIFY(m_base_type == BaseType::Environment);
    VERIFY(m_base_environment);

    // c. Return ? base.GetBindingValue(V.[[ReferencedName]], V.[[Strict]]) (see 9.1).
    if (m_environment_coordinate.has_value())
        return static_cast<DeclarativeEnvironment*>(m_base_environment)->get_binding_value_direct(vm, m_environment_coordinate->index);
    return m_base_environment->get_binding_value(vm, name().as_string(), m_strict);
}

// 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
ThrowCompletionOr<bool> Reference::delete_(VM& vm)
{
    // 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
    // UnaryExpression : delete UnaryExpression

    // NOTE: The following steps have already been evaluated by the time we get here:
    // 1. Let ref be the result of evaluating UnaryExpression.
    // 2. ReturnIfAbrupt(ref).
    // 3. If ref is not a Reference Record, return true.

    // 4. If IsUnresolvableReference(ref) is true, then
    if (is_unresolvable()) {
        // a. Assert: ref.[[Strict]] is false.
        VERIFY(!m_strict);
        // b. Return true.
        return true;
    }

    // 5. If IsPropertyReference(ref) is true, then
    if (is_property_reference()) {
        // a. Assert: IsPrivateReference(ref) is false.
        VERIFY(!is_private_reference());

        // b. If IsSuperReference(ref) is true, throw a ReferenceError exception.
        if (is_super_reference())
            return vm.throw_completion<ReferenceError>(ErrorType::UnsupportedDeleteSuperProperty);

        // c. Let baseObj be ? ToObject(ref.[[Base]]).
        auto base_obj = TRY(m_base_value.to_object(vm));

        // d. Let deleteStatus be ? baseObj.[[Delete]](ref.[[ReferencedName]]).
        bool delete_status = TRY(base_obj->internal_delete(name()));

        // e. If deleteStatus is false and ref.[[Strict]] is true, throw a TypeError exception.
        if (!delete_status && m_strict)
            return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishDeleteProperty, name(), m_base_value.to_string_without_side_effects());

        // f. Return deleteStatus.
        return delete_status;
    }

    // 6. Else,
    //    a. Let base be ref.[[Base]].
    //    b. Assert: base is an Environment Record.

    VERIFY(m_base_type == BaseType::Environment);

    //    c. Return ? base.DeleteBinding(ref.[[ReferencedName]]).
    return m_base_environment->delete_binding(vm, name().as_string());
}

// 6.2.4.8 InitializeReferencedBinding ( V, W ), https://tc39.es/ecma262/#sec-object.prototype.hasownproperty
// 1.2.1.1 InitializeReferencedBinding ( V, W, hint ), https://tc39.es/proposal-explicit-resource-management/#sec-initializereferencedbinding
ThrowCompletionOr<void> Reference::initialize_referenced_binding(VM& vm, Value value, Environment::InitializeBindingHint hint) const
{
    VERIFY(!is_unresolvable());
    VERIFY(m_base_type == BaseType::Environment);
    return m_base_environment->initialize_binding(vm, name().as_string(), value, hint);
}

// 6.2.4.9 MakePrivateReference ( baseValue, privateIdentifier ), https://tc39.es/ecma262/#sec-makeprivatereference
Reference make_private_reference(VM& vm, Value base_value, FlyString const& private_identifier)
{
    // 1. Let privEnv be the running execution context's PrivateEnvironment.
    auto private_environment = vm.running_execution_context().private_environment;

    // 2. Assert: privEnv is not null.
    VERIFY(private_environment);

    // 3. Let privateName be ResolvePrivateIdentifier(privEnv, privateIdentifier).
    auto private_name = private_environment->resolve_private_identifier(private_identifier);

    // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: privateName, [[Strict]]: true, [[ThisValue]]: empty }.
    return Reference { base_value, private_name };
}

}
