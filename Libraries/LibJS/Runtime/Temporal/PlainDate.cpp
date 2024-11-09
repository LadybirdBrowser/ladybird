/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateConstructor.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainDate);

// 3 Temporal.PlainDate Objects, https://tc39.es/proposal-temporal/#sec-temporal-plaindate-objects
PlainDate::PlainDate(i32 year, u8 month, u8 day, Object& calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_year(year)
    , m_iso_month(month)
    , m_iso_day(day)
    , m_calendar(calendar)
{
}

void PlainDate::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_calendar);
}

// 3.5.2 CreateISODateRecord ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-create-iso-date-record
ISODateRecord create_iso_date_record(i32 year, u8 month, u8 day)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Return the Record { [[Year]]: year, [[Month]]: month, [[Day]]: day }.
    return { .year = year, .month = month, .day = day };
}

// 3.5.1 CreateTemporalDate ( isoYear, isoMonth, isoDay, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaldate
ThrowCompletionOr<PlainDate*> create_temporal_date(VM& vm, i32 iso_year, u8 iso_month, u8 iso_day, Object& calendar, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: isoYear is an integer.
    // 2. Assert: isoMonth is an integer.
    // 3. Assert: isoDay is an integer.
    // 4. Assert: Type(calendar) is Object.

    // 5. If IsValidISODate(isoYear, isoMonth, isoDay) is false, throw a RangeError exception.
    if (!is_valid_iso_date(iso_year, iso_month, iso_day))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

    // 6. If ISODateTimeWithinLimits(isoYear, isoMonth, isoDay, 12, 0, 0, 0, 0, 0) is false, throw a RangeError exception.
    if (!iso_date_time_within_limits(iso_year, iso_month, iso_day, 12, 0, 0, 0, 0, 0))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

    // 7. If newTarget is not present, set newTarget to %Temporal.PlainDate%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_date_constructor();

    // 8. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainDate.prototype%", « [[InitializedTemporalDate]], [[ISOYear]], [[ISOMonth]], [[ISODay]], [[Calendar]] »).
    // 9. Set object.[[ISOYear]] to isoYear.
    // 10. Set object.[[ISOMonth]] to isoMonth.
    // 11. Set object.[[ISODay]] to isoDay.
    // 12. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainDate>(vm, *new_target, &Intrinsics::temporal_plain_date_prototype, iso_year, iso_month, iso_day, calendar));

    return object.ptr();
}

// 3.5.2 ToTemporalDate ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldate
ThrowCompletionOr<PlainDate*> to_temporal_date(VM& vm, Value item, Object const* options)
{
    // 1. If options is not present, set options to undefined.
    // 2. Assert: Type(options) is Object or Undefined.

    // 3. If Type(item) is Object, then
    if (item.is_object()) {
        auto& item_object = item.as_object();
        // a. If item has an [[InitializedTemporalDate]] internal slot, then
        if (is<PlainDate>(item_object)) {
            // i. Return item.
            return static_cast<PlainDate*>(&item_object);
        }

        // b. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(item_object)) {
            auto& zoned_date_time = static_cast<ZonedDateTime&>(item_object);

            // i. Perform ? ToTemporalOverflow(options).
            (void)TRY(to_temporal_overflow(vm, options));

            // ii. Let instant be ! CreateTemporalInstant(item.[[Nanoseconds]]).
            auto* instant = create_temporal_instant(vm, zoned_date_time.nanoseconds()).release_value();

            // iii. Let plainDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(item.[[TimeZone]], instant, item.[[Calendar]]).
            auto* plain_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &zoned_date_time.time_zone(), *instant, zoned_date_time.calendar()));

            // iv. Return ! CreateTemporalDate(plainDateTime.[[ISOYear]], plainDateTime.[[ISOMonth]], plainDateTime.[[ISODay]], plainDateTime.[[Calendar]]).
            return create_temporal_date(vm, plain_date_time->iso_year(), plain_date_time->iso_month(), plain_date_time->iso_day(), plain_date_time->calendar());
        }

        // c. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if (is<PlainDateTime>(item_object)) {
            auto& date_time_item = static_cast<PlainDateTime&>(item_object);

            // i. Perform ? ToTemporalOverflow(options).
            (void)TRY(to_temporal_overflow(vm, options));

            // ii. Return ! CreateTemporalDate(item.[[ISOYear]], item.[[ISOMonth]], item.[[ISODay]], item.[[Calendar]]).
            return create_temporal_date(vm, date_time_item.iso_year(), date_time_item.iso_month(), date_time_item.iso_day(), date_time_item.calendar());
        }

        // d. Let calendar be ? GetTemporalCalendarWithISODefault(item).
        auto* calendar = TRY(get_temporal_calendar_with_iso_default(vm, item_object));

        // e. Let fieldNames be ? CalendarFields(calendar, « "day", "month", "monthCode", "year" »).
        auto field_names = TRY(calendar_fields(vm, *calendar, { "day"sv, "month"sv, "monthCode"sv, "year"sv }));

        // f. Let fields be ? PrepareTemporalFields(item, fieldNames, «»).
        auto* fields = TRY(prepare_temporal_fields(vm, item_object, field_names, Vector<StringView> {}));

        // g. Return ? CalendarDateFromFields(calendar, fields, options).
        return calendar_date_from_fields(vm, *calendar, *fields, options);
    }

    // 4. Perform ? ToTemporalOverflow(options).
    (void)TRY(to_temporal_overflow(vm, options));

    // 5. Let string be ? ToString(item).
    auto string = TRY(item.to_string(vm));

    // 6. Let result be ? ParseTemporalDateString(string).
    auto result = TRY(parse_temporal_date_string(vm, string));

    // 7. Assert: IsValidISODate(result.[[Year]], result.[[Month]], result.[[Day]]) is true.
    VERIFY(is_valid_iso_date(result.year, result.month, result.day));

    // 8. Let calendar be ? ToTemporalCalendarWithISODefault(result.[[Calendar]]).
    auto* calendar = TRY(to_temporal_calendar_with_iso_default(vm, result.calendar.has_value() ? PrimitiveString::create(vm, *result.calendar) : js_undefined()));

    // 9. Return ? CreateTemporalDate(result.[[Year]], result.[[Month]], result.[[Day]], calendar).
    return create_temporal_date(vm, result.year, result.month, result.day, *calendar);
}

