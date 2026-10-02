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
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::abstract_operations::{
    LocaleKey, ResolvedOptions, SpecialBehaviors, StringOrBoolean, canonicalize_locale_list, default_number_option,
    filter_locales, get_boolean_or_string_number_format_option, get_number_option, is_well_formed_currency_code,
    is_well_formed_unit_identifier, resolve_options, throw_option_is_not_valid_value,
};
use crate::runtime::intl::number_format::{ComputedRoundingPriority, NumberFormat, NumberFormatBase, currency_digits};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::number_format::{self as unicode, Notation, NumberFormatStyle, RoundingType};
use crate::utf16::Utf16View;

fn ascii_uppercase_currency_code(currency: Utf16View<'_>) -> Utf16String {
    assert!(currency.length_in_code_units() == 3);

    let mut code = [0u16; 3];
    for (i, code_unit) in code.iter_mut().enumerate() {
        let character = u8::try_from(currency.code_unit_at(i)).expect("a well-formed currency code is ASCII");
        assert!(character.is_ascii_alphabetic());
        *code_unit = u16::from(character.to_ascii_uppercase());
    }

    Utf16String::from_utf16(&code)
}

#[repr(C)]
#[derive(Trace)]
pub struct NumberFormatConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    NumberFormatConstructor,
    initialize: NumberFormatConstructor::initialize,
    call: NumberFormatConstructor::call,
    construct: NumberFormatConstructor::construct
);

impl NumberFormatConstructor {
    // 16.1 The Intl.NumberFormat Constructor, https://tc39.es/ecma402/#sec-intl-numberformat-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<NumberFormatConstructor> {
        realm.create_object(
            vm,
            NumberFormatConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.NumberFormat.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 16.2.1 Intl.NumberFormat.prototype, https://tc39.es/ecma402/#sec-intl.numberformat.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_number_format_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.supportedLocalesOf,
            raw_native!(NumberFormatConstructor::supported_locales_of),
            1,
            attr,
            None,
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 16.1.1 Intl.NumberFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.numberformat
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object, else let newTarget be NewTarget.
        let number_format = Self::construct(function, vm, function.as_function_object_gc())?;
        Ok(Value::from_object(number_format))
    }

