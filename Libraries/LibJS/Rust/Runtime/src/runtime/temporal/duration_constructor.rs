/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/DurationConstructor.cpp: %Temporal.Duration%.

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{get_options_object, to_integer_if_integral};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::Overflow;
use crate::runtime::temporal::abstract_operations::{
    UnitCategory, get_temporal_relative_to_option, is_calendar_unit, temporal_unit_category,
};
use crate::runtime::temporal::duration::{
    DurationFields, add_24_hour_days_to_time_duration, compare_time_duration, create_temporal_duration,
    date_duration_days, default_temporal_largest_unit, to_internal_duration_record, to_temporal_duration,
};
use crate::runtime::temporal::zoned_date_time::add_zoned_date_time;
use crate::utf16::Utf16View;

// 7.1 The Temporal.Duration Constructor, https://tc39.es/proposal-temporal/#sec-temporal-duration-constructor
#[repr(C)]
#[derive(Trace)]
pub struct DurationConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    DurationConstructor,
    initialize: DurationConstructor::initialize,
    call: DurationConstructor::call,
    construct: DurationConstructor::construct
);

impl DurationConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DurationConstructor> {
        realm.create_object(
            vm,
            DurationConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Duration.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 7.2.1 Temporal.Duration.prototype, https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_duration_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(DurationConstructor::from),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.compare,
            raw_native!(DurationConstructor::compare),
            2,
            attr,
            None,
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 7.1.1 Temporal.Duration ( [ years [ , months [ , weeks [ , days [ , hours [ , minutes [ , seconds [ , milliseconds [ , microseconds [ , nanoseconds ] ] ] ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.Duration"],
        )
    }

    // 7.1.1 Temporal.Duration ( [ years [ , months [ , weeks [ , days [ , hours [ , minutes [ , seconds [ , milliseconds [ , microseconds [ , nanoseconds ] ] ] ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let mut index = 0;
        let mut next_integer_argument = || -> ThrowCompletionOr<f64> {
            let value = vm.argument(index);
            index += 1;
            if !value.is_undefined() {
                return to_integer_if_integral(vm, value, ErrorType::TemporalInvalidDuration, &[]);
            }
            Ok(0.0)
        };

        // 2. If years is undefined, let y be 0; else let y be ? ToIntegerIfIntegral(years).
        let years = next_integer_argument()?;

        // 3. If months is undefined, let mo be 0; else let mo be ? ToIntegerIfIntegral(months).
        let months = next_integer_argument()?;

        // 4. If weeks is undefined, let w be 0; else let w be ? ToIntegerIfIntegral(weeks).
        let weeks = next_integer_argument()?;

        // 5. If days is undefined, let d be 0; else let d be ? ToIntegerIfIntegral(days).
        let days = next_integer_argument()?;

        // 6. If hours is undefined, let h be 0; else let h be ? ToIntegerIfIntegral(hours).
        let hours = next_integer_argument()?;

        // 7. If minutes is undefined, let m be 0; else let m be ? ToIntegerIfIntegral(minutes).
        let minutes = next_integer_argument()?;

        // 8. If seconds is undefined, let s be 0; else let s be ? ToIntegerIfIntegral(seconds).
        let seconds = next_integer_argument()?;

        // 9. If milliseconds is undefined, let ms be 0; else let ms be ? ToIntegerIfIntegral(milliseconds).
        let milliseconds = next_integer_argument()?;

        // 10. If microseconds is undefined, let mis be 0; else let mis be ? ToIntegerIfIntegral(microseconds).
        let microseconds = next_integer_argument()?;

        // 11. If nanoseconds is undefined, let ns be 0; else let ns be ? ToIntegerIfIntegral(nanoseconds).
        let nanoseconds = next_integer_argument()?;

        // 12. Return ? CreateTemporalDuration(y, mo, w, d, h, m, s, ms, mis, ns, NewTarget).
        let duration = create_temporal_duration(
            vm,
            DurationFields {
                years,
                months,
                weeks,
                days,
                hours,
                minutes,
                seconds,
                milliseconds,
                microseconds,
                nanoseconds,
            },
            Some(new_target),
        )?;
        Ok(duration.upcast())
    }

    // 7.2.2 Temporal.Duration.from ( item ), https://tc39.es/proposal-temporal/#sec-temporal.duration.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalDuration(item).
        Ok(Value::from_object(to_temporal_duration(vm, vm.argument(0))?))
    }

    // 7.2.3 Temporal.Duration.compare ( one, two [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalDuration(one).
        let one = to_temporal_duration(vm, vm.argument(0))?;

        // 2. Set two to ? ToTemporalDuration(two).
        let two = to_temporal_duration(vm, vm.argument(1))?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(2))?;

        // 4. Let relativeToRecord be ? GetTemporalRelativeToOption(resolvedOptions).
        let relative_to_record = get_temporal_relative_to_option(vm, &resolved_options)?;

        // 5. If one.[[Years]] = two.[[Years]], and one.[[Months]] = two.[[Months]], and one.[[Weeks]] = two.[[Weeks]], and
        //    one.[[Days]] = two.[[Days]], and one.[[Hours]] = two.[[Hours]], and one.[[Minutes]] = two.[[Minutes]], and
        //    one.[[Seconds]] = two.[[Seconds]], and one.[[Milliseconds]] = two.[[Milliseconds]], and
        //    one.[[Microseconds]] = two.[[Microseconds]], and one.[[Nanoseconds]] = two.[[Nanoseconds]], then
        if one.fields() == two.fields() {
            // a. Return +0𝔽.
            return Ok(Value::from_i32(0));
        }

        // 6. Let zonedRelativeTo be relativeToRecord.[[ZonedRelativeTo]].
        // 7. Let plainRelativeTo be relativeToRecord.[[PlainRelativeTo]].
        let plain_relative_to = relative_to_record.plain_relative_to;
        let zoned_relative_to = relative_to_record.zoned_relative_to;

        // 8. Let largestUnit1 be DefaultTemporalLargestUnit(one).
        let largest_unit1 = default_temporal_largest_unit(&one);

        // 9. Let largestUnit2 be DefaultTemporalLargestUnit(two).
        let largest_unit2 = default_temporal_largest_unit(&two);

        // 10. Let duration1 be ToInternalDurationRecord(one).
        let duration1 = to_internal_duration_record(vm, &one);

        // 11. Let duration2 be ToInternalDurationRecord(two).
        let duration2 = to_internal_duration_record(vm, &two);

        // 12. If zonedRelativeTo is not undefined, and TemporalUnitCategory(largestUnit1) is DATE or TemporalUnitCategory(largestUnit2) is DATE, then
        if let Some(zoned_relative_to) = zoned_relative_to
            && (temporal_unit_category(largest_unit1) == UnitCategory::Date
                || temporal_unit_category(largest_unit2) == UnitCategory::Date)
        {
            // a. Let timeZone be zonedRelativeTo.[[TimeZone]].
            let time_zone = zoned_relative_to.time_zone();

            // b. Let calendar be zonedRelativeTo.[[Calendar]].
            let calendar = zoned_relative_to.calendar();

            // c. Let after1 be ? AddZonedDateTime(zonedRelativeTo.[[EpochNanoseconds]], timeZone, calendar, duration1, CONSTRAIN).
            let epoch_nanoseconds = zoned_relative_to.epoch_nanoseconds().big_integer().clone();
            let after1 = add_zoned_date_time(
                vm,
                &epoch_nanoseconds,
                Utf16View::of_string(&time_zone),
                Utf16View::of_string(&calendar),
                &duration1,
                Overflow::Constrain,
            )?;

            // d. Let after2 be ? AddZonedDateTime(zonedRelativeTo.[[EpochNanoseconds]], timeZone, calendar, duration2, CONSTRAIN).
            let after2 = add_zoned_date_time(
                vm,
                &epoch_nanoseconds,
                Utf16View::of_string(&time_zone),
                Utf16View::of_string(&calendar),
                &duration2,
                Overflow::Constrain,
            )?;

            // e. If after1 > after2, return 1𝔽.
            if after1 > after2 {
                return Ok(Value::from_i32(1));
            }

            // f. If after1 < after2, return -1𝔽.
            if after1 < after2 {
                return Ok(Value::from_i32(-1));
            }

            // g. Return +0𝔽.
            return Ok(Value::from_i32(0));
        }

        // 13. If IsCalendarUnit(largestUnit1) is true or IsCalendarUnit(largestUnit2) is true, then
        let (days1, days2) = if is_calendar_unit(largest_unit1) || is_calendar_unit(largest_unit2) {
            // a. If plainRelativeTo is undefined, throw a RangeError exception.
            let Some(plain_relative_to) = plain_relative_to else {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalMissingStartingPoint,
                    &[&"calendar units"],
                );
            };

            // b. Let days1 be ? DateDurationDays(duration1.[[Date]], plainRelativeTo).
            let days1 = date_duration_days(vm, &duration1.date, &plain_relative_to)?;

            // c. Let days2 be ? DateDurationDays(duration2.[[Date]], plainRelativeTo).
            let days2 = date_duration_days(vm, &duration2.date, &plain_relative_to)?;

            (days1, days2)
        }
        // 14. Else,
        else {
            // a. Let days1 be one.[[Days]].
            // b. Let days2 be two.[[Days]].
            (one.days(), two.days())
        };

        // 15. Let timeDuration1 be ? Add24HourDaysToTimeDuration(duration1.[[Time]], days1).
        let time_duration1 = add_24_hour_days_to_time_duration(vm, &duration1.time, days1)?;

        // 16. Let timeDuration2 be ? Add24HourDaysToTimeDuration(duration2.[[Time]], days2).
        let time_duration2 = add_24_hour_days_to_time_duration(vm, &duration2.time, days2)?;

        // 17. Return 𝔽(CompareTimeDuration(timeDuration1, timeDuration2)).
        Ok(Value::from_i32(i32::from(compare_time_duration(
            &time_duration1,
            &time_duration2,
        ))))
    }
}
