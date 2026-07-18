/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/TableGrid.h>

namespace Web::Layout {

enum class TableDimension {
    Row,
    Column
};

class TableFormattingContext final : public FormattingContext {
public:
    enum class RowMeasurement {
        Include,
        Skip,
    };

    explicit TableFormattingContext(LayoutState&, LayoutMode, Box const&, FormattingContext* parent);
    ~TableFormattingContext();

    void run_until_inline_size_calculation(LayoutInput const&, RowMeasurement = RowMeasurement::Include);

    virtual void run(LayoutInput const&) override;
    virtual CSSPixels automatic_content_inline_size() const override;
    virtual CSSPixels automatic_content_block_size() const override;

    Box const& table_box() const { return context_box(); }

    void set_pending_table_box_content_offset_in_wrapper(LogicalOffset offset) { m_pending_table_box_content_offset_in_wrapper = offset; }
    LogicalOffset pending_table_box_content_offset_in_wrapper() const { return m_pending_table_box_content_offset_in_wrapper; }

    static bool border_is_less_specific(CSS::BorderData const& a, CSS::BorderData const& b);

    virtual void parent_context_did_dimension_child_root_box() override;

private:
    struct MeasuredCellContent {
        CSSPixels content_block_size;
        CSSPixels first_baseline;
    };
    Optional<MeasuredCellContent> measure_cell_content(Box const&, LayoutState::UsedValues const&, AvailableSpace const& inner_available_space);

    CSSPixels run_caption_layout(CSS::CaptionSide, AvailableSpace const&);
    CSSPixels compute_capmin();
    void compute_constrainedness();
    void compute_cell_measures(RowMeasurement);
    void initialize_row_content_sizes();
    void compute_outer_content_sizes();
    template<class RowOrColumn>
    void initialize_table_measures();
    template<class RowOrColumn>
    void compute_table_measures();
    template<class RowOrColumn>
    void compute_intrinsic_percentage(size_t max_cell_span);
    void compute_table_inline_size();
    void distribute_inline_size_to_columns();
    void distribute_excess_inline_size_to_columns(CSSPixels available_inline_size);
    void distribute_excess_inline_size_to_columns_fixed_mode(CSSPixels excess_inline_size);
    bool can_skip_row_intrinsic_measurement() const;
    void compute_table_block_size();
    void distribute_block_size_to_rows();
    void position_row_boxes();
    void position_cell_boxes();
    void border_conflict_resolution();
    CSSPixels border_spacing_inline() const;
    CSSPixels border_spacing_block() const;
    void finish_grid_initialization(TableGrid const&);
    void seed_table_participant_used_values(ContainingBlockConstraints const&);

    CSSPixels compute_columns_total_used_inline_size() const;
    void commit_candidate_column_inline_sizes(Vector<CSSPixels> const& candidate_inline_sizes);
    void assign_columns_inline_size_linear_combination(Vector<CSSPixels> const& candidate_inline_sizes, CSSPixels available_inline_size);

    template<class ColumnFilter, class BaseInlineSizeGetter>
    bool distribute_excess_inline_size_proportionally_to_base_inline_size(CSSPixels excess_inline_size, ColumnFilter column_filter, BaseInlineSizeGetter base_inline_size_getter);
    template<class ColumnFilter>
    bool distribute_excess_inline_size_equally(CSSPixels excess_inline_size, ColumnFilter column_filter);
    template<class ColumnFilter>
    bool distribute_excess_inline_size_by_intrinsic_percentage(CSSPixels excess_inline_size, ColumnFilter column_filter);

    bool use_fixed_mode_layout() const;

    ContainingBlockConstraints m_table_constraints;
    ContainingBlockConstraints m_participant_constraints;

    CSSPixels m_table_block_size { 0 };
    CSSPixels m_automatic_content_block_size { 0 };
    LogicalOffset m_pending_table_box_content_offset_in_wrapper {};
    Optional<CSSPixels> m_min_border_box_block_size_from_flex_item;

    Optional<AvailableSpace> m_available_space;
    bool m_needs_fixed_mode_row_measurement { false };

    struct Column {
        CSSPixels inline_offset { 0 };
        CSSPixels min_size { 0 };
        CSSPixels max_size { 0 };
        CSSPixels used_inline_size { 0 };
        bool has_intrinsic_percentage { false };
        double intrinsic_percentage { 0 };
        // Store whether the column is constrained: https://www.w3.org/TR/css-tables-3/#constrainedness
        bool is_constrained { false };
        // Store whether the column has originating cells, defined in https://www.w3.org/TR/css-tables-3/#terminology.
        bool has_originating_cells { false };
    };

    using Cell = TableGrid::Cell;
    using Row = TableGrid::Row;

    // Accessors to enable direction-agnostic table measurement.

    template<class RowOrColumn>
    static size_t cell_span(Cell const& cell);

    template<class RowOrColumn>
    static size_t cell_index(Cell const& cell);

    template<class RowOrColumn>
    static CSSPixels cell_min_size(Cell const& cell);

    template<class RowOrColumn>
    static CSSPixels cell_max_size(Cell const& cell);

    template<class RowOrColumn>
    static double cell_percentage_contribution(Cell const& cell);

    template<class RowOrColumn>
    static bool cell_has_intrinsic_percentage(Cell const& cell);

    template<class RowOrColumn>
    void initialize_intrinsic_percentages_from_rows_or_columns();

    template<class RowOrColumn>
    void initialize_intrinsic_percentages_from_cells();

    template<class RowOrColumn>
    CSSPixels border_spacing();

    template<class RowOrColumn>
    Vector<RowOrColumn>& table_rows_or_columns();

    CSSPixels compute_row_content_block_size(Cell const& cell) const;

    Vector<Cell> m_cells;
    Vector<Column> m_columns;
    Vector<Row> m_rows;
};

}
