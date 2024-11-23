/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainTimeConstructor.h>
#include <math.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainTime);

// 4 Temporal.PlainTime Objects, https://tc39.es/proposal-temporal/#sec-temporal-plaintime-objects
PlainTime::PlainTime(Time const& time, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_time(time)
{
}

// FIXME: We should add a generic floor() method to our BigInt classes. But for now, since we know we are only dividing
//        by powers of 10, we can implement a very situationally specific method to compute the floor of a division.
static TimeDuration big_floor(TimeDuration const& numerator, Crypto::UnsignedBigInteger const& denominator)
{
    auto result = numerator.divided_by(denominator);

    if (result.remainder.is_zero())
        return result.quotient;
    if (!result.quotient.is_negative() && result.remainder.is_positive())
        return result.quotient;

    return result.quotient.minus(TimeDuration { 1 });
}

// 4.5.2 CreateTimeRecord ( hour, minute, second, millisecond, microsecond, nanosecond [ , deltaDays ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtimerecord
Time create_time_record(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, double delta_days)
{
    // 1. If deltaDays is not present, set deltaDays to 0.
    // 2. Assert: IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond).
    VERIFY(is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond));

    // 3. Return Time Record { [[Days]]: deltaDays, [[Hour]]: hour, [[Minute]]: minute, [[Second]]: second, [[Millisecond]]: millisecond, [[Microsecond]]: microsecond, [[Nanosecond]]: nanosecond  }.
    return {
        .days = delta_days,
        .hour = static_cast<u8>(hour),
        .minute = static_cast<u8>(minute),
        .second = static_cast<u8>(second),
        .millisecond = static_cast<u16>(millisecond),
        .microsecond = static_cast<u16>(microsecond),
        .nanosecond = static_cast<u16>(nanosecond),
    };
}

// 4.5.3 MidnightTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-midnighttimerecord
Time midnight_time_record()
{
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 0, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    return { .days = 0, .hour = 0, .minute = 0, .second = 0, .millisecond = 0, .microsecond = 0, .nanosecond = 0 };
}

// 4.5.4 NoonTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-noontimerecord
Time noon_time_record()
{
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 12, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    return { .days = 0, .hour = 12, .minute = 0, .second = 0, .millisecond = 0, .microsecond = 0, .nanosecond = 0 };
}

// 4.5.5 DifferenceTime ( time1, time2 ), https://tc39.es/proposal-temporal/#sec-temporal-differencetime
TimeDuration difference_time(Time const& time1, Time const& time2)
{
    // 1. Let hours be time2.[[Hour]] - time1.[[Hour]].
    auto hours = static_cast<double>(time2.hour) - static_cast<double>(time1.hour);

    // 2. Let minutes be time2.[[Minute]] - time1.[[Minute]].
    auto minutes = static_cast<double>(time2.minute) - static_cast<double>(time1.minute);

    // 3. Let seconds be time2.[[Second]] - time1.[[Second]].
    auto seconds = static_cast<double>(time2.second) - static_cast<double>(time1.second);

    // 4. Let milliseconds be time2.[[Millisecond]] - time1.[[Millisecond]].
    auto milliseconds = static_cast<double>(time2.millisecond) - static_cast<double>(time1.millisecond);

    // 5. Let microseconds be time2.[[Microsecond]] - time1.[[Microsecond]].
    auto microseconds = static_cast<double>(time2.microsecond) - static_cast<double>(time1.microsecond);

    // 6. Let nanoseconds be time2.[[Nanosecond]] - time1.[[Nanosecond]].
    auto nanoseconds = static_cast<double>(time2.nanosecond) - static_cast<double>(time1.nanosecond);

    // 7. Let timeDuration be TimeDurationFromComponents(hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto time_duration = time_duration_from_components(hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 8. Assert: abs(timeDuration) < nsPerDay.
    VERIFY(time_duration.unsigned_value() < NANOSECONDS_PER_DAY);

    // 9. Return timeDuration.
    return time_duration;
}

