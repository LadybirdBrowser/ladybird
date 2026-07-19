/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API PositionStyleValue final : public StyleValueWithDefaultOperators<PositionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create(ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y);
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create_center();
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create_computed_center();
    virtual ~PositionStyleValue() override = default;

    ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x() const { return *static_cast<EdgeStyleValue const*>(m_value->position.edge_x.pointer); }
    ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y() const { return *static_cast<EdgeStyleValue const*>(m_value->position.edge_y.pointer); }
    bool is_center(SerializationMode) const;
    CSSPixelPoint resolved(CSSPixelRect const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& computation_context) const;
    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(PositionStyleValue const& other) const { return edge_x() == other.edge_x() && edge_y() == other.edge_y(); }

private:
    PositionStyleValue(ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y)
        : StyleValueWithDefaultOperators(Type::Position, StyleValueFFI::rust_style_value_create_position(&edge_x.leak_ref(), &edge_y.leak_ref()))
    {
    }
};

}
