/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/String.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

class Display {
public:
    Display() = default;
    ~Display() = default;

    String to_string() const;

    bool operator==(Display const& other) const
    {
        if (m_type != other.m_type)
            return false;
        switch (m_type) {
        case Type::Box:
            return m_value.box == other.m_value.box;
        case Type::Internal:
            return m_value.internal == other.m_value.internal;
        case Type::OutsideAndInside:
            return m_value.outside_inside.outside == other.m_value.outside_inside.outside
                && m_value.outside_inside.inside == other.m_value.outside_inside.inside
                && m_value.outside_inside.list_item == other.m_value.outside_inside.list_item;
        }
        VERIFY_NOT_REACHED();
    }

    enum class ListItem {
        No,
        Yes,
    };

    enum class Type {
        OutsideAndInside,
        Internal,
        Box,
    };

    bool is_internal() const { return m_type == Type::Internal; }
    DisplayInternal internal() const
    {
        VERIFY(is_internal());
        return m_value.internal;
    }
    bool is_table_column() const { return is_internal() && internal() == DisplayInternal::TableColumn; }
    bool is_table_row_group() const { return is_internal() && internal() == DisplayInternal::TableRowGroup; }
    bool is_table_header_group() const { return is_internal() && internal() == DisplayInternal::TableHeaderGroup; }
    bool is_table_footer_group() const { return is_internal() && internal() == DisplayInternal::TableFooterGroup; }
    bool is_table_row() const { return is_internal() && internal() == DisplayInternal::TableRow; }
    bool is_table_cell() const { return is_internal() && internal() == DisplayInternal::TableCell; }
    bool is_table_column_group() const { return is_internal() && internal() == DisplayInternal::TableColumnGroup; }
    bool is_table_caption() const { return is_internal() && internal() == DisplayInternal::TableCaption; }
    // https://drafts.csswg.org/css-display-3/#internal-table-element
    bool is_internal_table() const
    {
        return is_internal() && (internal() == DisplayInternal::TableRowGroup || internal() == DisplayInternal::TableHeaderGroup || internal() == DisplayInternal::TableFooterGroup || internal() == DisplayInternal::TableRow || internal() == DisplayInternal::TableCell || internal() == DisplayInternal::TableColumnGroup || internal() == DisplayInternal::TableColumn);
    }

    bool is_none() const { return m_type == Type::Box && m_value.box == DisplayBox::None; }
    bool is_contents() const { return m_type == Type::Box && m_value.box == DisplayBox::Contents; }

    Type type() const { return m_type; }

    bool is_outside_and_inside() const { return m_type == Type::OutsideAndInside; }

    DisplayOutside outside() const
    {
        VERIFY(is_outside_and_inside());
        return m_value.outside_inside.outside;
    }

    bool is_block_outside() const { return is_outside_and_inside() && outside() == DisplayOutside::Block; }
    bool is_inline_outside() const { return is_outside_and_inside() && outside() == DisplayOutside::Inline; }
    bool is_inline_block() const { return is_inline_outside() && is_flow_root_inside(); }

    ListItem list_item() const
    {
        VERIFY(is_outside_and_inside());
        return m_value.outside_inside.list_item;
    }

    bool is_list_item() const { return is_outside_and_inside() && list_item() == ListItem::Yes; }

    DisplayInside inside() const
    {
        VERIFY(is_outside_and_inside());
        return m_value.outside_inside.inside;
    }

    bool is_flow_inside() const { return is_outside_and_inside() && inside() == DisplayInside::Flow; }
    bool is_flow_root_inside() const { return is_outside_and_inside() && inside() == DisplayInside::FlowRoot; }
    bool is_table_inside() const { return is_outside_and_inside() && inside() == DisplayInside::Table; }
    bool is_flex_inside() const { return is_outside_and_inside() && inside() == DisplayInside::Flex; }
    bool is_grid_inside() const { return is_outside_and_inside() && inside() == DisplayInside::Grid; }
    bool is_ruby_inside() const { return is_outside_and_inside() && inside() == DisplayInside::Ruby; }
    bool is_math_inside() const { return is_outside_and_inside() && inside() == DisplayInside::Math; }

    enum class Short {
        None,
        Contents,
        Block,
        Flow,
        FlowRoot,
        Inline,
        InlineBlock,
        RunIn,
        ListItem,
        InlineListItem,
        Flex,
        InlineFlex,
        Grid,
        InlineGrid,
        Ruby,
        Table,
        InlineTable,
        Math,
    };

    static Display from_short(Short short_)
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

    Display(DisplayOutside outside, DisplayInside inside)
        : m_type(Type::OutsideAndInside)
    {
        m_value.outside_inside = {
            .outside = outside,
            .inside = inside,
            .list_item = ListItem::No,
        };
    }

    Display(DisplayOutside outside, DisplayInside inside, ListItem list_item)
        : m_type(Type::OutsideAndInside)
    {
        m_value.outside_inside = {
            .outside = outside,
            .inside = inside,
            .list_item = list_item,
        };
    }

    explicit Display(DisplayInternal internal)
        : m_type(Type::Internal)
    {
        m_value.internal = internal;
    }

    explicit Display(DisplayBox box)
        : m_type(Type::Box)
    {
        m_value.box = box;
    }

private:
    Type m_type {};
    union {
        struct {
            DisplayOutside outside;
            DisplayInside inside;
            ListItem list_item;
        } outside_inside;
        DisplayInternal internal;
        DisplayBox box;
    } m_value {};
};

}
