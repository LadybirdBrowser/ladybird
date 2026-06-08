/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022-2023, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/GridFormattingContext.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/TableWrapper.h>

namespace Web::Layout {

// https://drafts.csswg.org/css-grid/#overlarge-grids
// Since memory is limited, UAs may clamp the possible size of the implicit grid to be within a UA-defined limit
// (which should accommodate lines in the range [-10000, 10000]), dropping all lines outside that limit. If a grid item
// is placed outside this limit, its grid area must be clamped to within this limited grid.
static constexpr i32 MAX_GRID_LINE_NUMBER = 10000;

static i32 clamp_grid_line(i32 value)
{
    return clamp(value, -MAX_GRID_LINE_NUMBER, MAX_GRID_LINE_NUMBER);
}

static i32 clamp_grid_span(i32 value)
{
    return clamp(value, 1, MAX_GRID_LINE_NUMBER);
}

static Alignment to_alignment(CSS::JustifyContent value)
{
    switch (value) {
    case CSS::JustifyContent::Left:
        return Alignment::Start;
    case CSS::JustifyContent::Right:
        return Alignment::End;
    case CSS::JustifyContent::FlexStart:
    case CSS::JustifyContent::Start:
        return Alignment::Start;
    case CSS::JustifyContent::FlexEnd:
    case CSS::JustifyContent::End:
        return Alignment::End;
    case CSS::JustifyContent::Center:
        return Alignment::Center;
    case CSS::JustifyContent::SpaceBetween:
        return Alignment::SpaceBetween;
    case CSS::JustifyContent::SpaceAround:
        return Alignment::SpaceAround;
    case CSS::JustifyContent::SpaceEvenly:
        return Alignment::SpaceEvenly;
    case CSS::JustifyContent::Stretch:
        return Alignment::Stretch;
    case CSS::JustifyContent::Normal:
        return Alignment::Normal;
    default:
        VERIFY_NOT_REACHED();
    }
}

static Alignment to_alignment(CSS::JustifyItems value)
{
    switch (value) {
    case CSS::JustifyItems::Baseline:
        return Alignment::Baseline;
    case CSS::JustifyItems::Center:
        return Alignment::Center;
    case CSS::JustifyItems::End:
        return Alignment::End;
    case CSS::JustifyItems::FlexEnd:
        return Alignment::End;
    case CSS::JustifyItems::FlexStart:
        return Alignment::Start;
    case CSS::JustifyItems::Legacy:
        return Alignment::Normal;
    case CSS::JustifyItems::Normal:
        return Alignment::Normal;
    case CSS::JustifyItems::Safe:
        return Alignment::Safe;
    case CSS::JustifyItems::SelfEnd:
        return Alignment::SelfEnd;
    case CSS::JustifyItems::SelfStart:
        return Alignment::SelfStart;
    case CSS::JustifyItems::Start:
        return Alignment::Start;
    case CSS::JustifyItems::Stretch:
        return Alignment::Stretch;
    case CSS::JustifyItems::Unsafe:
        return Alignment::Unsafe;
    case CSS::JustifyItems::Left:
        return Alignment::Start;
    case CSS::JustifyItems::Right:
        return Alignment::End;
    default:
        VERIFY_NOT_REACHED();
    }
}

static Alignment to_alignment(CSS::AlignContent value)
{
    switch (value) {
    case CSS::AlignContent::Start:
        return Alignment::Start;
    case CSS::AlignContent::End:
        return Alignment::End;
    case CSS::AlignContent::Center:
        return Alignment::Center;
    case CSS::AlignContent::SpaceBetween:
        return Alignment::SpaceBetween;
    case CSS::AlignContent::SpaceAround:
        return Alignment::SpaceAround;
    case CSS::AlignContent::SpaceEvenly:
        return Alignment::SpaceEvenly;
    case CSS::AlignContent::Stretch:
        return Alignment::Stretch;
    case CSS::AlignContent::Normal:
        return Alignment::Normal;
    case CSS::AlignContent::FlexStart:
        return Alignment::Start;
    case CSS::AlignContent::FlexEnd:
        return Alignment::End;
    default:
        VERIFY_NOT_REACHED();
    }
}

static Alignment to_alignment(CSS::AlignItems value)
{
    switch (value) {
    case CSS::AlignItems::Baseline:
        return Alignment::Baseline;
    case CSS::AlignItems::Center:
        return Alignment::Center;
    case CSS::AlignItems::End:
        return Alignment::End;
    case CSS::AlignItems::FlexEnd:
        return Alignment::End;
    case CSS::AlignItems::FlexStart:
        return Alignment::Start;
    case CSS::AlignItems::Normal:
        return Alignment::Normal;
    case CSS::AlignItems::Safe:
        return Alignment::Safe;
    case CSS::AlignItems::SelfEnd:
        return Alignment::SelfEnd;
    case CSS::AlignItems::SelfStart:
        return Alignment::SelfStart;
    case CSS::AlignItems::Start:
        return Alignment::Start;
    case CSS::AlignItems::Stretch:
        return Alignment::Stretch;
    case CSS::AlignItems::Unsafe:
        return Alignment::Unsafe;
    default:
        VERIFY_NOT_REACHED();
    }
}

GridFormattingContext::GridTrack GridFormattingContext::GridTrack::create_from_definition(CSS::ExplicitGridTrack const& definition, bool is_auto_fit, bool is_auto_repeat)
{
    // NOTE: repeat() is expected to be expanded beforehand.
    VERIFY(!definition.is_repeat());

    if (definition.is_minmax()) {
        return GridTrack {
            .min_track_sizing_function = definition.minmax().min_grid_size(),
            .max_track_sizing_function = definition.minmax().max_grid_size(),
            .is_auto_fit = is_auto_fit,
            .is_auto_repeat = is_auto_repeat,
        };
    }

    // https://drafts.csswg.org/css-grid-2/#algo-terms
    // min track sizing function:
    // If the track was sized with a minmax() function, this is the first argument to that function.
    // If the track was sized with a <flex> value or fit-content() function, auto. Otherwise, the track’s sizing function.
    auto min_track_sizing_function = definition.grid_size();
    if (min_track_sizing_function.is_flexible_length() || min_track_sizing_function.is_fit_content()) {
        min_track_sizing_function = CSS::GridSize::make_auto();
    }
    auto max_track_sizing_function = definition.grid_size();

    return GridTrack {
        .min_track_sizing_function = min_track_sizing_function,
        .max_track_sizing_function = max_track_sizing_function,
        .is_auto_fit = is_auto_fit,
        .is_auto_repeat = is_auto_repeat,
    };
}

GridFormattingContext::GridTrack GridFormattingContext::GridTrack::create_auto()
{
    return GridTrack {
        .min_track_sizing_function = CSS::GridSize::make_auto(),
        .max_track_sizing_function = CSS::GridSize::make_auto(),
    };
}

GridFormattingContext::GridTrack GridFormattingContext::GridTrack::create_from_subgrid_parent_track(GridTrack const& parent_track)
{
    return GridTrack {
        .min_track_sizing_function = parent_track.min_track_sizing_function,
        .max_track_sizing_function = parent_track.max_track_sizing_function,
        .is_auto_fit = parent_track.is_auto_fit,
    };
}

GridFormattingContext::GridTrack GridFormattingContext::GridTrack::create_fixed(CSSPixels size)
{
    auto fixed_size = CSS::GridSize(CSS::LengthStyleValue::create(CSS::Length::make_px(size)));
    return GridTrack {
        .min_track_sizing_function = fixed_size,
        .max_track_sizing_function = fixed_size,
        .base_size = size,
        .growth_limit = size,
    };
}

GridFormattingContext::GridTrack GridFormattingContext::GridTrack::create_gap(CSSPixels size)
{
    return GridTrack {
        .min_track_sizing_function = CSS::GridSize(CSS::LengthStyleValue::create(CSS::Length::make_px(size))),
        .max_track_sizing_function = CSS::GridSize(CSS::LengthStyleValue::create(CSS::Length::make_px(size))),
        .base_size = size,
        .is_gap = true,
    };
}

GridFormattingContext::GridFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& grid_container, FormattingContext* parent)
    : FormattingContext(Type::Grid, layout_mode, state, grid_container, parent)
    , m_grid_container_used_values(state.get_mutable(grid_container))
{
}

GridFormattingContext::~GridFormattingContext() = default;

static size_t count_subgrid_line_name_lists_from_index(CSS::GridTrackSizeList const& list, size_t start_index)
{
    size_t count = 0;
    auto const& items = list.list();
    for (size_t item_index = start_index; item_index < items.size(); ++item_index) {
        auto const& item = items[item_index];
        if (item.has<CSS::GridLineNames>()) {
            ++count;
        } else if (item.has<CSS::ExplicitGridTrack>()) {
            auto const& explicit_track = item.get<CSS::ExplicitGridTrack>();
            if (!explicit_track.is_repeat())
                continue;
            auto repeated_line_name_list_count = count_subgrid_line_name_lists_from_index(explicit_track.repeat().grid_track_size_list(), 0);
            if (explicit_track.repeat().is_fixed())
                count += repeated_line_name_list_count * explicit_track.repeat().repeat_count();
            else
                count += repeated_line_name_list_count;
        }
    }
    return count;
}

static size_t count_subgrid_line_name_lists(CSS::GridTrackSizeList const& list)
{
    return count_subgrid_line_name_lists_from_index(list, 0);
}

static size_t explicit_track_count_from_subgrid_line_name_list(CSS::GridTrackSizeList const& list)
{
    auto line_name_list_count = count_subgrid_line_name_lists(list);
    if (line_name_list_count <= 1)
        return 0;
    return line_name_list_count - 1;
}

bool GridFormattingContext::is_subgridded_axis(GridDimension dimension) const
{
    auto const& grid_computed_values = grid_container().computed_values();
    auto const& track_list = dimension == GridDimension::Column ? grid_computed_values.grid_template_columns() : grid_computed_values.grid_template_rows();
    if (!track_list.is_subgrid())
        return false;

    // https://drafts.csswg.org/css-grid-2/#subgrid-listing
    // If there is no parent grid, or if the grid container is otherwise forced
    // to establish an independent formatting context, the used value is
    // the initial value, grid-template-rows/none, and the grid container is not
    // a subgrid.
    // FIXME: Also reject subgrid here when the grid container is forced to
    // establish an independent formatting context.
    return parent_grid_item() != nullptr;
}

GridFormattingContext const* GridFormattingContext::parent_grid_formatting_context() const
{
    auto const* parent_context = parent();
    if (!parent_context || parent_context->type() != Type::Grid)
        return nullptr;
    return static_cast<GridFormattingContext const*>(parent_context);
}

GridItem const* GridFormattingContext::grid_item_for_box(Box const& box) const
{
    for (auto const& item : m_grid_items) {
        if (&item.box == &box)
            return &item;
    }
    return nullptr;
}

GridItem const* GridFormattingContext::parent_grid_item() const
{
    auto const* parent_grid = parent_grid_formatting_context();
    if (!parent_grid)
        return nullptr;
    return parent_grid->grid_item_for_box(grid_container());
}

bool GridFormattingContext::grid_item_is_subgridded_in_axis(GridItem const& item, GridDimension dimension) const
{
    if (!item.box.display().is_grid_inside())
        return false;

    auto const& grid_template = dimension == GridDimension::Column
        ? item.computed_values().grid_template_columns()
        : item.computed_values().grid_template_rows();
    return grid_template.is_subgrid();
}

void GridFormattingContext::for_each_item_contributing_to_track_sizing(GridDimension dimension, Function<void(GridItem const&)> const& callback)
{
    for (auto const& item : m_grid_items) {
        if (grid_item_is_subgridded_in_axis(item, dimension)) {
            for_each_subgrid_item_contributing_to_track_sizing(item, dimension, callback);
            continue;
        }
        callback(item);
    }
}

void GridFormattingContext::for_each_subgrid_item_contributing_to_track_sizing(GridItem const& subgrid, GridDimension dimension, Function<void(GridItem const&)> const& callback)
{
    VERIFY(m_available_space.has_value());

    // https://drafts.csswg.org/css-grid-2/#subgrid-size-contribution
    // The subgrid itself lays out as an ordinary grid item in its parent grid,
    // but acts as if it was completely empty for track sizing purposes
    // in the subgridded dimension.
    //
    // https://drafts.csswg.org/css-grid-2/#subgrid-item-contribution
    // The subgrid's own grid items participate in the sizing of its parent grid
    // in the subgridded dimension(s) and are aligned to it in those dimensions.
    //
    // https://drafts.csswg.org/css-grid-2/#algo-grid-sizing
    // In this process, any grid item which is subgridded in the grid container’s
    // inline axis is treated as empty and its grid items (the grandchildren) are
    // treated as direct children of the grid container (their grandparent).
    // This introspection is recursive.
    //
    // In this process, any grid item which is subgridded in the grid container’s
    // block axis is treated as empty and its grid items (the grandchildren) are
    // treated as direct children of the grid container (their grandparent).
    // This introspection is recursive.
    GridFormattingContext subgrid_context(m_state, LayoutMode::IntrinsicSizing, subgrid.box, this);
    subgrid_context.m_available_space = *m_available_space;
    subgrid_context.init_grid_lines(GridDimension::Column);
    subgrid_context.init_grid_lines(GridDimension::Row);
    subgrid_context.build_grid_areas();
    subgrid_context.m_explicit_columns_line_count = subgrid_context.m_column_lines.size();
    subgrid_context.m_explicit_rows_line_count = subgrid_context.m_row_lines.size();
    subgrid_context.place_grid_items();
    subgrid_context.initialize_grid_tracks_for_columns_and_rows();
    subgrid_context.initialize_gap_tracks(*m_available_space);
    subgrid_context.resolve_items_box_metrics(dimension);

    subgrid_context.for_each_item_contributing_to_track_sizing(dimension, [&](GridItem const& item) {
        LayoutState::UsedValues used_values_in_parent_grid;
        used_values_in_parent_grid = item.used_values;

        GridItem item_in_parent_grid {
            .box = item.box,
            .used_values = used_values_in_parent_grid,
            .row = item.row,
            .row_span = item.row_span,
            .column = item.column,
            .column_span = item.column_span,
        };
        subgrid_context.apply_subgrid_edge_extra_margins(item_in_parent_grid, dimension);

        if (dimension == GridDimension::Column)
            item_in_parent_grid.column = subgrid.raw_position(GridDimension::Column) + item.raw_position(GridDimension::Column);
        else
            item_in_parent_grid.row = subgrid.raw_position(GridDimension::Row) + item.raw_position(GridDimension::Row);

        callback(item_in_parent_grid);
    });
}

size_t GridFormattingContext::subgrid_track_count(GridDimension dimension) const
{
    auto const* item = parent_grid_item();
    if (!item)
        return 1;
    return item->span(dimension);
}

CSSPixels GridFormattingContext::parent_gap_size_for_subgrid(GridDimension dimension) const
{
    auto const* parent_grid = parent_grid_formatting_context();
    auto const* item = parent_grid_item();
    if (!parent_grid || !item || item->span(dimension) <= 1)
        return 0;

    auto const& gap_tracks = dimension == GridDimension::Column ? parent_grid->m_column_gap_tracks : parent_grid->m_row_gap_tracks;
    if (gap_tracks.is_empty())
        return 0;

    auto gap_index = item->raw_position(dimension);
    if (gap_index >= 0 && static_cast<size_t>(gap_index) < gap_tracks.size())
        return gap_tracks[gap_index].base_size;
    return gap_tracks.first().base_size;
}

CSSPixels GridFormattingContext::subgrid_gap_extra_margin(GridDimension dimension, AvailableSize const& available_size) const
{
    if (!is_subgridded_axis(dimension))
        return 0;

    auto const& computed_gap = dimension == GridDimension::Column ? grid_container().computed_values().column_gap() : grid_container().computed_values().row_gap();
    if (computed_gap.has<CSS::NormalGap>()) {
        // https://drafts.csswg.org/css-grid-2/#subgrid-gaps
        // A value of normal indicates that the subgrid has the same size gutters
        // as its parent grid, i.e. the applied difference is zero.
        return 0;
    }

    // https://drafts.csswg.org/css-grid-2/#subgrid-gaps
    // Half the size of the difference between the subgrid's gutters
    // (row-gap/column-gap) and its parent grid's gutters is applied as an extra
    // layer of (potentially negative) margin to the items not at those edges.
    return (gap_to_px(computed_gap, available_size.to_px_or_zero()) - parent_gap_size_for_subgrid(dimension)) / 2;
}

void GridFormattingContext::apply_subgrid_edge_extra_margins(GridItem& item, GridDimension dimension) const
{
    if (!is_subgridded_axis(dimension))
        return;

    // https://drafts.csswg.org/css-grid-2/#subgrid-margins
    // In this process, the sum of the subgrid's margin, padding, scrollbar
    // gutter, and border at each edge are applied as an extra layer of
    // (potentially negative) margin to the items at those edges.
    // This extra layer of "margin" accumulates through multiple levels of
    // subgrids.
    //
    // NB: Scrollbar gutters are not represented in UsedValues yet.
    auto start_extra_margin = dimension == GridDimension::Column
        ? m_grid_container_used_values.margin_box_left()
        : m_grid_container_used_values.margin_box_top();
    auto end_extra_margin = dimension == GridDimension::Column
        ? m_grid_container_used_values.margin_box_right()
        : m_grid_container_used_values.margin_box_bottom();

    auto const track_count = dimension == GridDimension::Column ? m_grid_columns.size() : m_grid_rows.size();
    auto const item_start = item.raw_position(dimension);
    auto const item_end = item_start + static_cast<int>(item.span(dimension));

    if (item_start == 0) {
        if (dimension == GridDimension::Column)
            item.used_values.margin_left += start_extra_margin;
        else
            item.used_values.margin_top += start_extra_margin;
    }

    if (item_end == static_cast<int>(track_count)) {
        if (dimension == GridDimension::Column)
            item.used_values.margin_right += end_extra_margin;
        else
            item.used_values.margin_bottom += end_extra_margin;
    }
}

void GridFormattingContext::apply_subgrid_gap_extra_margins(GridItem& item, GridDimension dimension, AvailableSize const& available_size) const
{
    auto extra_margin = subgrid_gap_extra_margin(dimension, available_size);
    if (extra_margin == 0)
        return;

    auto const track_count = dimension == GridDimension::Column ? m_grid_columns.size() : m_grid_rows.size();
    auto const item_start = item.raw_position(dimension);
    auto const item_end = item_start + static_cast<int>(item.span(dimension));

    if (item_start > 0) {
        if (dimension == GridDimension::Column)
            item.used_values.margin_left += extra_margin;
        else
            item.used_values.margin_top += extra_margin;
    }

    if (item_end < static_cast<int>(track_count)) {
        if (dimension == GridDimension::Column)
            item.used_values.margin_right += extra_margin;
        else
            item.used_values.margin_bottom += extra_margin;
    }
}

CSSPixels GridFormattingContext::resolved_gap_size(GridDimension dimension, AvailableSize const& available_size) const
{
    if (is_subgridded_axis(dimension)) {
        // https://drafts.csswg.org/css-grid-2/#subgrid-gaps
        // The parent's grid tracks will be sized as specified, and the
        // subgrid's gutters will visually center-align with the parent grid's
        // gutters.
        return parent_gap_size_for_subgrid(dimension);
    }

    auto const& computed_gap = dimension == GridDimension::Column ? grid_container().computed_values().column_gap() : grid_container().computed_values().row_gap();
    if (computed_gap.has<CSS::NormalGap>()) {
        return 0;
    }
    return gap_to_px(computed_gap, available_size.to_px_or_zero());
}

bool GridFormattingContext::has_gaps(GridDimension dimension) const
{
    if (is_subgridded_axis(dimension))
        return parent_gap_size_for_subgrid(dimension) != 0;

    auto const& computed_gap = dimension == GridDimension::Column ? grid_container().computed_values().column_gap() : grid_container().computed_values().row_gap();
    if (!computed_gap.has<CSS::NormalGap>())
        return true;
    return false;
}

CSSPixels GridFormattingContext::resolve_definite_track_size(CSS::GridSize const& grid_size, AvailableSpace const& available_space) const
{
    VERIFY(grid_size.is_definite());
    return grid_size.css_size().to_px(grid_container(), available_space.width.to_px_or_zero());
}