    // 16.1.1 Intl.NumberFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.numberformat
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let locales_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let numberFormat be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.NumberFormat.prototype%", « [[InitializedNumberFormat]], [[Locale]], [[LocaleData]], [[NumberingSystem]], [[Style]], [[Unit]], [[UnitDisplay]], [[Currency]], [[CurrencyDisplay]], [[CurrencySign]], [[MinimumIntegerDigits]], [[MinimumFractionDigits]], [[MaximumFractionDigits]], [[MinimumSignificantDigits]], [[MaximumSignificantDigits]], [[RoundingType]], [[Notation]], [[CompactDisplay]], [[UseGrouping]], [[SignDisplay]], [[RoundingIncrement]], [[RoundingMode]], [[ComputedRoundingPriority]], [[TrailingZeroDisplay]], [[BoundFormat]] »).
        let number_format = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_number_format_prototype,
            |prototype| NumberFormat::new(vm, prototype),
        )?;

        // 3. Let optionsResolution be ? ResolveOptions(%Intl.NumberFormat%, %Intl.NumberFormat%.[[LocaleData]], locales, options, « COERCE-OPTIONS »).
        // 4. Set options to optionsResolution.[[Options]].
        // 5. Let r be optionsResolution.[[ResolvedLocale]].
        let ResolvedOptions {
            options,
            resolved_locale: result,
            ..
        } = resolve_options(
            vm,
            &*number_format,
            locales_value,
            options_value,
            SpecialBehaviors::COERCE_OPTIONS,
            None,
        )?;

        // 6. Set numberFormat.[[Locale]] to r.[[Locale]].
        number_format.set_locale(result.locale);

        // 7. Set numberFormat.[[LocaleData]] to r.[[LocaleData]].

        // 8. Set numberFormat.[[NumberingSystem]] to r.[[nu]].
        if let LocaleKey::String(resolved_numbering_system) = result.nu {
            number_format.set_numbering_system(resolved_numbering_system);
        }

        // 9. Perform ? SetNumberFormatUnitOptions(numberFormat, options).
        set_number_format_unit_options(vm, &number_format, &options)?;

        // 10. Let style be numberFormat.[[Style]].
        let style = number_format.style();

        // 11. Let notation be ? GetOption(options, "notation", STRING, « "standard", "scientific", "engineering", "compact" », "standard").
        let notation = get_option(
            vm,
            &options,
            &names.notation,
            OptionType::String,
            &["standard", "scientific", "engineering", "compact"],
            OptionDefault::string("standard"),
        )?;

        // 12. Set numberFormat.[[Notation]] to notation.
        number_format.set_notation(notation.as_string().utf16_string_view());

        // 13. If style is "currency" and notation is "standard", then
        let (default_min_fraction_digits, default_max_fraction_digits) =
            if style == NumberFormatStyle::Currency && number_format.notation() == Notation::Standard {
                // a. Let currency be numberFormat.[[Currency]].
                let currency = number_format.currency();

                // b. Let cDigits be CurrencyDigits(currency).
                let digits = currency_digits(Utf16View::of_string(&currency));

                // c. Let mnfdDefault be cDigits.
                // d. Let mxfdDefault be cDigits.
                (digits, digits)
            }
            // 14. Else,
            else {
                // a. Let mnfdDefault be 0.
                // b. If style is "percent", then
                //     i. Let mxfdDefault be 0.
                // c. Else,
                //     i. Let mxfdDefault be 3.
                (0, if style == NumberFormatStyle::Percent { 0 } else { 3 })
            };

        // 15. Perform ? SetNumberFormatDigitOptions(numberFormat, options, mnfdDefault, mxfdDefault, notation).
        set_number_format_digit_options(
            vm,
            &number_format,
            &options,
            default_min_fraction_digits,
            default_max_fraction_digits,
            number_format.notation(),
        )?;

        // 16. Let compactDisplay be ? GetOption(options, "compactDisplay", STRING, « "short", "long" », "short").
        let compact_display = get_option(
            vm,
            &options,
            &names.compactDisplay,
            OptionType::String,
            &["short", "long"],
            OptionDefault::string("short"),
        )?;

        // 17. Let defaultUseGrouping be "auto".
        let mut default_use_grouping = "auto";

        // 18. If notation is "compact", then
        if number_format.notation() == Notation::Compact {
            // a. Set numberFormat.[[CompactDisplay]] to compactDisplay.
            number_format.set_compact_display(compact_display.as_string().utf16_string_view());

            // b. Set defaultUseGrouping to "min2".
            default_use_grouping = "min2";
        }

        // 19. NOTE: For historical reasons, the strings "true" and "false" are accepted and replaced with the default value.
        // 20. Let useGrouping be ? GetBooleanOrStringNumberFormatOption(options, "useGrouping", « "min2", "auto", "always", "true", "false" », defaultUseGrouping).
        let mut use_grouping = get_boolean_or_string_number_format_option(
            vm,
            &options,
            &names.useGrouping,
            &["min2", "auto", "always", "true", "false"],
            StringOrBoolean::String(default_use_grouping),
        )?;

        // 21. If useGrouping is "true" or useGrouping is "false", set useGrouping to defaultUseGrouping.
        if let StringOrBoolean::String(use_grouping_string) = use_grouping
            && (use_grouping_string == "true" || use_grouping_string == "false")
        {
            use_grouping = StringOrBoolean::String(default_use_grouping);
        }

        // 22. If useGrouping is true, set useGrouping to "always".
        if use_grouping == StringOrBoolean::Boolean(true) {
            use_grouping = StringOrBoolean::String("always");
        }

        // 23. Set numberFormat.[[UseGrouping]] to useGrouping.
        number_format.set_use_grouping(use_grouping);

        // 24. Let signDisplay be ? GetOption(options, "signDisplay", STRING, « "auto", "never", "always", "exceptZero", "negative" », "auto").
        let sign_display = get_option(
            vm,
            &options,
            &names.signDisplay,
            OptionType::String,
            &["auto", "never", "always", "exceptZero", "negative"],
            OptionDefault::string("auto"),
        )?;

        // 25. Set numberFormat.[[SignDisplay]] to signDisplay.
        number_format.set_sign_display(sign_display.as_string().utf16_string_view());

        // 26. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
        //     a. Let this be the this value.
        //     b. Return ? ChainNumberFormat(numberFormat, NewTarget, this).

        // Non-standard, create an ICU number formatter for this Intl object.
        let formatter = unicode::NumberFormat::create(
            Utf16View::of_string(&result.icu_locale),
            &number_format.display_options(),
            &number_format.rounding_options(),
        );
        number_format.set_formatter(formatter);

        // 27. Return numberFormat.
        Ok(number_format.upcast())
    }

    // 16.2.2 Intl.NumberFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.numberformat.supportedlocalesof
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %NumberFormat%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}

