/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainDateTimeConstructor.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainDateTimeConstructor);

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

// 5.1.1 Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond [ , calendarLike ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime
ThrowCompletionOr<Value> PlainDateTimeConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainDateTime");
}

// 5.1.1 Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond [ , calendarLike ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime
ThrowCompletionOr<NonnullGCPtr<Object>> PlainDateTimeConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let isoYear be ? ToIntegerWithTruncation(isoYear).
    auto iso_year = TRY(to_integer_with_truncation(vm, vm.argument(0), ErrorType::TemporalInvalidPlainDateTime));

    // 3. Let isoMonth be ? ToIntegerWithTruncation(isoMonth).
    auto iso_month = TRY(to_integer_with_truncation(vm, vm.argument(1), ErrorType::TemporalInvalidPlainDateTime));

    // 4. Let isoDay be ? ToIntegerWithTruncation(isoDay).
    auto iso_day = TRY(to_integer_with_truncation(vm, vm.argument(2), ErrorType::TemporalInvalidPlainDateTime));

    // 5. Let hour be ? ToIntegerWithTruncation(hour).
    auto hour = TRY(to_integer_with_truncation(vm, vm.argument(3), ErrorType::TemporalInvalidPlainDateTime));

    // 6. Let minute be ? ToIntegerWithTruncation(minute).
    auto minute = TRY(to_integer_with_truncation(vm, vm.argument(4), ErrorType::TemporalInvalidPlainDateTime));

    // 7. Let second be ? ToIntegerWithTruncation(second).
    auto second = TRY(to_integer_with_truncation(vm, vm.argument(5), ErrorType::TemporalInvalidPlainDateTime));

    // 8. Let millisecond be ? ToIntegerWithTruncation(millisecond).
    auto millisecond = TRY(to_integer_with_truncation(vm, vm.argument(6), ErrorType::TemporalInvalidPlainDateTime));

    // 9. Let microsecond be ? ToIntegerWithTruncation(microsecond).
    auto microsecond = TRY(to_integer_with_truncation(vm, vm.argument(7), ErrorType::TemporalInvalidPlainDateTime));

    // 10. Let nanosecond be ? ToIntegerWithTruncation(nanosecond).
    auto nanosecond = TRY(to_integer_with_truncation(vm, vm.argument(8), ErrorType::TemporalInvalidPlainDateTime));

    // 11. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
    auto* calendar = TRY(to_temporal_calendar_with_iso_default(vm, vm.argument(9)));

    // IMPLEMENTATION DEFINED: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
    // This does not change the exposed behavior as the call to CreateTemporalDateTime will immediately check that these values are valid
    // ISO values (for years: -273975 - 273975, for months: 1 - 12, for days: 1 - 31, for hours: 0 - 23, for minutes and seconds: 0 - 59,
    // milliseconds, microseconds, and nanoseconds: 0 - 999) all of which are subsets of this check.
    if (!AK::is_within_range<i32>(iso_year) || !AK::is_within_range<u8>(iso_month) || !AK::is_within_range<u8>(iso_day) || !AK::is_within_range<u8>(hour) || !AK::is_within_range<u8>(minute) || !AK::is_within_range<u8>(second) || !AK::is_within_range<u16>(millisecond) || !AK::is_within_range<u16>(microsecond) || !AK::is_within_range<u16>(nanosecond))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 12. Return ? CreateTemporalDateTime(isoYear, isoMonth, isoDay, hour, minute, second, millisecond, microsecond, nanosecond, calendar, NewTarget).
    return *TRY(create_temporal_date_time(vm, iso_year, iso_month, iso_day, hour, minute, second, millisecond, microsecond, nanosecond, *calendar, &new_target));
}

// 5.2.2 Temporal.PlainDateTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.from
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimeConstructor::from)
{
    auto item = vm.argument(0);

    // 1. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 2. If Type(item) is Object and item has an [[InitializedTemporalDateTime]] internal slot, then
    if (item.is_object() && is<PlainDateTime>(item.as_object())) {
        auto& plain_date_time = static_cast<PlainDateTime&>(item.as_object());

        // a. Perform ? ToTemporalOverflow(options).
        (void)TRY(to_temporal_overflow(vm, options));

        // b. Return ! CreateTemporalDateTime(item.[[ISOYear]], item.[[ISOMonth]], item.[[ISODay]], item.[[ISOHour]], item.[[ISOMinute]], item.[[ISOSecond]], item.[[ISOMillisecond]], item.[[ISOMicrosecond]], item.[[ISONanosecond]], item.[[Calendar]]).
        return MUST(create_temporal_date_time(vm, plain_date_time.iso_year(), plain_date_time.iso_month(), plain_date_time.iso_day(), plain_date_time.iso_hour(), plain_date_time.iso_minute(), plain_date_time.iso_second(), plain_date_time.iso_millisecond(), plain_date_time.iso_microsecond(), plain_date_time.iso_nanosecond(), plain_date_time.calendar()));
    }

    // 3. Return ? ToTemporalDateTime(item, options).
    return TRY(to_temporal_date_time(vm, item, options));
}

// 5.2.3 Temporal.PlainDateTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.compare
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimeConstructor::compare)
{
    // 1. Set one to ? ToTemporalDateTime(one).
    auto* one = TRY(to_temporal_date_time(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalDateTime(two).
    auto* two = TRY(to_temporal_date_time(vm, vm.argument(1)));

    // 3. Return 𝔽(! CompareISODateTime(one.[[ISOYear]], one.[[ISOMonth]], one.[[ISODay]], one.[[ISOHour]], one.[[ISOMinute]], one.[[ISOSecond]], one.[[ISOMillisecond]], one.[[ISOMicrosecond]], one.[[ISONanosecond]], two.[[ISOYear]], two.[[ISOMonth]], two.[[ISODay]], two.[[ISOHour]], two.[[ISOMinute]], two.[[ISOSecond]], two.[[ISOMillisecond]], two.[[ISOMicrosecond]], two.[[ISONanosecond]])).
    return Value(compare_iso_date_time(one->iso_year(), one->iso_month(), one->iso_day(), one->iso_hour(), one->iso_minute(), one->iso_second(), one->iso_millisecond(), one->iso_microsecond(), one->iso_nanosecond(), two->iso_year(), two->iso_month(), two->iso_day(), two->iso_hour(), two->iso_minute(), two->iso_second(), two->iso_millisecond(), two->iso_microsecond(), two->iso_nanosecond()));
}

}
