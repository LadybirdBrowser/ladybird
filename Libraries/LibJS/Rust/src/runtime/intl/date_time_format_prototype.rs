/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::date_time_format::{
    DateTimeFormat, FormattableDateTime, for_each_calendar_field, format_date_time_range,
    format_date_time_range_to_parts, format_date_time_to_parts, to_date_time_formattable,
};
use crate::runtime::intl::date_time_format_function::DateTimeFormatFunction;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::unicode::date_time_format::{self as unicode, CalendarPatternFieldValue, HourCycle};

/// 11.3 Properties of the Intl.DateTimeFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-datetimeformat-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct DateTimeFormatPrototype {
    base: Object,
}

define_object_class!(DateTimeFormatPrototype, extends: [Object], methods: {
    initialize: DateTimeFormatPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_date_time_format(vm: &Vm) -> ThrowCompletionOr<Gc<DateTimeFormat>> {
    typed_this_object::<DateTimeFormat>(vm, "Intl.DateTimeFormat")
}

impl DateTimeFormatPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DateTimeFormatPrototype> {
        realm.create_object(
            vm,
            DateTimeFormatPrototype {
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

        // 11.3.7 Intl.DateTimeFormat.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.DateTimeFormat")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_native_accessor(
            vm,
            realm,
            &names.format,
            raw_native!(DateTimeFormatPrototype::format),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(DateTimeFormatPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatRange,
            raw_native!(DateTimeFormatPrototype::format_range),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatRangeToParts,
            raw_native!(DateTimeFormatPrototype::format_range_to_parts),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatToParts,
            raw_native!(DateTimeFormatPrototype::format_to_parts),
            1,
            attr,
            None,
        );
    }

    // 11.3.2 Intl.DateTimeFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let dtf be the this value.
        // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
        //     a. Set dtf to ? UnwrapDateTimeFormat(dtf).
        // 3. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
        let date_time_format = typed_this_date_time_format(vm)?;

        // 4. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        let create_data_property = |property: &PropertyKey, value: Value| {
            options.create_data_property_or_throw(vm, property, value).must();
        };
        let string = |string: &'static str| Value::from_string(PrimitiveString::create_from_utf8(vm, string));

        // 5. For each row of Table 15, except the header row, in table order, do
        //    a. Let p be the Property value of the current row.
        //    b. If there is an Internal Slot value in the current row, then
        //        i. Let v be the value of dtf's internal slot whose name is the Internal Slot value of the current row.
        //    c. Else,
        //        i. Let format be dtf.[[DateTimeFormat]].
        //        ii. If format has a field [[<p>]] and dtf.[[DateStyle]] is undefined and dtf.[[TimeStyle]] is undefined, then
        //            1. Let v be format.[[<p>]].
        //        iii. Else,
        //            1. Let v be undefined.
        //    d. If v is not undefined, then
        //        i. If there is a Conversion value in the current row, then
        //            1. Let conversion be the Conversion value of the current row.
        //            2. If conversion is hour12, then
        //                a. If v is "h11" or "h12", set v to true. Otherwise, set v to false.
        //            3. Else,
        //                a. Assert: conversion is number.
        //                b. Set v to 𝔽(v).
        //        ii. Perform ! CreateDataPropertyOrThrow(options, p, v).
        create_data_property(
            &names.locale,
            Value::from_string(PrimitiveString::create(vm, date_time_format.locale())),
        );
        create_data_property(
            &names.calendar,
            Value::from_string(PrimitiveString::create(vm, date_time_format.calendar())),
        );
        create_data_property(
            &names.numberingSystem,
            Value::from_string(PrimitiveString::create(vm, date_time_format.numbering_system())),
        );
        create_data_property(
            &names.timeZone,
            Value::from_string(PrimitiveString::create(vm, date_time_format.time_zone())),
        );

        let format = date_time_format.date_time_format();

        if let Some(hour_cycle) = format.hour_cycle {
            create_data_property(&names.hourCycle, string(unicode::hour_cycle_to_string(hour_cycle)));

            match hour_cycle {
                HourCycle::H11 | HourCycle::H12 => create_data_property(&names.hour12, Value::from_bool(true)),
                HourCycle::H23 | HourCycle::H24 => create_data_property(&names.hour12, Value::from_bool(false)),
            }
        }

        if !date_time_format.has_date_style() && !date_time_format.has_time_style() {
            for_each_calendar_field(vm, |row| {
                match format.field(row.field) {
                    None => {}
                    Some(CalendarPatternFieldValue::FractionalSecondDigits(digits)) => {
                        create_data_property(row.property, Value::from_i32(i32::from(digits)));
                    }
                    Some(CalendarPatternFieldValue::Style(style)) => {
                        create_data_property(row.property, string(unicode::calendar_pattern_style_to_string(style)));
                    }
                }
                Ok(())
            })
            .must();
        }

        if date_time_format.has_date_style() {
            create_data_property(&names.dateStyle, string(date_time_format.date_style_string()));
        }
        if date_time_format.has_time_style() {
            create_data_property(&names.timeStyle, string(date_time_format.time_style_string()));
        }

        // 6. Return options.
        Ok(Value::from_object(options))
    }

    // 11.3.3 get Intl.DateTimeFormat.prototype.format, https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.format
    fn format(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let dtf be the this value.
        // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
        //     a. Set dtf to ? UnwrapDateTimeFormat(dtf).
        // 3. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
        let date_time_format = typed_this_date_time_format(vm)?;

        // 4. If dtf.[[BoundFormat]] is undefined, then
        if date_time_format.bound_format().is_none() {
            // a. Let F be a new built-in function object as defined in DateTime Format Functions (11.1.6).
            // b. Set F.[[DateTimeFormat]] to dtf.
            let bound_format = DateTimeFormatFunction::create(vm, realm, date_time_format);

            // c. Set dtf.[[BoundFormat]] to F.
            date_time_format.set_bound_format(Some(bound_format));
        }

        // 5. Return dtf.[[BoundFormat]].
        Ok(Value::from_object(
            date_time_format
                .bound_format()
                .expect("the bound format function was just created")
                .upcast::<Object>(),
        ))
    }

    // 11.3.4 Intl.DateTimeFormat.prototype.formatRange ( startDate, endDate ), https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.formatRange
    // 15.7.2 Intl.DateTimeFormat.prototype.formatRange ( startDate, endDate ), https://tc39.es/proposal-temporal/#sec-intl.datetimeformat.prototype.formatRange
    fn format_range(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start_date_value = vm.argument(0);
        let end_date_value = vm.argument(1);

        // 1. Let dtf be this value.
        // 2. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
        let date_time_format = typed_this_date_time_format(vm)?;

        // 3. If startDate is undefined or endDate is undefined, throw a TypeError exception.
        if start_date_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"startDate"]);
        }
        if end_date_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"endDate"]);
        }

        // 4. Let x be ? ToDateTimeFormattable(startDate).
        let start_date = to_date_time_formattable(vm, start_date_value)?;

        // 5. Let y be ? ToDateTimeFormattable(endDate).
        let end_date = to_date_time_formattable(vm, end_date_value)?;

        // 6. Return ? FormatDateTimeRange(dtf, x, y).
        let formatted = format_date_time_range(vm, &date_time_format, &start_date, &end_date)?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 11.3.5 Intl.DateTimeFormat.prototype.formatRangeToParts ( startDate, endDate ), https://tc39.es/ecma402/#sec-Intl.DateTimeFormat.prototype.formatRangeToParts
    // 15.7.3 Intl.DateTimeFormat.prototype.formatRangeToParts ( startDate, endDate ), https://tc39.es/proposal-temporal/#sec-Intl.DateTimeFormat.prototype.formatRangeToParts
    fn format_range_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start_date_value = vm.argument(0);
        let end_date_value = vm.argument(1);

        // 1. Let dtf be this value.
        // 2. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
        let date_time_format = typed_this_date_time_format(vm)?;

        // 3. If startDate is undefined or endDate is undefined, throw a TypeError exception.
        if start_date_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"startDate"]);
        }
        if end_date_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"endDate"]);
        }

        // 4. Let x be ? ToDateTimeFormattable(startDate).
        let start_date = to_date_time_formattable(vm, start_date_value)?;

        // 5. Let y be ? ToDateTimeFormattable(endDate).
        let end_date = to_date_time_formattable(vm, end_date_value)?;

        // 6. Return ? FormatDateTimeRangeToParts(dtf, x, y).
        Ok(Value::from_object(format_date_time_range_to_parts(
            vm,
            &date_time_format,
            &start_date,
            &end_date,
        )?))
    }

    // 11.3.6 Intl.DateTimeFormat.prototype.formatToParts ( date ), https://tc39.es/ecma402/#sec-Intl.DateTimeFormat.prototype.formatToParts
    // 15.7.1 Intl.DateTimeFormat.prototype.formatToParts ( date ), https://tc39.es/proposal-temporal/#sec-Intl.DateTimeFormat.prototype.formatToParts
    fn format_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        let date_value = vm.argument(0);

        // 1. Let dtf be the this value.
        // 2. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
        let date_time_format = typed_this_date_time_format(vm)?;

        // 3. If date is undefined, then
        let date = if date_value.is_undefined() {
            // a. Let x be ! Call(%Date.now%, undefined).
            FormattableDateTime::Number(
                call(
                    vm,
                    Value::from_object(realm.intrinsics().date_constructor_now_function().upcast::<Object>()),
                    Value::UNDEFINED,
                    &[],
                )
                .must()
                .as_f64(),
            )
        }
        // 4. Else,
        else {
            // a. Let x be ? ToDateTimeFormattable(date).
            to_date_time_formattable(vm, date_value)?
        };

        // 5. Return ? FormatDateTimeToParts(dtf, x).
        Ok(Value::from_object(format_date_time_to_parts(
            vm,
            &date_time_format,
            &date,
        )?))
    }
}