// 3.5.3 DifferenceISODate ( y1, m1, d1, y2, m2, d2, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-differenceisodate
DateDurationRecord difference_iso_date(VM& vm, i32 year1, u8 month1, u8 day1, i32 year2, u8 month2, u8 day2, StringView largest_unit)
{
    VERIFY(largest_unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv));

    // 1. If largestUnit is "year" or "month", then
    if (largest_unit.is_one_of("year"sv, "month"sv)) {
        // a. Let sign be -(! CompareISODate(y1, m1, d1, y2, m2, d2)).
        auto sign = -compare_iso_date(year1, month1, day1, year2, month2, day2);

        // b. If sign is 0, return ! CreateDateDurationRecord(0, 0, 0, 0).
        if (sign == 0)
            return create_date_duration_record(0, 0, 0, 0);

        // c. Let start be the Record { [[Year]]: y1, [[Month]]: m1, [[Day]]: d1 }.
        auto start = ISODateRecord { .year = year1, .month = month1, .day = day1 };

        // d. Let end be the Record { [[Year]]: y2, [[Month]]: m2, [[Day]]: d2 }.
        auto end = ISODateRecord { .year = year2, .month = month2, .day = day2 };

        // e. Let years be end.[[Year]] - start.[[Year]].
        double years = end.year - start.year;

        // f. Let mid be ! AddISODate(y1, m1, d1, years, 0, 0, 0, "constrain").
        auto mid = MUST(add_iso_date(vm, year1, month1, day1, years, 0, 0, 0, "constrain"sv));

        // g. Let midSign be -(! CompareISODate(mid.[[Year]], mid.[[Month]], mid.[[Day]], y2, m2, d2)).
        auto mid_sign = -compare_iso_date(mid.year, mid.month, mid.day, year2, month2, day2);

        // h. If midSign is 0, then
        if (mid_sign == 0) {
            // i. If largestUnit is "year", return ! CreateDateDurationRecord(years, 0, 0, 0).
            if (largest_unit == "year"sv)
                return create_date_duration_record(years, 0, 0, 0);

            // ii. Return ! CreateDateDurationRecord(0, years × 12, 0, 0).
            return create_date_duration_record(0, years * 12, 0, 0);
        }

        // i. Let months be end.[[Month]] - start.[[Month]].
        double months = end.month - start.month;

        // j. If midSign is not equal to sign, then
        if (mid_sign != sign) {
            // i. Set years to years - sign.
            years -= sign;

            // ii. Set months to months + sign × 12.
            months += sign * 12;
        }

        // k. Set mid to ! AddISODate(y1, m1, d1, years, months, 0, 0, "constrain").
        mid = MUST(add_iso_date(vm, year1, month1, day1, years, months, 0, 0, "constrain"sv));

        // l. Set midSign to -(! CompareISODate(mid.[[Year]], mid.[[Month]], mid.[[Day]], y2, m2, d2)).
        mid_sign = -compare_iso_date(mid.year, mid.month, mid.day, year2, month2, day2);

        // m. If midSign is 0, then
        if (mid_sign == 0) {
            // i. If largestUnit is "year", return ! CreateDateDurationRecord(years, months, 0, 0).
            if (largest_unit == "year"sv)
                return create_date_duration_record(years, months, 0, 0);

            // ii. Return ! CreateDateDurationRecord(0, months + years × 12, 0, 0).
            return create_date_duration_record(0, months + years * 12, 0, 0);
        }

        // n. If midSign is not equal to sign, then
        if (mid_sign != sign) {
            // i. Set months to months - sign.
            months -= sign;

            // ii. If months is equal to -sign, then
            if (months == -sign) {
                // 1. Set years to years - sign.
                years -= sign;

                // 2. Set months to 11 × sign.
                months = 11 * sign;
            }

            // iii. Set mid to ! AddISODate(y1, m1, d1, years, months, 0, 0, "constrain").
            mid = MUST(add_iso_date(vm, year1, month1, day1, years, months, 0, 0, "constrain"sv));
        }

        double days;

        // o. If mid.[[Month]] = end.[[Month]], then
        if (mid.month == end.month) {
            // i. Assert: mid.[[Year]] = end.[[Year]].
            VERIFY(mid.year == end.year);

            // ii. Let days be end.[[Day]] - mid.[[Day]].
            days = end.day - mid.day;
        }
        // p. Else if sign < 0, let days be -mid.[[Day]] - (! ISODaysInMonth(end.[[Year]], end.[[Month]]) - end.[[Day]]).
        else if (sign < 0) {
            days = -mid.day - (iso_days_in_month(end.year, end.month) - end.day);
        }
        // q. Else, let days be end.[[Day]] + (! ISODaysInMonth(mid.[[Year]], mid.[[Month]]) - mid.[[Day]]).
        else {
            days = end.day + (iso_days_in_month(mid.year, mid.month) - mid.day);
        }

        // r. If largestUnit is "month", then
        if (largest_unit == "month"sv) {
            // i. Set months to months + years × 12.
            months += years * 12;

            // ii. Set years to 0.
            years = 0;
        }

        // s. Return ! CreateDateDurationRecord(years, months, 0, days).
        return create_date_duration_record(years, months, 0, days);
    }
    // 2. Else,
    else {
        // a. Assert: largestUnit is "day" or "week".
        VERIFY(largest_unit.is_one_of("day"sv, "week"sv));

        // b. Let epochDays1 be MakeDay(𝔽(y1), 𝔽(m1 - 1), 𝔽(d1)).
        auto epoch_days_1 = make_day(year1, month1 - 1, day1);

        // c. Assert: epochDays1 is finite.
        VERIFY(isfinite(epoch_days_1));

        // d. Let epochDays2 be MakeDay(𝔽(y2), 𝔽(m2 - 1), 𝔽(d2)).
        auto epoch_days_2 = make_day(year2, month2 - 1, day2);

        // e. Assert: epochDays2 is finite.
        VERIFY(isfinite(epoch_days_2));

        // f. Let days be ℝ(epochDays2) - ℝ(epochDays1).
        auto days = epoch_days_2 - epoch_days_1;

        // g. Let weeks be 0.
        double weeks = 0;

        // h. If largestUnit is "week", then
        if (largest_unit == "week"sv) {
            // i. Set weeks to truncate(days / 7).
            weeks = trunc(days / 7);

            // ii. Set days to remainder(days, 7).
            days = fmod(days, 7);
        }

        // i. Return ! CreateDateDurationRecord(0, 0, weeks, days).
        return create_date_duration_record(0, 0, weeks, days);
    }
}

