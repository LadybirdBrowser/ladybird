/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredContrast.h>

namespace Web::CSS {

PreferredContrast preferred_contrast_from_string(Utf16View value)
{
    if (value.equals_ignoring_ascii_case(u"less"sv))
        return PreferredContrast::Less;
    if (value.equals_ignoring_ascii_case(u"more"sv))
        return PreferredContrast::More;
    if (value.equals_ignoring_ascii_case(u"no-preference"sv))
        return PreferredContrast::NoPreference;
    return PreferredContrast::Auto;
}

}
