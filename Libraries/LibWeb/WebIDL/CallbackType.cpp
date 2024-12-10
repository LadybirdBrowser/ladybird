/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(CallbackType);

CallbackType::CallbackType(JS::Object& callback, JS::Realm& callback_context, OperationReturnsPromise operation_returns_promise)
    : callback(callback)
    , callback_context(callback_context)
    , operation_returns_promise(operation_returns_promise)
{
}

void CallbackType::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(callback_context);
}

// Non-standard function to aid in converting a user-provided function into a WebIDL::Callback. This is essentially
// what the Bindings generator would do at compile time, but at runtime instead.
JS::ThrowCompletionOr<GC::Root<CallbackType>> property_to_callback(JS::VM& vm, JS::Value value, JS::PropertyKey const& property_key, OperationReturnsPromise operation_returns_promise)
{
    auto property = TRY(value.get(vm, property_key));

    if (property.is_undefined())
        return GC::Root<CallbackType> {};

    if (!property.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, property.to_string_without_side_effects());

    return vm.heap().allocate<CallbackType>(property.as_object(), HTML::incumbent_realm(), operation_returns_promise);
}

}
