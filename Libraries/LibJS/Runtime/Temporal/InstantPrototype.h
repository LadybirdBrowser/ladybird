/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/Instant.h>

namespace JS::Temporal {

class InstantPrototype final : public PrototypeObject<InstantPrototype, Instant> {
    JS_PROTOTYPE_OBJECT(InstantPrototype, Instant, Temporal.Instant);
    JS_DECLARE_ALLOCATOR(InstantPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~InstantPrototype() override = default;

private:
    explicit InstantPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(epoch_seconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(epoch_milliseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(epoch_microseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(epoch_nanoseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(add);
    JS_DECLARE_NATIVE_FUNCTION(subtract);
    JS_DECLARE_NATIVE_FUNCTION(until);
    JS_DECLARE_NATIVE_FUNCTION(since);
    JS_DECLARE_NATIVE_FUNCTION(round);
    JS_DECLARE_NATIVE_FUNCTION(equals);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(to_json);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
    JS_DECLARE_NATIVE_FUNCTION(to_zoned_date_time);
    JS_DECLARE_NATIVE_FUNCTION(to_zoned_date_time_iso);
};

}
