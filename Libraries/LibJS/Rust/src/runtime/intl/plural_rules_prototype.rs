/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::number_format::to_intl_mathematical_value;
use crate::runtime::intl::plural_rules::{PluralRules, resolve_plural, resolve_plural_range};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::unicode::number_format as unicode;

/// 17.3 Properties of the Intl.PluralRules Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-pluralrules-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct PluralRulesPrototype {
    base: Object,
}

define_object_class!(PluralRulesPrototype, extends: [Object], methods: {
    initialize: PluralRulesPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_plural_rules(vm: &Vm) -> ThrowCompletionOr<Gc<PluralRules>> {
    typed_this_object::<PluralRules>(vm, "Intl.PluralRules")
}

impl PluralRulesPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PluralRulesPrototype> {
        realm.create_object(
            vm,
            PluralRulesPrototype {
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

        // 17.3.5 Intl.PluralRules.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.pluralrules.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.PluralRules")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(PluralRulesPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.select,
            raw_native!(PluralRulesPrototype::select),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.selectRange,
            raw_native!(PluralRulesPrototype::select_range),
            2,
            attr,
            None,
        );
    }

    // 17.3.2 Intl.PluralRules.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let pr be the this value.
        // 2. Perform ? RequireInternalSlot(pr, [[InitializedPluralRules]]).
        let plural_rules = typed_this_plural_rules(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. Let pluralCategories be a List of Strings containing all possible results of PluralRuleSelect for the selected
        //    locale pr.[[Locale]], sorted according to the following order: "zero", "one", "two", "few", "many", "other".
        let available_categories = plural_rules.with_formatter(|formatter| formatter.available_plural_categories());

        let plural_category_values = MarkedVec::new(vm);
        for category in available_categories {
            plural_category_values.push(Value::from_string(PrimitiveString::create_from_utf8(
                vm,
                unicode::plural_category_to_string(category),
            )));
        }
        let plural_categories = Array::create_from_list(vm, realm, &plural_category_values);

        // 5. For each row of Table 32, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. If p is "pluralCategories", then
        //         i. Let v be CreateArrayFromList(pluralCategories).
        //     c. Else,
        //         i. Let v be the value of pr's internal slot whose name is the Internal Slot value of the current row.
        //     d. If v is not undefined, then
        //         i. Perform ! CreateDataPropertyOrThrow(options, p, v).
        let create = |property: &PropertyKey, value: Value| {
            options.create_data_property_or_throw(vm, property, value).must();
        };
        let string = |string: &str| Value::from_string(PrimitiveString::create_from_utf8(vm, string));

        create(
            &names.locale,
            Value::from_string(PrimitiveString::create(vm, plural_rules.locale())),
        );
        create(&names.type_, string(plural_rules.type_string()));
        create(&names.notation, string(plural_rules.notation_string()));
        if plural_rules.has_compact_display() {
            create(&names.compactDisplay, string(plural_rules.compact_display_string()));
        }
        create(
            &names.minimumIntegerDigits,
            Value::from_i32(plural_rules.min_integer_digits()),
        );
        if plural_rules.has_min_fraction_digits() {
            create(
                &names.minimumFractionDigits,
                Value::from_i32(plural_rules.min_fraction_digits()),
            );
        }
        if plural_rules.has_max_fraction_digits() {
            create(
                &names.maximumFractionDigits,
                Value::from_i32(plural_rules.max_fraction_digits()),
            );
        }
        if plural_rules.has_min_significant_digits() {
            create(
                &names.minimumSignificantDigits,
                Value::from_i32(plural_rules.min_significant_digits()),
            );
        }
        if plural_rules.has_max_significant_digits() {
            create(
                &names.maximumSignificantDigits,
                Value::from_i32(plural_rules.max_significant_digits()),
            );
        }
        create(&names.pluralCategories, Value::from_object(plural_categories));
        create(
            &names.roundingIncrement,
            Value::from_i32(plural_rules.rounding_increment()),
        );
        create(&names.roundingMode, string(plural_rules.rounding_mode_string()));
        create(
            &names.roundingPriority,
            string(plural_rules.computed_rounding_priority_string()),
        );
        create(
            &names.trailingZeroDisplay,
            string(plural_rules.trailing_zero_display_string()),
        );

        // 10. Return options.
        Ok(Value::from_object(options))
    }

    // 17.3.3 Intl.PluralRules.prototype.select ( value ), https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.select
    fn select(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let pr be the this value.
        // 2. Perform ? RequireInternalSlot(pr, [[InitializedPluralRules]]).
        let plural_rules = typed_this_plural_rules(vm)?;

        // 3. Let n be ? ToIntlMathematicalValue(value).
        let number = to_intl_mathematical_value(vm, value)?;

        // 4. Return ! ResolvePlural(pr, n).[[PluralCategory]].
        let plurality = resolve_plural(&plural_rules, &number);
        Ok(Value::from_string(PrimitiveString::create_from_utf8(
            vm,
            unicode::plural_category_to_string(plurality),
        )))
    }

    // 17.3.4 Intl.PluralRules.prototype.selectRange ( start, end ), https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.selectrange
    fn select_range(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let pr be the this value.
        // 2. Perform ? RequireInternalSlot(pr, [[InitializedPluralRules]]).
        let plural_rules = typed_this_plural_rules(vm)?;

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

        // 6. Return ? ResolvePluralRange(pr, x, y).
        let plurality = resolve_plural_range(vm, &plural_rules, &x, &y)?;
        Ok(Value::from_string(PrimitiveString::create_from_utf8(
            vm,
            unicode::plural_category_to_string(plurality),
        )))
    }
}