int GridFormattingContext::count_of_repeated_auto_fill_or_fit_tracks(GridDimension dimension, CSS::ExplicitGridTrack const& repeated_track)
{
    // https://www.w3.org/TR/css-grid-2/#auto-repeat
    // 7.2.3.2. Repeat-to-fill: auto-fill and auto-fit repetitions
    // On a subgridded axis, the auto-fill keyword is only valid once per <line-name-list>, and repeats
    // enough times for the name list to match the subgrid’s specified grid span (falling back to 0 if
    // the span is already fulfilled).

    // Otherwise on a standalone axis, when auto-fill is given as the repetition number
    // If the grid container has a definite size or max size in the relevant axis, then the number of
    // repetitions is the largest possible positive integer that does not cause the grid to overflow the
    // content box of its grid container

    auto const& grid_computed_values = grid_container().computed_values();
    CSSPixels size_of_repeated_tracks = 0;
    // (treating each track as its max track sizing function if that is definite or its minimum track sizing
    // function otherwise, flooring the max track sizing function by the min track sizing function if both
    // are definite, and taking gap into account)
    auto const& repeat_track_list = repeated_track.repeat().grid_track_size_list().track_list();
    for (auto const& explicit_grid_track : repeat_track_list) {
        auto const& track_sizing_function = explicit_grid_track;
        CSSPixels track_size = 0;
        if (track_sizing_function.is_minmax()) {
            auto const& min_size = track_sizing_function.minmax().min_grid_size();
            auto const& max_size = track_sizing_function.minmax().max_grid_size();
            if (max_size.is_definite()) {
                track_size = resolve_definite_track_size(max_size, *m_available_space);
                if (min_size.is_definite())
                    track_size = min(track_size, resolve_definite_track_size(min_size, *m_available_space));
            } else if (min_size.is_definite()) {
                track_size = resolve_definite_track_size(min_size, *m_available_space);
            } else {
                VERIFY_NOT_REACHED();
            }
        } else {
            track_size = resolve_definite_track_size(track_sizing_function.grid_size(), *m_available_space);
        }
        size_of_repeated_tracks += track_size;
    }

    if (size_of_repeated_tracks == 0)
        return 0;

    auto const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;
    auto const& gap = dimension == GridDimension::Column ? grid_computed_values.column_gap() : grid_computed_values.row_gap();
    auto gap_px = gap_to_px(gap, available_size.to_px_or_zero());
    auto size_of_repeated_tracks_with_gap = size_of_repeated_tracks + repeat_track_list.size() * gap_px;
    // Otherwise, if the grid container has a definite min size in the relevant axis, the number of repetitions is the
    // smallest possible positive integer that fulfills that minimum requirement
    if (available_size.is_definite()) {
        // NOTE: Gap size is added to free space to compensate for the fact that the last track does not have a gap
        auto free_space = available_size.to_px_or_zero();
        auto number_of_repetitions = ((free_space + gap_px) / size_of_repeated_tracks_with_gap).to_int();
        // If any number of repetitions would overflow, then 1 repetition.
        return max(1, number_of_repetitions);
    }
    // Otherwise, the specified track list repeats only once.
    return 1;

    // For the purpose of finding the number of auto-repeated tracks in a standalone axis, the UA must
    // floor the track size to a UA-specified value to avoid division by zero. It is suggested that this
    // floor be 1px.
}

GridFormattingContext::PlacementPosition GridFormattingContext::resolve_grid_position(Box const& child_box, GridDimension dimension)
{
    auto const& computed_values = child_box.computed_values();
    auto const& placement_start = dimension == GridDimension::Row ? computed_values.grid_row_start() : computed_values.grid_column_start();
    auto const& placement_end = dimension == GridDimension::Row ? computed_values.grid_row_end() : computed_values.grid_column_end();

    Optional<i32> placement_start_line_number = placement_start.has_line_number() ? clamp_grid_line(CSS::int_from_style_value(placement_start.line_number())) : Optional<i32> {};
    Optional<i32> placement_end_line_number = placement_end.has_line_number() ? clamp_grid_line(CSS::int_from_style_value(placement_end.line_number())) : Optional<i32> {};

    PlacementPosition result;

    if (placement_start_line_number.has_value() && placement_start_line_number.value() > 0)
        result.start = placement_start_line_number.value() - 1;
    else if (placement_start_line_number.has_value()) {
        auto explicit_line_count = dimension == GridDimension::Row ? m_explicit_rows_line_count : m_explicit_columns_line_count;
        result.start = explicit_line_count + placement_start_line_number.value();
    }
    if (placement_end_line_number.has_value())
        result.end = *placement_end_line_number - 1;

    if (result.end < 0) {
        if (dimension == GridDimension::Row)
            result.end = m_occupation_grid.row_count() + result.end + 2;
        else
            result.end = m_occupation_grid.column_count() + result.end + 2;
    }

    // FIXME: If a name is given as a <custom-ident>, only lines with that name are counted. If not enough lines with
    //        that name exist, all implicit grid lines on the side of the explicit grid corresponding to the search
    //        direction are assumed to have that name for the purpose of counting this span.
    if (placement_end.is_span())
        result.span = clamp_grid_span(CSS::int_from_style_value(placement_end.span()));
    if (placement_start.is_span()) {
        result.span = clamp_grid_span(CSS::int_from_style_value(placement_start.span()));
        result.start = result.end - result.span;
        // FIXME: Remove me once have implemented spans overflowing into negative indexes, e.g., grid-row: span 2 / 1
        if (result.start < 0)
            result.start = 0;
    }

    auto explicit_line_count = dimension == GridDimension::Row ? m_explicit_rows_line_count : m_explicit_columns_line_count;

    if (placement_end.has_identifier()) {
        auto area_end_line_name = MUST(String::formatted("{}-end", placement_end.identifier()));
        auto line_number = placement_end_line_number.value_or(1);
        if (auto area_end_line_index = get_nth_line_index_by_line_name(dimension, area_end_line_name, line_number); area_end_line_index.has_value()) {
            result.end = area_end_line_index.value();
        } else if (auto line_name_index = get_nth_line_index_by_line_name(dimension, placement_end.identifier(), line_number); line_name_index.has_value()) {
            result.end = line_name_index.value();
        } else {
            result.end = explicit_line_count;
        }
        if (!placement_start.has_line_number())
            result.start = result.end - 1;
    }

    if (placement_start.has_identifier()) {
        auto area_start_line_name = MUST(String::formatted("{}-start", placement_start.identifier()));
        auto line_number = placement_start_line_number.value_or(1);
        if (auto area_start_line_index = get_nth_line_index_by_line_name(dimension, area_start_line_name, line_number); area_start_line_index.has_value()) {
            result.start = area_start_line_index.value();
        } else if (auto line_name_index = get_nth_line_index_by_line_name(dimension, placement_start.identifier(), line_number); line_name_index.has_value()) {
            result.start = line_name_index.value();
        } else {
            result.start = explicit_line_count;
        }
    }

    if (!placement_start.is_positioned() && placement_end.is_positioned() && !placement_end.is_span()) {
        if (result.span == 0)
            result.span = 1;
        result.start = result.end - result.span;
    }

    if (placement_start.is_positioned() && placement_end.is_positioned()) {
        if (result.start > result.end)
            swap(result.start, result.end);
        if (result.start != result.end) {
            result.span = result.end - result.start;
        } else {
            result.span = 1;
            result.end = result.start + result.span;
        }
    }

    // If the placement contains two spans, remove the one contributed by the end grid-placement
    // property.
    if (placement_start.is_span() && placement_end.is_span())
        result.span = clamp_grid_span(CSS::int_from_style_value(placement_start.span()));

    return result;
}

size_t GridFormattingContext::resolve_grid_span(Box const& child_box, GridDimension dimension) const
{
    auto const& computed_values = child_box.computed_values();
    auto const& placement_start = dimension == GridDimension::Row ? computed_values.grid_row_start() : computed_values.grid_column_start();
    auto const& placement_end = dimension == GridDimension::Row ? computed_values.grid_row_end() : computed_values.grid_column_end();

    if (placement_start.is_span())
        return clamp_grid_span(CSS::int_from_style_value(placement_start.span()));
    if (placement_end.is_span())
        return clamp_grid_span(CSS::int_from_style_value(placement_end.span()));

    auto placement_has_implicit_span = placement_start.is_positioned() && placement_end.is_positioned();
    if (!placement_has_implicit_span && child_box.display().is_grid_inside()) {
        auto const& grid_template = dimension == GridDimension::Column
            ? computed_values.grid_template_columns()
            : computed_values.grid_template_rows();
        if (grid_template.is_subgrid()) {
            // https://drafts.csswg.org/css-grid-2/#grid-placement
            // Otherwise, its grid span is automatic: if it is subgridded in
            // that axis, its grid span is determined from its line-name-list;
            // otherwise its grid span is 1.
            //
            // https://drafts.csswg.org/css-grid-2/#subgrid-span
            // If it has an automatic grid span, then its used grid span is
            // taken from the number of explicit tracks specified for that axis
            // by its grid-template-* properties, floored at one.
            return max<size_t>(1, explicit_track_count_from_subgrid_line_name_list(grid_template));
        }
    }
    return 1;
}

void GridFormattingContext::place_item_with_row_and_column_position(Box const& child_box)
{
    auto row_placement_position = resolve_grid_position(child_box, GridDimension::Row);
    auto column_placement_position = resolve_grid_position(child_box, GridDimension::Column);

    auto row_start = row_placement_position.start;
    auto row_span = row_placement_position.span;
    auto column_start = column_placement_position.start;
    auto column_span = column_placement_position.span;

    record_grid_placement(GridItem {
        .box = child_box,
        .used_values = m_state.get_mutable(child_box),
        .row = row_start,
        .row_span = row_span,
        .column = column_start,
        .column_span = column_span });
}

void GridFormattingContext::place_item_with_row_position(Box const& child_box)
{
    auto placement_position = resolve_grid_position(child_box, GridDimension::Row);
    auto row_start = placement_position.start;
    size_t row_span = placement_position.span;

    int column_start = 0;
    auto column_span = resolve_grid_span(child_box, GridDimension::Column);

    bool found_available_column = false;
    for (int column_index = column_start; column_index <= m_occupation_grid.max_column_index(); column_index++) {
        if (!grid_area_is_occupied(column_index, row_start, column_span, row_span)) {
            found_available_column = true;
            column_start = column_index;
            break;
        }
    }
    if (!found_available_column) {
        column_start = m_occupation_grid.max_column_index() + 1;
    }

    record_grid_placement(GridItem {
        .box = child_box,
        .used_values = m_state.get_mutable(child_box),
        .row = row_start,
        .row_span = row_span,
        .column = column_start,
        .column_span = column_span });
}

void GridFormattingContext::place_item_with_column_position(Box const& child_box, int& auto_placement_cursor_row)
{
    auto placement_position = resolve_grid_position(child_box, GridDimension::Column);
    auto column_start = placement_position.start;
    size_t column_span = placement_position.span;

    auto row_span = resolve_grid_span(child_box, GridDimension::Row);

    // https://drafts.csswg.org/css-grid-2/#auto-placement-algo
    // Increment the auto-placement cursor's row position until a value is found
    // where the grid item does not overlap any occupied grid cells (creating
    // new rows in the implicit grid as necessary).
    //
    // https://drafts.csswg.org/css-grid-2/#subgrid-implicit
    // The subgrid does not have any implicit grid tracks in the subgridded
    // dimension(s). Hypothetical implicit grid lines are used to resolve
    // placement as usual when the explicit grid does not have enough lines;
    // however each grid item's grid area is clamped to the subgrid's explicit
    // grid.
    while (true) {
        if (!grid_area_is_occupied(column_start, auto_placement_cursor_row, column_span, row_span))
            break;
        ++auto_placement_cursor_row;
        if (is_subgridded_axis(GridDimension::Row) && auto_placement_cursor_row > m_occupation_grid.max_row_index())
            break;
    }

    record_grid_placement(GridItem {
        .box = child_box,
        .used_values = m_state.get_mutable(child_box),
        .row = auto_placement_cursor_row,
        .row_span = row_span,
        .column = column_start,
        .column_span = column_span });
}

void GridFormattingContext::place_item_with_no_declared_position(Box const& child_box, int& auto_placement_cursor_column, int& auto_placement_cursor_row)
{
    auto column_start = 0;
    auto column_span = resolve_grid_span(child_box, GridDimension::Column);
    auto row_start = 0;
    auto row_span = resolve_grid_span(child_box, GridDimension::Row);

    auto const& auto_flow = grid_container().computed_values().grid_auto_flow();
    auto dimension = auto_flow.row ? GridDimension::Column : GridDimension::Row;

    // 4.1.2.1. Increment the column position of the auto-placement cursor until either this item's grid
    // area does not overlap any occupied grid cells, or the cursor's column position, plus the item's
    // column span, overflow the number of columns in the implicit grid, as determined earlier in this
    // algorithm.
    auto found_unoccupied_area = find_unoccupied_grid_area(dimension, auto_placement_cursor_column, auto_placement_cursor_row, column_span, row_span);

    // 4.1.2.2. If a non-overlapping position was found in the previous step, set the item's row-start
    // and column-start lines to the cursor's position. Otherwise, increment the auto-placement cursor's
    // row position (creating new rows in the implicit grid as necessary), set its column position to the
    // start-most column line in the implicit grid, and return to the previous step.
    column_start = auto_placement_cursor_column;
    row_start = auto_placement_cursor_row;

    auto_placement_cursor_column += column_span - 1;
    auto_placement_cursor_row += row_span - 1;

    if (found_unoccupied_area == FoundUnoccupiedPlace::Yes) {
        if (dimension == GridDimension::Column) {
            auto_placement_cursor_column++;
            auto_placement_cursor_row = m_occupation_grid.min_row_index();
        } else {
            auto_placement_cursor_row++;
            auto_placement_cursor_column = m_occupation_grid.min_column_index();
        }
    }

    record_grid_placement(GridItem {
        .box = child_box,
        .used_values = m_state.get_mutable(child_box),
        .row = row_start,
        .row_span = row_span,
        .column = column_start,
        .column_span = column_span });
}

void GridFormattingContext::clamp_grid_area_to_subgrid(GridDimension dimension, int& start, size_t& span) const
{
    if (!is_subgridded_axis(dimension))
        return;

    // https://drafts.csswg.org/css-grid-2/#subgrid-implicit
    // The subgrid does not have any implicit grid tracks in the subgridded dimension(s).
    // Hypothetical implicit grid lines are used to resolve placement as usual when the
    // explicit grid does not have enough lines; however each grid item's grid area is
    // clamped to the subgrid's explicit grid.
    //
    // https://drafts.csswg.org/css-grid-2/#overlarge-grids
    // To clamp a grid area:
    // * If the grid area would span outside the limited grid, its span is clamped to the
    //   last line of the limited grid.
    // * If the grid area would be placed completely outside the limited grid, its span must
    //   be truncated to 1 and the area repositioned into the last grid track on that side
    //   of the grid.
    auto explicit_grid_track_count = dimension == GridDimension::Column ? m_column_lines.size() - 1 : m_row_lines.size() - 1;
    if (explicit_grid_track_count == 0)
        return;

    auto end = start + static_cast<int>(span);
    auto limited_grid_start = 0;
    auto limited_grid_end = static_cast<int>(explicit_grid_track_count);

    if (end <= limited_grid_start) {
        start = limited_grid_start;
        end = limited_grid_start + 1;
    } else if (start >= limited_grid_end) {
        start = limited_grid_end - 1;
        end = limited_grid_end;
    } else {
        start = max(start, limited_grid_start);
        end = min(end, limited_grid_end);
    }

    span = static_cast<size_t>(end - start);
}

void GridFormattingContext::clamp_grid_area_to_subgrid(GridItem& grid_item) const
{
    auto column_start = grid_item.column.value();
    auto column_span = grid_item.column_span.value();
    clamp_grid_area_to_subgrid(GridDimension::Column, column_start, column_span);
    grid_item.column = column_start;
    grid_item.column_span = column_span;

    auto row_start = grid_item.row.value();
    auto row_span = grid_item.row_span.value();
    clamp_grid_area_to_subgrid(GridDimension::Row, row_start, row_span);
    grid_item.row = row_start;
    grid_item.row_span = row_span;
}

bool GridFormattingContext::grid_area_is_occupied(int column_start, int row_start, size_t column_span, size_t row_span) const
{
    clamp_grid_area_to_subgrid(GridDimension::Column, column_start, column_span);
    clamp_grid_area_to_subgrid(GridDimension::Row, row_start, row_span);
    return m_occupation_grid.is_area_occupied(column_start, row_start, static_cast<int>(column_span), static_cast<int>(row_span));
}

FoundUnoccupiedPlace GridFormattingContext::find_unoccupied_grid_area(GridDimension dimension, int& column_index, int& row_index, size_t column_span, size_t row_span) const
{
    if (dimension == GridDimension::Column) {
        // Row-flow: columns are the inner (minor) axis, rows are the outer (major) axis.
        while (row_index <= m_occupation_grid.max_row_index()) {
            while (column_index <= m_occupation_grid.max_column_index()) {
                auto candidate_column_index = column_index;
                auto candidate_row_index = row_index;
                auto candidate_column_span = column_span;
                auto candidate_row_span = row_span;
                clamp_grid_area_to_subgrid(GridDimension::Column, candidate_column_index, candidate_column_span);
                clamp_grid_area_to_subgrid(GridDimension::Row, candidate_row_index, candidate_row_span);

                auto minor_axis_fits = candidate_column_index + static_cast<int>(candidate_column_span) - 1 <= m_occupation_grid.max_column_index();
                if (minor_axis_fits && !m_occupation_grid.is_area_occupied(candidate_column_index, candidate_row_index, static_cast<int>(candidate_column_span), static_cast<int>(candidate_row_span)))
                    return FoundUnoccupiedPlace::Yes;
                column_index++;
            }
            row_index++;
            column_index = m_occupation_grid.min_column_index();
        }
    } else {
        // Column-flow: rows are the inner (minor) axis, columns are the outer (major) axis.
        while (column_index <= m_occupation_grid.max_column_index()) {
            while (row_index <= m_occupation_grid.max_row_index()) {
                auto candidate_column_index = column_index;
                auto candidate_row_index = row_index;
                auto candidate_column_span = column_span;
                auto candidate_row_span = row_span;
                clamp_grid_area_to_subgrid(GridDimension::Column, candidate_column_index, candidate_column_span);
                clamp_grid_area_to_subgrid(GridDimension::Row, candidate_row_index, candidate_row_span);

                auto minor_axis_fits = candidate_row_index + static_cast<int>(candidate_row_span) - 1 <= m_occupation_grid.max_row_index();
                if (minor_axis_fits && !m_occupation_grid.is_area_occupied(candidate_column_index, candidate_row_index, static_cast<int>(candidate_column_span), static_cast<int>(candidate_row_span)))
                    return FoundUnoccupiedPlace::Yes;
                row_index++;
            }
            column_index++;
            row_index = m_occupation_grid.min_row_index();
        }
    }

    return FoundUnoccupiedPlace::No;
}

void GridFormattingContext::record_grid_placement(GridItem grid_item)
{
    clamp_grid_area_to_subgrid(grid_item);

    m_occupation_grid.set_occupied(grid_item.column.value(), grid_item.column.value() + grid_item.column_span.value(), grid_item.row.value(), grid_item.row.value() + grid_item.row_span.value());
    m_grid_items.append(grid_item);
}

void GridFormattingContext::initialize_grid_tracks_from_definition(GridDimension dimension)
{
    auto const& grid_computed_values = grid_container().computed_values();
    auto const& tracks_definition = dimension == GridDimension::Column ? grid_computed_values.grid_template_columns().track_list() : grid_computed_values.grid_template_rows().track_list();
    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    for (auto const& track_definition : tracks_definition) {
        int repeat_count = 1;
        bool is_auto_fit = false;
        bool is_auto_repeat = false;
        if (track_definition.is_repeat()) {
            is_auto_fit = track_definition.repeat().is_auto_fit();
            if (track_definition.repeat().is_auto_fill() || is_auto_fit) {
                is_auto_repeat = true;
                repeat_count = count_of_repeated_auto_fill_or_fit_tracks(dimension, track_definition);
            } else {
                repeat_count = track_definition.repeat().repeat_count();
            }
        }
        for (auto _ = 0; _ < repeat_count; _++) {
            if (track_definition.is_default() || track_definition.is_minmax()) {
                tracks.append(GridTrack::create_from_definition(track_definition, is_auto_fit, is_auto_repeat));
            } else if (track_definition.is_repeat()) {
                for (auto& explicit_grid_track : track_definition.repeat().grid_track_size_list().track_list()) {
                    tracks.append(GridTrack::create_from_definition(explicit_grid_track, is_auto_fit, is_auto_repeat));
                }
            } else {
                VERIFY_NOT_REACHED();
            }
        }
    }
}

void GridFormattingContext::initialize_grid_tracks_from_subgrid(GridDimension dimension)
{
    // https://drafts.csswg.org/css-grid-2/#subgrid-tracks
    // Placing the subgrid creates a correspondence between its subgridded tracks and those that it
    // spans in its parent grid. The grid lines thus shared between the subgrid and its parent form the
    // subgrid's explicit grid, and its track sizes are governed by the parent grid.
    auto const* parent_grid = parent_grid_formatting_context();
    auto const* item = parent_grid_item();
    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;

    if (!parent_grid || !item) {
        tracks.append(GridTrack::create_auto());
        return;
    }

    auto const& parent_tracks = dimension == GridDimension::Column ? parent_grid->m_grid_columns : parent_grid->m_grid_rows;
    auto parent_start = item->raw_position(dimension);
    for (size_t i = 0; i < item->span(dimension); ++i) {
        auto parent_track_index = parent_start + static_cast<int>(i);
        if (parent_track_index >= 0 && static_cast<size_t>(parent_track_index) < parent_tracks.size()) {
            if (m_layout_mode == LayoutMode::IntrinsicSizing) {
                // https://drafts.csswg.org/css-grid-2/#subgrid-size-contribution
                // The subgrid itself lays out as an ordinary grid item in its parent grid,
                // but acts as if it was completely empty for track sizing purposes in the
                // subgridded dimension.
                //
                // https://drafts.csswg.org/css-grid-2/#subgrid-item-contribution
                // The subgrid's own grid items participate in the sizing of its parent grid
                // in the subgridded dimension(s) and are aligned to it in those dimensions.
                tracks.append(GridTrack::create_from_subgrid_parent_track(parent_tracks[parent_track_index]));
            } else {
                tracks.append(GridTrack::create_fixed(parent_tracks[parent_track_index].base_size));
            }
        } else {
            tracks.append(GridTrack::create_auto());
        }
    }

    if (tracks.is_empty())
        tracks.append(GridTrack::create_auto());
}

