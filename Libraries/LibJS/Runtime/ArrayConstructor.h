/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class ArrayConstructor final : public NativeFunction {
    JS_OBJECT(ArrayConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(ArrayConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArrayConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit ArrayConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(from);
    JS_DECLARE_NATIVE_FUNCTION(from_async);
    JS_DECLARE_NATIVE_FUNCTION(is_array);
    JS_DECLARE_NATIVE_FUNCTION(of);

    JS_DECLARE_NATIVE_FUNCTION(symbol_species_getter);
};

}
