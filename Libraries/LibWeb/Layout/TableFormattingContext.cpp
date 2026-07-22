/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/TableFormattingContext.h>
#include <LibWeb/Layout/TableWrapper.h>

namespace Web::Layout {

TableFormattingContext::TableFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& root, FormattingContext* parent)
    : FormattingContext(Type::Table, layout_mode, state, root, parent)
{
}

TableFormattingContext::~TableFormattingContext() = default;

static inline bool is_table_column_group(Box const& box)
{
    return box.display().is_table_column_group();
}

static inline bool is_table_column(Box const& box)
{
    return box.display().is_table_column();
}

CSSPixels TableFormattingContext::run_caption_layout(CSS::CaptionSide phase, AvailableSpace const& caption_available_space)
{
    CSSPixels captions_block_size = 0;
    for (auto child = table_box().first_child(); child; child = child->next_sibling()) {
        auto const* child_box_ptr = as_if<Box>(*child);
        if (!child_box_ptr || !child_box_ptr->display().is_table_caption() || child_box_ptr->computed_values().caption_side() != phase) {
            continue;
        }
        auto const& child_box = *child_box_ptr;
        // Captions live inside the table wrapper, so their quirks percentage height basis derives
        // from the wrapper, not from anything the table box inherited.
        auto const& caption_constraints = m_participant_constraints;
        bool caption_was_placed = false;
        // The caption boxes are principal block-level boxes that retain their own content, padding, margin, and border areas,
        // and are rendered as normal block boxes inside the table wrapper box, as described in https://www.w3.org/TR/CSS22/tables.html#model
        if (auto caption_context = create_independent_formatting_context_if_needed(m_state, m_layout_mode, child_box, this)) {
            auto inner_available_space = caption_available_space;
            auto* block_context = as_if<BlockFormattingContext>(caption_context.ptr());
            LogicalOffset caption_offset;
            if (block_context) {
                auto available_inline_size = caption_available_space.inline_size.to_px_or_zero();
                block_context->resolve_vertical_box_model_metrics(child_box, available_inline_size);
                CSSPixelPoint caption_position_in_block_context {};
                block_context->compute_inline_size(child_box, caption_available_space, caption_constraints, caption_position_in_block_context);
                inner_available_space = m_state.get(child_box).available_inner_space_or_constraints_from(caption_available_space);

                auto const& caption_state = m_state.get(child_box);
                caption_offset = { caption_state.border_box_left(), 0 };
                if (phase == CSS::CaptionSide::Bottom)
                    caption_offset.block_offset = m_state.get(table_box()).margin_box_block_size() + caption_state.margin_box_top();
            }

            caption_context->run(LayoutInput { inner_available_space, caption_constraints });

            if (block_context) {
                auto& caption_state = m_state.get_mutable(child_box);
                if (should_treat_block_size_as_auto(child_box, caption_available_space, m_participant_constraints)) {
                    auto content_block_size = child_box.has_size_containment() ? 0 : caption_context->automatic_content_block_size();
                    caption_state.set_content_block_size(content_block_size);
                }
                place_child(child_box, { caption_offset.inline_offset, caption_offset.block_offset });
                caption_was_placed = true;
            }
        }

        auto const& caption_state = m_state.get(child_box);
        if (phase == CSS::CaptionSide::Top) {
            m_pending_table_box_content_offset_in_wrapper.block_offset = caption_state.content_block_size() + caption_state.margin_box_bottom();
        } else if (!caption_was_placed) {
            place_child(child_box, { caption_state.border_box_left(), m_state.get(table_box()).margin_box_block_size() + caption_state.margin_box_top() });
        }
        captions_block_size += caption_state.margin_box_block_size();
    }
    return captions_block_size;
}

void TableFormattingContext::compute_constrainedness()
{
    // Definition of constrainedness: https://www.w3.org/TR/css-tables-3/#constrainedness
    // NB: The definition uses https://www.w3.org/TR/CSS21/visudet.html#propdef-width for width, which doesn't include
    //     keyword values. The remaining checks can be simplified to checking whether the size is a length.
    size_t column_index = 0;
    TableGrid::for_each_child_box_matching(table_box(), is_table_column_group, [&](auto& column_group_box) {
        TableGrid::for_each_child_box_matching(column_group_box, is_table_column, [&](auto& column_box) {
            auto const& computed_values = column_box.computed_values();
            if (computed_values.width().is_length()) {
                m_columns[column_index].is_constrained = true;
            }
            auto const& col_node = static_cast<HTML::HTMLElement const&>(*column_box.dom_node());
            unsigned span = col_node.get_attribute_value(HTML::AttributeNames::span).to_number<unsigned>().value_or(1);
            column_index += span;
        });
    });

    for (auto& row : m_rows) {
        auto const& computed_values = row.box.computed_values();
        if (computed_values.height().is_length()) {
            row.is_constrained = true;
        }
    }

    for (auto& cell : m_cells) {
        auto const& computed_values = cell.box.computed_values();
        if (computed_values.width().is_length()) {
            m_columns[cell.column_index].is_constrained = true;
        }

        if (computed_values.height().is_length()) {
            m_rows[cell.row_index].is_constrained = true;
        }
    }
}

void TableFormattingContext::compute_cell_measures(RowMeasurement row_measurement)
{
    // Implements https://www.w3.org/TR/css-tables-3/#computing-cell-measures.
    auto containing_block_inline_size = m_table_constraints.percentage_basis_inline_size.value_or(0);
    auto containing_block_block_size = m_table_constraints.percentage_basis_block_size.value_or(0);

    compute_constrainedness();

    for (auto& cell : m_cells) {
        auto const& computed_values = cell.box.computed_values();
        CSSPixels padding_block_start = computed_values.padding().top().to_px_or_zero(containing_block_block_size);
        CSSPixels padding_block_end = computed_values.padding().bottom().to_px_or_zero(containing_block_block_size);
        CSSPixels padding_inline_start = computed_values.padding().left().to_px_or_zero(containing_block_inline_size);
        CSSPixels padding_inline_end = computed_values.padding().right().to_px_or_zero(containing_block_inline_size);

        auto const& cell_state = m_state.get(cell.box);
        auto use_collapsing_borders_model = cell_state.override_borders_data().has_value();
        // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
        CSSPixels border_block_start = use_collapsing_borders_model ? round(cell_state.border_top / 2) : computed_values.border_top().width;
        CSSPixels border_block_end = use_collapsing_borders_model ? round(cell_state.border_bottom / 2) : computed_values.border_bottom().width;
        CSSPixels border_inline_start = use_collapsing_borders_model ? round(cell_state.border_left / 2) : computed_values.border_left().width;
        CSSPixels border_inline_end = use_collapsing_borders_model ? round(cell_state.border_right / 2) : computed_values.border_right().width;

        auto cell_intrinsic_inline_size_offsets = padding_inline_start + padding_inline_end + border_inline_start + border_inline_end;

        auto min_inline_size = computed_values.min_width().to_px(containing_block_inline_size);
        auto inline_size = computed_values.width().is_length() ? computed_values.width().to_px(containing_block_inline_size) : 0;
        auto max_inline_size = computed_values.max_width().is_length() ? computed_values.max_width().to_px(containing_block_inline_size) : CSSPixels::max();

        if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox) {
            min_inline_size -= cell_intrinsic_inline_size_offsets;
            inline_size -= cell_intrinsic_inline_size_offsets;
            max_inline_size -= cell_intrinsic_inline_size_offsets;
        }

        CSSPixels min_content_inline_size;
        CSSPixels max_content_inline_size;

        // https://drafts.csswg.org/css-tables-3/#computing-column-measures
        // For the purpose of measuring a column when laid out in fixed mode [...] the min-content and max-content width
        // of cells is considered zero unless they are directly specified as a length-percentage, in which case they are
        // resolved based on the table width (if it is definite, otherwise use 0).
        if (use_fixed_mode_layout()) {
            if (computed_values.width().is_length_percentage()) {
                min_content_inline_size = inline_size;
                max_content_inline_size = inline_size;
            }
        } else {
            min_content_inline_size = calculate_min_content_inline_size(cell.box, m_participant_constraints);
            max_content_inline_size = calculate_max_content_inline_size(cell.box, m_participant_constraints);
        }

        // The outer min-content inline size of a table cell is its minimum inline size adjusted by the cell intrinsic offsets.
        cell.outer_min_inline_size = max(min_inline_size, min_content_inline_size) + cell_intrinsic_inline_size_offsets;

        if (row_measurement == RowMeasurement::Include) {
            auto min_content_block_size = calculate_min_content_block_size(cell.box, max_content_inline_size, m_participant_constraints);
            auto max_content_block_size = calculate_max_content_block_size(cell.box, min_content_inline_size, m_participant_constraints);

            // The outer min-content block size of a table cell is its minimum block size adjusted by the cell intrinsic offsets.
            auto min_block_size = computed_values.min_height().to_px(containing_block_block_size);
            auto cell_intrinsic_block_size_offsets = padding_block_start + padding_block_end + border_block_start + border_block_end;
            cell.outer_min_block_size = max(min_block_size, min_content_block_size) + cell_intrinsic_block_size_offsets;

            // The tables specification isn't explicit on how to use the height and max-height CSS properties in the outer max-content formulas.
            // However, during this early phase we don't have enough information to resolve percentage sizes yet and the formulas for outer sizes
            // in the specification give enough clues to pick defaults in a way that makes sense.
            auto block_size = computed_values.height().is_length() ? computed_values.height().to_px(containing_block_block_size) : 0;
            auto max_block_size = computed_values.max_height().is_length() ? computed_values.max_height().to_px(containing_block_block_size) : CSSPixels::max();
            if (m_rows[cell.row_index].is_constrained) {
                // The outer max-content height of a table-cell in a constrained row is
                // max(min-height, height, min-content height, min(max-height, height)) adjusted by the cell intrinsic offsets.
                // NB: min(max-height, height) doesn't have any effect here, we can simplify the expression to max(min-height, height, min-content height).
                cell.outer_max_block_size = max(min_block_size, max(block_size, min_content_block_size)) + cell_intrinsic_block_size_offsets;
            } else {
                // The outer max-content height of a table-cell in a non-constrained row is
                // max(min-height, height, min-content height, min(max-height, max-content height)) adjusted by the cell intrinsic offsets.
                cell.outer_max_block_size = max(min_block_size, max(block_size, max(min_content_block_size, min(max_block_size, max_content_block_size)))) + cell_intrinsic_block_size_offsets;
            }
        }

        // See the explanation for block_size and max_block_size above.
        if (m_columns[cell.column_index].is_constrained) {
            // The outer max-content width of a table-cell in a constrained column is
            // max(min-width, width, min-content width, min(max-width, width)) adjusted by the cell intrinsic offsets.

            // AD-HOC: The formula defined by the spec doesn't respect max-width. We use a different formula that
            //         matches the behavior that is expected by WPT and is implemented by other browsers.
            // FIXME: Open a spec issue about this.
            cell.outer_max_inline_size = max(min_inline_size, min(max_inline_size, max(inline_size, min_content_inline_size))) + cell_intrinsic_inline_size_offsets;
        } else {
            // The outer max-content width of a table-cell in a non-constrained column is
            // max(min-width, width, min-content width, min(max-width, max-content width)) adjusted by the cell intrinsic offsets.
            cell.outer_max_inline_size = max(min_inline_size, max(inline_size, max(min_content_inline_size, min(max_inline_size, max_content_inline_size)))) + cell_intrinsic_inline_size_offsets;
        }
    }
}

void TableFormattingContext::compute_outer_content_sizes()
{
    auto containing_block_inline_size = m_table_constraints.percentage_basis_inline_size.value_or(0);

    size_t column_index = 0;
    TableGrid::for_each_child_box_matching(table_box(), is_table_column_group, [&](auto& column_group_box) {
        TableGrid::for_each_child_box_matching(column_group_box, is_table_column, [&](auto& column_box) {
            auto const& computed_values = column_box.computed_values();
            auto min_inline_size = computed_values.min_width().to_px(containing_block_inline_size);
            auto max_inline_size = computed_values.max_width().is_length() ? computed_values.max_width().to_px(containing_block_inline_size) : CSSPixels::max();
            auto inline_size = computed_values.width().to_px(containing_block_inline_size);
            // The outer min-content inline size of a table-column or table-column-group is max(min-width, width).
            m_columns[column_index].min_size = max(min_inline_size, inline_size);
            // The outer max-content inline size of a table-column or table-column-group is max(min-width, min(max-width, width)).
            m_columns[column_index].max_size = max(min_inline_size, min(max_inline_size, inline_size));
            auto const& col_node = static_cast<HTML::HTMLElement const&>(*column_box.dom_node());
            unsigned span = col_node.get_attribute_value(HTML::AttributeNames::span).to_number<unsigned>().value_or(1);
            column_index += span;
        });
    });

    initialize_row_content_sizes();
}

