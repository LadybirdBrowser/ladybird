/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Keyword.h>

namespace Web::CSS {

String Display::to_string() const
{
    if (auto keyword = to_keyword(); keyword.has_value())
        return String::from_utf8_without_validation(string_from_keyword(keyword.value()).bytes());

    VERIFY(m_type == Type::OutsideAndInside);

    Vector<StringView, 3> parts;
    if (!(m_value.outside_inside.outside == DisplayOutside::Block && m_value.outside_inside.inside == DisplayInside::FlowRoot))
        parts.unchecked_append(CSS::to_string(m_value.outside_inside.outside));
    if (m_value.outside_inside.inside != DisplayInside::Flow)
        parts.unchecked_append(CSS::to_string(m_value.outside_inside.inside));
    if (m_value.outside_inside.list_item == ListItem::Yes)
        parts.unchecked_append("list-item"sv);
    return MUST(String::join(' ', parts));
}

Display Display::from_short(Short short_)
{
    switch (short_) {
    case Short::None:
        return Display { DisplayBox::None };
    case Short::Contents:
        return Display { DisplayBox::Contents };
    case Short::Block:
        return Display { DisplayOutside::Block, DisplayInside::Flow };
    case Short::Inline:
        return Display { DisplayOutside::Inline, DisplayInside::Flow };
    case Short::Flow:
        return Display { DisplayOutside::Block, DisplayInside::Flow };
    case Short::FlowRoot:
        return Display { DisplayOutside::Block, DisplayInside::FlowRoot };
    case Short::InlineBlock:
        return Display { DisplayOutside::Inline, DisplayInside::FlowRoot };
    case Short::RunIn:
        return Display { DisplayOutside::RunIn, DisplayInside::Flow };
    case Short::ListItem:
        return Display { DisplayOutside::Block, DisplayInside::Flow, ListItem::Yes };
    case Short::InlineListItem:
        return Display { DisplayOutside::Inline, DisplayInside::Flow, ListItem::Yes };
    case Short::Flex:
        return Display { DisplayOutside::Block, DisplayInside::Flex };
    case Short::InlineFlex:
        return Display { DisplayOutside::Inline, DisplayInside::Flex };
    case Short::Grid:
        return Display { DisplayOutside::Block, DisplayInside::Grid };
    case Short::InlineGrid:
        return Display { DisplayOutside::Inline, DisplayInside::Grid };
    case Short::Ruby:
        return Display { DisplayOutside::Inline, DisplayInside::Ruby };
    case Short::Table:
        return Display { DisplayOutside::Block, DisplayInside::Table };
    case Short::InlineTable:
        return Display { DisplayOutside::Inline, DisplayInside::Table };
    case Short::Math:
        // NOTE: The spec ( https://w3c.github.io/mathml-core/#new-display-math-value ) does not
        //       mention what the outside value for `display: math` should be but other browsers
        //       use `inline` so let's go with that.
        return Display { DisplayOutside::Inline, DisplayInside::Math };
    }
    VERIFY_NOT_REACHED();
}

Optional<Keyword> Display::to_keyword() const
{
    switch (m_type) {
    case Type::Box:
        return keyword_from_string(CSS::to_string(m_value.box));
    case Type::Internal:
        return keyword_from_string(CSS::to_string(m_value.internal));
    case Type::OutsideAndInside:
        // NOTE: Following the precedence rules of "most backwards-compatible, then shortest",
        //       serialization of equivalent display values uses the "Short display" column.
        if (*this == Display::from_short(Display::Short::None))
            return Keyword::None;
        if (*this == Display::from_short(Display::Short::Contents))
            return Keyword::Contents;
        if (*this == Display::from_short(Display::Short::Block))
            return Keyword::Block;
        if (*this == Display::from_short(Display::Short::FlowRoot))
            return Keyword::FlowRoot;
        if (*this == Display::from_short(Display::Short::Inline))
            return Keyword::Inline;
        if (*this == Display::from_short(Display::Short::InlineBlock))
            return Keyword::InlineBlock;
        if (*this == Display::from_short(Display::Short::RunIn))
            return Keyword::RunIn;
        if (*this == Display::from_short(Display::Short::ListItem))
            return Keyword::ListItem;
        if (*this == Display::from_short(Display::Short::Flex))
            return Keyword::Flex;
        if (*this == Display::from_short(Display::Short::InlineFlex))
            return Keyword::InlineFlex;
        if (*this == Display::from_short(Display::Short::Grid))
            return Keyword::Grid;
        if (*this == Display::from_short(Display::Short::InlineGrid))
            return Keyword::InlineGrid;
        if (*this == Display::from_short(Display::Short::Ruby))
            return Keyword::Ruby;
        if (*this == Display::from_short(Display::Short::Table))
            return Keyword::Table;
        if (*this == Display::from_short(Display::Short::InlineTable))
            return Keyword::InlineTable;
        if (*this == Display::from_short(Display::Short::Math))
            return Keyword::Math;

        return {};
    }
    VERIFY_NOT_REACHED();
}

}
