/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>

namespace JS::Temporal {

class TimeZonePrototype final : public PrototypeObject<TimeZonePrototype, TimeZone> {
    JS_PROTOTYPE_OBJECT(TimeZonePrototype, TimeZone, Temporal.TimeZone);
    JS_DECLARE_ALLOCATOR(TimeZonePrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~TimeZonePrototype() override = default;

private:
    explicit TimeZonePrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(id_getter);
    JS_DECLARE_NATIVE_FUNCTION(get_offset_nanoseconds_for);
    JS_DECLARE_NATIVE_FUNCTION(get_offset_string_for);
    JS_DECLARE_NATIVE_FUNCTION(get_plain_date_time_for);
    JS_DECLARE_NATIVE_FUNCTION(get_instant_for);
    JS_DECLARE_NATIVE_FUNCTION(get_possible_instants_for);
    JS_DECLARE_NATIVE_FUNCTION(get_next_transition);
    JS_DECLARE_NATIVE_FUNCTION(get_previous_transition);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_json);
};

}
