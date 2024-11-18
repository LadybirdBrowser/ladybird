/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/DurationConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(DurationConstructor);

// 7.1 The Temporal.Duration Constructor, https://tc39.es/proposal-temporal/#sec-temporal-duration-constructor
DurationConstructor::DurationConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Duration.as_string(), realm.intrinsics().function_prototype())
{
}

void DurationConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 7.2.1 Temporal.Duration.prototype, https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_duration_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 7.1.1 Temporal.Duration ( [ years [ , months [ , weeks [ , days [ , hours [ , minutes [ , seconds [ , milliseconds [ , microseconds [ , nanoseconds ] ] ] ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration
ThrowCompletionOr<Value> DurationConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, then
    //     a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.Duration");
}

// 7.1.1 Temporal.Duration ( [ years [ , months [ , weeks [ , days [ , hours [ , minutes [ , seconds [ , milliseconds [ , microseconds [ , nanoseconds ] ] ] ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration
ThrowCompletionOr<GC::Ref<Object>> DurationConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto next_integer_argument = [&, index = 0]() mutable -> ThrowCompletionOr<double> {
        if (auto value = vm.argument(index++); !value.is_undefined())
            return to_integer_if_integral(vm, value, ErrorType::TemporalInvalidDuration);
        return 0;
    };

    // 2. If years is undefined, let y be 0; else let y be ? ToIntegerIfIntegral(years).
    auto years = TRY(next_integer_argument());

    // 3. If months is undefined, let mo be 0; else let mo be ? ToIntegerIfIntegral(months).
    auto months = TRY(next_integer_argument());

    // 4. If weeks is undefined, let w be 0; else let w be ? ToIntegerIfIntegral(weeks).
    auto weeks = TRY(next_integer_argument());

    // 5. If days is undefined, let d be 0; else let d be ? ToIntegerIfIntegral(days).
    auto days = TRY(next_integer_argument());

    // 6. If hours is undefined, let h be 0; else let h be ? ToIntegerIfIntegral(hours).
    auto hours = TRY(next_integer_argument());

    // 7. If minutes is undefined, let m be 0; else let m be ? ToIntegerIfIntegral(minutes).
    auto minutes = TRY(next_integer_argument());

    // 8. If seconds is undefined, let s be 0; else let s be ? ToIntegerIfIntegral(seconds).
    auto seconds = TRY(next_integer_argument());

    // 9. If milliseconds is undefined, let ms be 0; else let ms be ? ToIntegerIfIntegral(milliseconds).
    auto milliseconds = TRY(next_integer_argument());

    // 10. If microseconds is undefined, let mis be 0; else let mis be ? ToIntegerIfIntegral(microseconds).
    auto microseconds = TRY(next_integer_argument());

    // 11. If nanoseconds is undefined, let ns be 0; else let ns be ? ToIntegerIfIntegral(nanoseconds).
    auto nanoseconds = TRY(next_integer_argument());

    // 12. Return ? CreateTemporalDuration(y, mo, w, d, h, m, s, ms, mis, ns, NewTarget).
    return TRY(create_temporal_duration(vm, years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, new_target));
}

// 7.2.2 Temporal.Duration.from ( item ), https://tc39.es/proposal-temporal/#sec-temporal.duration.from
JS_DEFINE_NATIVE_FUNCTION(DurationConstructor::from)
{
    // 1. Return ? ToTemporalDuration(item).
    return TRY(to_temporal_duration(vm, vm.argument(0)));
}

// 7.2.3 Temporal.Duration.compare ( one, two [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration.compare
JS_DEFINE_NATIVE_FUNCTION(DurationConstructor::compare)
{
    // 1. Set one to ? ToTemporalDuration(one).
    auto one = TRY(to_temporal_duration(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalDuration(two).
    auto two = TRY(to_temporal_duration(vm, vm.argument(1)));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(2)));

    // 4. Let relativeToRecord be ? GetTemporalRelativeToOption(resolvedOptions).
    auto relative_to_record = TRY(get_temporal_relative_to_option(vm, resolved_options));

    // 5. If one.[[Years]] = two.[[Years]], and one.[[Months]] = two.[[Months]], and one.[[Weeks]] = two.[[Weeks]], and
    //    one.[[Days]] = two.[[Days]], and one.[[Hours]] = two.[[Hours]], and one.[[Minutes]] = two.[[Minutes]], and
    //    one.[[Seconds]] = two.[[Seconds]], and one.[[Milliseconds]] = two.[[Milliseconds]], and
    //    one.[[Microseconds]] = two.[[Microseconds]], and one.[[Nanoseconds]] = two.[[Nanoseconds]], then
    if (one->years() == two->years()
        && one->months() == two->months()
        && one->weeks() == two->weeks()
        && one->days() == two->days()
        && one->hours() == two->hours()
        && one->minutes() == two->minutes()
        && one->seconds() == two->seconds()
        && one->milliseconds() == two->milliseconds()
        && one->microseconds() == two->microseconds()
        && one->nanoseconds() == two->nanoseconds()) {
        // a. Return +0𝔽.
        return 0;
    }

    // 6. Let zonedRelativeTo be relativeToRecord.[[ZonedRelativeTo]].
    // 7. Let plainRelativeTo be relativeToRecord.[[PlainRelativeTo]].
    auto [zoned_relative_to, plain_relative_to] = relative_to_record;

    // 8. Let largestUnit1 be DefaultTemporalLargestUnit(one).
    auto largest_unit1 = default_temporal_largest_unit(one);

    // 9. Let largestUnit2 be DefaultTemporalLargestUnit(two).
    auto largest_unit2 = default_temporal_largest_unit(two);

    // 10. Let duration1 be ToInternalDurationRecord(one).
    auto duration1 = to_internal_duration_record(vm, one);

    // 11. Let duration2 be ToInternalDurationRecord(two).
    auto duration2 = to_internal_duration_record(vm, two);

    // 12. If zonedRelativeTo is not undefined, and either TemporalUnitCategory(largestUnit1) or TemporalUnitCategory(largestUnit2) is date, then
    if (zoned_relative_to && (temporal_unit_category(largest_unit1) == UnitCategory::Date || temporal_unit_category(largest_unit2) == UnitCategory::Date)) {
        // FIXME: a. Let timeZone be zonedRelativeTo.[[TimeZone]].
        // FIXME: b. Let calendar be zonedRelativeTo.[[Calendar]].
        // FIXME: c. Let after1 be ? AddZonedDateTime(zonedRelativeTo.[[EpochNanoseconds]], timeZone, calendar, duration1, constrain).
        // FIXME: d. Let after2 be ? AddZonedDateTime(zonedRelativeTo.[[EpochNanoseconds]], timeZone, calendar, duration2, constrain).
        // FIXME: e. If after1 > after2, return 1𝔽.
        // FIXME: f. If after1 < after2, return -1𝔽.

        // g. Return +0𝔽.
        return 0;
    }

    double days1 = 0;
    double days2 = 0;

    // 13. If IsCalendarUnit(largestUnit1) is true or IsCalendarUnit(largestUnit2) is true, then
    if (is_calendar_unit(largest_unit1) || is_calendar_unit(largest_unit2)) {
        // a. If plainRelativeTo is undefined, throw a RangeError exception.
        if (!plain_relative_to)
            return vm.throw_completion<RangeError>(ErrorType::TemporalMissingStartingPoint, "calendar units");

        // FIXME: b. Let days1 be ? DateDurationDays(duration1.[[Date]], plainRelativeTo).
        // FIXME: c. Let days2 be ? DateDurationDays(duration2.[[Date]], plainRelativeTo).
    }
    // 14. Else,
    else {
        // a. Let days1 be one.[[Days]].
        days1 = one->days();

        // b. Let days2 be two.[[Days]].
        days2 = two->days();
    }

    // 15. Let timeDuration1 be ? Add24HourDaysToTimeDuration(duration1.[[Time]], days1).
    auto time_duration1 = TRY(add_24_hour_days_to_time_duration(vm, duration1.time, days1));

    // 16. Let timeDuration2 be ? Add24HourDaysToTimeDuration(duration2.[[Time]], days2).
    auto time_duration2 = TRY(add_24_hour_days_to_time_duration(vm, duration2.time, days2));

    // 17. Return 𝔽(CompareTimeDuration(timeDuration1, timeDuration2)).
    return compare_time_duration(time_duration1, time_duration2);
}

}
