/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/Duration.h>

namespace JS::Temporal {

class DurationPrototype final : public PrototypeObject<DurationPrototype, Duration> {
    JS_PROTOTYPE_OBJECT(DurationPrototype, Duration, Temporal.Duration);
    GC_DECLARE_ALLOCATOR(DurationPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DurationPrototype() override = default;

private:
    explicit DurationPrototype(Realm&);

#define __JS_ENUMERATE(unit) \
    JS_DECLARE_NATIVE_FUNCTION(unit##_getter);
    JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE

    JS_DECLARE_NATIVE_FUNCTION(sign_getter);
    JS_DECLARE_NATIVE_FUNCTION(blank_getter);
    JS_DECLARE_NATIVE_FUNCTION(with);
    JS_DECLARE_NATIVE_FUNCTION(negated);
    JS_DECLARE_NATIVE_FUNCTION(abs);
    JS_DECLARE_NATIVE_FUNCTION(add);
    JS_DECLARE_NATIVE_FUNCTION(subtract);
    JS_DECLARE_NATIVE_FUNCTION(round);
    JS_DECLARE_NATIVE_FUNCTION(total);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_json);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
};

}
