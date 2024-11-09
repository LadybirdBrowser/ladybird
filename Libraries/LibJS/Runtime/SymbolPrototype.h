/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class SymbolPrototype final : public Object {
    JS_OBJECT(SymbolPrototype, Object);
    JS_DECLARE_ALLOCATOR(SymbolPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SymbolPrototype() override = default;

private:
    explicit SymbolPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(description_getter);

    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
    JS_DECLARE_NATIVE_FUNCTION(symbol_to_primitive);
};

}
