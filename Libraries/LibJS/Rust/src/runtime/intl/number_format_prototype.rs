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
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::number_format::{
    NumberFormat, format_numeric_range, format_numeric_range_to_parts, format_numeric_to_parts,
    to_intl_mathematical_value,
};
use crate::runtime::intl::number_format_function::NumberFormatFunction;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// 16.3 Properties of the Intl.NumberFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-numberformat-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct NumberFormatPrototype {
    base: Object,
}

define_object_class!(NumberFormatPrototype, extends: [Object], methods: {
    initialize: NumberFormatPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_number_format(vm: &Vm) -> ThrowCompletionOr<Gc<NumberFormat>> {
    typed_this_object::<NumberFormat>(vm, "Intl.NumberFormat")
}

impl NumberFormatPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<NumberFormatPrototype> {
        realm.create_object(
            vm,
            NumberFormatPrototype {
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

        // 16.3.7 Intl.NumberFormat.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.numberformat.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.NumberFormat")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_native_accessor(
            vm,
            realm,
            &names.format,
            raw_native!(NumberFormatPrototype::format),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(NumberFormatPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatRange,
            raw_native!(NumberFormatPrototype::format_range),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatRangeToParts,
            raw_native!(NumberFormatPrototype::format_range_to_parts),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatToParts,
            raw_native!(NumberFormatPrototype::format_to_parts),
            1,
            attr,
            None,
        );
    }

    // 16.3.2 Intl.NumberFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let nf be the this value.
        // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
        //     a. Set nf to ? UnwrapNumberFormat(nf).
        // 3. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
        let number_format = typed_this_number_format(vm)?;

        // 4. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 5. For each row of Table 28, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of nf's internal slot whose name is the Internal Slot value of the current row.
        //     c. If v is not undefined, then
        //         i. Perform ! CreateDataPropertyOrThrow(options, p, v).
        let create = |property: &PropertyKey, value: Value| {
            options.create_data_property_or_throw(vm, property, value).must();
        };
        let string = |string: &str| Value::from_string(PrimitiveString::create_from_utf8(vm, string));

        create(
            &names.locale,
            Value::from_string(PrimitiveString::create(vm, number_format.locale())),
        );
        create(
            &names.numberingSystem,
            Value::from_string(PrimitiveString::create(vm, number_format.numbering_system())),
        );
        create(&names.style, string(number_format.style_string()));
        if number_format.has_currency() {
            create(
                &names.currency,
                Value::from_string(PrimitiveString::create(vm, number_format.currency())),
            );
        }
        if number_format.has_currency_display() {
            create(&names.currencyDisplay, string(number_format.currency_display_string()));
        }
        if number_format.has_currency_sign() {
            create(&names.currencySign, string(number_format.currency_sign_string()));
        }
        if number_format.has_unit() {
            create(
                &names.unit,
                Value::from_string(PrimitiveString::create(vm, number_format.unit())),
            );
        }
        if number_format.has_unit_display() {
            create(&names.unitDisplay, string(number_format.unit_display_string()));
        }
        create(
            &names.minimumIntegerDigits,
            Value::from_i32(number_format.min_integer_digits()),
        );
        if number_format.has_min_fraction_digits() {
            create(
                &names.minimumFractionDigits,
                Value::from_i32(number_format.min_fraction_digits()),
            );
        }
        if number_format.has_max_fraction_digits() {
            create(
                &names.maximumFractionDigits,
                Value::from_i32(number_format.max_fraction_digits()),
            );
        }
        if number_format.has_min_significant_digits() {
            create(
                &names.minimumSignificantDigits,
                Value::from_i32(number_format.min_significant_digits()),
            );
        }
        if number_format.has_max_significant_digits() {
            create(
                &names.maximumSignificantDigits,
                Value::from_i32(number_format.max_significant_digits()),
            );
        }
        create(&names.useGrouping, number_format.use_grouping_to_value(vm));
        create(&names.notation, string(number_format.notation_string()));
        if number_format.has_compact_display() {
            create(&names.compactDisplay, string(number_format.compact_display_string()));
        }
        create(&names.signDisplay, string(number_format.sign_display_string()));
        create(
            &names.roundingIncrement,
            Value::from_i32(number_format.rounding_increment()),
        );
        create(&names.roundingMode, string(number_format.rounding_mode_string()));
        create(
            &names.roundingPriority,
            string(number_format.computed_rounding_priority_string()),
        );
        create(
            &names.trailingZeroDisplay,
            string(number_format.trailing_zero_display_string()),
        );

        // 6. Return options.
        Ok(Value::from_object(options))
    }

    // 16.3.3 get Intl.NumberFormat.prototype.format, https://tc39.es/ecma402/#sec-intl.numberformat.prototype.format
    fn format(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let nf be the this value.
        // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
        //     a. Set nf to ? UnwrapNumberFormat(nf).
        // 3. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
        let number_format = typed_this_number_format(vm)?;

        // 4. If nf.[[BoundFormat]] is undefined, then
        if number_format.bound_format().is_none() {
            // a. Let F be a new built-in function object as defined in Number Format Functions (16.1.4).
            // b. Set F.[[NumberFormat]] to nf.
            let bound_format = NumberFormatFunction::create(vm, realm, number_format);

            // c. Set nf.[[BoundFormat]] to F.
            number_format.set_bound_format(Some(bound_format));
        }

        // 5. Return nf.[[BoundFormat]].
        Ok(Value::from_object(
            number_format
                .bound_format()
                .expect("the bound format function was just created"),
        ))
    }

    // 16.3.4 Intl.NumberFormat.prototype.formatRange ( start, end ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.formatrange
    fn format_range(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let nf be the this value.
        // 2. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
        let number_format = typed_this_number_format(vm)?;

        // 3. If start is undefined or end is undefined, throw a TypeError exception.
        if start.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"start"]);
        }
        if end.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"end"]);
        }

        // 4. Let x be ? ToIntlMathematicalValue(start).
        let x = to_intl_mathematical_value(vm, start)?;

        // 5. Let y be ? ToIntlMathematicalValue(end).
        let y = to_intl_mathematical_value(vm, end)?;

        // 6. Return ? FormatNumericRange(nf, x, y).
        let formatted = format_numeric_range(vm, &number_format, &x, &y)?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 16.3.5 Intl.NumberFormat.prototype.formatRangeToParts ( start, end ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.formatrangetoparts
    fn format_range_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let nf be the this value.
        // 2. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
        let number_format = typed_this_number_format(vm)?;

        // 3. If start is undefined or end is undefined, throw a TypeError exception.
        if start.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"start"]);
        }
        if end.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"end"]);
        }

        // 4. Let x be ? ToIntlMathematicalValue(start).
        let x = to_intl_mathematical_value(vm, start)?;

        // 5. Let y be ? ToIntlMathematicalValue(end).
        let y = to_intl_mathematical_value(vm, end)?;

        // 6. Return ? FormatNumericRangeToParts(nf, x, y).
        Ok(Value::from_object(format_numeric_range_to_parts(
            vm,
            number_format,
            &x,
            &y,
        )?))
    }

    // 16.3.6 Intl.NumberFormat.prototype.formatToParts ( value ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.formattoparts
    fn format_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let nf be the this value.
        // 2. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
        let number_format = typed_this_number_format(vm)?;

        // 3. Let x be ? ToIntlMathematicalValue(value).
        let mathematical_value = to_intl_mathematical_value(vm, value)?;

        // 4. Return ? FormatNumericToParts(nf, x).
        Ok(Value::from_object(format_numeric_to_parts(
            vm,
            number_format,
            &mathematical_value,
        )))
    }
}
