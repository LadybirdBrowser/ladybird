/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLightDark.h"
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

Color CSSLightDark::to_color(Optional<Layout::NodeWithStyle const&> node) const
{
    if (node.has_value() && node.value().computed_values().color_scheme() == PreferredColorScheme::Dark)
        return m_properties.dark->to_color(node);

    return m_properties.light->to_color(node);
}

bool CSSLightDark::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_light_dark = as<CSSLightDark>(other_color);
    return m_properties == other_light_dark.m_properties;
}

String CSSLightDark::to_string(SerializationMode mode) const
{
    // FIXME: We don't have enough information to determine the computed value here.
    return MUST(String::formatted("light-dark({}, {})", m_properties.light->to_string(mode), m_properties.dark->to_string(mode)));
}

}
