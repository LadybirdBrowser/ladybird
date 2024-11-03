/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

struct ParameterArgumentsAndBody {
    Vector<String> parameters;
    String body;
};

ThrowCompletionOr<ParameterArgumentsAndBody> extract_parameter_arguments_and_body(VM&, Span<Value> arguments);

class FunctionConstructor final : public NativeFunction {
    JS_OBJECT(FunctionConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(FunctionConstructor);

public:
    static ThrowCompletionOr<NonnullGCPtr<ECMAScriptFunctionObject>> create_dynamic_function(VM&, FunctionObject& constructor, FunctionObject* new_target, FunctionKind kind, ReadonlySpan<String> parameter_args, String const& body_string);

    virtual void initialize(Realm&) override;
    virtual ~FunctionConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit FunctionConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
