/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/DisplayNames.h>
#include <LibJS/Runtime/Intl/DisplayNamesConstructor.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DisplayNamesConstructor);

// 12.1 The Intl.DisplayNames Constructor, https://tc39.es/ecma402/#sec-intl-displaynames-constructor
DisplayNamesConstructor::DisplayNamesConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.DisplayNames.as_string(), realm.intrinsics().function_prototype())
{
}

void DisplayNamesConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 12.2.1 Intl.DisplayNames.prototype, https://tc39.es/ecma402/#sec-Intl.DisplayNames.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_display_names_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);
}

// 12.1.1 Intl.DisplayNames ( locales, options ), https://tc39.es/ecma402/#sec-Intl.DisplayNames
ThrowCompletionOr<Value> DisplayNamesConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.DisplayNames");
}

// 12.1.1 Intl.DisplayNames ( locales, options ), https://tc39.es/ecma402/#sec-Intl.DisplayNames
ThrowCompletionOr<GC::Ref<Object>> DisplayNamesConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locale_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let displayNames be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.DisplayNames.prototype%", « [[InitializedDisplayNames]], [[Locale]], [[Style]], [[Type]], [[Fallback]], [[LanguageDisplay]], [[Fields]] »).
    auto display_names = TRY(ordinary_create_from_constructor<DisplayNames>(vm, new_target, &Intrinsics::intl_display_names_prototype));

    // 3. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locale_value));

    // 4. If options is undefined, throw a TypeError exception.
    if (options_value.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "options"sv);

    // 5. Set options to ? GetOptionsObject(options).
    auto options = TRY(get_options_object(vm, options_value));

    // 6. Let opt be a new Record.
    LocaleOptions opt {};

    // 7. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 8. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 9. Let r be ResolveLocale(%Intl.DisplayNames%.[[AvailableLocales]], requestedLocales, opt, %Intl.DisplayNames%.[[RelevantExtensionKeys]], %Intl.DisplayNames%.[[LocaleData]]).
    auto result = resolve_locale(requested_locales, opt, {});

    // 10. Let style be ? GetOption(options, "style", string, « "narrow", "short", "long" », "long").
    auto style = TRY(get_option(vm, *options, vm.names.style, OptionType::String, { "narrow"sv, "short"sv, "long"sv }, "long"sv));

    // 11. Set displayNames.[[Style]] to style.
    display_names->set_style(style.as_string().utf8_string_view());

    // 12. Let type be ? GetOption(options, "type", string, « "language", "region", "script", "currency", "calendar", "dateTimeField" », undefined).
    auto type = TRY(get_option(vm, *options, vm.names.type, OptionType::String, { "language"sv, "region"sv, "script"sv, "currency"sv, "calendar"sv, "dateTimeField"sv }, Empty {}));

    // 13. If type is undefined, throw a TypeError exception.
    if (type.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "options.type"sv);

    // 14. Set displayNames.[[Type]] to type.
    display_names->set_type(type.as_string().utf8_string_view());

    // 15. Let fallback be ? GetOption(options, "fallback", string, « "code", "none" », "code").
    auto fallback = TRY(get_option(vm, *options, vm.names.fallback, OptionType::String, { "code"sv, "none"sv }, "code"sv));

    // 16. Set displayNames.[[Fallback]] to fallback.
    display_names->set_fallback(fallback.as_string().utf8_string_view());

    // 17. Set displayNames.[[Locale]] to r.[[Locale]].
    display_names->set_locale(move(result.locale));

    // 18. Let resolvedLocaleData be r.[[LocaleData]].
    // 19. Let types be resolvedLocaleData.[[types]].
    // 20. Assert: types is a Record (see 12.2.3).

    // 21. Let languageDisplay be ? GetOption(options, "languageDisplay", string, « "dialect", "standard" », "dialect").
    auto language_display = TRY(get_option(vm, *options, vm.names.languageDisplay, OptionType::String, { "dialect"sv, "standard"sv }, "dialect"sv));

    // 22. Let typeFields be types.[[<type>]].
    // 23. Assert: typeFields is a Record (see 12.2.3).

    // 24. If type is "language", then
    if (display_names->type() == DisplayNames::Type::Language) {
        // a. Set displayNames.[[LanguageDisplay]] to languageDisplay.
        display_names->set_language_display(language_display.as_string().utf8_string_view());

        // b. Set typeFields to typeFields.[[<languageDisplay>]].
        // c. Assert: typeFields is a Record (see 12.2.3).
    }

    // 25. Let styleFields be typeFields.[[<style>]].
    // 26. Assert: styleFields is a Record (see 12.2.3).
    // 27. Set displayNames.[[Fields]] to styleFields.

    // 28. Return displayNames.
    return display_names;
}

// 12.2.2 Intl.DisplayNames.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.DisplayNames.supportedLocalesOf
JS_DEFINE_NATIVE_FUNCTION(DisplayNamesConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %DisplayNames%.[[AvailableLocales]].
    // No-op, availability of each requested locale is checked via Unicode::is_locale_available()

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
