/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class EdgeStyleValue final : public StyleValueWithDefaultOperators<EdgeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<EdgeStyleValue const> create(Optional<PositionEdge> edge, Optional<LengthPercentage> const& offset)
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

    LengthPercentage offset() const
    {
        if (m_properties.edge == PositionEdge::Center)
            return Percentage(50);

        if (!m_properties.offset.has_value())
            return Percentage(0);

        return m_properties.offset.value();
    }

    virtual String to_string(SerializationMode) const override;

    ValueComparingNonnullRefPtr<EdgeStyleValue const> resolved_value(CalculationContext const& context) const;

    bool properties_equal(EdgeStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    EdgeStyleValue(Optional<PositionEdge> edge, Optional<LengthPercentage> const& offset)
        : StyleValueWithDefaultOperators(Type::Edge)
        , m_properties { .edge = edge, .offset = offset }
    {
    }

    struct Properties {
        Optional<PositionEdge> edge;
        Optional<LengthPercentage> offset;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
