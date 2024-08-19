/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/TimeZonePrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(TimeZonePrototype);

// 11.4 Properties of the Temporal.TimeZone Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-timezone-prototype-object
TimeZonePrototype::TimeZonePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void TimeZonePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_accessor(realm, vm.names.id, id_getter, {}, Attribute::Configurable);
    define_native_function(realm, vm.names.getOffsetNanosecondsFor, get_offset_nanoseconds_for, 1, attr);
    define_native_function(realm, vm.names.getOffsetStringFor, get_offset_string_for, 1, attr);
    define_native_function(realm, vm.names.getPlainDateTimeFor, get_plain_date_time_for, 1, attr);
    define_native_function(realm, vm.names.getInstantFor, get_instant_for, 1, attr);
    define_native_function(realm, vm.names.getPossibleInstantsFor, get_possible_instants_for, 1, attr);
    define_native_function(realm, vm.names.getNextTransition, get_next_transition, 1, attr);
    define_native_function(realm, vm.names.getPreviousTransition, get_previous_transition, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);

    // 11.4.2 Temporal.TimeZone.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.TimeZone"_string), Attribute::Configurable);
}

// 11.4.3 get Temporal.TimeZone.prototype.id, https://tc39.es/proposal-temporal/#sec-get-temporal.timezone.prototype.id
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::id_getter)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Return timeZone.[[Identifier]].
    return PrimitiveString::create(vm, time_zone->identifier());
}

// 11.4.4 Temporal.TimeZone.prototype.getOffsetNanosecondsFor ( instant ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getoffsetnanosecondsfor
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_offset_nanoseconds_for)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set instant to ? ToTemporalInstant(instant).
    auto* instant = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. If timeZone.[[OffsetNanoseconds]] is not undefined, return 𝔽(timeZone.[[OffsetNanoseconds]]).
    if (time_zone->offset_nanoseconds().has_value())
        return Value(*time_zone->offset_nanoseconds());

    // 5. Return 𝔽(GetNamedTimeZoneOffsetNanoseconds(timeZone.[[Identifier]], instant.[[Nanoseconds]])).
    auto offset = get_named_time_zone_offset_nanoseconds(time_zone->identifier(), instant->nanoseconds().big_integer());
    return Value(static_cast<double>(offset.offset.to_nanoseconds()));
}

// 11.4.5 Temporal.TimeZone.prototype.getOffsetStringFor ( instant ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getoffsetstringfor
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_offset_string_for)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set instant to ? ToTemporalInstant(instant).
    auto* instant = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. Return ? BuiltinTimeZoneGetOffsetStringFor(timeZone, instant).
    auto offset_string = TRY(builtin_time_zone_get_offset_string_for(vm, time_zone, *instant));
    return PrimitiveString::create(vm, move(offset_string));
}

// 11.4.6 Temporal.TimeZone.prototype.getPlainDateTimeFor ( instant [ , calendarLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getplaindatetimefor
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_plain_date_time_for)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set instant to ? ToTemporalInstant(instant).
    auto* instant = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
    auto* calendar = TRY(to_temporal_calendar_with_iso_default(vm, vm.argument(1)));

    // 5. Return ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    return TRY(builtin_time_zone_get_plain_date_time_for(vm, time_zone, *instant, *calendar));
}

// 11.4.7 Temporal.TimeZone.prototype.getInstantFor ( dateTime [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getinstantfor
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_instant_for)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set dateTime to ? ToTemporalDateTime(dateTime).
    auto* date_time = TRY(to_temporal_date_time(vm, vm.argument(0)));

    // 4. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 5. Let disambiguation be ? ToTemporalDisambiguation(options).
    auto disambiguation = TRY(to_temporal_disambiguation(vm, options));

    // 6. Return ? BuiltinTimeZoneGetInstantFor(timeZone, dateTime, disambiguation).
    return TRY(builtin_time_zone_get_instant_for(vm, time_zone, *date_time, disambiguation));
}

