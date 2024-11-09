/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class CollatorConstructor final : public NativeFunction {
    JS_OBJECT(CollatorConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(CollatorConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~CollatorConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit CollatorConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(supported_locales_of);
};

}
