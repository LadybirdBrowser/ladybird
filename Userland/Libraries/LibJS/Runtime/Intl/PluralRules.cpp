/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/PluralRules.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(PluralRules);

// 16 PluralRules Objects, https://tc39.es/ecma402/#pluralrules-objects
PluralRules::PluralRules(Object& prototype)
    : NumberFormatBase(prototype)
{
}

// 16.5.4 ResolvePlural ( pluralRules, n ), https://tc39.es/ecma402/#sec-resolveplural
::Locale::PluralCategory resolve_plural(PluralRules const& plural_rules, Value number)
{
    // 1. If n is not a finite Number, then
    if (!number.is_finite_number()) {
        // a. Let s be ! ToString(n).
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return ::Locale::PluralCategory::Other;
    }

    // 2. Let locale be pluralRules.[[Locale]].
    // 3. Let type be pluralRules.[[Type]].
    // 4. Let res be FormatNumericToString(pluralRules, ℝ(n)).
    // 5. Let s be res.[[FormattedString]].
    // 6. Let operands be GetOperands(s).
    // 7. Let p be PluralRuleSelect(locale, type, n, operands).
    // 8. Return the Record { [[PluralCategory]]: p, [[FormattedString]]: s }.
    return plural_rules.formatter().select_plural(number.as_double());
}

// 16.5.6 ResolvePluralRange ( pluralRules, x, y ), https://tc39.es/ecma402/#sec-resolveplural
ThrowCompletionOr<::Locale::PluralCategory> resolve_plural_range(VM& vm, PluralRules const& plural_rules, Value start, Value end)
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
    // 7. Return PluralRuleSelectRange(locale, type, xp.[[PluralCategory]], yp.[[PluralCategory]]).
    return plural_rules.formatter().select_plural_range(start.as_double(), end.as_double());
}

}
