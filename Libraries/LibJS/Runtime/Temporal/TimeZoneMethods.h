/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>

namespace JS::Temporal {

// 11.5.1 Time Zone Methods Records, https://tc39.es/proposal-temporal/#sec-temporal-time-zone-methods-records
struct TimeZoneMethods {
    // The time zone object, or a string indicating a built-in time zone.
    Variant<String, NonnullGCPtr<Object>> receiver; // [[Reciever]]

    // The time zone's getOffsetNanosecondsFor method. For a built-in time zone this is always %Temporal.TimeZone.prototype.getOffsetNanosecondsFor%.
    GCPtr<FunctionObject> get_offset_nanoseconds_for; // [[GetOffsetNanosecondsFor]]

    // The time zone's getPossibleInstantsFor method. For a built-in time zone this is always %Temporal.TimeZone.prototype.getPossibleInstantsFor%.
    GCPtr<FunctionObject> get_possible_instants_for; // [[GetPossibleInstantsFor]]
};

#define JS_ENUMERATE_TIME_ZONE_METHODS                                                           \
    __JS_ENUMERATE(GetOffsetNanosecondsFor, getOffsetNanosecondsFor, get_offset_nanoseconds_for) \
    __JS_ENUMERATE(GetPossibleInstantsFor, getPossibleInstantsFor, get_possible_instants_for)

enum class TimeZoneMethod {
#define __JS_ENUMERATE(PascalName, camelName, snake_name) \
    PascalName,
    JS_ENUMERATE_TIME_ZONE_METHODS
#undef __JS_ENUMERATE
};

ThrowCompletionOr<void> time_zone_methods_record_lookup(VM&, TimeZoneMethods&, TimeZoneMethod);
ThrowCompletionOr<TimeZoneMethods> create_time_zone_methods_record(VM&, Variant<String, NonnullGCPtr<Object>> time_zone, ReadonlySpan<TimeZoneMethod>);
bool time_zone_methods_record_has_looked_up(TimeZoneMethods const&, TimeZoneMethod);
bool time_zone_methods_record_is_builtin(TimeZoneMethods const&);
ThrowCompletionOr<Value> time_zone_methods_record_call(VM&, TimeZoneMethods const&, TimeZoneMethod, ReadonlySpan<Value> arguments);

}
