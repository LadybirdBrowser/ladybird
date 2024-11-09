/*
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class DateConstructor final : public NativeFunction {
    JS_OBJECT(DateConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(DateConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~DateConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit DateConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(now);
    JS_DECLARE_NATIVE_FUNCTION(parse);
    JS_DECLARE_NATIVE_FUNCTION(utc);
};

}
