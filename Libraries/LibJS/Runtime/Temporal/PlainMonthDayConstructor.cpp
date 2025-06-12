/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainMonthDayConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainMonthDayConstructor);

// 10.1 The Temporal.PlainMonthDay Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plainmonthday-constructor
PlainMonthDayConstructor::PlainMonthDayConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PlainMonthDay.as_string(), realm.intrinsics().function_prototype())
{
}

void PlainMonthDayConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 10.2.1 Temporal.PlainMonthDay.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_plain_month_day_prototype(), 0);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
}

// 10.1.1 Temporal.PlainMonthDay ( isoMonth, isoDay [ , calendar [ , referenceISOYear ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday
ThrowCompletionOr<Value> PlainMonthDayConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainMonthDay");
}

// 10.1.1 Temporal.PlainMonthDay ( isoMonth, isoDay [ , calendar [ , referenceISOYear ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday
ThrowCompletionOr<GC::Ref<Object>> PlainMonthDayConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto iso_month = vm.argument(0);
    auto iso_day = vm.argument(1);
    auto calendar_value = vm.argument(2);
    auto reference_iso_year = vm.argument(3);

    // 2. If referenceISOYear is undefined, then
    if (reference_iso_year.is_undefined()) {
        // a. Set referenceISOYear to 1972𝔽 (the first ISO 8601 leap year after the epoch).
        reference_iso_year = Value { 1972 };
    }

    // 3. Let m be ? ToIntegerWithTruncation(isoMonth).
    auto month = TRY(to_integer_with_truncation(vm, iso_month, ErrorType::TemporalInvalidPlainMonthDay));

    // 4. Let d be ? ToIntegerWithTruncation(isoDay).
    auto day = TRY(to_integer_with_truncation(vm, iso_day, ErrorType::TemporalInvalidPlainMonthDay));

    // 5. If calendar is undefined, set calendar to "iso8601".
    if (calendar_value.is_undefined())
        calendar_value = PrimitiveString::create(vm, "iso8601"_string);

    // 6. If calendar is not a String, throw a TypeError exception.
    if (!calendar_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, calendar_value);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    auto calendar = TRY(canonicalize_calendar(vm, calendar_value.as_string().utf8_string_view()));

    // 8. Let y be ? ToIntegerWithTruncation(referenceISOYear).
    auto year = TRY(to_integer_with_truncation(vm, reference_iso_year, ErrorType::TemporalInvalidPlainMonthDay));

    // 9. If IsValidISODate(y, m, d) is false, throw a RangeError exception.
    if (!is_valid_iso_date(year, month, day))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainMonthDay);

    // 10. Let isoDate be CreateISODateRecord(y, m, d).
    auto iso_date = create_iso_date_record(year, month, day);

    // 11. Return ? CreateTemporalMonthDay(isoDate, calendar, NewTarget).
    return TRY(create_temporal_month_day(vm, iso_date, move(calendar), &new_target));
}

// 10.2.2 Temporal.PlainMonthDay.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.from
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayConstructor::from)
{
    // 1. Return ? ToTemporalMonthDay(item, options).
    return TRY(to_temporal_month_day(vm, vm.argument(0), vm.argument(1)));
}

}
