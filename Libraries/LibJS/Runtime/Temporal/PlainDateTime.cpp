/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainDateTimeConstructor.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainDateTime);

// 5 Temporal.PlainDateTime Objects, https://tc39.es/proposal-temporal/#sec-temporal-plaindatetime-objects
PlainDateTime::PlainDateTime(i32 iso_year, u8 iso_month, u8 iso_day, u8 iso_hour, u8 iso_minute, u8 iso_second, u16 iso_millisecond, u16 iso_microsecond, u16 iso_nanosecond, Object& calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_year(iso_year)
    , m_iso_month(iso_month)
    , m_iso_day(iso_day)
    , m_iso_hour(iso_hour)
    , m_iso_minute(iso_minute)
    , m_iso_second(iso_second)
    , m_iso_millisecond(iso_millisecond)
    , m_iso_microsecond(iso_microsecond)
    , m_iso_nanosecond(iso_nanosecond)
    , m_calendar(calendar)
{
}

void PlainDateTime::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_calendar);
}

// nsMinInstant - nsPerDay
auto const DATETIME_NANOSECONDS_MIN = "-8640000086400000000000"_sbigint;
// nsMaxInstant + nsPerDay
auto const DATETIME_NANOSECONDS_MAX = "8640000086400000000000"_sbigint;

// 5.5.1 ISODateTimeWithinLimits ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetimewithinlimits
bool iso_date_time_within_limits(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Let ns be ℝ(GetUTCEpochNanoseconds(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond)).
    auto ns = get_utc_epoch_nanoseconds(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond);

    // 3. If ns ≤ nsMinInstant - nsPerDay, then
    if (ns <= DATETIME_NANOSECONDS_MIN) {
        // a. Return false.
        return false;
    }

    // 4. If ns ≥ nsMaxInstant + nsPerDay, then
    if (ns >= DATETIME_NANOSECONDS_MAX) {
        // a. Return false.
        return false;
    }
    // 5. Return true.
    return true;
}

// 5.5.2 InterpretTemporalDateTimeFields ( calendar, fields, options ), https://tc39.es/proposal-temporal/#sec-temporal-interprettemporaldatetimefields
ThrowCompletionOr<ISODateTime> interpret_temporal_date_time_fields(VM& vm, Object& calendar, Object& fields, Object const* options)
{
    // 1. Let timeResult be ? ToTemporalTimeRecord(fields).
    auto unregulated_time_result = TRY(to_temporal_time_record(vm, fields));

    // 2. Let overflow be ? ToTemporalOverflow(options).
    auto overflow = TRY(to_temporal_overflow(vm, options));

    // 3. Let temporalDate be ? CalendarDateFromFields(calendar, fields, options).
    auto* temporal_date = TRY(calendar_date_from_fields(vm, calendar, fields, options));

    // 4. Let timeResult be ? RegulateTime(timeResult.[[Hour]], timeResult.[[Minute]], timeResult.[[Second]], timeResult.[[Millisecond]], timeResult.[[Microsecond]], timeResult.[[Nanosecond]], overflow).
    auto time_result = TRY(regulate_time(vm, *unregulated_time_result.hour, *unregulated_time_result.minute, *unregulated_time_result.second, *unregulated_time_result.millisecond, *unregulated_time_result.microsecond, *unregulated_time_result.nanosecond, overflow));

    // 5. Return the Record { [[Year]]: temporalDate.[[ISOYear]], [[Month]]: temporalDate.[[ISOMonth]], [[Day]]: temporalDate.[[ISODay]], [[Hour]]: timeResult.[[Hour]], [[Minute]]: timeResult.[[Minute]], [[Second]]: timeResult.[[Second]], [[Millisecond]]: timeResult.[[Millisecond]], [[Microsecond]]: timeResult.[[Microsecond]], [[Nanosecond]]: timeResult.[[Nanosecond]] }.
    return ISODateTime {
        .year = temporal_date->iso_year(),
        .month = temporal_date->iso_month(),
        .day = temporal_date->iso_day(),
        .hour = time_result.hour,
        .minute = time_result.minute,
        .second = time_result.second,
        .millisecond = time_result.millisecond,
        .microsecond = time_result.microsecond,
        .nanosecond = time_result.nanosecond,
    };
}

