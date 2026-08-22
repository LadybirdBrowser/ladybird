/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

class WEB_API PositionStyleValue final : public StyleValueWithDefaultOperators<PositionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create(ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y);
    static ValueComparingNonnullRefPtr<PositionStyleValue const> create_center();
    virtual ~PositionStyleValue() override = default;

    ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x() const { return wrap_rust_child(m_value->position.edge_x)->as_edge(); }
    ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y() const { return wrap_rust_child(m_value->position.edge_y)->as_edge(); }
    bool is_center(SerializationMode) const;
    CSSPixelPoint resolved(CSSPixelRect const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& computation_context) const;

private:
    friend class StyleValue;

    explicit PositionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Position, data)
    {
    }

    PositionStyleValue(ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_x, ValueComparingNonnullRefPtr<EdgeStyleValue const> edge_y)
        : StyleValueWithDefaultOperators(Type::Position, StyleValueFFI::rust_style_value_create_position(StyleValueFFI::rust_style_value_retain(edge_x->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(edge_y->rust_style_value_data())))
    {
    }
};

}
