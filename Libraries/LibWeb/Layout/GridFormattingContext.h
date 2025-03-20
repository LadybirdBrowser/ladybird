/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022-2023, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/Layout/FormattingContext.h>

namespace Web::Layout {

enum class GridDimension {
    Row,
    Column
};

enum class Alignment {
    Normal,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
    Center,
    Start,
    End,
    Stretch,
};

struct GridPosition {
    int row;
    int column;
    inline bool operator==(GridPosition const&) const = default;
};

struct GridItem {
    GC::Ref<Box const> box;
    LayoutState::UsedValues& used_values;

    // Position and span are empty if the item is auto-placed which could only be the case for abspos items
    Optional<int> row;
    Optional<size_t> row_span;
    Optional<int> column;
    Optional<size_t> column_span;

    [[nodiscard]] size_t span(GridDimension const dimension) const
    {
        return dimension == GridDimension::Column ? column_span.value() : row_span.value();
    }

    [[nodiscard]] int raw_position(GridDimension const dimension) const
    {
        return dimension == GridDimension::Column ? column.value() : row.value();
    }

    [[nodiscard]] CSSPixels add_margin_box_sizes(CSSPixels content_size, GridDimension dimension) const
    {
        if (dimension == GridDimension::Column)
            return used_values.margin_box_left() + content_size + used_values.margin_box_right();
        return used_values.margin_box_top() + content_size + used_values.margin_box_bottom();
    }

    [[nodiscard]] int gap_adjusted_position(GridDimension const dimension) const
    {
        return dimension == GridDimension::Column ? gap_adjusted_column() : gap_adjusted_row();
    }

    [[nodiscard]] int gap_adjusted_row() const;
    [[nodiscard]] int gap_adjusted_column() const;

    CSS::ComputedValues const& computed_values() const
    {
        return box->computed_values();
    }

    CSS::Size const& minimum_size(GridDimension dimension) const
    {
        return dimension == GridDimension::Column ? computed_values().min_width() : computed_values().min_height();
    }

    CSS::Size const& maximum_size(GridDimension dimension) const
    {
        return dimension == GridDimension::Column ? computed_values().max_width() : computed_values().max_height();
    }

    CSS::Size const& preferred_size(GridDimension dimension) const
    {
        return dimension == GridDimension::Column ? computed_values().width() : computed_values().height();
    }

    AvailableSpace available_space() const
    {
        auto available_width = used_values.has_definite_width() ? AvailableSize::make_definite(used_values.content_width()) : AvailableSize::make_indefinite();
        auto available_height = used_values.has_definite_height() ? AvailableSize::make_definite(used_values.content_height()) : AvailableSize::make_indefinite();
        return { available_width, available_height };
    }
};

enum class FoundUnoccupiedPlace {
    No,
    Yes
};

class OccupationGrid {
public:
    OccupationGrid(size_t columns_count, size_t rows_count)
    {
        m_max_column_index = max(0, columns_count - 1);
        m_max_row_index = max(0, rows_count - 1);
    }
    OccupationGrid() { }

    void set_occupied(int column_start, int column_end, int row_start, int row_end);

    size_t column_count() const
    {
        return abs(m_min_column_index) + m_max_column_index + 1;
    }

    size_t row_count() const
    {
        return abs(m_min_row_index) + m_max_row_index + 1;
    }

    void set_max_column_index(size_t max_column_index) { m_max_column_index = max_column_index; }

    int min_column_index() const { return m_min_column_index; }
    int max_column_index() const { return m_max_column_index; }
    int min_row_index() const { return m_min_row_index; }
    int max_row_index() const { return m_max_row_index; }

    bool is_occupied(int column_index, int row_index) const;

    FoundUnoccupiedPlace find_unoccupied_place(GridDimension dimension, int& column_index, int& row_index, int column_span, int row_span) const;

private:
    HashTable<GridPosition> m_occupation_grid;

    int m_min_column_index { 0 };
    int m_max_column_index { 0 };
    int m_min_row_index { 0 };
    int m_max_row_index { 0 };
};

class GridFormattingContext final : public FormattingContext {
public:
    explicit GridFormattingContext(LayoutState&, LayoutMode, Box const& grid_container, FormattingContext* parent);
    ~GridFormattingContext();

    virtual bool inhibits_floating() const override { return true; }

