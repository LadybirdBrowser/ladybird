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
use crate::runtime::intl::display_names::{DisplayNames, Fallback, Type, canonical_code_for_display_names};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

/// 12.3 Properties of the Intl.DisplayNames Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-displaynames-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct DisplayNamesPrototype {
    base: Object,
}

define_object_class!(DisplayNamesPrototype, extends: [Object], methods: {
    initialize: DisplayNamesPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_display_names(vm: &Vm) -> ThrowCompletionOr<Gc<DisplayNames>> {
    typed_this_object::<DisplayNames>(vm, "Intl.DisplayNames")
}

impl DisplayNamesPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DisplayNamesPrototype> {
        realm.create_object(
            vm,
            DisplayNamesPrototype {
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

        // 12.3.4 Intl.DisplayNames.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.displaynames.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.DisplayNames")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(DisplayNamesPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.of,
            raw_native!(DisplayNamesPrototype::of),
            1,
            attr,
            None,
        );
    }

    // 12.3.2 Intl.DisplayNames.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-Intl.DisplayNames.prototype.resolvedOptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let displayNames be this value.
        // 2. Perform ? RequireInternalSlot(displayNames, [[InitializedDisplayNames]]).
        let display_names = typed_this_display_names(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. For each row of Table 18, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of displayNames's internal slot whose name is the Internal Slot value of the current row.
        //     c. If v is not undefined, then
        //         i. Perform ! CreateDataPropertyOrThrow(options, p, v).
        let string = |string: &str| Value::from_string(PrimitiveString::create_from_utf8(vm, string));
        options
            .create_data_property_or_throw(
                vm,
                &names.locale,
                Value::from_string(PrimitiveString::create(vm, display_names.locale())),
            )
            .must();
        options
            .create_data_property_or_throw(vm, &names.style, string(display_names.style_string()))
            .must();
        options
            .create_data_property_or_throw(vm, &names.type_, string(display_names.type_string()))
            .must();
        options
            .create_data_property_or_throw(vm, &names.fallback, string(display_names.fallback_string()))
            .must();

        if display_names.has_language_display() {
            options
                .create_data_property_or_throw(
                    vm,
                    &names.languageDisplay,
                    string(display_names.language_display_string()),
                )
                .must();
        }

        // 5. Return options.
        Ok(Value::from_object(options))
    }

    // 12.3.3 Intl.DisplayNames.prototype.of ( code ), https://tc39.es/ecma402/#sec-Intl.DisplayNames.prototype.of
    fn of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let code = vm.argument(0);

        // 1. Let displayNames be this value.
        // 2. Perform ? RequireInternalSlot(displayNames, [[InitializedDisplayNames]]).
        let display_names = typed_this_display_names(vm)?;

        // 3. Let code be ? ToString(code).
        let code_string = code.to_utf16_string(vm)?;

        // 4. Let code be ? CanonicalCodeForDisplayNames(displayNames.[[Type]], code).
        let code = canonical_code_for_display_names(vm, display_names.type_(), Utf16View::of_string(&code_string))?;
        let code_string = code.as_string().utf16_string();
        let code_view = Utf16View::of_string(&code_string);
        assert!(code_view.is_ascii());

        let locale = display_names.icu_locale();
        let locale_view = Utf16View::of_string(&locale);
        assert!(locale_view.is_ascii());

        // 5. Let fields be displayNames.[[Fields]].
        // 6. If fields has a field [[<code>]], return fields.[[<code>]].
        let result = match display_names.type_() {
            Type::Language => unicode::language_display_name(locale_view, code_view, display_names.language_display()),
            Type::Region => unicode::region_display_name(locale_view, code_view),
            Type::Script => unicode::script_display_name(locale_view, code_view),
            Type::Currency => unicode::currency_display_name(locale_view, code_view, display_names.style()),
            Type::Calendar => unicode::calendar_display_name(locale_view, code_view),
            Type::DateTimeField => unicode::date_time_field_display_name(locale_view, code_view, display_names.style()),
            Type::Invalid => unreachable!("the Intl.DisplayNames constructor sets the type"),
        };

        if let Some(result) = result {
            return Ok(Value::from_string(PrimitiveString::create(vm, result)));
        }

        // 7. If displayNames.[[Fallback]] is "code", return code.
        if display_names.fallback() == Fallback::Code {
            return Ok(code);
        }

        // 8. Return undefined.
        Ok(Value::UNDEFINED)
    }
}