// 5.5.3 ToTemporalDateTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldatetime
ThrowCompletionOr<PlainDateTime*> to_temporal_date_time(VM& vm, Value item, Object const* options)
{
    // 1. If options is not present, set options to undefined.
    // 2. Assert: Type(options) is Object or Undefined.

    Object* calendar;
    ISODateTime result;

    // 3. If Type(item) is Object, then
    if (item.is_object()) {
        auto& item_object = item.as_object();

        // a. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if (is<PlainDateTime>(item_object)) {
            // i. Return item.
            return &static_cast<PlainDateTime&>(item_object);
        }

        // b. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(item_object)) {
            auto& zoned_date_time = static_cast<ZonedDateTime&>(item_object);

            // i. Perform ? ToTemporalOverflow(options).
            (void)TRY(to_temporal_overflow(vm, options));

            // ii. Let instant be ! CreateTemporalInstant(item.[[Nanoseconds]]).
            auto* instant = create_temporal_instant(vm, zoned_date_time.nanoseconds()).release_value();

            // iii. Return ? BuiltinTimeZoneGetPlainDateTimeFor(item.[[TimeZone]], instant, item.[[Calendar]]).
            return builtin_time_zone_get_plain_date_time_for(vm, &zoned_date_time.time_zone(), *instant, zoned_date_time.calendar());
        }

        // c. If item has an [[InitializedTemporalDate]] internal slot, then
        if (is<PlainDate>(item_object)) {
            auto& plain_date = static_cast<PlainDate&>(item_object);

            // i. Perform ? ToTemporalOverflow(options).
            (void)TRY(to_temporal_overflow(vm, options));

            // ii. Return ? CreateTemporalDateTime(item.[[ISOYear]], item.[[ISOMonth]], item.[[ISODay]], 0, 0, 0, 0, 0, 0, item.[[Calendar]]).
            return create_temporal_date_time(vm, plain_date.iso_year(), plain_date.iso_month(), plain_date.iso_day(), 0, 0, 0, 0, 0, 0, plain_date.calendar());
        }

        // d. Let calendar be ? GetTemporalCalendarWithISODefault(item).
        calendar = TRY(get_temporal_calendar_with_iso_default(vm, item_object));

        // e. Let fieldNames be ? CalendarFields(calendar, « "day", "hour", "microsecond", "millisecond", "minute", "month", "monthCode", "nanosecond", "second", "year" »).
        auto field_names = TRY(calendar_fields(vm, *calendar, { "day"sv, "hour"sv, "microsecond"sv, "millisecond"sv, "minute"sv, "month"sv, "monthCode"sv, "nanosecond"sv, "second"sv, "year"sv }));

        // f. Let fields be ? PrepareTemporalFields(item, fieldNames, «»).
        auto* fields = TRY(prepare_temporal_fields(vm, item_object, field_names, Vector<StringView> {}));

        // g. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, options).
        result = TRY(interpret_temporal_date_time_fields(vm, *calendar, *fields, options));
    }
    // 4. Else,
    else {
        // a. Perform ? ToTemporalOverflow(options).
        (void)TRY(to_temporal_overflow(vm, options));

        // b. Let string be ? ToString(item).
        auto string = TRY(item.to_string(vm));

        // c. Let result be ? ParseTemporalDateTimeString(string).
        result = TRY(parse_temporal_date_time_string(vm, string));

        // d. Assert: IsValidISODate(result.[[Year]], result.[[Month]], result.[[Day]]) is true.
        VERIFY(is_valid_iso_date(result.year, result.month, result.day));

        // e. Assert: IsValidTime(result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]]) is true.
        VERIFY(is_valid_time(result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond));

        // f. Let calendar be ? ToTemporalCalendarWithISODefault(result.[[Calendar]]).
        calendar = TRY(to_temporal_calendar_with_iso_default(vm, result.calendar.has_value() ? PrimitiveString::create(vm, *result.calendar) : js_undefined()));
    }

    // 5. Return ? CreateTemporalDateTime(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], calendar).
    return create_temporal_date_time(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, *calendar);
}

