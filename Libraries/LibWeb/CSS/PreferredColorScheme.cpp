/*
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredColorScheme.h>

namespace Web::CSS {

PreferredColorScheme preferred_color_scheme_from_string(Utf16View value)
{
    if (value.equals_ignoring_ascii_case(u"light"sv))
        return PreferredColorScheme::Light;
    if (value.equals_ignoring_ascii_case(u"dark"sv))
        return PreferredColorScheme::Dark;
    return PreferredColorScheme::Auto;
}

Utf16FlyString preferred_color_scheme_to_utf16_fly_string(PreferredColorScheme value)
{
    switch (value) {
    case PreferredColorScheme::Light:
        return "light"_utf16_fly_string;
    case PreferredColorScheme::Dark:
        return "dark"_utf16_fly_string;
    case PreferredColorScheme::Auto:
        return "auto"_utf16_fly_string;
    }
    VERIFY_NOT_REACHED();
}

}
