/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class EdgeStyleValue final : public StyleValueWithDefaultOperators<EdgeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<EdgeStyleValue const> create(Optional<PositionEdge> edge, RefPtr<StyleValue const> const& offset)
    {
        return adopt_ref(*new (nothrow) EdgeStyleValue(edge, offset));
    }
    virtual ~EdgeStyleValue() override = default;

    Optional<PositionEdge> edge() const
    {
        if (m_properties.edge == PositionEdge::Center)
            return {};

        return m_properties.edge;
    }

    NonnullRefPtr<StyleValue const> offset() const
    {
        if (m_properties.edge == PositionEdge::Center)
            return PercentageStyleValue::create(Percentage(50));

        if (!m_properties.offset)
            return PercentageStyleValue::create(Percentage(0));

        return *m_properties.offset;
    }

    virtual String to_string(SerializationMode) const override;

    ValueComparingNonnullRefPtr<EdgeStyleValue const> resolved_value(CalculationContext context) const;

    bool properties_equal(EdgeStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    EdgeStyleValue(Optional<PositionEdge> edge, RefPtr<StyleValue const> const& offset)
        : StyleValueWithDefaultOperators(Type::Edge)
        , m_properties { .edge = edge, .offset = offset }
    {
    }

    struct Properties {
        Optional<PositionEdge> edge;
        ValueComparingRefPtr<StyleValue const> offset;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