// 5.5.4 BalanceISODateTime ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisodatetime
ISODateTime balance_iso_date_time(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, i64 nanosecond)
{
    // NOTE: The only use of this AO is in BuiltinTimeZoneGetPlainDateTimeFor, where we know that all values
    // but `nanosecond` are in their usual range, hence why that's the only outlier here. The range for that
    // is -86400000000000 to 86400000000999, so an i32 is not enough.

    // 1. Assert: year, month, day, hour, minute, second, millisecond, microsecond, and nanosecond are integers.

    // 2. Let balancedTime be ! BalanceTime(hour, minute, second, millisecond, microsecond, nanosecond).
    auto balanced_time = balance_time(hour, minute, second, millisecond, microsecond, nanosecond);

    // 3. Let balancedDate be BalanceISODate(year, month, day + balancedTime.[[Days]]).
    auto balanced_date = balance_iso_date(year, month, day + balanced_time.days);

    // 4. Return the Record { [[Year]]: balancedDate.[[Year]], [[Month]]: balancedDate.[[Month]], [[Day]]: balancedDate.[[Day]], [[Hour]]: balancedTime.[[Hour]], [[Minute]]: balancedTime.[[Minute]], [[Second]]: balancedTime.[[Second]], [[Millisecond]]: balancedTime.[[Millisecond]], [[Microsecond]]: balancedTime.[[Microsecond]], [[Nanosecond]]: balancedTime.[[Nanosecond]] }.
    return ISODateTime { .year = balanced_date.year, .month = balanced_date.month, .day = balanced_date.day, .hour = balanced_time.hour, .minute = balanced_time.minute, .second = balanced_time.second, .millisecond = balanced_time.millisecond, .microsecond = balanced_time.microsecond, .nanosecond = balanced_time.nanosecond };
}

// 5.5.5 CreateTemporalDateTime ( isoYear, isoMonth, isoDay, hour, minute, second, millisecond, microsecond, nanosecond, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaldatetime
ThrowCompletionOr<PlainDateTime*> create_temporal_date_time(VM& vm, i32 iso_year, u8 iso_month, u8 iso_day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Object& calendar, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: isoYear, isoMonth, isoDay, hour, minute, second, millisecond, microsecond, and nanosecond are integers.
    // 2. Assert: Type(calendar) is Object.

    // 3. If IsValidISODate(isoYear, isoMonth, isoDay) is false, throw a RangeError exception.
    if (!is_valid_iso_date(iso_year, iso_month, iso_day))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 4. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
    if (!is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 5. If ISODateTimeWithinLimits(isoYear, isoMonth, isoDay, hour, minute, second, millisecond, microsecond, nanosecond) is false, then
    if (!iso_date_time_within_limits(iso_year, iso_month, iso_day, hour, minute, second, millisecond, microsecond, nanosecond)) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);
    }

    // 6. If newTarget is not present, set newTarget to %Temporal.PlainDateTime%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_date_time_constructor();

    // 7. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainDateTime.prototype%", « [[InitializedTemporalDateTime]], [[ISOYear]], [[ISOMonth]], [[ISODay]], [[ISOHour]], [[ISOMinute]], [[ISOSecond]], [[ISOMillisecond]], [[ISOMicrosecond]], [[ISONanosecond]], [[Calendar]] »).
    // 8. Set object.[[ISOYear]] to isoYear.
    // 9. Set object.[[ISOMonth]] to isoMonth.
    // 10. Set object.[[ISODay]] to isoDay.
    // 11. Set object.[[ISOHour]] to hour.
    // 12. Set object.[[ISOMinute]] to minute.
    // 13. Set object.[[ISOSecond]] to second.
    // 14. Set object.[[ISOMillisecond]] to millisecond.
    // 15. Set object.[[ISOMicrosecond]] to microsecond.
    // 16. Set object.[[ISONanosecond]] to nanosecond.
    // 17. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainDateTime>(vm, *new_target, &Intrinsics::temporal_plain_date_prototype, iso_year, iso_month, iso_day, hour, minute, second, millisecond, microsecond, nanosecond, calendar));

    // 18. Return object.
    return object.ptr();
}