void TableFormattingContext::initialize_row_content_sizes()
{
    auto containing_block_block_size = m_table_constraints.percentage_basis_block_size.value_or(0);

    for (auto& row : m_rows) {
        auto const& computed_values = row.box.computed_values();
        auto min_block_size = computed_values.min_height().to_px(containing_block_block_size);
        auto max_block_size = computed_values.max_height().is_length() ? computed_values.max_height().to_px(containing_block_block_size) : CSSPixels::max();
        auto block_size = computed_values.height().to_px(containing_block_block_size);
        // The outer min-content block size of a table row or row group is max(min-block-size, block-size).
        row.min_size = max(min_block_size, block_size);
        // The outer max-content block size is max(min-block-size, min(max-block-size, block-size)).
        row.max_size = max(min_block_size, min(max_block_size, block_size));
    }
}

template<>
void TableFormattingContext::initialize_table_measures<TableFormattingContext::Row>()
{
    auto containing_block_block_size = m_table_constraints.percentage_basis_block_size.value_or(0);

    for (auto& cell : m_cells) {
        auto const& computed_values = cell.box.computed_values();
        if (cell.row_span == 1) {
            auto specified_block_size = computed_values.height().to_px(containing_block_block_size);
            // https://www.w3.org/TR/css-tables-3/#row-layout makes specified cell height part of the initialization formula for row table measures:
            // This is done by running the same algorithm as the column measurement, with the span=1 value being initialized (for min-content) with
            // the largest of the resulting height of the previous row layout, the height specified on the corresponding table-row (if any), and
            // the largest height specified on cells that span this row only (the algorithm starts by considering cells of span 2 on top of that assignment).
            m_rows[cell.row_index].min_size = max(m_rows[cell.row_index].min_size, max(cell.outer_min_block_size, specified_block_size));
            m_rows[cell.row_index].max_size = max(m_rows[cell.row_index].max_size, cell.outer_max_block_size);
        }
    }
}

template<>
void TableFormattingContext::initialize_table_measures<TableFormattingContext::Column>()
{
    // Implement the following parts of the specification, accounting for fixed layout mode:
    // https://www.w3.org/TR/css-tables-3/#min-content-width-of-a-column-based-on-cells-of-span-up-to-1
    // https://www.w3.org/TR/css-tables-3/#max-content-width-of-a-column-based-on-cells-of-span-up-to-1
    for (auto& cell : m_cells) {
        if (cell.column_span == 1 && (cell.row_index == 0 || !use_fixed_mode_layout())) {
            m_columns[cell.column_index].min_size = max(m_columns[cell.column_index].min_size, cell.outer_min_inline_size);
            m_columns[cell.column_index].max_size = max(m_columns[cell.column_index].max_size, cell.outer_max_inline_size);
        }
    }
}

template<class RowOrColumn>
void TableFormattingContext::compute_intrinsic_percentage(size_t max_cell_span)
{
    auto& rows_or_columns = table_rows_or_columns<RowOrColumn>();

    // https://www.w3.org/TR/css-tables-3/#intrinsic-percentage-width-of-a-column-based-on-cells-of-span-up-to-1
    initialize_intrinsic_percentages_from_rows_or_columns<RowOrColumn>();
    initialize_intrinsic_percentages_from_cells<RowOrColumn>();

    // Stores intermediate values for intrinsic percentage based on cells of span up to N for the iterative algorithm, to store them back at the end of the step.
    Vector<double> intrinsic_percentage_contribution_by_index;
    intrinsic_percentage_contribution_by_index.resize(rows_or_columns.size());
    for (size_t rc_index = 0; rc_index < rows_or_columns.size(); ++rc_index) {
        intrinsic_percentage_contribution_by_index[rc_index] = rows_or_columns[rc_index].intrinsic_percentage;
    }

    for (size_t current_span = 2; current_span <= max_cell_span; current_span++) {
        // https://www.w3.org/TR/css-tables-3/#intrinsic-percentage-width-of-a-column-based-on-cells-of-span-up-to-n-n--1
        for (auto& cell : m_cells) {
            auto cell_span_value = cell_span<RowOrColumn>(cell);
            if (cell_span_value != current_span) {
                continue;
            }
            auto cell_start_rc_index = cell_index<RowOrColumn>(cell);
            auto cell_end_rc_index = cell_start_rc_index + cell_span_value;
            // 1. Start with the percentage contribution of the cell.
            CSSPixels cell_contribution = CSSPixels::nearest_value_for(cell_percentage_contribution<RowOrColumn>(cell));
            // 2. Subtract the intrinsic percentage width of the column based on cells of span up to N-1 of all columns
            //    that the cell spans. If this gives a negative result, change it to 0%.
            for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                cell_contribution -= CSSPixels::nearest_value_for(rows_or_columns[rc_index].intrinsic_percentage);
                cell_contribution = max(cell_contribution, 0);
            }
            // Compute the sum of the non-spanning max-content sizes of all rows / columns spanned by the cell that have an intrinsic percentage
            // size of the row / column based on cells of span up to N-1 equal to 0%, to be used in step 3 of the cell contribution algorithm.
            CSSPixels size_sum_of_tracks_with_zero_intrinsic_percentage = 0;
            size_t number_of_tracks_with_zero_intrinsic_percentage = 0;
            for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                if (rows_or_columns[rc_index].intrinsic_percentage == 0) {
                    size_sum_of_tracks_with_zero_intrinsic_percentage += rows_or_columns[rc_index].max_size;
                    ++number_of_tracks_with_zero_intrinsic_percentage;
                }
            }
            for (size_t rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                // If the intrinsic percentage width of a column based on cells of span up to N-1 is greater than 0%, then the intrinsic percentage width of
                // the column based on cells of span up to N is the same as the intrinsic percentage width of the column based on cells of span up to N-1.
                if (rows_or_columns[rc_index].intrinsic_percentage > 0) {
                    continue;
                }
                // Otherwise, it is the largest of the contributions of the cells in the column whose colSpan is N,
                // where the contribution of a cell is the result of taking the following steps:
                // 1. Start with the percentage contribution of the cell.
                // 2. Subtract the intrinsic percentage width of the column based on cells of span up to N-1 of all columns
                //    that the cell spans. If this gives a negative result, change it to 0%.
                // 3. Multiply by the ratio of the column’s non-spanning max-content width to the sum of the non-spanning max-content widths of all
                //    columns spanned by the cell that have an intrinsic percentage width of the column based on cells of span up to N-1 equal to 0%.
                CSSPixels adjusted_cell_contribution;
                if (size_sum_of_tracks_with_zero_intrinsic_percentage != 0) {
                    adjusted_cell_contribution = cell_contribution.scaled(rows_or_columns[rc_index].max_size / static_cast<double>(size_sum_of_tracks_with_zero_intrinsic_percentage));
                } else {
                    // However, if this ratio is undefined because the denominator is zero, instead use the 1 divided by the number of columns
                    // spanned by the cell that have an intrinsic percentage width of the column based on cells of span up to N-1 equal to zero.
                    adjusted_cell_contribution = cell_contribution * 1 / number_of_tracks_with_zero_intrinsic_percentage;
                }
                intrinsic_percentage_contribution_by_index[rc_index] = max(static_cast<double>(adjusted_cell_contribution), intrinsic_percentage_contribution_by_index[rc_index]);
            }
        }
        for (size_t rc_index = 0; rc_index < rows_or_columns.size(); ++rc_index) {
            rows_or_columns[rc_index].intrinsic_percentage = intrinsic_percentage_contribution_by_index[rc_index];
        }
    }

    // Clamp total intrinsic percentage to 100%: https://www.w3.org/TR/css-tables-3/#intrinsic-percentage-width-of-a-column
    double total_intrinsic_percentage = 0;
    for (auto& rows_or_column : rows_or_columns) {
        rows_or_column.intrinsic_percentage = max(min(100 - total_intrinsic_percentage, rows_or_column.intrinsic_percentage), 0);
        total_intrinsic_percentage += rows_or_column.intrinsic_percentage;
    }
}

template<class RowOrColumn>
void TableFormattingContext::compute_table_measures()
{
    initialize_table_measures<RowOrColumn>();

    auto& rows_or_columns = table_rows_or_columns<RowOrColumn>();

    size_t max_cell_span = 1;
    for (auto& cell : m_cells) {
        max_cell_span = max(max_cell_span, cell_span<RowOrColumn>(cell));
    }

    // Since the intrinsic percentage specification uses non-spanning max-content size for the iterative algorithm,
    // run it before we compute the spanning max-content size with its own iterative algorithm for span up to N.
    compute_intrinsic_percentage<RowOrColumn>(max_cell_span);

    for (size_t current_span = 2; current_span <= max_cell_span; current_span++) {
        // https://www.w3.org/TR/css-tables-3/#min-content-width-of-a-column-based-on-cells-of-span-up-to-n-n--1
        Vector<Vector<CSSPixels>> cell_min_contributions_by_rc_index;
        cell_min_contributions_by_rc_index.resize(rows_or_columns.size());
        // https://www.w3.org/TR/css-tables-3/#max-content-width-of-a-column-based-on-cells-of-span-up-to-n-n--1
        Vector<Vector<CSSPixels>> cell_max_contributions_by_rc_index;
        cell_max_contributions_by_rc_index.resize(rows_or_columns.size());
        for (auto& cell : m_cells) {
            auto cell_span_value = cell_span<RowOrColumn>(cell);
            if (cell_span_value == current_span) {
                // Define the baseline max-content size as the sum of the max-content sizes based on cells of span up to N-1 of all columns that the cell spans.
                auto cell_start_rc_index = cell_index<RowOrColumn>(cell);
                auto cell_end_rc_index = cell_start_rc_index + cell_span_value;
                CSSPixels baseline_max_content_size = 0;
                for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                    baseline_max_content_size += rows_or_columns[rc_index].max_size;
                }
                CSSPixels baseline_min_content_size = 0;
                for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                    baseline_min_content_size += rows_or_columns[rc_index].min_size;
                }

                // Define the baseline border spacing as the sum of the horizontal border-spacing for any columns spanned by the cell, other than the one in which the cell originates.
                auto baseline_border_spacing = border_spacing<RowOrColumn>() * (cell_span_value - 1);

                // Add contribution from all rows / columns, since we've weighted the gap to the desired spanned size by the the
                // ratio of the max-content size based on cells of span up to N-1 of the row / column to the baseline max-content width.
                for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                    // The contribution of the cell is the sum of:
                    // the min-content size of the column based on cells of span up to N-1
                    auto cell_min_contribution = rows_or_columns[rc_index].min_size;
                    // the product of:
                    // - the ratio of:
                    //   - the max-content size of the row / column based on cells of span up to N-1 of the row / column minus the
                    //     min-content size of the row / column based on cells of span up to N-1 of the row / column, to
                    //   - the baseline max-content size minus the baseline min-content size
                    //   or zero if this ratio is undefined, and
                    // - the outer min-content size of the cell minus the baseline min-content size and the baseline border spacing, clamped
                    //   to be at least 0 and at most the difference between the baseline max-content size and the baseline min-content size
                    auto normalized_max_min_diff = baseline_max_content_size != baseline_min_content_size
                        ? (rows_or_columns[rc_index].max_size - rows_or_columns[rc_index].min_size) / static_cast<double>(baseline_max_content_size - baseline_min_content_size)
                        : 0;
                    auto clamped_diff_to_baseline_min = min(
                        max(cell_min_size<RowOrColumn>(cell) - baseline_min_content_size - baseline_border_spacing, 0),
                        baseline_max_content_size - baseline_min_content_size);
                    cell_min_contribution += CSSPixels::nearest_value_for(normalized_max_min_diff * clamped_diff_to_baseline_min);
                    // the product of:
                    // - the ratio of the max-content size based on cells of span up to N-1 of the column to the baseline max-content size
                    // - the outer min-content size of the cell minus the baseline max-content size and baseline border spacing, or 0 if this is negative
                    if (baseline_max_content_size != 0) {
                        cell_min_contribution += CSSPixels::nearest_value_for(rows_or_columns[rc_index].max_size / static_cast<double>(baseline_max_content_size))
                            * max(CSSPixels(0), cell_min_size<RowOrColumn>(cell) - baseline_max_content_size - baseline_border_spacing);
                    } else {
                        // AD-HOC: The spec does not define behavior when baseline is zero. We distribute equally.
                        //         This matches how undefined ratios are handled elsewhere.
                        cell_min_contribution += max(CSSPixels(0), cell_min_size<RowOrColumn>(cell) - baseline_border_spacing) / cell_span_value;
                    }

                    // The contribution of the cell is the sum of:
                    // the max-content size of the column based on cells of span up to N-1
                    auto cell_max_contribution = rows_or_columns[rc_index].max_size;
                    // and the product of:
                    // - the ratio of the max-content size based on cells of span up to N-1 of the column to the baseline max-content size
                    // - the outer max-content size of the cell minus the baseline max-content size and the baseline border spacing, or 0 if this is negative
                    if (baseline_max_content_size != 0) {
                        cell_max_contribution += CSSPixels::nearest_value_for(rows_or_columns[rc_index].max_size / static_cast<double>(baseline_max_content_size))
                            * max(CSSPixels(0), cell_max_size<RowOrColumn>(cell) - baseline_max_content_size - baseline_border_spacing);
                    } else {
                        // AD-HOC: The spec does not define behavior when baseline is zero. We distribute equally,
                        //         This matches how undefined ratios are handled elsewhere.
                        cell_max_contribution += max(CSSPixels(0), cell_max_size<RowOrColumn>(cell) - baseline_border_spacing) / cell_span_value;
                    }
                    cell_min_contributions_by_rc_index[rc_index].append(cell_min_contribution);
                    cell_max_contributions_by_rc_index[rc_index].append(cell_max_contribution);
                }
            }
        }

        for (size_t rc_index = 0; rc_index < rows_or_columns.size(); rc_index++) {
            // min-content size of a row / column based on cells of span up to N (N > 1) is
            // the largest of the min-content size of the row / column based on cells of span up to N-1 and
            // the contributions of the cells in the row / column whose rowSpan / colSpan is N
            for (auto min_contribution : cell_min_contributions_by_rc_index[rc_index])
                rows_or_columns[rc_index].min_size = max(rows_or_columns[rc_index].min_size, min_contribution);

            // max-content size of a row / column based on cells of span up to N (N > 1) is
            // the largest of the max-content size based on cells of span up to N-1 and the contributions of
            // the cells in the row / column whose rowSpan / colSpan is N
            for (auto max_contribution : cell_max_contributions_by_rc_index[rc_index])
                rows_or_columns[rc_index].max_size = max(rows_or_columns[rc_index].max_size, max_contribution);
        }
    }
}