void GridFormattingContext::initialize_grid_tracks_for_columns_and_rows()
{
    auto const& grid_computed_values = grid_container().computed_values();

    if (is_subgridded_axis(GridDimension::Column)) {
        initialize_grid_tracks_from_subgrid(GridDimension::Column);
    } else {
        auto const& grid_auto_columns = grid_computed_values.grid_auto_columns().track_list();
        size_t implicit_column_index = 0;
        // NOTE: If there are implicit tracks created by items with negative indexes they should prepend explicitly defined tracks
        auto negative_index_implied_column_tracks_count = abs(m_occupation_grid.min_column_index());
        m_explicit_columns_start_line_index = negative_index_implied_column_tracks_count;
        for (int i = 0; i < negative_index_implied_column_tracks_count; i++)
            m_column_lines.insert(0, {});
        for (int column_index = 0; column_index < negative_index_implied_column_tracks_count; column_index++) {
            if (grid_auto_columns.size() > 0) {
                auto definition = grid_auto_columns[implicit_column_index % grid_auto_columns.size()];
                m_grid_columns.append(GridTrack::create_from_definition(definition));
            } else {
                m_grid_columns.append(GridTrack::create_auto());
            }
            implicit_column_index++;
        }
        initialize_grid_tracks_from_definition(GridDimension::Column);
        for (size_t column_index = m_grid_columns.size(); column_index < m_occupation_grid.column_count(); column_index++) {
            if (grid_auto_columns.size() > 0) {
                auto definition = grid_auto_columns[implicit_column_index % grid_auto_columns.size()];
                m_grid_columns.append(GridTrack::create_from_definition(definition));
            } else {
                m_grid_columns.append(GridTrack::create_auto());
            }
            implicit_column_index++;
        }
    }

    if (is_subgridded_axis(GridDimension::Row)) {
        initialize_grid_tracks_from_subgrid(GridDimension::Row);
    } else {
        auto const& grid_auto_rows = grid_computed_values.grid_auto_rows().track_list();
        size_t implicit_row_index = 0;
        // NOTE: If there are implicit tracks created by items with negative indexes they should prepend explicitly defined tracks
        auto negative_index_implied_row_tracks_count = abs(m_occupation_grid.min_row_index());
        m_explicit_rows_start_line_index = negative_index_implied_row_tracks_count;
        for (int i = 0; i < negative_index_implied_row_tracks_count; i++)
            m_row_lines.insert(0, {});
        for (int row_index = 0; row_index < negative_index_implied_row_tracks_count; row_index++) {
            if (grid_auto_rows.size() > 0) {
                auto definition = grid_auto_rows[implicit_row_index % grid_auto_rows.size()];
                m_grid_rows.append(GridTrack::create_from_definition(definition));
            } else {
                m_grid_rows.append(GridTrack::create_auto());
            }
            implicit_row_index++;
        }
        initialize_grid_tracks_from_definition(GridDimension::Row);
        for (size_t row_index = m_grid_rows.size(); row_index < m_occupation_grid.row_count(); row_index++) {
            if (grid_auto_rows.size() > 0) {
                auto definition = grid_auto_rows[implicit_row_index % grid_auto_rows.size()];
                m_grid_rows.append(GridTrack::create_from_definition(definition));
            } else {
                m_grid_rows.append(GridTrack::create_auto());
            }
            implicit_row_index++;
        }
    }

    m_column_lines.resize(m_grid_columns.size() + 1);
    m_row_lines.resize(m_grid_rows.size() + 1);
}

void GridFormattingContext::initialize_gap_tracks(GridDimension dimension, AvailableSize const& available_size)
{
    // https://www.w3.org/TR/css-grid-2/#gutters
    // 11.1. Gutters: the row-gap, column-gap, and gap properties
    // For the purpose of track sizing, each gutter is treated as an extra, empty, fixed-size track of the specified
    // size, which is spanned by any grid items that span across its corresponding grid line.
    auto& grid_tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    auto& gap_tracks = dimension == GridDimension::Column ? m_column_gap_tracks : m_row_gap_tracks;
    auto& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;

    gap_tracks.clear();
    tracks_and_gaps.clear();

    if (grid_tracks.is_empty())
        return;

    auto gap_size = resolved_gap_size(dimension, available_size);

    gap_tracks.ensure_capacity(grid_tracks.size() - 1);

    for (size_t track_index = 0; track_index < grid_tracks.size(); track_index++) {
        tracks_and_gaps.append(grid_tracks[track_index]);

        if (track_index != grid_tracks.size() - 1) {
            gap_tracks.unchecked_append(GridTrack::create_gap(gap_size));
            tracks_and_gaps.append(gap_tracks.last());
        }
    }
}

void GridFormattingContext::initialize_gap_tracks(AvailableSpace const& available_space)
{
    initialize_gap_tracks(GridDimension::Column, available_space.width);
    initialize_gap_tracks(GridDimension::Row, available_space.height);
}

void GridFormattingContext::initialize_track_sizes(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#algo-init
    // 12.4. Initialize Track Sizes
    // Initialize each track’s base size and growth limit.

    auto& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
    auto& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

    if (dimension == GridDimension::Column)
        m_has_flexible_column_tracks = false;
    else
        m_has_flexible_row_tracks = false;

    for (auto& track : tracks_and_gaps) {
        track.base_size_frozen = false;
        track.growth_limit_frozen = false;
        track.infinitely_growable = false;
        track.space_to_distribute = 0;
        track.planned_increase = 0;
        track.item_incurred_increase = 0;

        if (track.is_gap)
            continue;

        // Normalize fit-content tracks with unresolvable percentage arguments to max-content,
        // since the percentage cannot be resolved against an indefinite available size.
        if (!available_size.is_definite()
            && track.max_track_sizing_function.is_fit_content()
            && track.max_track_sizing_function.css_size().contains_percentage()) {
            track.max_track_sizing_function = CSS::GridSize(CSS::KeywordStyleValue::create(CSS::Keyword::MaxContent));
        }

        if (track.min_track_sizing_function.is_fixed(available_size)) {
            track.base_size = track.min_track_sizing_function.css_size().to_px(grid_container(), available_size.to_px_or_zero());
        } else if (track.min_track_sizing_function.is_intrinsic(available_size)) {
            track.base_size = 0;
        }

        if (track.max_track_sizing_function.is_fixed(available_size)) {
            track.growth_limit = track.max_track_sizing_function.css_size().to_px(grid_container(), available_size.to_px_or_zero());
        } else if (track.max_track_sizing_function.is_flexible_length()) {
            if (dimension == GridDimension::Column) {
                m_has_flexible_column_tracks = true;
            } else {
                m_has_flexible_row_tracks = true;
            }
            track.growth_limit = {};
        } else if (track.max_track_sizing_function.is_intrinsic(available_size)) {
            track.growth_limit = {};
        } else {
            VERIFY_NOT_REACHED();
        }

        // In all cases, if the growth limit is less than the base size, increase the growth limit to match
        // the base size.
        if (track.growth_limit.has_value() && track.growth_limit.value() < track.base_size)
            track.growth_limit = track.base_size;
    }
}

void GridFormattingContext::resolve_intrinsic_track_sizes(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#algo-content
    // 12.5. Resolve Intrinsic Track Sizes
    // This step resolves intrinsic track sizing functions to absolute lengths. First it resolves those
    // sizes based on items that are contained wholly within a single track. Then it gradually adds in
    // the space requirements of items that span multiple tracks, evenly distributing the extra space
    // across those tracks insofar as possible.

    auto& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;

    // FIXME: 1. Shim baseline-aligned items so their intrinsic size contributions reflect their baseline alignment.

    // 2. Size tracks to fit non-spanning items:
    increase_sizes_to_accommodate_spanning_items_crossing_content_sized_tracks(dimension, 1);

    // 3. Increase sizes to accommodate spanning items crossing content-sized tracks: Next, consider the
    // items with a span of 2 that do not span a track with a flexible sizing function.
    // Repeat incrementally for items with greater spans until all items have been considered.
    size_t max_item_span = 1;
    for_each_item_contributing_to_track_sizing(dimension, [&](GridItem const& item) {
        max_item_span = max(item.span(dimension), max_item_span);
    });
    for (size_t span = 2; span <= max_item_span; span++)
        increase_sizes_to_accommodate_spanning_items_crossing_content_sized_tracks(dimension, span);

    // 4. Increase sizes to accommodate spanning items crossing flexible tracks: Next, repeat the previous
    // step instead considering (together, rather than grouped by span size) all items that do span a
    // track with a flexible sizing function while
    if (has_flexible_tracks(dimension)) {
        increase_sizes_to_accommodate_spanning_items_crossing_flexible_tracks(dimension);
    }

    // 5. If any track still has an infinite growth limit (because, for example, it had no items placed in
    // it or it is a flexible track), set its growth limit to its base size.
    for (auto& track : tracks_and_gaps) {
        if (!track.growth_limit.has_value())
            track.growth_limit = track.base_size;
    }
}

template<typename Match>
void GridFormattingContext::distribute_extra_space_across_spanned_tracks_base_size(GridDimension dimension, CSSPixels item_size_contribution, SpaceDistributionPhase phase, Vector<GridTrack&>& spanned_tracks, Match matcher)
{
    auto& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

    Vector<GridTrack&> affected_tracks;
    for (auto& track : spanned_tracks) {
        if (matcher(track))
            affected_tracks.append(track);
    }

    if (affected_tracks.size() == 0)
        return;

    for (auto& track : affected_tracks)
        track.item_incurred_increase = 0;

    // 1. Find the space to distribute:
    CSSPixels spanned_tracks_sizes_sum = 0;
    for (auto& track : spanned_tracks)
        spanned_tracks_sizes_sum += track.base_size;

    // Subtract the corresponding size of every spanned track from the item’s size contribution to find the item’s
    // remaining size contribution.
    auto extra_space = max(CSSPixels(0), item_size_contribution - spanned_tracks_sizes_sum);

    // 2. Distribute space up to limits:
    while (extra_space > 0) {
        auto all_frozen = all_of(affected_tracks, [](auto const& track) { return track.base_size_frozen; });
        if (all_frozen)
            break;

        // Find the item-incurred increase for each spanned track with an affected size by: distributing the space
        // equally among such tracks, freezing a track’s item-incurred increase as its affected size + item-incurred
        // increase reaches its limit
        CSSPixels increase_per_track = max(CSSPixels::smallest_positive_value(), extra_space / affected_tracks.size());
        for (auto& track : affected_tracks) {
            if (track.base_size_frozen)
                continue;

            auto increase = min(increase_per_track, extra_space);

            if (track.growth_limit.has_value()) {
                auto maximum_increase = track.growth_limit.value() - track.base_size;
                if (track.item_incurred_increase + increase >= maximum_increase) {
                    track.base_size_frozen = true;
                    increase = maximum_increase - track.item_incurred_increase;
                }
            }
            track.item_incurred_increase += increase;
            extra_space -= increase;
        }
    }

    // 3. Distribute space beyond limits
    if (extra_space > 0) {
        Vector<GridTrack&> tracks_to_grow_beyond_limits;

        // If space remains after all tracks are frozen, unfreeze and continue to
        // distribute space to the item-incurred increase of...
        if (phase == SpaceDistributionPhase::AccommodateMinimumContribution || phase == SpaceDistributionPhase::AccommodateMinContentContribution) {
            // when accommodating minimum contributions or accommodating min-content contributions: any affected track
            // that happens to also have an intrinsic max track sizing function
            for (auto& track : affected_tracks) {
                if (track.max_track_sizing_function.is_intrinsic(available_size))
                    tracks_to_grow_beyond_limits.append(track);
            }

            // if there are no such tracks, then all affected tracks.
            if (tracks_to_grow_beyond_limits.is_empty())
                tracks_to_grow_beyond_limits = affected_tracks;
        } else if (phase == SpaceDistributionPhase::AccommodateMaxContentContribution) {
            // when accommodating max-content contributions into base sizes: any affected track that happens to also have
            // a max-content max track sizing function;
            for (auto& track : affected_tracks) {
                if (track.max_track_sizing_function.is_max_content())
                    tracks_to_grow_beyond_limits.append(track);
            }

            // if there are no such tracks, then all affected tracks.
            if (tracks_to_grow_beyond_limits.is_empty())
                tracks_to_grow_beyond_limits = affected_tracks;
        }

        CSSPixels increase_per_track = extra_space / tracks_to_grow_beyond_limits.size();
        for (auto& track : tracks_to_grow_beyond_limits) {
            auto increase = min(increase_per_track, extra_space);
            track.item_incurred_increase += increase;
            extra_space -= increase;
        }
    }

    // 4. For each affected track, if the track’s item-incurred increase is larger than the track’s planned increase
    //    set the track’s planned increase to that value.
    for (auto& track : affected_tracks) {
        if (track.item_incurred_increase > track.planned_increase)
            track.planned_increase = track.item_incurred_increase;
    }
}

template<typename Match>
void GridFormattingContext::distribute_extra_space_across_spanned_tracks_growth_limit(CSSPixels item_size_contribution, Vector<GridTrack&>& spanned_tracks, Match matcher)
{
    Vector<GridTrack&> affected_tracks;
    for (auto& track : spanned_tracks) {
        if (matcher(track))
            affected_tracks.append(track);
    }

    for (auto& track : affected_tracks)
        track.item_incurred_increase = 0;

    if (affected_tracks.size() == 0)
        return;

    // 1. Find the space to distribute:
    CSSPixels spanned_tracks_sizes_sum = 0;
    for (auto& track : spanned_tracks) {
        if (track.growth_limit.has_value()) {
            spanned_tracks_sizes_sum += track.growth_limit.value();
        } else {
            spanned_tracks_sizes_sum += track.base_size;
        }
    }

    // Subtract the corresponding size of every spanned track from the item’s size contribution to find the item’s
    // remaining size contribution.
    auto extra_space = max(CSSPixels(0), item_size_contribution - spanned_tracks_sizes_sum);

    // 2. Distribute space up to limits:
    while (extra_space > 0) {
        auto all_frozen = all_of(affected_tracks, [](auto const& track) { return track.growth_limit_frozen; });
        if (all_frozen)
            break;

        // Find the item-incurred increase for each spanned track with an affected size by: distributing the space
        // equally among such tracks, freezing a track’s item-incurred increase as its affected size + item-incurred
        // increase reaches its limit
        CSSPixels increase_per_track = max(CSSPixels::smallest_positive_value(), extra_space / affected_tracks.size());
        for (auto& track : affected_tracks) {
            if (track.growth_limit_frozen)
                continue;

            auto increase = min(increase_per_track, extra_space);

            // For growth limits, the limit is infinity if it is marked as infinitely growable, and equal to the
            // growth limit otherwise.
            if (!track.infinitely_growable && track.growth_limit.has_value()) {
                auto maximum_increase = track.growth_limit.value() - track.base_size;
                if (track.item_incurred_increase + increase >= maximum_increase) {
                    track.growth_limit_frozen = true;
                    increase = maximum_increase - track.item_incurred_increase;
                }
            }
            track.item_incurred_increase += increase;
            extra_space -= increase;
        }
    }

    // FIXME: 3. Distribute space beyond limits

    // 4. For each affected track, if the track’s item-incurred increase is larger than the track’s planned increase
    //    set the track’s planned increase to that value.
    for (auto& track : spanned_tracks) {
        if (track.item_incurred_increase > track.planned_increase)
            track.planned_increase = track.item_incurred_increase;
    }
}

void GridFormattingContext::increase_sizes_to_accommodate_spanning_items_crossing_content_sized_tracks(GridDimension dimension, size_t span)
{
    auto& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;
    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    for_each_item_contributing_to_track_sizing(dimension, [&](GridItem const& item) {
        auto const item_span = item.span(dimension);
        if (item_span != span)
            return;

        Vector<GridTrack&> spanned_tracks;
        for_each_spanned_track_by_item(item, dimension, [&](GridTrack& track) {
            spanned_tracks.append(track);
        });

        bool item_spans_tracks_with_flexible_sizing_function = false;
        bool item_spans_tracks_with_intrinsic_sizing_function = false;
        for (auto& track : spanned_tracks) {
            if (track.max_track_sizing_function.is_flexible_length())
                item_spans_tracks_with_flexible_sizing_function = true;
            if (track.min_track_sizing_function.is_intrinsic(available_size) || track.max_track_sizing_function.is_intrinsic(available_size))
                item_spans_tracks_with_intrinsic_sizing_function = true;
        }
        if (!item_spans_tracks_with_intrinsic_sizing_function || item_spans_tracks_with_flexible_sizing_function)
            return;

        // 1. For intrinsic minimums: First increase the base size of tracks with an intrinsic min track sizing
        //    function by distributing extra space as needed to accommodate these items’ minimum contributions.
        auto item_size_contribution = [&] {
            // If the grid container is being sized under a min- or max-content constraint, use the items’ limited
            // min-content contributions in place of their minimum contributions here.
            if (available_size.is_intrinsic_sizing_constraint())
                return calculate_limited_min_content_contribution(item, dimension);
            return calculate_minimum_contribution(item, dimension);
        }();
        distribute_extra_space_across_spanned_tracks_base_size(dimension, item_size_contribution, SpaceDistributionPhase::AccommodateMinimumContribution, spanned_tracks, [&](GridTrack const& track) {
            return track.min_track_sizing_function.is_intrinsic(available_size);
        });
        for (auto& track : spanned_tracks) {
            track.base_size += track.planned_increase;
            track.planned_increase = 0;
        }

        // 2. For content-based minimums: Next continue to increase the base size of tracks with a min track
        //    sizing function of min-content or max-content by distributing extra space as needed to account for
        //    these items' min-content contributions.
        auto item_min_content_contribution = calculate_min_content_contribution(item, dimension);
        distribute_extra_space_across_spanned_tracks_base_size(dimension, item_min_content_contribution, SpaceDistributionPhase::AccommodateMinContentContribution, spanned_tracks, [&](GridTrack const& track) {
            return track.min_track_sizing_function.is_min_content() || track.min_track_sizing_function.is_max_content();
        });
        for (auto& track : spanned_tracks) {
            track.base_size += track.planned_increase;
            track.planned_increase = 0;
        }

        // 3. For max-content minimums: Next, if the grid container is being sized under a max-content constraint,
        //    continue to increase the base size of tracks with a min track sizing function of auto or max-content by
        //    distributing extra space as needed to account for these items' limited max-content contributions.
        if (available_size.is_max_content()) {
            auto item_limited_max_content_contribution = calculate_limited_max_content_contribution(item, dimension);
            distribute_extra_space_across_spanned_tracks_base_size(dimension, item_limited_max_content_contribution, SpaceDistributionPhase::AccommodateMaxContentContribution, spanned_tracks, [&](GridTrack const& track) {
                return track.min_track_sizing_function.is_auto(available_size) || track.min_track_sizing_function.is_max_content();
            });
            for (auto& track : spanned_tracks) {
                track.base_size += track.planned_increase;
                track.planned_increase = 0;
            }
        }

        // 4. If at this point any track’s growth limit is now less than its base size, increase its growth limit to
        //    match its base size.
        for (auto& track : tracks) {
            if (track.growth_limit.has_value() && track.growth_limit.value() < track.base_size)
                track.growth_limit = track.base_size;
        }

        // 5. For intrinsic maximums: Next increase the growth limit of tracks with an intrinsic max track sizing
        distribute_extra_space_across_spanned_tracks_growth_limit(item_min_content_contribution, spanned_tracks, [&](GridTrack const& track) {
            return track.max_track_sizing_function.is_intrinsic(available_size);
        });
        for (auto& track : spanned_tracks) {
            if (!track.growth_limit.has_value()) {
                // If the affected size is an infinite growth limit, set it to the track’s base size plus the planned increase.
                track.growth_limit = track.base_size + track.planned_increase;
                // Mark any tracks whose growth limit changed from infinite to finite in this step as infinitely growable
                // for the next step.
                track.infinitely_growable = true;
            } else {
                track.growth_limit.value() += track.planned_increase;
            }
            track.planned_increase = 0;
        }

        // 6. For max-content maximums: Lastly continue to increase the growth limit of tracks with a max track
        //    sizing function of max-content by distributing extra space as needed to account for these items' max-
        //    content contributions. However, limit the growth of any fit-content() tracks by their fit-content() argument.
        auto item_max_content_contribution = calculate_max_content_contribution(item, dimension);
        distribute_extra_space_across_spanned_tracks_growth_limit(item_max_content_contribution, spanned_tracks, [&](GridTrack const& track) {
            return track.max_track_sizing_function.is_max_content() || track.max_track_sizing_function.is_auto(available_size) || track.max_track_sizing_function.is_fit_content();
        });
        for (auto& track : spanned_tracks) {
            if (track.max_track_sizing_function.is_fit_content()) {
                track.growth_limit.value() += track.planned_increase;
                if (track.growth_limit.value() < track.base_size)
                    track.growth_limit = track.base_size;
                auto fit_content_limit = track.max_track_sizing_function.css_size().to_px(grid_container(), available_size.to_px_or_zero());
                if (track.growth_limit.value() > fit_content_limit)
                    track.growth_limit = max(track.base_size, fit_content_limit);
            } else if (!track.growth_limit.has_value()) {
                // If the affected size is an infinite growth limit, set it to the track’s base size plus the planned increase.
                track.growth_limit = track.base_size + track.planned_increase;
            } else {
                track.growth_limit.value() += track.planned_increase;
            }
            track.planned_increase = 0;
        }
    });
}

