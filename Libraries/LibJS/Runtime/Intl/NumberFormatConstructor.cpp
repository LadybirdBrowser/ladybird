/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(NumberFormatConstructor);

// 16.1 The Intl.NumberFormat Constructor, https://tc39.es/ecma402/#sec-intl-numberformat-constructor
NumberFormatConstructor::NumberFormatConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.NumberFormat.as_string(), realm.intrinsics().function_prototype())
{
}

void NumberFormatConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 16.2.1 Intl.NumberFormat.prototype, https://tc39.es/ecma402/#sec-intl.numberformat.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_number_format_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 16.1.1 Intl.NumberFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.numberformat
ThrowCompletionOr<Value> NumberFormatConstructor::call()
{
    // 1. If NewTarget is undefined, let newTarget be the active function object, else let newTarget be NewTarget.
    return TRY(construct(*this));
}

// 16.1.1 Intl.NumberFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.numberformat
ThrowCompletionOr<GC::Ref<Object>> NumberFormatConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let numberFormat be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.NumberFormat.prototype%", « [[InitializedNumberFormat]], [[Locale]], [[LocaleData]], [[NumberingSystem]], [[Style]], [[Unit]], [[UnitDisplay]], [[Currency]], [[CurrencyDisplay]], [[CurrencySign]], [[MinimumIntegerDigits]], [[MinimumFractionDigits]], [[MaximumFractionDigits]], [[MinimumSignificantDigits]], [[MaximumSignificantDigits]], [[RoundingType]], [[Notation]], [[CompactDisplay]], [[UseGrouping]], [[SignDisplay]], [[RoundingIncrement]], [[RoundingMode]], [[ComputedRoundingPriority]], [[TrailingZeroDisplay]], [[BoundFormat]] »).
    auto number_format = TRY(ordinary_create_from_constructor<NumberFormat>(vm, new_target, &Intrinsics::intl_number_format_prototype));

    // 3. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales_value));

    // 4. Set options to ? CoerceOptionsToObject(options).
    auto* options = TRY(coerce_options_to_object(vm, options_value));

    // 5. Let opt be a new Record.
    LocaleOptions opt {};

    // 6. Let matcher be ? GetOption(options, "localeMatcher", STRING, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 7. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 8. Let numberingSystem be ? GetOption(options, "numberingSystem", STRING, EMPTY, undefined).
    auto numbering_system = TRY(get_option(vm, *options, vm.names.numberingSystem, OptionType::String, {}, Empty {}));

    // 9. If numberingSystem is not undefined, then
    if (!numbering_system.is_undefined()) {
        // a. If numberingSystem cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(numbering_system.as_string().utf8_string_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, numbering_system, "numberingSystem"sv);
    }

    // 10. Set opt.[[nu]] to numberingSystem.
    opt.nu = locale_key_from_value(numbering_system);

    // 11. Let r be ResolveLocale(%Intl.NumberFormat%.[[AvailableLocales]], requestedLocales, opt, %Intl.NumberFormat%.[[RelevantExtensionKeys]], %Intl.NumberFormat%.[[LocaleData]]).
    auto result = resolve_locale(requested_locales, opt, NumberFormat::relevant_extension_keys());

    // 12. Set numberFormat.[[Locale]] to r.[[Locale]].
    number_format->set_locale(move(result.locale));

    // 13. Set numberFormat.[[LocaleData]] to r.[[LocaleData]].

    // 14. Set numberFormat.[[NumberingSystem]] to r.[[nu]].
    if (auto* resolved_numbering_system = result.nu.get_pointer<String>())
        number_format->set_numbering_system(move(*resolved_numbering_system));

    // 15. Perform ? SetNumberFormatUnitOptions(numberFormat, options).
    TRY(set_number_format_unit_options(vm, number_format, *options));

    // 16. Let style be numberFormat.[[Style]].
    auto style = number_format->style();

    // 17. Let notation be ? GetOption(options, "notation", STRING, « "standard", "scientific", "engineering", "compact" », "standard").
    auto notation = TRY(get_option(vm, *options, vm.names.notation, OptionType::String, { "standard"sv, "scientific"sv, "engineering"sv, "compact"sv }, "standard"sv));

    // 18. Set numberFormat.[[Notation]] to notation.
    number_format->set_notation(notation.as_string().utf8_string_view());

    int default_min_fraction_digits = 0;
    int default_max_fraction_digits = 0;

    // 19. If style is "currency" and notation is "standard", then
    if (style == Unicode::NumberFormatStyle::Currency && number_format->notation() == Unicode::Notation::Standard) {
        // a. Let currency be numberFormat.[[Currency]].
        auto const& currency = number_format->currency();

        // b. Let cDigits be CurrencyDigits(currency).
        int digits = currency_digits(currency);

        // c. Let mnfdDefault be cDigits.
        default_min_fraction_digits = digits;

        // d. Let mxfdDefault be cDigits.
        default_max_fraction_digits = digits;
    }
    // 20. Else,
    else {
        // a. Let mnfdDefault be 0.
        default_min_fraction_digits = 0;

        // b. If style is "percent", then
        //     i. Let mxfdDefault be 0.
        // c. Else,
        //     i. Let mxfdDefault be 3.
        default_max_fraction_digits = style == Unicode::NumberFormatStyle::Percent ? 0 : 3;
    }

    // 21. Perform ? SetNumberFormatDigitOptions(numberFormat, options, mnfdDefault, mxfdDefault, notation).
    TRY(set_number_format_digit_options(vm, number_format, *options, default_min_fraction_digits, default_max_fraction_digits, number_format->notation()));

    // 22. Let compactDisplay be ? GetOption(options, "compactDisplay", STRING, « "short", "long" », "short").
    auto compact_display = TRY(get_option(vm, *options, vm.names.compactDisplay, OptionType::String, { "short"sv, "long"sv }, "short"sv));

    // 23. Let defaultUseGrouping be "auto".
    auto default_use_grouping = "auto"sv;

    // 24. If notation is "compact", then
    if (number_format->notation() == Unicode::Notation::Compact) {
        // a. Set numberFormat.[[CompactDisplay]] to compactDisplay.
        number_format->set_compact_display(compact_display.as_string().utf8_string_view());

        // b. Set defaultUseGrouping to "min2".
        default_use_grouping = "min2"sv;
    }

    // 25. NOTE: For historical reasons, the strings "true" and "false" are accepted and replaced with the default value.
    // 26. Let useGrouping be ? GetBooleanOrStringNumberFormatOption(options, "useGrouping", « "min2", "auto", "always", "true", "false" », defaultUseGrouping).
    auto use_grouping = TRY(get_boolean_or_string_number_format_option(vm, *options, vm.names.useGrouping, { "min2"sv, "auto"sv, "always"sv, "true"sv, "false"sv }, default_use_grouping));

    // 27. If useGrouping is "true" or useGrouping is "false", set useGrouping to defaultUseGrouping.
    if (auto const* use_grouping_string = use_grouping.get_pointer<StringView>()) {
        if (use_grouping_string->is_one_of("true"sv, "false"sv))
            use_grouping = default_use_grouping;
    }

    // 28. If useGrouping is true, set useGrouping to "always".
    if (auto const* use_grouping_boolean = use_grouping.get_pointer<bool>()) {
        if (*use_grouping_boolean)
            use_grouping = "always"sv;
    }

    // 29. Set numberFormat.[[UseGrouping]] to useGrouping.
    number_format->set_use_grouping(use_grouping);

    // 30. Let signDisplay be ? GetOption(options, "signDisplay", STRING, « "auto", "never", "always", "exceptZero", "negative" », "auto").
    auto sign_display = TRY(get_option(vm, *options, vm.names.signDisplay, OptionType::String, { "auto"sv, "never"sv, "always"sv, "exceptZero"sv, "negative"sv }, "auto"sv));

    // 31. Set numberFormat.[[SignDisplay]] to signDisplay.
    number_format->set_sign_display(sign_display.as_string().utf8_string_view());

    // 32. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
    //     a. Let this be the this value.
    //     b. Return ? ChainNumberFormat(numberFormat, NewTarget, this).

    // Non-standard, create an ICU number formatter for this Intl object.
    auto formatter = Unicode::NumberFormat::create(
        number_format->locale(),
        number_format->numbering_system(),
        number_format->display_options(),
        number_format->rounding_options());
    number_format->set_formatter(move(formatter));

    // 33. Return numberFormat.
    return number_format;
}

