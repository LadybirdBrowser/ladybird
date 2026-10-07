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
use crate::runtime::intl::display_names::{DisplayNames, Type};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct DisplayNamesConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    DisplayNamesConstructor,
    initialize: DisplayNamesConstructor::initialize,
    call: DisplayNamesConstructor::call,
    construct: DisplayNamesConstructor::construct
);

impl DisplayNamesConstructor {
    // 12.1 The Intl.DisplayNames Constructor, https://tc39.es/ecma402/#sec-intl-displaynames-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DisplayNamesConstructor> {
        realm.create_object(
            vm,
            DisplayNamesConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.DisplayNames.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 12.2.1 Intl.DisplayNames.prototype, https://tc39.es/ecma402/#sec-Intl.DisplayNames.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_display_names_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_native_function(
            vm,
            realm,
            &vm.names.supportedLocalesOf,
            raw_native!(DisplayNamesConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 12.1.1 Intl.DisplayNames ( locales, options ), https://tc39.es/ecma402/#sec-Intl.DisplayNames
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.DisplayNames"],
        )
    }

    // 12.1.1 Intl.DisplayNames ( locales, options ), https://tc39.es/ecma402/#sec-Intl.DisplayNames
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let displayNames be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.DisplayNames.prototype%", « [[InitializedDisplayNames]], [[Locale]], [[Style]], [[Type]], [[Fallback]], [[LanguageDisplay]], [[Fields]] »).
        let display_names = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_display_names_prototype,
            |prototype| DisplayNames::new(vm, prototype),
        )?;

        // 3. Let optionsResolution be ? ResolveOptions(%Intl.DisplayNames%, %Intl.DisplayNames%.[[LocaleData]], locales, options, « REQUIRE-OPTIONS »).
        // 4. Set options to optionsResolution.[[Options]].
        // 5. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*display_names,
            locales_value,
            options_value,
            SpecialBehaviors::REQUIRE_OPTIONS,
            None,
        )?;

        // 6. Let style be ? GetOption(options, "style", string, « "narrow", "short", "long" », "long").
        let style = get_option(
            vm,
            &options,
            &names.style,
            OptionType::String,
            &["narrow", "short", "long"],
            OptionDefault::string("long"),
        )?;

        // 7. Set displayNames.[[Style]] to style.
        display_names.set_style(style.as_string().utf16_string_view());

        // 8. Let type be ? GetOption(options, "type", string, « "language", "region", "script", "currency", "calendar", "dateTimeField" », undefined).
        let type_ = get_option(
            vm,
            &options,
            &names.type_,
            OptionType::String,
            &["language", "region", "script", "currency", "calendar", "dateTimeField"],
            OptionDefault::Empty,
        )?;

        // 9. If type is undefined, throw a TypeError exception.
        if type_.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"options.type"]);
        }

        // 10. Set displayNames.[[Type]] to type.
        display_names.set_type(type_.as_string().utf16_string_view());

        // 11. Let fallback be ? GetOption(options, "fallback", string, « "code", "none" », "code").
        let fallback = get_option(
            vm,
            &options,
            &names.fallback,
            OptionType::String,
            &["code", "none"],
            OptionDefault::string("code"),
        )?;

        // 12. Set displayNames.[[Fallback]] to fallback.
        display_names.set_fallback(fallback.as_string().utf16_string_view());

        // 13. Set displayNames.[[Locale]] to r.[[Locale]].
        display_names.set_locale(result.locale);
        display_names.set_icu_locale(result.icu_locale);

        // 14. Let resolvedLocaleData be r.[[LocaleData]].
        // 15. Let types be resolvedLocaleData.[[types]].
        // 16. Assert: types is a Record (see 12.2.3).

        // 17. Let languageDisplay be ? GetOption(options, "languageDisplay", string, « "dialect", "standard" », "dialect").
        let language_display = get_option(
            vm,
            &options,
            &names.languageDisplay,
            OptionType::String,
            &["dialect", "standard"],
            OptionDefault::string("dialect"),
        )?;

        // 18. Let typeFields be types.[[<type>]].
        // 19. Assert: typeFields is a Record (see 12.2.3).

        // 20. If type is "language", then
        if display_names.type_() == Type::Language {
            // a. Set displayNames.[[LanguageDisplay]] to languageDisplay.
            display_names.set_language_display(language_display.as_string().utf16_string_view());

            // b. Set typeFields to typeFields.[[<languageDisplay>]].
            // c. Assert: typeFields is a Record (see 12.2.3).
        }

        // 21. Let styleFields be typeFields.[[<style>]].
        // 22. Assert: styleFields is a Record (see 12.2.3).
        // 23. Set displayNames.[[Fields]] to styleFields.

        // 24. Return displayNames.
        Ok(display_names.upcast())
    }

    // 12.2.2 Intl.DisplayNames.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.DisplayNames.supportedLocalesOf
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %DisplayNames%.[[AvailableLocales]].
        // No-op, availability of each requested locale is checked via Unicode::is_locale_available()

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
