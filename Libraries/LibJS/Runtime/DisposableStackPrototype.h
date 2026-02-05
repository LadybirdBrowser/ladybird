/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/DisposableStack.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class DisposableStackPrototype final : public PrototypeObject<DisposableStackPrototype, DisposableStack> {
    JS_PROTOTYPE_OBJECT(DisposableStackPrototype, DisposableStack, DisposableStack);
    GC_DECLARE_ALLOCATOR(DisposableStackPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DisposableStackPrototype() override = default;

private:
    explicit DisposableStackPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(adopt);
    JS_DECLARE_NATIVE_FUNCTION(defer);
    JS_DECLARE_NATIVE_FUNCTION(dispose);
    JS_DECLARE_NATIVE_FUNCTION(disposed_getter);
    JS_DECLARE_NATIVE_FUNCTION(move_);
    JS_DECLARE_NATIVE_FUNCTION(use);
};

}
