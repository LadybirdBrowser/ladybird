/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridAutoFlowStyleValue final : public StyleValueWithDefaultOperators<GridAutoFlowStyleValue> {
public:
    virtual ~GridAutoFlowStyleValue() override = default;

    [[nodiscard]] bool is_row() const { return m_value->grid_auto_flow.row; }
    [[nodiscard]] bool is_column() const { return !is_row(); }
    [[nodiscard]] bool is_dense() const { return m_value->grid_auto_flow.dense; }

private:
    friend class StyleValue;

    explicit GridAutoFlowStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::GridAutoFlow, data)
    {
    }
};

}
