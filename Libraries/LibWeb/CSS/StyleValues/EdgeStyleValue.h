/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    // This is nonnull as it is only called after resolving keywords
    NonnullRefPtr<StyleValue const> offset() const { return *offset_style_value(); }

    bool is_center(SerializationMode) const;

    void serialize(StringBuilder&, SerializationMode) const;

    ValueComparingNonnullRefPtr<EdgeStyleValue const> with_resolved_keywords() const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& computation_context) const;
    bool properties_equal(EdgeStyleValue const& other) const { return edge() == other.edge() && offset_style_value() == other.offset_style_value(); }

private:
    EdgeStyleValue(Optional<PositionEdge> edge, RefPtr<StyleValue const> const& offset)
        : StyleValueWithDefaultOperators(Type::Edge, make_edge_data(edge, offset))
    {
    }

    static StyleValueFFI::StyleValueData* make_edge_data(Optional<PositionEdge> edge, RefPtr<StyleValue const> const& offset)
    {
        // The Rust allocation takes ownership of one strong reference to the offset.
        if (offset)
            offset->ref();
        return StyleValueFFI::rust_style_value_create_edge(edge.has_value(), edge.has_value() ? to_underlying(*edge) : 0, offset.ptr());
    }

    Optional<PositionEdge> edge() const
    {
        if (!m_value->edge.has_edge)
            return {};
        return static_cast<PositionEdge>(m_value->edge.edge);
    }

    ValueComparingRefPtr<StyleValue const> offset_style_value() const { return static_cast<StyleValue const*>(m_value->edge.offset.pointer); }
};

}
