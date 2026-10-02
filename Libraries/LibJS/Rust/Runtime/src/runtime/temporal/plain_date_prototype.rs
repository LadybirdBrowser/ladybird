/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainDatePrototype.cpp: %Temporal.PlainDate.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::get_options_object;
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::date_time_format::{FormattableDateTime, format_date_time};
use crate::runtime::intl::date_time_format_constructor::{OptionDefaults, OptionRequired, create_date_time_format};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DateType, Disambiguation, DurationOperation, Overflow, ShowCalendar,
    get_temporal_overflow_option, get_temporal_show_calendar_name_option, is_partial_temporal_object,
    iso_date_to_fields,
};
use crate::runtime::temporal::calendar::{
    CalendarDate, CalendarField, CalendarFieldListOrPartial, calendar_date_from_fields, calendar_equals,
    calendar_iso_to_date, calendar_merge_fields, calendar_month_day_from_fields, calendar_year_month_from_fields,
    prepare_calendar_fields, to_temporal_calendar_identifier,
};
use crate::runtime::temporal::plain_date::{
    PlainDate, add_duration_to_date, compare_iso_date, create_temporal_date, difference_temporal_plain_date,
    temporal_date_to_string, to_temporal_date,
};
use crate::runtime::temporal::plain_date_time::{
    combine_iso_date_and_time_record, create_temporal_date_time, iso_date_time_within_limits,
};
use crate::runtime::temporal::plain_month_day::create_temporal_month_day;
use crate::runtime::temporal::plain_time::{to_temporal_time, to_time_record_or_midnight};
use crate::runtime::temporal::plain_year_month::create_temporal_year_month;
use crate::runtime::temporal::time_zone::{
    get_epoch_nanoseconds_for, get_start_of_day, to_temporal_time_zone_identifier,
};
use crate::runtime::temporal::zoned_date_time::create_temporal_zoned_date_time;
use crate::utf16::Utf16View;

// 3.3 Properties of the Temporal.PlainDate Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaindate-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct PlainDatePrototype {
    base: Object,
}

