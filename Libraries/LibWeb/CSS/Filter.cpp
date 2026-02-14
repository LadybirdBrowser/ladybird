/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Filter.h"
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>

namespace Web::CSS {

ReadonlySpan<FilterValue> Filter::filters() const
{
    VERIFY(has_filters());
    return m_filter_value_list->as_filter_value_list().filter_value_list().span();
}

}
