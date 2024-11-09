/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Display.h>

namespace Web::CSS {

String Display::to_string() const
{
    StringBuilder builder;
    switch (m_type) {
    case Type::OutsideAndInside:
        // NOTE: Following the precedence rules of “most backwards-compatible, then shortest”,
        //       serialization of equivalent display values uses the “Short display” column.
        if (*this == Display::from_short(Display::Short::Block))
            return "block"_string;
        if (*this == Display::from_short(Display::Short::FlowRoot))
            return "flow-root"_string;
        if (*this == Display::from_short(Display::Short::Inline))
            return "inline"_string;
        if (*this == Display::from_short(Display::Short::InlineBlock))
            return "inline-block"_string;
        if (*this == Display::from_short(Display::Short::RunIn))
            return "run-in"_string;
        if (*this == Display::from_short(Display::Short::ListItem))
            return "list-item"_string;
        if (*this == Display::from_short(Display::Short::Flex))
            return "flex"_string;
        if (*this == Display::from_short(Display::Short::InlineFlex))
            return "inline-flex"_string;
        if (*this == Display::from_short(Display::Short::Grid))
            return "grid"_string;
        if (*this == Display::from_short(Display::Short::InlineGrid))
            return "inline-grid"_string;
        if (*this == Display::from_short(Display::Short::Ruby))
            return "ruby"_string;
        if (*this == Display::from_short(Display::Short::Table))
            return "table"_string;
        if (*this == Display::from_short(Display::Short::InlineTable))
            return "inline-table"_string;

        builder.append(CSS::to_string(m_value.outside_inside.outside));
        builder.append(' ');
        builder.append(CSS::to_string(m_value.outside_inside.inside));
        if (m_value.outside_inside.list_item == ListItem::Yes)
            builder.append(" list-item"sv);
        break;
    case Type::Internal:
        builder.append(CSS::to_string(m_value.internal));
        break;
    case Type::Box:
        builder.append(CSS::to_string(m_value.box));
        break;
    };
    return MUST(builder.to_string());
}

}