CSSPixels TableFormattingContext::compute_capmin()
{
    // The caption width minimum (CAPMIN) is the largest of the table captions min-content contribution:
    // https://drafts.csswg.org/css-tables-3/#computing-the-table-width
    CSSPixels capmin = 0;
    auto table_wrapper_containing_block_inline_size = m_table_constraints.percentage_basis_inline_size.value_or(0);
    for (auto child = table_box().first_child(); child; child = child->next_sibling()) {
        auto const* child_box = as_if<Box>(*child);
        if (!child_box || !child_box->display().is_table_caption()) {
            continue;
        }
        auto const& computed_values = child_box->computed_values();

        auto margin_left = computed_values.margin().left().resolved_or_auto(table_wrapper_containing_block_inline_size).to_px_or_zero();
        auto margin_right = computed_values.margin().right().resolved_or_auto(table_wrapper_containing_block_inline_size).to_px_or_zero();
        auto padding_left = computed_values.padding().left().to_px_or_zero(table_wrapper_containing_block_inline_size);
        auto padding_right = computed_values.padding().right().to_px_or_zero(table_wrapper_containing_block_inline_size);
        auto outer_size_for_inner_size = [&](CSSPixels inner_size) {
            return inner_size
                + margin_left
                + computed_values.border_left().width
                + padding_left
                + padding_right
                + computed_values.border_right().width
                + margin_right;
        };

        auto caption_min_content_contribution = outer_size_for_inner_size(calculate_min_content_inline_size(*child_box, m_participant_constraints));
        if (!computed_values.width().is_auto() && !computed_values.width().contains_percentage()) {
            auto preferred_inner_inline_size = calculate_inner_inline_size(*child_box, AvailableSize::make_definite(table_wrapper_containing_block_inline_size), computed_values.width(), m_participant_constraints);
            caption_min_content_contribution = max(caption_min_content_contribution, outer_size_for_inner_size(preferred_inner_inline_size));
        }

        capmin = max(caption_min_content_contribution, capmin);
    }
    return capmin;
}

static bool inline_size_is_auto_or_indefinite_percentage(CSS::Size const& inline_size, bool containing_block_has_definite_inline_size)
{
    if (inline_size.is_auto())
        return true;
    if (inline_size.contains_percentage()) {
        if (!containing_block_has_definite_inline_size)
            return true;
    }
    return false;
}

void TableFormattingContext::compute_table_inline_size()
{
    // https://drafts.csswg.org/css-tables-3/#computing-the-table-width

    auto& table_box_state = m_state.get_mutable(table_box());

    auto& computed_values = table_box().computed_values();

    auto table_containing_block_inline_size = m_available_space->inline_size;

    // Percentages on 'width' and 'height' on the table are relative to the table wrapper box's containing block,
    // not the table wrapper box itself.
    CSSPixels table_wrapper_containing_block_inline_size = m_table_constraints.percentage_basis_inline_size.value_or(0);

    // Compute undistributable space due to border spacing: https://www.w3.org/TR/css-tables-3/#computing-undistributable-space.
    auto undistributable_space = (m_columns.size() + 1) * border_spacing_inline();

    // The row/column-grid inline-size minimum (GRIDMIN) is the sum of the min-content inline size
    // of all the columns plus cell spacing or borders.
    CSSPixels grid_min_inline_size = 0;
    for (auto& column : m_columns) {
        grid_min_inline_size += column.min_size;
    }
    grid_min_inline_size += undistributable_space;

    // The row/column-grid inline-size maximum (GRIDMAX) is the sum of the max-content inline size
    // of all the columns plus cell spacing or borders.
    CSSPixels grid_max_inline_size = 0;
    for (auto& column : m_columns) {
        grid_max_inline_size += column.max_size;
    }
    grid_max_inline_size += undistributable_space;

    auto resolve_inline_size_constraint_to_content_box = [&](auto const& inline_size_constraint) {
        if (inline_size_constraint.is_min_content())
            return grid_min_inline_size;
        if (inline_size_constraint.is_max_content())
            return grid_max_inline_size;
        if (inline_size_constraint.is_fit_content()) {
            auto fit_content_limit = table_wrapper_containing_block_inline_size;
            if (auto const& fit_content_available_space = inline_size_constraint.fit_content_available_space(); fit_content_available_space.has_value())
                fit_content_limit = fit_content_available_space->to_px(table_wrapper_containing_block_inline_size);
            return min(grid_max_inline_size, max(grid_min_inline_size, fit_content_limit));
        }
        // CSS Sizing says box-sizing:border-box applies length/percentage width/min-width/max-width constraints to
        // the border box. The table inline-size algorithm compares content inline sizes, so convert them before comparing.
        auto resolved_inline_size = inline_size_constraint.to_px(table_wrapper_containing_block_inline_size);
        if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox)
            resolved_inline_size -= table_box_state.border_box_left() + table_box_state.border_box_right();
        return max(CSSPixels(0), resolved_inline_size);
    };

    // The used minimum inline size of a table is the greater of the resolved min-width, CAPMIN, and GRIDMIN.
    auto used_min_inline_size = max(grid_min_inline_size, compute_capmin());
    if (!computed_values.min_width().is_auto()) {
        used_min_inline_size = max(used_min_inline_size, resolve_inline_size_constraint_to_content_box(computed_values.min_width()));
    }

    CSSPixels used_inline_size;
    if (inline_size_is_auto_or_indefinite_percentage(computed_values.width(), m_table_constraints.percentage_basis_inline_size.has_value())) {
        // If the table-root has 'width: auto', the used inline size is the greater of
        // min(GRIDMAX, the table’s containing block inline size), the used minimum inline size of the table.
        // NOTE: In normal layout the available inline size already is the wrapper's used inline size, which the
        //       parent context resolved with shrink-to-fit; filling it keeps the table and its
        //       wrapper consistent without reading the wrapper's state from here.
        if (m_available_space->inline_size.is_min_content())
            used_inline_size = grid_min_inline_size;
        else if (m_available_space->inline_size.is_max_content())
            used_inline_size = grid_max_inline_size;
        else if (table_containing_block_inline_size.is_definite() && m_layout_mode == LayoutMode::Normal)
            used_inline_size = max(table_containing_block_inline_size.to_px_or_zero(), used_min_inline_size);
        else if (table_containing_block_inline_size.is_definite())
            used_inline_size = max(min(grid_max_inline_size, table_containing_block_inline_size.to_px_or_zero()), used_min_inline_size);
        else
            used_inline_size = max(grid_max_inline_size, used_min_inline_size);
        // https://www.w3.org/TR/CSS22/tables.html#auto-table-layout
        // A percentage value for a column inline size is relative to the table inline size. If the table has
        // 'width: auto', a percentage represents a constraint on the column's inline size, which a UA should try to satisfy.
        if (!m_available_space->inline_size.is_intrinsic_sizing_constraint()) {
            for (auto& cell : m_cells) {
                auto const& cell_inline_size = cell.box.computed_values().width();
                if (cell_inline_size.is_percentage()) {
                    CSSPixels adjusted_used_inline_size = undistributable_space;
                    if (cell_inline_size.percentage().value() != 0)
                        adjusted_used_inline_size += CSSPixels::nearest_value_for(ceil(100 / cell_inline_size.percentage().value() * cell.outer_max_inline_size));

                    if (table_containing_block_inline_size.is_definite())
                        used_inline_size = min(max(used_inline_size, adjusted_used_inline_size), table_containing_block_inline_size.to_px_or_zero());
                    else
                        used_inline_size = max(used_inline_size, adjusted_used_inline_size);
                }
            }
        }
    } else if (computed_values.width().is_max_content()) {
        used_inline_size = grid_max_inline_size;
    } else {
        // If the table-root’s width property has a computed value (resolving to the table inline size) other than auto,
        // the used inline size is the greater of the resolved table inline size and the used minimum inline size.
        CSSPixels resolved_table_inline_size = resolve_inline_size_constraint_to_content_box(computed_values.width());
        used_inline_size = max(resolved_table_inline_size, used_min_inline_size);
        if (!should_treat_max_inline_size_as_none(table_box(), m_available_space->inline_size, m_table_constraints))
            used_inline_size = min(used_inline_size, resolve_inline_size_constraint_to_content_box(computed_values.max_width()));
    }

    if (!should_treat_max_inline_size_as_none(table_box(), m_available_space->inline_size, m_table_constraints))
        used_inline_size = min(used_inline_size, resolve_inline_size_constraint_to_content_box(computed_values.max_width()));
    used_inline_size = max(used_inline_size, used_min_inline_size);

    table_box_state.set_content_inline_size(used_inline_size);
}

CSSPixels TableFormattingContext::compute_columns_total_used_inline_size() const
{
    CSSPixels total_used_inline_size = 0;
    for (auto& column : m_columns) {
        total_used_inline_size += column.used_inline_size;
    }
    return total_used_inline_size;
}

static CSSPixels compute_columns_total_candidate_inline_size(Vector<CSSPixels> const& candidate_inline_sizes)
{
    CSSPixels total_candidate_inline_size = 0;
    for (auto inline_size : candidate_inline_sizes) {
        total_candidate_inline_size += inline_size;
    }
    return total_candidate_inline_size;
}

