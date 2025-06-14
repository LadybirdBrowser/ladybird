/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredContrast.h>

namespace Web::CSS {

PreferredContrast preferred_contrast_from_string(StringView value)
{
    if (value.equals_ignoring_ascii_case("less"_sv))
        return PreferredContrast::Less;
    if (value.equals_ignoring_ascii_case("more"_sv))
        return PreferredContrast::More;
    if (value.equals_ignoring_ascii_case("no-preference"_sv))
        return PreferredContrast::NoPreference;
    return PreferredContrast::Auto;
}

StringView preferred_contrast_to_string(PreferredContrast value)
{
    switch (value) {
    case PreferredContrast::Auto:
        return "auto"_sv;
    case PreferredContrast::Less:
        return "less"_sv;
    case PreferredContrast::More:
        return "more"_sv;
    case PreferredContrast::NoPreference:
        return "no-preference"_sv;
    }
    VERIFY_NOT_REACHED();
}

}
