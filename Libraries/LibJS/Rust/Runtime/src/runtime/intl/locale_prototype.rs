/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::intl::locale::{
    Locale, calendars_of_locale, collations_of_locale, get_locale_variants, hour_cycles_of_locale,
    numbering_systems_of_locale, text_direction_of_locale, time_zones_of_locale, week_info_of_locale,
};
use crate::runtime::native_function::{RawNativeFunctionPointer, raw_native};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

/// 15.3 Properties of the Intl.Locale Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-locale-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct LocalePrototype {
    base: Object,
}

define_object_class!(LocalePrototype, extends: [Object], methods: {
    initialize: LocalePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_locale(vm: &Vm) -> ThrowCompletionOr<Gc<Locale>> {
    typed_this_object::<Locale>(vm, "Intl.Locale")
}

fn string_value(vm: &Vm, string: Utf16String) -> Value {
    Value::from_string(PrimitiveString::create(vm, string))
}

fn optional_string_value(vm: &Vm, string: Option<Utf16String>) -> Value {
    string.map_or(Value::UNDEFINED, |string| string_value(vm, string))
}

impl LocalePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<LocalePrototype> {
        realm.create_object(
            vm,
            LocalePrototype {
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

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function: RawNativeFunctionPointer| {
            object.define_native_function(vm, realm, name, function, 0, attr, None);
        };
        define_native_function(&names.maximize, raw_native!(LocalePrototype::maximize));
        define_native_function(&names.minimize, raw_native!(LocalePrototype::minimize));
        define_native_function(&names.toString, raw_native!(LocalePrototype::to_string));
        define_native_function(&names.getCalendars, raw_native!(LocalePrototype::get_calendars));
        define_native_function(&names.getCollations, raw_native!(LocalePrototype::get_collations));
        define_native_function(&names.getHourCycles, raw_native!(LocalePrototype::get_hour_cycles));
        define_native_function(
            &names.getNumberingSystems,
            raw_native!(LocalePrototype::get_numbering_systems),
        );
        define_native_function(&names.getTimeZones, raw_native!(LocalePrototype::get_time_zones));
        define_native_function(&names.getTextInfo, raw_native!(LocalePrototype::get_text_info));
        define_native_function(&names.getWeekInfo, raw_native!(LocalePrototype::get_week_info));

        // 15.3.16 Intl.Locale.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.locale.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.Locale")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let define_native_accessor = |name: &PropertyKey, getter: RawNativeFunctionPointer| {
            object.define_native_accessor(
                vm,
                realm,
                name,
                getter,
                None,
                PropertyAttributes::new(Attribute::CONFIGURABLE),
            );
        };
        define_native_accessor(&names.baseName, raw_native!(LocalePrototype::base_name));
        define_native_accessor(&names.calendar, raw_native!(LocalePrototype::calendar));
        define_native_accessor(&names.caseFirst, raw_native!(LocalePrototype::case_first));
        define_native_accessor(&names.collation, raw_native!(LocalePrototype::collation));
        define_native_accessor(&names.firstDayOfWeek, raw_native!(LocalePrototype::first_day_of_week));
        define_native_accessor(&names.hourCycle, raw_native!(LocalePrototype::hour_cycle));
        define_native_accessor(&names.language, raw_native!(LocalePrototype::language));
        define_native_accessor(&names.numberingSystem, raw_native!(LocalePrototype::numbering_system));
        define_native_accessor(&names.numeric, raw_native!(LocalePrototype::numeric));
        define_native_accessor(&names.region, raw_native!(LocalePrototype::region));
        define_native_accessor(&names.script, raw_native!(LocalePrototype::script));
        define_native_accessor(&names.variants, raw_native!(LocalePrototype::variants));
    }

    // 15.3.2 get Intl.Locale.prototype.baseName, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.baseName
    fn base_name(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return GetLocaleBaseName(loc.[[Locale]]).
        Ok(string_value(
            vm,
            locale_object.locale_id().language_id.to_utf16_string(),
        ))
    }

    // 15.3.3 get Intl.Locale.prototype.calendar, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.calendar
    fn calendar(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(optional_string_value(vm, locale_object.calendar()))
    }

    // 15.3.4 get Intl.Locale.prototype.caseFirst, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.caseFirst
    fn case_first(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(optional_string_value(vm, locale_object.case_first()))
    }

    // 15.3.5 get Intl.Locale.prototype.collation, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.collation
    fn collation(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(optional_string_value(vm, locale_object.collation()))
    }

    // 15.3.6 get Intl.Locale.prototype.firstDayOfWeek, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.firstDayOfWeek
    fn first_day_of_week(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(optional_string_value(vm, locale_object.first_day_of_week()))
    }

    // 15.3.7 get Intl.Locale.prototype.hourCycle, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.hourCycle
    fn hour_cycle(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(optional_string_value(vm, locale_object.hour_cycle()))
    }

    // 15.3.8 get Intl.Locale.prototype.language, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.language
    fn language(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return GetLocaleLanguage(loc.[[Locale]]).
        let language = locale_object
            .locale_id()
            .language_id
            .language
            .expect("the [[Locale]] of an Intl.Locale has a language");
        Ok(string_value(vm, language))
    }

    // 15.3.9 Intl.Locale.prototype.maximize ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.maximize
    fn maximize(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Let maximal be the result of the Add Likely Subtags algorithm applied to loc.[[Locale]]. If an error is signaled, set maximal to loc.[[Locale]].
        let locale = locale_object.locale();
        let maximal = unicode::add_likely_subtags(Utf16View::of_string(&locale)).unwrap_or(locale);

        // 4. Return ! Construct(%Intl.Locale%, maximal).
        Ok(Value::from_object(Locale::create(vm, realm, locale_object, maximal)))
    }

    // 15.3.10 Intl.Locale.prototype.minimize ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.minimize
    fn minimize(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Let minimal be the result of the Remove Likely Subtags algorithm applied to loc.[[Locale]]. If an error is signaled, set minimal to loc.[[Locale]].
        let locale = locale_object.locale();
        let minimal = unicode::remove_likely_subtags(Utf16View::of_string(&locale)).unwrap_or(locale);

        // 4. Return ! Construct(%Intl.Locale%, minimal).
        Ok(Value::from_object(Locale::create(vm, realm, locale_object, minimal)))
    }

    // 15.3.11 get Intl.Locale.prototype.numberingSystem, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.numberingSystem
    fn numbering_system(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(optional_string_value(vm, locale_object.numbering_system()))
    }

    // 15.3.12 get Intl.Locale.prototype.numeric, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.numeric
    fn numeric(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return loc.[[Numeric]].
        Ok(Value::from_bool(locale_object.numeric()))
    }

    // 15.3.13 get Intl.Locale.prototype.region, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.region
    fn region(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return GetLocaleRegion(loc.[[Locale]]).
        Ok(optional_string_value(vm, locale_object.locale_id().language_id.region))
    }

    // 15.3.14 get Intl.Locale.prototype.script, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.script
    fn script(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return GetLocaleScript(loc.[[Locale]]).
        Ok(optional_string_value(vm, locale_object.locale_id().language_id.script))
    }

    // 15.3.15 Intl.Locale.prototype.toString ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.toString
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return loc.[[Locale]].
        Ok(string_value(vm, locale_object.locale()))
    }

    // 15.3.16 Intl.Locale.prototype.getCalendars ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getCalendars
    fn get_calendars(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(Value::from_object(calendars_of_locale(vm, &locale_object)))
    }

    // 15.3.17 Intl.Locale.prototype.getCollations ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getCollations
    fn get_collations(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(Value::from_object(collations_of_locale(vm, &locale_object)))
    }

    // 15.3.18 Intl.Locale.prototype.getHourCycles ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getHourCycles
    fn get_hour_cycles(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(Value::from_object(hour_cycles_of_locale(vm, &locale_object)))
    }

    // 15.3.19 Intl.Locale.prototype.getNumberingSystems ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getNumberingSystems
    fn get_numbering_systems(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(Value::from_object(numbering_systems_of_locale(vm, &locale_object)))
    }

    // 15.3.20 Intl.Locale.prototype.getTimeZones ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getTimeZones
    fn get_time_zones(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locale_object = typed_this_locale(vm)?;
        Ok(time_zones_of_locale(vm, &locale_object))
    }

    // 15.3.21 Intl.Locale.prototype.getTextInfo ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getTextInfo
    fn get_text_info(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Let info be OrdinaryObjectCreate(%Object.prototype%).
        let info = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. Let dir be TextDirectionOfLocale(loc).
        let direction = text_direction_of_locale(&locale_object);

        // 5. Perform ! CreateDataPropertyOrThrow(info, "direction", dir).
        info.create_data_property_or_throw(
            vm,
            &vm.names.direction,
            Value::from_string(PrimitiveString::create_from_utf8(vm, direction)),
        )
        .must();

        // 6. Return info.
        Ok(Value::from_object(info))
    }

    // 15.3.22 Intl.Locale.prototype.getWeekInfo ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getWeekInfo
    fn get_week_info(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Let info be OrdinaryObjectCreate(%Object.prototype%).
        let info = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. Let wi be WeekInfoOfLocale(loc).
        let week_info = week_info_of_locale(&locale_object);

        // 5. Perform ! CreateDataPropertyOrThrow(info, "firstDay", wi.[[FirstDay]]).
        info.create_data_property_or_throw(vm, &vm.names.firstDay, Value::from_i32(i32::from(week_info.first_day)))
            .must();

        // 6. Perform ! CreateDataPropertyOrThrow(info, "weekend", CreateArrayFromList(wi.[[Weekend]])).
        let weekend_days = MarkedVec::new(vm);
        for day in &week_info.weekend {
            weekend_days.push(Value::from_i32(i32::from(*day)));
        }
        let weekend = Array::create_from_list(vm, realm, &weekend_days);
        info.create_data_property_or_throw(vm, &vm.names.weekend, Value::from_object(weekend))
            .must();

        // 7. Return info.
        Ok(Value::from_object(info))
    }

    // 15.3.23 get Intl.Locale.prototype.variants, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.variants
    fn variants(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let loc be the this value.
        // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
        let locale_object = typed_this_locale(vm)?;

        // 3. Return GetLocaleVariants(loc.[[Locale]]).
        Ok(optional_string_value(
            vm,
            get_locale_variants(&locale_object.locale_id()),
        ))
    }
}
