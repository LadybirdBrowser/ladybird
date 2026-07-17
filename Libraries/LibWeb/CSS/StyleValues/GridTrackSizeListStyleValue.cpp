/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackSizeListStyleValue.h"

namespace Web::CSS {

// Arena keeping the borrowed input arrays alive for the duration of the create call.
struct GridTrackEntryInputArena {
    Vector<Vector<StyleValueFFI::GridTrackEntryInput>> entry_arrays;
    Vector<Vector<size_t>> name_arrays;
};

static Vector<StyleValueFFI::GridTrackEntryInput> const& build_grid_track_entry_inputs(GridTrackSizeList const& list, GridTrackEntryInputArena& arena)
{
    Vector<StyleValueFFI::GridTrackEntryInput> entries;
    entries.ensure_capacity(list.list().size());
    for (auto const& entry : list.list()) {
        StyleValueFFI::GridTrackEntryInput input {};
        entry.visit(
            [&](GridLineNames const& line_names) {
                input.kind = 0;
                Vector<size_t> raws;
                raws.ensure_capacity(line_names.names().size());
                for (auto const& name : line_names.names())
                    raws.unchecked_append(name.name.to_raw_leaked());
                arena.name_arrays.append(move(raws));
                input.names = arena.name_arrays.last().data();
                input.name_count = arena.name_arrays.last().size();
            },
            [&](ExplicitGridTrack const& track) {
                if (track.is_default()) {
                    input.kind = 1;
                    input.size_value = retain_style_value_for_rust(track.grid_size().style_value().ptr());
                } else if (track.is_minmax()) {
                    input.kind = 2;
                    input.min_value = retain_style_value_for_rust(track.minmax().min_grid_size().style_value().ptr());
                    input.max_value = retain_style_value_for_rust(track.minmax().max_grid_size().style_value().ptr());
                } else {
                    auto const& repeat = track.repeat();
                    input.kind = 3;
                    input.repeat_type = static_cast<u8>(to_underlying(repeat.type()));
                    input.repeat_count = retain_style_value_for_rust(repeat.repeat_count_style_value().ptr());
                    input.repeat_is_subgrid = repeat.grid_track_size_list().is_subgrid();
                    input.repeat_preserve_line_name_sets = repeat.grid_track_size_list().preserves_line_name_sets();
                    auto const& nested = build_grid_track_entry_inputs(repeat.grid_track_size_list(), arena);
                    input.repeat_entries = nested.data();
                    input.repeat_entry_count = nested.size();
                }
            });
        entries.unchecked_append(input);
    }
    arena.entry_arrays.append(move(entries));
    return arena.entry_arrays.last();
}

StyleValueFFI::StyleValueData* GridTrackSizeListStyleValue::make_grid_track_size_list_data(CSS::GridTrackSizeList const& list)
{
    // The Rust allocation takes ownership of one strong reference to each value and one leaked
    // reference to each line name.
    GridTrackEntryInputArena arena;
    auto const& entries = build_grid_track_entry_inputs(list, arena);
    return StyleValueFFI::rust_style_value_create_grid_track_size_list(list.is_subgrid(), list.preserves_line_name_sets(), entries.data(), entries.size());
}

static GridTrackSizeList materialize_grid_track_size_list(bool is_subgrid, bool preserve_line_name_sets, StyleValueFFI::RetainedGridTrackEntry const* entries, size_t entry_count)
{
    auto list = is_subgrid        ? GridTrackSizeList::make_subgrid()
        : preserve_line_name_sets ? GridTrackSizeList::make_line_name_list()
                                  : GridTrackSizeList::make_none();
    for (size_t i = 0; i < entry_count; ++i) {
        auto const& entry = entries[i];
        switch (entry.kind) {
        case 0: {
            GridLineNames names;
            for (size_t j = 0; j < entry.names.length; ++j)
                names.append(Utf16FlyString::from_raw(entry.names.pointer[j].raw));
            list.append(move(names));
            break;
        }
        case 1:
            list.append(ExplicitGridTrack { GridSize { *static_cast<StyleValue const*>(entry.size_value.pointer) } });
            break;
        case 2:
            list.append(ExplicitGridTrack { GridMinMax { GridSize { *static_cast<StyleValue const*>(entry.min_value.pointer) }, GridSize { *static_cast<StyleValue const*>(entry.max_value.pointer) } } });
            break;
        default: {
            auto nested = materialize_grid_track_size_list(entry.repeat_is_subgrid, entry.repeat_preserve_line_name_sets, entry.repeat_entries_pointer, entry.repeat_entries_length);
            list.append(ExplicitGridTrack { GridRepeat { static_cast<GridRepeatType>(entry.repeat_type), move(nested), static_cast<StyleValue const*>(entry.repeat_count.pointer) } });
            break;
        }
        }
    }
    return list;
}

CSS::GridTrackSizeList GridTrackSizeListStyleValue::grid_track_size_list() const
{
    auto const& data = m_value->grid_track_size_list;
    return materialize_grid_track_size_list(data.is_subgrid, data.preserve_line_name_sets, data.entries.pointer, data.entries.length);
}

void GridTrackSizeListStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    grid_track_size_list().serialize(builder, mode);
}

ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> GridTrackSizeListStyleValue::create(CSS::GridTrackSizeList grid_track_size_list)
{
    return adopt_ref(*new (nothrow) GridTrackSizeListStyleValue(grid_track_size_list));
}

ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> GridTrackSizeListStyleValue::make_auto()
{
    return adopt_ref(*new (nothrow) GridTrackSizeListStyleValue(CSS::GridTrackSizeList()));
}

ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> GridTrackSizeListStyleValue::make_none()
{
    return adopt_ref(*new (nothrow) GridTrackSizeListStyleValue(CSS::GridTrackSizeList()));
}

ValueComparingNonnullRefPtr<StyleValue const> GridTrackSizeListStyleValue::absolutized(ComputationContext const& context) const
{
    // NB: Materialize the track list once; each call rebuilds the whole recursive list.
    auto grid_track_size_list = this->grid_track_size_list();
    auto absolutized = grid_track_size_list.absolutized(context);
    if (absolutized == grid_track_size_list)
        return *this;
    return create(move(absolutized));
}

}
