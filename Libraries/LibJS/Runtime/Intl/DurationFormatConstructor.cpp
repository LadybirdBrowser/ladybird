/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/DurationFormat.h>
#include <LibJS/Runtime/Intl/DurationFormatConstructor.h>
#include <LibUnicode/DurationFormat.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DurationFormatConstructor);

// 13.2 The Intl.DurationFormat Constructor, https://tc39.es/ecma402/#sec-intl-durationformat-constructor
DurationFormatConstructor::DurationFormatConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.DurationFormat.as_string(), realm.intrinsics().function_prototype())
{
}

void DurationFormatConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 1.3.1 Intl.DurationFormat.prototype, https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_duration_format_prototype(), 0);
    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);
}

// 13.2.1 Intl.DurationFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat
ThrowCompletionOr<Value> DurationFormatConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.DurationFormat");
}

// 13.2.1 Intl.DurationFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat
ThrowCompletionOr<GC::Ref<Object>> DurationFormatConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let durationFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.DurationFormatPrototype%", « [[InitializedDurationFormat]], [[Locale]], [[NumberingSystem]], [[Style]], [[YearsStyle]], [[YearsDisplay]], [[MonthsStyle]], [[MonthsDisplay]], [[WeeksStyle]], [[WeeksDisplay]], [[DaysStyle]], [[DaysDisplay]], [[HoursStyle]], [[HoursDisplay]], [[MinutesStyle]], [[MinutesDisplay]], [[SecondsStyle]], [[SecondsDisplay]], [[MillisecondsStyle]], [[MillisecondsDisplay]], [[MicrosecondsStyle]], [[MicrosecondsDisplay]], [[NanosecondsStyle]], [[NanosecondsDisplay]], [[HourMinuteSeparator]], [[MinuteSecondSeparator]], [[FractionalDigits]] »).
    auto duration_format = TRY(ordinary_create_from_constructor<DurationFormat>(vm, new_target, &Intrinsics::intl_duration_format_prototype));

    // 3. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 4. Let options be ? GetOptionsObject(options).
    auto options = TRY(get_options_object(vm, options_value));

    // 5. Let matcher be ? GetOption(options, "localeMatcher", STRING, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 6. Let numberingSystem be ? GetOption(options, "numberingSystem", STRING, EMPTY, undefined).
    auto numbering_system = TRY(get_option(vm, *options, vm.names.numberingSystem, OptionType::String, {}, Empty {}));

    // 7. If numberingSystem is not undefined, then
    if (!numbering_system.is_undefined()) {
        // a. If numberingSystem does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(numbering_system.as_string().utf8_string_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, numbering_system, "numberingSystem"sv);
    }

    // 8. Let opt be the Record { [[localeMatcher]]: matcher, [[nu]]: numberingSystem }.
    LocaleOptions opt {};
    opt.locale_matcher = matcher;
    opt.nu = locale_key_from_value(numbering_system);

    // 9. Let r be ResolveLocale(%Intl.DurationFormat%.[[AvailableLocales]], requestedLocales, opt, %Intl.DurationFormat%.[[RelevantExtensionKeys]], %Intl.DurationFormat%.[[LocaleData]]).
    auto result = resolve_locale(requested_locales, opt, DurationFormat::relevant_extension_keys());

    // 10. Set durationFormat.[[Locale]] to r.[[Locale]].
    duration_format->set_locale(move(result.locale));

    // 11. Let resolvedLocaleData be r.[[LocaleData]].

    // 12. Let digitalFormat be resolvedLocaleData.[[DigitalFormat]].
    auto digital_format = Unicode::digital_format(duration_format->locale());

    // 13. Set durationFormat.[[HourMinuteSeparator]] to digitalFormat.[[HourMinuteSeparator]].
    duration_format->set_hour_minute_separator(move(digital_format.hours_minutes_separator));

    // 14. Set durationFormat.[[MinuteSecondSeparator]] to digitalFormat.[[MinuteSecondSeparator]].
    duration_format->set_minute_second_separator(move(digital_format.minutes_seconds_separator));

    // 15. Set durationFormat.[[NumberingSystem]] to r.[[nu]].
    if (auto* resolved_numbering_system = result.nu.get_pointer<String>())
        duration_format->set_numbering_system(move(*resolved_numbering_system));

    // 16. Let style be ? GetOption(options, "style", STRING, « "long", "short", "narrow", "digital" », "short").
    auto style = TRY(get_option(vm, *options, vm.names.style, OptionType::String, { "long"sv, "short"sv, "narrow"sv, "digital"sv }, "short"sv));

    // 17. Set durationFormat.[[Style]] to style.
    duration_format->set_style(style.as_string().utf8_string_view());

    // 18. Let prevStyle be the empty String.
    Optional<DurationFormat::ValueStyle> previous_style;

    // 19. For each row of Table 22, except the header row, in table order, do
    for (auto const& duration_instances_component : duration_instances_components) {
        // a. Let styleSlot be the Style Slot value of the current row.
        auto style_slot = duration_instances_component.set_style_slot;

        // b. Let displaySlot be the Display Slot value of the current row.
        auto display_slot = duration_instances_component.set_display_slot;

        // c. Let unit be the Unit value of the current row.
        auto unit = duration_instances_component.unit;

        // d. Let valueList be the Values value of the current row.
        auto value_list = duration_instances_component.values;

        // e. Let digitalBase be the Digital Default value of the current row.
        auto digital_base = duration_instances_component.digital_default;

        // f. Let unitOptions be ? GetDurationUnitOptions(unit, options, style, valueList, digitalBase, prevStyle, digitalFormat.[[TwoDigitHours]]).
        auto unit_options = TRY(get_duration_unit_options(vm, unit, *options, duration_format->style(), value_list, digital_base, previous_style, digital_format.uses_two_digit_hours));

        // g. Set the value of the styleSlot slot of durationFormat to unitOptions.[[Style]].
        (duration_format->*style_slot)(unit_options.style);

        // h. Set the value of the displaySlot slot of durationFormat to unitOptions.[[Display]].
        (duration_format->*display_slot)(unit_options.display);

        // i. If unit is one of "hours", "minutes", "seconds", "milliseconds", or "microseconds", then
        if (first_is_one_of(unit, DurationFormat::Unit::Hours, DurationFormat::Unit::Minutes, DurationFormat::Unit::Seconds, DurationFormat::Unit::Milliseconds, DurationFormat::Unit::Microseconds)) {
            // i. Set prevStyle to unitOptions.[[Style]].
            previous_style = unit_options.style;
        }
    }

    // 20. Set durationFormat.[[FractionalDigits]] to ? GetNumberOption(options, "fractionalDigits", 0, 9, undefined).
    duration_format->set_fractional_digits(Optional<u8>(TRY(get_number_option(vm, *options, vm.names.fractionalDigits, 0, 9, {}))));

    // 21. Return durationFormat.
    return duration_format;
}

// 13.3.2 Intl.DurationFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.supportedLocalesOf
JS_DEFINE_NATIVE_FUNCTION(DurationFormatConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %DurationFormat%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

}
