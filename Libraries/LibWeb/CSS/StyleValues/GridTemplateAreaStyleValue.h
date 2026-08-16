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
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridTemplateAreaStyleValue final : public StyleValueWithDefaultOperators<GridTemplateAreaStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue const> create(HashMap<Utf16FlyString, GridArea> grid_areas, size_t row_count, size_t column_count);
    virtual ~GridTemplateAreaStyleValue() override = default;

    HashMap<Utf16FlyString, GridArea> grid_areas() const
    {
        auto const& list = m_value->grid_template_area.grid_areas;
        HashMap<Utf16FlyString, GridArea> grid_areas;
        for (size_t i = 0; i < list.length; ++i) {
            auto const& area = list.pointer[i];
            grid_areas.set(Utf16FlyString::from_raw(area.name.raw), GridArea { area.row_start, area.row_end, area.column_start, area.column_end });
        }
        return grid_areas;
    }
    size_t row_count() const { return m_value->grid_template_area.row_count; }
    size_t column_count() const { return m_value->grid_template_area.column_count; }
    // NB: Callers materialize grid_areas() once and look up cells through it, because grid_areas()
    //     rebuilds the whole map from the Rust data on every call.
    static Utf16FlyString cell_name_in(HashMap<Utf16FlyString, GridArea> const& grid_areas, size_t row, size_t column);

private:
    friend class StyleValue;

    explicit GridTemplateAreaStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::GridTemplateArea, data)
    {
    }

    explicit GridTemplateAreaStyleValue(HashMap<Utf16FlyString, GridArea> grid_areas, size_t row_count, size_t column_count)
        : StyleValueWithDefaultOperators(Type::GridTemplateArea, make_grid_template_area_data(grid_areas, row_count, column_count))
    {
    }

    static StyleValueFFI::StyleValueData const* make_grid_template_area_data(HashMap<Utf16FlyString, GridArea> const& grid_areas, size_t row_count, size_t column_count)
    {
        // The Rust allocation takes ownership of one leaked reference to each area name.
        Vector<StyleValueFFI::RetainedGridArea> areas;
        areas.ensure_capacity(grid_areas.size());
        for (auto const& [name, area] : grid_areas) {
            auto implicit_start_name = implicit_grid_line_name(name, "-start"sv);
            auto implicit_end_name = implicit_grid_line_name(name, "-end"sv);
            areas.unchecked_append({
                { name.to_raw_leaked(), nullptr },
                { implicit_start_name.to_raw_leaked(), nullptr },
                { implicit_end_name.to_raw_leaked(), nullptr },
                area.row_start,
                area.row_end,
                area.column_start,
                area.column_end,
            });
        }
        return StyleValueFFI::rust_style_value_create_grid_template_area(areas.data(), areas.size(), row_count, column_count);
    }
};

}
