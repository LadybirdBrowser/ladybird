/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace Web::Bindings {

JS::Completion throw_copy_of_error_in_current_realm(JS::VM&, JS::Value error);

template<typename T>
JS::ThrowCompletionOr<T> run_in_a_new_global_object(JS::VM& vm, Function<JS::ThrowCompletionOr<T>(JS::Realm&)> const& steps)
{
    auto execution_context = MUST(JS::Realm::initialize_host_defined_realm(vm, nullptr, nullptr));
    auto result = steps(*execution_context->realm);
    auto* popped_execution_context = vm.pop_execution_context();
    VERIFY(popped_execution_context == execution_context.ptr());

    // An error that the steps throw belongs to the new global object. Recreate it in the current realm, so that no
    // object of the new global object reaches script.
    if (result.is_error())
        return throw_copy_of_error_in_current_realm(vm, result.error_value());
    return result;
}

JS::ThrowCompletionOr<Utf16String> serialize_to_json_in_a_new_global_object(JS::VM&, Function<JS::Value(JS::Realm&)> const& create_value);

}
