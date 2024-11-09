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
    JS_DECLARE_ALLOCATOR(BoundFunction);

public:
    static ThrowCompletionOr<NonnullGCPtr<BoundFunction>> create(Realm&, FunctionObject& target_function, Value bound_this, Vector<Value> bound_arguments);

    virtual ~BoundFunction() override = default;

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> internal_construct(ReadonlySpan<Value> arguments_list, FunctionObject& new_target) override;

    virtual DeprecatedFlyString const& name() const override { return m_name; }
    virtual bool is_strict_mode() const override { return m_bound_target_function->is_strict_mode(); }
    virtual bool has_constructor() const override { return m_bound_target_function->has_constructor(); }

    FunctionObject& bound_target_function() const { return *m_bound_target_function; }
    Value bound_this() const { return m_bound_this; }
    Vector<Value> const& bound_arguments() const { return m_bound_arguments; }

private:
    BoundFunction(Realm&, FunctionObject& target_function, Value bound_this, Vector<Value> bound_arguments, Object* prototype);

    virtual void visit_edges(Visitor&) override;

    GCPtr<FunctionObject> m_bound_target_function; // [[BoundTargetFunction]]
    Value m_bound_this;                            // [[BoundThis]]
    Vector<Value> m_bound_arguments;               // [[BoundArguments]]

    DeprecatedFlyString m_name;
};

}
