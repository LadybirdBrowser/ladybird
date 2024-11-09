/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/FinalizationRegistry.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class DisposableStackPrototype final : public PrototypeObject<DisposableStackPrototype, DisposableStack> {
    JS_PROTOTYPE_OBJECT(DisposableStackPrototype, DisposableStack, DisposableStack);
    JS_DECLARE_ALLOCATOR(DisposableStackPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DisposableStackPrototype() override = default;

private:
    explicit DisposableStackPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(disposed_getter);
    JS_DECLARE_NATIVE_FUNCTION(dispose);
    JS_DECLARE_NATIVE_FUNCTION(use);
    JS_DECLARE_NATIVE_FUNCTION(adopt);
    JS_DECLARE_NATIVE_FUNCTION(defer);
    JS_DECLARE_NATIVE_FUNCTION(move_);
};

}
