/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
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
    auto& realm = *vm.current_realm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let internalSlotsList be « [[InitializedCollator]], [[Locale]], [[Usage]], [[Collation]], [[Numeric]], [[CaseFirst]], [[Sensitivity]], [[IgnorePunctuation]], [[BoundCompare]] ».
    // 3. Let collator be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.Collator.prototype%", internalSlotsList).
    auto collator = TRY(ordinary_create_from_constructor<Collator>(vm, new_target, &Intrinsics::intl_collator_prototype));

    // 4. NOTE: The source of locale data for ResolveOptions depends upon the "usage" property of options, but the following
    //    two steps must observably precede that lookup (and must not observably repeat inside ResolveOptions).

    // 5. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales_value));

    // 6. Set options to ? CoerceOptionsToObject(options).
    auto options = TRY(coerce_options_to_object(vm, options_value));

    // 7. Let usage be ? GetOption(options, "usage", string, « "sort", "search" », "sort").
    auto usage = TRY(get_option(vm, options, vm.names.usage, OptionType::String, { "sort"sv, "search"sv }, "sort"sv));

    // 8. Set collator.[[Usage]] to usage.
    collator->set_usage(usage.as_string().utf8_string_view());

    // 9. If usage is "sort", then
    //     a. Let localeData be %Intl.Collator%.[[SortLocaleData]].
    // 10. Else,
    //     a. Let localeData be %Intl.Collator%.[[SearchLocaleData]].

    // 11. Let optionsResolution be ? ResolveOptions(%Intl.Collator%, localeData, CreateArrayFromList(requestedLocales), options).
    auto requested_locales_array = Array::create_from<String>(realm, requested_locales, [&](auto& locale) { return PrimitiveString::create(vm, move(locale)); });
    auto options_resolution = TRY(resolve_options(vm, collator, requested_locales_array, options_value));

    // 12. Let r be optionsResolution.[[ResolvedLocale]].
    auto result = move(options_resolution.resolved_locale);

    // 13. Set collator.[[Locale]] to r.[[Locale]].
    collator->set_locale(move(result.locale));

    // 14. If r.[[co]] is null, let collation be "default". Otherwise, let collation be r.[[co]].
    auto collation = result.co.has<Empty>()
        ? "default"_string
        : move(result.co.get<String>());

    // 15. Set collator.[[Collation]] to collation.
    collator->set_collation(move(collation));

    // 16. Set collator.[[Numeric]] to SameValue(r.[[kn]], "true").
    collator->set_numeric(result.kn == "true"_string);

    // 17. Set collator.[[CaseFirst]] to r.[[kf]].
    if (auto const* resolved_case_first = result.kf.get_pointer<String>())
        collator->set_case_first(*resolved_case_first);

    // 18. Let resolvedLocaleData be r.[[LocaleData]].

    // 19. If usage is "sort", let defaultSensitivity be "variant". Otherwise, let defaultSensitivity be resolvedLocaleData.[[sensitivity]].
    // NOTE: We do not acquire resolvedLocaleData.[[sensitivity]] here. Instead, we let LibUnicode fill in the
    //       default value if an override was not provided here.
    auto default_sensitivity = collator->usage() == Unicode::Usage::Sort ? "variant"sv : OptionDefault {};

    // 20. Set collator.[[Sensitivity]] to ? GetOption(options, "sensitivity", string, « "base", "accent", "case", "variant" », defaultSensitivity).
    auto sensitivity_value = TRY(get_option(vm, options, vm.names.sensitivity, OptionType::String, { "base"sv, "accent"sv, "case"sv, "variant"sv }, default_sensitivity));

    Optional<Unicode::Sensitivity> sensitivity;
    if (!sensitivity_value.is_undefined())
        sensitivity = Unicode::sensitivity_from_string(sensitivity_value.as_string().utf8_string_view());

    // 21. Let defaultIgnorePunctuation be resolvedLocaleData.[[ignorePunctuation]].
    // NOTE: We do not acquire resolvedLocaleData.[[ignorePunctuation]] here. Instead, we let LibUnicode fill in the
    //       default value if an override was not provided here.

    // 22. Set collator.[[IgnorePunctuation]] to ? GetOption(options, "ignorePunctuation", boolean, empty, defaultIgnorePunctuation).
    auto ignore_punctuation_value = TRY(get_option(vm, options, vm.names.ignorePunctuation, OptionType::Boolean, {}, Empty {}));

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

    collator->set_sensitivity(collator->collator().sensitivity());
    collator->set_ignore_punctuation(collator->collator().ignore_punctuation());

    // 23. Return collator.
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
