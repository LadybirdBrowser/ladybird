/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/Duration.h>

namespace JS::Temporal {

class DurationPrototype final : public PrototypeObject<DurationPrototype, Duration> {
    JS_PROTOTYPE_OBJECT(DurationPrototype, Duration, Temporal.Duration);
    JS_DECLARE_ALLOCATOR(DurationPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DurationPrototype() override = default;

private:
    explicit DurationPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(years_getter);
    JS_DECLARE_NATIVE_FUNCTION(months_getter);
    JS_DECLARE_NATIVE_FUNCTION(weeks_getter);
    JS_DECLARE_NATIVE_FUNCTION(days_getter);
    JS_DECLARE_NATIVE_FUNCTION(hours_getter);
    JS_DECLARE_NATIVE_FUNCTION(minutes_getter);
    JS_DECLARE_NATIVE_FUNCTION(seconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(milliseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(microseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(nanoseconds_getter);
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
    JS_DECLARE_NATIVE_FUNCTION(value_of);
};

}
