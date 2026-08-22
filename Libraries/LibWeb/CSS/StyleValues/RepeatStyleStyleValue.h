/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RepeatStyleStyleValue final : public StyleValueWithDefaultOperators<RepeatStyleStyleValue> {
public:
    virtual ~RepeatStyleStyleValue() override = default;

    Repetition repeat_x() const { return static_cast<Repetition>(m_value->repeat_style.repeat_x); }
    Repetition repeat_y() const { return static_cast<Repetition>(m_value->repeat_style.repeat_y); }

private:
    friend class StyleValue;

    explicit RepeatStyleStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::RepeatStyle, data)
    {
    }
};

}
