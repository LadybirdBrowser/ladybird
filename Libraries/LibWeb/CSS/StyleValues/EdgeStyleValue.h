/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

class EdgeStyleValue final : public StyleValueWithDefaultOperators<EdgeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<EdgeStyleValue> create(PositionEdge edge, LengthPercentage const& offset)
    {
        VERIFY(edge != PositionEdge::Center);
        return adopt_ref(*new (nothrow) EdgeStyleValue(edge, offset));
    }
    virtual ~EdgeStyleValue() override = default;

    // NOTE: `center` is converted to `left 50%` or `top 50%` in parsing, so is never returned here.
    PositionEdge edge() const { return m_properties.edge; }
    LengthPercentage const& offset() const { return m_properties.offset; }

    virtual String to_string() const override;

    bool properties_equal(EdgeStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    EdgeStyleValue(PositionEdge edge, LengthPercentage const& offset)
        : StyleValueWithDefaultOperators(Type::Edge)
        , m_properties { .edge = edge, .offset = offset }
    {
    }

    struct Properties {
        PositionEdge edge;
        LengthPercentage offset;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
