/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class ProxyConstructor final : public NativeFunction {
    JS_OBJECT(ProxyConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(ProxyConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~ProxyConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit ProxyConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(revocable);
};

}
