/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridTrackPlacementStyleValue final : public StyleValueWithDefaultOperators<GridTrackPlacementStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTrackPlacementStyleValue const> create(GridTrackPlacement grid_track_placement);
    virtual ~GridTrackPlacementStyleValue() override = default;

    GridTrackPlacement grid_track_placement() const
    {
        return placement_from_data(m_value.data());
    }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit GridTrackPlacementStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::GridTrackPlacement, data)
    {
    }

    explicit GridTrackPlacementStyleValue(GridTrackPlacement grid_track_placement)
        : StyleValueWithDefaultOperators(Type::GridTrackPlacement, make_grid_track_placement_data(grid_track_placement))
    {
    }

    static GridTrackPlacement placement_from_data(StyleValueFFI::StyleValueData const* data)
    {
        auto const& placement = data->grid_track_placement;
        Optional<Utf16FlyString> name;
        if (placement.has_name)
            name = Utf16FlyString::from_raw(placement.name.raw);
        auto* value_data = static_cast<StyleValueFFI::StyleValueData const*>(placement.value.pointer);
        switch (placement.kind) {
        case 0:
            return GridTrackPlacement::make_auto();
        case 1:
            VERIFY(value_data);
            return GridTrackPlacement::make_span(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value_data)), move(name));
        default:
            return GridTrackPlacement::make_line(value_data ? RefPtr<StyleValue const> { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value_data)) } : nullptr, move(name));
        }
    }

    static StyleValueFFI::StyleValueData const* make_grid_track_placement_data(GridTrackPlacement const& placement)
    {
        // The Rust allocation takes ownership of one strong reference to the value data and
        // one leaked reference to the name when they are present.
        u8 kind = 0;
        StyleValueFFI::StyleValueData const* value = nullptr;
        Optional<Utf16FlyString> name;
        if (placement.is_span()) {
            kind = 1;
            auto span_value = placement.span();
            value = StyleValueFFI::rust_style_value_retain(span_value->rust_style_value_data());
            name = placement.span_name();
        } else if (placement.is_area_or_line()) {
            kind = 2;
            if (placement.has_line_number()) {
                auto line_number = placement.line_number();
                value = StyleValueFFI::rust_style_value_retain(line_number->rust_style_value_data());
            }
            if (placement.has_identifier())
                name = placement.identifier();
        }
        Optional<Utf16FlyString> implicit_start_name;
        Optional<Utf16FlyString> implicit_end_name;
        if (kind == 2 && name.has_value()) {
            implicit_start_name = implicit_grid_line_name(*name, "-start"sv);
            implicit_end_name = implicit_grid_line_name(*name, "-end"sv);
        }
        return StyleValueFFI::rust_style_value_create_grid_track_placement(
            kind,
            value,
            name.has_value(),
            name.has_value() ? name->to_raw_leaked() : 0,
            implicit_start_name.has_value() ? implicit_start_name->to_raw_leaked() : 0,
            implicit_end_name.has_value() ? implicit_end_name->to_raw_leaked() : 0);
    }
};

}