// 3.5.4 RegulateISODate ( year, month, day, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulateisodate
ThrowCompletionOr<ISODateRecord> regulate_iso_date(VM& vm, double year, double month, double day, StringView overflow)
{
    VERIFY(year == trunc(year) && month == trunc(month) && day == trunc(day));

    // 1. If overflow is "constrain", then
    if (overflow == "constrain"sv) {
        // IMPLEMENTATION DEFINED: This is an optimization that allows us to treat this double as normal integer from this point onwards. This
        // does not change the exposed behavior as the parent's call to CreateTemporalDate will immediately check that this value is a valid
        // ISO value for years: -273975 - 273975, which is a subset of this check.
        if (!AK::is_within_range<i32>(year))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

        // a. Set month to the result of clamping month between 1 and 12.
        month = clamp(month, 1, 12);

        // b. Let daysInMonth be ! ISODaysInMonth(year, month).
        auto days_in_month = iso_days_in_month(static_cast<i32>(year), static_cast<u8>(month));

        // c. Set day to the result of clamping day between 1 and daysInMonth.
        day = clamp(day, 1, days_in_month);

        // d. Return CreateISODateRecord(year, month, day).
        return create_iso_date_record(static_cast<i32>(year), static_cast<u8>(month), static_cast<u8>(day));
    }
    // 2. Else,
    else {
        // a. Assert: overflow is "reject".
        VERIFY(overflow == "reject"sv);

        // IMPLEMENTATION DEFINED: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
        // This does not change the exposed behavior as the call to IsValidISODate will immediately check that these values are valid ISO
        // values (for years: -273975 - 273975, for months: 1 - 12, for days: 1 - 31) all of which are subsets of this check.
        if (!AK::is_within_range<i32>(year) || !AK::is_within_range<u8>(month) || !AK::is_within_range<u8>(day))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

        auto y = static_cast<i32>(year);
        auto m = static_cast<u8>(month);
        auto d = static_cast<u8>(day);
        // b. If IsValidISODate(year, month, day) is false, throw a RangeError exception.
        if (!is_valid_iso_date(y, m, d))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

        // c. Return the Record { [[Year]]: year, [[Month]]: month, [[Day]]: day }.
        return ISODateRecord { .year = y, .month = m, .day = d };
    }
}

