/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainMonthDayPrototype.cpp: %Temporal.PlainMonthDay.prototype%.

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
    DateType, Overflow, ShowCalendar, get_temporal_overflow_option, get_temporal_show_calendar_name_option,
    is_partial_temporal_object, iso_date_to_fields,
};
use crate::runtime::temporal::calendar::{
    CalendarField, CalendarFieldListOrPartial, calendar_date_from_fields, calendar_equals, calendar_iso_to_date,
    calendar_merge_fields, calendar_month_day_from_fields, prepare_calendar_fields,
};
use crate::runtime::temporal::plain_date::{compare_iso_date, create_temporal_date};
use crate::runtime::temporal::plain_month_day::{
    PlainMonthDay, create_temporal_month_day, temporal_month_day_to_string, to_temporal_month_day,
};
use crate::utf16::Utf16View;

// 10.3 Properties of the Temporal.PlainMonthDay Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plainmonthday-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct PlainMonthDayPrototype {
    base: Object,
}

define_object_class!(PlainMonthDayPrototype, extends: [Object], methods: {
    initialize: PlainMonthDayPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_plain_month_day(vm: &Vm) -> ThrowCompletionOr<Gc<PlainMonthDay>> {
    typed_this_object::<PlainMonthDay>(vm, "Temporal.PlainMonthDay")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

impl PlainMonthDayPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainMonthDayPrototype> {
        realm.create_object(
            vm,
            PlainMonthDayPrototype {
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

        // 10.3.2 Temporal.PlainMonthDay.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.PlainMonthDay")),
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
            raw_native!(PlainMonthDayPrototype::calendar_id_getter),
        );
        define_getter(&names.monthCode, raw_native!(PlainMonthDayPrototype::month_code_getter));
        define_getter(&names.day, raw_native!(PlainMonthDayPrototype::day_getter));

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.with, raw_native!(PlainMonthDayPrototype::with), 1);
        define_native_function(&names.equals, raw_native!(PlainMonthDayPrototype::equals), 1);
        define_native_function(&names.toString, raw_native!(PlainMonthDayPrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(PlainMonthDayPrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(PlainMonthDayPrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(PlainMonthDayPrototype::value_of), 0);
        define_native_function(
            &names.toPlainDate,
            raw_native!(PlainMonthDayPrototype::to_plain_date),
            1,
        );
    }

    // 10.3.3 get Temporal.PlainMonthDay.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.calendarid
    fn calendar_id_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainMonthDay be the this value
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. Return plainMonthDay.[[Calendar]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            plain_month_day.calendar(),
        )))
    }

    // 10.3.4 get Temporal.PlainMonthDay.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.monthcode
    fn month_code_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainMonthDay be the this value
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. Return CalendarISOToDate(plainMonthDay.[[Calendar]], plainMonthDay.[[ISODate]]).[[MonthCode]].
        let month_code = calendar_iso_to_date(
            Utf16View::of_string(&plain_month_day.calendar()),
            plain_month_day.iso_date(),
        )
        .month_code;
        Ok(Value::from_string(PrimitiveString::create(vm, month_code)))
    }

    // 10.3.5 get Temporal.PlainMonthDay.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.day
    fn day_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainMonthDay.[[Calendar]], plainMonthDay.[[ISODate]]).[[Day]]).
        let day = calendar_iso_to_date(
            Utf16View::of_string(&plain_month_day.calendar()),
            plain_month_day.iso_date(),
        )
        .day;
        Ok(Value::from_i32(i32::from(day)))
    }

    // 10.3.6 Temporal.PlainMonthDay.prototype.with ( temporalMonthDayLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_month_day_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. If ? IsPartialTemporalObject(temporalMonthDayLike) is false, throw a TypeError exception.
        if !is_partial_temporal_object(vm, temporal_month_day_like)? {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TemporalObjectMustBePartialTemporalObject,
                &[],
            );
        }

        // 4. Let calendar be plainMonthDay.[[Calendar]].
        let calendar_string = plain_month_day.calendar();
        let calendar = Utf16View::of_string(&calendar_string);

        // 5. Let fields be ISODateToFields(calendar, plainMonthDay.[[ISODate]], MONTH-DAY).
        let fields = iso_date_to_fields(calendar, plain_month_day.iso_date(), DateType::MonthDay);

        // 6. Let partialMonthDay be ? PrepareCalendarFields(calendar, temporalMonthDayLike, « YEAR, MONTH, MONTH-CODE, DAY », « », PARTIAL).
        let partial_month_day = prepare_calendar_fields(
            vm,
            calendar,
            &temporal_month_day_like.as_object(),
            &[
                CalendarField::Year,
                CalendarField::Month,
                CalendarField::MonthCode,
                CalendarField::Day,
            ],
            &[],
            CalendarFieldListOrPartial::Partial,
        )?;

        // 7. Set fields to CalendarMergeFields(calendar, fields, partialMonthDay).
        let mut fields = calendar_merge_fields(calendar, &fields, &partial_month_day);

        // 8. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 9. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // 10. Let isoDate be ? CalendarMonthDayFromFields(calendar, fields, overflow).
        let iso_date = calendar_month_day_from_fields(vm, calendar, &mut fields, overflow)?;

        // 11. Return ! CreateTemporalMonthDay(isoDate, calendar).
        Ok(Value::from_object(
            create_temporal_month_day(vm, iso_date, calendar_string, None).must(),
        ))
    }

    // 10.3.7 Temporal.PlainMonthDay.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. Set other to ? ToTemporalMonthDay(other).
        let other = to_temporal_month_day(vm, vm.argument(0), Value::UNDEFINED)?;

        // 4. If CompareISODate(plainMonthDay.[[ISODate]], other.[[ISODate]]) ≠ 0, return false.
        if compare_iso_date(plain_month_day.iso_date(), other.iso_date()) != 0 {
            return Ok(Value::from_bool(false));
        }

        // 5. Return CalendarEquals(plainMonthDay.[[Calendar]], other.[[Calendar]]).
        Ok(Value::from_bool(calendar_equals(
            Utf16View::of_string(&plain_month_day.calendar()),
            Utf16View::of_string(&other.calendar()),
        )))
    }

    // 10.3.8 Temporal.PlainMonthDay.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
        let show_calendar = get_temporal_show_calendar_name_option(vm, &resolved_options)?;

        // 5. Return TemporalMonthDayToString(plainMonthDay, showCalendar).
        Ok(string_value(
            vm,
            &temporal_month_day_to_string(&plain_month_day, show_calendar),
        ))
    }

    // 10.3.9 Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tolocalestring
    // 15.11.5.1 Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plainmonthday.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

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

        // 4. Return ? FormatDateTime(dateFormat, plainMonthDay).
        let formatted = format_date_time(vm, &date_format, &FormattableDateTime::PlainMonthDay(plain_month_day))?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 10.3.10 Temporal.PlainMonthDay.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tolocalestring
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. Return TemporalMonthDayToString(plainMonthDay, auto).
        Ok(string_value(
            vm,
            &temporal_month_day_to_string(&plain_month_day, ShowCalendar::Auto),
        ))
    }

    // 10.3.11 Temporal.PlainMonthDay.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.PlainMonthDay", &"a primitive value"],
        )
    }

    // 10.3.12 Temporal.PlainMonthDay.prototype.toPlainDate ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.toplaindate
    fn to_plain_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        let item = vm.argument(0);

        // 1. Let plainMonthDay be the this value.
        // 2. Perform ? RequireInternalSlot(plainMonthDay, [[InitializedTemporalMonthDay]]).
        let plain_month_day = typed_this_plain_month_day(vm)?;

        // 3. If item is not an Object, throw a TypeError exception.
        if !item.is_object() {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::TypeError,
                ErrorType::NotAnObject
                    .utf16_message(&[Utf16View::of_string(&item.to_utf16_string_without_side_effects())]),
            );
        }

        // 4. Let calendar be plainMonthDay.[[Calendar]].
        let calendar_string = plain_month_day.calendar();
        let calendar = Utf16View::of_string(&calendar_string);

        // 5. Let fields be ISODateToFields(calendar, plainMonthDay.[[ISODate]], MONTH-DAY).
        let fields = iso_date_to_fields(calendar, plain_month_day.iso_date(), DateType::MonthDay);

        // 6. Let inputFields be ? PrepareCalendarFields(calendar, item, « YEAR », « », « »).
        let input_fields = prepare_calendar_fields(
            vm,
            calendar,
            &item.as_object(),
            &[CalendarField::Year],
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
