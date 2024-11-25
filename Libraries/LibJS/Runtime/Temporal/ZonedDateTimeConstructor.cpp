/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/Temporal/ZonedDateTimeConstructor.h>
#include <Libraries/LibJS/Runtime/Intl/AbstractOperations.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(ZonedDateTimeConstructor);

// 6.1 The Temporal.ZonedDateTime Constructor, https://tc39.es/proposal-temporal/#sec-temporal-zoneddatetime-constructor
ZonedDateTimeConstructor::ZonedDateTimeConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.ZonedDateTime.as_string(), realm.intrinsics().function_prototype())
{
}

void ZonedDateTimeConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 6.2.1 Temporal.ZonedDateTime.prototype, https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_zoned_date_time_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);
}

// 6.1.1 Temporal.ZonedDateTime ( epochNanoseconds, timeZone [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime
ThrowCompletionOr<Value> ZonedDateTimeConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, then
    //     a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.ZonedDateTime");
}

// 6.1.1 Temporal.ZonedDateTime ( epochNanoseconds, timeZone [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime
ThrowCompletionOr<GC::Ref<Object>> ZonedDateTimeConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto epoch_nanoseconds_value = vm.argument(0);
    auto time_zone_value = vm.argument(1);
    auto calendar_value = vm.argument(2);

    // 2. Set epochNanoseconds to ? ToBigInt(epochNanoseconds).
    auto epoch_nanoseconds = TRY(epoch_nanoseconds_value.to_bigint(vm));

    // 3. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds->big_integer()))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 4. If timeZone is not a String, throw a TypeError exception.
    if (!time_zone_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, time_zone_value);

    // 5. Let timeZoneParse be ? ParseTimeZoneIdentifier(timeZone).
    auto time_zone_parse = TRY(parse_time_zone_identifier(vm, time_zone_value.as_string().utf8_string_view()));

    String time_zone;

    // 6. If timeZoneParse.[[OffsetMinutes]] is EMPTY, then
    if (!time_zone_parse.offset_minutes.has_value()) {
        // a. Let identifierRecord be GetAvailableNamedTimeZoneIdentifier(timeZoneParse.[[Name]]).
        auto identifier_record = Intl::get_available_named_time_zone_identifier(*time_zone_parse.name);

        // b. If identifierRecord is EMPTY, throw a RangeError exception.
        if (!identifier_record.has_value())
            return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidTimeZoneName, *time_zone_parse.name);

        // c. Set timeZone to identifierRecord.[[Identifier]].
        time_zone = identifier_record->identifier;
    }
    // 7. Else,
    else {
        // a. Set timeZone to FormatOffsetTimeZoneIdentifier(timeZoneParse.[[OffsetMinutes]]).
        time_zone = format_offset_time_zone_identifier(*time_zone_parse.offset_minutes);
    }

    // 8. If calendar is undefined, set calendar to "iso8601".
    if (calendar_value.is_undefined())
        calendar_value = PrimitiveString::create(vm, "iso8601"_string);

    // 9. If calendar is not a String, throw a TypeError exception.
    if (!calendar_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, calendar_value);

    // 10. Set calendar to ? CanonicalizeCalendar(calendar).
    auto calendar = TRY(canonicalize_calendar(vm, calendar_value.as_string().utf8_string_view()));

    // 11. Return ? CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar, NewTarget).
    return TRY(create_temporal_zoned_date_time(vm, epoch_nanoseconds, move(time_zone), move(calendar), new_target));
}

// 6.2.2 Temporal.ZonedDateTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.from
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimeConstructor::from)
{
    // 1. Return ? ToTemporalZonedDateTime(item, options).
    return TRY(to_temporal_zoned_date_time(vm, vm.argument(0), vm.argument(1)));
}

// 6.2.3 Temporal.ZonedDateTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.compare
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimeConstructor::compare)
{
    // 1. Set one to ? ToTemporalZonedDateTime(one).
    auto one = TRY(to_temporal_zoned_date_time(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalZonedDateTime(two).
    auto two = TRY(to_temporal_zoned_date_time(vm, vm.argument(1)));

    // 3. Return 𝔽(CompareEpochNanoseconds(one.[[EpochNanoseconds]], two.[[EpochNanoseconds]])).
    return compare_epoch_nanoseconds(one->epoch_nanoseconds()->big_integer(), two->epoch_nanoseconds()->big_integer());
}

}
