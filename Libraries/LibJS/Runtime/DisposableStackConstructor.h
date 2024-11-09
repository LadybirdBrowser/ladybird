/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class DisposableStackConstructor final : public NativeFunction {
    JS_OBJECT(DisposableStackConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(DisposableStackConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~DisposableStackConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject&) override;

private:
    explicit DisposableStackConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
