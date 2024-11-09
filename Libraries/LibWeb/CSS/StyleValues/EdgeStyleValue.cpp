/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EdgeStyleValue.h"

namespace Web::CSS {

String EdgeStyleValue::to_string() const
{
    return MUST(String::formatted("{} {}", CSS::to_string(m_properties.edge), m_properties.offset.to_string()));
}

}
