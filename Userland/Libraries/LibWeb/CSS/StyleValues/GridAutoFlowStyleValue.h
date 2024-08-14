/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

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
    static ValueComparingNonnullRefPtr<GridAutoFlowStyleValue> create(Axis, Dense);
    virtual ~GridAutoFlowStyleValue() override = default;

    [[nodiscard]] bool is_row() const { return m_row; }
    [[nodiscard]] bool is_column() const { return !m_row; }
    [[nodiscard]] bool is_dense() const { return m_dense; }

    virtual String to_string() const override;
    bool properties_equal(GridAutoFlowStyleValue const& other) const { return m_row == other.m_row && m_dense == other.m_dense; }

private:
    explicit GridAutoFlowStyleValue(Axis axis, Dense dense)
        : StyleValueWithDefaultOperators(Type::GridAutoFlow)
        , m_row(axis == Axis::Row)
        , m_dense(dense == Dense::Yes)
    {
    }

    bool m_row { false };
    bool m_dense { false };
};

}