void TableFormattingContext::commit_candidate_column_inline_sizes(Vector<CSSPixels> const& candidate_inline_sizes)
{
    VERIFY(candidate_inline_sizes.size() == m_columns.size());
    for (size_t i = 0; i < m_columns.size(); ++i) {
        m_columns[i].used_inline_size = candidate_inline_sizes[i];
    }
}

void TableFormattingContext::assign_columns_inline_size_linear_combination(Vector<CSSPixels> const& candidate_inline_sizes, CSSPixels available_inline_size)
{
    auto columns_total_candidate_inline_size = compute_columns_total_candidate_inline_size(candidate_inline_sizes);
    auto columns_total_used_inline_size = compute_columns_total_used_inline_size();
    if (columns_total_candidate_inline_size == columns_total_used_inline_size) {
        return;
    }
    auto candidate_weight = (available_inline_size - columns_total_used_inline_size) / static_cast<double>(columns_total_candidate_inline_size - columns_total_used_inline_size);
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        column.used_inline_size = CSSPixels::nearest_value_for(candidate_weight * candidate_inline_sizes[i] + (1 - candidate_weight) * column.used_inline_size);
    }
}

template<class ColumnFilter, class BaseInlineSizeGetter>
bool TableFormattingContext::distribute_excess_inline_size_proportionally_to_base_inline_size(CSSPixels excess_inline_size, ColumnFilter column_filter, BaseInlineSizeGetter base_inline_size_getter)
{
    bool found_matching_columns = false;
    CSSPixels total_base_inline_size = 0;
    for (auto const& column : m_columns) {
        if (column_filter(column)) {
            total_base_inline_size += base_inline_size_getter(column);
            found_matching_columns = true;
        }
    }
    if (!found_matching_columns) {
        return false;
    }
    VERIFY(total_base_inline_size > 0);
    for (auto& column : m_columns) {
        if (column_filter(column)) {
            column.used_inline_size += CSSPixels::nearest_value_for(excess_inline_size * base_inline_size_getter(column) / static_cast<double>(total_base_inline_size));
        }
    }
    return true;
}

template<class ColumnFilter>
bool TableFormattingContext::distribute_excess_inline_size_equally(CSSPixels excess_inline_size, ColumnFilter column_filter)
{
    size_t matching_column_count = 0;
    for (auto const& column : m_columns) {
        if (column_filter(column)) {
            ++matching_column_count;
        }
    }
    if (matching_column_count == 0) {
        return false;
    }
    for (auto& column : m_columns) {
        if (column_filter(column)) {
            column.used_inline_size += excess_inline_size / matching_column_count;
        }
    }
    return matching_column_count;
}

template<class ColumnFilter>
bool TableFormattingContext::distribute_excess_inline_size_by_intrinsic_percentage(CSSPixels excess_inline_size, ColumnFilter column_filter)
{
    bool found_matching_columns = false;
    double total_intrinsic_percentage = 0;
    for (auto const& column : m_columns) {
        if (column_filter(column)) {
            found_matching_columns = true;
            total_intrinsic_percentage += column.intrinsic_percentage;
        }
    }
    if (!found_matching_columns) {
        return false;
    }
    for (auto& column : m_columns) {
        if (column_filter(column)) {
            column.used_inline_size += CSSPixels::nearest_value_for(excess_inline_size * column.intrinsic_percentage / total_intrinsic_percentage);
        }
    }
    return true;
}

void TableFormattingContext::distribute_inline_size_to_columns()
{
    // Implements https://www.w3.org/TR/css-tables-3/#width-distribution-algorithm

    // The total inline-axis border spacing is defined for each table:
    // - For tables laid out in separated-borders mode containing at least one column, the inline-axis component of the computed value of the border-spacing property times one plus the number of columns in the table
    // - Otherwise, 0
    auto total_inline_border_spacing = m_columns.is_empty() ? 0 : (m_columns.size() + 1) * border_spacing_inline();

    // The assignable table inline size is its used inline size minus the inline-axis border spacing.
    CSSPixels const available_inline_size = m_state.get(table_box()).content_inline_size() - total_inline_border_spacing;

    Vector<CSSPixels> candidate_inline_sizes;
    candidate_inline_sizes.resize(m_columns.size());

    // 1. The min-content sizing guess assigns every column its min-content inline size.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        // In fixed mode, the min-content width of percent-columns and auto-columns is considered to be zero:
        // https://www.w3.org/TR/css-tables-3/#width-distribution-in-fixed-mode
        if (use_fixed_mode_layout() && !column.is_constrained) {
            continue;
        }
        column.used_inline_size = column.min_size;
        candidate_inline_sizes[i] = column.min_size;
    }

    // 2. The min-content-percentage sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width.
    //    - all other columns are assigned their min-content width.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        if (column.has_intrinsic_percentage) {
            candidate_inline_sizes[i] = max(column.min_size, CSSPixels::nearest_value_for(column.intrinsic_percentage / 100 * available_inline_size));
        }
    }

    // If the assignable inline size is no larger than the max-content sizing guess, use the linear combination
    // of the consecutive sizing guesses whose sums bound the available inline size.
    if (available_inline_size < compute_columns_total_candidate_inline_size(candidate_inline_sizes)) {
        assign_columns_inline_size_linear_combination(candidate_inline_sizes, available_inline_size);
        return;
    } else {
        commit_candidate_column_inline_sizes(candidate_inline_sizes);
    }

    // 3. The min-content-specified sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width
    //    - any other column that is constrained is assigned its max-content width
    //    - all other columns are assigned their min-content width.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        if (column.is_constrained) {
            candidate_inline_sizes[i] = column.max_size;
        }
    }

    if (available_inline_size < compute_columns_total_candidate_inline_size(candidate_inline_sizes)) {
        assign_columns_inline_size_linear_combination(candidate_inline_sizes, available_inline_size);
        return;
    } else {
        commit_candidate_column_inline_sizes(candidate_inline_sizes);
    }

    // 4. The max-content sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width
    //    - all other columns are assigned their max-content width.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        if (!column.has_intrinsic_percentage) {
            candidate_inline_sizes[i] = column.max_size;
        }
    }

    if (available_inline_size < compute_columns_total_candidate_inline_size(candidate_inline_sizes)) {
        assign_columns_inline_size_linear_combination(candidate_inline_sizes, available_inline_size);
        return;
    } else {
        commit_candidate_column_inline_sizes(candidate_inline_sizes);
    }

    // Otherwise, start from the max-content sizing guess and distribute the excess inline size to the columns.
    distribute_excess_inline_size_to_columns(available_inline_size);
}

void TableFormattingContext::distribute_excess_inline_size_to_columns(CSSPixels available_inline_size)
{
    // Implements https://www.w3.org/TR/css-tables-3/#distributing-width-to-columns
    auto columns_total_used_inline_size = compute_columns_total_used_inline_size();
    if (columns_total_used_inline_size >= available_inline_size) {
        return;
    }
    auto excess_inline_size = available_inline_size - columns_total_used_inline_size;
    if (excess_inline_size == 0) {
        return;
    }

    if (use_fixed_mode_layout()) {
        distribute_excess_inline_size_to_columns_fixed_mode(excess_inline_size);
        return;
    }

    // 1. If there are non-constrained columns that have originating cells with intrinsic percentage width of 0% and with nonzero
    //    max-content width (aka the columns allowed to grow by this rule), the distributed widths of the columns allowed to grow
    //    by this rule are increased in proportion to max-content width so the total increase adds to the excess width.
    if (distribute_excess_inline_size_proportionally_to_base_inline_size(
            excess_inline_size,
            [](auto const& column) {
                return !column.is_constrained && column.has_originating_cells && column.intrinsic_percentage == 0 && column.max_size > 0;
            },
            [](auto const& column) { return column.max_size; })) {
        excess_inline_size = available_inline_size - compute_columns_total_used_inline_size();
    }
    if (excess_inline_size == 0) {
        return;
    }
    // 2. Otherwise, if there are non-constrained columns that have originating cells with intrinsic percentage width of 0% (aka the columns
    //    allowed to grow by this rule, which thanks to the previous rule must have zero max-content width), the distributed widths of the
    //    columns allowed to grow by this rule are increased by equal amounts so the total increase adds to the excess width.
    if (distribute_excess_inline_size_equally(excess_inline_size,
            [](auto const& column) {
                return !column.is_constrained && column.has_originating_cells && column.intrinsic_percentage == 0;
            })) {
        excess_inline_size = available_inline_size - compute_columns_total_used_inline_size();
    }
    if (excess_inline_size == 0) {
        return;
    }
    // 3. Otherwise, if there are constrained columns with intrinsic percentage width of 0% and with nonzero max-content width
    //    (aka the columns allowed to grow by this rule, which, due to other rules, must have originating cells), the distributed widths of the
    //    columns allowed to grow by this rule are increased in proportion to max-content width so the total increase adds to the excess width.
    if (distribute_excess_inline_size_proportionally_to_base_inline_size(
            excess_inline_size,
            [](auto const& column) {
                return column.is_constrained && column.intrinsic_percentage == 0 && column.max_size > 0;
            },
            [](auto const& column) { return column.max_size; })) {
        excess_inline_size = available_inline_size - compute_columns_total_used_inline_size();
    }
    if (excess_inline_size == 0) {
        return;
    }
    // 4. Otherwise, if there are columns with intrinsic percentage width greater than 0% (aka the columns allowed to grow by this rule,
    //    which, due to other rules, must have originating cells), the distributed widths of the columns allowed to grow by this rule are
    //    increased in proportion to intrinsic percentage width so the total increase adds to the excess width.
    if (distribute_excess_inline_size_by_intrinsic_percentage(excess_inline_size, [](auto const& column) {
            return column.intrinsic_percentage > 0;
        })) {
        excess_inline_size = available_inline_size - compute_columns_total_used_inline_size();
    }
    if (excess_inline_size == 0) {
        return;
    }
    // 5. Otherwise, if there is any such column, the distributed widths of all columns that have originating cells are increased by equal amounts
    //    so the total increase adds to the excess width.
    if (distribute_excess_inline_size_equally(
            excess_inline_size,
            [](auto const& column) { return column.has_originating_cells; })) {
        excess_inline_size = available_inline_size - compute_columns_total_used_inline_size();
    }
    if (excess_inline_size == 0) {
        return;
    }
    // 6. Otherwise, the distributed widths of all columns are increased by equal amounts so the total increase adds to the excess width.
    distribute_excess_inline_size_equally(excess_inline_size, [](auto const&) { return true; });
}

void TableFormattingContext::distribute_excess_inline_size_to_columns_fixed_mode(CSSPixels excess_inline_size)
{
    // Implements the fixed mode for https://www.w3.org/TR/css-tables-3/#distributing-width-to-columns.

    // If any columns have no specified inline size, distribute the excess inline size equally to them.
    if (distribute_excess_inline_size_equally(excess_inline_size, [](auto const& column) { return !column.is_constrained && !column.has_intrinsic_percentage; })) {
        return;
    }
    // Otherwise, distribute proportionally among columns with non-zero length sizes from the base assignment.
    if (distribute_excess_inline_size_proportionally_to_base_inline_size(
            excess_inline_size, [](auto const& column) { return column.used_inline_size > 0; }, [](auto const& column) { return column.used_inline_size; })) {
        return;
    }
    // Otherwise, distribute proportionally among columns with non-zero percentage sizes from the base assignment.
    if (distribute_excess_inline_size_by_intrinsic_percentage(excess_inline_size, [](auto const& column) { return column.intrinsic_percentage > 0; })) {
        return;
    }
    // Otherwise, distribute the excess inline size equally to the zero-sized columns.
    distribute_excess_inline_size_equally(excess_inline_size, [](auto const& column) { return column.used_inline_size == 0; });
}

bool TableFormattingContext::can_skip_row_intrinsic_measurement() const
{
    for (auto const& row : m_rows) {
        if (row.is_collapsed)
            return false;

        auto const& computed_values = row.box.computed_values();
        if (!computed_values.height().is_auto()
            || !computed_values.min_height().is_auto()
            || !computed_values.max_height().is_none()) {
            return false;
        }
    }

    for (auto const& cell : m_cells) {
        if (cell.row_span != 1)
            return false;

        auto const& computed_values = cell.box.computed_values();
        if (!computed_values.height().is_auto()
            || !computed_values.min_height().is_auto()
            || !computed_values.max_height().is_none()) {
            return false;
        }
    }

    return true;
}

