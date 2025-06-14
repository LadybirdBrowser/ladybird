/*
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PreferredColorScheme.h>

namespace Web::CSS {

PreferredColorScheme preferred_color_scheme_from_string(StringView value)
{
    if (value.equals_ignoring_ascii_case("light"_sv))
        return PreferredColorScheme::Light;
    if (value.equals_ignoring_ascii_case("dark"_sv))
        return PreferredColorScheme::Dark;
    return PreferredColorScheme::Auto;
}

StringView preferred_color_scheme_to_string(PreferredColorScheme value)
{
    switch (value) {
    case PreferredColorScheme::Light:
        return "light"_sv;
    case PreferredColorScheme::Dark:
        return "dark"_sv;
    case PreferredColorScheme::Auto:
        return "auto"_sv;
    }
    VERIFY_NOT_REACHED();
}

}
