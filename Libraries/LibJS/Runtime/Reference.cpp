/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

// 6.2.4.6 PutValue ( V, W ), https://tc39.es/ecma262/#sec-putvalue
ThrowCompletionOr<void> Reference::put_value(VM& vm, Value value)
{
    // 4. If IsUnresolvableReference(V) is true, then
    if (is_unresolvable()) {
        // a. If V.[[Strict]] is true, throw a ReferenceError exception.
        if (m_strict == Strict::Yes)
            return throw_reference_error(vm);

        // b. Let globalObj be GetGlobalObject().
        auto& global_object = vm.current_realm()->global_object();

        // c. Perform ? Set(globalObj, V.[[ReferencedName]], W, false).
        TRY(completion_from_abi<void>(js_object_set(vm_to_abi(vm), object_to_abi(global_object), property_key_to_abi(m_name), value_to_abi(value), false)));

        // Return unused.
        return {};
    }

    // 6. Else,
    // a. Let base be V.[[Base]].
    // b. Assert: base is an Environment Record.
    VERIFY(m_base_environment);

    // c. Return ? base.SetMutableBinding(V.[[ReferencedName]], W, V.[[Strict]]) (see 9.1).
    return m_base_environment->set_mutable_binding(vm, m_name.as_string(), value, m_strict == Strict::Yes);
}

Completion Reference::throw_reference_error(VM& vm) const
{
    return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, m_name.to_utf16_string());
}

// 6.2.4.5 GetValue ( V ), https://tc39.es/ecma262/#sec-getvalue
ThrowCompletionOr<Value> Reference::get_value(VM& vm) const
{
    // 3. If IsUnresolvableReference(V) is true, throw a ReferenceError exception.
    if (is_unresolvable())
        return throw_reference_error(vm);

    // 5. Else,
    // a. Let base be V.[[Base]].
    // b. Assert: base is an Environment Record.
    VERIFY(m_base_environment);

    // c. Return ? base.GetBindingValue(V.[[ReferencedName]], V.[[Strict]]) (see 9.1).
    return m_base_environment->get_binding_value(vm, m_name.as_string(), m_strict == Strict::Yes);
}

// 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
ThrowCompletionOr<bool> Reference::delete_(VM& vm)
{
    // 4. If IsUnresolvableReference(ref) is true, then
    if (is_unresolvable()) {
        // a. Assert: ref.[[Strict]] is false.
        VERIFY(m_strict == Strict::No);
        // b. Return true.
        return true;
    }

    // 6. Else,
    //    a. Let base be ref.[[Base]].
    //    b. Assert: base is an Environment Record.
    VERIFY(m_base_environment);

    //    c. Return ? base.DeleteBinding(ref.[[ReferencedName]]).
    return m_base_environment->delete_binding(vm, m_name.as_string());
}

}
