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
use crate::runtime::intl::relative_time_format::{
    RelativeTimeFormat, format_relative_time, format_relative_time_to_parts,
};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::utf16::Utf16View;

/// 18.3 Properties of the Intl.RelativeTimeFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-relativetimeformat-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct RelativeTimeFormatPrototype {
    base: Object,
}

define_object_class!(RelativeTimeFormatPrototype, extends: [Object], methods: {
    initialize: RelativeTimeFormatPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_relative_time_format(vm: &Vm) -> ThrowCompletionOr<Gc<RelativeTimeFormat>> {
    typed_this_object::<RelativeTimeFormat>(vm, "Intl.RelativeTimeFormat")
}

impl RelativeTimeFormatPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RelativeTimeFormatPrototype> {
        realm.create_object(
            vm,
            RelativeTimeFormatPrototype {
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

        // 18.3.5 Intl.RelativeTimeFormat.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.prototype-toStringTag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.RelativeTimeFormat")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(RelativeTimeFormatPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.format,
            raw_native!(RelativeTimeFormatPrototype::format),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatToParts,
            raw_native!(RelativeTimeFormatPrototype::format_to_parts),
            2,
            attr,
            None,
        );
    }

    // 18.3.2 Intl.RelativeTimeFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.relativetimeformat.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let relativeTimeFormat be the this value.
        // 2. Perform ? RequireInternalSlot(relativeTimeFormat, [[InitializedRelativeTimeFormat]]).
        let relative_time_format = typed_this_relative_time_format(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. For each row of Table 33, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of relativeTimeFormat's internal slot whose name is the Internal Slot value of the current row.
        //     c. Assert: v is not undefined.
        //     d. Perform ! CreateDataPropertyOrThrow(options, p, v).
        options
            .create_data_property_or_throw(
                vm,
                &names.locale,
                Value::from_string(PrimitiveString::create(vm, relative_time_format.locale())),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.style,
                Value::from_string(PrimitiveString::create_from_utf8(
                    vm,
                    relative_time_format.style_string(),
                )),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.numeric,
                Value::from_string(PrimitiveString::create_from_utf8(
                    vm,
                    relative_time_format.numeric_string(),
                )),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.numberingSystem,
                Value::from_string(PrimitiveString::create(vm, relative_time_format.numbering_system())),
            )
            .must();

        // 5. Return options.
        Ok(Value::from_object(options))
    }

    // 18.3.3 Intl.RelativeTimeFormat.prototype.format ( value, unit ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.prototype.format
    fn format(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let relativeTimeFormat be the this value.
        // 2. Perform ? RequireInternalSlot(relativeTimeFormat, [[InitializedRelativeTimeFormat]]).
        let relative_time_format = typed_this_relative_time_format(vm)?;

        // 3. Let value be ? ToNumber(value).
        let value = vm.argument(0).to_number(vm)?;

        // 4. Let unit be ? ToString(unit).
        let unit = vm.argument(1).to_utf16_string(vm)?;

        // 5. Return ? FormatRelativeTime(relativeTimeFormat, value, unit).
        let formatted = format_relative_time(vm, &relative_time_format, value.as_f64(), Utf16View::of_string(&unit))?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 18.3.4 Intl.RelativeTimeFormat.prototype.formatToParts ( value, unit ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.prototype.formatToParts
    fn format_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let relativeTimeFormat be the this value.
        // 2. Perform ? RequireInternalSlot(relativeTimeFormat, [[InitializedRelativeTimeFormat]]).
        let relative_time_format = typed_this_relative_time_format(vm)?;

        // 3. Let value be ? ToNumber(value).
        let value = vm.argument(0).to_number(vm)?;

        // 4. Let unit be ? ToString(unit).
        let unit = vm.argument(1).to_utf16_string(vm)?;

        // 5. Return ? FormatRelativeTimeToParts(relativeTimeFormat, value, unit).
        Ok(Value::from_object(format_relative_time_to_parts(
            vm,
            &relative_time_format,
            value.as_f64(),
            Utf16View::of_string(&unit),
        )?))
    }
}
