/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{OptionDefault, OptionType, get_option, ordinary_create_from_constructor_of};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::abstract_operations::{
    LocaleKey, SpecialBehaviors, canonicalize_locale_list, coerce_options_to_object, create_array_from_string_list,
    filter_locales, resolve_options,
};
use crate::runtime::intl::collator::Collator;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::intl::{self as unicode, Usage};
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct CollatorConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    CollatorConstructor,
    initialize: CollatorConstructor::initialize,
    call: CollatorConstructor::call,
    construct: CollatorConstructor::construct
);

impl CollatorConstructor {
    // 10.1 The Intl.Collator Constructor, https://tc39.es/ecma402/#sec-the-intl-collator-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<CollatorConstructor> {
        realm.create_object(
            vm,
            CollatorConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Collator.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 10.2.1 Intl.Collator.prototype, https://tc39.es/ecma402/#sec-intl.collator.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_collator_prototype(vm)),
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
            raw_native!(CollatorConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );
    }

    // 10.1.1 Intl.Collator ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.collator
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object, else let newTarget be NewTarget
        let collator = Self::construct(function, vm, function.as_function_object_gc())?;
        Ok(Value::from_object(collator))
    }

    // 10.1.1 Intl.Collator ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.collator
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let internalSlotsList be « [[InitializedCollator]], [[Locale]], [[Usage]], [[Collation]], [[Numeric]], [[CaseFirst]], [[Sensitivity]], [[IgnorePunctuation]], [[BoundCompare]] ».
        // 3. Let collator be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.Collator.prototype%", internalSlotsList).
        let collator = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_collator_prototype,
            |prototype| Collator::new(vm, prototype),
        )?;

        // 4. NOTE: The source of locale data for ResolveOptions depends upon the "usage" property of options, but the following
        //    two steps must observably precede that lookup (and must not observably repeat inside ResolveOptions).

        // 5. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales_value)?;

        // 6. Set options to ? CoerceOptionsToObject(options).
        let options = coerce_options_to_object(vm, options_value)?;

        // 7. Let usage be ? GetOption(options, "usage", string, « "sort", "search" », "sort").
        let usage = get_option(
            vm,
            &options,
            &names.usage,
            OptionType::String,
            &["sort", "search"],
            OptionDefault::string("sort"),
        )?;

        // 8. Set collator.[[Usage]] to usage.
        collator.set_usage(usage.as_string().utf16_string_view());

        // 9. If usage is "sort", then
        //     a. Let localeData be %Intl.Collator%.[[SortLocaleData]].
        // 10. Else,
        //     a. Let localeData be %Intl.Collator%.[[SearchLocaleData]].

        // 11. Let optionsResolution be ? ResolveOptions(%Intl.Collator%, localeData, CreateArrayFromList(requestedLocales), options).
        let requested_locales_array = create_array_from_string_list(vm, realm, &requested_locales);
        let options_resolution = resolve_options(
            vm,
            &*collator,
            Value::from_object(requested_locales_array),
            options_value,
            SpecialBehaviors::NONE,
            None,
        )?;

        // 12. Let r be optionsResolution.[[ResolvedLocale]].
        let result = options_resolution.resolved_locale;

        // 13. Set collator.[[Locale]] to r.[[Locale]].
        collator.set_locale(result.locale);

        // 14. If r.[[co]] is null, let collation be "default". Otherwise, let collation be r.[[co]].
        let collation = match result.co {
            LocaleKey::Empty => Utf16String::from_utf8("default"),
            LocaleKey::String(collation) => collation,
        };

        // 15. Set collator.[[Collation]] to collation.
        collator.set_collation(collation);

        // 16. Set collator.[[Numeric]] to SameValue(r.[[kn]], "true").
        collator.set_numeric(matches!(&result.kn, LocaleKey::String(kn) if Utf16View::of_string(kn) == "true"));

        // 17. Set collator.[[CaseFirst]] to r.[[kf]].
        if let LocaleKey::String(resolved_case_first) = &result.kf {
            collator.set_case_first(Utf16View::of_string(resolved_case_first));
        }

        // 18. Let resolvedLocaleData be r.[[LocaleData]].

        // 19. If usage is "sort", let defaultSensitivity be "variant". Otherwise, let defaultSensitivity be resolvedLocaleData.[[sensitivity]].
        // NOTE: We do not acquire resolvedLocaleData.[[sensitivity]] here. Instead, we let LibUnicode fill in the
        //       default value if an override was not provided here.
        let default_sensitivity = if collator.usage() == Usage::Sort {
            OptionDefault::string("variant")
        } else {
            OptionDefault::Empty
        };

        // 20. Set collator.[[Sensitivity]] to ? GetOption(options, "sensitivity", string, « "base", "accent", "case", "variant" », defaultSensitivity).
        let sensitivity_value = get_option(
            vm,
            &options,
            &names.sensitivity,
            OptionType::String,
            &["base", "accent", "case", "variant"],
            default_sensitivity,
        )?;

        let sensitivity = (!sensitivity_value.is_undefined())
            .then(|| unicode::sensitivity_from_string(sensitivity_value.as_string().utf16_string_view()));

        // 21. Let defaultIgnorePunctuation be resolvedLocaleData.[[ignorePunctuation]].
        // NOTE: We do not acquire resolvedLocaleData.[[ignorePunctuation]] here. Instead, we let LibUnicode fill in the
        //       default value if an override was not provided here.

        // 22. Set collator.[[IgnorePunctuation]] to ? GetOption(options, "ignorePunctuation", boolean, empty, defaultIgnorePunctuation).
        let ignore_punctuation_value = get_option(
            vm,
            &options,
            &names.ignorePunctuation,
            OptionType::Boolean,
            &[],
            OptionDefault::Empty,
        )?;

        let ignore_punctuation = (!ignore_punctuation_value.is_undefined()).then(|| ignore_punctuation_value.as_bool());

        // Non-standard, create an ICU collator for this Intl object.
        let collation = collator.collation();
        let icu_collator = unicode::Collator::create(
            Utf16View::of_string(&result.icu_locale),
            collator.usage(),
            Utf16View::of_string(&collation),
            sensitivity,
            collator.case_first(),
            collator.numeric(),
            ignore_punctuation,
        );
        collator.set_collator(icu_collator);

        collator.set_sensitivity(collator.with_collator(unicode::Collator::sensitivity));
        collator.set_ignore_punctuation(collator.with_collator(unicode::Collator::ignore_punctuation));

        // 23. Return collator.
        Ok(collator.upcast())
    }

    // 10.2.2 Intl.Collator.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.collator.supportedlocalesof
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %Collator%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
