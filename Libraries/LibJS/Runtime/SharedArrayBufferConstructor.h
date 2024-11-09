/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class SharedArrayBufferConstructor final : public NativeFunction {
    JS_OBJECT(SharedArrayBufferConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(SharedArrayBufferConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~SharedArrayBufferConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit SharedArrayBufferConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(symbol_species_getter);
};

}
