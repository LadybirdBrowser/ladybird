/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

class WrappedFunction final : public FunctionObject {
    JS_OBJECT(WrappedFunction, FunctionObject);
    GC_DECLARE_ALLOCATOR(WrappedFunction);

public:
    static ThrowCompletionOr<GC::Ref<WrappedFunction>> create(Realm&, Realm& caller_realm, FunctionObject& target_function);

    virtual ~WrappedFunction() = default;

    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) override;

    virtual Realm* realm() const override { return m_realm; }

    FunctionObject const& wrapped_target_function() const { return m_wrapped_target_function; }
    FunctionObject& wrapped_target_function() { return m_wrapped_target_function; }

    virtual ThrowCompletionOr<void> get_stack_frame_size(size_t& registers_and_constants_and_locals_count, size_t& argument_count) override;

    virtual Utf16String name_for_call_stack() const override;

private:
    WrappedFunction(Realm&, FunctionObject&, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // Internal Slots of Wrapped Function Exotic Objects, https://tc39.es/proposal-shadowrealm/#table-internal-slots-of-wrapped-function-exotic-objects
    GC::Ref<FunctionObject> m_wrapped_target_function; // [[WrappedTargetFunction]]
    GC::Ref<Realm> m_realm;                            // [[Realm]]
};

ThrowCompletionOr<Value> ordinary_wrapped_function_call(WrappedFunction&, Value this_argument, Span<Value> arguments_list);
void prepare_for_wrapped_function_call(WrappedFunction&, ExecutionContext& callee_context);

}
