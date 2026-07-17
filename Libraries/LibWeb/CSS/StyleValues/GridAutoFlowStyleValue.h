/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridAutoFlowStyleValue final : public StyleValueWithDefaultOperators<GridAutoFlowStyleValue> {
public:
    enum Axis {
        Row,
        Column,
    };
    enum Dense {
        No,
        Yes,
    };
    static ValueComparingNonnullRefPtr<GridAutoFlowStyleValue const> create(Axis, Dense);
    virtual ~GridAutoFlowStyleValue() override = default;

    [[nodiscard]] bool is_row() const { return m_value->grid_auto_flow.row; }
    [[nodiscard]] bool is_column() const { return !is_row(); }
    [[nodiscard]] bool is_dense() const { return m_value->grid_auto_flow.dense; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    bool properties_equal(GridAutoFlowStyleValue const& other) const { return is_row() == other.is_row() && is_dense() == other.is_dense(); }

    virtual bool is_computationally_independent() const override { return true; }

private:
    explicit GridAutoFlowStyleValue(Axis axis, Dense dense)
        : StyleValueWithDefaultOperators(Type::GridAutoFlow)
        , m_value(StyleValueFFI::rust_style_value_create_grid_auto_flow(axis == Axis::Row, dense == Dense::Yes))
    {
    }

    RustStyleValueHandle m_value;
};

}
