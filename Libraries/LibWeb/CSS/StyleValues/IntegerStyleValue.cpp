/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IntegerStyleValue.h"

namespace Web::CSS {

String IntegerStyleValue::to_string() const
{
    return String::number(m_value);
}

}
