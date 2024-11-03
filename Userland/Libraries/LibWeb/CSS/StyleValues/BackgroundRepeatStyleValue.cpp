/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BackgroundRepeatStyleValue.h"
#include <AK/String.h>

namespace Web::CSS {

BackgroundRepeatStyleValue::BackgroundRepeatStyleValue(Repeat repeat_x, Repeat repeat_y)
    : StyleValueWithDefaultOperators(Type::BackgroundRepeat)
    , m_properties { .repeat_x = repeat_x, .repeat_y = repeat_y }
{
}

BackgroundRepeatStyleValue::~BackgroundRepeatStyleValue() = default;

String BackgroundRepeatStyleValue::to_string() const
{
    return MUST(String::formatted("{} {}", CSS::to_string(m_properties.repeat_x), CSS::to_string(m_properties.repeat_y)));
}

}
