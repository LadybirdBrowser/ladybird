/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class AsyncDisposableStackConstructor final : public NativeFunction {
    JS_OBJECT(AsyncDisposableStackConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(AsyncDisposableStackConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~AsyncDisposableStackConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject&) override;

private:
    explicit AsyncDisposableStackConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
