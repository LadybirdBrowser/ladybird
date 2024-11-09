/*
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class NumberConstructor final : public NativeFunction {
    JS_OBJECT(NumberConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(NumberConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~NumberConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit NumberConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(is_finite);
    JS_DECLARE_NATIVE_FUNCTION(is_integer);
    JS_DECLARE_NATIVE_FUNCTION(is_nan);
    JS_DECLARE_NATIVE_FUNCTION(is_safe_integer);
};

}
