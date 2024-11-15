/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::WebIDL {

bool is_buffer_source_type(JS::Value);
GC::Ptr<JS::ArrayBuffer> underlying_buffer_source(JS::Object& buffer_source);
ErrorOr<ByteBuffer> get_buffer_source_copy(JS::Object const& buffer_source);

JS::Completion call_user_object_operation(WebIDL::CallbackType& callback, String const& operation_name, Optional<JS::Value> this_argument, GC::MarkedVector<JS::Value> args);

// https://webidl.spec.whatwg.org/#call-a-user-objects-operation
template<typename... Args>
JS::Completion call_user_object_operation(WebIDL::CallbackType& callback, String const& operation_name, Optional<JS::Value> this_argument, Args&&... args)
{
    auto& function_object = callback.callback;

    GC::MarkedVector<JS::Value> arguments_list { function_object->heap() };
    (arguments_list.append(forward<Args>(args)), ...);

    return call_user_object_operation(callback, operation_name, move(this_argument), move(arguments_list));
}

JS::Completion invoke_callback(WebIDL::CallbackType& callback, Optional<JS::Value> this_argument, GC::MarkedVector<JS::Value> args);

// https://webidl.spec.whatwg.org/#invoke-a-callback-function
template<typename... Args>
JS::Completion invoke_callback(WebIDL::CallbackType& callback, Optional<JS::Value> this_argument, Args&&... args)
{
    auto& function_object = callback.callback;

    GC::MarkedVector<JS::Value> arguments_list { function_object->heap() };
    (arguments_list.append(forward<Args>(args)), ...);

    return invoke_callback(callback, move(this_argument), move(arguments_list));
}

JS::Completion construct(WebIDL::CallbackType& callback, GC::MarkedVector<JS::Value> args);

// https://webidl.spec.whatwg.org/#construct-a-callback-function
template<typename... Args>
JS::Completion construct(WebIDL::CallbackType& callback, Args&&... args)
{
    auto& function_object = callback.callback;

    GC::MarkedVector<JS::Value> arguments_list { function_object->heap() };
    (arguments_list.append(forward<Args>(args)), ...);

    return construct(callback, move(arguments_list));
}

// https://webidl.spec.whatwg.org/#abstract-opdef-integerpart
double integer_part(double n);

enum class EnforceRange {
    Yes,
    No,
};

enum class Clamp {
    Yes,
    No,
};

// https://webidl.spec.whatwg.org/#abstract-opdef-converttoint
template<Integral T>
JS::ThrowCompletionOr<T> convert_to_int(JS::VM& vm, JS::Value, EnforceRange enforce_range = EnforceRange::No, Clamp clamp = Clamp::No);

}