// 4.5.6 ToTemporalTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltime
ThrowCompletionOr<GC::Ref<PlainTime>> to_temporal_time(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    Time time;

    // 2. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalTime]] internal slot, then
        if (is<PlainTime>(object)) {
            auto const& plain_time = static_cast<PlainTime const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalTime(item.[[Time]]).
            return MUST(create_temporal_time(vm, plain_time.time()));
        }

        // FIXME: b. If item has an [[InitializedTemporalDateTime]] internal slot, then
        // FIXME:     i. Let resolvedOptions be ? GetOptionsObject(options).
        // FIXME:     ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
        // FIXME:     iii. Return ! CreateTemporalTime(item.[[ISODateTime]].[[Time]]).

        // FIXME: c. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        // FIXME:     i. Let isoDateTime be GetISODateTimeFor(item.[[TimeZone]], item.[[EpochNanoseconds]]).
        // FIXME:     ii. Let resolvedOptions be ? GetOptionsObject(options).
        // FIXME:     iii. Perform ? GetTemporalOverflowOption(resolvedOptions).
        // FIXME:     iv. Return ! CreateTemporalTime(isoDateTime.[[Time]]).

        // d. Let result be ? ToTemporalTimeRecord(item).
        auto result = TRY(to_temporal_time_record(vm, object));

        // e. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // f. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // g. Set result to ? RegulateTime(result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], overflow).
        time = TRY(regulate_time(vm, *result.hour, *result.minute, *result.second, *result.millisecond, *result.microsecond, *result.nanosecond, overflow));
    }
    // 3. Else,
    else {
        // a. If item is not a String, throw a TypeError exception.
        if (!item.is_string())
            return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidPlainTime);

        // b. Let parseResult be ? ParseISODateTime(item, « TemporalTimeString »).
        auto parse_result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalTimeString } }));

        // c. Assert: parseResult.[[Time]] is not START-OF-DAY.
        VERIFY(!parse_result.time.has<ParsedISODateTime::StartOfDay>());

        // d. Set result to parseResult.[[Time]].
        time = parse_result.time.get<Time>();

        // e. NOTE: A successful parse using TemporalTimeString guarantees absence of ambiguity with respect to any
        //    ISO 8601 date-only, year-month, or month-day representation.

        // f. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // g. Perform ? GetTemporalOverflowOption(resolvedOptions).
        TRY(get_temporal_overflow_option(vm, resolved_options));
    }

    // 4. Return ! CreateTemporalTime(result).
    return MUST(create_temporal_time(vm, time));
}

// 4.5.8 RegulateTime ( hour, minute, second, millisecond, microsecond, nanosecond, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulatetime
ThrowCompletionOr<Time> regulate_time(VM& vm, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, Overflow overflow)
{
    switch (overflow) {
    // 1. If overflow is CONSTRAIN, then
    case Overflow::Constrain:
        // a. Set hour to the result of clamping hour between 0 and 23.
        hour = clamp(hour, 0, 23);

        // b. Set minute to the result of clamping minute between 0 and 59.
        minute = clamp(minute, 0, 59);

        // c. Set second to the result of clamping second between 0 and 59.
        second = clamp(second, 0, 59);

        // d. Set millisecond to the result of clamping millisecond between 0 and 999.
        millisecond = clamp(millisecond, 0, 999);

        // e. Set microsecond to the result of clamping microsecond between 0 and 999.
        microsecond = clamp(microsecond, 0, 999);

        // f. Set nanosecond to the result of clamping nanosecond between 0 and 999.
        nanosecond = clamp(nanosecond, 0, 999);

        break;

    // 2. Else,
    case Overflow::Reject:
        // a. Assert: overflow is REJECT.
        // b. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
        if (!is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainTime);

        break;
    }

    // 3. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond,nanosecond).
    return create_time_record(hour, minute, second, millisecond, microsecond, nanosecond);
}

