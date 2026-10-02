/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::OnceLock;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::abstract_operations::{
    available_named_time_zone_identifiers, canonicalize_locale_list, create_array_from_string_list,
};
use crate::runtime::intl::single_unit_identifiers::sanctioned_single_unit_identifiers;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

/// 8 The Intl Object, https://tc39.es/ecma402/#intl-object
#[repr(C)]
#[derive(Trace)]
pub struct Intl {
    base: Object,
}

define_object_class!(Intl, extends: [Object], methods: {
    initialize: Intl::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl Intl {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Intl> {
        realm.create_object(
            vm,
            Intl {
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

        // 8.1.1 Intl[ @@toStringTag ], https://tc39.es/ecma402/#sec-Intl-toStringTag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.Intl.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_intrinsic_accessor(vm, &names.Collator, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().intl_collator_constructor(vm))
        });
        // NB: Intl.DateTimeFormat comes with its builtins, as the global object's Date does.
        object.define_intrinsic_accessor(vm, &names.DisplayNames, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().intl_display_names_constructor(vm))
        });
        // NB: Intl.DurationFormat comes with its builtins.
        object.define_intrinsic_accessor(vm, &names.ListFormat, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().intl_list_format_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.Locale, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().intl_locale_constructor(vm))
        });
        // NB: Intl.NumberFormat, Intl.PluralRules and Intl.RelativeTimeFormat come with their builtins.
        object.define_intrinsic_accessor(vm, &names.Segmenter, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().intl_segmenter_constructor(vm))
        });

        object.define_native_function(
            vm,
            realm,
            &names.getCanonicalLocales,
            raw_native!(Intl::get_canonical_locales),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.supportedValuesOf,
            raw_native!(Intl::supported_values_of),
            1,
            attr,
            None,
        );
    }

    // 8.3.1 Intl.getCanonicalLocales ( locales ), https://tc39.es/ecma402/#sec-intl.getcanonicallocales
    fn get_canonical_locales(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        let locales = vm.argument(0);

        // 1. Let ll be ? CanonicalizeLocaleList(locales).
        let locale_list = canonicalize_locale_list(vm, locales)?;

        // 2. Return CreateArrayFromList(ll).
        Ok(Value::from_object(create_array_from_string_list(
            vm,
            realm,
            &locale_list,
        )))
    }

    // 8.3.2 Intl.supportedValuesOf ( key ), https://tc39.es/ecma402/#sec-intl.supportedvaluesof
    fn supported_values_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let key be ? ToString(key).
        let key = vm.argument(0).to_utf16_string(vm)?;
        let key_view = Utf16View::of_string(&key);

        // 2. If key is "calendar", then
        let list = if key_view == "calendar" {
            // a. Let list be ! AvailableCanonicalCalendars( ).
            unicode::available_calendars()
        }
        // 3. Else if key is "collation", then
        else if key_view == "collation" {
            // a. Let list be ! AvailableCanonicalCollations( ).
            unicode::available_collations()
        }
        // 4. Else if key is "currency", then
        else if key_view == "currency" {
            // a. Let list be ! AvailableCanonicalCurrencies( ).
            unicode::available_currencies()
        }
        // 5. Else if key is "numberingSystem", then
        else if key_view == "numberingSystem" {
            // a. Let list be ! AvailableCanonicalNumberingSystems( ).
            unicode::available_number_systems()
        }
        // 6. Else if key is "timeZone", then
        else if key_view == "timeZone" {
            // a. Let list be ! AvailablePrimaryTimeZoneIdentifiers( ).
            static TIME_ZONES: OnceLock<Vec<Utf16String>> = OnceLock::new();
            TIME_ZONES.get_or_init(available_primary_time_zone_identifiers).clone()
        }
        // 7. Else if key is "unit", then
        else if key_view == "unit" {
            // a. Let list be ! AvailableCanonicalUnits( ).
            sanctioned_single_unit_identifiers()
                .iter()
                .map(|unit| Utf16String::from_utf8(unit))
                .collect()
        }
        // 8. Else,
        else {
            // a. Throw a RangeError exception.
            return vm.throw_completion_with_utf16_message(
                ErrorKind::RangeError,
                ErrorType::IntlInvalidKey.utf16_message(&[key_view]),
            );
        };

        // 9. Return CreateArrayFromList( list ).
        Ok(Value::from_object(create_array_from_string_list(vm, realm, &list)))
    }
}

// 6.5.4 AvailablePrimaryTimeZoneIdentifiers ( ), https://tc39.es/ecma402/#sec-availableprimarytimezoneidentifiers
fn available_primary_time_zone_identifiers() -> Vec<Utf16String> {
    // 1. Let records be AvailableNamedTimeZoneIdentifiers().
    let records = available_named_time_zone_identifiers();

    // 2. Let result be a new empty List.
    let mut result = Vec::new();

    // 3. For each element timeZoneIdentifierRecord of records, do
    for time_zone_identifier_record in records {
        // a. If timeZoneIdentifierRecord.[[Identifier]] is timeZoneIdentifierRecord.[[PrimaryIdentifier]], then
        if time_zone_identifier_record.identifier == time_zone_identifier_record.primary_identifier {
            // i. Append timeZoneIdentifierRecord.[[Identifier]] to result.
            result.push(time_zone_identifier_record.identifier.clone());
        }
    }

    // 4. Return result.
    result
}
