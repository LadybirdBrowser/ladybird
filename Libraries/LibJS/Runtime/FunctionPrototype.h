/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

class FunctionPrototype final : public FunctionObject {
    JS_OBJECT(FunctionPrototype, FunctionObject);
    GC_DECLARE_ALLOCATOR(FunctionPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~FunctionPrototype() override = default;

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;
    virtual FlyString const& name() const override { return m_name; }

private:
    explicit FunctionPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(apply);
    JS_DECLARE_NATIVE_FUNCTION(bind);
    JS_DECLARE_NATIVE_FUNCTION(call);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(symbol_has_instance);

    // 20.2.3: The Function prototype object has a "name" property whose value is the empty String.
    FlyString m_name;
};

}