// 3.5.5 IsValidISODate ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidisodate
bool is_valid_iso_date(i32 year, u8 month, u8 day)
{
    // 1. If month < 1 or month > 12, then
    if (month < 1 || month > 12) {
        // a. Return false.
        return false;
    }

    // 2. Let daysInMonth be ! ISODaysInMonth(year, month).
    auto days_in_month = iso_days_in_month(year, month);

    // 3. If day < 1 or day > daysInMonth, then
    if (day < 1 || day > days_in_month) {
        // a. Return false.
        return false;
    }

    // 4. Return true.
    return true;
}

// 3.5.6 DifferenceDate ( calendarRec, one, two, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencedate
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_date(VM& vm, CalendarMethods const& calendar_record, PlainDate const& one, PlainDate const& two, Object const& options)
{
    // FIXME: 1. Assert: one.[[Calendar]] and two.[[Calendar]] have been determined to be equivalent as with CalendarEquals.
    // FIXME: 2. Assert: options is an ordinary Object.

    // 3. Assert: options.[[Prototype]] is null.
    VERIFY(!options.prototype());

    // 4. Assert: options has a "largestUnit" data property.
    VERIFY(MUST(options.has_own_property(vm.names.largestUnit)));

    // 5. If one.[[ISOYear]] = two.[[ISOYear]] and one.[[ISOMonth]] = two.[[ISOMonth]] and one.[[ISODay]] = two.[[ISODay]], then
    if (one.iso_year() == two.iso_year() && one.iso_month() == two.iso_month() && one.iso_day() == two.iso_day()) {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    }

    // 6. If ! Get(options, "largestUnit") is "day", then
    auto largest_unit = MUST(options.get(vm.names.largestUnit));
    if (largest_unit.is_string() && largest_unit.as_string().utf8_string_view() == "day"sv) {
        // a. Let days be DaysUntil(one, two).
        auto days = days_until(one, two);

        // b. Return ! CreateTemporalDuration(0, 0, 0, days, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, days, 0, 0, 0, 0, 0, 0));
    }

    // 7. Return ? CalendarDateUntil(calendarRec, one, two, options).
    return TRY(calendar_date_until(vm, calendar_record, Value { &one }, Value { &two }, options));
}

