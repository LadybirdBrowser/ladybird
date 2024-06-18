/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/DisplayNames.h>
#include <LibJS/Runtime/Intl/DisplayNamesConstructor.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibLocale/Locale.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(DisplayNamesConstructor);

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
ThrowCompletionOr<NonnullGCPtr<Object>> DisplayNamesConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locale_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let displayNames be ? OrdinaryCreateFromConstructor(NewTarget, "%DisplayNames.prototype%", « [[InitializedDisplayNames]], [[Locale]], [[Style]], [[Type]], [[Fallback]], [[LanguageDisplay]], [[Fields]] »).
    auto display_names = TRY(ordinary_create_from_constructor<DisplayNames>(vm, new_target, &Intrinsics::intl_display_names_prototype));

    // 3. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locale_value));

    // 4. If options is undefined, throw a TypeError exception.
    if (options_value.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "options"sv);

    // 5. Set options to ? GetOptionsObject(options).
    auto* options = TRY(Temporal::get_options_object(vm, options_value));

    // 6. Let opt be a new Record.
    LocaleOptions opt {};

    // 7. Let localeData be %DisplayNames%.[[LocaleData]].

    // 8. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 9. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 10. Let r be ResolveLocale(%DisplayNames%.[[AvailableLocales]], requestedLocales, opt, %DisplayNames%.[[RelevantExtensionKeys]]).
    auto result = resolve_locale(requested_locales, opt, {});

    // 11. Let style be ? GetOption(options, "style", string, « "narrow", "short", "long" », "long").
    auto style = TRY(get_option(vm, *options, vm.names.style, OptionType::String, { "narrow"sv, "short"sv, "long"sv }, "long"sv));

    // 12. Set displayNames.[[Style]] to style.
    display_names->set_style(style.as_string().utf8_string_view());

    // 13. Let type be ? GetOption(options, "type", string, « "language", "region", "script", "currency", "calendar", "dateTimeField" », undefined).
    auto type = TRY(get_option(vm, *options, vm.names.type, OptionType::String, { "language"sv, "region"sv, "script"sv, "currency"sv, "calendar"sv, "dateTimeField"sv }, Empty {}));

    // 14. If type is undefined, throw a TypeError exception.
    if (type.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "options.type"sv);

    // 15. Set displayNames.[[Type]] to type.
    display_names->set_type(type.as_string().utf8_string_view());

    // 16. Let fallback be ? GetOption(options, "fallback", string, « "code", "none" », "code").
    auto fallback = TRY(get_option(vm, *options, vm.names.fallback, OptionType::String, { "code"sv, "none"sv }, "code"sv));

    // 17. Set displayNames.[[Fallback]] to fallback.
    display_names->set_fallback(fallback.as_string().utf8_string_view());

    // 18. Set displayNames.[[Locale]] to r.[[locale]].
    display_names->set_locale(move(result.locale));

    // Note: Several of the steps below are skipped in favor of deferring to LibUnicode.

    // 19. Let dataLocale be r.[[dataLocale]].
    // 20. Let dataLocaleData be localeData.[[<dataLocale>]].
    // 21. Let types be dataLocaleData.[[types]].
    // 22. Assert: types is a Record (see 12.4.3).

    // 23. Let languageDisplay be ? GetOption(options, "languageDisplay", string, « "dialect", "standard" », "dialect").
    auto language_display = TRY(get_option(vm, *options, vm.names.languageDisplay, OptionType::String, { "dialect"sv, "standard"sv }, "dialect"sv));

    // 24. Let typeFields be types.[[<type>]].
    // 25. Assert: typeFields is a Record (see 12.4.3).

    // 26. If type is "language", then
    if (display_names->type() == DisplayNames::Type::Language) {
        // a. Set displayNames.[[LanguageDisplay]] to languageDisplay.
        display_names->set_language_display(language_display.as_string().utf8_string_view());

        // b. Let typeFields be typeFields.[[<languageDisplay>]].
        // c. Assert: typeFields is a Record (see 12.4.3).
    }

    // 27. Let styleFields be typeFields.[[<style>]].
    // 28. Assert: styleFields is a Record (see 12.4.3).
    // 29. Set displayNames.[[Fields]] to styleFields.

    // 30. Return displayNames.
    return display_names;
}

// 12.2.2 Intl.DisplayNames.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.DisplayNames.supportedLocalesOf
JS_DEFINE_NATIVE_FUNCTION(DisplayNamesConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %DisplayNames%.[[AvailableLocales]].
    // No-op, availability of each requested locale is checked via ::Locale::is_locale_available()

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
