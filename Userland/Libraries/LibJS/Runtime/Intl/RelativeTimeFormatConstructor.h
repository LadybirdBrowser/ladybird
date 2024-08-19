/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class RelativeTimeFormatConstructor final : public NativeFunction {
    JS_OBJECT(RelativeTimeFormatConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(RelativeTimeFormatConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~RelativeTimeFormatConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;

private:
    explicit RelativeTimeFormatConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(supported_locales_of);
};

}