// 5.5.6 TemporalDateTimeToString ( isoYear, isoMonth, isoDay, hour, minute, second, millisecond, microsecond, nanosecond, calendar, precision, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldatetimetostring
ThrowCompletionOr<String> temporal_date_time_to_string(VM& vm, i32 iso_year, u8 iso_month, u8 iso_day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Object const* calendar, Variant<StringView, u8> const& precision, StringView show_calendar)
{
    // 1. Assert: isoYear, isoMonth, isoDay, hour, minute, second, millisecond, microsecond, and nanosecond are integers.

    // 2. Let year be ! PadISOYear(isoYear).
    // 3. Let month be ToZeroPaddedDecimalString(isoMonth, 2).
    // 4. Let day be ToZeroPaddedDecimalString(isoDay, 2).
    // 5. Let hour be ToZeroPaddedDecimalString(hour, 2).
    // 6. Let minute be ToZeroPaddedDecimalString(minute, 2).

    // 7. Let seconds be ! FormatSecondsStringPart(second, millisecond, microsecond, nanosecond, precision).
    auto seconds = MUST_OR_THROW_OOM(format_seconds_string_part(vm, second, millisecond, microsecond, nanosecond, precision));

    // 8. Let calendarString be ? MaybeFormatCalendarAnnotation(calendar, showCalendar).
    auto calendar_string = TRY(maybe_format_calendar_annotation(vm, calendar, show_calendar));

    // 9. Return the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), month, the code unit 0x002D (HYPHEN-MINUS), day, 0x0054 (LATIN CAPITAL LETTER T), hour, the code unit 0x003A (COLON), minute, seconds, and calendarString.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}-{:02}-{:02}T{:02}:{:02}{}{}", MUST_OR_THROW_OOM(pad_iso_year(vm, iso_year)), iso_month, iso_day, hour, minute, seconds, calendar_string));
}

// 5.5.7 CompareISODateTime ( y1, mon1, d1, h1, min1, s1, ms1, mus1, ns1, y2, mon2, d2, h2, min2, s2, ms2, mus2, ns2 ), https://tc39.es/proposal-temporal/#sec-temporal-compareisodatetime
i8 compare_iso_date_time(i32 year1, u8 month1, u8 day1, u8 hour1, u8 minute1, u8 second1, u16 millisecond1, u16 microsecond1, u16 nanosecond1, i32 year2, u8 month2, u8 day2, u8 hour2, u8 minute2, u8 second2, u16 millisecond2, u16 microsecond2, u16 nanosecond2)
{
    // 1. Assert: y1, mon1, d1, h1, min1, s1, ms1, mus1, ns1, y2, mon2, d2, h2, min2, s2, ms2, mus2, and ns2 are integers.

    // 2. Let dateResult be ! CompareISODate(y1, mon1, d1, y2, mon2, d2).
    auto date_result = compare_iso_date(year1, month1, day1, year2, month2, day2);

    // 3. If dateResult is not 0, then
    if (date_result != 0) {
        // a. Return dateResult.
        return date_result;
    }

    // 4. Return ! CompareTemporalTime(h1, min1, s1, ms1, mus1, ns1, h2, min2, s2, ms2, mus2, ns2).
    return compare_temporal_time(hour1, minute1, second1, millisecond1, microsecond1, nanosecond1, hour2, minute2, second2, millisecond2, microsecond2, nanosecond2);
}