static bool cell_block_size_depends_on_table_block_size(Box const& cell_box)
{
    return cell_box.computed_values().height().is_percentage();
}

Optional<TableFormattingContext::MeasuredCellContent> TableFormattingContext::measure_cell_content(Box const& cell_box, LayoutState::UsedValues const& cell_used_values, AvailableSpace const& inner_available_space)
{
    if (m_layout_mode == LayoutMode::IntrinsicSizing
        && !cell_box.is_inline()
        && cell_used_values.inline_size_constraint == SizeConstraint::None
        && cell_used_values.block_size_constraint == SizeConstraint::None
        && cell_used_values.has_definite_inline_size()
        && cell_used_values.has_definite_block_size()) {
        return {};
    }
    if (!cell_box.can_have_children())
        return {};

    LayoutState throwaway_state(cell_box, LayoutState::Purpose::Measurement);
    auto& throwaway_cell_used_values = throwaway_state.create(cell_box, {}, {});

    // The table formatting context owns the cell's outer geometry. Seed the inputs
    // needed to lay out its contents without copying placement or layout outputs.
    throwaway_cell_used_values.inline_size_constraint = cell_used_values.inline_size_constraint;
    throwaway_cell_used_values.block_size_constraint = cell_used_values.block_size_constraint;
    throwaway_cell_used_values.set_content_inline_size(cell_used_values.content_inline_size());
    throwaway_cell_used_values.set_content_block_size(cell_used_values.content_block_size());
    throwaway_cell_used_values.set_has_definite_inline_size(cell_used_values.has_definite_inline_size());
    throwaway_cell_used_values.set_has_definite_block_size(cell_used_values.has_definite_block_size());

    throwaway_cell_used_values.margin_left = cell_used_values.margin_left;
    throwaway_cell_used_values.margin_right = cell_used_values.margin_right;
    throwaway_cell_used_values.margin_top = cell_used_values.margin_top;
    throwaway_cell_used_values.margin_bottom = cell_used_values.margin_bottom;

    throwaway_cell_used_values.border_left = cell_used_values.border_left;
    throwaway_cell_used_values.border_right = cell_used_values.border_right;
    throwaway_cell_used_values.border_top = cell_used_values.border_top;
    throwaway_cell_used_values.border_bottom = cell_used_values.border_bottom;

    throwaway_cell_used_values.padding_left = cell_used_values.padding_left;
    throwaway_cell_used_values.padding_right = cell_used_values.padding_right;
    throwaway_cell_used_values.padding_top = cell_used_values.padding_top;
    throwaway_cell_used_values.padding_bottom = cell_used_values.padding_bottom;

    if (auto const& override_borders_data = cell_used_values.override_borders_data(); override_borders_data.has_value())
        throwaway_cell_used_values.set_override_borders_data(override_borders_data.value());

    auto measuring_context = create_independent_formatting_context_if_needed(throwaway_state, m_layout_mode, cell_box, this);
    if (!measuring_context)
        return {};
    measuring_context->run(LayoutInput { inner_available_space });

    auto content_block_size = measuring_context->automatic_content_block_size();
    throwaway_cell_used_values.set_content_block_size(content_block_size);
    return MeasuredCellContent {
        .content_block_size = content_block_size,
        .first_baseline = measuring_context->box_baseline(cell_box, BaselineSet::First),
    };
}

void TableFormattingContext::compute_table_block_size()
{
    // First pass of row block-size calculation:
    for (auto& row : m_rows) {
        if (row.is_collapsed) {
            row.base_block_size = 0;
            continue;
        }
        auto row_computed_block_size = row.box.computed_values().height();
        if (row_computed_block_size.is_length()) {
            // NOTE: A <length> block size resolves without a percentage basis.
            auto row_used_block_size = row_computed_block_size.to_px(0);
            row.base_block_size = max(row.base_block_size, row_used_block_size);
        }
    }

    // First pass of cells layout:
    for (auto& cell : m_cells) {
        auto& row = m_rows[cell.row_index];
        auto& cell_state = m_state.get_mutable(cell.box);

        CSSPixels span_inline_size = 0;
        for (size_t i = 0; i < cell.column_span; ++i)
            span_inline_size += m_columns[cell.column_index + i].used_inline_size;

        auto containing_block_inline_size = m_participant_constraints.percentage_basis_inline_size.value_or(0);
        auto containing_block_block_size = m_participant_constraints.percentage_basis_block_size.value_or(0);

        cell_state.padding_top = cell.box.computed_values().padding().top().to_px_or_zero(containing_block_inline_size);
        cell_state.padding_bottom = cell.box.computed_values().padding().bottom().to_px_or_zero(containing_block_inline_size);
        cell_state.padding_left = cell.box.computed_values().padding().left().to_px_or_zero(containing_block_inline_size);
        cell_state.padding_right = cell.box.computed_values().padding().right().to_px_or_zero(containing_block_inline_size);

        if (table_box().computed_values().border_collapse() == CSS::BorderCollapse::Separate) {
            cell_state.border_top = cell.box.computed_values().border_top().width;
            cell_state.border_bottom = cell.box.computed_values().border_bottom().width;
            cell_state.border_left = cell.box.computed_values().border_left().width;
            cell_state.border_right = cell.box.computed_values().border_right().width;
        }

        if (!row.is_collapsed) {
            auto cell_computed_block_size = cell.box.computed_values().height();
            if (cell_computed_block_size.is_length()) {
                auto cell_used_block_size = cell_computed_block_size.to_px(containing_block_block_size);
                cell_state.set_content_block_size(cell_used_block_size - cell_state.border_box_top() - cell_state.border_box_bottom());

                row.base_block_size = max(row.base_block_size, cell_used_block_size);
            }
        }

        // Compute cell inline size as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
        // The position of any table cell, track, or track group is defined by the sums of its spanned columns and rows:
        // - the inline/block sizes of all spanned visible columns/rows
        // - the inline/block border spacing times the amount of spanned visible columns/rows minus one
        // FIXME: Account for visibility.
        cell_state.set_content_inline_size(span_inline_size - cell_state.border_box_left() - cell_state.border_box_right() + (cell.column_span - 1) * border_spacing_inline());
        Optional<CSSPixels> measured_baseline;
        if (cell_block_size_depends_on_table_block_size(cell.box)) {
            // This cell's final inside layout happens in the second pass below; measure its
            // content in a throwaway state instead of laying out the committing state twice.
            if (auto measured = measure_cell_content(cell.box, cell_state, cell_state.available_inner_space_or_constraints_from(*m_available_space)); measured.has_value()) {
                cell_state.set_content_block_size(measured->content_block_size);
                measured_baseline = measured->first_baseline;
            }
        } else if (auto independent_formatting_context = layout_inside(cell.box, m_layout_mode, LayoutInput { cell_state.available_inner_space_or_constraints_from(*m_available_space) })) {
            cell_state.set_content_block_size(independent_formatting_context->automatic_content_block_size());
            independent_formatting_context->parent_context_did_dimension_child_root_box();
        }
        if (m_needs_fixed_mode_row_measurement) {
            auto const& computed_values = cell.box.computed_values();
            auto min_block_size = computed_values.min_height().to_px(containing_block_block_size);
            auto cell_intrinsic_block_size_offsets = cell_state.border_box_top() + cell_state.border_box_bottom();
            auto measured_outer_block_size = max(cell_state.border_box_block_size(), min_block_size + cell_intrinsic_block_size_offsets);
            cell.outer_min_block_size = measured_outer_block_size;
            cell.outer_max_block_size = measured_outer_block_size;
        }

        // https://drafts.csswg.org/css2/#height-layout
        // The baseline of a cell is the baseline of the first in-flow line box in the cell, or the first in-flow
        // table-row in the cell, whichever comes first.
        cell.baseline = measured_baseline.value_or_lazy_evaluated([&] { return box_baseline(cell.box, BaselineSet::First); });

        // Implements https://www.w3.org/TR/css-tables-3/#computing-the-table-height

        // The minimum block size of a row is the maximum of:
        // - the computed block size (if definite, percentages being considered 0px) of its corresponding table row,
        // - the computed block size of each cell spanning the current row exclusively (if definite, percentages being treated as 0px), and
        // - the minimum block size (ROWMIN) required by the cells spanning the row.
        // Note that we've already applied the first rule at the top of the method.
        if (!row.is_collapsed) {
            if (cell.row_span == 1) {
                row.base_block_size = max(row.base_block_size, cell_state.border_box_block_size());
            }
            if (!m_needs_fixed_mode_row_measurement)
                row.base_block_size = max(row.base_block_size, m_rows[cell.row_index].min_size);
            row.baseline = max(row.baseline, cell.baseline);
        }
    }

    if (m_needs_fixed_mode_row_measurement) {
        initialize_row_content_sizes();
        compute_table_measures<Row>();
        for (auto& row : m_rows) {
            if (!row.is_collapsed)
                row.base_block_size = max(row.base_block_size, row.min_size);
        }
    }

    CSSPixels sum_base_block_sizes = 0;
    for (auto& row : m_rows) {
        sum_base_block_sizes += row.base_block_size;
    }

    m_table_block_size = sum_base_block_sizes;

    if (m_min_border_box_block_size_from_flex_item.has_value()) {
        auto const& table_state = m_state.get(table_box());
        auto min_content_block_size = *m_min_border_box_block_size_from_flex_item - table_state.border_box_top() - table_state.border_box_bottom();
        m_table_block_size = max(m_table_block_size, min_content_block_size);
    }

    if (!table_box().computed_values().height().is_auto()) {
        // If the table has a `height` property other than auto, it is treated as a minimum block size for the
        // table grid, and will eventually be distributed to the rows if their collective minimum block size is smaller.
        CSSPixels table_containing_block_block_size = m_table_constraints.percentage_basis_block_size.value_or(0);
        auto specified_table_block_size = table_box().computed_values().height().to_px(table_containing_block_block_size);
        if (table_box().computed_values().box_sizing() == CSS::BoxSizing::BorderBox) {
            auto const& table_state = m_state.get(table_box());
            specified_table_block_size -= table_state.border_box_top() + table_state.border_box_bottom();
        }
        m_table_block_size = max(m_table_block_size, specified_table_block_size);
    }

    for (auto& row : m_rows) {
        // Reference size is the largest of
        // - its initial base block size and
        // - its new base block size (the one evaluated during the second layout pass, where percentages used in
        //   row groups, rows, and cells were resolved according to the table block size, instead of
        //   being ignored as 0px).

        // Assign reference size to base size. Later, the reference size might change to a larger value during
        // the second pass of rows layout.
        row.reference_block_size = row.base_block_size;
    }

    // Second pass of row block-size calculation:
    // At this point, percentage row block sizes can be resolved because the final table block size is calculated.
    for (auto& row : m_rows) {
        if (row.is_collapsed) {
            row.reference_block_size = 0;
            continue;
        }
        auto row_computed_block_size = row.box.computed_values().height();
        if (row_computed_block_size.is_percentage()) {
            auto row_used_block_size = row_computed_block_size.to_px(m_table_block_size);
            row.reference_block_size = max(row.reference_block_size, row_used_block_size);
        } else {
            continue;
        }
    }

    // Second pass cells layout:
    // At this point, percentage cell block sizes can be resolved because the final table block size is calculated.
    for (auto& cell : m_cells) {
        auto& row = m_rows[cell.row_index];
        auto& cell_state = m_state.get_mutable(cell.box);

        CSSPixels span_inline_size = 0;
        for (size_t i = 0; i < cell.column_span; ++i)
            span_inline_size += m_columns[cell.column_index + i].used_inline_size;

        if (!cell_block_size_depends_on_table_block_size(cell.box))
            continue;

        auto cell_used_block_size = cell.box.computed_values().height().to_px(m_table_block_size);
        cell_state.set_content_block_size(cell_used_block_size - cell_state.border_box_top() - cell_state.border_box_bottom());

        if (!row.is_collapsed)
            row.reference_block_size = max(row.reference_block_size, cell_used_block_size);

        cell_state.set_content_inline_size(span_inline_size - cell_state.border_box_left() - cell_state.border_box_right() + (cell.column_span - 1) * border_spacing_inline());
        // The first pass only measured this cell in a throwaway state; this is its one and
        // only inside layout in the committing state.
        if (auto independent_formatting_context = layout_inside(cell.box, m_layout_mode, LayoutInput { cell_state.available_inner_space_or_constraints_from(*m_available_space) })) {
            independent_formatting_context->parent_context_did_dimension_child_root_box();
        }

        cell.baseline = box_baseline(cell.box, BaselineSet::First);

        if (!row.is_collapsed) {
            row.reference_block_size = max(row.reference_block_size, cell_state.border_box_block_size());
            row.baseline = max(row.baseline, cell.baseline);
        }
    }
}

