/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/Intl/CollatorConstructor.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(CollatorConstructor);

// 10.1 The Intl.Collator Constructor, https://tc39.es/ecma402/#sec-the-intl-collator-constructor
CollatorConstructor::CollatorConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Collator.as_string(), realm.intrinsics().function_prototype())
{
}

void CollatorConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 10.2.1 Intl.Collator.prototype, https://tc39.es/ecma402/#sec-intl.collator.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_collator_prototype(), 0);
    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);
}

// 10.1.1 Intl.Collator ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.collator
ThrowCompletionOr<Value> CollatorConstructor::call()
{
    // 1. If NewTarget is undefined, let newTarget be the active function object, else let newTarget be NewTarget
    return TRY(construct(*this));
}

// 10.1.1 Intl.Collator ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.collator
ThrowCompletionOr<GC::Ref<Object>> CollatorConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let internalSlotsList be « [[InitializedCollator]], [[Locale]], [[Usage]], [[Collation]], [[Numeric]], [[CaseFirst]], [[Sensitivity]], [[IgnorePunctuation]], [[BoundCompare]] ».
    // 3. Let collator be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.Collator.prototype%", internalSlotsList).
    auto collator = TRY(ordinary_create_from_constructor<Collator>(vm, new_target, &Intrinsics::intl_collator_prototype));

    // 4. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales_value));

    // 5. Set options to ? CoerceOptionsToObject(options).
    auto* options = TRY(coerce_options_to_object(vm, options_value));

    // 6. Let usage be ? GetOption(options, "usage", string, « "sort", "search" », "sort").
    auto usage = TRY(get_option(vm, *options, vm.names.usage, OptionType::String, { "sort"sv, "search"sv }, "sort"sv));

    // 7. Set collator.[[Usage]] to usage.
    collator->set_usage(usage.as_string().utf8_string_view());

    // 8. If usage is "sort", then
    //     a. Let localeData be %Intl.Collator%.[[SortLocaleData]].
    // 9. Else,
    //     a. Let localeData be %Intl.Collator%.[[SearchLocaleData]].

    // 10. Let opt be a new Record.
    LocaleOptions opt {};

    // 11. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 12. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 13. Let collation be ? GetOption(options, "collation", string, empty, undefined).
    auto collation = TRY(get_option(vm, *options, vm.names.collation, OptionType::String, {}, Empty {}));

    // 14. If collation is not undefined, then
    if (!collation.is_undefined()) {
        // a. If collation cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(collation.as_string().utf8_string_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, collation, "collation"sv);
    }

    // 15. Set opt.[[co]] to collation.
    opt.co = locale_key_from_value(collation);

    // 16. Let numeric be ? GetOption(options, "numeric", boolean, empty, undefined).
    auto numeric = TRY(get_option(vm, *options, vm.names.numeric, OptionType::Boolean, {}, Empty {}));

    // 17. If numeric is not undefined, then
    if (!numeric.is_undefined()) {
        // a. Set numeric to ! ToString(numeric).
        numeric = PrimitiveString::create(vm, MUST(numeric.to_string(vm)));
    }

    // 18. Set opt.[[kn]] to numeric.
    opt.kn = locale_key_from_value(numeric);

    // 19. Let caseFirst be ? GetOption(options, "caseFirst", string, « "upper", "lower", "false" », undefined).
    auto case_first = TRY(get_option(vm, *options, vm.names.caseFirst, OptionType::String, { "upper"sv, "lower"sv, "false"sv }, Empty {}));

    // 20. Set opt.[[kf]] to caseFirst.
    opt.kf = locale_key_from_value(case_first);

    // 21. Let r be ResolveLocale(%Intl.Collator%.[[AvailableLocales]], requestedLocales, opt, %Intl.Collator%.[[RelevantExtensionKeys]], localeData).
    auto result = resolve_locale(requested_locales, opt, Collator::relevant_extension_keys());

    // 22. Set collator.[[Locale]] to r.[[Locale]].
    collator->set_locale(move(result.locale));

    // 23. Set collation to r.[[co]].
    auto collation_value = move(result.co);

    // 24. If collation is null, set collation to "default".
    if (collation_value.has<Empty>())
        collation_value = "default"_string;

    // 25. Set collator.[[Collation]] to collation.
    collator->set_collation(move(collation_value.get<String>()));

    // 26. Set collator.[[Numeric]] to SameValue(r.[[kn]], "true").
    collator->set_numeric(result.kn == "true"_string);

    // 27. Set collator.[[CaseFirst]] to r.[[kf]].
    if (auto const* resolved_case_first = result.kf.get_pointer<String>())
        collator->set_case_first(*resolved_case_first);

    // 28. Let resolvedLocaleData be r.[[LocaleData]].

    // 29. Let sensitivity be ? GetOption(options, "sensitivity", string, « "base", "accent", "case", "variant" », undefined).
    auto sensitivity_value = TRY(get_option(vm, *options, vm.names.sensitivity, OptionType::String, { "base"sv, "accent"sv, "case"sv, "variant"sv }, Empty {}));

    // 30. If sensitivity is undefined, then
    if (sensitivity_value.is_undefined()) {
        // a. If usage is "sort", then
        if (collator->usage() == Unicode::Usage::Sort) {
            // i. Set sensitivity to "variant".
            sensitivity_value = PrimitiveString::create(vm, "variant"_string);
        }
        // b. Else,
        else {
            // i. Set sensitivity to resolvedLocaleData.[[sensitivity]].
            // NOTE: We do not acquire the default [[sensitivity]] here. Instead, we default the option to null,
            //       and let LibUnicode fill in the default value if an override was not provided here.
        }
    }

    Optional<Unicode::Sensitivity> sensitivity;
    if (!sensitivity_value.is_undefined())
        sensitivity = Unicode::sensitivity_from_string(sensitivity_value.as_string().utf8_string_view());

    // 32. Let defaultIgnorePunctuation be resolvedLocaleData.[[ignorePunctuation]].
    // NOTE: We do not acquire the default [[ignorePunctuation]] here. Instead, we default the option to null,
    //       and let LibUnicode fill in the default value if an override was not provided here.

    // 33. Let ignorePunctuation be ? GetOption(options, "ignorePunctuation", boolean, empty, defaultIgnorePunctuation).
    auto ignore_punctuation_value = TRY(get_option(vm, *options, vm.names.ignorePunctuation, OptionType::Boolean, {}, Empty {}));

    Optional<bool> ignore_punctuation;
    if (!ignore_punctuation_value.is_undefined())
        ignore_punctuation = ignore_punctuation_value.as_bool();

    // Non-standard, create an ICU collator for this Intl object.
    auto icu_collator = Unicode::Collator::create(
        collator->locale(),
        collator->usage(),
        collator->collation(),
        sensitivity,
        collator->case_first(),
        collator->numeric(),
        ignore_punctuation);
    collator->set_collator(move(icu_collator));

    // 31. Set collator.[[Sensitivity]] to sensitivity.
    collator->set_sensitivity(collator->collator().sensitivity());

    // 34. Set collator.[[IgnorePunctuation]] to ignorePunctuation.
    collator->set_ignore_punctuation(collator->collator().ignore_punctuation());

    // 35. Return collator.
    return collator;
}

// 10.2.2 Intl.Collator.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.collator.supportedlocalesof
JS_DEFINE_NATIVE_FUNCTION(CollatorConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %Collator%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
