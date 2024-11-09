/*
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class BigIntConstructor final : public NativeFunction {
    JS_OBJECT(BigIntConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(BigIntConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~BigIntConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit BigIntConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(as_int_n);
    JS_DECLARE_NATIVE_FUNCTION(as_uint_n);
};

}