void TableFormattingContext::distribute_block_size_to_rows()
{
    CSSPixels sum_reference_block_sizes = 0;
    size_t number_of_visible_rows = 0;
    for (auto& row : m_rows) {
        sum_reference_block_sizes += row.reference_block_size;
        if (!row.is_collapsed)
            ++number_of_visible_rows;
    }

    if (sum_reference_block_sizes == 0)
        return;

    Vector<Row&> rows_with_auto_block_size;
    for (auto& row : m_rows) {
        if (row.box.computed_values().height().is_auto() && !row.is_collapsed) {
            rows_with_auto_block_size.append(row);
        }
    }

    if (m_table_block_size <= sum_reference_block_sizes) {
        // If the table block size is no larger than the sum of reference sizes, each final row block size is the
        // weighted mean of the base and reference sizes that yields the correct total block size.

        for (auto& row : m_rows) {
            if (row.is_collapsed) {
                row.final_block_size = 0;
                continue;
            }
            auto weight = row.reference_block_size / static_cast<double>(sum_reference_block_sizes);
            auto final_block_size = m_table_block_size * weight;
            row.final_block_size = CSSPixels::nearest_value_for(final_block_size);
        }
    } else if (rows_with_auto_block_size.size() > 0) {
        // Else, if the table owns any auto-block-size row, each non-auto row receives its reference block size and
        // auto rows receive their reference size plus an equal share of the missing table block size.

        for (auto& row : m_rows) {
            row.final_block_size = row.reference_block_size;
        }

        auto auto_block_size_rows_increment = (m_table_block_size - sum_reference_block_sizes) / rows_with_auto_block_size.size();
        for (auto& row : rows_with_auto_block_size) {
            row.final_block_size += auto_block_size_rows_increment;
        }
    } else {
        // Else, all rows receive their reference size plus an equal share of the missing table block size.

        auto increment = (m_table_block_size - sum_reference_block_sizes) / number_of_visible_rows;
        for (auto& row : m_rows) {
            if (row.is_collapsed) {
                row.final_block_size = 0;
                continue;
            }
            row.final_block_size = row.reference_block_size + increment;
        }
    }

    // Add undistributable space due to border spacing: https://www.w3.org/TR/css-tables-3/#computing-undistributable-space.
    m_table_block_size += (number_of_visible_rows + 1) * border_spacing_block();
}

void TableFormattingContext::position_row_boxes()
{
    auto const& table_state = m_state.get(table_box());

    CSSPixels row_block_offset = m_pending_table_box_content_offset_in_wrapper.block_offset + border_spacing_block();
    CSSPixels row_inline_offset = table_state.border_left + table_state.padding_left + border_spacing_inline();
    for (size_t row_index = 0; row_index < m_rows.size(); row_index++) {
        auto& row = m_rows[row_index];
        auto& row_state = m_state.get_mutable(row.box);
        CSSPixels row_inline_size = 0;
        for (auto& column : m_columns) {
            row_inline_size += column.used_inline_size;
        }
        if (m_columns.size() >= 2)
            row_inline_size += (m_columns.size() - 1) * border_spacing_inline();

        row_state.set_content_block_size(row.final_block_size);
        row_state.set_content_inline_size(row_inline_size);
        place_child(row.box, { row_inline_offset, row_block_offset });
        if (!row.is_collapsed)
            row_block_offset += row_state.content_block_size() + border_spacing_block();
    }

    CSSPixels row_group_block_offset = m_pending_table_box_content_offset_in_wrapper.block_offset + border_spacing_block();
    CSSPixels row_group_inline_offset = table_state.border_left + table_state.padding_left + border_spacing_inline();
    TableGrid::for_each_child_box_matching(table_box(), TableGrid::is_table_row_group, [&](auto& row_group_box) {
        CSSPixels row_group_block_size = 0;
        CSSPixels row_group_inline_size = 0;

        auto& row_group_box_state = m_state.get_mutable(row_group_box);

        int num_rows = 0;
        TableGrid::for_each_child_box_matching(row_group_box, TableGrid::is_table_row, [&](auto& row) {
            auto const& row_state = m_state.get(row);
            row_group_block_size += row_state.border_box_block_size();
            row_group_inline_size = max(row_group_inline_size, row_state.border_box_inline_size());
            num_rows += 1;
        });
        if (num_rows >= 2)
            row_group_block_size += (num_rows - 1) * border_spacing_block();

        row_group_box_state.set_content_block_size(row_group_block_size);
        row_group_box_state.set_content_inline_size(row_group_inline_size);
        place_child(row_group_box, { row_group_inline_offset, row_group_block_offset });

        row_group_block_offset += row_group_block_size + (num_rows > 0 ? border_spacing_block() : 0);
    });

    auto total_content_block_size = max(row_block_offset, row_group_block_offset) - m_pending_table_box_content_offset_in_wrapper.block_offset - table_state.padding_top;
    m_table_block_size = max(total_content_block_size, m_table_block_size);
}

void TableFormattingContext::position_cell_boxes()
{
    CSSPixels column_inline_offset = 0;
    for (auto& column : m_columns) {
        column.inline_offset = column_inline_offset;
        column_inline_offset += column.used_inline_size;
    }

    // When a table cell is an anonymous wrapper around a flex or grid container (e.g., a <td> with display:flex is
    // wrapped in an anonymous table-cell box per CSS Tables 3), the cell should be aligned to the top. This allows
    // the flex/grid container to fill the cell and handle alignment of its children via its own properties.
    auto cell_is_anonymous_wrapper_for_flex_or_grid = [](auto const& cell) {
        if (!cell.box.is_anonymous())
            return false;
        auto child = cell.box.first_child();
        if (!child || child->next_sibling())
            return false;
        auto const* child_with_style = as_if<NodeWithStyle>(*child);
        if (!child_with_style)
            return false;
        auto const& display = child_with_style->display();
        return display.is_flex_inside() || display.is_grid_inside();
    };

    for (auto& cell : m_cells) {
        auto& cell_state = m_state.get_mutable(cell.box);
        auto& row_state = m_state.get(m_rows[cell.row_index].box);
        auto const row_content_block_size = compute_row_content_block_size(cell);
        auto const& vertical_align = cell.box.computed_values().vertical_align();
        // The following image shows various alignment lines of a row:
        // https://www.w3.org/TR/css-tables-3/images/cell-align-explainer.png
        // https://drafts.csswg.org/css2/#height-layout
        // In the context of tables, values for vertical-align have the following meanings:
        if (cell_is_anonymous_wrapper_for_flex_or_grid(cell)) {
            cell_state.padding_bottom += row_content_block_size - cell_state.border_box_block_size();
        } else if (vertical_align.has<CSS::VerticalAlign>()) {
            switch (vertical_align.get<CSS::VerticalAlign>()) {
            // The center of the cell is aligned with the center of the rows it spans.
            case CSS::VerticalAlign::Middle: {
                auto const block_size_difference = row_content_block_size - cell_state.border_box_block_size();
                cell_state.padding_top += block_size_difference / 2;
                cell_state.padding_bottom += block_size_difference / 2;
                break;
            }
            // The top of the cell box is aligned with the top of the first row it spans.
            case CSS::VerticalAlign::Top: {
                cell_state.padding_bottom += row_content_block_size - cell_state.border_box_block_size();
                break;
            }
            // The bottom of the cell box is aligned with the bottom of the last row it spans.
            case CSS::VerticalAlign::Bottom: {
                cell_state.padding_top += row_content_block_size - cell_state.border_box_block_size();
                break;
            }
            // These values do not apply to cells; the cell is aligned at the baseline instead.
            case CSS::VerticalAlign::Sub:
            case CSS::VerticalAlign::Super:
            case CSS::VerticalAlign::TextBottom:
            case CSS::VerticalAlign::TextTop:
            // The baseline of the cell is put at the same height as the baseline of the first of the rows it spans.
            case CSS::VerticalAlign::Baseline: {
                cell_state.padding_top += m_rows[cell.row_index].baseline - cell.baseline;
                cell_state.padding_bottom += row_content_block_size - cell_state.border_box_block_size();
                break;
            }
            default:
                VERIFY_NOT_REACHED();
            }
        }

        // Compute cell position as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
        // left/top location is the sum of:
        // - for top: the height reserved for top captions (including margins), if any
        // - the padding-left/padding-top and border-left-width/border-top-width of the table
        // FIXME: Account for visibility.
        auto cell_offset = row_state.content_offset().translated(
            cell_state.border_box_left() + m_columns[cell.column_index].inline_offset + cell.column_index * border_spacing_inline(),
            cell_state.border_box_top());
        place_child(cell.box, cell_offset);
    }
}

bool TableFormattingContext::use_fixed_mode_layout() const
{
    // Implements https://www.w3.org/TR/css-tables-3/#in-fixed-mode.
    // A table-root is said to be laid out in fixed mode whenever the computed value of the table-layout property is equal to fixed, and the
    // specified width of the table root is either a <length-percentage>, min-content or fit-content. When the specified width is not one of
    // those values, or if the computed value of the table-layout property is auto, then the table-root is said to be laid out in auto mode.
    auto const& inline_size = table_box().computed_values().width();
    return table_box().computed_values().table_layout() == CSS::TableLayout::Fixed && (inline_size.is_length() || inline_size.is_percentage() || inline_size.is_min_content() || inline_size.is_fit_content());
}

static unsigned line_style_score(CSS::LineStyle line_style)
{
    switch (line_style) {
    case CSS::LineStyle::Inset:
        return 0;
    case CSS::LineStyle::Groove:
        return 1;
    case CSS::LineStyle::Outset:
        return 2;
    case CSS::LineStyle::Ridge:
        return 3;
    case CSS::LineStyle::Dotted:
        return 4;
    case CSS::LineStyle::Dashed:
        return 5;
    case CSS::LineStyle::Solid:
        return 6;
    case CSS::LineStyle::Double:
        return 7;
    default:
        VERIFY_NOT_REACHED();
    }
}

bool TableFormattingContext::border_is_less_specific(CSS::BorderData const& a, CSS::BorderData const& b)
{
    // Implements criteria for steps 1, 2 and 3 of border conflict resolution algorithm, as described in
    // https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution.

    // 1. Borders with the 'border-style' of 'hidden' take precedence over all other conflicting borders. Any border with this
    //    value suppresses all borders at this location.
    if (a.line_style == CSS::LineStyle::Hidden) {
        return false;
    }

    if (b.line_style == CSS::LineStyle::Hidden) {
        return true;
    }

    // 2. Borders with a style of 'none' have the lowest priority. Only if the border properties of all the elements meeting
    //    at this edge are 'none' will the border be omitted (but note that 'none' is the default value for the border style.)
    if (a.line_style == CSS::LineStyle::None) {
        return true;
    }
    if (b.line_style == CSS::LineStyle::None) {
        return false;
    }
    // 3. If none of the styles are 'hidden' and at least one of them is not 'none', then narrow borders are discarded in favor
    //    of wider ones. If several have the same 'border-width' then styles are preferred in this order: 'double', 'solid',
    //    'dashed', 'dotted', 'ridge', 'outset', 'groove', and the lowest: 'inset'.
    if (a.width != b.width)
        return a.width < b.width;
    return line_style_score(a.line_style) < line_style_score(b.line_style);
}

