/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Temporal {

class PlainTimeConstructor final : public NativeFunction {
    JS_OBJECT(PlainTimeConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(PlainTimeConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~PlainTimeConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit PlainTimeConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(from);
    JS_DECLARE_NATIVE_FUNCTION(compare);
};

}
