/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>

namespace JS::Temporal {

// 3.5.2 CreateISODateRecord ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-create-iso-date-record
ISODate create_iso_date_record(double year, double month, double day)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Return ISO Date Record { [[Year]]: year, [[Month]]: month, [[Day]]: day }.
    return { .year = static_cast<i32>(year), .month = static_cast<u8>(month), .day = static_cast<u8>(day) };
}

// 3.5.6 RegulateISODate ( year, month, day, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulateisodate
ThrowCompletionOr<ISODate> regulate_iso_date(VM& vm, double year, double month, double day, Overflow overflow)
{
    switch (overflow) {
    // 1. If overflow is CONSTRAIN, then
    case Overflow::Constrain:
        // a. Set month to the result of clamping month between 1 and 12.
        month = clamp(month, 1, 12);

        // b. Let daysInMonth be ISODaysInMonth(year, month).
        // c. Set day to the result of clamping day between 1 and daysInMonth.
        day = clamp(day, 1, iso_days_in_month(year, month));

        // AD-HOC: We further clamp the year to the range allowed by ISOYearMonthWithinLimits, to ensure we do not
        //         overflow when we store the year as an integer.
        year = clamp(year, -271821, 275760);

        break;

    // 2. Else,
    case Overflow::Reject:
        // a. Assert: overflow is REJECT.
        // b. If IsValidISODate(year, month, day) is false, throw a RangeError exception.
        if (!is_valid_iso_date(year, month, day))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);
        break;
    }

    // 3. Return CreateISODateRecord(year, month, day).
    return create_iso_date_record(year, month, day);
}

// 3.5.7 IsValidISODate ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidisodate
bool is_valid_iso_date(double year, double month, double day)
{
    // AD-HOC: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
    //         This does not change the exposed behavior as the call to CreateISODateRecord will immediately check that
    //         these values are valid ISO values (years: [-271821, 275760], months: [1, 12], days: [1, 31]), all of
    //         which are subsets of this check.
    if (!AK::is_within_range<i32>(year) || !AK::is_within_range<u8>(month) || !AK::is_within_range<u8>(day))
        return false;

    // 1. If month < 1 or month > 12, then
    if (month < 1 || month > 12) {
        // a. Return false.
        return false;
    }

    // 2. Let daysInMonth be ISODaysInMonth(year, month).
    auto days_in_month = iso_days_in_month(year, month);

    // 3. If day < 1 or day > daysInMonth, then
    if (day < 1 || day > days_in_month) {
        // a. Return false.
        return false;
    }

    // 4. Return true.
    return true;
}

// 3.5.11 ISODateWithinLimits ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodatewithinlimits
bool iso_date_within_limits(ISODate iso_date)
{
    // 1. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, NoonTimeRecord()).
    auto iso_date_time = combine_iso_date_and_time_record(iso_date, noon_time_record());

    // 2. Return ISODateTimeWithinLimits(isoDateTime).
    return iso_date_time_within_limits(iso_date_time);
}

}
