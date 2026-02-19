/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>

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
    bool is_table_column() const;
    bool is_table_row_group() const;
    bool is_table_header_group() const;
    bool is_table_footer_group() const;
    bool is_table_row() const;
    bool is_table_cell() const;
    bool is_table_column_group() const;
    bool is_table_caption() const;
    // https://drafts.csswg.org/css-display-3/#internal-table-element
    bool is_internal_table() const;

    bool is_none() const;
    bool is_contents() const;

    Type type() const { return m_type; }

    bool is_outside_and_inside() const { return m_type == Type::OutsideAndInside; }

    DisplayOutside outside() const
    {
        VERIFY(is_outside_and_inside());
        return m_value.outside_inside.outside;
    }

    bool is_block_outside() const;
    bool is_inline_outside() const;
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

    bool is_flow_inside() const;
    bool is_flow_root_inside() const;
    bool is_table_inside() const;
    bool is_flex_inside() const;
    bool is_grid_inside() const;
    bool is_ruby_inside() const;
    bool is_math_inside() const;

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

    static Display from_short(Short short_);

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
