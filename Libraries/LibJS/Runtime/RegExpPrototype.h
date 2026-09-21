/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/RegExpObject.h>

namespace JS {

ThrowCompletionOr<Value> regexp_exec(VM&, Object& regexp_object, GC::Ref<PrimitiveString> string);
size_t advance_string_index(Utf16View const& string, size_t index, bool unicode);

class RegExpPrototype final : public PrototypeObject<RegExpPrototype, RegExpObject> {
    JS_PROTOTYPE_OBJECT(RegExpPrototype, RegExpObject, RegExp);
    GC_DECLARE_ALLOCATOR(RegExpPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~RegExpPrototype() override = default;

    static ThrowCompletionOr<Value> symbol_replace_impl(VM&, Object& regexp_object, GC::Ref<PrimitiveString> string, Value replace_value);
    static ThrowCompletionOr<Value> symbol_split_impl(VM&, Object& regexp_object, GC::Ref<PrimitiveString> string, Value limit_value);

private:
    explicit RegExpPrototype(Realm&);

    // A fast path may only run when every operation it skips would have been unobservable. These decide that — without
    // observing anything themselves: no Get, no SpeciesConstructor, just slot and pointer comparisons.
    static bool is_unmodified_regexp_instance(VM&, Realm&, Object& regexp_object);
    static bool reading_flags_is_unobservable(VM&, Realm&);
    static bool reading_last_index_is_unobservable(VM&, Object& regexp_object);
    static bool writing_last_index_is_unobservable(VM&, Object& regexp_object);
    static bool replace_is_fast_and_non_observable(VM&, Realm&, Object& regexp_object);
    static bool split_is_fast_and_non_observable(VM&, Realm&, Object& regexp_object);
    static bool test_is_fast_and_non_observable(VM&, Realm&, Object& regexp_object);

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
