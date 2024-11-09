/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/PluralRules.h>

namespace Unicode {

PluralForm plural_form_from_string(StringView plural_form)
{
    if (plural_form == "cardinal"sv)
        return PluralForm::Cardinal;
    if (plural_form == "ordinal"sv)
        return PluralForm::Ordinal;
    VERIFY_NOT_REACHED();
}

StringView plural_form_to_string(PluralForm plural_form)
{
    switch (plural_form) {
    case PluralForm::Cardinal:
        return "cardinal"sv;
    case PluralForm::Ordinal:
        return "ordinal"sv;
    }
    VERIFY_NOT_REACHED();
}

PluralCategory plural_category_from_string(StringView category)
{
    if (category == "other"sv)
        return PluralCategory::Other;
    if (category == "zero"sv)
        return PluralCategory::Zero;
    if (category == "one"sv)
        return PluralCategory::One;
    if (category == "two"sv)
        return PluralCategory::Two;
    if (category == "few"sv)
        return PluralCategory::Few;
    if (category == "many"sv)
        return PluralCategory::Many;
    if (category == "0"sv)
        return PluralCategory::ExactlyZero;
    if (category == "1"sv)
        return PluralCategory::ExactlyOne;
    VERIFY_NOT_REACHED();
}

StringView plural_category_to_string(PluralCategory category)
{
    switch (category) {
    case PluralCategory::Other:
        return "other"sv;
    case PluralCategory::Zero:
        return "zero"sv;
    case PluralCategory::One:
        return "one"sv;
    case PluralCategory::Two:
        return "two"sv;
    case PluralCategory::Few:
        return "few"sv;
    case PluralCategory::Many:
        return "many"sv;
    case PluralCategory::ExactlyZero:
        return "0"sv;
    case PluralCategory::ExactlyOne:
        return "1"sv;
    }
    VERIFY_NOT_REACHED();
}

}
