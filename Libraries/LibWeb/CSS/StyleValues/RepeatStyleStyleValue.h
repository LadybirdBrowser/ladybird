/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RepeatStyleStyleValue final : public StyleValueWithDefaultOperators<RepeatStyleStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RepeatStyleStyleValue const> create(Repetition repeat_x, Repetition repeat_y)
    {
        return adopt_ref(*new (nothrow) RepeatStyleStyleValue(repeat_x, repeat_y));
    }
    virtual ~RepeatStyleStyleValue() override;

    Repetition repeat_x() const { return m_properties.repeat_x; }
    Repetition repeat_y() const { return m_properties.repeat_y; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(RepeatStyleStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    RepeatStyleStyleValue(Repetition repeat_x, Repetition repeat_y);

    struct Properties {
        Repetition repeat_x;
        Repetition repeat_y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
