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
use crate::runtime::intl::segmenter::Segmenter;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct SegmenterConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    SegmenterConstructor,
    initialize: SegmenterConstructor::initialize,
    call: SegmenterConstructor::call,
    construct: SegmenterConstructor::construct
);

impl SegmenterConstructor {
    // 19.1 The Intl.Segmenter Constructor, https://tc39.es/ecma402/#sec-intl-segmenter-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SegmenterConstructor> {
        realm.create_object(
            vm,
            SegmenterConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Segmenter.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 19.2.1 Intl.Segmenter.prototype, https://tc39.es/ecma402/#sec-intl.segmenter.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_segmenter_prototype(vm)),
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
            raw_native!(SegmenterConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );
    }

    // 19.1.1 Intl.Segmenter ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.segmenter
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.Segmenter"],
        )
    }

    // 19.1.1 Intl.Segmenter ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.segmenter
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let internalSlotsList be « [[InitializedSegmenter]], [[Locale]], [[SegmenterGranularity]] ».
        // 3. Let segmenter be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.Segmenter.prototype%", internalSlotsList).
        let segmenter = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_segmenter_prototype,
            |prototype| Segmenter::new(vm, prototype),
        )?;

        // 4. Let optionsResolution be ? ResolveOptions(%Intl.Segmenter%, %Intl.Segmenter%.[[LocaleData]], locales, options).
        // 5. Set options to optionsResolution.[[Options]].
        // 6. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*segmenter,
            locales_value,
            options_value,
            SpecialBehaviors::NONE,
            None,
        )?;

        // 7. Set segmenter.[[Locale]] to r.[[locale]].
        segmenter.set_locale(result.locale);

        // 8. Let granularity be ? GetOption(options, "granularity", string, « "grapheme", "word", "sentence" », "grapheme").
        let granularity = get_option(
            vm,
            &options,
            &vm.names.granularity,
            OptionType::String,
            &["grapheme", "word", "sentence"],
            OptionDefault::string("grapheme"),
        )?;

        // 9. Set segmenter.[[SegmenterGranularity]] to granularity.
        segmenter.set_segmenter_granularity(granularity.as_string().utf16_string_view());

        let locale_segmenter = unicode::Segmenter::create(
            Utf16View::of_string(&result.icu_locale),
            segmenter.segmenter_granularity(),
        );
        segmenter.set_segmenter(locale_segmenter);

        // 10. Return segmenter.
        Ok(segmenter.upcast())
    }

    // 19.2.2 Intl.Segmenter.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.segmenter.supportedlocalesof
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %Intl.Segmenter%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