// 3.5.6 BalanceISODate ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisodate
ISODateRecord balance_iso_date(double year, double month, double day)
{
    // 1. Let epochDays be MakeDay(𝔽(year), 𝔽(month - 1), 𝔽(day)).
    auto epoch_days = make_day(year, month - 1, day);

    // 2. Assert: epochDays is finite.
    VERIFY(isfinite(epoch_days));

    // 3. Let ms be MakeDate(epochDays, +0𝔽).
    auto ms = make_date(epoch_days, 0);

    // 4. Return CreateISODateRecord(ℝ(YearFromTime(ms)), ℝ(MonthFromTime(ms)) + 1, ℝ(DateFromTime(ms))).
    return create_iso_date_record(year_from_time(ms), static_cast<u8>(month_from_time(ms) + 1), date_from_time(ms));
}

// 3.5.7 PadISOYear ( y ), https://tc39.es/proposal-temporal/#sec-temporal-padisoyear
ThrowCompletionOr<String> pad_iso_year(VM& vm, i32 y)
{
    // 1. Assert: y is an integer.

    // 2. If y ≥ 0 and y ≤ 9999, then
    if (y >= 0 && y <= 9999) {
        // a. Return ToZeroPaddedDecimalString(y, 4).
        return TRY_OR_THROW_OOM(vm, String::formatted("{:04}", y));
    }

    // 3. If y > 0, let yearSign be "+"; otherwise, let yearSign be "-".
    auto year_sign = y > 0 ? '+' : '-';

    // 4. Let year be ToZeroPaddedDecimalString(abs(y), 6).
    // 5. Return the string-concatenation of yearSign and year.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}{:06}", year_sign, abs(y)));
}

// 3.5.8 TemporalDateToString ( temporalDate, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldatetostring
ThrowCompletionOr<String> temporal_date_to_string(VM& vm, PlainDate& temporal_date, StringView show_calendar)
{
    // 1. Assert: Type(temporalDate) is Object.
    // 2. Assert: temporalDate has an [[InitializedTemporalDate]] internal slot.

    // 3. Let year be ! PadISOYear(temporalDate.[[ISOYear]]).
    auto year = MUST_OR_THROW_OOM(pad_iso_year(vm, temporal_date.iso_year()));

    // 4. Let month be ToZeroPaddedDecimalString(monthDay.[[ISOMonth]], 2).
    auto month = TRY_OR_THROW_OOM(vm, String::formatted("{:02}", temporal_date.iso_month()));

    // 5. Let day be ToZeroPaddedDecimalString(monthDay.[[ISODay]], 2).
    auto day = TRY_OR_THROW_OOM(vm, String::formatted("{:02}", temporal_date.iso_day()));

    // 6. Let calendar be ? MaybeFormatCalendarAnnotation(temporalDate.[[Calendar]], showCalendar).
    auto calendar = TRY(maybe_format_calendar_annotation(vm, &temporal_date.calendar(), show_calendar));

    // 7. Return the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), month, the code unit 0x002D (HYPHEN-MINUS), day, and calendar.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}-{}-{}{}", year, month, day, calendar));
}

// 3.5.9 AddISODate ( year, month, day, years, months, weeks, days, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-addisodate
ThrowCompletionOr<ISODateRecord> add_iso_date(VM& vm, i32 year, u8 month, u8 day, double years, double months, double weeks, double days, StringView overflow)
{
    // 1. Assert: year, month, day, years, months, weeks, and days are integers.
    VERIFY(years == trunc(years) && months == trunc(months) && weeks == trunc(weeks) && days == trunc(days));

    // 2. Assert: overflow is either "constrain" or "reject".
    VERIFY(overflow == "constrain"sv || overflow == "reject"sv);

    // 3. Let intermediate be ! BalanceISOYearMonth(year + years, month + months).
    auto intermediate_year_month = balance_iso_year_month(year + years, month + months);

    // 4. Let intermediate be ? RegulateISODate(intermediate.[[Year]], intermediate.[[Month]], day, overflow).
    auto intermediate = TRY(regulate_iso_date(vm, intermediate_year_month.year, intermediate_year_month.month, day, overflow));

    // 5. Set days to days + 7 × weeks.
    days += 7 * weeks;

    // 6. Let d be intermediate.[[Day]] + days.
    auto d = intermediate.day + days;

    // 7. Return BalanceISODate(intermediate.[[Year]], intermediate.[[Month]], d).
    return balance_iso_date(intermediate.year, intermediate.month, d);
}

