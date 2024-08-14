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

namespace Web::CSS {

class GridTemplateAreaStyleValue final : public StyleValueWithDefaultOperators<GridTemplateAreaStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue> create(Vector<Vector<String>> grid_template_area);
    virtual ~GridTemplateAreaStyleValue() override = default;

    Vector<Vector<String>> const& grid_template_area() const { return m_grid_template_area; }
    virtual String to_string() const override;

    bool properties_equal(GridTemplateAreaStyleValue const& other) const { return m_grid_template_area == other.m_grid_template_area; }

private:
    explicit GridTemplateAreaStyleValue(Vector<Vector<String>> grid_template_area)
        : StyleValueWithDefaultOperators(Type::GridTemplateArea)
        , m_grid_template_area(grid_template_area)
    {
    }

    Vector<Vector<String>> m_grid_template_area;
};

}
