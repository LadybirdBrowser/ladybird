/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

class Filter {
public:
    Filter() = default;
    Filter(StyleValueList const& filter_value_list)
        : m_filter_value_list { filter_value_list }
    {
    }

    static Filter make_none()
    {
        return Filter {};
    }

    bool has_filters() const { return m_filter_value_list; }
    bool is_none() const { return !has_filters(); }

    StyleValueVector const& filters() const;

private:
    RefPtr<StyleValueList const> m_filter_value_list { nullptr };
};

}
