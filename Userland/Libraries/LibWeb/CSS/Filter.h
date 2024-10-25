/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>

namespace Web::CSS {

class Filter {
public:
    Filter() = default;
    Filter(FilterValueListStyleValue const& filter_value_list)
        : m_filter_value_list { filter_value_list } {};

    static Filter make_none()
    {
        return Filter {};
    }

    bool has_filters() const { return m_filter_value_list; }
    bool is_none() const { return !has_filters(); }

    ReadonlySpan<FilterFunction> filters() const
    {
        VERIFY(has_filters());
        return m_filter_value_list->filter_value_list().span();
    }

private:
    RefPtr<FilterValueListStyleValue const> m_filter_value_list { nullptr };
};

}