// 5.5.8 AddDateTime ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond, calendar, years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddatetime
ThrowCompletionOr<TemporalPlainDateTime> add_date_time(VM& vm, i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Object& calendar, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Object* options)
{
    // 1. Assert: ISODateTimeWithinLimits(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond) is true.
    VERIFY(iso_date_time_within_limits(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond));

    // 2. Let timeResult be ! AddTime(hour, minute, second, millisecond, microsecond, nanosecond, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto time_result = add_time(hour, minute, second, millisecond, microsecond, nanosecond, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 3. Let datePart be ! CreateTemporalDate(year, month, day, calendar).
    auto* date_part = MUST(create_temporal_date(vm, year, month, day, calendar));

    // 4. Let dateDuration be ? CreateTemporalDuration(years, months, weeks, days + timeResult.[[Days]], 0, 0, 0, 0, 0, 0).
    auto date_duration = TRY(create_temporal_duration(vm, years, months, weeks, days + time_result.days, 0, 0, 0, 0, 0, 0));

    // 5. Let addedDate be ? CalendarDateAdd(calendar, datePart, dateDuration, options).
    auto* added_date = TRY(calendar_date_add(vm, calendar, date_part, *date_duration, options));

    // 6. Return the Record { [[Year]]: addedDate.[[ISOYear]], [[Month]]: addedDate.[[ISOMonth]], [[Day]]: addedDate.[[ISODay]], [[Hour]]: timeResult.[[Hour]], [[Minute]]: timeResult.[[Minute]], [[Second]]: timeResult.[[Second]], [[Millisecond]]: timeResult.[[Millisecond]], [[Microsecond]]: timeResult.[[Microsecond]], [[Nanosecond]]: timeResult.[[Nanosecond]] }.
    return TemporalPlainDateTime { .year = added_date->iso_year(), .month = added_date->iso_month(), .day = added_date->iso_day(), .hour = time_result.hour, .minute = time_result.minute, .second = time_result.second, .millisecond = time_result.millisecond, .microsecond = time_result.microsecond, .nanosecond = time_result.nanosecond };
}

// 5.5.9 RoundISODateTime ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond, increment, unit, roundingMode [ , dayLength ] ), https://tc39.es/proposal-temporal/#sec-temporal-roundisodatetime
ISODateTime round_iso_date_time(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, u64 increment, StringView unit, StringView rounding_mode, Optional<double> day_length)
{
    // 1. Assert: year, month, day, hour, minute, second, millisecond, microsecond, and nanosecond are integers.

    // 2. Assert: ISODateTimeWithinLimits(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond) is true.
    VERIFY(iso_date_time_within_limits(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond));

    // 3. If dayLength is not present, set dayLength to nsPerDay.
    if (!day_length.has_value())
        day_length = ns_per_day;

    // 4. Let roundedTime be ! RoundTime(hour, minute, second, millisecond, microsecond, nanosecond, increment, unit, roundingMode, dayLength).
    auto rounded_time = round_time(hour, minute, second, millisecond, microsecond, nanosecond, increment, unit, rounding_mode, day_length);

    // 5. Let balanceResult be BalanceISODate(year, month, day + roundedTime.[[Days]]).
    auto balance_result = balance_iso_date(year, month, day + rounded_time.days);

    // 6. Return the Record { [[Year]]: balanceResult.[[Year]], [[Month]]: balanceResult.[[Month]], [[Day]]: balanceResult.[[Day]], [[Hour]]: roundedTime.[[Hour]], [[Minute]]: roundedTime.[[Minute]], [[Second]]: roundedTime.[[Second]], [[Millisecond]]: roundedTime.[[Millisecond]], [[Microsecond]]: roundedTime.[[Microsecond]], [[Nanosecond]]: roundedTime.[[Nanosecond]] }.
    return ISODateTime { .year = balance_result.year, .month = balance_result.month, .day = balance_result.day, .hour = rounded_time.hour, .minute = rounded_time.minute, .second = rounded_time.second, .millisecond = rounded_time.millisecond, .microsecond = rounded_time.microsecond, .nanosecond = rounded_time.nanosecond };
}