    virtual void run(AvailableSpace const& available_space) override;
    virtual CSSPixels automatic_content_width() const override;
    virtual CSSPixels automatic_content_height() const override;
    StaticPositionRect calculate_static_position_rect(Box const&) const;

    Box const& grid_container() const { return context_box(); }

private:
    CSS::JustifyItems justification_for_item(Box const& box) const;
    CSS::AlignItems alignment_for_item(Box const& box) const;

    void resolve_items_box_metrics(GridDimension const dimension);

    CSSPixels m_automatic_content_height { 0 };

    bool is_auto_positioned_track(CSS::GridTrackPlacement const&, CSS::GridTrackPlacement const&) const;

    struct GridTrack {
        CSS::GridSize min_track_sizing_function;
        CSS::GridSize max_track_sizing_function;

        CSSPixels base_size { 0 };
        bool base_size_frozen { false };

        Optional<CSSPixels> growth_limit { 0 };
        bool growth_limit_frozen { false };
        bool infinitely_growable { false };

        CSSPixels space_to_distribute { 0 };
        CSSPixels planned_increase { 0 };
        CSSPixels item_incurred_increase { 0 };

        bool is_gap { false };

        static GridTrack create_from_definition(CSS::ExplicitGridTrack const& definition);
        static GridTrack create_auto();
        static GridTrack create_gap(CSSPixels size);
    };

    struct GridArea {
        String name;
        size_t row_start { 0 };
        size_t row_end { 1 };
        size_t column_start { 0 };
        size_t column_end { 1 };
        bool invalid { false }; /* FIXME: Ignore ignore invalid areas during layout */
    };

    struct GridLine {
        Vector<String> names;
    };
    Vector<GridLine> m_row_lines;
    Vector<GridLine> m_column_lines;

    void init_grid_lines(GridDimension);

    Vector<GridTrack> m_grid_rows;
    Vector<GridTrack> m_grid_columns;

    bool has_gaps(GridDimension const dimension) const
    {
        if (dimension == GridDimension::Column) {
            return !grid_container().computed_values().column_gap().has<CSS::NormalGap>();
        } else {
            return !grid_container().computed_values().row_gap().has<CSS::NormalGap>();
        }
    }

    template<typename Callback>
    void for_each_spanned_track_by_item(GridItem const& item, GridDimension const dimension, Callback callback)
    {
        auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
        auto& gaps = dimension == GridDimension::Column ? m_column_gap_tracks : m_row_gap_tracks;
        auto has_gaps = this->has_gaps(dimension);
        auto item_span = item.span(dimension);
        auto item_index = item.raw_position(dimension);
        for (size_t span = 0; span < item_span; span++) {
            auto track_index = item_index + span;
            if (track_index >= tracks.size())
                break;

            auto& track = tracks[track_index];
            callback(track);

            auto is_last_spanned_track = span == item_span - 1;
            if (has_gaps && !is_last_spanned_track) {
                auto& gap = gaps[track_index];
                callback(gap);
            }
        }
    }

    template<typename Callback>
    void for_each_spanned_track_by_item(GridItem const& item, GridDimension const dimension, Callback callback) const
    {
        auto& tracks = dimension == GridDimension::Column ? m_grid_columns : m_grid_rows;
        auto& gaps = dimension == GridDimension::Column ? m_column_gap_tracks : m_row_gap_tracks;
        auto has_gaps = this->has_gaps(dimension);
        auto item_span = item.span(dimension);
        auto item_index = item.raw_position(dimension);
        for (size_t span = 0; span < item_span; span++) {
            auto track_index = item_index + span;
            if (track_index >= tracks.size())
                break;

            auto& track = tracks[track_index];
            callback(track);

            auto is_last_spanned_track = span == item_span - 1;
            if (has_gaps && !is_last_spanned_track) {
                auto& gap = gaps[track_index];
                callback(gap);
            }
        }
    }

    Vector<GridTrack> m_row_gap_tracks;
    Vector<GridTrack> m_column_gap_tracks;

    Vector<GridTrack&> m_grid_rows_and_gaps;
    Vector<GridTrack&> m_grid_columns_and_gaps;

    size_t m_explicit_rows_line_count { 0 };
    size_t m_explicit_columns_line_count { 0 };

    OccupationGrid m_occupation_grid;
    Vector<GridItem> m_grid_items;

    Optional<AvailableSpace> m_available_space;

