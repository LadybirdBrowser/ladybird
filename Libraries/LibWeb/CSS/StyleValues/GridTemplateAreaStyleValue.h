/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridTemplateAreaStyleValue final : public StyleValueWithDefaultOperators<GridTemplateAreaStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue const> create(HashMap<String, GridArea> grid_areas, size_t row_count, size_t column_count);
    virtual ~GridTemplateAreaStyleValue() override = default;

    HashMap<String, GridArea> const& grid_areas() const { return m_grid_areas; }
    size_t row_count() const { return m_row_count; }
    size_t column_count() const { return m_column_count; }
    String cell_name_at(size_t row, size_t column) const;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(GridTemplateAreaStyleValue const& other) const
    {
        return m_row_count == other.m_row_count
            && m_column_count == other.m_column_count
            && m_grid_areas == other.m_grid_areas;
    }

private:
    explicit GridTemplateAreaStyleValue(HashMap<String, GridArea> grid_areas, size_t row_count, size_t column_count)
        : StyleValueWithDefaultOperators(Type::GridTemplateArea)
        , m_grid_areas(move(grid_areas))
        , m_row_count(row_count)
        , m_column_count(column_count)
    {
    }

    HashMap<String, GridArea> m_grid_areas;
    size_t m_row_count { 0 };
    size_t m_column_count { 0 };
};

}