// 16.1.2 SetNumberFormatDigitOptions ( intlObj, options, mnfdDefault, mxfdDefault, notation ), https://tc39.es/ecma402/#sec-setnfdigitoptions
pub fn set_number_format_digit_options(
    vm: &Vm,
    intl_object: &NumberFormatBase,
    options: &Object,
    default_min_fraction_digits: i32,
    mut default_max_fraction_digits: i32,
    notation: Notation,
) -> ThrowCompletionOr<()> {
    let names = &vm.names;

    // 1. Let mnid be ? GetNumberOption(options, "minimumIntegerDigits", 1, 21, 1).
    let min_integer_digits = get_number_option(vm, options, &names.minimumIntegerDigits, 1, 21, Some(1))?
        .expect("GetNumberOption with a fallback returns a number");

    // 2. Let mnfd be ? Get(options, "minimumFractionDigits").
    let min_fraction_digits = options.get(vm, &names.minimumFractionDigits)?;

    // 3. Let mxfd be ? Get(options, "maximumFractionDigits").
    let max_fraction_digits = options.get(vm, &names.maximumFractionDigits)?;

    // 4. Let mnsd be ? Get(options, "minimumSignificantDigits").
    let min_significant_digits = options.get(vm, &names.minimumSignificantDigits)?;

    // 5. Let mxsd be ? Get(options, "maximumSignificantDigits").
    let max_significant_digits = options.get(vm, &names.maximumSignificantDigits)?;

    // 6. Set intlObj.[[MinimumIntegerDigits]] to mnid.
    intl_object.set_min_integer_digits(min_integer_digits);

    // 7. Let roundingIncrement be ? GetNumberOption(options, "roundingIncrement", 1, 5000, 1).
    let rounding_increment = get_number_option(vm, options, &names.roundingIncrement, 1, 5000, Some(1))?
        .expect("GetNumberOption with a fallback returns a number");

    // 8. If roundingIncrement is not in « 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 », throw a RangeError exception.
    const SANCTIONED_ROUNDING_INCREMENTS: [i32; 15] =
        [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000];

    if !SANCTIONED_ROUNDING_INCREMENTS.contains(&rounding_increment) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IntlInvalidRoundingIncrement,
            &[&rounding_increment],
        );
    }

    // 9. Let roundingMode be ? GetOption(options, "roundingMode", STRING, « "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" », "halfExpand").
    let rounding_mode = get_option(
        vm,
        options,
        &names.roundingMode,
        OptionType::String,
        &[
            "ceil",
            "floor",
            "expand",
            "trunc",
            "halfCeil",
            "halfFloor",
            "halfExpand",
            "halfTrunc",
            "halfEven",
        ],
        OptionDefault::string("halfExpand"),
    )?;

    // 10. Let roundingPriority be ? GetOption(options, "roundingPriority", STRING, « "auto", "morePrecision", "lessPrecision" », "auto").
    let rounding_priority_option = get_option(
        vm,
        options,
        &names.roundingPriority,
        OptionType::String,
        &["auto", "morePrecision", "lessPrecision"],
        OptionDefault::string("auto"),
    )?;
    let rounding_priority_string = rounding_priority_option.as_string().utf16_string();
    let rounding_priority = Utf16View::of_string(&rounding_priority_string);

    // 11. Let trailingZeroDisplay be ? GetOption(options, "trailingZeroDisplay", STRING, « "auto", "stripIfInteger" », "auto").
    let trailing_zero_display = get_option(
        vm,
        options,
        &names.trailingZeroDisplay,
        OptionType::String,
        &["auto", "stripIfInteger"],
        OptionDefault::string("auto"),
    )?;

    // 12. NOTE: All fields required by SetNumberFormatDigitOptions have now been read from options. The remainder of this AO interprets the options and may throw exceptions.

    // 13. If roundingIncrement is not 1, set mxfdDefault to mnfdDefault.
    if rounding_increment != 1 {
        default_max_fraction_digits = default_min_fraction_digits;
    }

    // 14. Set intlObj.[[RoundingIncrement]] to roundingIncrement.
    intl_object.set_rounding_increment(rounding_increment);

    // 15. Set intlObj.[[RoundingMode]] to roundingMode.
    intl_object.set_rounding_mode(rounding_mode.as_string().utf16_string_view());

    // 16. Set intlObj.[[TrailingZeroDisplay]] to trailingZeroDisplay.
    intl_object.set_trailing_zero_display(trailing_zero_display.as_string().utf16_string_view());

    // 17. If mnsd is undefined and mxsd is undefined, let hasSd be false. Otherwise, let hasSd be true.
    let has_significant_digits = !min_significant_digits.is_undefined() || !max_significant_digits.is_undefined();

    // 18. If mnfd is undefined and mxsd is undefined, let hasFd be false. Otherwise, let hasFd be true.
    let has_fraction_digits = !min_fraction_digits.is_undefined() || !max_fraction_digits.is_undefined();

    // 19. Let needSd be true.
    let mut need_significant_digits = true;

    // 20. Let needFd be true.
    let mut need_fraction_digits = true;

    // 21. If roundingPriority is "auto", then
    if rounding_priority == "auto" {
        // a. Set needSd to hasSd.
        need_significant_digits = has_significant_digits;

        // b. If needSd is true, or hasFd is false and notation is "compact", then
        if need_significant_digits || (!has_fraction_digits && notation == Notation::Compact) {
            // i. Set needFd to false.
            need_fraction_digits = false;
        }
    }

    // 22. If needSd is true, then
    if need_significant_digits {
        // a. If hasSd is true, then
        if has_significant_digits {
            // i. Set intlObj.[[MinimumSignificantDigits]] to ? DefaultNumberOption(mnsd, 1, 21, 1).
            let min_digits = default_number_option(vm, min_significant_digits, 1, 21, Some(1))?
                .expect("DefaultNumberOption with a fallback returns a number");
            intl_object.set_min_significant_digits(min_digits);

            // ii. Set intlObj.[[MaximumSignificantDigits]] to ? DefaultNumberOption(mxsd, intlObj.[[MinimumSignificantDigits]], 21, 21).
            let max_digits = default_number_option(vm, max_significant_digits, min_digits, 21, Some(21))?
                .expect("DefaultNumberOption with a fallback returns a number");
            intl_object.set_max_significant_digits(max_digits);
        }
        // b. Else,
        else {
            // i. Set intlObj.[[MinimumSignificantDigits]] to 1.
            intl_object.set_min_significant_digits(1);

            // ii. Set intlObj.[[MaximumSignificantDigits]] to 21.
            intl_object.set_max_significant_digits(21);
        }
    }

    // 23. If needFd is true, then
    if need_fraction_digits {
        // a. If hasFd is true, then
        if has_fraction_digits {
            // i. Set mnfd to ? DefaultNumberOption(mnfd, 0, 100, undefined).
            let mut min_digits = default_number_option(vm, min_fraction_digits, 0, 100, None)?;

            // ii. Set mxfd to ? DefaultNumberOption(mxfd, 0, 100, undefined).
            let mut max_digits = default_number_option(vm, max_fraction_digits, 0, 100, None)?;

            match (min_digits, max_digits) {
                // iii. If mnfd is undefined, set mnfd to min(mnfdDefault, mxfd).
                (None, max) => {
                    min_digits = Some(default_min_fraction_digits.min(max.expect("hasFd is true")));
                }
                // iv. Else if mxfd is undefined, set mxfd to max(mxfdDefault, mnfd).
                (Some(min), None) => max_digits = Some(default_max_fraction_digits.max(min)),
                // v. Else if mnfd is greater than mxfd, throw a RangeError exception.
                (Some(min), Some(max)) if min > max => {
                    return vm.throw_completion(
                        ErrorKind::RangeError,
                        ErrorType::IntlMinimumExceedsMaximum,
                        &[&min, &max],
                    );
                }
                (Some(_), Some(_)) => {}
            }

            // vi. Set intlObj.[[MinimumFractionDigits]] to mnfd.
            intl_object.set_min_fraction_digits(min_digits.expect("mnfd is set"));

            // vii. Set intlObj.[[MaximumFractionDigits]] to mxfd.
            intl_object.set_max_fraction_digits(max_digits.expect("mxfd is set"));
        }
        // b. Else,
        else {
            // i. Set intlObj.[[MinimumFractionDigits]] to mnfdDefault.
            intl_object.set_min_fraction_digits(default_min_fraction_digits);

            // ii. Set intlObj.[[MaximumFractionDigits]] to mxfdDefault.
            intl_object.set_max_fraction_digits(default_max_fraction_digits);
        }
    }

    // 24. If needSd is false and needFd is false, then
    if !need_significant_digits && !need_fraction_digits {
        // a. Set intlObj.[[MinimumFractionDigits]] to 0.
        intl_object.set_min_fraction_digits(0);

        // b. Set intlObj.[[MaximumFractionDigits]] to 0.
        intl_object.set_max_fraction_digits(0);

        // c. Set intlObj.[[MinimumSignificantDigits]] to 1.
        intl_object.set_min_significant_digits(1);

        // d. Set intlObj.[[MaximumSignificantDigits]] to 2.
        intl_object.set_max_significant_digits(2);

        // e. Set intlObj.[[RoundingType]] to MORE-PRECISION.
        intl_object.set_rounding_type(RoundingType::MorePrecision);

        // f. Set intlObj.[[ComputedRoundingPriority]] to "morePrecision".
        intl_object.set_computed_rounding_priority(ComputedRoundingPriority::MorePrecision);
    }
    // 25. Else if roundingPriority is "morePrecision", then
    else if rounding_priority == "morePrecision" {
        // a. Set intlObj.[[RoundingType]] to MORE-PRECISION.
        intl_object.set_rounding_type(RoundingType::MorePrecision);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "morePrecision".
        intl_object.set_computed_rounding_priority(ComputedRoundingPriority::MorePrecision);
    }
    // 26. Else if roundingPriority is "lessPrecision", then
    else if rounding_priority == "lessPrecision" {
        // a. Set intlObj.[[RoundingType]] to LESS-PRECISION.
        intl_object.set_rounding_type(RoundingType::LessPrecision);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "lessPrecision".
        intl_object.set_computed_rounding_priority(ComputedRoundingPriority::LessPrecision);
    }
    // 27. Else if hasSd is true, then
    else if has_significant_digits {
        // a. Set intlObj.[[RoundingType]] to SIGNIFICANT-DIGITS.
        intl_object.set_rounding_type(RoundingType::SignificantDigits);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "auto".
        intl_object.set_computed_rounding_priority(ComputedRoundingPriority::Auto);
    }
    // 28. Else,
    else {
        // a. Set intlObj.[[RoundingType]] to FRACTION-DIGITS.
        intl_object.set_rounding_type(RoundingType::FractionDigits);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "auto".
        intl_object.set_computed_rounding_priority(ComputedRoundingPriority::Auto);
    }

    // 29. If roundingIncrement is not 1, then
    if rounding_increment != 1 {
        // a. If intlObj.[[RoundingType]] is not FRACTION-DIGITS, throw a TypeError exception.
        if intl_object.rounding_type() != RoundingType::FractionDigits {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IntlInvalidRoundingIncrementForRoundingType,
                &[&rounding_increment, &intl_object.rounding_type_string()],
            );
        }

        // b. If intlObj.[[MaximumFractionDigits]] is not intlObj.[[MinimumFractionDigits]], throw a RangeError exception.
        if intl_object.max_fraction_digits() != intl_object.min_fraction_digits() {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::IntlInvalidRoundingIncrementForFractionDigits,
                &[&rounding_increment],
            );
        }
    }

    // 30. Return UNUSED.
    Ok(())
}

