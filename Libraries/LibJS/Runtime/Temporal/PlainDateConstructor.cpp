/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainDateConstructor);

// 3.1 The Temporal.PlainDate Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaindate-constructor
PlainDateConstructor::PlainDateConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PlainDate.as_string(), realm.intrinsics().function_prototype())
{
}

void PlainDateConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 3.2.1 Temporal.PlainDate.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_plain_date_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(3), Attribute::Configurable);
}

// 3.1.1 Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate
ThrowCompletionOr<Value> PlainDateConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainDate");
}

// 3.1.1 Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate
ThrowCompletionOr<GC::Ref<Object>> PlainDateConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto iso_year = vm.argument(0);
    auto iso_month = vm.argument(1);
    auto iso_day = vm.argument(2);
    auto calendar_value = vm.argument(3);

    // 2. Let y be ? ToIntegerWithTruncation(isoYear).
    auto year = TRY(to_integer_with_truncation(vm, iso_year, ErrorType::TemporalInvalidPlainDate));

    // 3. Let m be ? ToIntegerWithTruncation(isoMonth).
    auto month = TRY(to_integer_with_truncation(vm, iso_month, ErrorType::TemporalInvalidPlainDate));

    // 4. Let d be ? ToIntegerWithTruncation(isoDay).
    auto day = TRY(to_integer_with_truncation(vm, iso_day, ErrorType::TemporalInvalidPlainDate));

    // 5. If calendar is undefined, set calendar to "iso8601".
    if (calendar_value.is_undefined())
        calendar_value = PrimitiveString::create(vm, "iso8601"_string);

    // 6. If calendar is not a String, throw a TypeError exception.
    if (!calendar_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, "calendar"sv);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    auto calendar = TRY(canonicalize_calendar(vm, calendar_value.as_string().utf8_string_view()));

    // 8. If IsValidISODate(y, m, d) is false, throw a RangeError exception.
    if (!is_valid_iso_date(year, month, day))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

    // 9. Let isoDate be CreateISODateRecord(y, m, d).
    auto iso_date = create_iso_date_record(year, month, day);

    // 10. Return ? CreateTemporalDate(isoDate, calendar, NewTarget).
    return TRY(create_temporal_date(vm, iso_date, move(calendar), &new_target));
}

// 3.2.2 3.2.2 Temporal.PlainDate.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.from
JS_DEFINE_NATIVE_FUNCTION(PlainDateConstructor::from)
{
    // 1. Return ? ToTemporalDate(item, options).
    return TRY(to_temporal_date(vm, vm.argument(0), vm.argument(1)));
}

// 3.2.3 Temporal.PlainDate.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.compare
JS_DEFINE_NATIVE_FUNCTION(PlainDateConstructor::compare)
{
    // 1. Set one to ? ToTemporalDate(one).
    auto one = TRY(to_temporal_date(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalDate(two).
    auto two = TRY(to_temporal_date(vm, vm.argument(1)));

    // 3. Return 𝔽(CompareISODate(one.[[ISODate]], two.[[ISODate]])).
    return compare_iso_date(one->iso_date(), two->iso_date());
}

}
