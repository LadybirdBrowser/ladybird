/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Filter.h"
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>

namespace Web::CSS {

StyleValueVector const& Filter::filters() const
{
    VERIFY(has_filters());
    return m_filter_value_list->values();
}

}
