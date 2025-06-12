/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/PlainYearMonthConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainYearMonthConstructor);

// 9.1 The Temporal.PlainYearMonth Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plainyearmonth-constructor
PlainYearMonthConstructor::PlainYearMonthConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PlainYearMonth.as_string(), realm.intrinsics().function_prototype())
{
}

void PlainYearMonthConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 9.2.1 Temporal.PlainYearMonth.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_plain_year_month_prototype(), 0);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);
}

// 9.1.1 Temporal.PlainYearMonth ( isoYear, isoMonth [ , calendar [ , referenceISODay ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth
ThrowCompletionOr<Value> PlainYearMonthConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, then
    //     a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainYearMonth");
}

// 9.1.1 Temporal.PlainYearMonth ( isoYear, isoMonth [ , calendar [ , referenceISODay ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth
ThrowCompletionOr<GC::Ref<Object>> PlainYearMonthConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto iso_year = vm.argument(0);
    auto iso_month = vm.argument(1);
    auto calendar_value = vm.argument(2);
    auto reference_iso_day = vm.argument(3);

    // 2. If referenceISODay is undefined, then
    if (reference_iso_day.is_undefined()) {
        // a. Set referenceISODay to 1𝔽.
        reference_iso_day = Value { 1 };
    }

    // 3. Let y be ? ToIntegerWithTruncation(isoYear).
    auto year = TRY(to_integer_with_truncation(vm, iso_year, ErrorType::TemporalInvalidPlainYearMonth));

    // 4. Let m be ? ToIntegerWithTruncation(isoMonth).
    auto month = TRY(to_integer_with_truncation(vm, iso_month, ErrorType::TemporalInvalidPlainYearMonth));

    // 5. If calendar is undefined, set calendar to "iso8601".
    if (calendar_value.is_undefined())
        calendar_value = PrimitiveString::create(vm, "iso8601"_string);

    // 6. If calendar is not a String, throw a TypeError exception.
    if (!calendar_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, "calendar"sv);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    auto calendar = TRY(canonicalize_calendar(vm, calendar_value.as_string().utf8_string_view()));

    // 8. Let ref be ? ToIntegerWithTruncation(referenceISODay).
    auto reference = TRY(to_integer_with_truncation(vm, reference_iso_day, ErrorType::TemporalInvalidPlainYearMonth));

    // 9. If IsValidISODate(y, m, ref) is false, throw a RangeError exception.
    if (!is_valid_iso_date(year, month, reference))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainYearMonth);

    // 10. Let isoDate be CreateISODateRecord(y, m, ref).
    auto iso_date = create_iso_date_record(year, month, reference);

    // 11. Return ? CreateTemporalYearMonth(isoDate, calendar, NewTarget).
    return TRY(create_temporal_year_month(vm, iso_date, move(calendar), &new_target));
}

// 9.2.2 Temporal.PlainYearMonth.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.from
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthConstructor::from)
{
    // 1. Return ? ToTemporalYearMonth(item, options).
    return TRY(to_temporal_year_month(vm, vm.argument(0), vm.argument(1)));
}

// 9.2.3 Temporal.PlainYearMonth.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.compare
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthConstructor::compare)
{
    // 1. Set one to ? ToTemporalYearMonth(one).
    auto one = TRY(to_temporal_year_month(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalYearMonth(two).
    auto two = TRY(to_temporal_year_month(vm, vm.argument(1)));

    // 3. Return 𝔽(CompareISODate(one.[[ISODate]], two.[[ISODate]])).
    return compare_iso_date(one->iso_date(), two->iso_date());
}

}
