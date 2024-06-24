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

JS_DEFINE_ALLOCATOR(CollatorConstructor);

// 10.1.2 InitializeCollator ( collator, locales, options ), https://tc39.es/ecma402/#sec-initializecollator
static ThrowCompletionOr<NonnullGCPtr<Collator>> initialize_collator(VM& vm, Collator& collator, Value locales_value, Value options_value)
{
    // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales_value));

    // 2. Set options to ? CoerceOptionsToObject(options).
    auto* options = TRY(coerce_options_to_object(vm, options_value));

    // 3. Let usage be ? GetOption(options, "usage", string, « "sort", "search" », "sort").
    auto usage = TRY(get_option(vm, *options, vm.names.usage, OptionType::String, { "sort"sv, "search"sv }, "sort"sv));

    // 4. Set collator.[[Usage]] to usage.
    collator.set_usage(usage.as_string().utf8_string_view());

    // 5. If usage is "sort", then
    //     a. Let localeData be %Collator%.[[SortLocaleData]].
    // 6. Else,
    //     a. Let localeData be %Collator%.[[SearchLocaleData]].

    // 7. Let opt be a new Record.
    LocaleOptions opt {};

    // 8. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 9. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 10. Let collation be ? GetOption(options, "collation", string, empty, undefined).
    auto collation = TRY(get_option(vm, *options, vm.names.collation, OptionType::String, {}, Empty {}));

    // 11. If collation is not undefined, then
    if (!collation.is_undefined()) {
        // a. If collation does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(collation.as_string().utf8_string_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, collation, "collation"sv);

        // 12. Set opt.[[co]] to collation.
        opt.co = collation.as_string().utf8_string();
    }

    // 13. Let numeric be ? GetOption(options, "numeric", boolean, empty, undefined).
    auto numeric = TRY(get_option(vm, *options, vm.names.numeric, OptionType::Boolean, {}, Empty {}));

    // 14. If numeric is not undefined, then
    if (!numeric.is_undefined()) {
        // a. Let numeric be ! ToString(numeric).
        numeric = PrimitiveString::create(vm, MUST(numeric.to_string(vm)));
    }

    // 15. Set opt.[[kn]] to numeric.
    opt.kn = locale_key_from_value(numeric);

    // 16. Let caseFirst be ? GetOption(options, "caseFirst", string, « "upper", "lower", "false" », undefined).
    auto case_first = TRY(get_option(vm, *options, vm.names.caseFirst, OptionType::String, { "upper"sv, "lower"sv, "false"sv }, Empty {}));

    // 17. Set opt.[[kf]] to caseFirst.
    opt.kf = locale_key_from_value(case_first);

    // 18. Let relevantExtensionKeys be %Collator%.[[RelevantExtensionKeys]].
    auto relevant_extension_keys = Collator::relevant_extension_keys();

    // 19. Let r be ResolveLocale(%Collator%.[[AvailableLocales]], requestedLocales, opt, relevantExtensionKeys, localeData).
    auto result = resolve_locale(requested_locales, opt, relevant_extension_keys);

    // 20. Set collator.[[Locale]] to r.[[locale]].
    collator.set_locale(move(result.locale));

    // 21. Let collation be r.[[co]].
    auto& collation_value = result.co;

    // 22. If collation is null, let collation be "default".
    if (collation_value.has<Empty>())
        collation_value = "default"_string;

    // 23. Set collator.[[Collation]] to collation.
    collator.set_collation(move(collation_value.get<String>()));

    // 24. If relevantExtensionKeys contains "kn", then
    if (relevant_extension_keys.span().contains_slow("kn"sv)) {
        // a. Set collator.[[Numeric]] to SameValue(r.[[kn]], "true").
        collator.set_numeric(result.kn == "true"_string);
    }

    // 25. If relevantExtensionKeys contains "kf", then
    if (relevant_extension_keys.span().contains_slow("kf"sv)) {
        // a. Set collator.[[CaseFirst]] to r.[[kf]].
        if (auto* resolved_case_first = result.kf.get_pointer<String>())
            collator.set_case_first(*resolved_case_first);
    }

    // 26. Let sensitivity be ? GetOption(options, "sensitivity", string, « "base", "accent", "case", "variant" », undefined).
    auto sensitivity = TRY(get_option(vm, *options, vm.names.sensitivity, OptionType::String, { "base"sv, "accent"sv, "case"sv, "variant"sv }, Empty {}));

    // 27. If sensitivity is undefined, then
    if (sensitivity.is_undefined()) {
        // a. If usage is "sort", then
        if (collator.usage() == Collator::Usage::Sort) {
            // i. Let sensitivity be "variant".
            sensitivity = PrimitiveString::create(vm, "variant"_string);
        }
        // b. Else,
        else {
            // i. Let dataLocale be r.[[dataLocale]].
            // ii. Let dataLocaleData be localeData.[[<dataLocale>]].
            // iii. Let sensitivity be dataLocaleData.[[sensitivity]].
            sensitivity = PrimitiveString::create(vm, "base"_string);
        }
    }

    // 28. Set collator.[[Sensitivity]] to sensitivity.
    collator.set_sensitivity(sensitivity.as_string().utf8_string_view());

    // 29. Let ignorePunctuation be ? GetOption(options, "ignorePunctuation", boolean, empty, false).
    auto ignore_punctuation = TRY(get_option(vm, *options, vm.names.ignorePunctuation, OptionType::Boolean, {}, false));

    // 30. Set collator.[[IgnorePunctuation]] to ignorePunctuation.
    collator.set_ignore_punctuation(ignore_punctuation.as_bool());

    // 31. Return collator.
    return collator;
}

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
ThrowCompletionOr<NonnullGCPtr<Object>> CollatorConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 2. Let internalSlotsList be « [[InitializedCollator]], [[Locale]], [[Usage]], [[Sensitivity]], [[IgnorePunctuation]], [[Collation]], [[BoundCompare]] ».
    // 3. If %Collator%.[[RelevantExtensionKeys]] contains "kn", then
    //     a. Append [[Numeric]] as the last element of internalSlotsList.
    // 4. If %Collator%.[[RelevantExtensionKeys]] contains "kf", then
    //     a. Append [[CaseFirst]] as the last element of internalSlotsList.

    // 5. Let collator be ? OrdinaryCreateFromConstructor(newTarget, "%Collator.prototype%", internalSlotsList).
    auto collator = TRY(ordinary_create_from_constructor<Collator>(vm, new_target, &Intrinsics::intl_collator_prototype));

    // 6. Return ? InitializeCollator(collator, locales, options).
    return TRY(initialize_collator(vm, collator, locales, options));
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
