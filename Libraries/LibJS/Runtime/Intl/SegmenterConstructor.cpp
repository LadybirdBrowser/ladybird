/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Intl/SegmenterConstructor.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(SegmenterConstructor);

// 19.1 The Intl.Segmenter Constructor, https://tc39.es/ecma402/#sec-intl-segmenter-constructor
SegmenterConstructor::SegmenterConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Segmenter.as_string(), realm.intrinsics().function_prototype())
{
}

void SegmenterConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 19.2.1 Intl.Segmenter.prototype, https://tc39.es/ecma402/#sec-intl.segmenter.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_segmenter_prototype(), 0);
    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);
}

// 19.1.1 Intl.Segmenter ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.segmenter
ThrowCompletionOr<Value> SegmenterConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.Segmenter");
}

// 19.1.1 Intl.Segmenter ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.segmenter
ThrowCompletionOr<GC::Ref<Object>> SegmenterConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let internalSlotsList be « [[InitializedSegmenter]], [[Locale]], [[SegmenterGranularity]] ».
    // 3. Let segmenter be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.Segmenter.prototype%", internalSlotsList).
    auto segmenter = TRY(ordinary_create_from_constructor<Segmenter>(vm, new_target, &Intrinsics::intl_segmenter_prototype));

    // 4. Let optionsResolution be ? ResolveOptions(%Intl.Segmenter%, %Intl.Segmenter%.[[LocaleData]], locales, options).
    // 5. Set options to optionsResolution.[[Options]].
    // 6. Let r be optionsResolution.[[ResolvedLocale]].
    auto [options, result, _] = TRY(resolve_options(vm, segmenter, locales_value, options_value));

    // 7. Set segmenter.[[Locale]] to r.[[locale]].
    segmenter->set_locale(move(result.locale));

    // 8. Let granularity be ? GetOption(options, "granularity", string, « "grapheme", "word", "sentence" », "grapheme").
    auto granularity = TRY(get_option(vm, *options, vm.names.granularity, OptionType::String, { "grapheme"sv, "word"sv, "sentence"sv }, "grapheme"sv));

    // 9. Set segmenter.[[SegmenterGranularity]] to granularity.
    segmenter->set_segmenter_granularity(granularity.as_string().utf8_string_view());

    auto locale_segmenter = Unicode::Segmenter::create(segmenter->locale(), segmenter->segmenter_granularity());
    segmenter->set_segmenter(move(locale_segmenter));

    // 10. Return segmenter.
    return segmenter;
}

// 19.2.2 Intl.Segmenter.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.segmenter.supportedlocalesof
JS_DEFINE_NATIVE_FUNCTION(SegmenterConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %Intl.Segmenter%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
