/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Temporal {

class PlainMonthDayConstructor final : public NativeFunction {
    JS_OBJECT(PlainMonthDayConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(PlainMonthDayConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~PlainMonthDayConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit PlainMonthDayConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(from);
};

}