// 4.5.9 IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidtime
bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond)
{
    // 1. If hour < 0 or hour > 23, then
    if (hour < 0 || hour > 23) {
        // a. Return false.
        return false;
    }

    // 2. If minute < 0 or minute > 59, then
    if (minute < 0 || minute > 59) {
        // a. Return false.
        return false;
    }

    // 3. If second < 0 or second > 59, then
    if (second < 0 || second > 59) {
        // a. Return false.
        return false;
    }

    // 4. If millisecond < 0 or millisecond > 999, then
    if (millisecond < 0 || millisecond > 999) {
        // a. Return false.
        return false;
    }

    // 5. If microsecond < 0 or microsecond > 999, then
    if (microsecond < 0 || microsecond > 999) {
        // a. Return false.
        return false;
    }

    // 6. If nanosecond < 0 or nanosecond > 999, then
    if (nanosecond < 0 || nanosecond > 999) {
        // a. Return false.
        return false;
    }

    // 7. Return true.
    return true;
}

// 4.5.10 BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balancetime
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond)
{
    // 1. Set microsecond to microsecond + floor(nanosecond / 1000).
    microsecond += floor(nanosecond / 1000.0);

    // 2. Set nanosecond to nanosecond modulo 1000.
    nanosecond = modulo(nanosecond, 1000.0);

    // 3. Set millisecond to millisecond + floor(microsecond / 1000).
    millisecond += floor(microsecond / 1000.0);

    // 4. Set microsecond to microsecond modulo 1000.
    microsecond = modulo(microsecond, 1000.0);

    // 5. Set second to second + floor(millisecond / 1000).
    second += floor(millisecond / 1000.0);

    // 6. Set millisecond to millisecond modulo 1000.
    millisecond = modulo(millisecond, 1000.0);

    // 7. Set minute to minute + floor(second / 60).
    minute += floor(second / 60.0);

    // 8. Set second to second modulo 60.
    second = modulo(second, 60.0);

    // 9. Set hour to hour + floor(minute / 60).
    hour += floor(minute / 60.0);

    // 10. Set minute to minute modulo 60.
    minute = modulo(minute, 60.0);

    // 11. Let deltaDays be floor(hour / 24).
    auto delta_days = floor(hour / 24.0);

    // 12. Set hour to hour modulo 24.
    hour = modulo(hour, 24.0);

    // 13. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond, deltaDays).
    return create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, delta_days);
}

// 4.5.10 BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balancetime
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, TimeDuration const& nanosecond_value)
{
    // 1. Set microsecond to microsecond + floor(nanosecond / 1000).
    auto microsecond_value = TimeDuration { microsecond }.plus(big_floor(nanosecond_value, NANOSECONDS_PER_MICROSECOND));

    // 2. Set nanosecond to nanosecond modulo 1000.
    auto nanosecond = modulo(nanosecond_value, NANOSECONDS_PER_MICROSECOND).to_double();

    // 3. Set millisecond to millisecond + floor(microsecond / 1000).
    auto millisecond_value = TimeDuration { millisecond }.plus(big_floor(microsecond_value, MICROSECONDS_PER_MILLISECOND));

    // 4. Set microsecond to microsecond modulo 1000.
    microsecond = modulo(microsecond_value, MICROSECONDS_PER_MILLISECOND).to_double();

    // 5. Set second to second + floor(millisecond / 1000).
    auto second_value = TimeDuration { second }.plus(big_floor(millisecond_value, MILLISECONDS_PER_SECOND));

    // 6. Set millisecond to millisecond modulo 1000.
    millisecond = modulo(millisecond_value, MILLISECONDS_PER_SECOND).to_double();

    // 7. Set minute to minute + floor(second / 60).
    auto minute_value = TimeDuration { minute }.plus(big_floor(second_value, SECONDS_PER_MINUTE));

    // 8. Set second to second modulo 60.
    second = modulo(second_value, SECONDS_PER_MINUTE).to_double();

    // 9. Set hour to hour + floor(minute / 60).
    auto hour_value = TimeDuration { hour }.plus(big_floor(minute_value, MINUTES_PER_HOUR));

    // 10. Set minute to minute modulo 60.
    minute = modulo(minute_value, MINUTES_PER_HOUR).to_double();

    // 11. Let deltaDays be floor(hour / 24).
    auto delta_days = big_floor(hour_value, HOURS_PER_DAY).to_double();

    // 12. Set hour to hour modulo 24.
    hour = modulo(hour_value, HOURS_PER_DAY).to_double();

    // 13. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond, deltaDays).
    return create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, delta_days);
}