// 3.5.10 CompareISODate ( y1, m1, d1, y2, m2, d2 ), https://tc39.es/proposal-temporal/#sec-temporal-compareisodate
i8 compare_iso_date(i32 year1, u8 month1, u8 day1, i32 year2, u8 month2, u8 day2)
{
    // 1. Assert: y1, m1, d1, y2, m2, and d2 are integers.

    // 2. If y1 > y2, return 1.
    if (year1 > year2)
        return 1;

    // 3. If y1 < y2, return -1.
    if (year1 < year2)
        return -1;

    // 4. If m1 > m2, return 1.
    if (month1 > month2)
        return 1;

    // 5. If m1 < m2, return -1.
    if (month1 < month2)
        return -1;

    // 6. If d1 > d2, return 1.
    if (day1 > day2)
        return 1;

    // 7. If d1 < d2, return -1.
    if (day1 < day2)
        return -1;

    // 8. Return 0.
    return 0;
}

// 3.5.11 DifferenceTemporalPlainDate ( operation, temporalDate, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaindate
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_plain_date(VM& vm, DifferenceOperation operation, PlainDate& temporal_date, Value other_value, Value options)
{
    // 1. If operation is SINCE, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == DifferenceOperation::Since ? -1 : 1;

    // 2. Set other to ? ToTemporalDate(other).
    auto* other = TRY(to_temporal_date(vm, other_value));

    // 3. If ? CalendarEquals(temporalDate.[[Calendar]], other.[[Calendar]]) is false, throw a RangeError exception.
    if (!TRY(calendar_equals(vm, temporal_date.calendar(), other->calendar())))
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);

    // 4. Let resolvedOptions be ? SnapshotOwnProperties(? GetOptionsObject(options), null).
    auto resolved_options = TRY(TRY(get_options_object(vm, options))->snapshot_own_properties(vm, nullptr));

    // 5. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATE, « », "day", "day").
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::Date, {}, { "day"sv }, "day"sv));

    // 6. If temporalDate.[[ISOYear]] = other.[[ISOYear]], and temporalDate.[[ISOMonth]] = other.[[ISOMonth]], and temporalDate.[[ISODay]] = other.[[ISODay]], then
    if (temporal_date.iso_year() == other->iso_year() && temporal_date.iso_month() == other->iso_month() && temporal_date.iso_day() == other->iso_day()) {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    }

    // 7. Let calendarRec be ? CreateCalendarMethodsRecord(temporalDate.[[Calendar]], « DATE-ADD, DATE-UNTIL »).
    // FIXME: The type of calendar in PlainDate does not align with latest spec
    auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { temporal_date.calendar() }, { { CalendarMethod::DateAdd, CalendarMethod::DateUntil } }));

    // 8. Perform ! CreateDataPropertyOrThrow(resolvedOptions, "largestUnit", settings.[[LargestUnit]]).
    MUST(resolved_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, settings.largest_unit)));

    // 9. Let result be ? DifferenceDate(calendarRec, temporalDate, other, resolvedOptions).
    auto result = TRY(difference_date(vm, calendar_record, temporal_date, *other, resolved_options));

    // 10. If settings.[[SmallestUnit]] is "day" and settings.[[RoundingIncrement]] = 1, let roundingGranularityIsNoop be true; else let roundingGranularityIsNoop be false.
    bool rounding_granularity_is_noop = settings.smallest_unit == "day"sv && settings.rounding_increment == 1;

    // 11. If roundingGranularityIsNoop is false, then
    if (!rounding_granularity_is_noop) {
        // a. Let roundRecord be ? RoundDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], ZeroTimeDuration(), settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]], temporalDate, calendarRec).
        auto round_record = TRY(round_duration(vm, result->years(), result->months(), result->weeks(), result->days(), 0, 0, 0, 0, 0, 0, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode, &temporal_date, calendar_record)).duration_record;

        // FIXME: b. Let roundResult be roundRecord.[[NormalizedDuration]].
        // FIXME: c. Set result to ? BalanceDateDurationRelative(roundResult.[[Years]], roundResult.[[Months]], roundResult.[[Weeks]], roundResult.[[Days]], settings.[[LargestUnit]], settings.[[SmallestUnit]], temporalDate, calendarRec).
        result = MUST(create_temporal_duration(vm, round_record.years, round_record.months, round_record.weeks, round_record.days, 0, 0, 0, 0, 0, 0));
    }

    // 16. Return ! CreateTemporalDuration(sign × result.[[Years]], sign × result.[[Months]], sign × result.[[Weeks]], sign × result.[[Days]], 0, 0, 0, 0, 0, 0).
    return MUST(create_temporal_duration(vm, sign * result->years(), sign * result->months(), sign * result->weeks(), sign * result->days(), 0, 0, 0, 0, 0, 0));
}

}