// 5.5.10 DifferenceISODateTime ( y1, mon1, d1, h1, min1, s1, ms1, mus1, ns1, y2, mon2, d2, h2, min2, s2, ms2, mus2, ns2, calendar, largestUnit, options ), https://tc39.es/proposal-temporal/#sec-temporal-differenceisodatetime
ThrowCompletionOr<DurationRecord> difference_iso_date_time(VM& vm, i32 year1, u8 month1, u8 day1, u8 hour1, u8 minute1, u8 second1, u16 millisecond1, u16 microsecond1, u16 nanosecond1, i32 year2, u8 month2, u8 day2, u8 hour2, u8 minute2, u8 second2, u16 millisecond2, u16 microsecond2, u16 nanosecond2, Object& calendar, StringView largest_unit, Object const& options)
{
    // 1. Assert: ISODateTimeWithinLimits(y1, mon1, d1, h1, min1, s1, ms1, mus1, ns1) is true.
    VERIFY(iso_date_time_within_limits(year1, month1, day1, hour1, minute1, second1, millisecond1, microsecond1, nanosecond1));

    // 2. Assert: ISODateTimeWithinLimits(y2, mon2, d2, h2, min2, s2, ms2, mus2, ns2) is true.
    VERIFY(iso_date_time_within_limits(year2, month2, day2, hour2, minute2, second2, millisecond2, microsecond2, nanosecond2));

    // 3. Let timeDifference be ! DifferenceTime(h1, min1, s1, ms1, mus1, ns1, h2, min2, s2, ms2, mus2, ns2).
    auto time_difference = difference_time(vm, hour1, minute1, second1, millisecond1, microsecond1, nanosecond1, hour2, minute2, second2, millisecond2, microsecond2, nanosecond2);

    // 4. Let timeSign be ! DurationSign(0, 0, 0, 0, timeDifference.[[Hours]], timeDifference.[[Minutes]], timeDifference.[[Seconds]], timeDifference.[[Milliseconds]], timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]]).
    auto time_sign = duration_sign(0, 0, 0, 0, time_difference.hours, time_difference.minutes, time_difference.seconds, time_difference.milliseconds, time_difference.microseconds, time_difference.nanoseconds);

    // 5. Let dateSign be ! CompareISODate(y2, mon2, d2, y1, mon1, d1).
    auto date_sign = compare_iso_date(year2, month2, day2, year1, month1, day1);

    // 6. Let adjustedDate be CreateISODateRecord(y1, mon1, d1).
    auto adjusted_date = create_iso_date_record(year1, month1, day1);

    // 7. If timeSign is -dateSign, then
    if (time_sign == -date_sign) {
        // a. Set adjustedDate to BalanceISODate(adjustedDate.[[Year]], adjustedDate.[[Month]], adjustedDate.[[Day]] - timeSign).
        adjusted_date = balance_iso_date(adjusted_date.year, adjusted_date.month, adjusted_date.day - time_sign);

        // b. Set timeDifference to ! BalanceDuration(-timeSign, timeDifference.[[Hours]], timeDifference.[[Minutes]], timeDifference.[[Seconds]], timeDifference.[[Milliseconds]], timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]], largestUnit).
        time_difference = MUST(balance_duration(vm, -time_sign, time_difference.hours, time_difference.minutes, time_difference.seconds, time_difference.milliseconds, time_difference.microseconds, Crypto::SignedBigInteger { time_difference.nanoseconds }, largest_unit));
    }

    // 8. Let date1 be ! CreateTemporalDate(adjustedDate.[[Year]], adjustedDate.[[Month]], adjustedDate.[[Day]], calendar).
    auto* date1 = MUST(create_temporal_date(vm, adjusted_date.year, adjusted_date.month, adjusted_date.day, calendar));

    // 9. Let date2 be ! CreateTemporalDate(y2, mon2, d2, calendar).
    auto* date2 = MUST(create_temporal_date(vm, year2, month2, day2, calendar));

    // 10. Let dateLargestUnit be ! LargerOfTwoTemporalUnits("day", largestUnit).
    auto date_largest_unit = larger_of_two_temporal_units("day"sv, largest_unit);

    // 11. Let untilOptions be ? MergeLargestUnitOption(options, dateLargestUnit).
    auto* until_options = TRY(merge_largest_unit_option(vm, options, TRY_OR_THROW_OOM(vm, String::from_utf8(date_largest_unit))));

    // 12. Let dateDifference be ? CalendarDateUntil(calendar, date1, date2, untilOptions).
    // FIXME: AD-HOC calendar records use as this AO is not up to date with latest spec
    auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { calendar }, { { CalendarMethod::DateAdd, CalendarMethod::DateUntil } }));
    auto date_difference = TRY(calendar_date_until(vm, calendar_record, date1, date2, *until_options));

    // 13. Let balanceResult be ? BalanceDuration(dateDifference.[[Days]], timeDifference.[[Hours]], timeDifference.[[Minutes]], timeDifference.[[Seconds]], timeDifference.[[Milliseconds]], timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]], largestUnit).
    auto balance_result = TRY(balance_duration(vm, date_difference->days(), time_difference.hours, time_difference.minutes, time_difference.seconds, time_difference.milliseconds, time_difference.microseconds, Crypto::SignedBigInteger { time_difference.nanoseconds }, largest_unit));

    // 14. Return ! CreateDurationRecord(dateDifference.[[Years]], dateDifference.[[Months]], dateDifference.[[Weeks]], balanceResult.[[Days]], balanceResult.[[Hours]], balanceResult.[[Minutes]], balanceResult.[[Seconds]], balanceResult.[[Milliseconds]], balanceResult.[[Microseconds]], balanceResult.[[Nanoseconds]]).
    return create_duration_record(date_difference->years(), date_difference->months(), date_difference->weeks(), balance_result.days, balance_result.hours, balance_result.minutes, balance_result.seconds, balance_result.milliseconds, balance_result.microseconds, balance_result.nanoseconds);
}

