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

// 13.1 The Intl.DurationFormat Constructor, https://tc39.es/ecma402/#sec-intl-durationformat-constructor
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

// 13.1.1 Intl.DurationFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat
ThrowCompletionOr<Value> DurationFormatConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.DurationFormat");
}

// 13.1.1 Intl.DurationFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat
ThrowCompletionOr<GC::Ref<Object>> DurationFormatConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let durationFormat be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.DurationFormatPrototype%", « [[InitializedDurationFormat]], [[Locale]], [[NumberingSystem]], [[Style]], [[YearsOptions]], [[MonthsOptions]], [[WeeksOptions]], [[DaysOptions]], [[HoursOptions]], [[MinutesOptions]], [[SecondsOptions]], [[MillisecondsOptions]], [[MicrosecondsOptions]], [[NanosecondsOptions]], [[HourMinuteSeparator]], [[MinuteSecondSeparator]], [[FractionalDigits]] »).
    auto duration_format = TRY(ordinary_create_from_constructor<DurationFormat>(vm, new_target, &Intrinsics::intl_duration_format_prototype));

    // 3. Let optionsResolution be ? ResolveOptions(%Intl.DurationFormat%, %Intl.DurationFormat%.[[LocaleData]], locales, options).
    // 4. Set options to optionsResolution.[[Options]].
    // 5. Let r be optionsResolution.[[ResolvedLocale]].
    auto [options, result, _] = TRY(resolve_options(vm, duration_format, locales_value, options_value));

    // 6. Set durationFormat.[[Locale]] to r.[[Locale]].
    duration_format->set_locale(move(result.locale));

    // 7. Let resolvedLocaleData be r.[[LocaleData]].

    // 8. Let digitalFormat be resolvedLocaleData.[[DigitalFormat]].
    auto digital_format = Unicode::digital_format(result.icu_locale);

    // 9. Set durationFormat.[[HourMinuteSeparator]] to digitalFormat.[[HourMinuteSeparator]].
    duration_format->set_hour_minute_separator(move(digital_format.hours_minutes_separator));

    // 10. Set durationFormat.[[MinuteSecondSeparator]] to digitalFormat.[[MinuteSecondSeparator]].
    duration_format->set_minute_second_separator(move(digital_format.minutes_seconds_separator));

    // 11. Set durationFormat.[[NumberingSystem]] to r.[[nu]].
    if (auto* resolved_numbering_system = result.nu.get_pointer<String>())
        duration_format->set_numbering_system(move(*resolved_numbering_system));

    // 12. Let style be ? GetOption(options, "style", STRING, « "long", "short", "narrow", "digital" », "short").
    auto style = TRY(get_option(vm, *options, vm.names.style, OptionType::String, { "long"sv, "short"sv, "narrow"sv, "digital"sv }, "short"sv));

    // 13. Set durationFormat.[[Style]] to style.
    duration_format->set_style(style.as_string().utf8_string_view());

    // 14. Let prevStyle be the empty String.
    Optional<DurationFormat::ValueStyle> previous_style;

    // 15. For each row of Table 20, except the header row, in table order, do
    for (auto const& duration_instances_component : duration_instances_components) {
        // a. Let slot be the Internal Slot value of the current row.
        auto slot = duration_instances_component.set_internal_slot;

        // b. Let unit be the Unit value of the current row.
        auto unit = duration_instances_component.unit;

        // c. Let styles be the Styles value of the current row.
        auto styles = duration_instances_component.styles;

        // d. Let digitalBase be the Digital Default value of the current row.
        auto digital_base = duration_instances_component.digital_default;

        // e. Let unitOptions be ? GetDurationUnitOptions(unit, options, style, styles, digitalBase, prevStyle, digitalFormat.[[TwoDigitHours]]).
        auto unit_options = TRY(get_duration_unit_options(vm, unit, *options, duration_format->style(), styles, digital_base, previous_style, digital_format.uses_two_digit_hours));

        // f. Set the value of durationFormat's internal slot whose name is slot to unitOptions.
        (duration_format->*slot)(unit_options);

        // g. If unit is one of "hours", "minutes", "seconds", "milliseconds", or "microseconds", then
        if (first_is_one_of(unit, DurationFormat::Unit::Hours, DurationFormat::Unit::Minutes, DurationFormat::Unit::Seconds, DurationFormat::Unit::Milliseconds, DurationFormat::Unit::Microseconds)) {
            // i. Set prevStyle to unitOptions.[[Style]].
            previous_style = unit_options.style;
        }
    }

    // 16. Set durationFormat.[[FractionalDigits]] to ? GetNumberOption(options, "fractionalDigits", 0, 9, undefined).
    duration_format->set_fractional_digits(Optional<u8>(TRY(get_number_option(vm, *options, vm.names.fractionalDigits, 0, 9, {}))));

    // 17. Return durationFormat.
    return duration_format;
}

// 13.2.2 Intl.DurationFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.supportedLocalesOf
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
