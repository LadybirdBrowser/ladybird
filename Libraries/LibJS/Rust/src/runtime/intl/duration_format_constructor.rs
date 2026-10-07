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
    LocaleKey, ResolvedOptions, SpecialBehaviors, canonicalize_locale_list, filter_locales, get_number_option,
    resolve_options,
};
use crate::runtime::intl::duration_format::{
    DURATION_INSTANCES_COMPONENTS, DurationFormat, Unit, ValueStyle, get_duration_unit_options,
};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::number_format as unicode;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct DurationFormatConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    DurationFormatConstructor,
    initialize: DurationFormatConstructor::initialize,
    call: DurationFormatConstructor::call,
    construct: DurationFormatConstructor::construct
);

impl DurationFormatConstructor {
    // 13.1 The Intl.DurationFormat Constructor, https://tc39.es/ecma402/#sec-intl-durationformat-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DurationFormatConstructor> {
        realm.create_object(
            vm,
            DurationFormatConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.DurationFormat.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 1.3.1 Intl.DurationFormat.prototype, https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_duration_format_prototype(vm)),
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
            raw_native!(DurationFormatConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );
    }

    // 13.1.1 Intl.DurationFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.DurationFormat"],
        )
    }

    // 13.1.1 Intl.DurationFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let durationFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.DurationFormatPrototype%", « [[InitializedDurationFormat]], [[Locale]], [[NumberingSystem]], [[Style]], [[YearsOptions]], [[MonthsOptions]], [[WeeksOptions]], [[DaysOptions]], [[HoursOptions]], [[MinutesOptions]], [[SecondsOptions]], [[MillisecondsOptions]], [[MicrosecondsOptions]], [[NanosecondsOptions]], [[HourMinuteSeparator]], [[MinuteSecondSeparator]], [[FractionalDigits]] »).
        let duration_format = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_duration_format_prototype,
            |prototype| DurationFormat::new(vm, prototype),
        )?;

        // 3. Let optionsResolution be ? ResolveOptions(%Intl.DurationFormat%, %Intl.DurationFormat%.[[LocaleData]], locales, options).
        // 4. Set options to optionsResolution.[[Options]].
        // 5. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*duration_format,
            locales_value,
            options_value,
            SpecialBehaviors::NONE,
            None,
        )?;

        // 6. Set durationFormat.[[Locale]] to r.[[Locale]].
        duration_format.set_locale(result.locale);

        // 7. Let resolvedLocaleData be r.[[LocaleData]].

        // 8. Let digitalFormat be resolvedLocaleData.[[DigitalFormat]].
        let digital_format = unicode::digital_format(Utf16View::of_string(&result.icu_locale));

        // 9. Set durationFormat.[[HourMinuteSeparator]] to digitalFormat.[[HourMinuteSeparator]].
        duration_format.set_hour_minute_separator(digital_format.hours_minutes_separator);

        // 10. Set durationFormat.[[MinuteSecondSeparator]] to digitalFormat.[[MinuteSecondSeparator]].
        duration_format.set_minute_second_separator(digital_format.minutes_seconds_separator);

        // 11. Set durationFormat.[[NumberingSystem]] to r.[[nu]].
        if let LocaleKey::String(resolved_numbering_system) = result.nu {
            duration_format.set_numbering_system(resolved_numbering_system);
        }

        // 12. Let style be ? GetOption(options, "style", STRING, « "long", "short", "narrow", "digital" », "short").
        let style = get_option(
            vm,
            &options,
            &names.style,
            OptionType::String,
            &["long", "short", "narrow", "digital"],
            OptionDefault::string("short"),
        )?;

        // 13. Set durationFormat.[[Style]] to style.
        duration_format.set_style(style.as_string().utf16_string_view());

        // 14. Let prevStyle be the empty String.
        let mut previous_style: Option<ValueStyle> = None;

        // 15. For each row of Table 20, except the header row, in table order, do
        for duration_instances_component in &DURATION_INSTANCES_COMPONENTS {
            // a. Let slot be the Internal Slot value of the current row.
            let slot = duration_instances_component.set_internal_slot;

            // b. Let unit be the Unit value of the current row.
            let unit = duration_instances_component.unit;

            // c. Let styles be the Styles value of the current row.
            let styles = duration_instances_component.styles;

            // d. Let digitalBase be the Digital Default value of the current row.
            let digital_base = duration_instances_component.digital_default;

            // e. Let unitOptions be ? GetDurationUnitOptions(unit, options, style, styles, digitalBase, prevStyle, digitalFormat.[[TwoDigitHours]]).
            let unit_options = get_duration_unit_options(
                vm,
                unit,
                &options,
                duration_format.style(),
                styles,
                digital_base,
                previous_style,
                digital_format.uses_two_digit_hours,
            )?;

            // f. Set the value of durationFormat's internal slot whose name is slot to unitOptions.
            slot(&duration_format, unit_options);

            // g. If unit is one of "hours", "minutes", "seconds", "milliseconds", or "microseconds", then
            if matches!(
                unit,
                Unit::Hours | Unit::Minutes | Unit::Seconds | Unit::Milliseconds | Unit::Microseconds
            ) {
                // i. Set prevStyle to unitOptions.[[Style]].
                previous_style = Some(unit_options.style);
            }
        }

        // 16. Set durationFormat.[[FractionalDigits]] to ? GetNumberOption(options, "fractionalDigits", 0, 9, undefined).
        let fractional_digits = get_number_option(vm, &options, &names.fractionalDigits, 0, 9, None)?;
        duration_format.set_fractional_digits(fractional_digits.map(|digits| {
            u8::try_from(digits).expect("GetNumberOption returns a number between the minimum and the maximum")
        }));

        // 17. Return durationFormat.
        Ok(duration_format.upcast())
    }

    // 13.2.2 Intl.DurationFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.supportedLocalesOf
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %DurationFormat%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}
