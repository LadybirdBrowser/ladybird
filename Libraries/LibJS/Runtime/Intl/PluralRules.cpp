/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(PluralRules);

// 17 PluralRules Objects, https://tc39.es/ecma402/#pluralrules-objects
PluralRules::PluralRules(Object& prototype)
    : NumberFormatBase(prototype)
{
}

// 17.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.pluralrules-internal-slots
ReadonlySpan<StringView> PluralRules::relevant_extension_keys() const
{
    // The value of the [[RelevantExtensionKeys]] internal slot is « ».
    return {};
}

// 17.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.pluralrules-internal-slots
ReadonlySpan<ResolutionOptionDescriptor> PluralRules::resolution_option_descriptors(VM&) const
{
    // The value of the [[ResolutionOptionDescriptors]] internal slot is « ».
    return {};
}

// 17.5.2 ResolvePlural ( pluralRules, n ), https://tc39.es/ecma402/#sec-resolveplural
Unicode::PluralCategory resolve_plural(PluralRules const& plural_rules, MathematicalValue const& number)
{
    // 1. If n is NOT-A-NUMBER, then
    if (number.is_nan()) {
        // a. Let s be an ILD String value indicating the NaN value.
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return Unicode::PluralCategory::Other;
    }

    // 2. If n is POSITIVE-INFINITY, then
    if (number.is_positive_infinity()) {
        // a. Let s be an ILD String value indicating positive infinity.
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return Unicode::PluralCategory::Other;
    }

    // 3. If n is NEGATIVE-INFINITY, then
    if (number.is_negative_infinity()) {
        // a. Let s be an ILD String value indicating negative infinity.
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return Unicode::PluralCategory::Other;
    }

    // 4. Let res be FormatNumericToString(pluralRules, n).
    // 5. Let s be res.[[FormattedString]].
    // 6. Let locale be pluralRules.[[Locale]].
    // 7. Let type be pluralRules.[[Type]].
    // 8. Let notation be pluralRules.[[Notation]].
    // 9. Let compactDisplay be pluralRules.[[CompactDisplay]].
    // 10. Let p be PluralRuleSelect(locale, type, notation, compactDisplay, s).
    // 11. Return the Record { [[PluralCategory]]: p, [[FormattedString]]: s }.
    return plural_rules.formatter().select_plural(number.to_value());
}

// 17.5.4 ResolvePluralRange ( pluralRules, x, y ), https://tc39.es/ecma402/#sec-resolvepluralrange
ThrowCompletionOr<Unicode::PluralCategory> resolve_plural_range(VM& vm, PluralRules const& plural_rules, MathematicalValue const& start, MathematicalValue const& end)
{
    // 1. If x is NOT-A-NUMBER or y is NOT-A-NUMBER, throw a RangeError exception.
    if (start.is_nan())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "start"sv);
    if (end.is_nan())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "end"sv);

    // 2. Let xp be ResolvePlural(pluralRules, x).
    // 3. Let yp be ResolvePlural(pluralRules, y).
    // 4. If xp.[[FormattedString]] is yp.[[FormattedString]], then
    //     a. Return xp.[[PluralCategory]].
    // 5. Let locale be pluralRules.[[Locale]].
    // 6. Let type be pluralRules.[[Type]].
    // 7. Let notation be pluralRules.[[Notation]].
    // 8. Let compactDisplay be pluralRules.[[CompactDisplay]].
    // 9. Return PluralRuleSelectRange(locale, type, notation, compactDisplay, xp.[[PluralCategory]], yp.[[PluralCategory]]).
    return plural_rules.formatter().select_plural_range(start.to_value(), end.to_value());
}

}