// 5.5.11 DifferenceTemporalPlainDateTime ( operation, dateTime, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaindatetime
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_plain_date_time(VM& vm, DifferenceOperation operation, PlainDateTime& date_time, Value other_value, Value options_value)
{
    // 1. If operation is since, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == DifferenceOperation::Since ? -1 : 1;

    // 2. Set other to ? ToTemporalDateTime(other).
    auto* other = TRY(to_temporal_date_time(vm, other_value));

    // 3. If ? CalendarEquals(dateTime.[[Calendar]], other.[[Calendar]]) is false, throw a RangeError exception.
    if (!TRY(calendar_equals(vm, date_time.calendar(), other->calendar())))
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);

    // 4. Let settings be ? GetDifferenceSettings(operation, options, datetime, « », "nanosecond", "day").
    auto settings = TRY(get_difference_settings(vm, operation, options_value, UnitGroup::DateTime, {}, { "nanosecond"sv }, "day"sv));

    // 5. Let diff be ? DifferenceISODateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], other.[[ISOYear]], other.[[ISOMonth]], other.[[ISODay]], other.[[ISOHour]], other.[[ISOMinute]], other.[[ISOSecond]], other.[[ISOMillisecond]], other.[[ISOMicrosecond]], other.[[ISONanosecond]], dateTime.[[Calendar]], settings.[[LargestUnit]], settings.[[Options]]).
    auto diff = TRY(difference_iso_date_time(vm, date_time.iso_year(), date_time.iso_month(), date_time.iso_day(), date_time.iso_hour(), date_time.iso_minute(), date_time.iso_second(), date_time.iso_millisecond(), date_time.iso_microsecond(), date_time.iso_nanosecond(), other->iso_year(), other->iso_month(), other->iso_day(), other->iso_hour(), other->iso_minute(), other->iso_second(), other->iso_millisecond(), other->iso_microsecond(), other->iso_nanosecond(), date_time.calendar(), settings.largest_unit, settings.options));

    // 6. Let relativeTo be ! CreateTemporalDate(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[Calendar]]).
    auto* relative_to = MUST(create_temporal_date(vm, date_time.iso_year(), date_time.iso_month(), date_time.iso_day(), date_time.calendar()));

    // 7. Let roundResult be (? RoundDuration(diff.[[Years]], diff.[[Months]], diff.[[Weeks]], diff.[[Days]], diff.[[Hours]], diff.[[Minutes]], diff.[[Seconds]], diff.[[Milliseconds]], diff.[[Microseconds]], diff.[[Nanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]], relativeTo)).[[DurationRecord]].
    // FIXME: AD-HOC calendar records use as this AO is not up to date with latest spec
    auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { date_time.calendar() }, { { CalendarMethod::DateAdd, CalendarMethod::DateUntil } }));
    auto round_result = TRY(round_duration(vm, diff.years, diff.months, diff.weeks, diff.days, diff.hours, diff.minutes, diff.seconds, diff.milliseconds, diff.microseconds, diff.nanoseconds, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode, relative_to, calendar_record)).duration_record;

    // 8. Let result be ? BalanceDuration(roundResult.[[Days]], roundResult.[[Hours]], roundResult.[[Minutes]], roundResult.[[Seconds]], roundResult.[[Milliseconds]], roundResult.[[Microseconds]], roundResult.[[Nanoseconds]], settings.[[LargestUnit]]).
    auto result = MUST(balance_duration(vm, round_result.days, round_result.hours, round_result.minutes, round_result.seconds, round_result.milliseconds, round_result.microseconds, Crypto::SignedBigInteger { round_result.nanoseconds }, settings.largest_unit));

    // 9. Return ! CreateTemporalDuration(sign × roundResult.[[Years]], sign × roundResult.[[Months]], sign × roundResult.[[Weeks]], sign × result.[[Days]], sign × result.[[Hours]], sign × result.[[Minutes]], sign × result.[[Seconds]], sign × result.[[Milliseconds]], sign × result.[[Microseconds]], sign × result.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, sign * round_result.years, sign * round_result.months, sign * round_result.weeks, sign * result.days, sign * result.hours, sign * result.minutes, sign * result.seconds, sign * result.milliseconds, sign * result.microseconds, sign * result.nanoseconds));
}

