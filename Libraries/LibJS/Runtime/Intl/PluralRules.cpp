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
Unicode::PluralCategory resolve_plural(PluralRules const& plural_rules, Value number)
{
    // 1. If n is not a finite Number, then
    if (!number.is_finite_number()) {
        // a. Let s be ! ToString(n).
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return Unicode::PluralCategory::Other;
    }

    // 2. Let res be FormatNumericToString(pluralRules, ℝ(n)).
    // 3. Let s be res.[[FormattedString]].
    // 4. Let locale be pluralRules.[[Locale]].
    // 5. Let type be pluralRules.[[Type]].
    // 6. Let notation be pluralRules.[[Notation]].
    // 7. Let p be PluralRuleSelect(locale, type, notation, s).
    // 8. Return the Record { [[PluralCategory]]: p, [[FormattedString]]: s }.
    return plural_rules.formatter().select_plural(number.as_double());
}

// 17.5.4 ResolvePluralRange ( pluralRules, x, y ), https://tc39.es/ecma402/#sec-resolveplural
ThrowCompletionOr<Unicode::PluralCategory> resolve_plural_range(VM& vm, PluralRules const& plural_rules, Value start, Value end)
{
    // 1. If x is NaN or y is NaN, throw a RangeError exception.
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
    // 8. Return PluralRuleSelectRange(locale, type, notation, xp.[[PluralCategory]], yp.[[PluralCategory]]).
    return plural_rules.formatter().select_plural_range(start.as_double(), end.as_double());
}

}
