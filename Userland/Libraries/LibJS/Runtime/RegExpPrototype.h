/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Utf16String.h>

namespace JS {

ThrowCompletionOr<Value> regexp_exec(VM&, Object& regexp_object, Utf16String string);
size_t advance_string_index(Utf16View const& string, size_t index, bool unicode);

class RegExpPrototype final : public PrototypeObject<RegExpPrototype, RegExpObject> {
    JS_PROTOTYPE_OBJECT(RegExpPrototype, RegExpObject, RegExp);
    JS_DECLARE_ALLOCATOR(RegExpPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~RegExpPrototype() override = default;

private:
    explicit RegExpPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(exec);
    JS_DECLARE_NATIVE_FUNCTION(flags);
    JS_DECLARE_NATIVE_FUNCTION(symbol_match);
    JS_DECLARE_NATIVE_FUNCTION(symbol_match_all);
    JS_DECLARE_NATIVE_FUNCTION(symbol_replace);
    JS_DECLARE_NATIVE_FUNCTION(symbol_search);
    JS_DECLARE_NATIVE_FUNCTION(source);
    JS_DECLARE_NATIVE_FUNCTION(symbol_split);
    JS_DECLARE_NATIVE_FUNCTION(test);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(compile);

#define __JS_ENUMERATE(FlagName, flagName, flag_name, ...) \
    JS_DECLARE_NATIVE_FUNCTION(flag_name);
    JS_ENUMERATE_REGEXP_FLAGS
#undef __JS_ENUMERATE
};

}
