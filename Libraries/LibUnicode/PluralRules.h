/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Unicode {

enum class PluralForm {
    Cardinal,
    Ordinal,
};
PluralForm plural_form_from_string(StringView);
StringView plural_form_to_string(PluralForm);

enum class PluralCategory {
    // NOTE: These are sorted in preferred order for Intl.PluralRules.prototype.resolvedOptions.
    Zero,
    One,
    Two,
    Few,
    Many,
    Other,

    // https://unicode.org/reports/tr35/tr35-numbers.html#Explicit_0_1_rules
    ExactlyZero,
    ExactlyOne,
};
PluralCategory plural_category_from_string(StringView);
StringView plural_category_to_string(PluralCategory);

}
