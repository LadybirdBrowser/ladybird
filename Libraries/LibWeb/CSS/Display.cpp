/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/Enums.h>
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

        {
            Vector<StringView, 3> parts;
            if (!(m_value.outside_inside.outside == DisplayOutside::Block && m_value.outside_inside.inside == DisplayInside::FlowRoot))
                parts.append(CSS::to_string(m_value.outside_inside.outside));
            if (m_value.outside_inside.inside != DisplayInside::Flow)
                parts.append(CSS::to_string(m_value.outside_inside.inside));
            if (m_value.outside_inside.list_item == ListItem::Yes)
                parts.append("list-item"sv);
            builder.join(' ', parts);
        }
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

bool Display::is_table_column() const
{
    return is_internal() && internal() == DisplayInternal::TableColumn;
}

bool Display::is_table_row_group() const
{
    return is_internal() && internal() == DisplayInternal::TableRowGroup;
}

bool Display::is_table_header_group() const
{
    return is_internal() && internal() == DisplayInternal::TableHeaderGroup;
}

bool Display::is_table_footer_group() const
{
    return is_internal() && internal() == DisplayInternal::TableFooterGroup;
}

bool Display::is_table_row() const
{
    return is_internal() && internal() == DisplayInternal::TableRow;
}

bool Display::is_table_cell() const
{
    return is_internal() && internal() == DisplayInternal::TableCell;
}

bool Display::is_table_column_group() const
{
    return is_internal() && internal() == DisplayInternal::TableColumnGroup;
}

bool Display::is_table_caption() const
{
    return is_internal() && internal() == DisplayInternal::TableCaption;
}

bool Display::is_internal_table() const
{
    return is_internal() && (internal() == DisplayInternal::TableRowGroup || internal() == DisplayInternal::TableHeaderGroup || internal() == DisplayInternal::TableFooterGroup || internal() == DisplayInternal::TableRow || internal() == DisplayInternal::TableCell || internal() == DisplayInternal::TableColumnGroup || internal() == DisplayInternal::TableColumn);
}

bool Display::is_none() const
{
    return m_type == Type::Box && m_value.box == DisplayBox::None;
}

bool Display::is_contents() const
{
    return m_type == Type::Box && m_value.box == DisplayBox::Contents;
}

bool Display::is_block_outside() const
{
    return is_outside_and_inside() && outside() == DisplayOutside::Block;
}

bool Display::is_inline_outside() const
{
    return is_outside_and_inside() && outside() == DisplayOutside::Inline;
}

bool Display::is_flow_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::Flow;
}

bool Display::is_flow_root_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::FlowRoot;
}

bool Display::is_table_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::Table;
}

bool Display::is_flex_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::Flex;
}

bool Display::is_grid_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::Grid;
}

bool Display::is_ruby_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::Ruby;
}

bool Display::is_math_inside() const
{
    return is_outside_and_inside() && inside() == DisplayInside::Math;
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
        //       mention what the outside value for `display: math` should be.
        //       The UA stylesheet does `* { display: block math; }` so let's go with that.
        return Display { DisplayOutside::Block, DisplayInside::Math };
    }
    VERIFY_NOT_REACHED();
}

}
