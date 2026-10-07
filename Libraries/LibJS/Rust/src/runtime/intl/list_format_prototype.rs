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
use crate::runtime::intl::list_format::{ListFormat, format_list, format_list_to_parts, string_list_from_iterable};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// 14.3 Properties of the Intl.ListFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-listformat-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct ListFormatPrototype {
    base: Object,
}

define_object_class!(ListFormatPrototype, extends: [Object], methods: {
    initialize: ListFormatPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_list_format(vm: &Vm) -> ThrowCompletionOr<Gc<ListFormat>> {
    typed_this_object::<ListFormat>(vm, "Intl.ListFormat")
}

impl ListFormatPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ListFormatPrototype> {
        realm.create_object(
            vm,
            ListFormatPrototype {
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

        // 14.3.5 Intl.ListFormat.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype-toStringTag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.ListFormat")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(ListFormatPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.format,
            raw_native!(ListFormatPrototype::format),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.formatToParts,
            raw_native!(ListFormatPrototype::format_to_parts),
            1,
            attr,
            None,
        );
    }

    // 14.3.2 Intl.ListFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let lf be the this value.
        // 2. Perform ? RequireInternalSlot(lf, [[InitializedListFormat]]).
        let list_format = typed_this_list_format(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. For each row of Table 25, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of lf's internal slot whose name is the Internal Slot value of the current row.
        //     c. Assert: v is not undefined.
        //     d. Perform ! CreateDataPropertyOrThrow(options, p, v).
        options
            .create_data_property_or_throw(
                vm,
                &names.locale,
                Value::from_string(PrimitiveString::create(vm, list_format.locale())),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.type_,
                Value::from_string(PrimitiveString::create_from_utf8(vm, list_format.type_string())),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.style,
                Value::from_string(PrimitiveString::create_from_utf8(vm, list_format.style_string())),
            )
            .must();

        // 5. Return options.
        Ok(Value::from_object(options))
    }

    // 14.3.3 Intl.ListFormat.prototype.format ( list ), https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype.format
    fn format(vm: &Vm) -> ThrowCompletionOr<Value> {
        let list = vm.argument(0);

        // 1. Let lf be the this value.
        // 2. Perform ? RequireInternalSlot(lf, [[InitializedListFormat]]).
        let list_format = typed_this_list_format(vm)?;

        // 3. Let stringList be ? StringListFromIterable(list).
        let string_list = string_list_from_iterable(vm, list)?;

        // 4. Return ! FormatList(lf, stringList).
        let formatted = format_list(&list_format, &string_list);
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 14.3.4 Intl.ListFormat.prototype.formatToParts ( list ), https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype.formatToParts
    fn format_to_parts(vm: &Vm) -> ThrowCompletionOr<Value> {
        let list = vm.argument(0);

        // 1. Let lf be the this value.
        // 2. Perform ? RequireInternalSlot(lf, [[InitializedListFormat]]).
        let list_format = typed_this_list_format(vm)?;

        // 3. Let stringList be ? StringListFromIterable(list).
        let string_list = string_list_from_iterable(vm, list)?;

        // 4. Return ! FormatListToParts(lf, stringList).
        Ok(Value::from_object(format_list_to_parts(vm, &list_format, &string_list)))
    }
}
