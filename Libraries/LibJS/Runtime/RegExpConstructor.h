/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/RegExpLegacyStaticProperties.h>

namespace JS {

class RegExpConstructor final : public NativeFunction {
    JS_OBJECT(RegExpConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(RegExpConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~RegExpConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

    RegExpLegacyStaticProperties& legacy_static_properties() { return m_legacy_static_properties; }

private:
    explicit RegExpConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(symbol_species_getter);
    JS_DECLARE_NATIVE_FUNCTION(input_getter);
    JS_DECLARE_NATIVE_FUNCTION(input_alias_getter);
    JS_DECLARE_NATIVE_FUNCTION(input_setter);
    JS_DECLARE_NATIVE_FUNCTION(input_alias_setter);
    JS_DECLARE_NATIVE_FUNCTION(last_match_getter);
    JS_DECLARE_NATIVE_FUNCTION(last_match_alias_getter);
    JS_DECLARE_NATIVE_FUNCTION(last_paren_getter);
    JS_DECLARE_NATIVE_FUNCTION(last_paren_alias_getter);
    JS_DECLARE_NATIVE_FUNCTION(left_context_getter);
    JS_DECLARE_NATIVE_FUNCTION(left_context_alias_getter);
    JS_DECLARE_NATIVE_FUNCTION(right_context_getter);
    JS_DECLARE_NATIVE_FUNCTION(right_context_alias_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_1_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_2_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_3_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_4_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_5_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_6_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_7_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_8_getter);
    JS_DECLARE_NATIVE_FUNCTION(group_9_getter);

    RegExpLegacyStaticProperties m_legacy_static_properties;
};

}