// 4.5.11 CreateTemporalTime ( time [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaltime
ThrowCompletionOr<GC::Ref<PlainTime>> create_temporal_time(VM& vm, Time const& time, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If newTarget is not present, set newTarget to %Temporal.PlainTime%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_time_constructor();

    // 2. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainTime.prototype%", « [[InitializedTemporalTime]], [[Time]] »).
    // 3. Set object.[[Time]] to time.
    auto object = TRY(ordinary_create_from_constructor<PlainTime>(vm, *new_target, &Intrinsics::temporal_plain_time_prototype, time));

    // 4. Return object.
    return object;
}

// 4.5.12 ToTemporalTimeRecord ( temporalTimeLike [ , completeness ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimerecord
ThrowCompletionOr<TemporalTimeLike> to_temporal_time_record(VM& vm, Object const& temporal_time_like, Completeness completeness)
{
    // 1. If completeness is not present, set completeness to COMPLETE.

    TemporalTimeLike result;

    // 2. If completeness is COMPLETE, then
    if (completeness == Completeness::Complete) {
        // a. Let result be a new TemporalTimeLike Record with each field set to 0.
        result = TemporalTimeLike::zero();
    }
    // 3. Else,
    else {
        // a. Let result be a new TemporalTimeLike Record with each field set to UNSET.
    }

    // 4. Let any be false.
    auto any = false;

    auto apply_field = [&](auto const& key, auto& result_field) -> ThrowCompletionOr<void> {
        auto field = TRY(temporal_time_like.get(key));
        if (field.is_undefined())
            return {};

        result_field = TRY(to_integer_with_truncation(vm, field, ErrorType::TemporalInvalidTimeLikeField, field, key));
        any = true;

        return {};
    };

    // 5. Let hour be ? Get(temporalTimeLike, "hour").
    // 6. If hour is not undefined, then
    //     a. Set result.[[Hour]] to ? ToIntegerWithTruncation(hour).
    //     b. Set any to true.
    TRY(apply_field(vm.names.hour, result.hour));

    // 7. Let microsecond be ? Get(temporalTimeLike, "microsecond").
    // 8. If microsecond is not undefined, then
    //     a. Set result.[[Microsecond]] to ? ToIntegerWithTruncation(microsecond).
    //     b. Set any to true.
    TRY(apply_field(vm.names.microsecond, result.microsecond));

    // 9. Let millisecond be ? Get(temporalTimeLike, "millisecond").
    // 10. If millisecond is not undefined, then
    //     a. Set result.[[Millisecond]] to ? ToIntegerWithTruncation(millisecond).
    //     b. Set any to true.
    TRY(apply_field(vm.names.millisecond, result.millisecond));

    // 11. Let minute be ? Get(temporalTimeLike, "minute").
    // 12. If minute is not undefined, then
    //     a. Set result.[[Minute]] to ? ToIntegerWithTruncation(minute).
    //     b. Set any to true.
    TRY(apply_field(vm.names.minute, result.minute));

    // 13. Let nanosecond be ? Get(temporalTimeLike, "nanosecond").
    // 14. If nanosecond is not undefined, then
    //     a. Set result.[[Nanosecond]] to ? ToIntegerWithTruncation(nanosecond).
    //     b. Set any to true.
    TRY(apply_field(vm.names.nanosecond, result.nanosecond));

    // 15. Let second be ? Get(temporalTimeLike, "second").
    // 16. If second is not undefined, then
    //     a. Set result.[[Second]] to ? ToIntegerWithTruncation(second).
    //     b. Set any to true.
    TRY(apply_field(vm.names.second, result.second));

    // 17. If any is false, throw a TypeError exception.
    if (!any)
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidTime);

    // 18. Return result.
    return result;
}

