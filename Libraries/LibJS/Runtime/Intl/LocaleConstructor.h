/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class LocaleConstructor final : public NativeFunction {
    JS_OBJECT(LocaleConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(LocaleConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~LocaleConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;

private:
    explicit LocaleConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }
};

}
