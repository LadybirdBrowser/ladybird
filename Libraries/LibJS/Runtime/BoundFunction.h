/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

class BoundFunction final : public FunctionObject {
    JS_OBJECT(BoundFunction, FunctionObject);
    GC_DECLARE_ALLOCATOR(BoundFunction);

public:
    static ThrowCompletionOr<GC::Ref<BoundFunction>> create(Realm&, FunctionObject& target_function, Value bound_this, Vector<Value> bound_arguments);

    virtual ~BoundFunction() override = default;

    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ReadonlySpan<Value> arguments_list, FunctionObject& new_target) override;

    virtual bool is_strict_mode() const override { return m_bound_target_function->is_strict_mode(); }
    virtual bool has_constructor() const override { return m_bound_target_function->has_constructor(); }

    FunctionObject& bound_target_function() const { return *m_bound_target_function; }
    Value bound_this() const { return m_bound_this; }
    Vector<Value> const& bound_arguments() const { return m_bound_arguments; }

private:
    BoundFunction(Realm&, FunctionObject& target_function, Value bound_this, Vector<Value> bound_arguments, Object* prototype);

    ThrowCompletionOr<void> get_stack_frame_size(size_t& registers_and_constants_and_locals_count, size_t& argument_count) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ptr<FunctionObject> m_bound_target_function; // [[BoundTargetFunction]]
    Value m_bound_this;                              // [[BoundThis]]
    Vector<Value> m_bound_arguments;                 // [[BoundArguments]]
};

}
