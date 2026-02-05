/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormat.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormatConstructor.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(RelativeTimeFormatConstructor);

// 18.1 The Intl.RelativeTimeFormat Constructor, https://tc39.es/ecma402/#sec-intl-relativetimeformat-constructor
RelativeTimeFormatConstructor::RelativeTimeFormatConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.RelativeTimeFormat.as_string(), realm.intrinsics().function_prototype())
{
}

void RelativeTimeFormatConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 18.2.1 Intl.RelativeTimeFormat.prototype, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_relative_time_format_prototype(), 0);
    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);
}

// 18.1.1 Intl.RelativeTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat
ThrowCompletionOr<Value> RelativeTimeFormatConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.RelativeTimeFormat");
}

// 18.1.1 Intl.RelativeTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat
ThrowCompletionOr<GC::Ref<Object>> RelativeTimeFormatConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let relativeTimeFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.RelativeTimeFormat.prototype%", « [[InitializedRelativeTimeFormat]], [[Locale]], [[LocaleData]], [[Style]], [[Numeric]], [[NumberFormat]], [[NumberingSystem]], [[PluralRules]] »).
    auto relative_time_format = TRY(ordinary_create_from_constructor<RelativeTimeFormat>(vm, new_target, &Intrinsics::intl_relative_time_format_prototype));

    // 3. Let optionsResolution be ? ResolveOptions(%Intl.RelativeTimeFormat%, %Intl.RelativeTimeFormat%.[[LocaleData]], locales, options, « COERCE-OPTIONS »).
    // 4. Set options to optionsResolution.[[Options]].
    // 5. Let r be optionsResolution.[[ResolvedLocale]].
    auto [options, result, _] = TRY(resolve_options(vm, relative_time_format, locales_value, options_value, SpecialBehaviors::CoerceOptions));

    // 6. Let locale be r.[[Locale]].
    auto locale = move(result.locale);

    // 7. Set relativeTimeFormat.[[Locale]] to locale.
    relative_time_format->set_locale(locale);

    // 8. Set relativeTimeFormat.[[LocaleData]] to r.[[LocaleData]].

    // 9. Set relativeTimeFormat.[[NumberingSystem]] to r.[[nu]].
    if (auto* resolved_numbering_system = result.nu.get_pointer<String>())
        relative_time_format->set_numbering_system(move(*resolved_numbering_system));

    // 10. Let style be ? GetOption(options, "style", STRING, « "long", "short", "narrow" », "long").
    auto style = TRY(get_option(vm, *options, vm.names.style, OptionType::String, { "long"sv, "short"sv, "narrow"sv }, "long"sv));

    // 11. Set relativeTimeFormat.[[Style]] to style.
    relative_time_format->set_style(style.as_string().utf8_string_view());

    // 12. Let numeric be ? GetOption(options, "numeric", STRING, « "always", "auto" », "always").
    auto numeric = TRY(get_option(vm, *options, vm.names.numeric, OptionType::String, { "always"sv, "auto"sv }, "always"sv));

    // 13. Set relativeTimeFormat.[[Numeric]] to numeric.
    relative_time_format->set_numeric(numeric.as_string().utf8_string_view());

    // 14. Let nfOptions be OrdinaryObjectCreate(null).
    // 15. Perform ! CreateDataPropertyOrThrow(nfOptions, "numberingSystem", relativeTimeFormat.[[NumberingSystem]]).
    // 16. Let relativeTimeFormat.[[NumberFormat]] be ! Construct(%Intl.NumberFormat%, « locale, nfOptions »).
    // 17. Let relativeTimeFormat.[[PluralRules]] be ! Construct(%Intl.PluralRules%, « locale »).
    auto formatter = Unicode::RelativeTimeFormat::create(
        result.icu_locale,
        relative_time_format->style());
    relative_time_format->set_formatter(move(formatter));

    // 18. Return relativeTimeFormat.
    return relative_time_format;
}

// 18.2.2 Intl.RelativeTimeFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat.supportedLocalesOf
JS_DEFINE_NATIVE_FUNCTION(RelativeTimeFormatConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %RelativeTimeFormat%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
