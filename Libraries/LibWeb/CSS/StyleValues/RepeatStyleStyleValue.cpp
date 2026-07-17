/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RepeatStyleStyleValue.h"
#include <AK/String.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

RepeatStyleStyleValue::RepeatStyleStyleValue(Repetition repeat_x, Repetition repeat_y)
    : StyleValueWithDefaultOperators(Type::RepeatStyle, StyleValueFFI::rust_style_value_create_repeat_style(to_underlying(repeat_x), to_underlying(repeat_y)))
{
}

RepeatStyleStyleValue::~RepeatStyleStyleValue() = default;

void RepeatStyleStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    if (repeat_x() == repeat_y()) {
        builder.append(CSS::to_string(repeat_x()));
        return;
    }

    if (repeat_x() == Repetition::Repeat && repeat_y() == Repetition::NoRepeat) {
        builder.append("repeat-x"sv);
        return;
    }
    if (repeat_x() == Repetition::NoRepeat && repeat_y() == Repetition::Repeat) {
        builder.append("repeat-y"sv);
        return;
    }

    builder.appendff("{} {}", CSS::to_string(repeat_x()), CSS::to_string(repeat_y()));
}

}
