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
        auto const& data = m_value->grid_track_placement;
        Optional<Utf16FlyString> name;
        if (data.has_name)
            name = Utf16FlyString::from_raw(data.name.raw);
        switch (data.kind) {
        case 0:
            return GridTrackPlacement::make_auto();
        case 1:
            return GridTrackPlacement::make_span(*static_cast<StyleValue const*>(data.value.pointer), move(name));
        default:
            return GridTrackPlacement::make_line(static_cast<StyleValue const*>(data.value.pointer), move(name));
        }
    }
    void serialize(StringBuilder&, SerializationMode) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(GridTrackPlacementStyleValue const& other) const { return grid_track_placement() == other.grid_track_placement(); }

private:
    explicit GridTrackPlacementStyleValue(GridTrackPlacement grid_track_placement)
        : StyleValueWithDefaultOperators(Type::GridTrackPlacement, make_grid_track_placement_data(grid_track_placement))
    {
    }

    static StyleValueFFI::StyleValueData* make_grid_track_placement_data(GridTrackPlacement const& placement)
    {
        // The Rust allocation takes ownership of one strong reference to the value and one
        // leaked reference to the name when they are present.
        u8 kind = 0;
        void const* value = nullptr;
        Optional<Utf16FlyString> name;
        if (placement.is_span()) {
            kind = 1;
            auto span_value = placement.span();
            span_value->ref();
            value = span_value.ptr();
            name = placement.span_name();
        } else if (placement.is_area_or_line()) {
            kind = 2;
            if (placement.has_line_number()) {
                auto line_number = placement.line_number();
                line_number->ref();
                value = line_number.ptr();
            }
            if (placement.has_identifier())
                name = placement.identifier();
        }
        return StyleValueFFI::rust_style_value_create_grid_track_placement(kind, value, name.has_value(), name.has_value() ? name->to_raw_leaked() : 0);
    }
};

}