// 4.5.13 TimeRecordToString ( time, precision ), https://tc39.es/proposal-temporal/#sec-temporal-timerecordtostring
String time_record_to_string(Time const& time, SecondsStringPrecision::Precision precision)
{
    // 1. Let subSecondNanoseconds be time.[[Millisecond]] × 10**6 + time.[[Microsecond]] × 10**3 + time.[[Nanosecond]].
    auto sub_second_nanoseconds = (static_cast<u64>(time.millisecond) * 1'000'000) + (static_cast<u64>(time.microsecond) * 1000) + static_cast<u64>(time.nanosecond);

    // 2. Return FormatTimeString(time.[[Hour]], time.[[Minute]], time.[[Second]], subSecondNanoseconds, precision).
    return format_time_string(time.hour, time.minute, time.second, sub_second_nanoseconds, precision);
}

// 4.5.14 CompareTimeRecord ( time1, time2 ), https://tc39.es/proposal-temporal/#sec-temporal-comparetimerecord
i8 compare_time_record(Time const& time1, Time const& time2)
{
    // 1. If time1.[[Hour]] > time2.[[Hour]], return 1.
    if (time1.hour > time2.hour)
        return 1;
    // 2. If time1.[[Hour]] < time2.[[Hour]], return -1.
    if (time1.hour < time2.hour)
        return -1;

    // 3. If time1.[[Minute]] > time2.[[Minute]], return 1.
    if (time1.minute > time2.minute)
        return 1;
    // 4. If time1.[[Minute]] < time2.[[Minute]], return -1.
    if (time1.minute < time2.minute)
        return -1;

    // 5. If time1.[[Second]] > time2.[[Second]], return 1.
    if (time1.second > time2.second)
        return 1;
    // 6. If time1.[[Second]] < time2.[[Second]], return -1.
    if (time1.second < time2.second)
        return -1;

    // 7. If time1.[[Millisecond]] > time2.[[Millisecond]], return 1.
    if (time1.millisecond > time2.millisecond)
        return 1;
    // 8. If time1.[[Millisecond]] < time2.[[Millisecond]], return -1.
    if (time1.millisecond < time2.millisecond)
        return -1;

    // 9. If time1.[[Microsecond]] > time2.[[Microsecond]], return 1.
    if (time1.microsecond > time2.microsecond)
        return 1;
    // 10. If time1.[[Microsecond]] < time2.[[Microsecond]], return -1.
    if (time1.microsecond < time2.microsecond)
        return -1;

    // 11. If time1.[[Nanosecond]] > time2.[[Nanosecond]], return 1.
    if (time1.nanosecond > time2.nanosecond)
        return 1;
    // 12. If time1.[[Nanosecond]] < time2.[[Nanosecond]], return -1.
    if (time1.nanosecond < time2.nanosecond)
        return -1;

    // 13. Return 0.
    return 0;
}

// 4.5.15 AddTime ( time, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-addtime
Time add_time(Time const& time, TimeDuration const& time_duration)
{
    auto nanoseconds = time_duration.plus(TimeDuration { static_cast<i64>(time.nanosecond) });

    // 1. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], time.[[Nanosecond]] + timeDuration).
    return balance_time(time.hour, time.minute, time.second, time.millisecond, time.microsecond, nanoseconds);
}

