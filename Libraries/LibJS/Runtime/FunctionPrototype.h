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

    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) override;

    virtual Utf16String name_for_call_stack() const override;

private:
    explicit FunctionPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(apply);
    JS_DECLARE_NATIVE_FUNCTION(bind);
    JS_DECLARE_NATIVE_FUNCTION(call);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(symbol_has_instance);
};

}
