/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainYearMonthPrototype.cpp: %Temporal.PlainYearMonth.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::get_options_object;
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
    ArithmeticOperation, DateType, DurationOperation, Overflow, ShowCalendar, get_temporal_overflow_option,
    get_temporal_show_calendar_name_option, is_partial_temporal_object, iso_date_to_fields,
};
use crate::runtime::temporal::calendar::{
    CalendarDate, CalendarField, CalendarFieldListOrPartial, calendar_date_from_fields, calendar_equals,
    calendar_iso_to_date, calendar_merge_fields, calendar_year_month_from_fields, prepare_calendar_fields,
};
use crate::runtime::temporal::plain_date::{compare_iso_date, create_temporal_date};
use crate::runtime::temporal::plain_year_month::{
    PlainYearMonth, add_duration_to_year_month, create_temporal_year_month, difference_temporal_plain_year_month,
    temporal_year_month_to_string, to_temporal_year_month,
};
use crate::utf16::Utf16View;

// 9.3 Properties of the Temporal.PlainYearMonth Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plainyearmonth-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct PlainYearMonthPrototype {
    base: Object,
}

define_object_class!(PlainYearMonthPrototype, extends: [Object], methods: {
    initialize: PlainYearMonthPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_plain_year_month(vm: &Vm) -> ThrowCompletionOr<Gc<PlainYearMonth>> {
    typed_this_object::<PlainYearMonth>(vm, "Temporal.PlainYearMonth")
}

/// CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]) of the this value.
fn calendar_date_of_this_plain_year_month(vm: &Vm) -> ThrowCompletionOr<CalendarDate> {
    // 1. Let plainYearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
    let plain_year_month = typed_this_plain_year_month(vm)?;

    Ok(calendar_iso_to_date(
        Utf16View::of_string(&plain_year_month.calendar()),
        plain_year_month.iso_date(),
    ))
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

impl PlainYearMonthPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainYearMonthPrototype> {
        realm.create_object(
            vm,
            PlainYearMonthPrototype {
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

        // 9.3.2 Temporal.PlainYearMonth.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.PlainYearMonth")),
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
        define_getter(
            &names.calendarId,
            raw_native!(PlainYearMonthPrototype::calendar_id_getter),
        );
        define_getter(&names.era, raw_native!(PlainYearMonthPrototype::era_getter));
        define_getter(&names.eraYear, raw_native!(PlainYearMonthPrototype::era_year_getter));
        define_getter(&names.year, raw_native!(PlainYearMonthPrototype::year_getter));
        define_getter(&names.month, raw_native!(PlainYearMonthPrototype::month_getter));
        define_getter(
            &names.monthCode,
            raw_native!(PlainYearMonthPrototype::month_code_getter),
        );
        define_getter(
            &names.daysInYear,
            raw_native!(PlainYearMonthPrototype::days_in_year_getter),
        );
        define_getter(
            &names.daysInMonth,
            raw_native!(PlainYearMonthPrototype::days_in_month_getter),
        );
        define_getter(
            &names.monthsInYear,
            raw_native!(PlainYearMonthPrototype::months_in_year_getter),
        );
        define_getter(
            &names.inLeapYear,
            raw_native!(PlainYearMonthPrototype::in_leap_year_getter),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.with, raw_native!(PlainYearMonthPrototype::with), 1);
        define_native_function(&names.add, raw_native!(PlainYearMonthPrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(PlainYearMonthPrototype::subtract), 1);
        define_native_function(&names.until, raw_native!(PlainYearMonthPrototype::until), 1);
        define_native_function(&names.since, raw_native!(PlainYearMonthPrototype::since), 1);
        define_native_function(&names.equals, raw_native!(PlainYearMonthPrototype::equals), 1);
        define_native_function(&names.toString, raw_native!(PlainYearMonthPrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(PlainYearMonthPrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(PlainYearMonthPrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(PlainYearMonthPrototype::value_of), 0);
        define_native_function(
            &names.toPlainDate,
            raw_native!(PlainYearMonthPrototype::to_plain_date),
            1,
        );
    }

    // 9.3.3 get Temporal.PlainYearMonth.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.calendarid
    fn calendar_id_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Return plainYearMonth.[[Calendar]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            plain_year_month.calendar(),
        )))
    }

    // 9.3.4 get Temporal.PlainYearMonth.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.era
    fn era_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[Era]].
        let result = calendar_date_of_this_plain_year_month(vm)?.era;

        Ok(match result {
            Some(era) => Value::from_string(PrimitiveString::create(vm, era)),
            None => Value::UNDEFINED,
        })
    }

    // 9.3.5 get Temporal.PlainYearMonth.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.erayear
    fn era_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Let result be CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[EraYear]].
        let result = calendar_date_of_this_plain_year_month(vm)?.era_year;

        // 4. If result is undefined, return undefined.
        // 5. Return 𝔽(result).
        Ok(result.map_or(Value::UNDEFINED, Value::from_i32))
    }

    // 9.3.6 get Temporal.PlainYearMonth.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.year
    fn year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[Year]].
        Ok(Value::from_i32(calendar_date_of_this_plain_year_month(vm)?.year))
    }

    // 9.3.7 get Temporal.PlainYearMonth.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.month
    fn month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[Month]].
        Ok(Value::from_i32(i32::from(
            calendar_date_of_this_plain_year_month(vm)?.month,
        )))
    }

    // 9.3.8 get Temporal.PlainYearMonth.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.monthcode
    fn month_code_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[MonthCode]].
        let month_code = calendar_date_of_this_plain_year_month(vm)?.month_code;
        Ok(Value::from_string(PrimitiveString::create(vm, month_code)))
    }

    // 9.3.9 get Temporal.PlainYearMonth.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.daysinyear
    fn days_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[DaysInYear]].
        Ok(Value::from_i32(i32::from(
            calendar_date_of_this_plain_year_month(vm)?.days_in_year,
        )))
    }

    // 9.3.10 get Temporal.PlainYearMonth.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.daysinmonth
    fn days_in_month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[DaysInMonth]].
        Ok(Value::from_i32(i32::from(
            calendar_date_of_this_plain_year_month(vm)?.days_in_month,
        )))
    }

    // 9.3.11 get Temporal.PlainYearMonth.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.monthsinyear
    fn months_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[MonthsInYear]].
        Ok(Value::from_i32(i32::from(
            calendar_date_of_this_plain_year_month(vm)?.months_in_year,
        )))
    }

    // 9.3.12 get Temporal.PlainYearMonth.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.inleapyear
    fn in_leap_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[InLeapYear]].
        Ok(Value::from_bool(
            calendar_date_of_this_plain_year_month(vm)?.in_leap_year,
        ))
    }

    // 9.3.13 Temporal.PlainYearMonth.prototype.with ( temporalYearMonthLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_year_month_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. If ? IsPartialTemporalObject(temporalYearMonthLike) is false, throw a TypeError exception.
        if !is_partial_temporal_object(vm, temporal_year_month_like)? {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TemporalObjectMustBePartialTemporalObject,
                &[],
            );
        }

        // 4. Let calendar be plainYearMonth.[[Calendar]].
        let calendar_string = plain_year_month.calendar();
        let calendar = Utf16View::of_string(&calendar_string);

        // 5. Let fields be ISODateToFields(calendar, plainYearMonth.[[ISODate]], YEAR-MONTH).
        let fields = iso_date_to_fields(calendar, plain_year_month.iso_date(), DateType::YearMonth);

        // 6. Let partialYearMonth be ? PrepareCalendarFields(calendar, temporalYearMonthLike, « YEAR, MONTH, MONTH-CODE », « », PARTIAL).
        let partial_year_month = prepare_calendar_fields(
            vm,
            calendar,
            &temporal_year_month_like.as_object(),
            &[CalendarField::Year, CalendarField::Month, CalendarField::MonthCode],
            &[],
            CalendarFieldListOrPartial::Partial,
        )?;

        // 7. Set fields to CalendarMergeFields(calendar, fields, partialYearMonth).
        let mut fields = calendar_merge_fields(calendar, &fields, &partial_year_month);

        // 8. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 9. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // 10. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, overflow).
        let iso_date = calendar_year_month_from_fields(vm, calendar, &mut fields, overflow)?;

        // 11. Return ! CreateTemporalYearMonth(isoDate, calendar).
        Ok(Value::from_object(
            create_temporal_year_month(vm, iso_date, calendar_string, None).must(),
        ))
    }

    // 9.3.14 Temporal.PlainYearMonth.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Return ? AddDurationToYearMonth(ADD, plainYearMonth, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_year_month(
            vm,
            ArithmeticOperation::Add,
            &plain_year_month,
            temporal_duration_like,
            options,
        )?))
    }

    // 9.3.15 Temporal.PlainYearMonth.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.subtract
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Return ? AddDurationToYearMonth(SUBTRACT, plainYearMonth, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_year_month(
            vm,
            ArithmeticOperation::Subtract,
            &plain_year_month,
            temporal_duration_like,
            options,
        )?))
    }

    // 9.3.16 Temporal.PlainYearMonth.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.until
    fn until(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Return ? DifferenceTemporalPlainYearMonth(UNTIL, plainYearMonth, other, options).
        Ok(Value::from_object(difference_temporal_plain_year_month(
            vm,
            DurationOperation::Until,
            &plain_year_month,
            other,
            options,
        )?))
    }

    // 9.3.17 Temporal.PlainYearMonth.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.since
    fn since(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Return ? DifferenceTemporalPlainYearMonth(SINCE, plainYearMonth, other, options).
        Ok(Value::from_object(difference_temporal_plain_year_month(
            vm,
            DurationOperation::Since,
            &plain_year_month,
            other,
            options,
        )?))
    }

    // 9.3.18 Temporal.PlainYearMonth.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Set other to ? ToTemporalYearMonth(other).
        let other = to_temporal_year_month(vm, vm.argument(0), Value::UNDEFINED)?;

        // 4. If CompareISODate(plainYearMonth.[[ISODate]], other.[[ISODate]]) ≠ 0, return false.
        if compare_iso_date(plain_year_month.iso_date(), other.iso_date()) != 0 {
            return Ok(Value::from_bool(false));
        }

        // 5. Return CalendarEquals(plainYearMonth.[[Calendar]], other.[[Calendar]]).
        Ok(Value::from_bool(calendar_equals(
            Utf16View::of_string(&plain_year_month.calendar()),
            Utf16View::of_string(&other.calendar()),
        )))
    }

    // 9.3.19 Temporal.PlainYearMonth.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
        let show_calendar = get_temporal_show_calendar_name_option(vm, &resolved_options)?;

        // 5. Return TemporalYearMonthToString(plainYearMonth, showCalendar).
        Ok(string_value(
            vm,
            &temporal_year_month_to_string(&plain_year_month, show_calendar),
        ))
    }

    // 9.3.20 Temporal.PlainYearMonth.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.tolocalestring
    // 15.11.7.1 Temporal.PlainYearMonth.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plainyearmonth.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

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

        // 4. Return ? FormatDateTime(dateFormat, plainYearMonth).
        let formatted = format_date_time(vm, &date_format, &FormattableDateTime::PlainYearMonth(plain_year_month))?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 9.3.21 Temporal.PlainYearMonth.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. Return TemporalYearMonthToString(plainYearMonth, AUTO).
        Ok(string_value(
            vm,
            &temporal_year_month_to_string(&plain_year_month, ShowCalendar::Auto),
        ))
    }

    // 9.3.22 Temporal.PlainYearMonth.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.PlainYearMonth", &"a primitive value"],
        )
    }

    // 9.3.23 Temporal.PlainYearMonth.prototype.toPlainDate ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.toplaindate
    fn to_plain_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        let item = vm.argument(0);

        // 1. Let plainYearMonth be the this value.
        // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
        let plain_year_month = typed_this_plain_year_month(vm)?;

        // 3. If item is not an Object, throw a TypeError exception.
        if !item.is_object() {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::TypeError,
                ErrorType::NotAnObject
                    .utf16_message(&[Utf16View::of_string(&item.to_utf16_string_without_side_effects())]),
            );
        }

        // 4. Let calendar be plainYearMonth.[[Calendar]].
        let calendar_string = plain_year_month.calendar();
        let calendar = Utf16View::of_string(&calendar_string);

        // 5. Let fields be ISODateToFields(calendar, plainYearMonth.[[ISODate]], YEAR-MONTH).
        let fields = iso_date_to_fields(calendar, plain_year_month.iso_date(), DateType::YearMonth);

        // 6. Let inputFields be ? PrepareCalendarFields(calendar, item, « DAY », « », « »).
        let input_fields = prepare_calendar_fields(
            vm,
            calendar,
            &item.as_object(),
            &[CalendarField::Day],
            &[],
            CalendarFieldListOrPartial::List(&[]),
        )?;

        // 7. Let mergedFields be CalendarMergeFields(calendar, fields, inputFields).
        let mut merged_fields = calendar_merge_fields(calendar, &fields, &input_fields);

        // 8. Let isoDate be ? CalendarDateFromFields(calendar, mergedFields, CONSTRAIN).
        let iso_date = calendar_date_from_fields(vm, calendar, &mut merged_fields, Overflow::Constrain)?;

        // 9. Return ! CreateTemporalDate(isoDate, calendar).
        Ok(Value::from_object(
            create_temporal_date(vm, iso_date, calendar_string, None).must(),
        ))
    }
}
