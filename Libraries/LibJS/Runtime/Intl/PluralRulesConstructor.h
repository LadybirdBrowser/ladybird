/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class PluralRulesConstructor final : public NativeFunction {
    JS_OBJECT(PluralRulesConstructor, NativeFunction);
    GC_DECLARE_ALLOCATOR(PluralRulesConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~PluralRulesConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target) override;

private:
    explicit PluralRulesConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(supported_locales_of);
};

}