// 16.1.3 SetNumberFormatUnitOptions ( intlObj, options ), https://tc39.es/ecma402/#sec-setnumberformatunitoptions
pub fn set_number_format_unit_options(vm: &Vm, intl_object: &NumberFormat, options: &Object) -> ThrowCompletionOr<()> {
    let names = &vm.names;

    // 1. Let style be ? GetOption(options, "style", STRING, « "decimal", "percent", "currency", "unit" », "decimal").
    let style = get_option(
        vm,
        options,
        &names.style,
        OptionType::String,
        &["decimal", "percent", "currency", "unit"],
        OptionDefault::string("decimal"),
    )?;

    // 2. Set intlObj.[[Style]] to style.
    intl_object.set_style(style.as_string().utf16_string_view());

    // 3. Let currency be ? GetOption(options, "currency", STRING, EMPTY, undefined).
    let currency = get_option(
        vm,
        options,
        &names.currency,
        OptionType::String,
        &[],
        OptionDefault::Empty,
    )?;

    // 4. If currency is undefined, then
    if currency.is_undefined() {
        // a. If style is "currency", throw a TypeError exception.
        if intl_object.style() == NumberFormatStyle::Currency {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IntlOptionUndefined,
                &[&"currency", &"style", &style],
            );
        }
    }
    // 5. Else,
    //     a. If IsWellFormedCurrencyCode(currency) is false, throw a RangeError exception.
    else if !is_well_formed_currency_code(currency.as_string().utf16_string_view()) {
        let currency_string = currency.as_string().utf16_string();
        return throw_option_is_not_valid_value(vm, Utf16View::of_string(&currency_string), &names.currency);
    }

    // 6. Let currencyDisplay be ? GetOption(options, "currencyDisplay", STRING, « "code", "symbol", "narrowSymbol", "name" », "symbol").
    let currency_display = get_option(
        vm,
        options,
        &names.currencyDisplay,
        OptionType::String,
        &["code", "symbol", "narrowSymbol", "name"],
        OptionDefault::string("symbol"),
    )?;

    // 7. Let currencySign be ? GetOption(options, "currencySign", STRING, « "standard", "accounting" », "standard").
    let currency_sign = get_option(
        vm,
        options,
        &names.currencySign,
        OptionType::String,
        &["standard", "accounting"],
        OptionDefault::string("standard"),
    )?;

    // 8. Let unit be ? GetOption(options, "unit", STRING, EMPTY, undefined).
    let unit = get_option(vm, options, &names.unit, OptionType::String, &[], OptionDefault::Empty)?;

    // 9. If unit is undefined, then
    if unit.is_undefined() {
        // a. If style is "unit", throw a TypeError exception.
        if intl_object.style() == NumberFormatStyle::Unit {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IntlOptionUndefined,
                &[&"unit", &"style", &style],
            );
        }
    }
    // 10. Else,
    //     a. If IsWellFormedUnitIdentifier(unit) is false, throw a RangeError exception.
    else if !is_well_formed_unit_identifier(unit.as_string().utf16_string_view()) {
        let unit_string = unit.as_string().utf16_string();
        return throw_option_is_not_valid_value(vm, Utf16View::of_string(&unit_string), &names.unit);
    }

    // 11. Let unitDisplay be ? GetOption(options, "unitDisplay", STRING, « "short", "narrow", "long" », "short").
    let unit_display = get_option(
        vm,
        options,
        &names.unitDisplay,
        OptionType::String,
        &["short", "narrow", "long"],
        OptionDefault::string("short"),
    )?;

    // 12. If style is "currency", then
    if intl_object.style() == NumberFormatStyle::Currency {
        // a. Set intlObj.[[Currency]] to the ASCII-uppercase of currency.
        intl_object.set_currency(ascii_uppercase_currency_code(currency.as_string().utf16_string_view()));

        // c. Set intlObj.[[CurrencyDisplay]] to currencyDisplay.
        intl_object.set_currency_display(currency_display.as_string().utf16_string_view());

        // d. Set intlObj.[[CurrencySign]] to currencySign.
        intl_object.set_currency_sign(currency_sign.as_string().utf16_string_view());
    }

    // 13. If style is "unit", then
    if intl_object.style() == NumberFormatStyle::Unit {
        // a. Set intlObj.[[Unit]] to unit.
        intl_object.set_unit(unit.as_string().utf16_string());

        // b. Set intlObj.[[UnitDisplay]] to unitDisplay.
        intl_object.set_unit_display(unit_display.as_string().utf16_string_view());
    }

    // 14. Return UNUSED.
    Ok(())
}
