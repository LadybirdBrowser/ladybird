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
use crate::runtime::intl::list_format::ListFormat;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct ListFormatConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ListFormatConstructor,
    initialize: ListFormatConstructor::initialize,
    call: ListFormatConstructor::call,
    construct: ListFormatConstructor::construct
);

impl ListFormatConstructor {
    // 14.1 The Intl.ListFormat Constructor, https://tc39.es/ecma402/#sec-intl-listformat-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ListFormatConstructor> {
        realm.create_object(
            vm,
            ListFormatConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.ListFormat.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 14.2.1 Intl.ListFormat.prototype, https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_list_format_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_native_function(
            vm,
            realm,
            &vm.names.supportedLocalesOf,
            raw_native!(ListFormatConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 14.1.1 Intl.ListFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.ListFormat
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.ListFormat"],
        )
    }

    // 14.1.1 Intl.ListFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.ListFormat
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let listFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.ListFormat.prototype%", « [[InitializedListFormat]], [[Locale]], [[Type]], [[Style]], [[Templates]] »).
        let list_format = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_list_format_prototype,
            |prototype| ListFormat::new(vm, prototype),
        )?;

        // 3. Let optionsResolution be ? ResolveOptions(%Intl.ListFormat%, %Intl.ListFormat%.[[LocaleData]], locales, options).
        // 4. Set options to optionsResolution.[[Options]].
        // 5. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*list_format,
            locales_value,
            options_value,
            SpecialBehaviors::NONE,
            None,
        )?;

        // 6. Set listFormat.[[Locale]] to r.[[Locale]].
        list_format.set_locale(result.locale);

        // 7. Let type be ? GetOption(options, "type", string, « "conjunction", "disjunction", "unit" », "conjunction").
        let type_ = get_option(
            vm,
            &options,
            &names.type_,
            OptionType::String,
            &["conjunction", "disjunction", "unit"],
            OptionDefault::string("conjunction"),
        )?;

        // 8. Set listFormat.[[Type]] to type.
        list_format.set_type(type_.as_string().utf16_string_view());

        // 9. Let style be ? GetOption(options, "style", string, « "long", "short", "narrow" », "long").
        let style = get_option(
            vm,
            &options,
            &names.style,
            OptionType::String,
            &["long", "short", "narrow"],
            OptionDefault::string("long"),
        )?;

        // 10. Set listFormat.[[Style]] to style.
        list_format.set_style(style.as_string().utf16_string_view());

        // 11. Let resolvedLocaleData be r.[[LocaleData]].
        // 12. Let dataLocaleTypes be resolvedLocaleData.[[<type>]].
        // 13. Set listFormat.[[Templates]] to dataLocaleTypes.[[<style>]].
        let formatter = unicode::ListFormat::create(
            Utf16View::of_string(&result.icu_locale),
            list_format.type_(),
            list_format.style(),
        );
        list_format.set_formatter(formatter);

        // 14. Return listFormat.
        Ok(list_format.upcast())
    }

    // 14.2.2 Intl.ListFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.ListFormat.supportedLocalesOf
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %ListFormat%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
