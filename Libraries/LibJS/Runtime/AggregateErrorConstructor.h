/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class AggregateErrorConstructor final : public NativeFunction {
    JS_OBJECT(AggregateErrorConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(AggregateErrorConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~AggregateErrorConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit AggregateErrorConstructor(Realm&);
    virtual bool has_constructor() const override { return true; }
};

}
