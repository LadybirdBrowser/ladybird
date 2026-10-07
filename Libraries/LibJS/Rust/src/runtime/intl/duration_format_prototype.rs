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
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::intl::duration_format::{
    DurationFormat, DurationUnitOptions, ValueStyle, partition_duration_format_pattern,
};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::duration::to_temporal_duration;
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// 13.3 Properties of the Intl.DurationFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-durationformat-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct DurationFormatPrototype {
    base: Object,
}

define_object_class!(DurationFormatPrototype, extends: [Object], methods: {
    initialize: DurationFormatPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_duration_format(vm: &Vm) -> ThrowCompletionOr<Gc<DurationFormat>> {
    typed_this_object::<DurationFormat>(vm, "Intl.DurationFormat")
}

impl DurationFormatPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DurationFormatPrototype> {
        realm.create_object(
            vm,
            DurationFormatPrototype {
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

        // 13.3.5 Intl.DurationFormat.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.DurationFormat")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(DurationFormatPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.format,
            raw_native!(DurationFormatPrototype::format),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatToParts,
            raw_native!(DurationFormatPrototype::format_to_parts),
            1,
            attr,
            None,
        );
    }

    // 13.3.2 Intl.DurationFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.resolvedOptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let df be the this value.
        // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
        let duration_format = typed_this_duration_format(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. For each row of Table 21, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of df's internal slot whose name is the Internal Slot value of the current row.
        //     c. If v is not undefined, then
        //         i. If there is a Conversion value in the current row, let conversion be that value; else let conversion be empty.
        //         ii. If conversion is number, then
        //             1. Set v to 𝔽(v).
        // NOTE: This case is for fractionalDigits and is handled separately below.
        //         iv. Perform ! CreateDataPropertyOrThrow(options, p, v).
        let create_string_option = |property: &PropertyKey, value: Value| {
            options.create_data_property_or_throw(vm, property, value).must();
        };

        //         iii. Else if conversion is not empty, then
        let create_unit_options_option =
            |property: &PropertyKey, display_property: &PropertyKey, value: DurationUnitOptions| {
                // 1. Assert: conversion is STYLE+DISPLAY and v is a Duration Unit Options Record.
                // 2. NOTE: v.[[Style]] will be represented with a property named p (a plural Temporal unit), then v.[[Display]] will be represented with a property whose name suffixes p with "Display".

                // 3. Let style be v.[[Style]].
                let mut style = value.style;

                // 4. If style is "fractional", then
                if style == ValueStyle::Fractional {
                    // a. Assert: IsFractionalSecondUnitName(p) is true.
                    // b. Set style to "numeric".
                    style = ValueStyle::Numeric;
                }

                // 5. Perform ! CreateDataPropertyOrThrow(options, p, style).
                let style_string = DurationFormat::value_style_to_string(style);
                options
                    .create_data_property_or_throw(
                        vm,
                        property,
                        Value::from_string(PrimitiveString::create_from_utf8(vm, style_string)),
                    )
                    .must();

                // 6. Set p to the string-concatenation of p and "Display".
                // 7. Set v to v.[[Display]].
                let display_string = DurationFormat::display_to_string(value.display);
                options
                    .create_data_property_or_throw(
                        vm,
                        display_property,
                        Value::from_string(PrimitiveString::create_from_utf8(vm, display_string)),
                    )
                    .must();
            };

        create_string_option(
            &names.locale,
            Value::from_string(PrimitiveString::create(vm, duration_format.locale())),
        );
        create_string_option(
            &names.numberingSystem,
            Value::from_string(PrimitiveString::create(vm, duration_format.numbering_system())),
        );
        create_string_option(
            &names.style,
            Value::from_string(PrimitiveString::create_from_utf8(vm, duration_format.style_string())),
        );
        create_unit_options_option(&names.years, &names.yearsDisplay, duration_format.years_options());
        create_unit_options_option(&names.months, &names.monthsDisplay, duration_format.months_options());
        create_unit_options_option(&names.weeks, &names.weeksDisplay, duration_format.weeks_options());
        create_unit_options_option(&names.days, &names.daysDisplay, duration_format.days_options());
        create_unit_options_option(&names.hours, &names.hoursDisplay, duration_format.hours_options());
        create_unit_options_option(&names.minutes, &names.minutesDisplay, duration_format.minutes_options());
        create_unit_options_option(&names.seconds, &names.secondsDisplay, duration_format.seconds_options());
        create_unit_options_option(
            &names.milliseconds,
            &names.millisecondsDisplay,
            duration_format.milliseconds_options(),
        );
        create_unit_options_option(
            &names.microseconds,
            &names.microsecondsDisplay,
            duration_format.microseconds_options(),
        );
        create_unit_options_option(
            &names.nanoseconds,
            &names.nanosecondsDisplay,
            duration_format.nanoseconds_options(),
        );

        if duration_format.has_fractional_digits() {
            options
                .create_data_property_or_throw(
                    vm,
                    &names.fractionalDigits,
                    Value::from_i32(i32::from(duration_format.fractional_digits())),
                )
                .must();
        }

        // 5. Return options.
        Ok(Value::from_object(options))
    }

    // 13.3.3 Intl.DurationFormat.prototype.format ( duration ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.format
    // 15.10.1 Intl.DurationFormat.prototype.format ( durationLike ), https://tc39.es/proposal-temporal/#sec-Intl.DurationFormat.prototype.format
    fn format(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let df be this value.
        // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
        let duration_format = typed_this_duration_format(vm)?;

        // 3. Let duration be ? ToTemporalDuration(durationLike).
        let duration = to_temporal_duration(vm, vm.argument(0))?;

        // 4. Let parts be PartitionDurationFormatPattern(df, duration).
        let parts = partition_duration_format_pattern(vm, duration_format, duration);

        // 5. Let result be a new empty String.
        let mut result = Utf16StringBuilder::new();

        // 6. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
        for part in &parts {
            // a. Set result to the string-concatenation of result and part.[[Value]].
            result.append(Utf16View::of_string(&part.value));
        }

        // 7. Return result.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            result.to_utf16_string(),
        )))
    }

    // 13.3.4 Intl.DurationFormat.prototype.formatToParts ( duration ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.formatToParts
    // 15.10.2 Intl.DurationFormat.prototype.formatToParts ( durationLike ), https://tc39.es/proposal-temporal/#sec-Intl.DurationFormat.prototype.formatToParts
    fn format_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let df be this value.
        // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
        let duration_format = typed_this_duration_format(vm)?;

        // 3. Let duration be ? ToTemporalDuration(durationLike).
        let duration = to_temporal_duration(vm, vm.argument(0))?;

        // 4. Let parts be PartitionDurationFormatPattern(df, duration).
        let parts = partition_duration_format_pattern(vm, duration_format, duration);

        // 5. Let result be ! ArrayCreate(0).
        let result = Array::create(vm, realm, 0, None).must();

        // 6. Let n be 0.
        // 7. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
        for (n, part) in parts.into_iter().enumerate() {
            // a. Let obj be OrdinaryObjectCreate(%Object.prototype%).
            let object = Object::create(vm, realm, Some(realm.object_prototype()));

            // b. Perform ! CreateDataPropertyOrThrow(obj, "type", part.[[Type]]).
            object
                .create_data_property_or_throw(
                    vm,
                    &names.type_,
                    Value::from_string(PrimitiveString::create(vm, part.type_)),
                )
                .must();

            // c. Perform ! CreateDataPropertyOrThrow(obj, "value", part.[[Value]]).
            object
                .create_data_property_or_throw(
                    vm,
                    &names.value,
                    Value::from_string(PrimitiveString::create(vm, part.value)),
                )
                .must();

            // d. If part.[[Unit]] is not empty, perform ! CreateDataPropertyOrThrow(obj, "unit", part.[[Unit]]).
            if !part.unit.is_empty() {
                object
                    .create_data_property_or_throw(
                        vm,
                        &names.unit,
                        Value::from_string(PrimitiveString::create(vm, part.unit)),
                    )
                    .must();
            }

            // e. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), obj).
            result
                .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
                .must();

            // f. Set n to n + 1.
        }

        // 8. Return result.
        Ok(Value::from_object(result))
    }
}
