/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class SymbolConstructor final : public NativeFunction {
    JS_OBJECT(SymbolConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(SymbolConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~SymbolConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit SymbolConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(for_);
    JS_DECLARE_NATIVE_FUNCTION(key_for);
};

}
