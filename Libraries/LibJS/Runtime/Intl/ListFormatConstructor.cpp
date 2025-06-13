/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Intl/ListFormatConstructor.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(ListFormatConstructor);

// 14.1 The Intl.ListFormat Constructor, https://tc39.es/ecma402/#sec-intl-listformat-constructor
ListFormatConstructor::ListFormatConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.ListFormat.as_string(), realm.intrinsics().function_prototype())
{
}

void ListFormatConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 14.2.1 Intl.ListFormat.prototype, https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_list_format_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 14.1.1 Intl.ListFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.ListFormat
ThrowCompletionOr<Value> ListFormatConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.ListFormat");
}

// 14.1.1 Intl.ListFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.ListFormat
ThrowCompletionOr<GC::Ref<Object>> ListFormatConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let listFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.ListFormat.prototype%", « [[InitializedListFormat]], [[Locale]], [[Type]], [[Style]], [[Templates]] »).
    auto list_format = TRY(ordinary_create_from_constructor<ListFormat>(vm, new_target, &Intrinsics::intl_list_format_prototype));

    // 3. Let optionsResolution be ? ResolveOptions(%Intl.ListFormat%, %Intl.ListFormat%.[[LocaleData]], locales, options).
    // 4. Set options to optionsResolution.[[Options]].
    // 5. Let r be optionsResolution.[[ResolvedLocale]].
    auto [options, result, _] = TRY(resolve_options(vm, list_format, locales_value, options_value));

    // 6. Set listFormat.[[Locale]] to r.[[Locale]].
    list_format->set_locale(move(result.locale));

    // 7. Let type be ? GetOption(options, "type", string, « "conjunction", "disjunction", "unit" », "conjunction").
    auto type = TRY(get_option(vm, *options, vm.names.type, OptionType::String, { "conjunction"sv, "disjunction"sv, "unit"sv }, "conjunction"sv));

    // 8. Set listFormat.[[Type]] to type.
    list_format->set_type(type.as_string().utf8_string_view());

    // 9. Let style be ? GetOption(options, "style", string, « "long", "short", "narrow" », "long").
    auto style = TRY(get_option(vm, *options, vm.names.style, OptionType::String, { "long"sv, "short"sv, "narrow"sv }, "long"sv));

    // 10. Set listFormat.[[Style]] to style.
    list_format->set_style(style.as_string().utf8_string_view());

    // 11. Let resolvedLocaleData be r.[[LocaleData]].
    // 12. Let dataLocaleTypes be resolvedLocaleData.[[<type>]].
    // 13. Set listFormat.[[Templates]] to dataLocaleTypes.[[<style>]].
    auto formatter = Unicode::ListFormat::create(
        list_format->locale(),
        list_format->type(),
        list_format->style());
    list_format->set_formatter(move(formatter));

    // 14. Return listFormat.
    return list_format;
}

// 14.2.2 Intl.ListFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.ListFormat.supportedLocalesOf
JS_DEFINE_NATIVE_FUNCTION(ListFormatConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %ListFormat%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
