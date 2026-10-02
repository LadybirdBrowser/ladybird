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
    LocaleKey, ResolvedOptions, SpecialBehaviors, canonicalize_locale_list, filter_locales, resolve_options,
};
use crate::runtime::intl::relative_time_format::RelativeTimeFormat;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::relative_time_format as unicode_relative_time_format;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct RelativeTimeFormatConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    RelativeTimeFormatConstructor,
    initialize: RelativeTimeFormatConstructor::initialize,
    call: RelativeTimeFormatConstructor::call,
    construct: RelativeTimeFormatConstructor::construct
);

impl RelativeTimeFormatConstructor {
    // 18.1 The Intl.RelativeTimeFormat Constructor, https://tc39.es/ecma402/#sec-intl-relativetimeformat-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RelativeTimeFormatConstructor> {
        realm.create_object(
            vm,
            RelativeTimeFormatConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.RelativeTimeFormat.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 18.2.1 Intl.RelativeTimeFormat.prototype, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_relative_time_format_prototype(vm)),
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
            raw_native!(RelativeTimeFormatConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );
    }

    // 18.1.1 Intl.RelativeTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.RelativeTimeFormat"],
        )
    }

    // 18.1.1 Intl.RelativeTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let relativeTimeFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.RelativeTimeFormat.prototype%", « [[InitializedRelativeTimeFormat]], [[Locale]], [[LocaleData]], [[Style]], [[Numeric]], [[NumberFormat]], [[NumberingSystem]], [[PluralRules]] »).
        let relative_time_format = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_relative_time_format_prototype,
            |prototype| RelativeTimeFormat::new(vm, prototype),
        )?;

        // 3. Let optionsResolution be ? ResolveOptions(%Intl.RelativeTimeFormat%, %Intl.RelativeTimeFormat%.[[LocaleData]], locales, options, « COERCE-OPTIONS »).
        // 4. Set options to optionsResolution.[[Options]].
        // 5. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*relative_time_format,
            locales_value,
            options_value,
            SpecialBehaviors::COERCE_OPTIONS,
            None,
        )?;

        // 6. Let locale be r.[[Locale]].
        let locale = result.locale.clone();

        // 7. Set relativeTimeFormat.[[Locale]] to locale.
        relative_time_format.set_locale(locale);

        // 8. Set relativeTimeFormat.[[LocaleData]] to r.[[LocaleData]].

        // 9. Set relativeTimeFormat.[[NumberingSystem]] to r.[[nu]].
        if let LocaleKey::String(resolved_numbering_system) = result.nu {
            relative_time_format.set_numbering_system(resolved_numbering_system);
        }

        // 10. Let style be ? GetOption(options, "style", STRING, « "long", "short", "narrow" », "long").
        let style = get_option(
            vm,
            &options,
            &names.style,
            OptionType::String,
            &["long", "short", "narrow"],
            OptionDefault::string("long"),
        )?;

        // 11. Set relativeTimeFormat.[[Style]] to style.
        relative_time_format.set_style(style.as_string().utf16_string_view());

        // 12. Let numeric be ? GetOption(options, "numeric", STRING, « "always", "auto" », "always").
        let numeric = get_option(
            vm,
            &options,
            &names.numeric,
            OptionType::String,
            &["always", "auto"],
            OptionDefault::string("always"),
        )?;

        // 13. Set relativeTimeFormat.[[Numeric]] to numeric.
        relative_time_format.set_numeric(numeric.as_string().utf16_string_view());

        // 14. Let nfOptions be OrdinaryObjectCreate(null).
        // 15. Perform ! CreateDataPropertyOrThrow(nfOptions, "numberingSystem", relativeTimeFormat.[[NumberingSystem]]).
        // 16. Let relativeTimeFormat.[[NumberFormat]] be ! Construct(%Intl.NumberFormat%, « locale, nfOptions »).
        // 17. Let relativeTimeFormat.[[PluralRules]] be ! Construct(%Intl.PluralRules%, « locale »).
        let formatter = unicode_relative_time_format::RelativeTimeFormat::create(
            Utf16View::of_string(&result.icu_locale),
            relative_time_format.style(),
        );
        relative_time_format.set_formatter(formatter);

        // 18. Return relativeTimeFormat.
        Ok(relative_time_format.upcast())
    }

    // 18.2.2 Intl.RelativeTimeFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.supportedLocalesOf
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %RelativeTimeFormat%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
