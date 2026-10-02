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
use crate::runtime::abstract_operations::{OptionDefault, OptionType, get_option, ordinary_create_from_constructor_of};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::abstract_operations::{
    ResolvedOptions, SpecialBehaviors, canonicalize_locale_list, filter_locales, resolve_options,
};
use crate::runtime::intl::number_format_constructor::set_number_format_digit_options;
use crate::runtime::intl::plural_rules::PluralRules;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::number_format::{self as unicode, Notation};
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct PluralRulesConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PluralRulesConstructor,
    initialize: PluralRulesConstructor::initialize,
    call: PluralRulesConstructor::call,
    construct: PluralRulesConstructor::construct
);

impl PluralRulesConstructor {
    // 17.1 The Intl.PluralRules Constructor, https://tc39.es/ecma402/#sec-intl-pluralrules-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PluralRulesConstructor> {
        realm.create_object(
            vm,
            PluralRulesConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.PluralRules.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 17.2.1 Intl.PluralRules.prototype, https://tc39.es/ecma402/#sec-intl.pluralrules.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_plural_rules_prototype(vm)),
            PropertyAttributes::new(0),
        );
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_native_function(
            vm,
            realm,
            &vm.names.supportedLocalesOf,
            raw_native!(PluralRulesConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );
    }

    // 17.1.1 Intl.PluralRules ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.pluralrules
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.PluralRules"],
        )
    }

    // 17.1.1 Intl.PluralRules ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.pluralrules
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let pluralRules be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.PluralRules.prototype%", « [[InitializedPluralRules]], [[Locale]], [[Type]], [[Notation]], [[CompactDisplay]], [[MinimumIntegerDigits]], [[MinimumFractionDigits]], [[MaximumFractionDigits]], [[MinimumSignificantDigits]], [[MaximumSignificantDigits]], [[RoundingType]], [[RoundingIncrement]], [[RoundingMode]], [[ComputedRoundingPriority]], [[TrailingZeroDisplay]] »).
        let plural_rules = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_plural_rules_prototype,
            |prototype| PluralRules::new(vm, prototype),
        )?;

        // 3. Let optionsResolution be ? ResolveOptions(%Intl.PluralRules%, %Intl.PluralRules%.[[LocaleData]], locales, options, « COERCE-OPTIONS »).
        // 4. Set options to optionsResolution.[[Options]].
        // 5. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*plural_rules,
            locales_value,
            options_value,
            SpecialBehaviors::COERCE_OPTIONS,
            None,
        )?;

        // 6. Set pluralRules.[[Locale]] to r.[[locale]].
        plural_rules.set_locale(result.locale);

        // 7. Let t be ? GetOption(options, "type", string, « "cardinal", "ordinal" », "cardinal").
        let type_ = get_option(
            vm,
            &options,
            &names.type_,
            OptionType::String,
            &["cardinal", "ordinal"],
            OptionDefault::string("cardinal"),
        )?;

        // 8. Set pluralRules.[[Type]] to t.
        plural_rules.set_type(type_.as_string().utf16_string_view());

        // 9. Let notation be ? GetOption(options, "notation", string, « "standard", "scientific", "engineering", "compact" », "standard").
        let notation = get_option(
            vm,
            &options,
            &names.notation,
            OptionType::String,
            &["standard", "scientific", "engineering", "compact"],
            OptionDefault::string("standard"),
        )?;

        // 10. Set pluralRules.[[Notation]] to notation.
        plural_rules.set_notation(notation.as_string().utf16_string_view());

        // 11. Let compactDisplay be ? GetOption(options, "compactDisplay", string, « "short", "long" », "short").
        let compact_display = get_option(
            vm,
            &options,
            &names.compactDisplay,
            OptionType::String,
            &["short", "long"],
            OptionDefault::string("short"),
        )?;

        // 12. If notation is "compact", then
        if plural_rules.notation() == Notation::Compact {
            // a. Set pluralRules.[[CompactDisplay]] to compactDisplay.
            plural_rules.set_compact_display(compact_display.as_string().utf16_string_view());
        }

        // 13. Perform ? SetNumberFormatDigitOptions(pluralRules, options, 0, 3, notation).
        set_number_format_digit_options(vm, &plural_rules, &options, 0, 3, plural_rules.notation())?;

        // Non-standard, create an ICU number formatter for this Intl object.
        let mut formatter = unicode::NumberFormat::create(
            Utf16View::of_string(&result.icu_locale),
            &plural_rules.display_options(),
            &plural_rules.rounding_options(),
        );

        formatter.create_plural_rules(plural_rules.type_());
        plural_rules.set_formatter(formatter);

        // 14. Return pluralRules.
        Ok(plural_rules.upcast())
    }

    // 17.2.2 Intl.PluralRules.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.pluralrules.supportedlocalesof
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %PluralRules%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