void GridFormattingContext::increase_sizes_to_accommodate_spanning_items_crossing_flexible_tracks(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-1/#algo-spanning-flex-items
    // 11.5.4. Increase sizes to accommodate spanning items crossing flexible tracks

    auto const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

    auto dominated_by_available_size = [&](GridTrack const& track) {
        // NB: This step repeats the content-sized track step, but only distributes space to flexible tracks.
        // For min-content column sizing, the later "Expand Flexible Tracks" step resolves the flex fraction
        // to zero, so fixed-min flexible columns must not grow from their items' intrinsic width here.
        // Keep min-content row sizing here so intrinsic-height grids still account for their contents.
        return available_size.is_max_content()
            || (dimension == GridDimension::Row && available_size.is_min_content())
            || track.min_track_sizing_function.is_intrinsic(available_size);
    };

    HashMap<GridTrack*, CSSPixels> track_contributions;
    for_each_item_contributing_to_track_sizing(dimension, [&](GridItem const& item) {
        Vector<GridTrack&> spanned_tracks;
        for_each_spanned_track_by_item(item, dimension, [&](GridTrack& track) {
            spanned_tracks.append(track);
        });

        double total_flex = 0;
        size_t intrinsic_flexible_track_count = 0;
        CSSPixels non_flexible_space = 0;
        for (auto const& track : spanned_tracks) {
            if (track.max_track_sizing_function.is_flexible_length() && dominated_by_available_size(track)) {
                total_flex += track.max_track_sizing_function.flex_factor();
                ++intrinsic_flexible_track_count;
            } else {
                non_flexible_space += track.base_size;
            }
        }

        if (intrinsic_flexible_track_count == 0)
            return;

        // If the grid container is being sized under a min- or max-content constraint, use the items' limited
        // min-content contributions in place of their minimum contributions here.
        auto item_size_contribution = [&] {
            if (available_size.is_intrinsic_sizing_constraint()) {
                // https://drafts.csswg.org/css-grid-2/#min-size-auto
                // A grid item's automatic minimum size is zero if its computed overflow is a scrollable
                // overflow value. Preserve that zero minimum for collapsed zero-flex tracks.
                if (total_flex == 0 && item.box.is_scroll_container())
                    return calculate_minimum_contribution(item, dimension);
                return calculate_limited_min_content_contribution(item, dimension);
            }
            return calculate_minimum_contribution(item, dimension);
        }();
        // NB: Subtract the space already accounted for by non-flexible spanned tracks (sized in 11.5.3), since only
        //     the remaining contribution needs to be distributed among flexible tracks.
        item_size_contribution = max(CSSPixels(0), item_size_contribution - non_flexible_space);

        // Distributing space to flexible tracks:
        // - If the sum of the flexible sizing functions of all flexible tracks spanned by the item is greater
        //   than or equal to one, distributing space to such tracks according to the ratios of their flexible
        //   sizing functions rather than distributing space equally.
        // - If the sum is less than one, distributing that proportion of space according to the ratios of their
        //   flexible sizing functions and the rest equally.
        // FIXME: Handle 0 < total_flex < 1 case separately per spec.
        for (auto& track : spanned_tracks) {
            if (!track.max_track_sizing_function.is_flexible_length() || !dominated_by_available_size(track))
                continue;
            double flex = track.max_track_sizing_function.flex_factor();
            CSSPixels contribution = total_flex > 0
                ? CSSPixels::nearest_value_for(item_size_contribution.to_double() * (flex / total_flex))
                : item_size_contribution / intrinsic_flexible_track_count;
            auto& track_contribution = track_contributions.ensure(&track, [] { return 0; });
            if (track_contribution < contribution)
                track_contribution = contribution;
        }
    });

    for (auto& [track, contribution] : track_contributions) {
        if (contribution > track->base_size)
            track->base_size = contribution;
        // If at this point any track's growth limit is now less than its base size, increase its growth limit to match
        // its base size.
        if (track->growth_limit.has_value() && track->growth_limit.value() < track->base_size)
            track->growth_limit = track->base_size;
    }
}

void GridFormattingContext::maximize_tracks_using_available_size(AvailableSpace const& available_space, GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#algo-grow-tracks
    // 12.6. Maximize Tracks

    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;

    auto get_free_space_px = [&]() -> CSSPixels {
        // For the purpose of this step: if sizing the grid container under a max-content constraint, the
        // free space is infinite; if sizing under a min-content constraint, the free space is zero.
        auto free_space = get_free_space(available_space, dimension);
        if (free_space.is_max_content() || free_space.is_indefinite())
            return CSSPixels::max();
        if (free_space.is_min_content())
            return 0;
        return free_space.to_px_or_zero();
    };

    auto free_space_px = get_free_space_px();

    // If the free space is positive, distribute it equally to the base sizes of all tracks, freezing
    // tracks as they reach their growth limits (and continuing to grow the unfrozen tracks as needed).
    size_t growable_track_count = 0;
    for (auto& track : tracks) {
        if (track.base_size_frozen)
            continue;
        VERIFY(track.growth_limit.has_value());
        if (track.base_size < track.growth_limit.value())
            growable_track_count++;
    }
    while (free_space_px > 0 && growable_track_count > 0) {
        auto free_space_to_distribute_per_track = free_space_px / growable_track_count;
        for (auto& track : tracks) {
            if (track.base_size_frozen)
                continue;
            if (track.base_size >= track.growth_limit.value())
                continue;
            auto new_base_size = min(track.growth_limit.value(), track.base_size + free_space_to_distribute_per_track);
            if (new_base_size >= track.growth_limit.value())
                --growable_track_count;
            track.base_size = new_base_size;
        }
        if (get_free_space_px() == free_space_px)
            break;
        free_space_px = get_free_space_px();
    }
}

void GridFormattingContext::maximize_tracks(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#algo-grow-tracks
    // 12.6. Maximize Tracks

    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;

    Vector<CSSPixels> saved_base_sizes;
    for (auto& track : tracks)
        saved_base_sizes.append(track.base_size);

    maximize_tracks_using_available_size(*m_available_space, dimension);

    // If this would cause the grid to be larger than the grid container’s inner size as limited by its
    // max-width/height, then redo this step, treating the available grid space as equal to the grid
    // container’s inner size when it’s sized to its max-width/height.
    CSSPixels grid_container_inner_size = 0;
    for (auto& track : tracks)
        grid_container_inner_size += track.base_size;
    auto const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;
    auto const& computed_values = grid_container().computed_values();
    auto should_treat_grid_container_maximum_size_as_none = [&] {
        if (dimension == GridDimension::Column)
            return should_treat_max_width_as_none(grid_container(), available_size);
        return !computed_values.max_height().is_auto();
    }();

    if (!should_treat_grid_container_maximum_size_as_none) {
        auto maximum_size = calculate_grid_container_maximum_size(dimension);
        if (grid_container_inner_size > maximum_size) {
            for (size_t i = 0; i < tracks.size(); i++)
                tracks[i].base_size = saved_base_sizes[i];
            auto available_space_with_max_width = *m_available_space;
            if (dimension == GridDimension::Column)
                available_space_with_max_width.width = AvailableSize::make_definite(maximum_size);
            else
                available_space_with_max_width.height = AvailableSize::make_definite(maximum_size);
            maximize_tracks_using_available_size(available_space_with_max_width, dimension);
        }
    }
}

void GridFormattingContext::expand_flexible_tracks(GridDimension dimension)
{
    // https://drafts.csswg.org/css-grid/#algo-flex-tracks
    // 12.7. Expand Flexible Tracks
    // This step sizes flexible tracks using the largest value it can assign to an fr without exceeding
    // the available space.

    auto& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    auto& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;
    // FIXME: This should ideally take a Span, as that is more idomatic, but Span does not yet support holding references
    auto find_the_size_of_an_fr = [&](Vector<GridTrack&> const& tracks, CSSPixels space_to_fill) -> CSSPixelFraction {
        // https://www.w3.org/TR/css-grid-2/#algo-find-fr-size
        auto treat_track_as_inflexiable = MUST(AK::Bitmap::create(tracks.size(), false));
        do {
            // 1. Let leftover space be the space to fill minus the base sizes of the non-flexible grid tracks.
            auto leftover_space = space_to_fill;
            for (auto track_index = 0u; track_index < tracks.size(); track_index++) {
                if (treat_track_as_inflexiable.view().get(track_index) || !tracks[track_index].max_track_sizing_function.is_flexible_length()) {
                    leftover_space -= tracks[track_index].base_size;
                }
            }

            // 2. Let flex factor sum be the sum of the flex factors of the flexible tracks.
            //    If this value is less than 1, set it to 1 instead.
            CSSPixels flex_factor_sum = 0;
            for (auto track_index = 0u; track_index < tracks.size(); track_index++) {
                if (treat_track_as_inflexiable.view().get(track_index) || !tracks[track_index].max_track_sizing_function.is_flexible_length())
                    continue;
                flex_factor_sum += CSSPixels::nearest_value_for(tracks[track_index].max_track_sizing_function.flex_factor());
            }
            if (flex_factor_sum < 1)
                flex_factor_sum = 1;

            // 3. Let the hypothetical fr size be the leftover space divided by the flex factor sum.
            auto hypothetical_fr_size = leftover_space / flex_factor_sum;

            // 4. If the product of the hypothetical fr size and a flexible track’s flex factor is less than the track’s
            //    base size, restart this algorithm treating all such tracks as inflexible.
            bool need_to_restart = false;
            for (auto track_index = 0u; track_index < tracks.size(); track_index++) {
                if (treat_track_as_inflexiable.view().get(track_index) || !tracks[track_index].max_track_sizing_function.is_flexible_length())
                    continue;
                auto scaled_fraction = CSSPixels::nearest_value_for(tracks[track_index].max_track_sizing_function.flex_factor()) * hypothetical_fr_size;
                if (scaled_fraction < tracks[track_index].base_size) {
                    treat_track_as_inflexiable.set(track_index, true);
                    need_to_restart = true;
                }
            }
            if (need_to_restart)
                continue;

            // 5. Return the hypothetical fr size.
            return hypothetical_fr_size;
        } while (true);

        VERIFY_NOT_REACHED();
    };

    // First, find the grid’s used flex fraction:
    auto flex_fraction = [&]() -> CSSPixelFraction {
        auto free_space = get_free_space(*m_available_space, dimension);
        // If the free space is zero or if sizing the grid container under a min-content constraint:
        if ((free_space.is_definite() && free_space.to_px_or_zero() == 0) || available_size.is_min_content()) {
            // The used flex fraction is zero.
            return 0;
            // Otherwise, if the free space is a definite length:
        } else if (free_space.is_definite()) {
            // The used flex fraction is the result of finding the size of an fr using all of the grid tracks and a space
            // to fill of the available grid space.
            return find_the_size_of_an_fr(tracks_and_gaps, available_size.to_px_or_zero());
        } else {
            // Otherwise, if the free space is an indefinite length:
            // The used flex fraction is the maximum of:
            CSSPixelFraction result = 0;
            // For each flexible track, if the flexible track’s flex factor is greater than one, the result of dividing
            // the track’s base size by its flex factor; otherwise, the track’s base size.
            for (auto& track : tracks) {
                if (track.max_track_sizing_function.is_flexible_length()) {
                    if (track.max_track_sizing_function.flex_factor() > 1) {
                        result = max(result, track.base_size / CSSPixels::nearest_value_for(track.max_track_sizing_function.flex_factor()));
                    } else {
                        result = max(result, track.base_size / 1);
                    }
                }
            }
            // For each grid item that crosses a flexible track, the result of finding the size of an fr using all the
            // grid tracks that the item crosses and a space to fill of the item’s max-content contribution.
            for_each_item_contributing_to_track_sizing(dimension, [&](GridItem const& item) {
                Vector<GridTrack&> spanned_tracks;
                bool crosses_flexible_track = false;
                for_each_spanned_track_by_item(item, dimension, [&](GridTrack& track) {
                    spanned_tracks.append(track);
                    if (track.max_track_sizing_function.is_flexible_length())
                        crosses_flexible_track = true;
                });

                if (crosses_flexible_track)
                    result = max(result, find_the_size_of_an_fr(spanned_tracks, calculate_max_content_contribution(item, dimension)));
            });

            return result;
        }
    }();

    // For each flexible track, if the product of the used flex fraction and the track’s flex factor is greater than
    // the track’s base size, set its base size to that product.
    for (auto& track : tracks_and_gaps) {
        if (track.max_track_sizing_function.is_flexible_length()) {
            auto scaled_fraction = CSSPixels::nearest_value_for(track.max_track_sizing_function.flex_factor()) * flex_fraction;
            if (scaled_fraction > track.base_size) {
                track.base_size = scaled_fraction;
            }
        }
    }
}

void GridFormattingContext::stretch_auto_tracks(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#algo-stretch
    // 12.8. Stretch auto Tracks
    // This step expands tracks that have an auto max track sizing function by dividing any remaining positive,
    // definite free space equally amongst them. If the free space is indefinite, but the grid container has a
    // definite min-width/height, use that size to calculate the free space for this step instead.

    auto content_distribution_property_is_normal_or_stretch = false;
    if (dimension == GridDimension::Column) {
        auto const& justify_content = grid_container().computed_values().justify_content();
        content_distribution_property_is_normal_or_stretch = justify_content == CSS::JustifyContent::Normal || justify_content == CSS::JustifyContent::Stretch;
    } else {
        auto const& align_content = grid_container().computed_values().align_content();
        content_distribution_property_is_normal_or_stretch = align_content == CSS::AlignContent::Normal || align_content == CSS::AlignContent::Stretch;
    }

    if (!content_distribution_property_is_normal_or_stretch)
        return;

    auto& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
    auto& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

    auto count_of_auto_max_sizing_tracks = 0;
    for (auto& track : tracks_and_gaps) {
        if (track.max_track_sizing_function.is_auto(available_size))
            count_of_auto_max_sizing_tracks++;
    }

    if (count_of_auto_max_sizing_tracks == 0)
        return;

    CSSPixels remaining_space = get_free_space(*m_available_space, dimension).to_px_or_zero();
    auto remaining_space_to_distribute_per_track = remaining_space / count_of_auto_max_sizing_tracks;
    for (auto& track : tracks_and_gaps) {
        if (!track.max_track_sizing_function.is_auto(available_size))
            continue;
        track.base_size += remaining_space_to_distribute_per_track;
    }
}

void GridFormattingContext::run_track_sizing(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#algo-track-sizing
    // 12.3. Track Sizing Algorithm

    // 1. Initialize Track Sizes
    initialize_track_sizes(dimension);

    // 2. Resolve Intrinsic Track Sizes
    resolve_intrinsic_track_sizes(dimension);

    // 3. Maximize Tracks
    maximize_tracks(dimension);

    // 4. Expand Flexible Tracks
    if (has_flexible_tracks(dimension)) {
        expand_flexible_tracks(dimension);
    }

    // 5. Expand Stretched auto Tracks
    stretch_auto_tracks(dimension);

    // If calculating the layout of a grid item in this step depends on the available space in the block
    // axis, assume the available space that it would have if any row with a definite max track sizing
    // function had that size and all other rows were infinite. If both the grid container and all
    // tracks have definite sizes, also apply align-content to find the final effective size of any gaps
    // spanned by such items; otherwise ignore the effects of track alignment in this estimation.
}

void GridFormattingContext::build_grid_areas()
{
    auto const& grid_template_areas = grid_container().computed_values().grid_template_areas();
    if (grid_template_areas.is_empty())
        return;

    size_t max_column_line_index_of_area = 0;
    size_t max_row_line_index_of_area = 0;
    for (auto const& [name, area] : grid_template_areas.areas) {
        max_column_line_index_of_area = max(max_column_line_index_of_area, area.column_end);
        max_row_line_index_of_area = max(max_row_line_index_of_area, area.row_end);
    }

    if (max_column_line_index_of_area >= m_column_lines.size())
        m_column_lines.resize(max_column_line_index_of_area + 1);
    if (max_row_line_index_of_area >= m_row_lines.size())
        m_row_lines.resize(max_row_line_index_of_area + 1);

    // https://www.w3.org/TR/css-grid-2/#implicitly-assigned-line-name
    // 7.3.2. Implicitly-Assigned Line Names
    // The grid-template-areas property generates implicitly-assigned line names from the named grid areas in the
    // template. For each named grid area foo, four implicitly-assigned line names are created: two named foo-start,
    // naming the row-start and column-start lines of the named grid area, and two named foo-end, naming the row-end
    // and column-end lines of the named grid area.
    for (auto const& [name, area] : grid_template_areas.areas) {
        m_column_lines[area.column_start].append({ .name = MUST(String::formatted("{}-start", name)), .implicit = true });
        m_column_lines[area.column_end].append({ .name = MUST(String::formatted("{}-end", name)), .implicit = true });
        m_row_lines[area.row_start].append({ .name = MUST(String::formatted("{}-start", name)), .implicit = true });
        m_row_lines[area.row_end].append({ .name = MUST(String::formatted("{}-end", name)), .implicit = true });
    }
}

