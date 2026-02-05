/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredMotion.h>

namespace Web::CSS {

PreferredMotion preferred_motion_from_string(StringView value)
{
    if (value.equals_ignoring_ascii_case("no-preference"sv))
        return PreferredMotion::NoPreference;
    if (value.equals_ignoring_ascii_case("reduce"sv))
        return PreferredMotion::Reduce;
    return PreferredMotion::Auto;
}

StringView preferred_motion_to_string(PreferredMotion value)
{
    switch (value) {
    case PreferredMotion::Auto:
        return "auto"sv;
    case PreferredMotion::NoPreference:
        return "no-preference"sv;
    case PreferredMotion::Reduce:
        return "reduce"sv;
    }
    VERIFY_NOT_REACHED();
}

}