// 4.5.16 RoundTime ( time, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtime
Time round_time(Time const& time, u64 increment, Unit unit, RoundingMode rounding_mode)
{
    double quantity = 0;

    switch (unit) {
    // 1. If unit is DAY or HOUR, then
    case Unit::Day:
    case Unit::Hour:
        // a. Let quantity be ((((time.[[Hour]] × 60 + time.[[Minute]]) × 60 + time.[[Second]]) × 1000 + time.[[Millisecond]]) × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
        quantity = ((((time.hour * 60.0 + time.minute) * 60.0 + time.second) * 1000.0 + time.millisecond) * 1000.0 + time.microsecond) * 1000.0 + time.nanosecond;
        break;

    // 2. Else if unit is MINUTE, then
    case Unit::Minute:
        // a. Let quantity be (((time.[[Minute]] × 60 + time.[[Second]]) × 1000 + time.[[Millisecond]]) × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
        quantity = (((time.minute * 60.0 + time.second) * 1000.0 + time.millisecond) * 1000.0 + time.microsecond) * 1000.0 + time.nanosecond;
        break;

    // 3. Else if unit is SECOND, then
    case Unit::Second:
        // a. Let quantity be ((time.[[Second]] × 1000 + time.[[Millisecond]]) × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
        quantity = ((time.second * 1000.0 + time.millisecond) * 1000.0 + time.microsecond) * 1000.0 + time.nanosecond;
        break;

    // 4. Else if unit is MILLISECOND, then
    case Unit::Millisecond:
        // a. Let quantity be (time.[[Millisecond]] × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
        quantity = (time.millisecond * 1000.0 + time.microsecond) * 1000.0 + time.nanosecond;
        break;

    // 5. Else if unit is MICROSECOND, then
    case Unit::Microsecond:
        // a. Let quantity be time.[[Microsecond]] × 1000 + time.[[Nanosecond]].
        quantity = time.microsecond * 1000.0 + time.nanosecond;
        break;

    // 6. Else,
    case Unit::Nanosecond:
        // a. Assert: unit is NANOSECOND.
        // b. Let quantity be time.[[Nanosecond]].
        quantity = time.nanosecond;
        break;

    default:
        VERIFY_NOT_REACHED();
    }

    // 7. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    auto unit_length = temporal_unit_length_in_nanoseconds(unit).to_u64();

    // 8. Let result be RoundNumberToIncrement(quantity, increment × unitLength, roundingMode) / unitLength.
    auto result = round_number_to_increment(quantity, increment * unit_length, rounding_mode) / static_cast<double>(unit_length);

    switch (unit) {
    // 9. If unit is DAY, then
    case Unit::Day:
        // a. Return CreateTimeRecord(0, 0, 0, 0, 0, 0, result).
        return create_time_record(0, 0, 0, 0, 0, 0, result);

    // 10. If unit is HOUR, then
    case Unit::Hour:
        // a. Return BalanceTime(result, 0, 0, 0, 0, 0).
        return balance_time(result, 0, 0, 0, 0, 0);

    // 11. If unit is MINUTE, then
    case Unit::Minute:
        // a. Return BalanceTime(time.[[Hour]], result, 0, 0, 0, 0).
        return balance_time(time.hour, result, 0, 0, 0, 0);

    // 12. If unit is SECOND, then
    case Unit::Second:
        // a. Return BalanceTime(time.[[Hour]], time.[[Minute]], result, 0, 0, 0).
        return balance_time(time.hour, time.minute, result, 0, 0, 0);

    // 13. If unit is MILLISECOND, then
    case Unit::Millisecond:
        // a. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], result, 0, 0).
        return balance_time(time.hour, time.minute, time.second, result, 0, 0);

    // 14. If unit is MICROSECOND, then
    case Unit::Microsecond:
        // a. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], result, 0).
        return balance_time(time.hour, time.minute, time.second, time.millisecond, result, 0);

    // 15. Assert: unit is NANOSECOND.
    case Unit::Nanosecond:
        // 16. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], result).
        return balance_time(time.hour, time.minute, time.second, time.millisecond, time.microsecond, result);

    default:
        break;
    }

    VERIFY_NOT_REACHED();
}

// 4.5.18 AddDurationToTime ( operation, temporalTime, temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtotime
ThrowCompletionOr<GC::Ref<PlainTime>> add_duration_to_time(VM& vm, ArithmeticOperation operation, PlainTime const& temporal_time, Value temporal_duration_like)
{
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, temporal_duration_like));

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if (operation == ArithmeticOperation::Subtract)
        duration = create_negated_temporal_duration(vm, duration);

    // 3. Let internalDuration be ToInternalDurationRecord(duration).
    auto internal_duration = to_internal_duration_record(vm, duration);

    // 4. Let result be AddTime(temporalTime.[[Time]], internalDuration.[[Time]]).
    auto result = add_time(temporal_time.time(), internal_duration.time);

    // 5. Return ! CreateTemporalTime(result).
    return MUST(create_temporal_time(vm, result));
}

}
