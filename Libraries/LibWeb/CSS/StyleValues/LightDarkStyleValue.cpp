/*
 * Copyright (c) 2025-present, the Ladybird developers.
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LightDarkStyleValue.h"

namespace Web::CSS {

Optional<Color> LightDarkStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    if (color_resolution_context.color_scheme == PreferredColorScheme::Dark)
        return dark()->to_color(color_resolution_context);

    return light()->to_color(color_resolution_context);
}

ValueComparingNonnullRefPtr<StyleValue const> LightDarkStyleValue::absolutized(ComputationContext const& context) const
{
    if (!context.color_scheme.has_value())
        return *this;

    if (context.color_scheme == PreferredColorScheme::Dark)
        return dark()->absolutized(context);

    return light()->absolutized(context);
}

}