define_object_class!(PlainDatePrototype, extends: [Object], methods: {
    initialize: PlainDatePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_plain_date(vm: &Vm) -> ThrowCompletionOr<Gc<PlainDate>> {
    typed_this_object::<PlainDate>(vm, "Temporal.PlainDate")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

/// CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).
fn calendar_date_of(plain_date: &PlainDate) -> CalendarDate {
    let calendar = plain_date.calendar();
    calendar_iso_to_date(Utf16View::of_string(&calendar), plain_date.iso_date())
}

impl PlainDatePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainDatePrototype> {
        realm.create_object(
            vm,
            PlainDatePrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 3.3.2 Temporal.PlainDate.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.PlainDate")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let define_getter = |name: &PropertyKey, getter| {
            object.define_native_accessor(
                vm,
                realm,
                name,
                getter,
                None,
                PropertyAttributes::new(Attribute::CONFIGURABLE),
            );
        };
        define_getter(&names.calendarId, raw_native!(PlainDatePrototype::calendar_id_getter));
        define_getter(&names.era, raw_native!(PlainDatePrototype::era_getter));
        define_getter(&names.eraYear, raw_native!(PlainDatePrototype::era_year_getter));
        define_getter(&names.year, raw_native!(PlainDatePrototype::year_getter));
        define_getter(&names.month, raw_native!(PlainDatePrototype::month_getter));
        define_getter(&names.monthCode, raw_native!(PlainDatePrototype::month_code_getter));
        define_getter(&names.day, raw_native!(PlainDatePrototype::day_getter));
        define_getter(&names.dayOfWeek, raw_native!(PlainDatePrototype::day_of_week_getter));
        define_getter(&names.dayOfYear, raw_native!(PlainDatePrototype::day_of_year_getter));
        define_getter(&names.weekOfYear, raw_native!(PlainDatePrototype::week_of_year_getter));
        define_getter(&names.yearOfWeek, raw_native!(PlainDatePrototype::year_of_week_getter));
        define_getter(&names.daysInWeek, raw_native!(PlainDatePrototype::days_in_week_getter));
        define_getter(
            &names.daysInMonth,
            raw_native!(PlainDatePrototype::days_in_month_getter),
        );
        define_getter(&names.daysInYear, raw_native!(PlainDatePrototype::days_in_year_getter));
        define_getter(
            &names.monthsInYear,
            raw_native!(PlainDatePrototype::months_in_year_getter),
        );
        define_getter(&names.inLeapYear, raw_native!(PlainDatePrototype::in_leap_year_getter));

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(
            &names.toPlainYearMonth,
            raw_native!(PlainDatePrototype::to_plain_year_month),
            0,
        );
        define_native_function(
            &names.toPlainMonthDay,
            raw_native!(PlainDatePrototype::to_plain_month_day),
            0,
        );
        define_native_function(&names.add, raw_native!(PlainDatePrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(PlainDatePrototype::subtract), 1);
        define_native_function(&names.with, raw_native!(PlainDatePrototype::with), 1);
        define_native_function(&names.withCalendar, raw_native!(PlainDatePrototype::with_calendar), 1);
        define_native_function(&names.until, raw_native!(PlainDatePrototype::until), 1);
        define_native_function(&names.since, raw_native!(PlainDatePrototype::since), 1);
        define_native_function(&names.equals, raw_native!(PlainDatePrototype::equals), 1);
        define_native_function(
            &names.toPlainDateTime,
            raw_native!(PlainDatePrototype::to_plain_date_time),
            0,
        );
        define_native_function(
            &names.toZonedDateTime,
            raw_native!(PlainDatePrototype::to_zoned_date_time),
            1,
        );
        define_native_function(&names.toString, raw_native!(PlainDatePrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(PlainDatePrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(PlainDatePrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(PlainDatePrototype::value_of), 0);
    }

    // 3.3.3 get Temporal.PlainDate.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.calendarid
    fn calendar_id_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return plainDate.[[Calendar]].
        Ok(Value::from_string(PrimitiveString::create(vm, plain_date.calendar())))
    }

    // 3.3.4 get Temporal.PlainDate.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.era
    fn era_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[Era]].
        let result = calendar_date_of(&plain_date).era;

        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        Ok(Value::from_string(PrimitiveString::create(vm, result)))
    }

    // 3.3.5 get Temporal.PlainDate.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.erayear
    fn era_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let result be CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[EraYear]].
        let result = calendar_date_of(&plain_date).era_year;

        // 4. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 5. Return 𝔽(result).
        Ok(Value::from_i32(result))
    }

    // 3.3.6 get Temporal.PlainDate.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.year
    fn year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[Year]].
        Ok(Value::from_i32(calendar_date_of(&plain_date).year))
    }

    // 3.3.7 get Temporal.PlainDate.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.month
    fn month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[Month]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).month)))
    }

    // 3.3.8 get Temporal.PlainDate.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.monthcode
    fn month_code_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[MonthCode]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            calendar_date_of(&plain_date).month_code,
        )))
    }

    // 3.3.9 get Temporal.PlainDate.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.day
    fn day_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[Day]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).day)))
    }

    // 3.3.10 get Temporal.PlainDate.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.dayofweek
    fn day_of_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[DayOfWeek]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).day_of_week)))
    }

    // 3.3.11 get Temporal.PlainDate.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.dayofyear
    fn day_of_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[DayOfYear]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).day_of_year)))
    }

    // 3.3.12 get Temporal.PlainDate.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.weekofyear
    fn week_of_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let result be CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[WeekOfYear]].[[Week]].
        let result = calendar_date_of(&plain_date).week_of_year.week;

        // 4. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 5. Return 𝔽(result).
        Ok(Value::from_i32(i32::from(result)))
    }

    // 3.3.13 get Temporal.PlainDate.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.yearofweek
    fn year_of_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let result be CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[WeekOfYear]].[[Year]].
        let result = calendar_date_of(&plain_date).week_of_year.year;

        // 4. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 5. Return 𝔽(result).
        Ok(Value::from_i32(result))
    }

    // 3.3.14 get Temporal.PlainDate.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinweek
    fn days_in_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[DaysInWeek]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).days_in_week)))
    }

    // 3.3.15 get Temporal.PlainDate.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinmonth
    fn days_in_month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[DaysInMonth]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).days_in_month)))
    }

    // 3.3.16 get Temporal.PlainDate.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinyear
    fn days_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[DaysInYear]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).days_in_year)))
    }

    // 3.3.17 get Temporal.PlainDate.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.monthsinyear
    fn months_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[MonthsInYear]].
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date).months_in_year)))
    }

    // 3.3.18 get Temporal.PlainDate.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.inleapyear
    fn in_leap_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[InLeapYear]].
        Ok(Value::from_bool(calendar_date_of(&plain_date).in_leap_year))
    }

    // 3.3.19 Temporal.PlainDate.prototype.toPlainYearMonth ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplainyearmonth
    fn to_plain_year_month(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let calendar be plainDate.[[Calendar]].
        let calendar = plain_date.calendar();

        // 4. Let fields be ISODateToFields(calendar, plainDate.[[ISODate]], DATE).
        let mut fields = iso_date_to_fields(Utf16View::of_string(&calendar), plain_date.iso_date(), DateType::Date);

        // 5. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, CONSTRAIN).
        let iso_date =
            calendar_year_month_from_fields(vm, Utf16View::of_string(&calendar), &mut fields, Overflow::Constrain)?;

        // 6. Return ! CreateTemporalYearMonth(isoDate, calendar).
        Ok(Value::from_object(
            create_temporal_year_month(vm, iso_date, calendar, None).must(),
        ))
    }

    // 3.3.20 Temporal.PlainDate.prototype.toPlainMonthDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplainmonthday
    fn to_plain_month_day(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let calendar be plainDate.[[Calendar]].
        let calendar = plain_date.calendar();

        // 4. Let fields be ISODateToFields(calendar, plainDate.[[ISODate]], DATE).
        let mut fields = iso_date_to_fields(Utf16View::of_string(&calendar), plain_date.iso_date(), DateType::Date);

        // 5. Let isoDate be ? CalendarMonthDayFromFields(calendar, fields, CONSTRAIN).
        let iso_date =
            calendar_month_day_from_fields(vm, Utf16View::of_string(&calendar), &mut fields, Overflow::Constrain)?;

        // 6. Return ! CreateTemporalMonthDay(isoDate, calendar).
        Ok(Value::from_object(
            create_temporal_month_day(vm, iso_date, calendar, None).must(),
        ))
    }

    // 3.3.21 Temporal.PlainDate.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return ? AddDurationToDate(ADD, plainDate, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_date(
            vm,
            ArithmeticOperation::Add,
            &plain_date,
            temporal_duration_like,
            options,
        )?))
    }

    // 3.3.22 Temporal.PlainDate.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.subtract
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDate be the this value.
        let plain_date = typed_this_plain_date(vm)?;

        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        // 3. Return ? AddDurationToDate(SUBTRACT, plainDate, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_date(
            vm,
            ArithmeticOperation::Subtract,
            &plain_date,
            temporal_duration_like,
            options,
        )?))
    }

    // 3.3.23 Temporal.PlainDate.prototype.with ( temporalDateLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_date_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. If ? IsPartialTemporalObject(temporalDateLike) is false, throw a TypeError exception.
        if !is_partial_temporal_object(vm, temporal_date_like)? {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TemporalObjectMustBePartialTemporalObject,
                &[],
            );
        }

        // 4. Let calendar be plainDate.[[Calendar]].
        let calendar = plain_date.calendar();

        // 5. Let fields be ISODateToFields(calendar, plainDate.[[ISODate]], DATE).
        let fields = iso_date_to_fields(Utf16View::of_string(&calendar), plain_date.iso_date(), DateType::Date);

        // 6. Let partialDate be ? PrepareCalendarFields(calendar, temporalDateLike, « YEAR, MONTH, MONTH-CODE, DAY », « », PARTIAL).
        let partial_date = prepare_calendar_fields(
            vm,
            Utf16View::of_string(&calendar),
            &temporal_date_like.as_object(),
            &[
                CalendarField::Year,
                CalendarField::Month,
                CalendarField::MonthCode,
                CalendarField::Day,
            ],
            &[],
            CalendarFieldListOrPartial::Partial,
        )?;

        // 7. Set fields to CalendarMergeFields(calendar, fields, partialDate).
        let mut fields = calendar_merge_fields(Utf16View::of_string(&calendar), &fields, &partial_date);

        // 8. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 9. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // 10. Let isoDate be ? CalendarDateFromFields(calendar, fields, overflow).
        let iso_date = calendar_date_from_fields(vm, Utf16View::of_string(&calendar), &mut fields, overflow)?;

        // 11. Return ! CreateTemporalDate(isoDate, calendar).
        Ok(Value::from_object(
            create_temporal_date(vm, iso_date, calendar, None).must(),
        ))
    }

    // 3.3.24 Temporal.PlainDate.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.with
    fn with_calendar(vm: &Vm) -> ThrowCompletionOr<Value> {
        let calendar_like = vm.argument(0);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike).
        let calendar = to_temporal_calendar_identifier(vm, calendar_like)?;

        // 4. Return ! CreateTemporalDate(plainDate.[[ISODate]], calendar).
        Ok(Value::from_object(
            create_temporal_date(vm, plain_date.iso_date(), calendar, None).must(),
        ))
    }

    // 3.3.25 Temporal.PlainDate.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.until
    fn until(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return ? DifferenceTemporalPlainDate(UNTIL, plainDate, other, options).
        Ok(Value::from_object(difference_temporal_plain_date(
            vm,
            DurationOperation::Until,
            &plain_date,
            other,
            options,
        )?))
    }

    // 3.3.26 Temporal.PlainDate.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.since
    fn since(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return ? DifferenceTemporalPlainDate(SINCE, plainDate, other, options).
        Ok(Value::from_object(difference_temporal_plain_date(
            vm,
            DurationOperation::Since,
            &plain_date,
            other,
            options,
        )?))
    }

    // 3.3.27 Temporal.PlainDate.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Set other to ? ToTemporalDate(other).
        let other = to_temporal_date(vm, vm.argument(0), Value::UNDEFINED)?;

        // 4. If CompareISODate(plainDate.[[ISODate]], other.[[ISODate]]) ≠ 0, return false.
        if compare_iso_date(plain_date.iso_date(), other.iso_date()) != 0 {
            return Ok(Value::from_bool(false));
        }

        // 5. Return CalendarEquals(plainDate.[[Calendar]], other.[[Calendar]]).
        Ok(Value::from_bool(calendar_equals(
            Utf16View::of_string(&plain_date.calendar()),
            Utf16View::of_string(&other.calendar()),
        )))
    }

    // 3.3.28 Temporal.PlainDate.prototype.toPlainDateTime ( [ temporalTime ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplaindatetime
    fn to_plain_date_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time = vm.argument(0);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let time be ? ToTimeRecordOrMidnight(temporalTime).
        let time = to_time_record_or_midnight(vm, temporal_time)?;

        // 4. Let isoDateTime be CombineISODateAndTimeRecord(plainDate.[[ISODate]], time).
        let iso_date_time = combine_iso_date_and_time_record(plain_date.iso_date(), time);

        // 5. Return ? CreateTemporalDateTime(isoDateTime, plainDate.[[Calendar]]).
        Ok(Value::from_object(create_temporal_date_time(
            vm,
            &iso_date_time,
            plain_date.calendar(),
            None,
        )?))
    }

    // 3.3.29 Temporal.PlainDate.prototype.toZonedDateTime ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tozoneddatetime
    fn to_zoned_date_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        let item = vm.argument(0);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        let time_zone: Utf16String;
        let temporal_time: Value;

        // 3. If item is an Object, then
        if item.is_object() {
            let object = item.as_object();

            // a. Let timeZoneLike be ? Get(item, "timeZone").
            let time_zone_like = object.get(vm, &vm.names.timeZone)?;

            // b. If timeZoneLike is undefined, then
            if time_zone_like.is_undefined() {
                // i. Let timeZone be ? ToTemporalTimeZoneIdentifier(item).
                time_zone = to_temporal_time_zone_identifier(vm, item)?;

                // ii. Let temporalTime be undefined.
                temporal_time = Value::UNDEFINED;
            }
            // c. Else,
            else {
                // i. Let timeZone be ? ToTemporalTimeZoneIdentifier(timeZoneLike).
                time_zone = to_temporal_time_zone_identifier(vm, time_zone_like)?;

                // ii. Let temporalTime be ? Get(item, "plainTime").
                temporal_time = object.get(vm, &vm.names.plainTime)?;
            }
        }
        // 4. Else,
        else {
            // a. Let timeZone be ? ToTemporalTimeZoneIdentifier(item).
            time_zone = to_temporal_time_zone_identifier(vm, item)?;

            // b. Let temporalTime be undefined.
            temporal_time = Value::UNDEFINED;
        }

        // 5. If temporalTime is undefined, then
        let epoch_nanoseconds = if temporal_time.is_undefined() {
            // a. Let epochNs be ? GetStartOfDay(timeZone, plainDate.[[ISODate]]).
            get_start_of_day(vm, Utf16View::of_string(&time_zone), plain_date.iso_date())?
        }
        // 6. Else,
        else {
            // a. Set temporalTime to ? ToTemporalTime(temporalTime).
            let plain_temporal_time = to_temporal_time(vm, temporal_time, Value::UNDEFINED)?;

            // b. Let isoDateTime be CombineISODateAndTimeRecord(plainDate.[[ISODate]], temporalTime.[[Time]]).
            let iso_date_time = combine_iso_date_and_time_record(plain_date.iso_date(), plain_temporal_time.time());

            // c. If ISODateTimeWithinLimits(isoDateTime) is false, throw a RangeError exception.
            if !iso_date_time_within_limits(&iso_date_time) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODateTime, &[]);
            }

            // d. Let epochNs be ? GetEpochNanosecondsFor(timeZone, isoDateTime, COMPATIBLE).
            get_epoch_nanoseconds_for(
                vm,
                Utf16View::of_string(&time_zone),
                &iso_date_time,
                Disambiguation::Compatible,
            )?
        };

        // 7. Return ! CreateTemporalZonedDateTime(epochNs, timeZone, plainDate.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                BigInt::create(vm, epoch_nanoseconds),
                time_zone,
                plain_date.calendar(),
                None,
            )
            .must(),
        ))
    }

    // 3.3.30 Temporal.PlainDate.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
        let show_calendar = get_temporal_show_calendar_name_option(vm, &resolved_options)?;

        // 5. Return TemporalDateToString(plainDate, showCalendar).
        Ok(string_value(vm, &temporal_date_to_string(&plain_date, show_calendar)))
    }

    // 3.3.31 Temporal.PlainDate.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tolocalestring
    // 15.11.3.1 Temporal.PlainDate.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plaindate.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, DATE, DATE).
        let date_format = create_date_time_format(
            vm,
            realm.intrinsics().intl_date_time_format_constructor(vm),
            locales,
            options,
            OptionRequired::Date,
            OptionDefaults::Date,
            None,
        )?;

        // 4. Return ? FormatDateTime(dateFormat, plainDate).
        let formatted = format_date_time(vm, &date_format, &FormattableDateTime::PlainDate(plain_date))?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 3.3.32 Temporal.PlainDate.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDate be the this value.
        // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
        let plain_date = typed_this_plain_date(vm)?;

        // 3. Return TemporalDateToString(plainDate, AUTO).
        Ok(string_value(
            vm,
            &temporal_date_to_string(&plain_date, ShowCalendar::Auto),
        ))
    }

    // 3.3.33 Temporal.PlainDate.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.PlainDate", &"a primitive value"],
        )
    }
}