void GridFormattingContext::place_grid_items()
{
    auto grid_template_columns = grid_container().computed_values().grid_template_columns();
    auto grid_template_rows = grid_container().computed_values().grid_template_rows();
    auto column_tracks_count = m_column_lines.size() - 1;
    auto row_tracks_count = m_row_lines.size() - 1;

    // https://drafts.csswg.org/css-grid/#overview-placement
    // 2.2. Placing Items
    // The contents of the grid container are organized into individual grid items (analogous to
    // flex items), which are then assigned to predefined areas in the grid. They can be explicitly
    // placed using coordinates through the grid-placement properties or implicitly placed into
    // empty areas using auto-placement.
    HashMap<int, Vector<Box const&>> order_item_bucket;
    grid_container().for_each_child_of_type<Box>([&](Box& child_box) {
        if (can_skip_is_anonymous_text_run(child_box))
            return IterationDecision::Continue;

        if (child_box.is_out_of_flow(*this))
            return IterationDecision::Continue;

        child_box.set_grid_item(true);

        auto& order_bucket = order_item_bucket.ensure(child_box.computed_values().order());
        order_bucket.append(child_box);

        return IterationDecision::Continue;
    });

    m_occupation_grid = OccupationGrid(column_tracks_count, row_tracks_count);

    // https://drafts.csswg.org/css-grid/#auto-placement-algo
    // 8.5. Grid Item Placement Algorithm

    auto keys = order_item_bucket.keys();
    quick_sort(keys, [](auto& a, auto& b) { return a < b; });

    // FIXME: 0. Generate anonymous grid items

    // 1. Position anything that's not auto-positioned.
    for (auto key : keys) {
        auto& boxes_to_place = order_item_bucket.get(key).value();
        for (size_t i = 0; i < boxes_to_place.size(); i++) {
            auto const& child_box = boxes_to_place[i];
            auto const& computed_values = child_box.computed_values();
            if (is_auto_positioned_track(computed_values.grid_row_start(), computed_values.grid_row_end())
                || is_auto_positioned_track(computed_values.grid_column_start(), computed_values.grid_column_end()))
                continue;
            place_item_with_row_and_column_position(child_box);
            boxes_to_place.remove(i);
            i--;
        }
    }

    // 2. Process the items locked to a given row.
    for (auto key : keys) {
        auto& boxes_to_place = order_item_bucket.get(key).value();
        for (size_t i = 0; i < boxes_to_place.size(); i++) {
            auto const& child_box = boxes_to_place[i];
            auto const& computed_values = child_box.computed_values();
            if (is_auto_positioned_track(computed_values.grid_row_start(), computed_values.grid_row_end()))
                continue;
            place_item_with_row_position(child_box);
            boxes_to_place.remove(i);
            i--;
        }
    }

    // 3. Determine the columns in the implicit grid.
    // NOTE: "implicit grid" here is the same as the m_occupation_grid

    // 3.1. Start with the columns from the explicit grid.
    // NOTE: Done in step 1.

    // 3.2. Among all the items with a definite column position (explicitly positioned items, items
    // positioned in the previous step, and items not yet positioned but with a definite column) add
    // columns to the beginning and end of the implicit grid as necessary to accommodate those items.
    // NOTE: "Explicitly positioned items" and "items positioned in the previous step" done in step 1
    // and 2, respectively. Adding columns for "items not yet positioned but with a definite column"
    // will be done in step 4.

    // 3.3. If the largest column span among all the items without a definite column position is larger
    // than the width of the implicit grid, add columns to the end of the implicit grid to accommodate
    // that column span.
    for (auto key : keys) {
        auto& boxes_to_place = order_item_bucket.get(key).value();
        for (auto const& child_box : boxes_to_place) {
            auto column_span = resolve_grid_span(child_box, GridDimension::Column);
            auto column_start = 0;
            clamp_grid_area_to_subgrid(GridDimension::Column, column_start, column_span);
            auto max_column_index = static_cast<size_t>(m_occupation_grid.max_column_index());

            if (column_span - 1 > max_column_index)
                m_occupation_grid.set_max_column_index(column_span - 1);
        }
    }

    // 4. Position the remaining grid items.
    // For each grid item that hasn't been positioned by the previous steps, in order-modified document
    // order:
    auto auto_placement_cursor_column = 0;
    auto auto_placement_cursor_row = 0;
    auto const& auto_flow = grid_container().computed_values().grid_auto_flow();

    for (auto key : keys) {
        auto& boxes_to_place = order_item_bucket.get(key).value();
        for (size_t i = 0; i < boxes_to_place.size(); i++) {
            auto const& child_box = boxes_to_place[i];
            auto const& computed_values = child_box.computed_values();

            if (!is_auto_positioned_track(computed_values.grid_column_start(), computed_values.grid_column_end())) {
                // 4.1.1 / 4.2.1: Item with definite column position.
                auto column_start = resolve_grid_position(child_box, GridDimension::Column).start;

                if (auto_flow.dense) {
                    // Dense: reset row cursor to start of implicit grid.
                    auto_placement_cursor_row = m_occupation_grid.min_row_index();
                } else {
                    // Sparse: if column moved backward, bump row.
                    if (column_start < auto_placement_cursor_column)
                        auto_placement_cursor_row++;
                }
                auto_placement_cursor_column = column_start;

                place_item_with_column_position(child_box, auto_placement_cursor_row);
            } else {
                // 4.1.2 / 4.2.2: Item with auto position in both axes.
                if (auto_flow.dense) {
                    // Dense: reset cursor to start of implicit grid.
                    auto_placement_cursor_column = m_occupation_grid.min_column_index();
                    auto_placement_cursor_row = m_occupation_grid.min_row_index();
                }

                place_item_with_no_declared_position(child_box, auto_placement_cursor_column, auto_placement_cursor_row);
            }

            boxes_to_place.remove(i);
            i--;
        }
    }

    // NOTE: When final implicit grid sizes are known, we can offset their positions so leftmost grid track has 0 index.
    for (auto& item : m_grid_items) {
        item.row = item.row.value() - m_occupation_grid.min_row_index();
        item.column = item.column.value() - m_occupation_grid.min_column_index();
    }
}

void GridFormattingContext::determine_grid_container_height()
{
    CSSPixels total_y = 0;
    for (auto& grid_row : m_grid_rows_and_gaps)
        total_y += grid_row.base_size;
    m_automatic_content_height = total_y;
    m_row_track_alignment_grid_container_height = total_y;
    m_use_row_track_alignment_grid_container_height = false;
}

CSSPixels GridFormattingContext::resolve_used_grid_container_height_for_second_row_layout() const
{
    auto height = m_automatic_content_height;
    auto const& computed_values = grid_container().computed_values();

    if (!should_treat_max_height_as_none(grid_container(), m_available_space->height) && !computed_values.max_height().is_auto()) {
        auto max_height = calculate_inner_height(grid_container(), *m_available_space, computed_values.max_height());
        height = min(height, max_height);
    }

    if (!computed_values.min_height().is_auto())
        height = max(height, calculate_inner_height(grid_container(), *m_available_space, computed_values.min_height()));

    return height;
}

void GridFormattingContext::rerun_row_track_sizing_using_grid_container_height(CSSPixels grid_container_height)
{
    m_available_space->height = AvailableSize::make_definite(grid_container_height);
    initialize_gap_tracks(GridDimension::Row, m_available_space->height);

    resolve_items_box_metrics(GridDimension::Row);
    run_track_sizing(GridDimension::Row);
    resolve_items_box_metrics(GridDimension::Row);
    resolve_grid_item_sizes(GridDimension::Row);
}

