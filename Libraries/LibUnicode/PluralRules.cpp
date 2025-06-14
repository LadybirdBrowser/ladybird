/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/PluralRules.h>

namespace Unicode {

PluralForm plural_form_from_string(StringView plural_form)
{
    if (plural_form == "cardinal"_sv)
        return PluralForm::Cardinal;
    if (plural_form == "ordinal"_sv)
        return PluralForm::Ordinal;
    VERIFY_NOT_REACHED();
}

StringView plural_form_to_string(PluralForm plural_form)
{
    switch (plural_form) {
    case PluralForm::Cardinal:
        return "cardinal"_sv;
    case PluralForm::Ordinal:
        return "ordinal"_sv;
    }
    VERIFY_NOT_REACHED();
}

PluralCategory plural_category_from_string(StringView category)
{
    if (category == "other"_sv)
        return PluralCategory::Other;
    if (category == "zero"_sv)
        return PluralCategory::Zero;
    if (category == "one"_sv)
        return PluralCategory::One;
    if (category == "two"_sv)
        return PluralCategory::Two;
    if (category == "few"_sv)
        return PluralCategory::Few;
    if (category == "many"_sv)
        return PluralCategory::Many;
    if (category == "0"_sv)
        return PluralCategory::ExactlyZero;
    if (category == "1"_sv)
        return PluralCategory::ExactlyOne;
    VERIFY_NOT_REACHED();
}

StringView plural_category_to_string(PluralCategory category)
{
    switch (category) {
    case PluralCategory::Other:
        return "other"_sv;
    case PluralCategory::Zero:
        return "zero"_sv;
    case PluralCategory::One:
        return "one"_sv;
    case PluralCategory::Two:
        return "two"_sv;
    case PluralCategory::Few:
        return "few"_sv;
    case PluralCategory::Many:
        return "many"_sv;
    case PluralCategory::ExactlyZero:
        return "0"_sv;
    case PluralCategory::ExactlyOne:
        return "1"_sv;
    }
    VERIFY_NOT_REACHED();
}

}
