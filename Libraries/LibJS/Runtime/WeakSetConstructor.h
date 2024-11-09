/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class WeakSetConstructor final : public NativeFunction {
    JS_OBJECT(WeakSetConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(WeakSetConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~WeakSetConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject&) override;

private:
    explicit WeakSetConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
