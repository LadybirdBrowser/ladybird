/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredContrast.h>

namespace Web::CSS {

PreferredContrast preferred_contrast_from_string(StringView value)
{
    if (value.equals_ignoring_ascii_case("less"sv))
        return PreferredContrast::Less;
    if (value.equals_ignoring_ascii_case("more"sv))
        return PreferredContrast::More;
    if (value.equals_ignoring_ascii_case("no-preference"sv))
        return PreferredContrast::NoPreference;
    return PreferredContrast::Auto;
}

StringView preferred_contrast_to_string(PreferredContrast value)
{
    switch (value) {
    case PreferredContrast::Auto:
        return "auto"sv;
    case PreferredContrast::Less:
        return "less"sv;
    case PreferredContrast::More:
        return "more"sv;
    case PreferredContrast::NoPreference:
        return "no-preference"sv;
    }
    VERIFY_NOT_REACHED();
}

}
