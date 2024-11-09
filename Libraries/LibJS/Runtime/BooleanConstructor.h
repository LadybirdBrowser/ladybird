/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class BooleanConstructor final : public NativeFunction {
    JS_OBJECT(BooleanConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(BooleanConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~BooleanConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit BooleanConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