// 16.1.2 SetNumberFormatDigitOptions ( intlObj, options, mnfdDefault, mxfdDefault, notation ), https://tc39.es/ecma402/#sec-setnfdigitoptions
ThrowCompletionOr<void> set_number_format_digit_options(VM& vm, NumberFormatBase& intl_object, Object const& options, int default_min_fraction_digits, int default_max_fraction_digits, Unicode::Notation notation)
{
    // 1. Let mnid be ? GetNumberOption(options, "minimumIntegerDigits,", 1, 21, 1).
    auto min_integer_digits = TRY(get_number_option(vm, options, vm.names.minimumIntegerDigits, 1, 21, 1));

    // 2. Let mnfd be ? Get(options, "minimumFractionDigits").
    auto min_fraction_digits = TRY(options.get(vm.names.minimumFractionDigits));

    // 3. Let mxfd be ? Get(options, "maximumFractionDigits").
    auto max_fraction_digits = TRY(options.get(vm.names.maximumFractionDigits));

    // 4. Let mnsd be ? Get(options, "minimumSignificantDigits").
    auto min_significant_digits = TRY(options.get(vm.names.minimumSignificantDigits));

    // 5. Let mxsd be ? Get(options, "maximumSignificantDigits").
    auto max_significant_digits = TRY(options.get(vm.names.maximumSignificantDigits));

    // 6. Set intlObj.[[MinimumIntegerDigits]] to mnid.
    intl_object.set_min_integer_digits(*min_integer_digits);

    // 7. Let roundingIncrement be ? GetNumberOption(options, "roundingIncrement", 1, 5000, 1).
    auto rounding_increment = TRY(get_number_option(vm, options, vm.names.roundingIncrement, 1, 5000, 1));

    // 8. If roundingIncrement is not in « 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 », throw a RangeError exception.
    static constexpr auto sanctioned_rounding_increments = AK::Array { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 };

    if (!sanctioned_rounding_increments.span().contains_slow(*rounding_increment))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidRoundingIncrement, *rounding_increment);

    // 9. Let roundingMode be ? GetOption(options, "roundingMode", STRING, « "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" », "halfExpand").
    auto rounding_mode = TRY(get_option(vm, options, vm.names.roundingMode, OptionType::String, { "ceil"sv, "floor"sv, "expand"sv, "trunc"sv, "halfCeil"sv, "halfFloor"sv, "halfExpand"sv, "halfTrunc"sv, "halfEven"sv }, "halfExpand"sv));

    // 10. Let roundingPriority be ? GetOption(options, "roundingPriority", STRING, « "auto", "morePrecision", "lessPrecision" », "auto").
    auto rounding_priority_option = TRY(get_option(vm, options, vm.names.roundingPriority, OptionType::String, { "auto"sv, "morePrecision"sv, "lessPrecision"sv }, "auto"sv));
    auto rounding_priority = rounding_priority_option.as_string().utf8_string_view();

    // 11. Let trailingZeroDisplay be ? GetOption(options, "trailingZeroDisplay", STRING, « "auto", "stripIfInteger" », "auto").
    auto trailing_zero_display = TRY(get_option(vm, options, vm.names.trailingZeroDisplay, OptionType::String, { "auto"sv, "stripIfInteger"sv }, "auto"sv));

    // 12. NOTE: All fields required by SetNumberFormatDigitOptions have now been read from options. The remainder of this AO interprets the options and may throw exceptions.

    // 13. If roundingIncrement is not 1, set mxfdDefault to mnfdDefault.
    if (rounding_increment != 1)
        default_max_fraction_digits = default_min_fraction_digits;

    // 14. Set intlObj.[[RoundingIncrement]] to roundingIncrement.
    intl_object.set_rounding_increment(*rounding_increment);

    // 15. Set intlObj.[[RoundingMode]] to roundingMode.
    intl_object.set_rounding_mode(rounding_mode.as_string().utf8_string_view());

    // 16. Set intlObj.[[TrailingZeroDisplay]] to trailingZeroDisplay.
    intl_object.set_trailing_zero_display(trailing_zero_display.as_string().utf8_string_view());

    // 17. If mnsd is undefined and mxsd is undefined, let hasSd be false. Otherwise, let hasSd be true.
    bool has_significant_digits = !min_significant_digits.is_undefined() || !max_significant_digits.is_undefined();

    // 18. If mnfd is undefined and mxsd is undefined, let hasFd be false. Otherwise, let hasFd be true.
    bool has_fraction_digits = !min_fraction_digits.is_undefined() || !max_fraction_digits.is_undefined();

    // 19. Let needSd be true.
    bool need_significant_digits = true;

    // 20. Let needFd be true.
    bool need_fraction_digits = true;

    // 21. If roundingPriority is "auto", then
    if (rounding_priority == "auto"sv) {
        // a. Set needSd to hasSd.
        need_significant_digits = has_significant_digits;

        // b. If needSd is true, or hasFd is false and notation is "compact", then
        if (need_significant_digits || (!has_fraction_digits && notation == Unicode::Notation::Compact)) {
            // i. Set needFd to false.
            need_fraction_digits = false;
        }
    }

    // 22. If needSd is true, then
    if (need_significant_digits) {
        // a. If hasSd is true, then
        if (has_significant_digits) {
            // i. Set intlObj.[[MinimumSignificantDigits]] to ? DefaultNumberOption(mnsd, 1, 21, 1).
            auto min_digits = TRY(default_number_option(vm, min_significant_digits, 1, 21, 1));
            intl_object.set_min_significant_digits(*min_digits);

            // ii. Set intlObj.[[MaximumSignificantDigits]] to ? DefaultNumberOption(mxsd, intlObj.[[MinimumSignificantDigits]], 21, 21).
            auto max_digits = TRY(default_number_option(vm, max_significant_digits, *min_digits, 21, 21));
            intl_object.set_max_significant_digits(*max_digits);
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
    if (need_fraction_digits) {
        // a. If hasFd is true, then
        if (has_fraction_digits) {
            // i. Set mnfd to ? DefaultNumberOption(mnfd, 0, 100, undefined).
            auto min_digits = TRY(default_number_option(vm, min_fraction_digits, 0, 100, {}));

            // ii. Set mxfd to ? DefaultNumberOption(mxfd, 0, 100, undefined).
            auto max_digits = TRY(default_number_option(vm, max_fraction_digits, 0, 100, {}));

            // iii. If mnfd is undefined, set mnfd to min(mnfdDefault, mxfd).
            if (!min_digits.has_value())
                min_digits = min(default_min_fraction_digits, *max_digits);
            // iv. Else if mxfd is undefined, set mxfd to max(mxfdDefault, mnfd).
            else if (!max_digits.has_value())
                max_digits = max(default_max_fraction_digits, *min_digits);
            // v. Else if mnfd is greater than mxfd, throw a RangeError exception.
            else if (*min_digits > *max_digits)
                return vm.throw_completion<RangeError>(ErrorType::IntlMinimumExceedsMaximum, *min_digits, *max_digits);

            // vi. Set intlObj.[[MinimumFractionDigits]] to mnfd.
            intl_object.set_min_fraction_digits(*min_digits);

            // vii. Set intlObj.[[MaximumFractionDigits]] to mxfd.
            intl_object.set_max_fraction_digits(*max_digits);
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
    if (!need_significant_digits && !need_fraction_digits) {
        // a. Set intlObj.[[MinimumFractionDigits]] to 0.
        intl_object.set_min_fraction_digits(0);

        // b. Set intlObj.[[MaximumFractionDigits]] to 0.
        intl_object.set_max_fraction_digits(0);

        // c. Set intlObj.[[MinimumSignificantDigits]] to 1.
        intl_object.set_min_significant_digits(1);

        // d. Set intlObj.[[MaximumSignificantDigits]] to 2.
        intl_object.set_max_significant_digits(2);

        // e. Set intlObj.[[RoundingType]] to MORE-PRECISION.
        intl_object.set_rounding_type(Unicode::RoundingType::MorePrecision);

        // f. Set intlObj.[[ComputedRoundingPriority]] to "morePrecision".
        intl_object.set_computed_rounding_priority(NumberFormatBase::ComputedRoundingPriority::MorePrecision);
    }
    // 25. Else if roundingPriority is "morePrecision", then
    else if (rounding_priority == "morePrecision"sv) {
        // a. Set intlObj.[[RoundingType]] to MORE-PRECISION.
        intl_object.set_rounding_type(Unicode::RoundingType::MorePrecision);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "morePrecision".
        intl_object.set_computed_rounding_priority(NumberFormatBase::ComputedRoundingPriority::MorePrecision);
    }
    // 26. Else if roundingPriority is "lessPrecision", then
    else if (rounding_priority == "lessPrecision"sv) {
        // a. Set intlObj.[[RoundingType]] to LESS-PRECISION.
        intl_object.set_rounding_type(Unicode::RoundingType::LessPrecision);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "lessPrecision".
        intl_object.set_computed_rounding_priority(NumberFormatBase::ComputedRoundingPriority::LessPrecision);
    }
    // 27. Else if hasSd is true, then
    else if (has_significant_digits) {
        // a. Set intlObj.[[RoundingType]] to SIGNIFICANT-DIGITS.
        intl_object.set_rounding_type(Unicode::RoundingType::SignificantDigits);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "auto".
        intl_object.set_computed_rounding_priority(NumberFormatBase::ComputedRoundingPriority::Auto);
    }
    // 28. Else,
    else {
        // a. Set intlObj.[[RoundingType]] to FRACTION-DIGITS.
        intl_object.set_rounding_type(Unicode::RoundingType::FractionDigits);

        // b. Set intlObj.[[ComputedRoundingPriority]] to "auto".
        intl_object.set_computed_rounding_priority(NumberFormatBase::ComputedRoundingPriority::Auto);
    }

    // 29. If roundingIncrement is not 1, then
    if (rounding_increment != 1) {
        // a. If intlObj.[[RoundingType]] is not FRACTION-DIGITS, throw a TypeError exception.
        if (intl_object.rounding_type() != Unicode::RoundingType::FractionDigits)
            return vm.throw_completion<TypeError>(ErrorType::IntlInvalidRoundingIncrementForRoundingType, *rounding_increment, intl_object.rounding_type_string());

        // b. If intlObj.[[MaximumFractionDigits]] is not intlObj.[[MinimumFractionDigits]], throw a RangeError exception.
        if (intl_object.max_fraction_digits() != intl_object.min_fraction_digits())
            return vm.throw_completion<RangeError>(ErrorType::IntlInvalidRoundingIncrementForFractionDigits, *rounding_increment);
    }

    // 30. Return UNUSED.
    return {};
}

// 16.1.3 SetNumberFormatUnitOptions ( intlObj, options ), https://tc39.es/ecma402/#sec-setnumberformatunitoptions
ThrowCompletionOr<void> set_number_format_unit_options(VM& vm, NumberFormat& intl_object, Object const& options)
{
    // 1. Let style be ? GetOption(options, "style", STRING, « "decimal", "percent", "currency", "unit" », "decimal").
    auto style = TRY(get_option(vm, options, vm.names.style, OptionType::String, { "decimal"sv, "percent"sv, "currency"sv, "unit"sv }, "decimal"sv));

    // 2. Set intlObj.[[Style]] to style.
    intl_object.set_style(style.as_string().utf8_string_view());

    // 3. Let currency be ? GetOption(options, "currency", STRING, EMPTY, undefined).
    auto currency = TRY(get_option(vm, options, vm.names.currency, OptionType::String, {}, Empty {}));

    // 4. If currency is undefined, then
    if (currency.is_undefined()) {
        // a. If style is "currency", throw a TypeError exception.
        if (intl_object.style() == Unicode::NumberFormatStyle::Currency)
            return vm.throw_completion<TypeError>(ErrorType::IntlOptionUndefined, "currency"sv, "style"sv, style);
    }
    // 5. Else,
    //     a. If IsWellFormedCurrencyCode(currency) is false, throw a RangeError exception.
    else if (!is_well_formed_currency_code(currency.as_string().utf8_string_view())) {
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, currency, "currency"sv);
    }

    // 6. Let currencyDisplay be ? GetOption(options, "currencyDisplay", STRING, « "code", "symbol", "narrowSymbol", "name" », "symbol").
    auto currency_display = TRY(get_option(vm, options, vm.names.currencyDisplay, OptionType::String, { "code"sv, "symbol"sv, "narrowSymbol"sv, "name"sv }, "symbol"sv));

    // 7. Let currencySign be ? GetOption(options, "currencySign", STRING, « "standard", "accounting" », "standard").
    auto currency_sign = TRY(get_option(vm, options, vm.names.currencySign, OptionType::String, { "standard"sv, "accounting"sv }, "standard"sv));

    // 8. Let unit be ? GetOption(options, "unit", STRING, EMPTY, undefined).
    auto unit = TRY(get_option(vm, options, vm.names.unit, OptionType::String, {}, Empty {}));

    // 9. If unit is undefined, then
    if (unit.is_undefined()) {
        // a. If style is "unit", throw a TypeError exception.
        if (intl_object.style() == Unicode::NumberFormatStyle::Unit)
            return vm.throw_completion<TypeError>(ErrorType::IntlOptionUndefined, "unit"sv, "style"sv, style);
    }
    // 10. Else,
    //     a. If IsWellFormedUnitIdentifier(unit) is false, throw a RangeError exception.
    else if (!is_well_formed_unit_identifier(unit.as_string().utf8_string_view())) {
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, unit, "unit"sv);
    }

    // 11. Let unitDisplay be ? GetOption(options, "unitDisplay", STRING, « "short", "narrow", "long" », "short").
    auto unit_display = TRY(get_option(vm, options, vm.names.unitDisplay, OptionType::String, { "short"sv, "narrow"sv, "long"sv }, "short"sv));

    // 12. If style is "currency", then
    if (intl_object.style() == Unicode::NumberFormatStyle::Currency) {
        // a. Set intlObj.[[Currency]] to the ASCII-uppercase of currency.
        intl_object.set_currency(MUST(currency.as_string().utf8_string().to_uppercase()));

        // c. Set intlObj.[[CurrencyDisplay]] to currencyDisplay.
        intl_object.set_currency_display(currency_display.as_string().utf8_string_view());

        // d. Set intlObj.[[CurrencySign]] to currencySign.
        intl_object.set_currency_sign(currency_sign.as_string().utf8_string_view());
    }

    // 13. If style is "unit", then
    if (intl_object.style() == Unicode::NumberFormatStyle::Unit) {
        // a. Set intlObj.[[Unit]] to unit.
        intl_object.set_unit(unit.as_string().utf8_string());

        // b. Set intlObj.[[UnitDisplay]] to unitDisplay.
        intl_object.set_unit_display(unit_display.as_string().utf8_string_view());
    }

    // 14. Return UNUSED.
    return {};
}

// 16.2.2 Intl.NumberFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.numberformat.supportedlocalesof
JS_DEFINE_NATIVE_FUNCTION(NumberFormatConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %NumberFormat%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