// 5.5.12 AddDurationToOrSubtractDurationFromPlainDateTime ( operation, dateTime, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoorsubtractdurationfromplaindatetime
ThrowCompletionOr<PlainDateTime*> add_duration_to_or_subtract_duration_from_plain_date_time(VM& vm, ArithmeticOperation operation, PlainDateTime& date_time, Value temporal_duration_like, Value options_value)
{
    // 1. If operation is subtract, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == ArithmeticOperation::Subtract ? -1 : 1;

    // 2. Let duration be ? ToTemporalDurationRecord(temporalDurationLike).
    auto duration = TRY(to_temporal_duration_record(vm, temporal_duration_like));

    // 3. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, options_value));

    // 4. Let result be ? AddDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], dateTime.[[Calendar]], sign × duration.[[Years]], sign × duration.[[Months]], sign × duration.[[Weeks]], sign × duration.[[Days]], sign × duration.[[Hours]], sign × duration.[[Minutes]], sign × duration.[[Seconds]], sign × duration.[[Milliseconds]], sign × duration.[[Microseconds]], sign × duration.[[Nanoseconds]], options).
    auto result = TRY(add_date_time(vm, date_time.iso_year(), date_time.iso_month(), date_time.iso_day(), date_time.iso_hour(), date_time.iso_minute(), date_time.iso_second(), date_time.iso_millisecond(), date_time.iso_microsecond(), date_time.iso_nanosecond(), date_time.calendar(), sign * duration.years, sign * duration.months, sign * duration.weeks, sign * duration.days, sign * duration.hours, sign * duration.minutes, sign * duration.seconds, sign * duration.milliseconds, sign * duration.microseconds, sign * duration.nanoseconds, options));

    // 5. Assert: IsValidISODate(result.[[Year]], result.[[Month]], result.[[Day]]) is true.
    VERIFY(is_valid_iso_date(result.year, result.month, result.day));

    // 6. Assert: IsValidTime(result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]]) is true.
    VERIFY(is_valid_time(result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond));

    // 7. Return ? CreateTemporalDateTime(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], dateTime.[[Calendar]]).
    return create_temporal_date_time(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, date_time.calendar());
}

}