namespace {

// Each segment stores the border that currently wins at one slot boundary of the table grid,
// together with the kind of element it came from. A value-initialized segment acts as a sentinel
// meaning "no border applied yet": since candidates with a line style of 'none' never replace an
// incumbent, a segment whose winner still has style 'none' received no visible contribution at all.
using CollapsedBorder = Painting::Paintable::BorderDataWithElementKind;

// Implements border conflict resolution, as described in
// https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution, with a "push" model over a
// grid of border line segments: each boundary between two grid slots is a single shared segment, so
// borders of adjacent elements collapse by construction instead of requiring neighbor lookups.
//
// Horizontal border lines run between (and around) rows: there are row_count + 1 of them, each with
// one segment per column. Vertical border lines run between (and around) columns: column_count + 1
// of them, each with one segment per row.
//
// Table parts must be applied in order of decreasing precedence — cells, rows, row groups, columns,
// column groups, and lastly the table — with parts of equal precedence applied leftmost/topmost
// first. A candidate border only replaces the current winner of a segment when it is strictly more
// specific (steps 1-3 of the border conflict resolution algorithm), so ties resolve towards the
// earlier-applied part, which implements step 4 without tracking element kinds or coordinates.
class CollapsedBorderGrid {
public:
    CollapsedBorderGrid(size_t row_count, size_t column_count)
    {
        m_horizontal_lines.resize(row_count + 1);
        for (auto& line : m_horizontal_lines)
            line.resize(column_count);
        m_vertical_lines.resize(column_count + 1);
        for (auto& line : m_vertical_lines)
            line.resize(row_count);
    }

    void apply_borders(NodeWithStyle const& element, size_t row_start, size_t row_end, size_t column_start, size_t column_end, Painting::Paintable::ConflictingElementKind element_kind)
    {
        auto const& style = element.computed_values();
        apply_to_segments(m_horizontal_lines[row_start], column_start, column_end, style.border_top(), element_kind);
        apply_to_segments(m_horizontal_lines[row_end], column_start, column_end, style.border_bottom(), element_kind);
        apply_to_segments(m_vertical_lines[column_start], row_start, row_end, style.border_left(), element_kind);
        apply_to_segments(m_vertical_lines[column_end], row_start, row_end, style.border_right(), element_kind);
    }

    // Segments strictly inside a spanning cell are not borders of any element; mark them as hidden
    // so that borders of rows and columns crossing the span cannot win there.
    void hide_segments_inside_span(size_t row_start, size_t row_end, size_t column_start, size_t column_end)
    {
        CollapsedBorder hidden { .border_data = { .color = Color::Transparent, .line_style = CSS::LineStyle::Hidden, .width = 0 }, .element_kind = Painting::Paintable::ConflictingElementKind::Cell };
        for (size_t row = row_start + 1; row < row_end; ++row) {
            for (size_t column = column_start; column < column_end; ++column)
                m_horizontal_lines[row][column] = hidden;
        }
        for (size_t column = column_start + 1; column < column_end; ++column) {
            for (size_t row = row_start; row < row_end; ++row)
                m_vertical_lines[column][row] = hidden;
        }
    }

    CollapsedBorder resolve_horizontal(size_t line_index, size_t column_start, size_t column_end) const { return most_specific_segment(m_horizontal_lines[line_index], column_start, column_end); }
    CollapsedBorder resolve_vertical(size_t line_index, size_t row_start, size_t row_end) const { return most_specific_segment(m_vertical_lines[line_index], row_start, row_end); }

private:
    static bool candidate_wins_over_incumbent(CSS::BorderData const& candidate, CSS::BorderData const& incumbent)
    {
        if (candidate.line_style == CSS::LineStyle::None)
            return false;
        return TableFormattingContext::border_is_less_specific(incumbent, candidate);
    }

    static void apply_to_segments(Vector<CollapsedBorder>& line, size_t start, size_t end, CSS::BorderData const& data, Painting::Paintable::ConflictingElementKind element_kind)
    {
        if (data.line_style == CSS::LineStyle::None)
            return;
        for (size_t i = start; i < end; ++i) {
            if (candidate_wins_over_incumbent(data, line[i].border_data))
                line[i] = { .border_data = data, .element_kind = element_kind };
        }
    }

    static CollapsedBorder most_specific_segment(Vector<CollapsedBorder> const& line, size_t start, size_t end)
    {
        auto winner = line[start];
        for (size_t i = start + 1; i < end; ++i) {
            if (candidate_wins_over_incumbent(line[i].border_data, winner.border_data))
                winner = line[i];
        }
        return winner;
    }

    Vector<Vector<CollapsedBorder>> m_horizontal_lines;
    Vector<Vector<CollapsedBorder>> m_vertical_lines;
};

}

void TableFormattingContext::border_conflict_resolution()
{
    auto table_cell_coordinates = [](Cell const& cell) {
        return Painting::Paintable::TableCellCoordinates {
            .row_index = cell.row_index,
            .column_index = cell.column_index,
            .row_span = cell.row_span,
            .column_span = cell.column_span
        };
    };

    if (table_box().computed_values().border_collapse() == CSS::BorderCollapse::Separate) {
        for (auto& cell : m_cells)
            m_state.get_mutable(cell.box).set_table_cell_coordinates(table_cell_coordinates(cell));
        return;
    }

    auto row_count = m_rows.size();
    auto column_count = m_columns.size();
    CollapsedBorderGrid grid { row_count, column_count };

    // Cells, column by column so that on ties the cell further to the left, then further to the
    // top, wins. Cell spans are already clipped to the table end by TableGrid.
    Vector<Cell const*> cells_by_column_then_row;
    cells_by_column_then_row.ensure_capacity(m_cells.size());
    for (auto const& cell : m_cells)
        cells_by_column_then_row.unchecked_append(&cell);
    quick_sort(cells_by_column_then_row, [](Cell const* a, Cell const* b) {
        if (a->column_index != b->column_index)
            return a->column_index < b->column_index;
        return a->row_index < b->row_index;
    });
    for (auto const* cell : cells_by_column_then_row) {
        if (cell->row_span > 1 || cell->column_span > 1)
            grid.hide_segments_inside_span(cell->row_index, cell->row_index + cell->row_span, cell->column_index, cell->column_index + cell->column_span);
        grid.apply_borders(cell->box, cell->row_index, cell->row_index + cell->row_span, cell->column_index, cell->column_index + cell->column_span, Painting::Paintable::ConflictingElementKind::Cell);
    }

    for (size_t row = 0; row < row_count; ++row)
        grid.apply_borders(m_rows[row].box, row, row + 1, 0, column_count, Painting::Paintable::ConflictingElementKind::Row);

    // Row groups, in the order their rows appear in the grid. Rows of a group are contiguous in
    // m_rows, since TableGrid collects them in tree order.
    for (size_t row = 0; row < row_count;) {
        auto const* row_group_box = m_rows[row].box.parent();
        if (!row_group_box || !TableGrid::is_table_row_group(*row_group_box)) {
            ++row;
            continue;
        }
        auto group_row_start = row;
        while (row < row_count && m_rows[row].box.parent() == row_group_box)
            ++row;
        grid.apply_borders(*row_group_box, group_row_start, row, 0, column_count, Painting::Paintable::ConflictingElementKind::RowGroup);
    }

    // Column (<col>) elements.
    size_t column_index = 0;
    TableGrid::for_each_child_box_matching(table_box(), TableGrid::is_table_column_group, [&](auto& column_group_box) {
        TableGrid::for_each_child_box_matching(column_group_box, is_table_column, [&](auto& column_box) {
            size_t span = 1;
            if (auto const* col_element = as_if<HTML::HTMLTableColElement>(column_box.dom_node()))
                span = col_element->span();
            auto column_end = min(column_index + span, column_count);
            for (; column_index < column_end; ++column_index)
                grid.apply_borders(column_box, 0, row_count, column_index, column_index + 1, Painting::Paintable::ConflictingElementKind::Column);
        });
    });

    grid.apply_borders(table_box(), 0, row_count, 0, column_count, Painting::Paintable::ConflictingElementKind::Table);

    for (auto& cell : m_cells) {
        auto const& cell_style = cell.box.computed_values();
        auto harvest = [&](CollapsedBorder winner, CSS::BorderData const& own_border) {
            // A winner whose style is 'none' means every border meeting at this edge is 'none'; fall
            // back to the cell's own (invisible) border so the stored winner matches the cell.
            if (winner.border_data.line_style == CSS::LineStyle::None)
                return CollapsedBorder { .border_data = own_border, .element_kind = Painting::Paintable::ConflictingElementKind::Cell };
            return winner;
        };

        auto row_end = cell.row_index + cell.row_span;
        auto column_end = cell.column_index + cell.column_span;
        Painting::Paintable::BordersDataWithElementKind override_borders_data {
            .top = harvest(grid.resolve_horizontal(cell.row_index, cell.column_index, column_end), cell_style.border_top()),
            .right = harvest(grid.resolve_vertical(column_end, cell.row_index, row_end), cell_style.border_right()),
            .bottom = harvest(grid.resolve_horizontal(row_end, cell.column_index, column_end), cell_style.border_bottom()),
            .left = harvest(grid.resolve_vertical(cell.column_index, cell.row_index, row_end), cell_style.border_left()),
        };

        auto& cell_state = m_state.get_mutable(cell.box);
        cell_state.set_table_cell_coordinates(table_cell_coordinates(cell));
        cell_state.border_top = override_borders_data.top.border_data.width;
        cell_state.border_right = override_borders_data.right.border_data.width;
        cell_state.border_bottom = override_borders_data.bottom.border_data.width;
        cell_state.border_left = override_borders_data.left.border_data.width;
        cell_state.set_override_borders_data(override_borders_data);
    }
}

CSSPixels TableFormattingContext::compute_row_content_block_size(Cell const& cell) const
{
    auto& row_state = m_state.get(m_rows[cell.row_index].box);
    if (cell.row_span == 1) {
        return row_state.content_block_size();
    }
    // The block size of a cell is the sum of all spanned rows, as described in
    // https://www.w3.org/TR/css-tables-3/#bounding-box-assignment

    // When the row span is greater than 1, the borders of inner rows within the span have to be
    // included in the content block size of the spanning cell. First top and final bottom borders are
    // excluded to be consistent with the handling of row span 1 case above, which uses the content
    // block size (no top and bottom borders) of the row.
    CSSPixels span_block_size = 0;
    for (size_t i = 0; i < cell.row_span; ++i) {
        auto const& row_state = m_state.get(m_rows[cell.row_index + i].box);
        if (i == 0) {
            span_block_size += row_state.content_block_size() + row_state.border_box_bottom();
        } else if (i == cell.row_span - 1) {
            span_block_size += row_state.border_box_top() + row_state.content_block_size();
        } else {
            span_block_size += row_state.border_box_block_size();
        }
    }

    // Compute cell block size as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
    // The logical size is the sum of:
    // - the inline/block sizes of all spanned visible columns/rows
    // - the inline/block border spacing times the amount of spanned visible columns/rows minus one
    // FIXME: Account for visibility.
    span_block_size += (cell.row_span - 1) * border_spacing_block();
    return span_block_size;
}

void TableFormattingContext::finish_grid_initialization(TableGrid const& table_grid)
{
    m_columns.resize(table_grid.column_count());
    for (auto const& cell : m_cells)
        m_columns[cell.column_index].has_originating_cells = true;
}

void TableFormattingContext::seed_table_participant_used_values(ContainingBlockConstraints const& participant_constraints)
{
    auto const& participant_percentage_basis = participant_constraints;

    TableGrid::for_each_child_box_matching(table_box(), TableGrid::is_table_row_group, [&](auto& row_group_box) {
        m_state.create(row_group_box, participant_percentage_basis.percentage_basis_inline_size, participant_percentage_basis.percentage_basis_block_size);
    });
    for (auto& row : m_rows)
        m_state.create(row.box, participant_percentage_basis.percentage_basis_inline_size, participant_percentage_basis.percentage_basis_block_size);
    for (auto& cell : m_cells)
        m_state.create(cell.box, participant_percentage_basis.percentage_basis_inline_size, participant_percentage_basis.percentage_basis_block_size);

    for (auto child = table_box().first_child(); child; child = child->next_sibling()) {
        auto* child_box = as_if<Box>(*child);
        if (!child_box)
            continue;

        if (child_box->display().is_table_caption())
            m_state.create(*child_box, participant_percentage_basis.percentage_basis_inline_size, participant_percentage_basis.percentage_basis_block_size);
    }
}

