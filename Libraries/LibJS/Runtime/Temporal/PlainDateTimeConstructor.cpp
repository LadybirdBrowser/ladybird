/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainDateTimeConstructor.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainDateTimeConstructor);

// 5.1 The Temporal.PlainDateTime Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaindatetime-constructor
PlainDateTimeConstructor::PlainDateTimeConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PlainDateTime.as_string(), realm.intrinsics().function_prototype())
{
}

void PlainDateTimeConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 5.2.1 Temporal.PlainDateTime.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_plain_date_time_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(3), Attribute::Configurable);
}

// 5.1.1 Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond [ , calendar ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime
ThrowCompletionOr<Value> PlainDateTimeConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, then
    //     a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainDateTime");
}

// 5.1.1 Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond [ , calendar ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime
ThrowCompletionOr<GC::Ref<Object>> PlainDateTimeConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto calendar_value = vm.argument(9);

    auto next_integer_argument = [&, index = 0](Optional<double> fallback) mutable -> ThrowCompletionOr<double> {
        auto value = vm.argument(index++);
        if (value.is_undefined() && fallback.has_value())
            return *fallback;

        return to_integer_with_truncation(vm, value, ErrorType::TemporalInvalidPlainDateTime);
    };

    // 2. Set isoYear to ? ToIntegerWithTruncation(isoYear).
    auto iso_year = TRY(next_integer_argument({}));

    // 3. Set isoMonth to ? ToIntegerWithTruncation(isoMonth).
    auto iso_month = TRY(next_integer_argument({}));

    // 4. Set isoDay to ? ToIntegerWithTruncation(isoDay).
    auto iso_day = TRY(next_integer_argument({}));

    // 5. If hour is undefined, set hour to 0; else set hour to ? ToIntegerWithTruncation(hour).
    auto hour = TRY(next_integer_argument(0));

    // 6. If minute is undefined, set minute to 0; else set minute to ? ToIntegerWithTruncation(minute).
    auto minute = TRY(next_integer_argument(0));

    // 7. If second is undefined, set second to 0; else set second to ? ToIntegerWithTruncation(second).
    auto second = TRY(next_integer_argument(0));

    // 8. If millisecond is undefined, set millisecond to 0; else set millisecond to ? ToIntegerWithTruncation(millisecond).
    auto millisecond = TRY(next_integer_argument(0));

    // 9. If microsecond is undefined, set microsecond to 0; else set microsecond to ? ToIntegerWithTruncation(microsecond).
    auto microsecond = TRY(next_integer_argument(0));

    // 10. If nanosecond is undefined, set nanosecond to 0; else set nanosecond to ? ToIntegerWithTruncation(nanosecond).
    auto nanosecond = TRY(next_integer_argument(0));

    // 11. If calendar is undefined, set calendar to "iso8601".
    if (calendar_value.is_undefined())
        calendar_value = PrimitiveString::create(vm, "iso8601"_string);

    // 12. If calendar is not a String, throw a TypeError exception.
    if (!calendar_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, "calendar"sv);

    // 13. Set calendar to ? CanonicalizeCalendar(calendar).
    auto calendar = TRY(canonicalize_calendar(vm, calendar_value.as_string().utf8_string_view()));

    // 14. If IsValidISODate(isoYear, isoMonth, isoDay) is false, throw a RangeError exception.
    if (!is_valid_iso_date(iso_year, iso_month, iso_day))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 15. Let isoDate be CreateISODateRecord(isoYear, isoMonth, isoDay).
    auto iso_date = create_iso_date_record(iso_year, iso_month, iso_day);

    // 16. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
    if (!is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 17. Let time be CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond).
    auto time = create_time_record(hour, minute, second, millisecond, microsecond, nanosecond);

    // 18. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, time).
    auto iso_date_time = combine_iso_date_and_time_record(iso_date, time);

    // 19. Return ? CreateTemporalDateTime(isoDateTime, calendar, NewTarget).
    return TRY(create_temporal_date_time(vm, iso_date_time, move(calendar), new_target));
}

// 5.2.2 Temporal.PlainDateTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.from
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimeConstructor::from)
{
    // 1. Return ? ToTemporalDateTime(item, options).
    return TRY(to_temporal_date_time(vm, vm.argument(0), vm.argument(1)));
}

// 5.2.3 Temporal.PlainDateTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.compare
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimeConstructor::compare)
{
    // 1. Set one to ? ToTemporalDateTime(one).
    auto one = TRY(to_temporal_date_time(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalDateTime(two).
    auto two = TRY(to_temporal_date_time(vm, vm.argument(1)));

    // 3. Return 𝔽(CompareISODateTime(one.[[ISODateTime]], two.[[ISODateTime]])).
    return compare_iso_date_time(one->iso_date_time(), two->iso_date_time());
}

}
