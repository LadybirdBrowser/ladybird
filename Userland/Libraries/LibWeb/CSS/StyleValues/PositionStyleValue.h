/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>

namespace Web::CSS {

class PositionStyleValue final : public StyleValueWithDefaultOperators<PositionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PositionStyleValue> create(ValueComparingNonnullRefPtr<EdgeStyleValue> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue> edge_y)
    {
        return adopt_ref(*new (nothrow) PositionStyleValue(move(edge_x), move(edge_y)));
    }
    static ValueComparingNonnullRefPtr<PositionStyleValue> create_center()
    {
        return adopt_ref(*new (nothrow) PositionStyleValue(
            EdgeStyleValue::create(PositionEdge::Left, Percentage { 50 }),
            EdgeStyleValue::create(PositionEdge::Top, Percentage { 50 })));
    }
    virtual ~PositionStyleValue() override = default;

    ValueComparingNonnullRefPtr<EdgeStyleValue> edge_x() const { return m_properties.edge_x; }
    ValueComparingNonnullRefPtr<EdgeStyleValue> edge_y() const { return m_properties.edge_y; }
    bool is_center() const;
    CSSPixelPoint resolved(Layout::Node const&, CSSPixelRect const&) const;

    virtual String to_string() const override;

    bool properties_equal(PositionStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    PositionStyleValue(ValueComparingNonnullRefPtr<EdgeStyleValue> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue> edge_y)
        : StyleValueWithDefaultOperators(Type::Position)
        , m_properties { .edge_x = edge_x, .edge_y = edge_y }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<EdgeStyleValue> edge_x;
        ValueComparingNonnullRefPtr<EdgeStyleValue> edge_y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
