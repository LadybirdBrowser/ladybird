/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/Intl/PluralRulesConstructor.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(PluralRulesConstructor);

// 17.1 The Intl.PluralRules Constructor, https://tc39.es/ecma402/#sec-intl-pluralrules-constructor
PluralRulesConstructor::PluralRulesConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PluralRules.as_string(), realm.intrinsics().function_prototype())
{
}

void PluralRulesConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 17.2.1 Intl.PluralRules.prototype, https://tc39.es/ecma402/#sec-intl.pluralrules.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_plural_rules_prototype(), 0);
    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);
}

// 17.1.1 Intl.PluralRules ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.pluralrules
ThrowCompletionOr<Value> PluralRulesConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.PluralRules");
}

// 17.1.1 Intl.PluralRules ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.pluralrules
ThrowCompletionOr<GC::Ref<Object>> PluralRulesConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let pluralRules be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.PluralRules.prototype%", « [[InitializedPluralRules]], [[Locale]], [[Type]], [[MinimumIntegerDigits]], [[MinimumFractionDigits]], [[MaximumFractionDigits]], [[MinimumSignificantDigits]], [[MaximumSignificantDigits]], [[RoundingType]], [[RoundingIncrement]], [[RoundingMode]], [[ComputedRoundingPriority]], [[TrailingZeroDisplay]] »).
    auto plural_rules = TRY(ordinary_create_from_constructor<PluralRules>(vm, new_target, &Intrinsics::intl_plural_rules_prototype));

    // 3. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales_value));

    // 4. Set options to ? CoerceOptionsToObject(options).
    auto* options = TRY(coerce_options_to_object(vm, options_value));

    // 5. Let opt be a new Record.
    LocaleOptions opt {};

    // 6. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, AK::Array { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 7. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 8. Let r be ResolveLocale(%Intl.PluralRules%.[[AvailableLocales]], requestedLocales, opt, %Intl.PluralRules%.[[RelevantExtensionKeys]], %Intl.PluralRules%.[[LocaleData]]).
    auto result = resolve_locale(requested_locales, opt, {});

    // 9. Set pluralRules.[[Locale]] to r.[[locale]].
    plural_rules->set_locale(move(result.locale));

    // 10. Let t be ? GetOption(options, "type", string, « "cardinal", "ordinal" », "cardinal").
    auto type = TRY(get_option(vm, *options, vm.names.type, OptionType::String, AK::Array { "cardinal"sv, "ordinal"sv }, "cardinal"sv));

    // 11. Set pluralRules.[[Type]] to t.
    plural_rules->set_type(type.as_string().utf8_string_view());

    // 12. Perform ? SetNumberFormatDigitOptions(pluralRules, options, 0, 3, "standard").
    TRY(set_number_format_digit_options(vm, plural_rules, *options, 0, 3, Unicode::Notation::Standard));

    // Non-standard, create an ICU number formatter for this Intl object.
    auto formatter = Unicode::NumberFormat::create(
        result.icu_locale,
        {},
        plural_rules->rounding_options());

    formatter->create_plural_rules(plural_rules->type());
    plural_rules->set_formatter(move(formatter));

    // 13. Return pluralRules.
    return plural_rules;
}

// 17.2.2 Intl.PluralRules.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.pluralrules.supportedlocalesof
JS_DEFINE_NATIVE_FUNCTION(PluralRulesConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %PluralRules%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
