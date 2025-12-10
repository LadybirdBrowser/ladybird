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
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class PositionStyleValue final : public StyleValueWithDefaultOperators<PositionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create(ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y)
    {
        return adopt_ref(*new (nothrow) PositionStyleValue(move(edge_x), move(edge_y)));
    }
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create_center()
    {
        return adopt_ref(*new (nothrow) PositionStyleValue(
            EdgeStyleValue::create(PositionEdge::Center, {}),
            EdgeStyleValue::create(PositionEdge::Center, {})));
    }
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create_computed_center()
    {
        return adopt_ref(*new (nothrow) PositionStyleValue(
            EdgeStyleValue::create({}, LengthPercentage { Percentage { 50 } }),
            EdgeStyleValue::create({}, LengthPercentage { Percentage { 50 } })));
    }
    virtual ~PositionStyleValue() override = default;

    ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x() const { return m_properties.edge_x; }
    ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y() const { return m_properties.edge_y; }
    bool is_center() const;
    CSSPixelPoint resolved(Layout::Node const&, CSSPixelRect const&) const;

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(PositionStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    PositionStyleValue(ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y)
        : StyleValueWithDefaultOperators(Type::Position)
        , m_properties { .edge_x = move(edge_x), .edge_y = move(edge_y) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x;
        ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