    LayoutState::UsedValues& m_grid_container_used_values;

    void determine_grid_container_height();
    void determine_intrinsic_size_of_grid_container(AvailableSpace const& available_space);

    void layout_absolutely_positioned_element(Box const&);
    virtual void parent_context_did_dimension_child_root_box() override;

    void resolve_grid_item_widths();
    void resolve_grid_item_heights();

    void resolve_track_spacing(GridDimension const dimension);

    AvailableSize get_free_space(AvailableSpace const&, GridDimension const) const;

    Optional<int> get_line_index_by_line_name(GridDimension dimension, String const&);
    CSSPixels resolve_definite_track_size(CSS::GridSize const&, AvailableSpace const&);
    int count_of_repeated_auto_fill_or_fit_tracks(GridDimension, CSS::ExplicitGridTrack const& repeated_track);

    void build_grid_areas();

    struct PlacementPosition {
        int start { 0 };
        int end { 0 };
        size_t span { 1 };
    };
    PlacementPosition resolve_grid_position(Box const& child_box, GridDimension const dimension);

    void place_grid_items();
    void place_item_with_row_and_column_position(Box const& child_box);
    void place_item_with_row_position(Box const& child_box);
    void place_item_with_column_position(Box const& child_box, int& auto_placement_cursor_x, int& auto_placement_cursor_y);
    void place_item_with_no_declared_position(Box const& child_box, int& auto_placement_cursor_x, int& auto_placement_cursor_y);
    void record_grid_placement(GridItem);

    void initialize_grid_tracks_from_definition(GridDimension);
    void initialize_grid_tracks_for_columns_and_rows();
    void initialize_gap_tracks(AvailableSpace const&);

    void collapse_auto_fit_tracks_if_needed(GridDimension const);

    enum class SpaceDistributionPhase {
        AccommodateMinimumContribution,
        AccommodateMinContentContribution,
        AccommodateMaxContentContribution
    };

    template<typename Match>
    void distribute_extra_space_across_spanned_tracks_base_size(GridDimension dimension, CSSPixels item_size_contribution, SpaceDistributionPhase phase, Vector<GridTrack&>& spanned_tracks, Match matcher);

    template<typename Match>
    void distribute_extra_space_across_spanned_tracks_growth_limit(CSSPixels item_size_contribution, Vector<GridTrack&>& spanned_tracks, Match matcher);

    void initialize_track_sizes(GridDimension);
    void resolve_intrinsic_track_sizes(GridDimension);
    void increase_sizes_to_accommodate_spanning_items_crossing_content_sized_tracks(GridDimension, size_t span);
    void increase_sizes_to_accommodate_spanning_items_crossing_flexible_tracks(GridDimension);
    void maximize_tracks_using_available_size(AvailableSpace const& available_space, GridDimension dimension);
    void maximize_tracks(GridDimension);
    void expand_flexible_tracks(GridDimension);
    void stretch_auto_tracks(GridDimension);
    void run_track_sizing(GridDimension);

    CSSPixels calculate_grid_container_maximum_size(GridDimension const) const;

    CSSPixels calculate_min_content_size(GridItem const&, GridDimension const) const;
    CSSPixels calculate_max_content_size(GridItem const&, GridDimension const) const;

    CSSPixels calculate_min_content_contribution(GridItem const&, GridDimension const) const;
    CSSPixels calculate_max_content_contribution(GridItem const&, GridDimension const) const;

    CSSPixels calculate_limited_min_content_contribution(GridItem const&, GridDimension const) const;
    CSSPixels calculate_limited_max_content_contribution(GridItem const&, GridDimension const) const;

    CSSPixels containing_block_size_for_item(GridItem const&, GridDimension const) const;

    CSSPixelRect get_grid_area_rect(GridItem const&) const;

    CSSPixels content_size_suggestion(GridItem const&, GridDimension const) const;
    Optional<CSSPixels> specified_size_suggestion(GridItem const&, GridDimension const) const;
    Optional<CSSPixels> transferred_size_suggestion(GridItem const&, GridDimension const) const;
    CSSPixels content_based_minimum_size(GridItem const&, GridDimension const) const;
    CSSPixels automatic_minimum_size(GridItem const&, GridDimension const) const;
    CSSPixels calculate_minimum_contribution(GridItem const&, GridDimension const) const;
};

}