Alignment GridFormattingContext::alignment_for_item(Box const& box, GridDimension dimension) const
{
    if (dimension == GridDimension::Column) {
        switch (box.computed_values().justify_self()) {
        case CSS::JustifySelf::Auto:
            return to_alignment(grid_container().computed_values().justify_items());
        case CSS::JustifySelf::End:
            return Alignment::End;
        case CSS::JustifySelf::Normal:
            return Alignment::Normal;
        case CSS::JustifySelf::SelfStart:
            return Alignment::SelfStart;
        case CSS::JustifySelf::SelfEnd:
            return Alignment::SelfEnd;
        case CSS::JustifySelf::FlexStart:
            return Alignment::Start;
        case CSS::JustifySelf::FlexEnd:
            return Alignment::End;
        case CSS::JustifySelf::Center:
            return Alignment::Center;
        case CSS::JustifySelf::Baseline:
            return Alignment::Baseline;
        case CSS::JustifySelf::Start:
            return Alignment::Start;
        case CSS::JustifySelf::Stretch:
            return Alignment::Stretch;
        case CSS::JustifySelf::Safe:
            return Alignment::Safe;
        case CSS::JustifySelf::Unsafe:
            return Alignment::Unsafe;
        case CSS::JustifySelf::Left:
            return Alignment::Start;
        case CSS::JustifySelf::Right:
            return Alignment::End;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    switch (box.computed_values().align_self()) {
    case CSS::AlignSelf::Auto:
        return to_alignment(grid_container().computed_values().align_items());
    case CSS::AlignSelf::End:
        return Alignment::End;
    case CSS::AlignSelf::Normal:
        return Alignment::Normal;
    case CSS::AlignSelf::SelfStart:
        return Alignment::SelfStart;
    case CSS::AlignSelf::SelfEnd:
        return Alignment::SelfEnd;
    case CSS::AlignSelf::FlexStart:
        return Alignment::Start;
    case CSS::AlignSelf::FlexEnd:
        return Alignment::End;
    case CSS::AlignSelf::Center:
        return Alignment::Center;
    case CSS::AlignSelf::Baseline:
        return Alignment::Baseline;
    case CSS::AlignSelf::Start:
        return Alignment::Start;
    case CSS::AlignSelf::Stretch:
        return Alignment::Stretch;
    case CSS::AlignSelf::Safe:
        return Alignment::Safe;
    case CSS::AlignSelf::Unsafe:
        return Alignment::Unsafe;
    default:
        VERIFY_NOT_REACHED();
    }
}

void GridFormattingContext::resolve_grid_item_sizes(GridDimension dimension)
{
    for (auto& item : m_grid_items) {
        CSSPixels containing_block_size = containing_block_size_for_item(item, dimension);
        Alignment alignment = alignment_for_item(item.box, dimension);

        auto const& preferred_size = item.preferred_size(dimension);

        struct ItemAlignment {
            CSSPixels margin_start;
            CSSPixels margin_end;
            CSSPixels size;
        };

        auto try_compute_size = [&item, containing_block_size, alignment, dimension](CSSPixels a_size, CSS::Size const& css_size, Optional<CSSPixels> containing_block_size_override = {}) -> ItemAlignment {
            auto alignment_containing_block_size = containing_block_size_override.value_or(containing_block_size);
            ItemAlignment result = {
                .margin_start = item.used_margin_start(dimension),
                .margin_end = item.used_margin_end(dimension),
                .size = a_size
            };

            // https://drafts.csswg.org/css-grid/#auto-margins
            // Auto margins in either axis absorb positive free space prior to alignment via the box alignment
            // properties, thereby disabling the effects of any self-alignment properties in that axis.
            // Overflowing grid items resolve their auto margins to zero and overflow as specified by their box
            // alignment properties.
            auto free_space_left_for_margins = alignment_containing_block_size - result.size - item.used_margin_box_start(dimension) - item.used_margin_box_end(dimension);
            bool start_is_auto = item.margin_start(dimension).is_auto();
            bool end_is_auto = item.margin_end(dimension).is_auto();
            auto absorbed_margin_space = max(CSSPixels(0), free_space_left_for_margins);
            if (start_is_auto && end_is_auto) {
                result.margin_start = absorbed_margin_space / 2;
                result.margin_end = absorbed_margin_space / 2;
            } else if (start_is_auto) {
                result.margin_start = absorbed_margin_space;
            } else if (end_is_auto) {
                result.margin_end = absorbed_margin_space;
            } else if (css_size.is_auto() && !item.box.is_replaced_box()) {
                result.size += free_space_left_for_margins;
            }

            // If auto margins absorbed positive free space, alignment properties have no effect in this dimension.
            if ((start_is_auto || end_is_auto) && free_space_left_for_margins > 0)
                return result;

            auto free_space_left_for_alignment = alignment_containing_block_size - a_size - item.used_margin_box_start(dimension) - item.used_margin_box_end(dimension);
            switch (alignment) {
            case Alignment::Normal:
            case Alignment::Stretch:
                break;
            case Alignment::Center:
                result.margin_start += free_space_left_for_alignment / 2;
                result.margin_end += free_space_left_for_alignment / 2;
                result.size = a_size;
                break;
            case Alignment::Baseline:
            case Alignment::Start:
                result.margin_end += free_space_left_for_alignment;
                result.size = a_size;
                break;
            case Alignment::End:
                result.margin_start += free_space_left_for_alignment;
                result.size = a_size;
                break;
            default:
                break;
            }

            return result;
        };

        AvailableSpace available_space {
            AvailableSize::make_definite(clamp_to_max_dimension_value(containing_block_size_for_item(item, GridDimension::Column))),
            AvailableSize::make_definite(clamp_to_max_dimension_value(containing_block_size_for_item(item, GridDimension::Row)))
        };

        auto calculate_inner_size = [this, &item, dimension, available_space](CSS::Size const& size) {
            if (dimension == GridDimension::Column)
                return calculate_inner_width(item.box, available_space.width, size);
            return calculate_inner_height(item.box, available_space, size);
        };

        auto tentative_size_for_replaced_element = [this, &item, dimension, available_space](CSS::Size const& size) {
            if (dimension == GridDimension::Column)
                return tentative_width_for_replaced_element(item.box, size, available_space);
            return tentative_height_for_replaced_element(item.box, size, available_space);
        };

        ItemAlignment used_alignment;
        auto hint = item.box.auto_content_box_size();
        bool has_replaced_size_hint_in_this_axis = false;
        if (dimension == GridDimension::Column) {
            has_replaced_size_hint_in_this_axis = hint.has_width() || (hint.has_height() && item.box.has_preferred_aspect_ratio());
        } else {
            has_replaced_size_hint_in_this_axis = hint.has_height() || (hint.has_width() && item.box.has_preferred_aspect_ratio());
        }

        if (item.box.is_replaced_box() && has_replaced_size_hint_in_this_axis) {
            auto tentative_size = tentative_size_for_replaced_element(preferred_size);
            used_alignment = try_compute_size(tentative_size, item.preferred_size(dimension));
        } else {
            // OPTIMIZATION: For auto-sized items with stretch/normal alignment and no auto margins, the item stretches
            //               to fill the containing block. We can compute this directly without the expensive
            //               calculate_fit_content_width/height calls that trigger intrinsic sizing.
            // NB: Final grid item alignment works with the resolved grid area size. Percentage preferred sizes
            //     must resolve against that definite area instead of being reclassified as auto from the outer
            //     grid container's own definiteness.
            bool can_stretch_directly = preferred_size.is_auto()
                && (alignment == Alignment::Stretch || alignment == Alignment::Normal)
                && !item.margin_start(dimension).is_auto()
                && !item.margin_end(dimension).is_auto();
            if (can_stretch_directly) {
                auto stretched_size = containing_block_size - item.used_margin_box_start(dimension) - item.used_margin_box_end(dimension);
                used_alignment = {
                    .margin_start = item.used_margin_start(dimension),
                    .margin_end = item.used_margin_end(dimension),
                    .size = stretched_size
                };
            } else if (dimension == GridDimension::Column && is<TableWrapper>(item.box)) {
                // CSS Grid lays out each grid item into its grid-area containing block before alignment. For
                // display:table, the anonymous table wrapper is the grid item, while table layout computes the inner
                // table's border-box width, so resolve the wrapper width with the same grid-area basis used later.
                auto table_wrapper_containing_block_width = non_cyclic_containing_block_width_for_table_wrapper(item, containing_block_size);
                auto table_wrapper_width = compute_table_box_width_inside_table_wrapper(
                    item.box, available_space, table_wrapper_containing_block_width, TableWrapperWidthMode::UseTableUsedWidthIfNotAuto);
                if (table_box_inside_table_wrapper(item).computed_values().width().is_auto())
                    table_wrapper_width = max(table_wrapper_width, calculate_min_content_width(item.box));
                used_alignment = try_compute_size(table_wrapper_width, CSS::Size::make_px(table_wrapper_width), table_wrapper_containing_block_width);
            } else if (preferred_size.is_auto() || preferred_size.is_fit_content()) {
                CSSPixels fit_content_size;
                if (dimension == GridDimension::Column) {
                    fit_content_size = calculate_fit_content_width(item.box, available_space);
                } else if (preferred_size.is_auto() && item.box.has_preferred_aspect_ratio() && *item.box.preferred_aspect_ratio() != 0 && item.used_values.has_definite_width()) {
                    // NB: When the item has a preferred aspect ratio and a definite width, resolve the
                    //     height through the aspect ratio instead of using fit-content sizing, which would
                    //     incorrectly use the available width (grid area width) instead of the item's width.
                    fit_content_size = item.used_values.content_width() / *item.box.preferred_aspect_ratio();
                } else {
                    fit_content_size = calculate_fit_content_height(item.box, available_space);
                }
                used_alignment = try_compute_size(fit_content_size, preferred_size);
            } else {
                auto size_px = calculate_inner_size(preferred_size);
                used_alignment = try_compute_size(size_px, preferred_size);
            }
        }

        bool should_treat_maximum_size_as_none = dimension == GridDimension::Column ? should_treat_max_width_as_none(item.box, available_space.width) : should_treat_max_height_as_none(item.box, available_space.height);
        if (!should_treat_maximum_size_as_none) {
            auto const& maximum_size = item.maximum_size(dimension);
            auto max_size_px = calculate_inner_size(maximum_size);
            auto max_width_alignment = try_compute_size(max_size_px, maximum_size);
            if (used_alignment.size > max_width_alignment.size) {
                used_alignment = max_width_alignment;
            }
        }

        auto const& minimum_size = item.minimum_size(dimension);
        if (!minimum_size.is_auto()) {
            auto min_size_alignment = try_compute_size(calculate_inner_size(minimum_size), minimum_size);
            if (used_alignment.size < min_size_alignment.size) {
                used_alignment = min_size_alignment;
            }
        }

        if (dimension == GridDimension::Column) {
            item.used_values.margin_left = used_alignment.margin_start;
            item.used_values.margin_right = used_alignment.margin_end;
            item.used_values.set_content_width(used_alignment.size);
        } else {
            item.used_values.margin_top = used_alignment.margin_start;
            item.used_values.margin_bottom = used_alignment.margin_end;
            item.used_values.set_content_height(used_alignment.size);
        }
    }
}

void GridFormattingContext::resolve_track_spacing(GridDimension dimension)
{
    auto is_column_dimension = dimension == GridDimension::Column;

    auto grid_container_size = grid_container_size_for_track_alignment(dimension);
    auto total_gap_space = grid_container_size;

    auto& grid_tracks = is_column_dimension ? m_grid_columns : m_grid_rows;
    for (auto& track : grid_tracks) {
        total_gap_space -= track.base_size;
    }
    total_gap_space = max(total_gap_space, 0);

    auto gap_track_count = is_column_dimension ? m_column_gap_tracks.size() : m_row_gap_tracks.size();
    if (gap_track_count == 0)
        return;

    CSSPixels space_between_tracks = 0;
    Alignment alignment;
    if (is_column_dimension) {
        alignment = to_alignment(grid_container().computed_values().justify_content());
    } else {
        alignment = to_alignment(grid_container().computed_values().align_content());
    }

    switch (alignment) {
    case Alignment::SpaceBetween:
        space_between_tracks = CSSPixels(total_gap_space / gap_track_count);
        break;
    case Alignment::SpaceAround:
        space_between_tracks = CSSPixels(total_gap_space / (gap_track_count + 1));
        break;
    case Alignment::SpaceEvenly:
        space_between_tracks = CSSPixels(total_gap_space / (gap_track_count + 2));
        break;
    case Alignment::Normal:
    case Alignment::Stretch:
    case Alignment::Start:
    case Alignment::End:
    case Alignment::Center:
    default:
        break;
    }

    auto const& computed_gap = is_column_dimension ? grid_container().computed_values().column_gap() : grid_container().computed_values().row_gap();
    auto minimum_gap_size = is_subgridded_axis(dimension)
        ? parent_gap_size_for_subgrid(dimension)
        : gap_to_px(computed_gap, grid_container_size);
    space_between_tracks = max(space_between_tracks, minimum_gap_size);

    auto& gap_tracks = is_column_dimension ? m_column_gap_tracks : m_row_gap_tracks;
    for (auto& track : gap_tracks) {
        track.base_size = space_between_tracks;
    }
}

void GridFormattingContext::save_grid_layout_data()
{
    if (!m_state.should_collect_devtools_layout_data())
        return;

    auto data = make<GridLayoutData>();
    data->direction = grid_container().computed_values().direction();
    data->is_subgrid = is_subgridded_axis(GridDimension::Column) || is_subgridded_axis(GridDimension::Row);
    data->writing_mode = grid_container().computed_values().writing_mode();

    auto track_type_for_index = [](size_t index, size_t explicit_start_line_index, size_t explicit_line_count) {
        if (explicit_line_count == 0)
            return GridTrackType::Implicit;

        auto explicit_track_count = explicit_line_count - 1;
        if (index >= explicit_start_line_index && index < explicit_start_line_index + explicit_track_count)
            return GridTrackType::Explicit;
        return GridTrackType::Implicit;
    };

    auto line_type_for_index = [](size_t index, size_t explicit_start_line_index, size_t explicit_line_count) {
        if (index >= explicit_start_line_index && index < explicit_start_line_index + explicit_line_count)
            return GridTrackType::Explicit;
        return GridTrackType::Implicit;
    };

    auto line_number_for_index = [](size_t index, size_t explicit_start_line_index) -> u32 {
        if (index < explicit_start_line_index)
            return 0;
        return static_cast<u32>(index - explicit_start_line_index + 1);
    };

    auto negative_line_number_for_index = [](size_t index, size_t explicit_start_line_index, size_t explicit_line_count) -> i32 {
        auto explicit_end_line_index = explicit_start_line_index + explicit_line_count;
        if (index >= explicit_end_line_index)
            return 0;
        return -static_cast<i32>(explicit_end_line_index - index);
    };

    auto track_state = [&](GridTrack const& track, GridDimension dimension, size_t track_index) {
        if (!track.is_auto_repeat)
            return GridTrackState::Static;

        if (track.is_auto_fit && !m_occupation_grid.is_occupied(dimension == GridDimension::Column ? track_index : 0, dimension == GridDimension::Row ? track_index : 0))
            return GridTrackState::Removed;

        return GridTrackState::Repeat;
    };

    auto axis_start_offset = [&](GridDimension dimension) -> CSSPixels {
        auto const& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
        auto grid_container_size = grid_container_size_for_track_alignment(dimension);

        CSSPixels sum_of_base_sizes_including_gaps = 0;
        for (auto const& track : tracks_and_gaps)
            sum_of_base_sizes_including_gaps += track.base_size;

        Alignment alignment;
        if (dimension == GridDimension::Column)
            alignment = to_alignment(grid_container().computed_values().justify_content());
        else
            alignment = to_alignment(grid_container().computed_values().align_content());

        if (alignment == Alignment::Center) {
            auto free_space = grid_container_size - sum_of_base_sizes_including_gaps;
            return static_cast<CSSPixels>(free_space / 2);
        }
        if (alignment == Alignment::SpaceAround || alignment == Alignment::SpaceEvenly) {
            auto free_space = grid_container_size - sum_of_base_sizes_including_gaps;
            return static_cast<CSSPixels>(max(free_space, CSSPixels { 0 }) / 2);
        }
        if (alignment == Alignment::End) {
            auto free_space = grid_container_size - sum_of_base_sizes_including_gaps;
            return free_space;
        }
        return CSSPixels { 0 };
    };

    auto serialize_dimension = [&](GridDimension dimension) {
        auto const& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
        auto const& gap_tracks = dimension == GridDimension::Column ? m_column_gap_tracks : m_row_gap_tracks;
        auto const& lines = dimension == GridDimension::Column ? m_column_lines : m_row_lines;
        auto explicit_start_line_index = dimension == GridDimension::Column ? m_explicit_columns_start_line_index : m_explicit_rows_start_line_index;
        auto explicit_line_count = dimension == GridDimension::Column ? m_explicit_columns_line_count : m_explicit_rows_line_count;

        GridLayoutDimension result;
        auto line_start = axis_start_offset(dimension);

        for (size_t i = 0; i < lines.size(); ++i) {
            CSSPixels line_breadth = i == 0 || i - 1 >= gap_tracks.size() ? CSSPixels { 0 } : gap_tracks[i - 1].base_size;

            GridLayoutLine line;
            line.start = line_start;
            line.breadth = line_breadth;
            line.type = line_type_for_index(i, explicit_start_line_index, explicit_line_count);
            line.number = line_number_for_index(i, explicit_start_line_index);
            line.negative_number = negative_line_number_for_index(i, explicit_start_line_index, explicit_line_count);

            for (auto const& line_name : lines[i])
                line.names.append(line_name.name.to_string());

            result.lines.append(move(line));

            if (i < tracks.size()) {
                GridLayoutTrack track;
                track.start = line_start + line_breadth;
                track.breadth = tracks[i].base_size;
                track.type = track_type_for_index(i, explicit_start_line_index, explicit_line_count);
                track.state = track_state(tracks[i], dimension, i);
                result.tracks.append(track);

                line_start = track.start + track.breadth;
            }
        }

        return result;
    };

    GridLayoutFragment fragment;
    fragment.columns = serialize_dimension(GridDimension::Column);
    fragment.rows = serialize_dimension(GridDimension::Row);

    auto const& grid_template_areas = grid_container().computed_values().grid_template_areas();
    for (auto const& [name, area] : grid_template_areas.areas) {
        auto row_start_index = m_explicit_rows_start_line_index + area.row_start;
        auto row_end_index = m_explicit_rows_start_line_index + area.row_end;
        auto column_start_index = m_explicit_columns_start_line_index + area.column_start;
        auto column_end_index = m_explicit_columns_start_line_index + area.column_end;

        fragment.areas.append(GridLayoutArea {
            .name = name,
            .type = GridTrackType::Explicit,
            .row_start = line_number_for_index(row_start_index, m_explicit_rows_start_line_index),
            .row_end = line_number_for_index(row_end_index, m_explicit_rows_start_line_index),
            .column_start = line_number_for_index(column_start_index, m_explicit_columns_start_line_index),
            .column_end = line_number_for_index(column_end_index, m_explicit_columns_start_line_index),
        });
    }

    data->fragments.append(move(fragment));
    m_grid_container_used_values.set_grid_layout_data(move(data));
}

CSSPixels GridFormattingContext::grid_container_size_for_track_alignment(GridDimension dimension) const
{
    if (dimension == GridDimension::Column)
        return m_grid_container_used_values.content_width();
    if (m_use_row_track_alignment_grid_container_height) {
        if (!grid_container().computed_values().min_height().is_auto())
            return max(m_row_track_alignment_grid_container_height, m_grid_container_used_values.content_height());
        return m_row_track_alignment_grid_container_height;
    }
    if (m_grid_container_used_values.has_definite_height())
        return m_grid_container_used_values.content_height();
    return m_row_track_alignment_grid_container_height;
}

void GridFormattingContext::resolve_items_box_metrics(GridDimension dimension)
{
    for (auto& item : m_grid_items) {
        auto& computed_values = item.box.computed_values();

        CSSPixels containing_block_width = containing_block_size_for_item(item, GridDimension::Column);
        if (dimension == GridDimension::Column) {
            item.used_values.padding_right = computed_values.padding().right().to_px_or_zero(grid_container(), containing_block_width);
            item.used_values.padding_left = computed_values.padding().left().to_px_or_zero(grid_container(), containing_block_width);

            item.used_values.margin_right = computed_values.margin().right().to_px_or_zero(grid_container(), containing_block_width);
            item.used_values.margin_left = computed_values.margin().left().to_px_or_zero(grid_container(), containing_block_width);

            item.used_values.border_right = computed_values.border_right().width;
            item.used_values.border_left = computed_values.border_left().width;

            apply_subgrid_gap_extra_margins(item, dimension, m_available_space->width);
        } else {
            item.used_values.padding_top = computed_values.padding().top().to_px_or_zero(grid_container(), containing_block_width);
            item.used_values.padding_bottom = computed_values.padding().bottom().to_px_or_zero(grid_container(), containing_block_width);

            item.used_values.margin_top = computed_values.margin().top().to_px_or_zero(grid_container(), containing_block_width);
            item.used_values.margin_bottom = computed_values.margin().bottom().to_px_or_zero(grid_container(), containing_block_width);

            item.used_values.border_top = computed_values.border_top().width;
            item.used_values.border_bottom = computed_values.border_bottom().width;

            apply_subgrid_gap_extra_margins(item, dimension, m_available_space->height);
        }
    }
}

void GridFormattingContext::collapse_auto_fit_tracks_if_needed(GridDimension dimension)
{
    // https://www.w3.org/TR/css-grid-2/#auto-repeat
    // The auto-fit keyword behaves the same as auto-fill, except that after grid item placement any
    // empty repeated tracks are collapsed. An empty track is one with no in-flow grid items placed into
    // or spanning across it. (This can result in all tracks being collapsed, if they’re all empty.)
    auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    for (size_t track_index = 0; track_index < tracks.size(); track_index++) {
        if (!tracks[track_index].is_auto_fit)
            continue;
        if (m_occupation_grid.is_occupied(dimension == GridDimension::Column ? track_index : 0, dimension == GridDimension::Row ? track_index : 0))
            continue;

        // NOTE: A collapsed track is treated as having a fixed track sizing function of 0px
        tracks[track_index].min_track_sizing_function = CSS::GridSize(CSS::LengthStyleValue::create(CSS::Length::make_px(0)));
        tracks[track_index].max_track_sizing_function = CSS::GridSize(CSS::LengthStyleValue::create(CSS::Length::make_px(0)));
    }
}

CSSPixelRect GridFormattingContext::get_grid_area_rect(GridItem const& grid_item) const
{
    CSSPixelRect area_rect;

    auto place_into_track = [&](GridDimension dimension) {
        auto const& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;

        auto resolved_span = grid_item.span(dimension) * 2;
        auto gap_adjusted_position = grid_item.gap_adjusted_position(dimension);

        int start = gap_adjusted_position;
        int end = start + resolved_span;
        VERIFY(start <= end);

        auto grid_container_size = grid_container_size_for_track_alignment(dimension);

        CSSPixels sum_of_base_sizes_including_gaps = 0;
        for (auto const& track : tracks_and_gaps) {
            sum_of_base_sizes_including_gaps += track.base_size;
        }

        Alignment alignment;
        if (dimension == GridDimension::Column) {
            alignment = to_alignment(grid_container().computed_values().justify_content());
        } else {
            alignment = to_alignment(grid_container().computed_values().align_content());
        }
        CSSPixels start_offset = 0;
        CSSPixels end_offset = 0;
        if (alignment == Alignment::Center) {
            // CSS Align's automatic overflow alignment is unsafe for grid content
            // alignment, so preserve negative free space here.
            auto free_space = grid_container_size - sum_of_base_sizes_including_gaps;
            start_offset = free_space / 2;
            end_offset = free_space / 2;
        } else if (alignment == Alignment::SpaceAround || alignment == Alignment::SpaceEvenly) {
            auto free_space = grid_container_size - sum_of_base_sizes_including_gaps;
            free_space = max(free_space, 0);
            start_offset = free_space / 2;
            end_offset = free_space / 2;
        } else if (alignment == Alignment::End) {
            auto free_space = grid_container_size - sum_of_base_sizes_including_gaps;
            start_offset = free_space;
            end_offset = free_space;
        }

        for (int i = 0; i < min(start, tracks_and_gaps.size()); i++)
            start_offset += tracks_and_gaps[i].base_size;
        for (int i = 0; i < min(end, tracks_and_gaps.size()); i++) {
            end_offset += tracks_and_gaps[i].base_size;
        }

        if (dimension == GridDimension::Column) {
            area_rect.set_x(start_offset);
            area_rect.set_width(end_offset - start_offset);
        } else {
            area_rect.set_y(start_offset);
            area_rect.set_height(end_offset - start_offset);
        }
    };

    auto place_into_track_formed_by_last_line_and_grid_container_padding_edge = [&](GridDimension dimension) {
        VERIFY(grid_item.box.is_absolutely_positioned());
        auto const& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
        CSSPixels offset = 0;
        for (auto const& row_track : tracks_and_gaps) {
            offset += row_track.base_size;
        }
        CSSPixels size = dimension == GridDimension::Column ? m_grid_container_used_values.padding_right : m_grid_container_used_values.padding_bottom;
        if (dimension == GridDimension::Column) {
            area_rect.set_x(offset);
            area_rect.set_width(size);
        } else {
            area_rect.set_y(offset);
            area_rect.set_height(size);
        }
    };

    if (grid_item.row.has_value()) {
        if (grid_item.row == (int)m_grid_rows.size()) {
            place_into_track_formed_by_last_line_and_grid_container_padding_edge(GridDimension::Row);
        } else {
            place_into_track(GridDimension::Row);
        }
    } else {
        // https://www.w3.org/TR/css-grid-2/#abspos-items
        // Instead of auto-placement, an auto value for a grid-placement property contributes a special line to the placement whose position
        // is that of the corresponding padding edge of the grid container (the padding edge of the scrollable area, if the grid container
        // overflows). These lines become the first and last lines (0th and -0th) of the augmented grid used for positioning absolutely-positioned items.
        CSSPixels height = 0;
        if (m_available_space->height.is_definite()) {
            height = m_available_space->height.to_px_or_zero();
        } else {
            for (auto const& row_track : m_grid_rows_and_gaps)
                height += row_track.base_size;
        }
        height += m_grid_container_used_values.padding_top + m_grid_container_used_values.padding_bottom;

        area_rect.set_height(height);
        area_rect.set_y(-m_grid_container_used_values.padding_top);
    }

    if (grid_item.column.has_value()) {
        if (grid_item.column == (int)m_grid_columns.size()) {
            place_into_track_formed_by_last_line_and_grid_container_padding_edge(GridDimension::Column);
        } else {
            place_into_track(GridDimension::Column);
        }
    } else {
        CSSPixels width = 0;
        if (m_available_space->width.is_definite()) {
            width = m_available_space->width.to_px_or_zero();
        } else {
            for (auto const& col_track : m_grid_columns_and_gaps)
                width += col_track.base_size;
        }
        width += m_grid_container_used_values.padding_left + m_grid_container_used_values.padding_right;

        area_rect.set_width(width);
        area_rect.set_x(-m_grid_container_used_values.padding_left);
    }

    return area_rect;
}

void GridFormattingContext::run(AvailableSpace const& available_space)
{
    // OPTIMIZATION: If we're in intrinsic sizing layout, but the grid container is not the
    //               box being measured, we can skip everything here.
    //               The parent formatting context has already figured out our size anyway.
    if (m_layout_mode == LayoutMode::IntrinsicSizing
        && !available_space.width.is_intrinsic_sizing_constraint()
        && !available_space.height.is_intrinsic_sizing_constraint()) {
        return;
    }

    FORMATTING_CONTEXT_TRACE();
    m_available_space = available_space;

    init_grid_lines(GridDimension::Column);
    init_grid_lines(GridDimension::Row);

    build_grid_areas();

    // NOTE: We store explicit grid sizes to later use in determining the position of items with negative index.
    m_explicit_columns_line_count = m_column_lines.size();
    m_explicit_rows_line_count = m_row_lines.size();

    place_grid_items();

    initialize_grid_tracks_for_columns_and_rows();

    initialize_gap_tracks(available_space);

    collapse_auto_fit_tracks_if_needed(GridDimension::Column);
    collapse_auto_fit_tracks_if_needed(GridDimension::Row);

    for (auto& item : m_grid_items) {
        auto& computed_values = item.box.computed_values();

        // NOTE: As the containing blocks of grid items are created by implicit grid areas that are not present in the
        // layout tree, the initial value of has_definite_width/height computed by LayoutState::UsedValues::set_node
        // will be incorrect for anything other (auto, percentage, calculated) than fixed lengths.
        // Therefor, it becomes necessary to reset this value to indefinite.
        // TODO: Handle this in LayoutState::UsedValues::set_node
        if (!computed_values.width().is_length())
            item.used_values.set_indefinite_content_width();
        if (!computed_values.height().is_length())
            item.used_values.set_indefinite_content_height();
    }

    // Do the first pass of resolving grid items box metrics to compute values that are independent of a track width
    resolve_items_box_metrics(GridDimension::Column);

    run_track_sizing(GridDimension::Column);

    // Do the second pass of resolving box metrics to compute values that depend on a track width
    resolve_items_box_metrics(GridDimension::Column);

    // Once the sizes of column tracks, which determine the widths of the grid areas forming the containing blocks
    // for grid items, ara calculated, it becomes possible to determine the final widths of the grid items.
    resolve_grid_item_sizes(GridDimension::Column);

    // Do the first pass of resolving grid items box metrics to compute values that are independent of a track height
    resolve_items_box_metrics(GridDimension::Row);

    run_track_sizing(GridDimension::Row);

    // Do the second pass of resolving box metrics to compute values that depend on a track height
    resolve_items_box_metrics(GridDimension::Row);

    resolve_grid_item_sizes(GridDimension::Row);

    determine_grid_container_height();

    auto intrinsic_grid_container_height = m_automatic_content_height;
    if (m_layout_mode == LayoutMode::Normal && m_available_space->height.is_indefinite()) {
        auto resolved_grid_container_height = resolve_used_grid_container_height_for_second_row_layout();
        rerun_row_track_sizing_using_grid_container_height(resolved_grid_container_height);
        m_row_track_alignment_grid_container_height = resolved_grid_container_height;
        m_use_row_track_alignment_grid_container_height = true;
        m_automatic_content_height = intrinsic_grid_container_height;
    } else if (m_layout_mode == LayoutMode::Normal && m_available_space->height.is_definite() && should_treat_height_as_auto(grid_container(), available_space)) {
        m_row_track_alignment_grid_container_height = m_available_space->height.to_px_or_zero();
        m_use_row_track_alignment_grid_container_height = true;
    }

    resolve_track_spacing(GridDimension::Column);

    resolve_track_spacing(GridDimension::Row);

    if (m_layout_mode == LayoutMode::IntrinsicSizing) {
        determine_intrinsic_size_of_grid_container(available_space);
        return;
    }

    for (auto& grid_item : m_grid_items) {
        auto const grid_area_rect = get_grid_area_rect(grid_item);
        if (is<TableWrapper>(grid_item.box)) {
            // Track spacing can expand the final grid area after the earlier width pass. Recompute the wrapper width
            // against that final area and store it so the real table layout resolves percentages against the grid area.
            resolve_table_wrapper_grid_item_width(grid_item, grid_area_rect.width());
            auto grid_area_size = grid_area_rect.size();
            grid_area_size.set_width(non_cyclic_containing_block_width_for_table_wrapper(grid_item, grid_area_size.width()));
            grid_item.used_values.set_grid_area_size(grid_area_size);
        }
        CSSPixelPoint margin_offset = { grid_item.used_values.margin_box_left(), grid_item.used_values.margin_box_top() };
        grid_item.used_values.set_content_offset(grid_area_rect.top_left() + margin_offset);
        compute_inset(grid_item.box, grid_area_rect.size());

        auto available_space_for_children = AvailableSpace(AvailableSize::make_definite(grid_item.used_values.content_width()), AvailableSize::make_definite(grid_item.used_values.content_height()));
        grid_item.used_values.set_has_definite_width(true);
        grid_item.used_values.set_has_definite_height(true);
        if (auto independent_formatting_context = layout_inside(grid_item.box, LayoutMode::Normal, available_space_for_children))
            independent_formatting_context->parent_context_did_dimension_child_root_box();
    }

    auto serialize_standalone_axis = [](auto const& tracks, auto const& lines) {
        CSS::GridTrackSizeList result;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto const& names = lines[i];
            if (!names.is_empty()) {
                CSS::GridLineNames grid_line_names;
                for (auto const& line_name : names) {
                    if (!line_name.implicit)
                        grid_line_names.append(line_name.name);
                }
                if (!grid_line_names.is_empty())
                    result.append(CSS::GridLineNames { move(grid_line_names) });
            }

            if (i < tracks.size()) {
                auto const& track = tracks[i];
                result.append(CSS::ExplicitGridTrack { CSS::GridSize { CSS::LengthStyleValue::create(CSS::Length::make_px(track.base_size)) } });
            }
        }
        return result;
    };

    auto serialize_subgridded_axis = [](auto const& lines) {
        // https://drafts.csswg.org/css-grid-2/#resolved-track-list-subgrid
        // When an element generates a grid container box that is a subgrid, the resolved value of the
        // grid-template-rows and grid-template-columns properties represents the used number of columns,
        // serialized as the subgrid keyword followed by a list representing each of its lines as a line
        // name set of all the line's names explicitly defined on the subgrid, without using repeat().
        CSS::GridTrackSizeList result = CSS::GridTrackSizeList::make_subgrid();
        for (auto const& names : lines) {
            CSS::GridLineNames grid_line_names;
            for (auto const& line_name : names) {
                if (!line_name.implicit && !line_name.adopted_from_parent_grid)
                    grid_line_names.append(line_name.name);
            }
            result.append(move(grid_line_names));
        }
        return result;
    };

    auto grid_track_columns = is_subgridded_axis(GridDimension::Column)
        ? serialize_subgridded_axis(m_column_lines)
        : serialize_standalone_axis(m_grid_columns, m_column_lines);
    auto grid_track_rows = is_subgridded_axis(GridDimension::Row)
        ? serialize_subgridded_axis(m_row_lines)
        : serialize_standalone_axis(m_grid_rows, m_row_lines);

    // getComputedStyle() needs to return the resolved values of grid-template-columns and grid-template-rows
    // so they need to be saved in the state, and then assigned to paintables in LayoutState::commit()
    m_grid_container_used_values.set_grid_template_columns(CSS::GridTrackSizeListStyleValue::create(move(grid_track_columns)));
    m_grid_container_used_values.set_grid_template_rows(CSS::GridTrackSizeListStyleValue::create(move(grid_track_rows)));
    save_grid_layout_data();
}

// https://www.w3.org/TR/css-grid-2/#abspos-items
AbsposContainingBlockInfo GridFormattingContext::resolve_abspos_containing_block_info(Box const& box)
{
    auto& abspos_box_state = m_state.get_mutable(box);
    auto containing_block_info = FormattingContext::resolve_abspos_containing_block_info(box);

    auto grid_area_rect = [&] -> CSSPixelRect {
        auto const& computed_values = box.computed_values();
        GridItem item { box, abspos_box_state, {}, {}, {}, {} };
        auto row_placement_position = resolve_grid_position(box, GridDimension::Row);
        auto column_placement_position = resolve_grid_position(box, GridDimension::Column);
        if (!is_auto_positioned_track(computed_values.grid_row_start(), computed_values.grid_row_end())) {
            item.row = row_placement_position.start;
            item.row_span = row_placement_position.span;
        }
        if (!is_auto_positioned_track(computed_values.grid_column_start(), computed_values.grid_column_end())) {
            item.column = column_placement_position.start;
            item.column_span = column_placement_position.span;
        }

        auto rect = get_grid_area_rect(item);

        auto explicit_line_position = [&](GridDimension dimension, int line_index) {
            auto const& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
            auto const& grid_container_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

            CSSPixels offset = 0;
            auto sum_of_base_sizes_including_gaps = CSSPixels(0);
            for (auto const& track : tracks_and_gaps)
                sum_of_base_sizes_including_gaps += track.base_size;

            auto alignment = dimension == GridDimension::Column
                ? to_alignment(grid_container().computed_values().justify_content())
                : to_alignment(grid_container().computed_values().align_content());
            if (alignment == Alignment::Center) {
                // CSS Align's automatic overflow alignment is unsafe for grid content
                // alignment, so preserve negative free space here.
                auto free_space = grid_container_size.to_px_or_zero() - sum_of_base_sizes_including_gaps;
                offset = free_space / 2;
            } else if (alignment == Alignment::SpaceAround || alignment == Alignment::SpaceEvenly) {
                auto free_space = grid_container_size.to_px_or_zero() - sum_of_base_sizes_including_gaps;
                offset = max(free_space, 0) / 2;
            } else if (alignment == Alignment::End) {
                auto free_space = grid_container_size.to_px_or_zero() - sum_of_base_sizes_including_gaps;
                offset = free_space;
            }

            for (int i = 0; i < min(line_index * 2, tracks_and_gaps.size()); ++i)
                offset += tracks_and_gaps[i].base_size;
            return offset;
        };

        auto augmented_grid_padding_edge = [&](GridDimension dimension, bool is_start_line) {
            auto const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;
            auto padding_start = dimension == GridDimension::Column ? m_grid_container_used_values.padding_left : m_grid_container_used_values.padding_top;
            auto padding_end = dimension == GridDimension::Column ? m_grid_container_used_values.padding_right : m_grid_container_used_values.padding_bottom;
            auto const& tracks_and_gaps = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;

            if (is_start_line)
                return -padding_start;

            CSSPixels offset = available_size.is_definite() ? available_size.to_px_or_zero() : 0;
            if (!available_size.is_definite()) {
                for (auto const& track : tracks_and_gaps)
                    offset += track.base_size;
            }
            return offset + padding_end;
        };

        auto override_mixed_auto_axis = [&](GridDimension dimension, CSS::GridTrackPlacement const& placement_start, CSS::GridTrackPlacement const& placement_end, PlacementPosition const& placement_position) {
            auto const start_is_augmented = placement_start.is_auto_positioned() && placement_end.is_positioned() && !placement_end.is_span();
            auto const end_is_augmented = placement_end.is_auto_positioned() && placement_start.is_positioned() && !placement_start.is_span();
            if (!start_is_augmented && !end_is_augmented)
                return;

            auto start_offset = start_is_augmented
                ? augmented_grid_padding_edge(dimension, true)
                : explicit_line_position(dimension, placement_position.start);
            auto end_offset = end_is_augmented
                ? augmented_grid_padding_edge(dimension, false)
                : explicit_line_position(dimension, placement_position.end);

            if (dimension == GridDimension::Column) {
                rect.set_x(start_offset);
                rect.set_width(end_offset - start_offset);
            } else {
                rect.set_y(start_offset);
                rect.set_height(end_offset - start_offset);
            }
        };

        override_mixed_auto_axis(GridDimension::Row, computed_values.grid_row_start(), computed_values.grid_row_end(), row_placement_position);
        override_mixed_auto_axis(GridDimension::Column, computed_values.grid_column_start(), computed_values.grid_column_end(), column_placement_position);

        return rect;
    }();

    containing_block_info.rect = grid_area_rect;
    containing_block_info.horizontal_alignment = alignment_for_item(box, GridDimension::Column);
    containing_block_info.vertical_alignment = alignment_for_item(box, GridDimension::Row);

    // An absolutely positioned child of a grid container gets its static
    // position from grid placement and alignment, but deeper descendants
    // inside grid items still use the generic static-position behavior from
    // their in-flow ancestor.
    if (box.static_position_containing_block() == &grid_container()) {
        containing_block_info.horizontal_axis_mode = AbsposAxisMode::InsetFromRect;
        containing_block_info.vertical_axis_mode = AbsposAxisMode::InsetFromRect;
    }

    return containing_block_info;
}

void GridFormattingContext::parent_context_did_dimension_child_root_box()
{
    if (m_layout_mode != LayoutMode::Normal)
        return;

    grid_container().for_each_child_of_type<Box>([&](Layout::Box& box) {
        if (box.is_absolutely_positioned()) {
            m_state.get_mutable(box).set_static_position_rect(calculate_static_position_rect(box));
        }
        return IterationDecision::Continue;
    });

    layout_absolutely_positioned_children();
}

void GridFormattingContext::determine_intrinsic_size_of_grid_container(AvailableSpace const& available_space)
{
    // https://www.w3.org/TR/css-grid-1/#intrinsic-sizes
    // The max-content size (min-content size) of a grid container is the sum of the grid container’s track sizes
    // (including gutters) in the appropriate axis, when the grid is sized under a max-content constraint (min-content constraint).

    if (available_space.height.is_intrinsic_sizing_constraint()) {
        CSSPixels grid_container_height = 0;
        for (auto& track : m_grid_rows_and_gaps) {
            grid_container_height += track.base_size;
        }
        m_grid_container_used_values.set_content_height(grid_container_height);
    }

    if (available_space.width.is_intrinsic_sizing_constraint()) {
        CSSPixels grid_container_width = 0;
        for (auto& track : m_grid_columns_and_gaps) {
            grid_container_width += track.base_size;
        }
        m_grid_container_used_values.set_content_width(grid_container_width);
    }
}

CSSPixels GridFormattingContext::automatic_content_width() const
{
    return m_grid_container_used_values.content_width();
}

CSSPixels GridFormattingContext::automatic_content_height() const
{
    return m_automatic_content_height;
}

bool GridFormattingContext::is_auto_positioned_track(CSS::GridTrackPlacement const& grid_track_start, CSS::GridTrackPlacement const& grid_track_end) const
{
    return grid_track_start.is_auto_positioned() && grid_track_end.is_auto_positioned();
}

AvailableSize GridFormattingContext::get_free_space(AvailableSpace const& available_space, GridDimension dimension) const
{
    // https://www.w3.org/TR/css-grid-2/#algo-terms
    // free space: Equal to the available grid space minus the sum of the base sizes of all the grid
    // tracks (including gutters), floored at zero. If available grid space is indefinite, the free
    // space is indefinite as well.
    auto& available_size = dimension == GridDimension::Column ? available_space.width : available_space.height;
    auto& tracks = dimension == GridDimension::Column ? m_grid_columns_and_gaps : m_grid_rows_and_gaps;
    if (available_size.is_definite()) {
        CSSPixels sum_base_sizes = 0;
        for (auto& track : tracks)
            sum_base_sizes += track.base_size;
        return AvailableSize::make_definite(max(CSSPixels(0), available_size.to_px_or_zero() - sum_base_sizes));
    }

    return available_size;
}

Optional<int> GridFormattingContext::get_nth_line_index_by_line_name(GridDimension dimension, String const& line_name, int nth_line)
{
    auto const& lines = dimension == GridDimension::Column ? m_column_lines : m_row_lines;
    size_t line_index = nth_line < 0 ? lines.size() + nth_line : nth_line - 1;
    // FIXME: If not enough lines with the name exist, all implicit grid lines on the side
    // of the explicit grid corresponding to the search direction are assumed to have that name for the purpose of counting this span.
    // Source: https://drafts.csswg.org/css-grid/#line-placement
    for (size_t actual_line_index = 0; actual_line_index < lines.size(); actual_line_index++) {
        for (auto const& line : lines[actual_line_index]) {
            if (line.name == line_name) {
                // https://drafts.csswg.org/css-grid/#line-placement
                // Contributes the nth grid line to the grid item’s placement.
                if (line_index == 0)
                    return static_cast<int>(actual_line_index);
                else
                    line_index--;
            }
        }
    }
    return {};
}

void GridFormattingContext::init_grid_lines(GridDimension dimension)
{
    auto const& grid_computed_values = grid_container().computed_values();
    auto const& lines_definition = dimension == GridDimension::Column ? grid_computed_values.grid_template_columns() : grid_computed_values.grid_template_rows();
    auto& lines = dimension == GridDimension::Column ? m_column_lines : m_row_lines;

    if (is_subgridded_axis(dimension)) {
        // https://drafts.csswg.org/css-grid-2/#subgrid-span
        // The number of explicit tracks in the subgrid in a subgridded dimension always corresponds
        // to the number of grid tracks that it spans in its parent grid.
        lines.resize(subgrid_track_count(dimension) + 1);

        // https://drafts.csswg.org/css-grid-2/#subgrid-line-name-inheritance
        // Since subgrids can be placed before their contents are placed, the subgridded lines
        // automatically receive the explicitly-assigned line names specified on the corresponding
        // lines of the parent grid. These names are in addition to any line names specified locally
        // on the subgrid.
        if (auto const* parent_grid = parent_grid_formatting_context()) {
            if (auto const* item = parent_grid_item()) {
                auto const& parent_lines = dimension == GridDimension::Column ? parent_grid->m_column_lines : parent_grid->m_row_lines;
                auto parent_start = item->raw_position(dimension);
                for (size_t i = 0; i < lines.size(); ++i) {
                    auto parent_line_index = parent_start + static_cast<int>(i);
                    if (parent_line_index < 0 || static_cast<size_t>(parent_line_index) >= parent_lines.size())
                        continue;
                    for (auto const& parent_line_name : parent_lines[parent_line_index]) {
                        if (!parent_line_name.implicit)
                            lines[i].append({ .name = parent_line_name.name, .adopted_from_parent_grid = true });
                    }
                }

                // https://drafts.csswg.org/css-grid-2/#subgrid-area-inheritance
                // When a subgrid overlaps a named grid area in its parent that was created by a
                // grid-template-areas property declaration, implicitly-assigned line names are assigned to represent
                // the parent's named grid area within the subgrid.
                //
                // Note: If a named grid area only partially overlaps the subgrid, its implicitly-assigned line names
                // will be assigned to the first and/or last line of the subgrid such that a named grid area exists
                // representing that partially overlapped area of the subgrid; thus the line name assignments of the
                // subgrid might not always correspond exactly to the line name assignments of the parent grid.
                struct ParentGridArea {
                    Optional<size_t> row_start;
                    Optional<size_t> row_end;
                    Optional<size_t> column_start;
                    Optional<size_t> column_end;
                };

                struct AreaLineName {
                    String area_name;
                    bool is_start { false };
                };

                auto parse_implicit_area_line_name = [](CSS::GridLineName const& line_name) -> Optional<AreaLineName> {
                    if (!line_name.implicit)
                        return {};

                    auto line_name_view = line_name.name.bytes_as_string_view();
                    auto constexpr start_suffix = "-start"sv;
                    auto constexpr end_suffix = "-end"sv;
                    if (line_name_view.ends_with(start_suffix)) {
                        return AreaLineName {
                            .area_name = MUST(String::from_utf8(line_name_view.substring_view(0, line_name_view.length() - start_suffix.length()))),
                            .is_start = true,
                        };
                    }
                    if (line_name_view.ends_with(end_suffix)) {
                        return AreaLineName {
                            .area_name = MUST(String::from_utf8(line_name_view.substring_view(0, line_name_view.length() - end_suffix.length()))),
                            .is_start = false,
                        };
                    }
                    return {};
                };

                HashMap<String, ParentGridArea> parent_grid_areas;
                auto collect_implicit_area_line_names = [&](GridDimension line_dimension) {
                    auto const& parent_lines_for_dimension = line_dimension == GridDimension::Column ? parent_grid->m_column_lines : parent_grid->m_row_lines;
                    for (size_t line_index = 0; line_index < parent_lines_for_dimension.size(); ++line_index) {
                        for (auto const& line_name : parent_lines_for_dimension[line_index]) {
                            auto area_line_name = parse_implicit_area_line_name(line_name);
                            if (!area_line_name.has_value())
                                continue;

                            auto& area = parent_grid_areas.ensure(area_line_name->area_name, [] { return ParentGridArea {}; });
                            auto& line = [&]() -> Optional<size_t>& {
                                if (line_dimension == GridDimension::Column)
                                    return area_line_name->is_start ? area.column_start : area.column_end;
                                return area_line_name->is_start ? area.row_start : area.row_end;
                            }();

                            if (!line.has_value()) {
                                line = line_index;
                            } else if (area_line_name->is_start) {
                                line = min(line.value(), line_index);
                            } else {
                                line = max(line.value(), line_index);
                            }
                        }
                    }
                };

                collect_implicit_area_line_names(GridDimension::Column);
                collect_implicit_area_line_names(GridDimension::Row);

                auto subgrid_start_in_parent = [item](GridDimension dimension) {
                    return item->raw_position(dimension);
                };
                auto subgrid_end_in_parent = [item](GridDimension dimension) {
                    return item->raw_position(dimension) + static_cast<int>(item->span(dimension));
                };
                auto overlaps = [](int start_a, int end_a, int start_b, int end_b) {
                    return max(start_a, start_b) < min(end_a, end_b);
                };

                for (auto const& [area_name, area] : parent_grid_areas) {
                    if (!area.row_start.has_value() || !area.row_end.has_value() || !area.column_start.has_value() || !area.column_end.has_value())
                        continue;

                    auto area_row_start = static_cast<int>(area.row_start.value());
                    auto area_row_end = static_cast<int>(area.row_end.value());
                    auto area_column_start = static_cast<int>(area.column_start.value());
                    auto area_column_end = static_cast<int>(area.column_end.value());

                    if (!overlaps(area_row_start, area_row_end, subgrid_start_in_parent(GridDimension::Row), subgrid_end_in_parent(GridDimension::Row))
                        || !overlaps(area_column_start, area_column_end, subgrid_start_in_parent(GridDimension::Column), subgrid_end_in_parent(GridDimension::Column))) {
                        continue;
                    }

                    auto area_start = dimension == GridDimension::Column ? area_column_start : area_row_start;
                    auto area_end = dimension == GridDimension::Column ? area_column_end : area_row_end;
                    auto subgrid_start = subgrid_start_in_parent(dimension);
                    auto subgrid_end = subgrid_end_in_parent(dimension);

                    auto start_line_index = max(area_start, subgrid_start) - subgrid_start;
                    auto end_line_index = min(area_end, subgrid_end) - subgrid_start;
                    if (start_line_index >= 0 && static_cast<size_t>(start_line_index) < lines.size()) {
                        lines[start_line_index].append({ .name = MUST(String::formatted("{}-start", area_name)), .implicit = true });
                    }
                    if (end_line_index >= 0 && static_cast<size_t>(end_line_index) < lines.size()) {
                        lines[end_line_index].append({ .name = MUST(String::formatted("{}-end", area_name)), .implicit = true });
                    }
                }
            }
        }

        size_t line_index = 0;
        Function<void(CSS::GridTrackSizeList const&)> expand_line_names = [&](CSS::GridTrackSizeList const& list) {
            auto const& items = list.list();
            for (size_t item_index = 0; item_index < items.size(); ++item_index) {
                auto const& item = items[item_index];
                if (item.has<CSS::GridLineNames>()) {
                    if (line_index < lines.size())
                        lines[line_index].extend(item.get<CSS::GridLineNames>().names());
                    ++line_index;
                } else if (item.has<CSS::ExplicitGridTrack>()) {
                    auto const& explicit_track = item.get<CSS::ExplicitGridTrack>();
                    if (!explicit_track.is_repeat())
                        continue;

                    auto const& repeat = explicit_track.repeat();
                    size_t repeat_count = 0;
                    if (repeat.is_fixed()) {
                        repeat_count = repeat.repeat_count();
                    } else if (repeat.is_auto_fill()) {
                        // https://drafts.csswg.org/css-grid-2/#auto-repeat
                        // On a subgridded axis, the auto-fill keyword is only valid once per
                        // <line-name-list>, and repeats enough times for the name list to match the
                        // subgrid's specified grid span, falling back to 0 if the span is already
                        // fulfilled.
                        auto line_names_per_repeat = count_subgrid_line_name_lists(repeat.grid_track_size_list());
                        auto remaining_line_name_lists = count_subgrid_line_name_lists_from_index(list, item_index + 1);
                        if (line_names_per_repeat > 0 && line_index < lines.size() && lines.size() - line_index > remaining_line_name_lists)
                            repeat_count = (lines.size() - line_index - remaining_line_name_lists) / line_names_per_repeat;
                    }

                    for (size_t i = 0; i < repeat_count; ++i)
                        expand_line_names(repeat.grid_track_size_list());
                }
            }
        };
        expand_line_names(lines_definition);
        return;
    }

    if (lines_definition.is_subgrid()) {
        // https://drafts.csswg.org/css-grid-2/#subgrid-listing
        // If there is no parent grid, or if the grid container is otherwise
        // forced to establish an independent formatting context, the used value
        // is the initial value, grid-template-rows/none, and the grid container
        // is not a subgrid.
        lines.append({});
        return;
    }

    Vector<CSS::GridLineName> line_names;
    Function<void(CSS::GridTrackSizeList const&)> expand_lines_definition = [&](CSS::GridTrackSizeList const& lines_definition) {
        for (auto const& item : lines_definition.list()) {
            if (item.has<CSS::GridLineNames>()) {
                line_names.extend(item.get<CSS::GridLineNames>().names());
            } else if (item.has<CSS::ExplicitGridTrack>()) {
                auto const& explicit_track = item.get<CSS::ExplicitGridTrack>();
                if (explicit_track.is_default() || explicit_track.is_minmax()) {
                    lines.append(line_names);
                    line_names.clear();
                } else if (explicit_track.is_repeat()) {
                    int repeat_count = 0;
                    if (explicit_track.repeat().is_auto_fill() || explicit_track.repeat().is_auto_fit())
                        repeat_count = count_of_repeated_auto_fill_or_fit_tracks(dimension, explicit_track);
                    else
                        repeat_count = explicit_track.repeat().repeat_count();
                    auto const& repeat_track = explicit_track.repeat();
                    for (int i = 0; i < repeat_count; i++)
                        expand_lines_definition(repeat_track.grid_track_size_list());
                } else {
                    VERIFY_NOT_REACHED();
                }
            }
        }
    };

    expand_lines_definition(lines_definition);
    lines.append(line_names);
}

void OccupationGrid::set_occupied(int column_start, int column_end, int row_start, int row_end)
{
    for (int row_index = row_start; row_index < row_end; row_index++) {
        for (int column_index = column_start; column_index < column_end; column_index++) {
            m_min_column_index = min(m_min_column_index, column_index);
            m_max_column_index = max(m_max_column_index, column_index);
            m_min_row_index = min(m_min_row_index, row_index);
            m_max_row_index = max(m_max_row_index, row_index);

            m_occupation_grid.set(GridPosition { .row = row_index, .column = column_index });
        }
    }
}

bool OccupationGrid::is_occupied(int column_index, int row_index) const
{
    return m_occupation_grid.contains(GridPosition { row_index, column_index });
}

bool OccupationGrid::is_area_occupied(int column_start, int row_start, int column_span, int row_span) const
{
    for (int row = row_start; row < row_start + row_span; row++) {
        for (int column = column_start; column < column_start + column_span; column++) {
            if (is_occupied(column, row))
                return true;
        }
    }
    return false;
}

int GridItem::gap_adjusted_row() const
{
    return row.value() * 2;
}

int GridItem::gap_adjusted_column() const
{
    return column.value() * 2;
}

bool GridFormattingContext::should_treat_grid_container_maximum_size_as_none(GridDimension dimension) const
{
    if (dimension == GridDimension::Column)
        return should_treat_max_width_as_none(grid_container(), m_available_space->width);
    return should_treat_max_height_as_none(grid_container(), m_available_space->height);
}

CSSPixels GridFormattingContext::calculate_grid_container_maximum_size(GridDimension dimension) const
{
    auto const& computed_values = grid_container().computed_values();
    if (dimension == GridDimension::Column)
        return calculate_inner_width(grid_container(), m_available_space->width, computed_values.max_width());
    return calculate_inner_height(grid_container(), m_available_space.value(), computed_values.max_height());
}

bool GridFormattingContext::should_treat_preferred_size_as_auto_for_intrinsic_contribution(GridItem const& item, GridDimension dimension) const
{
    auto available_space_for_item = item.available_space();
    auto should_treat_preferred_size_as_auto = [&] {
        if (dimension == GridDimension::Column)
            return should_treat_width_as_auto(item.box, available_space_for_item);
        return should_treat_height_as_auto(item.box, available_space_for_item);
    }();
    if (should_treat_preferred_size_as_auto)
        return true;

    // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
    // When a non-replaced grid item's percentage preferred size contributes to
    // sizing tracks in the same axis, the percentage is cyclic and behaves as
    // the property's initial value for intrinsic contribution calculations.
    return !item.box.is_replaced_box() && item.preferred_size(dimension).contains_percentage();
}

CSSPixels GridFormattingContext::calculate_min_content_size(GridItem const& item, GridDimension dimension) const
{
    if (dimension == GridDimension::Column) {
        return calculate_min_content_width(item.box);
    }
    return calculate_min_content_height(item.box, item.available_space().width.to_px_or_zero());
}

CSSPixels GridFormattingContext::calculate_max_content_size(GridItem const& item, GridDimension dimension) const
{
    if (dimension == GridDimension::Column) {
        return calculate_max_content_width(item.box);
    }
    return calculate_max_content_height(item.box, item.available_space().width.to_px_or_zero());
}

CSSPixels GridFormattingContext::containing_block_size_for_item(GridItem const& item, GridDimension dimension) const
{
    CSSPixels containing_block_size = 0;
    for_each_spanned_track_by_item(item, dimension, [&](GridTrack const& track) {
        containing_block_size += track.base_size;
    });
    return containing_block_size;
}

Box const& GridFormattingContext::table_box_inside_table_wrapper(GridItem const& item) const
{
    Optional<Box const&> table_box;
    item.box.for_each_in_subtree_of_type<Box>([&](Box const& child_box) {
        if (child_box.display().is_table_inside()) {
            table_box = child_box;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    VERIFY(table_box.has_value());
    return *table_box;
}

void GridFormattingContext::resolve_table_wrapper_grid_item_width(GridItem& item, CSSPixels containing_block_width)
{
    VERIFY(is<TableWrapper>(item.box));

    auto table_wrapper_containing_block_width = non_cyclic_containing_block_width_for_table_wrapper(item, containing_block_width);
    auto available_space = AvailableSpace {
        AvailableSize::make_definite(clamp_to_max_dimension_value(table_wrapper_containing_block_width)),
        AvailableSize::make_definite(clamp_to_max_dimension_value(containing_block_size_for_item(item, GridDimension::Row)))
    };
    auto table_wrapper_width = compute_table_box_width_inside_table_wrapper(
        item.box, available_space, table_wrapper_containing_block_width, TableWrapperWidthMode::UseTableUsedWidthIfNotAuto);
    auto const& preferred_width = item.preferred_size(GridDimension::Column);
    if (!preferred_width.is_auto())
        table_wrapper_width = max(table_wrapper_width, calculate_inner_width(item.box, available_space.width, preferred_width));
    if (table_box_inside_table_wrapper(item).computed_values().width().is_auto())
        table_wrapper_width = max(table_wrapper_width, calculate_min_content_width(item.box));
    auto alignment = alignment_for_item(item.box, GridDimension::Column);
    if (table_box_inside_table_wrapper(item).computed_values().width().is_auto()
        && (alignment == Alignment::Normal || alignment == Alignment::Stretch)
        && !item.margin_start(GridDimension::Column).is_auto()
        && !item.margin_end(GridDimension::Column).is_auto()) {
        table_wrapper_width = max(table_wrapper_width, table_wrapper_containing_block_width - item.used_margin_box_start(GridDimension::Column) - item.used_margin_box_end(GridDimension::Column));
    }
    if (!should_treat_max_width_as_none(item.box, available_space.width))
        table_wrapper_width = min(table_wrapper_width, calculate_inner_width(item.box, available_space.width, item.maximum_size(GridDimension::Column)));
    if (!item.minimum_size(GridDimension::Column).is_auto())
        table_wrapper_width = max(table_wrapper_width, calculate_inner_width(item.box, available_space.width, item.minimum_size(GridDimension::Column)));

    auto const& computed_values = item.box.computed_values();
    item.used_values.margin_left = computed_values.margin().left().to_px_or_zero(grid_container(), table_wrapper_containing_block_width);
    item.used_values.margin_right = computed_values.margin().right().to_px_or_zero(grid_container(), table_wrapper_containing_block_width);

    auto margin_start = item.used_margin_start(GridDimension::Column);
    auto margin_end = item.used_margin_end(GridDimension::Column);
    auto free_space_left_for_margins = table_wrapper_containing_block_width - table_wrapper_width - item.used_margin_box_start(GridDimension::Column) - item.used_margin_box_end(GridDimension::Column);
    bool start_is_auto = item.margin_start(GridDimension::Column).is_auto();
    bool end_is_auto = item.margin_end(GridDimension::Column).is_auto();
    auto absorbed_margin_space = max(CSSPixels(0), free_space_left_for_margins);
    if (start_is_auto && end_is_auto) {
        margin_start = absorbed_margin_space / 2;
        margin_end = absorbed_margin_space / 2;
    } else if (start_is_auto) {
        margin_start = absorbed_margin_space;
    } else if (end_is_auto) {
        margin_end = absorbed_margin_space;
    }

    if (!(start_is_auto || end_is_auto) || free_space_left_for_margins <= 0) {
        auto free_space_left_for_alignment = table_wrapper_containing_block_width - table_wrapper_width - item.used_margin_box_start(GridDimension::Column) - item.used_margin_box_end(GridDimension::Column);
        switch (alignment) {
        case Alignment::Center:
            margin_start += free_space_left_for_alignment / 2;
            margin_end += free_space_left_for_alignment / 2;
            break;
        case Alignment::Start:
        case Alignment::Baseline:
            margin_end += free_space_left_for_alignment;
            break;
        case Alignment::End:
            margin_start += free_space_left_for_alignment;
            break;
        case Alignment::Normal:
        case Alignment::Stretch:
        default:
            break;
        }
    }

    item.used_values.margin_left = margin_start;
    item.used_values.margin_right = margin_end;
    item.used_values.set_content_width(table_wrapper_width);
}

CSSPixels GridFormattingContext::non_cyclic_containing_block_width_for_table_wrapper(GridItem const& item, CSSPixels containing_block_width) const
{
    auto const& table_box = table_box_inside_table_wrapper(item);
    auto const& table_box_computed_values = table_box.computed_values();
    if (!table_box_computed_values.width().contains_percentage()
        && !table_box_computed_values.min_width().contains_percentage()
        && !table_box_computed_values.max_width().contains_percentage()) {
        return containing_block_width;
    }

    if (!m_grid_container_used_values.has_definite_width())
        return containing_block_width;

    auto const available_width = AvailableSize::make_definite(clamp_to_max_dimension_value(m_grid_container_used_values.content_width()));
    bool spans_intrinsic_column_track = false;
    for_each_spanned_track_by_item(item, GridDimension::Column, [&](GridTrack const& track) {
        if (track.is_gap)
            return;
        if (track.min_track_sizing_function.is_intrinsic(available_width) || track.max_track_sizing_function.is_intrinsic(available_width))
            spans_intrinsic_column_track = true;
    });
    if (!spans_intrinsic_column_track)
        return containing_block_width;

    // CSS Grid breaks cyclic percentage dependencies during intrinsic track sizing. Percentage table width/min/max
    // constraints can contribute to intrinsic column tracks, so do not feed that contribution back into the table
    // wrapper containing block when resolving the final table width or margins.
    CSSPixels total_column_width = 0;
    for (auto const& track : m_grid_columns_and_gaps)
        total_column_width += track.base_size;

    if (total_column_width <= m_grid_container_used_values.content_width())
        return containing_block_width;

    auto non_spanned_column_width = max(CSSPixels(0), total_column_width - containing_block_width);
    auto non_cyclic_containing_block_width = max(CSSPixels(0), m_grid_container_used_values.content_width() - non_spanned_column_width);
    return min(containing_block_width, non_cyclic_containing_block_width);
}

CSSPixels GridFormattingContext::calculate_min_content_contribution(GridItem const& item, GridDimension dimension) const
{
    auto should_treat_preferred_size_as_auto = should_treat_preferred_size_as_auto_for_intrinsic_contribution(item, dimension);

    auto maximum_size = CSSPixels::max();
    if (auto const& css_maximum_size = item.maximum_size(dimension); css_maximum_size.is_length_percentage() && !css_maximum_size.contains_percentage())
        maximum_size = css_maximum_size.to_px(item.box, 0);

    if (should_treat_preferred_size_as_auto) {
        CSSPixels min_content_size;
        // NOTE: Not defined in spec, but matches other browsers: a scroll container's min-content
        //       width contribution is 0 because its content can overflow and scroll horizontally.
        //       This does NOT apply to the row dimension — scroll containers must still contribute
        //       their content height, otherwise grids with height:min-content collapse rows to 0.
        if (dimension == GridDimension::Column && item.box.is_scroll_container()) {
            min_content_size = 0;
        } else {
            min_content_size = calculate_min_content_size(item, dimension);
        }
        auto result = item.add_margin_box_sizes(min_content_size, dimension);
        return min(result, maximum_size);
    }

    auto preferred_size = item.preferred_size(dimension);
    if (dimension == GridDimension::Column) {
        auto width = calculate_inner_width(item.box, m_available_space->width, preferred_size);
        return min(item.add_margin_box_sizes(width, dimension), maximum_size);
    }
    auto height = calculate_inner_height(item.box, *m_available_space, preferred_size);
    return min(item.add_margin_box_sizes(height, dimension), maximum_size);
}

CSSPixels GridFormattingContext::calculate_max_content_contribution(GridItem const& item, GridDimension dimension) const
{
    auto available_space_for_item = item.available_space();

    auto should_treat_preferred_size_as_auto = should_treat_preferred_size_as_auto_for_intrinsic_contribution(item, dimension);

    auto maximum_size = CSSPixels::max();
    if (auto const& css_maximum_size = item.maximum_size(dimension); css_maximum_size.is_length_percentage() && !css_maximum_size.contains_percentage())
        maximum_size = css_maximum_size.to_px(item.box, 0);

    auto preferred_size = item.preferred_size(dimension);
    if (should_treat_preferred_size_as_auto || preferred_size.is_fit_content()) {
        auto fit_content_size = dimension == GridDimension::Column ? calculate_fit_content_width(item.box, available_space_for_item) : calculate_fit_content_height(item.box, available_space_for_item);
        auto result = item.add_margin_box_sizes(fit_content_size, dimension);
        return min(result, maximum_size);
    }

    auto resolve_size = [&] {
        auto available_width = AvailableSize::make_definite(clamp_to_max_dimension_value(containing_block_size_for_item(item, GridDimension::Column)));
        if (dimension == GridDimension::Row) {
            auto available_height = AvailableSize::make_definite(clamp_to_max_dimension_value(containing_block_size_for_item(item, GridDimension::Row)));
            AvailableSpace item_available_space { available_width, available_height };
            return calculate_inner_height(item.box, item_available_space, preferred_size);
        }
        return calculate_inner_width(item.box, available_width, preferred_size);
    };

    auto result = item.add_margin_box_sizes(resolve_size(), dimension);
    return min(result, maximum_size);
}

Optional<CSSPixels> GridFormattingContext::calculate_fixed_max_track_size_limit(GridItem const& item, GridDimension dimension) const
{
    // https://drafts.csswg.org/css-grid-2/#algo-spanning-items
    // For an item spanning multiple tracks, the upper limit used to calculate its limited min-/max-content
    // contribution is the sum of the fixed max track sizing functions of any tracks it spans, and is applied
    // if it only spans such tracks.
    //
    // https://drafts.csswg.org/css-grid-2#algo-terms
    // In all cases, treat auto and fit-content() as max-content, except where specified otherwise for fit-content().
    auto const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

    CSSPixels result = 0;
    bool saw_track = false;
    bool has_limit = true;
    for_each_spanned_track_by_item(item, dimension, [&](GridTrack const& track) {
        if (!has_limit)
            return;

        saw_track = true;
        auto const& max_track_sizing_function = track.max_track_sizing_function;

        if (max_track_sizing_function.is_fixed(available_size)) {
            auto const& max_track_size = max_track_sizing_function.css_size();
            result += max_track_size.to_px(grid_container(), available_size.to_px_or_zero());
            return;
        }

        if (max_track_sizing_function.is_fit_content()) {
            auto const& max_track_size = max_track_sizing_function.css_size();
            auto const& fit_content_available_space = max_track_size.fit_content_available_space();
            if (fit_content_available_space.has_value()
                && (!fit_content_available_space->contains_percentage() || available_size.is_definite())) {
                result += max_track_size.to_px(grid_container(), available_size.to_px_or_zero());
                return;
            }
        }

        has_limit = false;
    });

    if (!has_limit)
        return {};
    if (!saw_track)
        return {};
    return result;
}

CSSPixels GridFormattingContext::calculate_limited_min_content_contribution(GridItem const& item, GridDimension dimension) const
{
    // The limited min-content contribution of an item is its min-content contribution,
    // limited by the max track sizing function (which could be the argument to a fit-content() track
    // sizing function) if that is fixed and ultimately floored by its minimum contribution.
    auto min_content_contribution = calculate_min_content_contribution(item, dimension);
    auto minimum_contribution = calculate_minimum_contribution(item, dimension);
    if (min_content_contribution < minimum_contribution)
        return minimum_contribution;

    if (auto fixed_max_track_size_limit = calculate_fixed_max_track_size_limit(item, dimension); fixed_max_track_size_limit.has_value())
        return max(min(min_content_contribution, fixed_max_track_size_limit.value()), minimum_contribution);

    if (!should_treat_grid_container_maximum_size_as_none(dimension)) {
        auto max_size = calculate_grid_container_maximum_size(dimension);
        if (min_content_contribution > max_size)
            return max_size;
    }

    return min_content_contribution;
}

CSSPixels GridFormattingContext::calculate_limited_max_content_contribution(GridItem const& item, GridDimension dimension) const
{
    // The limited max-content contribution of an item is its max-content contribution,
    // limited by the max track sizing function (which could be the argument to a fit-content() track
    // sizing function) if that is fixed and ultimately floored by its minimum contribution.
    auto max_content_contribution = calculate_max_content_contribution(item, dimension);
    auto minimum_contribution = calculate_minimum_contribution(item, dimension);
    if (max_content_contribution < minimum_contribution)
        return minimum_contribution;

    if (auto fixed_max_track_size_limit = calculate_fixed_max_track_size_limit(item, dimension); fixed_max_track_size_limit.has_value())
        return max(min(max_content_contribution, fixed_max_track_size_limit.value()), minimum_contribution);

    if (!should_treat_grid_container_maximum_size_as_none(dimension)) {
        auto max_size = calculate_grid_container_maximum_size(dimension);
        if (max_content_contribution > max_size)
            return max_size;
    }

    return max_content_contribution;
}

CSSPixels GridFormattingContext::content_size_suggestion(GridItem const& item, GridDimension dimension) const
{
    // The content size suggestion is the min-content size in the relevant axis
    // FIXME: clamped, if it has a preferred aspect ratio, by any definite opposite-axis minimum and maximum sizes
    // converted through the aspect ratio.
    return calculate_min_content_size(item, dimension);
}

Optional<CSSPixels> GridFormattingContext::specified_size_suggestion(GridItem const& item, GridDimension dimension) const
{
    // https://www.w3.org/TR/css-grid-1/#specified-size-suggestion
    // If the item’s preferred size in the relevant axis is definite, then the specified size suggestion is that size.
    // It is otherwise undefined.
    if (!item.box.is_replaced_box() && item.preferred_size(dimension).contains_percentage())
        return {};

    auto has_definite_preferred_size = dimension == GridDimension::Column ? item.used_values.has_definite_width() : item.used_values.has_definite_height();
    if (has_definite_preferred_size) {
        // FIXME: consider margins, padding and borders because it is outer size.
        auto containing_block_size = containing_block_size_for_item(item, dimension);
        return item.preferred_size(dimension).to_px(item.box, containing_block_size);
    }

    return {};
}

Optional<CSSPixels> GridFormattingContext::transferred_size_suggestion(GridItem const& item, GridDimension dimension) const
{
    // https://www.w3.org/TR/css-grid-2/#transferred-size-suggestion
    // If the item has a preferred aspect ratio and its preferred size in the opposite axis is definite, then the transferred
    // size suggestion is that size (clamped by the opposite-axis minimum and maximum sizes if they are definite), converted
    // through the aspect ratio. It is otherwise undefined.
    if (!item.box.preferred_aspect_ratio().has_value()) {
        return {};
    }

    CSS::Size const& preferred_size_in_opposite_axis = item.preferred_size(dimension == GridDimension::Column ? GridDimension::Row : GridDimension::Column);
    if (preferred_size_in_opposite_axis.is_length()) {
        auto opposite_axis_size = preferred_size_in_opposite_axis.length().to_px(item.box);
        // FIXME: Clamp by opposite-axis minimum and maximum sizes if they are definite
        return opposite_axis_size * item.box.preferred_aspect_ratio().value();
    }

    return {};
}

CSSPixels GridFormattingContext::content_based_minimum_size(GridItem const& item, GridDimension dimension) const
{
    // https://www.w3.org/TR/css-grid-1/#content-based-minimum-size

    CSSPixels result = 0;
    // The content-based minimum size for a grid item in a given dimension is its specified size suggestion if it exists,
    if (auto specified_size_suggestion = this->specified_size_suggestion(item, dimension); specified_size_suggestion.has_value()) {
        result = specified_size_suggestion.value();
    }
    // otherwise its transferred size suggestion if that exists,
    else if (auto transferred_size_suggestion = this->transferred_size_suggestion(item, dimension); transferred_size_suggestion.has_value()) {
        result = transferred_size_suggestion.value();
    }
    // else its content size suggestion.
    else {
        result = content_size_suggestion(item, dimension);
    }

    // However, if in a given dimension the grid item spans only grid tracks that have a fixed max track sizing function, then
    // its specified size suggestion and content size suggestion in that dimension (and its input from this dimension to the
    // transferred size suggestion in the opposite dimension) are further clamped to less than or equal to the stretch fit into
    // the grid area’s maximum size in that dimension, as represented by the sum of those grid tracks’ max track sizing functions
    // plus any intervening fixed gutters.
    // FIXME: Account for intervening fixed gutters.
    auto const& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    auto const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;
    auto item_track_index = item.raw_position(dimension);
    auto item_track_span = item.span(dimension);
    bool spans_only_tracks_with_limited_max_track_sizing_function = true;
    CSSPixels sum_of_max_sizing_functions = 0;
    for (size_t index = 0; index < item_track_span; index++) {
        auto const& track = tracks[item_track_index + index];
        if (!track.max_track_sizing_function.is_fixed(available_size)) {
            spans_only_tracks_with_limited_max_track_sizing_function = false;
            break;
        }
        sum_of_max_sizing_functions += track.max_track_sizing_function.css_size().to_px(item.box, m_available_space->width.to_px_or_zero());
    }
    if (spans_only_tracks_with_limited_max_track_sizing_function) {
        result = min(result, sum_of_max_sizing_functions);
    }

    // In all cases, the size suggestion is additionally clamped by the maximum size in the affected axis, if it’s definite.
    auto const& maximum_size = item.maximum_size(dimension);
    if (maximum_size.is_length_percentage() && !maximum_size.contains_percentage())
        result = min(result, maximum_size.to_px(item.box, 0));

    // If the item is a compressible replaced element, and has a definite preferred size or maximum size in the relevant axis,
    // the size suggestion is capped by those sizes; for this purpose, any indefinite percentages in these sizes are resolved
    // against zero (and considered definite).
    // FIXME: "compressible replaced element" includes more elements than is_replaced_box().
    auto const& preferred_size = item.preferred_size(dimension);
    if (item.box.is_replaced_box() && (preferred_size.is_percentage() || maximum_size.is_percentage())) {
        // NOTE: Implements "for this purpose, any indefinite percentages in these sizes are resolved
        //       against zero (and considered definite)." part.
        result = 0;
    }

    return result;
}

CSSPixels GridFormattingContext::automatic_minimum_size(GridItem const& item, GridDimension dimension) const
{
    // To provide a more reasonable default minimum size for grid items, the used value of its automatic minimum size
    // in a given axis is the content-based minimum size if all of the following are true:
    // - it is not a scroll container
    // - it spans at least one track in that axis whose min track sizing function is auto
    // - if it spans more than one track in that axis, none of those tracks are flexible
    auto const& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
    auto item_track_index = item.raw_position(dimension);
    auto item_track_span = item.span(dimension);

    AvailableSize const& available_size = dimension == GridDimension::Column ? m_available_space->width : m_available_space->height;

    bool spans_auto_tracks = false;
    bool spans_flexible_tracks = false;
    for (size_t index = 0; index < item_track_span; index++) {
        auto const& track = tracks[item_track_index + index];
        if (track.max_track_sizing_function.is_flexible_length())
            spans_flexible_tracks = true;
        if (track.min_track_sizing_function.is_auto(available_size))
            spans_auto_tracks = true;
    }
    if (spans_auto_tracks && !item.box.is_scroll_container() && (item_track_span == 1 || !spans_flexible_tracks)) {
        return content_based_minimum_size(item, dimension);
    }

    // Otherwise, the automatic minimum size is zero, as usual.
    return 0;
}

CSSPixels GridFormattingContext::calculate_minimum_contribution(GridItem const& item, GridDimension dimension) const
{
    // The minimum contribution of an item is the smallest outer size it can have.
    // Specifically, if the item’s computed preferred size behaves as auto or depends on the size of its
    // containing block in the relevant axis, its minimum contribution is the outer size that would
    // result from assuming the item’s used minimum size as its preferred size; else the item’s minimum
    // contribution is its min-content contribution. Because the minimum contribution often depends on
    // the size of the item’s content, it is considered a type of intrinsic size contribution.

    auto should_treat_preferred_size_as_auto = should_treat_preferred_size_as_auto_for_intrinsic_contribution(item, dimension);

    if (should_treat_preferred_size_as_auto) {
        auto minimum_size = item.minimum_size(dimension);
        if (minimum_size.is_auto())
            return item.add_margin_box_sizes(automatic_minimum_size(item, dimension), dimension);
        if (minimum_size.is_min_content())
            return calculate_min_content_contribution(item, dimension);
        if (minimum_size.is_max_content())
            return calculate_max_content_contribution(item, dimension);
        CSSPixels inner_minimum_size;
        if (dimension == GridDimension::Column)
            inner_minimum_size = calculate_inner_width(item.box, item.available_space().width, minimum_size);
        else
            inner_minimum_size = calculate_inner_height(item.box, item.available_space(), minimum_size);
        return item.add_margin_box_sizes(inner_minimum_size, dimension);
    }

    return calculate_min_content_contribution(item, dimension);
}

StaticPositionRect GridFormattingContext::calculate_static_position_rect(Box const& box) const
{
    // Result of this function is only used when containing block is not a grid container.
    // If the containing block is a grid container then static position is a grid area rect and
    // layout_absolutely_positioned_element() defined for GFC knows how to handle this case.
    StaticPositionRect static_position;
    auto const& box_state = m_state.get(box);
    static_position.rect = { { 0, 0 }, { box_state.content_width(), box_state.content_height() } };
    return static_position;
}

}

namespace AK {

template<>
struct Traits<Web::Layout::GridPosition> : public DefaultTraits<Web::Layout::GridPosition> {
    static unsigned hash(Web::Layout::GridPosition const& key) { return pair_int_hash(key.row, key.column); }
};

}
