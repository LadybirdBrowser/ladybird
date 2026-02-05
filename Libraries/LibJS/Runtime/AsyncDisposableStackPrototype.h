/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/AsyncDisposableStack.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class AsyncDisposableStackPrototype final : public PrototypeObject<AsyncDisposableStackPrototype, AsyncDisposableStack> {
    JS_PROTOTYPE_OBJECT(AsyncDisposableStackPrototype, AsyncDisposableStack, AsyncDisposableStack);
    GC_DECLARE_ALLOCATOR(AsyncDisposableStackPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~AsyncDisposableStackPrototype() override = default;

private:
    explicit AsyncDisposableStackPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(adopt);
    JS_DECLARE_NATIVE_FUNCTION(defer);
    JS_DECLARE_NATIVE_FUNCTION(dispose_async);
    JS_DECLARE_NATIVE_FUNCTION(disposed_getter);
    JS_DECLARE_NATIVE_FUNCTION(move_);
    JS_DECLARE_NATIVE_FUNCTION(use);
};

}
