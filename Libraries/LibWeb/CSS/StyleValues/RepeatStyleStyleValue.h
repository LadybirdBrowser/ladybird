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
    static ValueComparingNonnullRefPtr<RepeatStyleStyleValue const> create(Repetition repeat_x, Repetition repeat_y)
    {
        return adopt_ref(*new (nothrow) RepeatStyleStyleValue(repeat_x, repeat_y));
    }
    virtual ~RepeatStyleStyleValue() override;

    Repetition repeat_x() const { return static_cast<Repetition>(m_value->repeat_style.repeat_x); }
    Repetition repeat_y() const { return static_cast<Repetition>(m_value->repeat_style.repeat_y); }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(RepeatStyleStyleValue const& other) const { return repeat_x() == other.repeat_x() && repeat_y() == other.repeat_y(); }

private:
    RepeatStyleStyleValue(Repetition repeat_x, Repetition repeat_y);
};

}