void TableFormattingContext::run_until_inline_size_calculation(LayoutInput const& layout_input, RowMeasurement row_measurement)
{
    auto const& available_space = layout_input.available_space;
    m_available_space = available_space;

    // Determine the number of rows/columns the table requires.
    finish_grid_initialization(TableGrid::calculate_row_column_grid(context_box(), m_cells, m_rows));

    // The containing block of every internal table box and caption is the table wrapper;
    // the table's own input carries the wrapper's constraints, and participant percentages
    // resolve against those. Percentage block sizes of participants only resolve once the table
    // itself has a non-auto block size.
    m_table_constraints = layout_input.containing_block_constraints;
    m_participant_constraints = ContainingBlockConstraints {
        m_table_constraints.percentage_basis_inline_size,
        table_box().computed_values().height().is_auto() ? Optional<CSSPixels> {} : m_table_constraints.percentage_basis_block_size,
        m_table_constraints.quirks_mode_percentage_basis_block_size,
    };
    seed_table_participant_used_values(m_participant_constraints);

    border_conflict_resolution();

    auto effective_row_measurement = row_measurement;
    m_needs_fixed_mode_row_measurement = false;
    // OPTIMIZATION: Row intrinsic measurements are only needed when row block-size constraints or row spans can affect
    //               the later row distribution. Simple tables get their actual row block sizes from cell layout.
    if (effective_row_measurement == RowMeasurement::Include && can_skip_row_intrinsic_measurement())
        effective_row_measurement = RowMeasurement::Skip;
    if (effective_row_measurement == RowMeasurement::Include && use_fixed_mode_layout()) {
        // https://drafts.csswg.org/css-tables-3/#computing-column-measures
        // For the purpose of measuring a column when laid out in fixed mode ... the min-content and max-content width
        // of cells is considered zero.
        //
        // https://drafts.csswg.org/css-tables-3/#ROWMIN
        // ROWMIN is defined as the sum of the minimum block sizes of the rows after a first row layout pass.
        // NB: So defer fixed-mode row measurement until after columns have their used widths.
        effective_row_measurement = RowMeasurement::Skip;
        m_needs_fixed_mode_row_measurement = true;
    }

    // Compute the minimum width of each column.
    compute_cell_measures(effective_row_measurement);
    compute_outer_content_sizes();
    compute_table_measures<Column>();

    if (effective_row_measurement == RowMeasurement::Include) {
        // https://www.w3.org/TR/css-tables-3/#row-layout
        // Since specified cell block sizes were ignored during row layout and cells spanning multiple rows were not
        // sized correctly, their block size must eventually be distributed to the rows they span. This is done
        // by running the same algorithm as the column measurement, with the span=1 value being initialized (for min-content) with the largest
        // of the resulting block size of the previous row layout, the size specified on the corresponding table row,
        // and the largest block size specified on cells that span only this row.
        compute_table_measures<Row>();
    }

    // Compute the inline size of the table.
    compute_table_inline_size();
}

void TableFormattingContext::parent_context_did_dimension_child_root_box()
{
    if (m_layout_mode != LayoutMode::Normal)
        return;

    context_box().for_each_in_subtree_of_type<Box const>([&](Layout::Box const& box) {
        if (box.is_absolutely_positioned()) {
            // FIXME: Calculate the real static position for a box nested inside a table
            //        instead of registering a zero rect.
            register_contained_abspos_child(box, {});
        }

        if (formatting_context_type_created_by_box(box).has_value()) {
            return TraversalDecision::SkipChildrenAndContinue;
        }

        return TraversalDecision::Continue;
    });

    layout_absolutely_positioned_children();
}

void TableFormattingContext::run(LayoutInput const& layout_input)
{
    auto const& available_space = layout_input.available_space;
    FORMATTING_CONTEXT_TRACE();
    m_available_space = available_space;
    m_min_border_box_block_size_from_flex_item = layout_input.table_grid_min_border_box_block_size;

    run_until_inline_size_calculation(layout_input);

    if (available_space.inline_size.is_intrinsic_sizing_constraint() && !available_space.block_size.is_intrinsic_sizing_constraint()) {
        return;
    }

    auto const& table_state = m_state.get(table_box());
    auto caption_available_space = AvailableSpace(
        AvailableSize::make_definite(clamp_to_max_dimension_value(table_state.border_box_inline_size())),
        available_space.block_size);

    auto captions_block_size = run_caption_layout(CSS::CaptionSide::Top, caption_available_space);

    // Distribute the inline size of the table among columns.
    distribute_inline_size_to_columns();

    compute_table_block_size();

    distribute_block_size_to_rows();

    position_row_boxes();
    position_cell_boxes();

    for (auto& cell : m_cells)
        layout_absolutely_positioned_children(cell.box);

    m_state.get_mutable(table_box()).set_content_block_size(m_table_block_size);

    captions_block_size += run_caption_layout(CSS::CaptionSide::Bottom, caption_available_space);

    // Table captions are positioned between the table margins and its borders (outside the grid box borders) as described in
    // https://www.w3.org/TR/css-tables-3/#bounding-box-assignment
    // A visual representation of this model can be found at https://www.w3.org/TR/css-tables-3/images/table_container.png
    m_state.get_mutable(table_box()).margin_bottom += captions_block_size;

    // Derive baselines for the table internals bottom-up (rows, then row groups, then the table box)
    // now that all offsets are final, so the table exports its baseline to outside consumers
    // (e.g. an inline-table participating in a line box).
    for (auto& row : m_rows)
        compute_and_store_baselines(m_state.get_mutable(row.box));
    table_box().for_each_child_of_type<Box>([&](Box const& child) {
        auto const& display = child.display();
        if (display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group())
            compute_and_store_baselines(m_state.get_mutable(child));
        return IterationDecision::Continue;
    });
    compute_and_store_baselines(m_state.get_mutable(table_box()));

    m_automatic_content_block_size = m_table_block_size;
}

CSSPixels TableFormattingContext::automatic_content_inline_size() const
{
    return m_state.get(table_box()).content_inline_size();
}

CSSPixels TableFormattingContext::automatic_content_block_size() const
{
    return m_automatic_content_block_size;
}

template<>
size_t TableFormattingContext::cell_span<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.row_span;
}

template<>
size_t TableFormattingContext::cell_span<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.column_span;
}

template<>
size_t TableFormattingContext::cell_index<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.row_index;
}

template<>
size_t TableFormattingContext::cell_index<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.column_index;
}

template<>
CSSPixels TableFormattingContext::cell_min_size<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_min_block_size;
}

template<>
CSSPixels TableFormattingContext::cell_min_size<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_min_inline_size;
}

template<>
CSSPixels TableFormattingContext::cell_max_size<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_max_block_size;
}

template<>
CSSPixels TableFormattingContext::cell_max_size<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_max_inline_size;
}

template<>
double TableFormattingContext::cell_percentage_contribution<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
    auto const& computed_values = cell.box.computed_values();
    auto max_block_size_percentage = computed_values.max_height().is_percentage() ? computed_values.max_height().percentage().value() : static_cast<double>(INFINITY);
    auto block_size_percentage = computed_values.height().is_percentage() ? computed_values.height().percentage().value() : 0;
    return min(block_size_percentage, max_block_size_percentage);
}

template<>
double TableFormattingContext::cell_percentage_contribution<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
    auto const& computed_values = cell.box.computed_values();
    auto max_inline_size_percentage = computed_values.max_width().is_percentage() ? computed_values.max_width().percentage().value() : static_cast<double>(INFINITY);
    auto inline_size_percentage = computed_values.width().is_percentage() ? computed_values.width().percentage().value() : 0;
    return min(inline_size_percentage, max_inline_size_percentage);
}

template<>
bool TableFormattingContext::cell_has_intrinsic_percentage<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.box.computed_values().height().is_percentage();
}

template<>
bool TableFormattingContext::cell_has_intrinsic_percentage<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.box.computed_values().width().is_percentage();
}

template<>
void TableFormattingContext::initialize_intrinsic_percentages_from_rows_or_columns<TableFormattingContext::Row>()
{
    for (auto& row : m_rows) {
        auto const& computed_values = row.box.computed_values();
        // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
        auto max_block_size_percentage = computed_values.max_height().is_percentage() ? computed_values.max_height().percentage().value() : static_cast<double>(INFINITY);
        auto block_size_percentage = computed_values.height().is_percentage() ? computed_values.height().percentage().value() : 0;
        row.has_intrinsic_percentage = computed_values.max_height().is_percentage() || computed_values.height().is_percentage();
        row.intrinsic_percentage = min(block_size_percentage, max_block_size_percentage);
    }
}

template<>
void TableFormattingContext::initialize_intrinsic_percentages_from_rows_or_columns<TableFormattingContext::Column>()
{
    size_t column_index = 0;
    TableGrid::for_each_child_box_matching(table_box(), is_table_column_group, [&](auto& column_group_box) {
        TableGrid::for_each_child_box_matching(column_group_box, is_table_column, [&](auto& column_box) {
            auto const& computed_values = column_box.computed_values();
            // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
            auto max_inline_size_percentage = computed_values.max_width().is_percentage() ? computed_values.max_width().percentage().value() : static_cast<double>(INFINITY);
            auto inline_size_percentage = computed_values.width().is_percentage() ? computed_values.width().percentage().value() : 0;
            m_columns[column_index].has_intrinsic_percentage = computed_values.max_width().is_percentage() || computed_values.width().is_percentage();
            m_columns[column_index].intrinsic_percentage = min(inline_size_percentage, max_inline_size_percentage);
            auto const& col_node = static_cast<HTML::HTMLElement const&>(*column_box.dom_node());
            unsigned span = col_node.get_attribute_value(HTML::AttributeNames::span).to_number<unsigned>().value_or(1);
            column_index += span;
        });
    });
}

template<class RowOrColumn>
void TableFormattingContext::initialize_intrinsic_percentages_from_cells()
{
    auto& rows_or_columns = table_rows_or_columns<RowOrColumn>();

    for (auto& cell : m_cells) {
        auto cell_span_value = cell_span<RowOrColumn>(cell);
        auto cell_start_rc_index = cell_index<RowOrColumn>(cell);
        auto cell_end_rc_index = cell_start_rc_index + cell_span_value;
        if (cell_has_intrinsic_percentage<RowOrColumn>(cell)) {
            for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                rows_or_columns[rc_index].has_intrinsic_percentage = true;
            }
            if (cell_span_value != 1) {
                continue;
            }
        } else {
            continue;
        }
        size_t rc_index = cell_index<RowOrColumn>(cell);
        rows_or_columns[rc_index].has_intrinsic_percentage = true;
        rows_or_columns[rc_index].intrinsic_percentage = max(cell_percentage_contribution<RowOrColumn>(cell), rows_or_columns[rc_index].intrinsic_percentage);
    }
}

template<>
CSSPixels TableFormattingContext::border_spacing<TableFormattingContext::Row>()
{
    return border_spacing_block();
}

template<>
CSSPixels TableFormattingContext::border_spacing<TableFormattingContext::Column>()
{
    return border_spacing_inline();
}

CSSPixels TableFormattingContext::border_spacing_inline() const
{
    auto const& computed_values = table_box().computed_values();
    // When a table is laid out in collapsed-borders mode, the border-spacing of the table-root is ignored (as if it was set to 0px):
    // https://www.w3.org/TR/css-tables-3/#collapsed-style-overrides
    if (computed_values.border_collapse() == CSS::BorderCollapse::Collapse)
        return 0;
    return computed_values.border_spacing_horizontal();
}

CSSPixels TableFormattingContext::border_spacing_block() const
{
    auto const& computed_values = table_box().computed_values();
    // When a table is laid out in collapsed-borders mode, the border-spacing of the table-root is ignored (as if it was set to 0px):
    // https://www.w3.org/TR/css-tables-3/#collapsed-style-overrides
    if (computed_values.border_collapse() == CSS::BorderCollapse::Collapse)
        return 0;
    return computed_values.border_spacing_vertical();
}

template<>
Vector<TableFormattingContext::Row>& TableFormattingContext::table_rows_or_columns()
{
    return m_rows;
}

template<>
Vector<TableFormattingContext::Column>& TableFormattingContext::table_rows_or_columns()
{
    return m_columns;
}

}
