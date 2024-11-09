/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class PromiseConstructor final : public NativeFunction {
    JS_OBJECT(PromiseConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(PromiseConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~PromiseConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit PromiseConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(all);
    JS_DECLARE_NATIVE_FUNCTION(all_settled);
    JS_DECLARE_NATIVE_FUNCTION(any);
    JS_DECLARE_NATIVE_FUNCTION(race);
    JS_DECLARE_NATIVE_FUNCTION(reject);
    JS_DECLARE_NATIVE_FUNCTION(resolve);
    JS_DECLARE_NATIVE_FUNCTION(symbol_species_getter);
    JS_DECLARE_NATIVE_FUNCTION(try_);
    JS_DECLARE_NATIVE_FUNCTION(with_resolvers);
};

}
