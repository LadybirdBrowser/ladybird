/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredMotion.h>

namespace Web::CSS {

PreferredMotion preferred_motion_from_string(Utf16View value)
{
    if (value.equals_ignoring_ascii_case(u"no-preference"sv))
        return PreferredMotion::NoPreference;
    if (value.equals_ignoring_ascii_case(u"reduce"sv))
        return PreferredMotion::Reduce;
    return PreferredMotion::Auto;
}

}