// 11.4.8 Temporal.TimeZone.prototype.getPossibleInstantsFor ( dateTime ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getpossibleinstantsfor
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_possible_instants_for)
{
    auto& realm = *vm.current_realm();

    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimezone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set dateTime to ? ToTemporalDateTime(dateTime).
    auto* date_time = TRY(to_temporal_date_time(vm, vm.argument(0)));

    Vector<Crypto::SignedBigInteger> possible_epoch_nanoseconds;

    // 4. If timeZone.[[OffsetNanoseconds]] is not undefined, then
    if (time_zone->offset_nanoseconds().has_value()) {
        // a. Let epochNanoseconds be GetUTCEpochNanoseconds(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]]).
        auto epoch_nanoseconds = get_utc_epoch_nanoseconds(date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond());

        // b. Let possibleEpochNanoseconds be « epochNanoseconds - ℤ(timeZone.[[OffsetNanoseconds]]) ».
        possible_epoch_nanoseconds.append(epoch_nanoseconds.minus(Crypto::SignedBigInteger { *time_zone->offset_nanoseconds() }));
    }
    // 5. Else,
    else {
        // a. Let possibleEpochNanoseconds be GetNamedTimeZoneEpochNanoseconds(timeZone.[[Identifier]], dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]]).
        possible_epoch_nanoseconds = get_named_time_zone_epoch_nanoseconds(time_zone->identifier(), date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond());
    }

    // 6. Let possibleInstants be a new empty List.
    auto possible_instants = GC::MarkedVector<Value> { vm.heap() };

    // 7. For each value epochNanoseconds in possibleEpochNanoseconds, do
    for (auto& epoch_nanoseconds : possible_epoch_nanoseconds) {
        // a. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

        // b. Let instant be ! CreateTemporalInstant(epochNanoseconds).
        auto epoch_nanoseconds_bigint = BigInt::create(vm, move(epoch_nanoseconds));
        auto* instant = MUST(create_temporal_instant(vm, epoch_nanoseconds_bigint));

        // c. Append instant to possibleInstants.
        possible_instants.append(instant);
    }

    // 8. Return CreateArrayFromList(possibleInstants).
    return Array::create_from(realm, possible_instants);
}

// 11.4.9 Temporal.TimeZone.prototype.getNextTransition ( startingPoint ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getnexttransition
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_next_transition)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set startingPoint to ? ToTemporalInstant(startingPoint).
    auto* starting_point = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. If timeZone.[[OffsetNanoseconds]] is not undefined, return null.
    if (!time_zone->offset_nanoseconds().has_value())
        return js_null();

    // 5. Let transition be GetNamedTimeZoneNextTransition(timeZone.[[Identifier]], startingPoint.[[Nanoseconds]]).
    auto* transition = get_named_time_zone_next_transition(vm, time_zone->identifier(), starting_point->nanoseconds());

    // 6. If transition is null, return null.
    if (!transition)
        return js_null();

    // 7. Return ! CreateTemporalInstant(transition).
    return MUST(create_temporal_instant(vm, *transition));
}

// 11.4.10 Temporal.TimeZone.prototype.getPreviousTransition ( startingPoint ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.getprevioustransition
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::get_previous_transition)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Set startingPoint to ? ToTemporalInstant(startingPoint).
    auto* starting_point = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. If timeZone.[[OffsetNanoseconds]] is not undefined, return null.
    if (!time_zone->offset_nanoseconds().has_value())
        return js_null();

    // 5. Let transition be GetNamedTimeZonePreviousTransition(timeZone.[[Identifier]], startingPoint.[[Nanoseconds]]).
    auto* transition = get_named_time_zone_previous_transition(vm, time_zone->identifier(), starting_point->nanoseconds());

    // 6. If transition is null, return null.
    if (!transition)
        return js_null();

    // 7. Return ! CreateTemporalInstant(transition).
    return MUST(create_temporal_instant(vm, *transition));
}

// 11.4.11 Temporal.TimeZone.prototype.toString ( ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::to_string)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Return timeZone.[[Identifier]].
    return PrimitiveString::create(vm, time_zone->identifier());
}

// 11.4.12 Temporal.TimeZone.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(TimeZonePrototype::to_json)
{
    // 1. Let timeZone be the this value.
    // 2. Perform ? RequireInternalSlot(timeZone, [[InitializedTemporalTimeZone]]).
    auto time_zone = TRY(typed_this_object(vm));

    // 3. Return ? ToString(timeZone).
    return PrimitiveString::create(vm, TRY(Value(time_zone).to_string(vm)));
}

}
