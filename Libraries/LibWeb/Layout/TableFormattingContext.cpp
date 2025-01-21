/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/TableFormattingContext.h>

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

CSSPixels TableFormattingContext::run_caption_layout(CSS::CaptionSide phase)
{
    CSSPixels caption_height = 0;
    for (auto* child = table_box().first_child(); child; child = child->next_sibling()) {
        if (!child->display().is_table_caption() || child->computed_values().caption_side() != phase) {
            continue;
        }
        // The caption boxes are principal block-level boxes that retain their own content, padding, margin, and border areas,
        // and are rendered as normal block boxes inside the table wrapper box, as described in https://www.w3.org/TR/CSS22/tables.html#model
        auto caption_context = make<BlockFormattingContext>(m_state, m_layout_mode, *as<BlockContainer>(child), this);
        caption_context->run(*m_available_space);
        VERIFY(child->is_box());
        auto const& child_box = static_cast<Box const&>(*child);
        // FIXME: Since caption only has inline children, BlockFormattingContext doesn't resolve the vertical metrics.
        //        We need to do it manually here.
        caption_context->resolve_vertical_box_model_metrics(child_box, m_available_space->width.to_px_or_zero());
        auto const& caption_state = m_state.get(child_box);
        if (phase == CSS::CaptionSide::Top) {
            m_state.get_mutable(table_box()).set_content_y(caption_state.content_height() + caption_state.margin_box_bottom());
        } else {
            m_state.get_mutable(child_box).set_content_y(
                m_state.get(table_box()).margin_box_height() + caption_state.margin_box_top());
        }
        caption_height += caption_state.margin_box_height();
    }
    return caption_height;
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
            auto const& col_node = static_cast<HTML::HTMLTableColElement const&>(*column_box.dom_node());
            unsigned span = col_node.get_attribute_value(HTML::AttributeNames::span).to_number<unsigned>().value_or(1);
            column_index += span;
        });
    });

    for (auto& row : m_rows) {
        auto const& computed_values = row.box->computed_values();
        if (computed_values.height().is_length()) {
            row.is_constrained = true;
        }
    }

    for (auto& cell : m_cells) {
        auto const& computed_values = cell.box->computed_values();
        if (computed_values.width().is_length()) {
            m_columns[cell.column_index].is_constrained = true;
        }

        if (computed_values.height().is_length()) {
            m_rows[cell.row_index].is_constrained = true;
        }
    }
}

void TableFormattingContext::compute_cell_measures()
{
    // Implements https://www.w3.org/TR/css-tables-3/#computing-cell-measures.
    auto const& containing_block = m_state.get(*table_wrapper().containing_block());

    compute_constrainedness();

    for (auto& cell : m_cells) {
        auto const& computed_values = cell.box->computed_values();
        CSSPixels padding_top = computed_values.padding().top().to_px(cell.box, containing_block.content_height());
        CSSPixels padding_bottom = computed_values.padding().bottom().to_px(cell.box, containing_block.content_height());
        CSSPixels padding_left = computed_values.padding().left().to_px(cell.box, containing_block.content_width());
        CSSPixels padding_right = computed_values.padding().right().to_px(cell.box, containing_block.content_width());

        auto const& cell_state = m_state.get(cell.box);
        auto use_collapsing_borders_model = cell_state.override_borders_data().has_value();
        // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
        CSSPixels border_top = use_collapsing_borders_model ? round(cell_state.border_top / 2) : computed_values.border_top().width;
        CSSPixels border_bottom = use_collapsing_borders_model ? round(cell_state.border_bottom / 2) : computed_values.border_bottom().width;
        CSSPixels border_left = use_collapsing_borders_model ? round(cell_state.border_left / 2) : computed_values.border_left().width;
        CSSPixels border_right = use_collapsing_borders_model ? round(cell_state.border_right / 2) : computed_values.border_right().width;

        auto min_content_width = calculate_min_content_width(cell.box);
        auto max_content_width = calculate_max_content_width(cell.box);
        auto min_content_height = calculate_min_content_height(cell.box, max_content_width);
        auto max_content_height = calculate_max_content_height(cell.box, min_content_width);

        // The outer min-content height of a table-cell is max(min-height, min-content height) adjusted by the cell intrinsic offsets.
        auto min_height = computed_values.min_height().to_px(cell.box, containing_block.content_height());
        auto cell_intrinsic_height_offsets = padding_top + padding_bottom + border_top + border_bottom;
        cell.outer_min_height = max(min_height, min_content_height) + cell_intrinsic_height_offsets;
        // The outer min-content width of a table-cell is max(min-width, min-content width) adjusted by the cell intrinsic offsets.
        auto min_width = computed_values.min_width().to_px(cell.box, containing_block.content_width());
        auto cell_intrinsic_width_offsets = padding_left + padding_right + border_left + border_right;
        // For fixed mode, according to https://www.w3.org/TR/css-tables-3/#computing-column-measures:
        // The min-content and max-content width of cells is considered zero unless they are directly specified as a length-percentage,
        // in which case they are resolved based on the table width (if it is definite, otherwise use 0).
        auto width_is_specified_length_or_percentage = computed_values.width().is_length() || computed_values.width().is_percentage();
        if (!use_fixed_mode_layout() || width_is_specified_length_or_percentage) {
            cell.outer_min_width = max(min_width, min_content_width) + cell_intrinsic_width_offsets;
        }

        // The tables specification isn't explicit on how to use the height and max-height CSS properties in the outer max-content formulas.
        // However, during this early phase we don't have enough information to resolve percentage sizes yet and the formulas for outer sizes
        // in the specification give enough clues to pick defaults in a way that makes sense.
        auto height = computed_values.height().is_length() ? computed_values.height().to_px(cell.box, containing_block.content_height()) : 0;
        auto max_height = computed_values.max_height().is_length() ? computed_values.max_height().to_px(cell.box, containing_block.content_height()) : CSSPixels::max();
        if (m_rows[cell.row_index].is_constrained) {
            // The outer max-content height of a table-cell in a constrained row is
            // max(min-height, height, min-content height, min(max-height, height)) adjusted by the cell intrinsic offsets.
            // NB: min(max-height, height) doesn't have any effect here, we can simplify the expression to max(min-height, height, min-content height).
            cell.outer_max_height = max(min_height, max(height, min_content_height)) + cell_intrinsic_height_offsets;
        } else {
            // The outer max-content height of a table-cell in a non-constrained row is
            // max(min-height, height, min-content height, min(max-height, max-content height)) adjusted by the cell intrinsic offsets.
            cell.outer_max_height = max(min_height, max(height, max(min_content_height, min(max_height, max_content_height)))) + cell_intrinsic_height_offsets;
        }

        // See the explanation for height and max_height above.
        auto width = computed_values.width().is_length() ? computed_values.width().to_px(cell.box, containing_block.content_width()) : 0;
        auto max_width = computed_values.max_width().is_length() ? computed_values.max_width().to_px(cell.box, containing_block.content_width()) : CSSPixels::max();
        if (use_fixed_mode_layout() && !width_is_specified_length_or_percentage) {
            continue;
        }
        if (m_columns[cell.column_index].is_constrained) {
            // The outer max-content width of a table-cell in a constrained column is
            // max(min-width, width, min-content width, min(max-width, width)) adjusted by the cell intrinsic offsets.
            // NB: min(max-width, width) doesn't have any effect here, we can simplify the expression to max(min-width, width, min-content width).
            cell.outer_max_width = max(min_width, max(width, min_content_width)) + cell_intrinsic_width_offsets;
        } else {
            // The outer max-content width of a table-cell in a non-constrained column is
            // max(min-width, width, min-content width, min(max-width, max-content width)) adjusted by the cell intrinsic offsets.
            cell.outer_max_width = max(min_width, max(width, max(min_content_width, min(max_width, max_content_width)))) + cell_intrinsic_width_offsets;
        }
    }
}

void TableFormattingContext::compute_outer_content_sizes()
{
    auto const& containing_block = m_state.get(*table_wrapper().containing_block());

    size_t column_index = 0;
    TableGrid::for_each_child_box_matching(table_box(), is_table_column_group, [&](auto& column_group_box) {
        TableGrid::for_each_child_box_matching(column_group_box, is_table_column, [&](auto& column_box) {
            auto const& computed_values = column_box.computed_values();
            auto min_width = computed_values.min_width().to_px(column_box, containing_block.content_width());
            auto max_width = computed_values.max_width().is_length() ? computed_values.max_width().to_px(column_box, containing_block.content_width()) : CSSPixels::max();
            auto width = computed_values.width().to_px(column_box, containing_block.content_width());
            // The outer min-content width of a table-column or table-column-group is max(min-width, width).
            m_columns[column_index].min_size = max(min_width, width);
            // The outer max-content width of a table-column or table-column-group is max(min-width, min(max-width, width)).
            m_columns[column_index].max_size = max(min_width, min(max_width, width));
            auto const& col_node = static_cast<HTML::HTMLTableColElement const&>(*column_box.dom_node());
            unsigned span = col_node.get_attribute_value(HTML::AttributeNames::span).to_number<unsigned>().value_or(1);
            column_index += span;
        });
    });

    for (auto& row : m_rows) {
        auto const& computed_values = row.box->computed_values();
        auto min_height = computed_values.min_height().to_px(row.box, containing_block.content_height());
        auto max_height = computed_values.max_height().is_length() ? computed_values.max_height().to_px(row.box, containing_block.content_height()) : CSSPixels::max();
        auto height = computed_values.height().to_px(row.box, containing_block.content_height());
        // The outer min-content height of a table-row or table-row-group is max(min-height, height).
        row.min_size = max(min_height, height);
        // The outer max-content height of a table-row or table-row-group is max(min-height, min(max-height, height)).
        row.max_size = max(min_height, min(max_height, height));
    }
}

template<>
void TableFormattingContext::initialize_table_measures<TableFormattingContext::Row>()
{
    auto const& containing_block = m_state.get(*table_wrapper().containing_block());

    for (auto& cell : m_cells) {
        auto const& computed_values = cell.box->computed_values();
        if (cell.row_span == 1) {
            auto specified_height = computed_values.height().to_px(cell.box, containing_block.content_height());
            // https://www.w3.org/TR/css-tables-3/#row-layout makes specified cell height part of the initialization formula for row table measures:
            // This is done by running the same algorithm as the column measurement, with the span=1 value being initialized (for min-content) with
            // the largest of the resulting height of the previous row layout, the height specified on the corresponding table-row (if any), and
            // the largest height specified on cells that span this row only (the algorithm starts by considering cells of span 2 on top of that assignment).
            m_rows[cell.row_index].min_size = max(m_rows[cell.row_index].min_size, max(cell.outer_min_height, specified_height));
            m_rows[cell.row_index].max_size = max(m_rows[cell.row_index].max_size, cell.outer_max_height);
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
            m_columns[cell.column_index].min_size = max(m_columns[cell.column_index].min_size, cell.outer_min_width);
            m_columns[cell.column_index].max_size = max(m_columns[cell.column_index].max_size, cell.outer_max_width);
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
            CSSPixels width_sum_of_columns_with_zero_intrinsic_percentage = 0;
            size_t number_of_columns_with_zero_intrinsic_percentage = 0;
            for (auto rc_index = cell_start_rc_index; rc_index < cell_end_rc_index; rc_index++) {
                if (rows_or_columns[rc_index].intrinsic_percentage == 0) {
                    width_sum_of_columns_with_zero_intrinsic_percentage += rows_or_columns[rc_index].max_size;
                    ++number_of_columns_with_zero_intrinsic_percentage;
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
                CSSPixels ajusted_cell_contribution;
                if (width_sum_of_columns_with_zero_intrinsic_percentage != 0) {
                    ajusted_cell_contribution = cell_contribution.scaled(rows_or_columns[rc_index].max_size / static_cast<double>(width_sum_of_columns_with_zero_intrinsic_percentage));
                } else {
                    // However, if this ratio is undefined because the denominator is zero, instead use the 1 divided by the number of columns
                    // spanned by the cell that have an intrinsic percentage width of the column based on cells of span up to N-1 equal to zero.
                    ajusted_cell_contribution = cell_contribution * 1 / number_of_columns_with_zero_intrinsic_percentage;
                }
                intrinsic_percentage_contribution_by_index[rc_index] = max(static_cast<double>(ajusted_cell_contribution), intrinsic_percentage_contribution_by_index[rc_index]);
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
    for (auto* child = table_box().first_child(); child; child = child->next_sibling()) {
        if (!child->display().is_table_caption()) {
            continue;
        }
        VERIFY(child->is_box());
        capmin = max(calculate_min_content_width(static_cast<Box const&>(*child)), capmin);
    }
    return capmin;
}

static bool width_is_auto_relative_to_state(CSS::Size const& width, LayoutState::UsedValues const& state)
{
    return width.is_auto() || (width.contains_percentage() && !state.has_definite_width());
}

void TableFormattingContext::compute_table_width()
{
    // https://drafts.csswg.org/css-tables-3/#computing-the-table-width

    auto& table_box_state = m_state.get_mutable(table_box());

    auto& computed_values = table_box().computed_values();

    auto width_of_table_containing_block = m_available_space->width;

    // Percentages on 'width' and 'height' on the table are relative to the table wrapper box's containing block,
    // not the table wrapper box itself.
    auto const& containing_block_state = m_state.get(*table_wrapper().containing_block());
    CSSPixels width_of_table_wrapper_containing_block = containing_block_state.content_width();

    // Compute undistributable space due to border spacing: https://www.w3.org/TR/css-tables-3/#computing-undistributable-space.
    auto undistributable_space = (m_columns.size() + 1) * border_spacing_horizontal();

    // The row/column-grid width minimum (GRIDMIN) width is the sum of the min-content width
    // of all the columns plus cell spacing or borders.
    CSSPixels grid_min = 0;
    for (auto& column : m_columns) {
        grid_min += column.min_size;
    }
    grid_min += undistributable_space;

    // The row/column-grid width maximum (GRIDMAX) width is the sum of the max-content width
    // of all the columns plus cell spacing or borders.
    CSSPixels grid_max = 0;
    for (auto& column : m_columns) {
        grid_max += column.max_size;
    }
    grid_max += undistributable_space;

    // The used min-width of a table is the greater of the resolved min-width, CAPMIN, and GRIDMIN.
    auto used_min_width = max(grid_min, compute_capmin());
    if (!computed_values.min_width().is_auto()) {
        used_min_width = max(used_min_width, computed_values.min_width().to_px(table_box(), width_of_table_wrapper_containing_block));
    }

    CSSPixels used_width;
    if (m_available_space->width.is_min_content()) {
        used_width = grid_min;
    } else if (m_available_space->width.is_max_content()) {
        used_width = grid_max;
    } else if (width_is_auto_relative_to_state(computed_values.width(), containing_block_state)) {
        // If the table-root has 'width: auto', the used width is the greater of
        // min(GRIDMAX, the table’s containing block width), the used min-width of the table.
        if (width_of_table_containing_block.is_definite())
            used_width = max(min(grid_max, width_of_table_containing_block.to_px_or_zero()), used_min_width);
        else
            used_width = max(grid_max, used_min_width);
        // https://www.w3.org/TR/CSS22/tables.html#auto-table-layout
        // A percentage value for a column width is relative to the table width. If the table has 'width: auto',
        // a percentage represents a constraint on the column's width, which a UA should try to satisfy.
        for (auto& cell : m_cells) {
            auto const& cell_width = cell.box->computed_values().width();
            if (cell_width.is_percentage()) {
                CSSPixels adjusted_used_width = undistributable_space;
                if (cell_width.percentage().value() != 0)
                    adjusted_used_width += CSSPixels::nearest_value_for(ceil(100 / cell_width.percentage().value() * cell.outer_max_width));

                if (width_of_table_containing_block.is_definite())
                    used_width = min(max(used_width, adjusted_used_width), width_of_table_containing_block.to_px_or_zero());
                else
                    used_width = max(used_width, adjusted_used_width);
            }
        }
    } else if (computed_values.width().is_max_content()) {
        used_width = grid_max;
    } else {
        // If the table-root’s width property has a computed value (resolving to
        // resolved-table-width) other than auto, the used width is the greater
        // of resolved-table-width, and the used min-width of the table.
        CSSPixels resolved_table_width = computed_values.width().to_px(table_box(), width_of_table_wrapper_containing_block);
        // Since used_width is content width, we need to subtract the border and padding spacing from the specified width for a consistent comparison.
        if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox)
            resolved_table_width -= table_box_state.border_box_left() + table_box_state.border_box_right();
        used_width = max(resolved_table_width, used_min_width);
        if (!should_treat_max_width_as_none(table_box(), m_available_space->width))
            used_width = min(used_width, computed_values.max_width().to_px(table_box(), width_of_table_wrapper_containing_block));
    }

    table_box_state.set_content_width(used_width);
    auto& table_wrapper_box_state = m_state.get_mutable(table_wrapper());
    table_wrapper_box_state.set_content_width(table_box_state.border_box_width());
}

CSSPixels TableFormattingContext::compute_columns_total_used_width() const
{
    CSSPixels total_used_width = 0;
    for (auto& column : m_columns) {
        total_used_width += column.used_width;
    }
    return total_used_width;
}

static CSSPixels compute_columns_total_candidate_width(Vector<CSSPixels> const& candidate_widths)
{
    CSSPixels total_candidate_width = 0;
    for (auto width : candidate_widths) {
        total_candidate_width += width;
    }
    return total_candidate_width;
}

void TableFormattingContext::commit_candidate_column_widths(Vector<CSSPixels> const& candidate_widths)
{
    VERIFY(candidate_widths.size() == m_columns.size());
    for (size_t i = 0; i < m_columns.size(); ++i) {
        m_columns[i].used_width = candidate_widths[i];
    }
}

void TableFormattingContext::assign_columns_width_linear_combination(Vector<CSSPixels> const& candidate_widths, CSSPixels available_width)
{
    auto columns_total_candidate_width = compute_columns_total_candidate_width(candidate_widths);
    auto columns_total_used_width = compute_columns_total_used_width();
    if (columns_total_candidate_width == columns_total_used_width) {
        return;
    }
    auto candidate_weight = (available_width - columns_total_used_width) / static_cast<double>(columns_total_candidate_width - columns_total_used_width);
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        column.used_width = CSSPixels::nearest_value_for(candidate_weight * candidate_widths[i] + (1 - candidate_weight) * column.used_width);
    }
}

template<class ColumnFilter, class BaseWidthGetter>
bool TableFormattingContext::distribute_excess_width_proportionally_to_base_width(CSSPixels excess_width, ColumnFilter column_filter, BaseWidthGetter base_width_getter)
{
    bool found_matching_columns = false;
    CSSPixels total_base_width = 0;
    for (auto const& column : m_columns) {
        if (column_filter(column)) {
            total_base_width += base_width_getter(column);
            found_matching_columns = true;
        }
    }
    if (!found_matching_columns) {
        return false;
    }
    VERIFY(total_base_width > 0);
    for (auto& column : m_columns) {
        if (column_filter(column)) {
            column.used_width += CSSPixels::nearest_value_for(excess_width * base_width_getter(column) / static_cast<double>(total_base_width));
        }
    }
    return true;
}

template<class ColumnFilter>
bool TableFormattingContext::distribute_excess_width_equally(CSSPixels excess_width, ColumnFilter column_filter)
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
            column.used_width += excess_width / matching_column_count;
        }
    }
    return matching_column_count;
}

template<class ColumnFilter>
bool TableFormattingContext::distribute_excess_width_by_intrinsic_percentage(CSSPixels excess_width, ColumnFilter column_filter)
{
    bool found_matching_columns = false;
    double total_percentage_width = 0;
    for (auto const& column : m_columns) {
        if (column_filter(column)) {
            found_matching_columns = true;
            total_percentage_width += column.intrinsic_percentage;
        }
    }
    if (!found_matching_columns) {
        return false;
    }
    for (auto& column : m_columns) {
        if (column_filter(column)) {
            column.used_width += CSSPixels::nearest_value_for(excess_width * column.intrinsic_percentage / total_percentage_width);
        }
    }
    return true;
}

void TableFormattingContext::distribute_width_to_columns()
{
    // Implements https://www.w3.org/TR/css-tables-3/#width-distribution-algorithm

    // The total horizontal border spacing is defined for each table:
    // - For tables laid out in separated-borders mode containing at least one column, the horizontal component of the computed value of the border-spacing property times one plus the number of columns in the table
    // - Otherwise, 0
    auto total_horizontal_border_spacing = m_columns.is_empty() ? 0 : (m_columns.size() + 1) * border_spacing_horizontal();

    // The assignable table width is the used width of the table minus the total horizontal border spacing (if any).
    // This is the width that we will be able to allocate to the columns.
    CSSPixels const available_width = m_state.get(table_box()).content_width() - total_horizontal_border_spacing;

    Vector<CSSPixels> candidate_widths;
    candidate_widths.resize(m_columns.size());

    // 1. The min-content sizing-guess is the set of column width assignments where each column is assigned its min-content width.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        // In fixed mode, the min-content width of percent-columns and auto-columns is considered to be zero:
        // https://www.w3.org/TR/css-tables-3/#width-distribution-in-fixed-mode
        if (use_fixed_mode_layout() && !column.is_constrained) {
            continue;
        }
        column.used_width = column.min_size;
        candidate_widths[i] = column.min_size;
    }

    // 2. The min-content-percentage sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width.
    //    - all other columns are assigned their min-content width.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        if (column.has_intrinsic_percentage) {
            candidate_widths[i] = max(column.min_size, CSSPixels::nearest_value_for(column.intrinsic_percentage / 100 * available_width));
        }
    }

    // If the assignable table width is less than or equal to the max-content sizing-guess, the used widths of the columns must be the
    // linear combination (with weights adding to 1) of the two consecutive sizing-guesses whose width sums bound the available width.
    if (available_width < compute_columns_total_candidate_width(candidate_widths)) {
        assign_columns_width_linear_combination(candidate_widths, available_width);
        return;
    } else {
        commit_candidate_column_widths(candidate_widths);
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
            candidate_widths[i] = column.max_size;
        }
    }

    if (available_width < compute_columns_total_candidate_width(candidate_widths)) {
        assign_columns_width_linear_combination(candidate_widths, available_width);
        return;
    } else {
        commit_candidate_column_widths(candidate_widths);
    }

    // 4. The max-content sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width
    //    - all other columns are assigned their max-content width.
    for (size_t i = 0; i < m_columns.size(); ++i) {
        auto& column = m_columns[i];
        if (!column.has_intrinsic_percentage) {
            candidate_widths[i] = column.max_size;
        }
    }

    if (available_width < compute_columns_total_candidate_width(candidate_widths)) {
        assign_columns_width_linear_combination(candidate_widths, available_width);
        return;
    } else {
        commit_candidate_column_widths(candidate_widths);
    }

    // Otherwise, the used widths of the columns are the result of starting from the max-content sizing-guess and distributing
    // the excess width to the columns of the table according to the rules for distributing excess width to columns (for used width).
    distribute_excess_width_to_columns(available_width);
}

void TableFormattingContext::distribute_excess_width_to_columns(CSSPixels available_width)
{
    // Implements https://www.w3.org/TR/css-tables-3/#distributing-width-to-columns
    auto columns_total_used_width = compute_columns_total_used_width();
    if (columns_total_used_width >= available_width) {
        return;
    }
    auto excess_width = available_width - columns_total_used_width;
    if (excess_width == 0) {
        return;
    }

    if (use_fixed_mode_layout()) {
        distribute_excess_width_to_columns_fixed_mode(excess_width);
        return;
    }

    // 1. If there are non-constrained columns that have originating cells with intrinsic percentage width of 0% and with nonzero
    //    max-content width (aka the columns allowed to grow by this rule), the distributed widths of the columns allowed to grow
    //    by this rule are increased in proportion to max-content width so the total increase adds to the excess width.
    if (distribute_excess_width_proportionally_to_base_width(
            excess_width,
            [](auto const& column) {
                return !column.is_constrained && column.has_originating_cells && column.intrinsic_percentage == 0 && column.max_size > 0;
            },
            [](auto const& column) { return column.max_size; })) {
        excess_width = available_width - compute_columns_total_used_width();
    }
    if (excess_width == 0) {
        return;
    }
    // 2. Otherwise, if there are non-constrained columns that have originating cells with intrinsic percentage width of 0% (aka the columns
    //    allowed to grow by this rule, which thanks to the previous rule must have zero max-content width), the distributed widths of the
    //    columns allowed to grow by this rule are increased by equal amounts so the total increase adds to the excess width.
    if (distribute_excess_width_equally(excess_width,
            [](auto const& column) {
                return !column.is_constrained && column.has_originating_cells && column.intrinsic_percentage == 0;
            })) {
        excess_width = available_width - compute_columns_total_used_width();
    }
    if (excess_width == 0) {
        return;
    }
    // 3. Otherwise, if there are constrained columns with intrinsic percentage width of 0% and with nonzero max-content width
    //    (aka the columns allowed to grow by this rule, which, due to other rules, must have originating cells), the distributed widths of the
    //    columns allowed to grow by this rule are increased in proportion to max-content width so the total increase adds to the excess width.
    if (distribute_excess_width_proportionally_to_base_width(
            excess_width,
            [](auto const& column) {
                return column.is_constrained && column.intrinsic_percentage == 0 && column.max_size > 0;
            },
            [](auto const& column) { return column.max_size; })) {
        excess_width = available_width - compute_columns_total_used_width();
    }
    if (excess_width == 0) {
        return;
    }
    // 4. Otherwise, if there are columns with intrinsic percentage width greater than 0% (aka the columns allowed to grow by this rule,
    //    which, due to other rules, must have originating cells), the distributed widths of the columns allowed to grow by this rule are
    //    increased in proportion to intrinsic percentage width so the total increase adds to the excess width.
    if (distribute_excess_width_by_intrinsic_percentage(excess_width, [](auto const& column) {
            return column.intrinsic_percentage > 0;
        })) {
        excess_width = available_width - compute_columns_total_used_width();
    }
    if (excess_width == 0) {
        return;
    }
    // 5. Otherwise, if there is any such column, the distributed widths of all columns that have originating cells are increased by equal amounts
    //    so the total increase adds to the excess width.
    if (distribute_excess_width_equally(
            excess_width,
            [](auto const& column) { return column.has_originating_cells; })) {
        excess_width = available_width - compute_columns_total_used_width();
    }
    if (excess_width == 0) {
        return;
    }
    // 6. Otherwise, the distributed widths of all columns are increased by equal amounts so the total increase adds to the excess width.
    distribute_excess_width_equally(excess_width, [](auto const&) { return true; });
}

void TableFormattingContext::distribute_excess_width_to_columns_fixed_mode(CSSPixels excess_width)
{
    // Implements the fixed mode for https://www.w3.org/TR/css-tables-3/#distributing-width-to-columns.

    // If there are any columns with no width specified, the excess width is distributed in equally to such columns
    if (distribute_excess_width_equally(excess_width, [](auto const& column) { return !column.is_constrained && !column.has_intrinsic_percentage; })) {
        return;
    }
    // otherwise, if there are columns with non-zero length widths from the base assignment, the excess width is distributed proportionally to width among those columns
    if (distribute_excess_width_proportionally_to_base_width(
            excess_width, [](auto const& column) { return column.used_width > 0; }, [](auto const& column) { return column.used_width; })) {
        return;
    }
    // otherwise, if there are columns with non-zero percentage widths from the base assignment, the excess width is distributed proportionally to percentage width among those columns
    if (distribute_excess_width_by_intrinsic_percentage(excess_width, [](auto const& column) { return column.intrinsic_percentage > 0; })) {
        return;
    }
    // otherwise, the excess width is distributed equally to the zero-sized columns
    distribute_excess_width_equally(excess_width, [](auto const& column) { return column.used_width == 0; });
}

void TableFormattingContext::compute_table_height()
{
    // First pass of row height calculation:
    for (auto& row : m_rows) {
        auto row_computed_height = row.box->computed_values().height();
        if (row_computed_height.is_length()) {
            auto height_of_containing_block = m_state.get(*row.box->containing_block()).content_height();
            auto row_used_height = row_computed_height.to_px(row.box, height_of_containing_block);
            row.base_height = max(row.base_height, row_used_height);
        }
    }

    // First pass of cells layout:
    for (auto& cell : m_cells) {
        auto& row = m_rows[cell.row_index];
        auto& cell_state = m_state.get_mutable(cell.box);

        CSSPixels span_width = 0;
        for (size_t i = 0; i < cell.column_span; ++i)
            span_width += m_columns[cell.column_index + i].used_width;

        auto width_of_containing_block = cell_state.containing_block_used_values()->content_width();
        auto height_of_containing_block = cell_state.containing_block_used_values()->content_height();

        cell_state.padding_top = cell.box->computed_values().padding().top().to_px(cell.box, width_of_containing_block);
        cell_state.padding_bottom = cell.box->computed_values().padding().bottom().to_px(cell.box, width_of_containing_block);
        cell_state.padding_left = cell.box->computed_values().padding().left().to_px(cell.box, width_of_containing_block);
        cell_state.padding_right = cell.box->computed_values().padding().right().to_px(cell.box, width_of_containing_block);

        if (cell.box->computed_values().border_collapse() == CSS::BorderCollapse::Separate) {
            cell_state.border_top = cell.box->computed_values().border_top().width;
            cell_state.border_bottom = cell.box->computed_values().border_bottom().width;
            cell_state.border_left = cell.box->computed_values().border_left().width;
            cell_state.border_right = cell.box->computed_values().border_right().width;
        }

        auto cell_computed_height = cell.box->computed_values().height();
        if (cell_computed_height.is_length()) {
            auto cell_used_height = cell_computed_height.to_px(cell.box, height_of_containing_block);
            cell_state.set_content_height(cell_used_height - cell_state.border_box_top() - cell_state.border_box_bottom());

            row.base_height = max(row.base_height, cell_used_height);
        }

        // Compute cell width as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
        // The position of any table-cell, table-track, or table-track-group box within the table is defined as the rectangle whose width/height is the sum of:
        // - the widths/heights of all spanned visible columns/rows
        // - the horizontal/vertical border-spacing times the amount of spanned visible columns/rows minus one
        // FIXME: Account for visibility.
        cell_state.set_content_width(span_width - cell_state.border_box_left() - cell_state.border_box_right() + (cell.column_span - 1) * border_spacing_horizontal());
        if (auto independent_formatting_context = layout_inside(cell.box, m_layout_mode, cell_state.available_inner_space_or_constraints_from(*m_available_space))) {
            cell_state.set_content_height(independent_formatting_context->automatic_content_height());
            independent_formatting_context->parent_context_did_dimension_child_root_box();
        }

        cell.baseline = box_baseline(cell.box);

        // Implements https://www.w3.org/TR/css-tables-3/#computing-the-table-height

        // The minimum height of a row is the maximum of:
        // - the computed height (if definite, percentages being considered 0px) of its corresponding table-row (if nay)
        // - the computed height of each cell spanning the current row exclusively (if definite, percentages being treated as 0px), and
        // - the minimum height (ROWMIN) required by the cells spanning the row.
        // Note that we've already applied the first rule at the top of the method.
        if (cell.row_span == 1) {
            row.base_height = max(row.base_height, cell_state.border_box_height());
        }
        row.base_height = max(row.base_height, m_rows[cell.row_index].min_size);
        row.baseline = max(row.baseline, cell.baseline);
    }

    CSSPixels sum_rows_height = 0;
    for (auto& row : m_rows) {
        sum_rows_height += row.base_height;
    }

    m_table_height = sum_rows_height;

    if (!table_box().computed_values().height().is_auto()) {
        // If the table has a height property with a value other than auto, it is treated as a minimum height for the
        // table grid, and will eventually be distributed to the height of the rows if their collective minimum height
        // ends up smaller than this number.
        CSSPixels height_of_table_containing_block = m_state.get(*table_wrapper().containing_block()).content_height();
        auto specified_table_height = table_box().computed_values().height().to_px(table_box(), height_of_table_containing_block);
        if (table_box().computed_values().box_sizing() == CSS::BoxSizing::BorderBox) {
            auto const& table_state = m_state.get(table_box());
            specified_table_height -= table_state.border_box_top() + table_state.border_box_bottom();
        }
        m_table_height = max(m_table_height, specified_table_height);
    }

    for (auto& row : m_rows) {
        // Reference size is the largest of
        // - its initial base height and
        // - its new base height (the one evaluated during the second layout pass, where percentages used in
        //   rowgroups/rows/cells' specified heights were resolved according to the table height, instead of
        //   being ignored as 0px).

        // Assign reference size to base size. Later, the reference size might change to a larger value during
        // the second pass of rows layout.
        row.reference_height = row.base_height;
    }

    // Second pass of rows height calculation:
    // At this point, percentage row height can be resolved because the final table height is calculated.
    for (auto& row : m_rows) {
        auto row_computed_height = row.box->computed_values().height();
        if (row_computed_height.is_percentage()) {
            auto row_used_height = row_computed_height.to_px(row.box, m_table_height);
            row.reference_height = max(row.reference_height, row_used_height);
        } else {
            continue;
        }
    }

    // Second pass cells layout:
    // At this point, percentage cell height can be resolved because the final table height is calculated.
    for (auto& cell : m_cells) {
        auto& row = m_rows[cell.row_index];
        auto& cell_state = m_state.get_mutable(cell.box);

        CSSPixels span_width = 0;
        for (size_t i = 0; i < cell.column_span; ++i)
            span_width += m_columns[cell.column_index + i].used_width;

        auto cell_computed_height = cell.box->computed_values().height();
        if (cell_computed_height.is_percentage()) {
            auto cell_used_height = cell_computed_height.to_px(cell.box, m_table_height);
            cell_state.set_content_height(cell_used_height - cell_state.border_box_top() - cell_state.border_box_bottom());

            row.reference_height = max(row.reference_height, cell_used_height);
        } else {
            continue;
        }

        cell_state.set_content_width(span_width - cell_state.border_box_left() - cell_state.border_box_right() + (cell.column_span - 1) * border_spacing_horizontal());
        if (auto independent_formatting_context = layout_inside(cell.box, m_layout_mode, cell_state.available_inner_space_or_constraints_from(*m_available_space))) {
            independent_formatting_context->parent_context_did_dimension_child_root_box();
        }

        cell.baseline = box_baseline(cell.box);

        row.reference_height = max(row.reference_height, cell_state.border_box_height());
        row.baseline = max(row.baseline, cell.baseline);
    }
}

void TableFormattingContext::distribute_height_to_rows()
{
    CSSPixels sum_reference_height = 0;
    for (auto& row : m_rows) {
        sum_reference_height += row.reference_height;
    }

    if (sum_reference_height == 0)
        return;

    Vector<Row&> rows_with_auto_height;
    for (auto& row : m_rows) {
        if (row.box->computed_values().height().is_auto()) {
            rows_with_auto_height.append(row);
        }
    }

    if (m_table_height <= sum_reference_height) {
        // If the table height is equal or smaller than sum of reference sizes, the final height assigned to each row
        // will be the weighted mean of the base and the reference size that yields the correct total height.

        for (auto& row : m_rows) {
            auto weight = row.reference_height / static_cast<double>(sum_reference_height);
            auto final_height = m_table_height * weight;
            row.final_height = CSSPixels::nearest_value_for(final_height);
        }
    } else if (rows_with_auto_height.size() > 0) {
        // Else, if the table owns any “auto-height” row (a row whose size is only determined by its content size and
        // none of the specified heights), each non-auto-height row receives its reference height and auto-height rows
        // receive their reference size plus some increment which is equal to the height missing to amount to the
        // specified table height divided by the amount of such rows.

        for (auto& row : m_rows) {
            row.final_height = row.reference_height;
        }

        auto auto_height_rows_increment = (m_table_height - sum_reference_height) / rows_with_auto_height.size();
        for (auto& row : rows_with_auto_height) {
            row.final_height += auto_height_rows_increment;
        }
    } else {
        // Else, all rows receive their reference size plus some increment which is equal to the height missing to
        // amount to the specified table height divided by the amount of rows.

        auto increment = (m_table_height - sum_reference_height) / m_rows.size();
        for (auto& row : m_rows) {
            row.final_height = row.reference_height + increment;
        }
    }

    // Add undistributable space due to border spacing: https://www.w3.org/TR/css-tables-3/#computing-undistributable-space.
    m_table_height += (m_rows.size() + 1) * border_spacing_vertical();
}

void TableFormattingContext::position_row_boxes()
{
    auto const& table_state = m_state.get(table_box());

    CSSPixels row_top_offset = table_state.offset.y() + border_spacing_vertical();
    CSSPixels row_left_offset = table_state.border_left + table_state.padding_left + border_spacing_horizontal();
    for (size_t y = 0; y < m_rows.size(); y++) {
        auto& row = m_rows[y];
        auto& row_state = m_state.get_mutable(row.box);
        CSSPixels row_width = 0;
        for (auto& column : m_columns) {
            row_width += column.used_width;
        }
        if (m_columns.size() >= 2)
            row_width += (m_columns.size() - 1) * border_spacing_horizontal();

        row_state.set_content_height(row.final_height);
        row_state.set_content_width(row_width);
        row_state.set_content_x(row_left_offset);
        row_state.set_content_y(row_top_offset);
        row_top_offset += row_state.content_height() + border_spacing_vertical();
    }

    CSSPixels row_group_top_offset = table_state.offset.y() + border_spacing_vertical();
    CSSPixels row_group_left_offset = table_state.border_left + table_state.padding_left + border_spacing_horizontal();
    TableGrid::for_each_child_box_matching(table_box(), TableGrid::is_table_row_group, [&](auto& row_group_box) {
        CSSPixels row_group_height = 0;
        CSSPixels row_group_width = 0;

        auto& row_group_box_state = m_state.get_mutable(row_group_box);
        row_group_box_state.set_content_x(row_group_left_offset);
        row_group_box_state.set_content_y(row_group_top_offset);

        int num_rows = 0;
        TableGrid::for_each_child_box_matching(row_group_box, TableGrid::is_table_row, [&](auto& row) {
            auto const& row_state = m_state.get(row);
            row_group_height += row_state.border_box_height();
            row_group_width = max(row_group_width, row_state.border_box_width());
            num_rows += 1;
        });
        if (num_rows >= 2)
            row_group_height += (num_rows - 1) * border_spacing_vertical();

        row_group_box_state.set_content_height(row_group_height);
        row_group_box_state.set_content_width(row_group_width);

        row_group_top_offset += row_group_height + (num_rows > 0 ? border_spacing_vertical() : 0);
    });

    auto total_content_height = max(row_top_offset, row_group_top_offset) - table_state.offset.y() - table_state.padding_top;
    m_table_height = max(total_content_height, m_table_height);
}

void TableFormattingContext::position_cell_boxes()
{
    CSSPixels left_column_offset = 0;
    for (auto& column : m_columns) {
        column.left_offset = left_column_offset;
        left_column_offset += column.used_width;
    }

    for (auto& cell : m_cells) {
        auto& cell_state = m_state.get_mutable(cell.box);
        auto& row_state = m_state.get(m_rows[cell.row_index].box);
        auto const row_content_height = compute_row_content_height(cell);
        auto const& vertical_align = cell.box->computed_values().vertical_align();
        // The following image shows various alignment lines of a row:
        // https://www.w3.org/TR/css-tables-3/images/cell-align-explainer.png
        // https://drafts.csswg.org/css2/#height-layout
        // In the context of tables, values for vertical-align have the following meanings:
        if (vertical_align.has<CSS::VerticalAlign>()) {
            switch (vertical_align.get<CSS::VerticalAlign>()) {
            // The center of the cell is aligned with the center of the rows it spans.
            case CSS::VerticalAlign::Middle: {
                auto const height_diff = row_content_height - cell_state.border_box_height();
                cell_state.padding_top += height_diff / 2;
                cell_state.padding_bottom += height_diff / 2;
                break;
            }
            // The top of the cell box is aligned with the top of the first row it spans.
            case CSS::VerticalAlign::Top: {
                cell_state.padding_bottom += row_content_height - cell_state.border_box_height();
                break;
            }
            // The bottom of the cell box is aligned with the bottom of the last row it spans.
            case CSS::VerticalAlign::Bottom: {
                cell_state.padding_top += row_content_height - cell_state.border_box_height();
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
                cell_state.padding_bottom += row_content_height - cell_state.border_box_height();
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
        cell_state.offset = row_state.offset.translated(
            cell_state.border_box_left() + m_columns[cell.column_index].left_offset + cell.column_index * border_spacing_horizontal(),
            cell_state.border_box_top());
    }
}

bool TableFormattingContext::use_fixed_mode_layout() const
{
    // Implements https://www.w3.org/TR/css-tables-3/#in-fixed-mode.
    // A table-root is said to be laid out in fixed mode whenever the computed value of the table-layout property is equal to fixed, and the
    // specified width of the table root is either a <length-percentage>, min-content or fit-content. When the specified width is not one of
    // those values, or if the computed value of the table-layout property is auto, then the table-root is said to be laid out in auto mode.
    auto const& width = table_box().computed_values().width();
    return table_box().computed_values().table_layout() == CSS::TableLayout::Fixed && (width.is_length() || width.is_percentage() || width.is_min_content() || width.is_fit_content());
}

bool TableFormattingContext::border_is_less_specific(const CSS::BorderData& a, const CSS::BorderData& b)
{
    // Implements criteria for steps 1, 2 and 3 of border conflict resolution algorithm, as described in
    // https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution.
    static HashMap<CSS::LineStyle, unsigned> const line_style_score = {
        { CSS::LineStyle::Inset, 0 },
        { CSS::LineStyle::Groove, 1 },
        { CSS::LineStyle::Outset, 2 },
        { CSS::LineStyle::Ridge, 3 },
        { CSS::LineStyle::Dotted, 4 },
        { CSS::LineStyle::Dashed, 5 },
        { CSS::LineStyle::Solid, 6 },
        { CSS::LineStyle::Double, 7 },
    };

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
    if (a.width > b.width) {
        return false;
    } else if (a.width < b.width) {
        return true;
    }
    if (*line_style_score.get(a.line_style) > *line_style_score.get(b.line_style)) {
        return false;
    } else if (*line_style_score.get(a.line_style) < *line_style_score.get(b.line_style)) {
        return true;
    }
    return false;
}

const CSS::BorderData& TableFormattingContext::border_data_conflicting_edge(TableFormattingContext::ConflictingEdge const& conflicting_edge)
{
    auto const& style = conflicting_edge.element->computed_values();
    switch (conflicting_edge.side) {
    case ConflictingSide::Top: {
        return style.border_top();
    }
    case ConflictingSide::Bottom: {
        return style.border_bottom();
    }
    case ConflictingSide::Left: {
        return style.border_left();
    }
    case ConflictingSide::Right: {
        return style.border_right();
    }
    default: {
        VERIFY_NOT_REACHED();
    }
    }
}

Painting::PaintableBox::BorderDataWithElementKind const TableFormattingContext::border_data_with_element_kind_from_conflicting_edge(ConflictingEdge const& conflicting_edge)
{
    auto const& border_data = border_data_conflicting_edge(conflicting_edge);
    return { .border_data = border_data, .element_kind = conflicting_edge.element_kind };
}

TableFormattingContext::ConflictingEdge const& TableFormattingContext::winning_conflicting_edge(TableFormattingContext::ConflictingEdge const& a, TableFormattingContext::ConflictingEdge const& b)
{
    auto a_border_data = border_data_conflicting_edge(a);
    auto b_border_data = border_data_conflicting_edge(b);
    // First check if step 4 of border conflict resolution applies, as described in https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution.
    if (a_border_data.line_style == b_border_data.line_style && a_border_data.width == b_border_data.width) {
        // 4. If border styles differ only in color, then a style set on a cell wins over one on a row, which wins over a
        //    row group, column, column group and, lastly, table. When two elements of the same type conflict, then the one
        //    further to the left (if the table's 'direction' is 'ltr'; right, if it is 'rtl') and further to the top wins.
        if (static_cast<unsigned>(a.element_kind) < static_cast<unsigned>(b.element_kind)) {
            return a;
        } else if (static_cast<unsigned>(a.element_kind) > static_cast<unsigned>(b.element_kind)) {
            return b;
        }
        // Here the element kind is the same, thus the coordinates are either both set or not set.
        VERIFY(a.column.has_value() == b.column.has_value());
        VERIFY(a.row.has_value() == b.row.has_value());
        if (a.column.has_value()) {
            if (a.column.value() < b.column.value()) {
                return a;
            } else if (a.column.value() > b.column.value()) {
                return b;
            }
        }
        if (a.row.has_value()) {
            if (a.row.value() < b.row.value()) {
                return a;
            } else if (a.row.value() > b.row.value()) {
                return b;
            }
        }
        return a;
    }
    // Apply steps 1, 2 and 3 of the border conflict resolution algorithm.
    return border_is_less_specific(a_border_data, b_border_data) ? b : a;
}

void TableFormattingContext::border_conflict_resolution()
{
    // Implements border conflict resolution, as described in https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution.
    BorderConflictFinder finder(this);
    for (auto& cell : m_cells) {
        auto& cell_state = m_state.get_mutable(cell.box);
        cell_state.set_table_cell_coordinates(
            Painting::PaintableBox::TableCellCoordinates {
                .row_index = cell.row_index,
                .column_index = cell.column_index,
                .row_span = cell.row_span,
                .column_span = cell.column_span });
        if (cell.box->computed_values().border_collapse() == CSS::BorderCollapse::Separate) {
            continue;
        }
        Painting::PaintableBox::BordersDataWithElementKind override_borders_data;
        ConflictingEdge winning_edge_left {
            .element = cell.box,
            .element_kind = Painting::PaintableBox::ConflictingElementKind::Cell,
            .side = ConflictingSide::Left,
            .row = cell.row_index,
            .column = cell.column_index,
        };
        for (auto const& conflicting_edge : finder.conflicting_edges(cell, ConflictingSide::Left)) {
            winning_edge_left = winning_conflicting_edge(winning_edge_left, conflicting_edge);
        }
        override_borders_data.left = border_data_with_element_kind_from_conflicting_edge(winning_edge_left);
        cell_state.border_left = override_borders_data.left.border_data.width;
        ConflictingEdge winning_edge_right {
            .element = cell.box,
            .element_kind = Painting::PaintableBox::ConflictingElementKind::Cell,
            .side = ConflictingSide::Right,
            .row = cell.row_index,
            .column = cell.column_index,
        };
        for (auto const& conflicting_edge : finder.conflicting_edges(cell, ConflictingSide::Right)) {
            winning_edge_right = winning_conflicting_edge(winning_edge_right, conflicting_edge);
        }
        override_borders_data.right = border_data_with_element_kind_from_conflicting_edge(winning_edge_right);
        cell_state.border_right = override_borders_data.right.border_data.width;
        ConflictingEdge winning_edge_top {
            .element = cell.box,
            .element_kind = Painting::PaintableBox::ConflictingElementKind::Cell,
            .side = ConflictingSide::Top,
            .row = cell.row_index,
            .column = cell.column_index,
        };
        for (auto const& conflicting_edge : finder.conflicting_edges(cell, ConflictingSide::Top)) {
            winning_edge_top = winning_conflicting_edge(winning_edge_top, conflicting_edge);
        }
        override_borders_data.top = border_data_with_element_kind_from_conflicting_edge(winning_edge_top);
        cell_state.border_top = override_borders_data.top.border_data.width;
        ConflictingEdge winning_edge_bottom {
            .element = cell.box,
            .element_kind = Painting::PaintableBox::ConflictingElementKind::Cell,
            .side = ConflictingSide::Bottom,
            .row = cell.row_index,
            .column = cell.column_index,
        };
        for (auto const& conflicting_edge : finder.conflicting_edges(cell, ConflictingSide::Bottom)) {
            winning_edge_bottom = winning_conflicting_edge(winning_edge_bottom, conflicting_edge);
        }
        override_borders_data.bottom = border_data_with_element_kind_from_conflicting_edge(winning_edge_bottom);
        cell_state.border_bottom = override_borders_data.bottom.border_data.width;
        cell_state.set_override_borders_data(override_borders_data);
    }
}

CSSPixels TableFormattingContext::compute_row_content_height(Cell const& cell) const
{
    auto& row_state = m_state.get(m_rows[cell.row_index].box);
    if (cell.row_span == 1) {
        return row_state.content_height();
    }
    // The height of a cell is the sum of all spanned rows, as described in
    // https://www.w3.org/TR/css-tables-3/#bounding-box-assignment

    // When the row span is greater than 1, the borders of inner rows within the span have to be
    // included in the content height of the spanning cell. First top and final bottom borders are
    // excluded to be consistent with the handling of row span 1 case above, which uses the content
    // height (no top and bottom borders) of the row.
    CSSPixels span_height = 0;
    for (size_t i = 0; i < cell.row_span; ++i) {
        auto const& row_state = m_state.get(m_rows[cell.row_index + i].box);
        if (i == 0) {
            span_height += row_state.content_height() + row_state.border_box_bottom();
        } else if (i == cell.row_span - 1) {
            span_height += row_state.border_box_top() + row_state.content_height();
        } else {
            span_height += row_state.border_box_height();
        }
    }

    // Compute cell height as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
    // width/height is the sum of:
    // - the widths/heights of all spanned visible columns/rows
    // - the horizontal/vertical border-spacing times the amount of spanned visible columns/rows minus one
    // FIXME: Account for visibility.
    span_height += (cell.row_span - 1) * border_spacing_vertical();
    return span_height;
}

TableFormattingContext::BorderConflictFinder::BorderConflictFinder(TableFormattingContext const* context)
    : m_context(context)
{
    collect_conflicting_col_elements();
    collect_conflicting_row_group_elements();
}

void TableFormattingContext::BorderConflictFinder::collect_conflicting_col_elements()
{
    m_col_elements_by_index.resize(m_context->m_columns.size());
    for (auto* child = m_context->table_box().first_child(); child; child = child->next_sibling()) {
        if (!child->display().is_table_column_group()) {
            continue;
        }
        size_t column_index = 0;
        for (auto* child_of_column_group = child->first_child(); child_of_column_group; child_of_column_group = child_of_column_group->next_sibling()) {
            VERIFY(child_of_column_group->display().is_table_column());
            auto const& col_node = static_cast<HTML::HTMLTableColElement const&>(*child_of_column_group->dom_node());
            unsigned span = col_node.get_attribute_value(HTML::AttributeNames::span).to_number<unsigned>().value_or(1);
            m_col_elements_by_index.resize(column_index + span);
            for (size_t i = column_index; i < column_index + span; ++i) {
                m_col_elements_by_index[i] = child_of_column_group;
            }
            column_index += span;
        }
    }
}

void TableFormattingContext::BorderConflictFinder::collect_conflicting_row_group_elements()
{
    m_row_group_elements_by_index.resize(m_context->m_rows.size());
    size_t current_row_index = 0;
    TableGrid::for_each_child_box_matching(m_context->table_box(), TableGrid::is_table_row_group, [&](auto& row_group_box) {
        auto start_row_index = current_row_index;
        size_t row_count = 0;
        TableGrid::for_each_child_box_matching(row_group_box, TableGrid::is_table_row, [&](auto&) {
            ++row_count;
        });
        TableGrid::for_each_child_box_matching(row_group_box, TableGrid::is_table_row, [&](auto&) {
            m_row_group_elements_by_index[current_row_index] = RowGroupInfo {
                .row_group = &row_group_box, .start_index = start_row_index, .row_count = row_count
            };
            ++current_row_index;
            return IterationDecision::Continue;
        });
    });
}

void TableFormattingContext::BorderConflictFinder::collect_cell_conflicting_edges(Vector<ConflictingEdge>& result, Cell const& cell, TableFormattingContext::ConflictingSide edge) const
{
    // Right edge of the cell to the left.
    if (cell.column_index >= cell.column_span && edge == ConflictingSide::Left) {
        auto left_cell_column_index = cell.column_index - cell.column_span;
        auto maybe_cell_to_left = m_context->m_cells_by_coordinate[cell.row_index][left_cell_column_index];
        if (maybe_cell_to_left.has_value()) {
            result.append({ maybe_cell_to_left->box, Painting::PaintableBox::ConflictingElementKind::Cell, ConflictingSide::Right, cell.row_index, left_cell_column_index });
        }
    }
    // Left edge of the cell to the right.
    if (cell.column_index + cell.column_span < m_context->m_cells_by_coordinate[cell.row_index].size() && edge == ConflictingSide::Right) {
        auto right_cell_column_index = cell.column_index + cell.column_span;
        auto maybe_cell_to_right = m_context->m_cells_by_coordinate[cell.row_index][right_cell_column_index];
        if (maybe_cell_to_right.has_value()) {
            result.append({ maybe_cell_to_right->box, Painting::PaintableBox::ConflictingElementKind::Cell, ConflictingSide::Left, cell.row_index, right_cell_column_index });
        }
    }
    // Bottom edge of the cell above.
    if (cell.row_index >= cell.row_span && edge == ConflictingSide::Top) {
        auto above_cell_row_index = cell.row_index - cell.row_span;
        auto maybe_cell_above = m_context->m_cells_by_coordinate[above_cell_row_index][cell.column_index];
        if (maybe_cell_above.has_value()) {
            result.append({ maybe_cell_above->box, Painting::PaintableBox::ConflictingElementKind::Cell, ConflictingSide::Bottom, above_cell_row_index, cell.column_index });
        }
    }
    // Top edge of the cell below.
    if (cell.row_index + cell.row_span < m_context->m_cells_by_coordinate.size() && edge == ConflictingSide::Bottom) {
        auto below_cell_row_index = cell.row_index + cell.row_span;
        auto maybe_cell_below = m_context->m_cells_by_coordinate[below_cell_row_index][cell.column_index];
        if (maybe_cell_below.has_value()) {
            result.append({ maybe_cell_below->box, Painting::PaintableBox::ConflictingElementKind::Cell, ConflictingSide::Top, below_cell_row_index, cell.column_index });
        }
    }
}

void TableFormattingContext::BorderConflictFinder::collect_row_conflicting_edges(Vector<ConflictingEdge>& result, Cell const& cell, TableFormattingContext::ConflictingSide edge) const
{
    // Top edge of the row.
    if (edge == ConflictingSide::Top) {
        result.append({ m_context->m_rows[cell.row_index].box, Painting::PaintableBox::ConflictingElementKind::Row, ConflictingSide::Top, cell.row_index, {} });
    }
    // Bottom edge of the row.
    if (edge == ConflictingSide::Bottom) {
        result.append({ m_context->m_rows[cell.row_index].box, Painting::PaintableBox::ConflictingElementKind::Row, ConflictingSide::Bottom, cell.row_index, {} });
    }
    // Bottom edge of the row above.
    if (cell.row_index >= cell.row_span && edge == ConflictingSide::Top) {
        auto above_row_index = cell.row_index - cell.row_span;
        result.append({ m_context->m_rows[above_row_index].box, Painting::PaintableBox::ConflictingElementKind::Row, ConflictingSide::Bottom, above_row_index, {} });
    }
    // Top edge of the row below.
    if (cell.row_index + cell.row_span < m_context->m_rows.size() && edge == ConflictingSide::Bottom) {
        auto below_row_index = cell.row_index + cell.row_span;
        result.append({ m_context->m_rows[below_row_index].box, Painting::PaintableBox::ConflictingElementKind::Row, ConflictingSide::Top, below_row_index, {} });
    }
}

void TableFormattingContext::BorderConflictFinder::collect_row_group_conflicting_edges(Vector<ConflictingEdge>& result, Cell const& cell, TableFormattingContext::ConflictingSide edge) const
{
    auto const& maybe_row_group = m_row_group_elements_by_index[cell.row_index];
    // Top edge of the row group.
    if (maybe_row_group.has_value() && cell.row_index == maybe_row_group->start_index && edge == ConflictingSide::Top) {
        result.append({ maybe_row_group->row_group, Painting::PaintableBox::ConflictingElementKind::RowGroup, ConflictingSide::Top, maybe_row_group->start_index, {} });
    }
    // Bottom edge of the row group above.
    if (cell.row_index >= cell.row_span) {
        auto const& maybe_row_group_above = m_row_group_elements_by_index[cell.row_index - cell.row_span];
        if (maybe_row_group_above.has_value() && cell.row_index == maybe_row_group_above->start_index + maybe_row_group_above->row_count && edge == ConflictingSide::Top) {
            result.append({ maybe_row_group_above->row_group, Painting::PaintableBox::ConflictingElementKind::RowGroup, ConflictingSide::Bottom, maybe_row_group_above->start_index, {} });
        }
    }
    // Bottom edge of the row group.
    if (maybe_row_group.has_value() && cell.row_index == maybe_row_group->start_index + maybe_row_group->row_count - 1 && edge == ConflictingSide::Bottom) {
        result.append({ maybe_row_group->row_group, Painting::PaintableBox::ConflictingElementKind::RowGroup, ConflictingSide::Bottom, maybe_row_group->start_index, {} });
    }
    // Top edge of the row group below.
    if (cell.row_index + cell.row_span < m_row_group_elements_by_index.size()) {
        auto const& maybe_row_group_below = m_row_group_elements_by_index[cell.row_index + cell.row_span];
        if (maybe_row_group_below.has_value() && cell.row_index + cell.row_span == maybe_row_group_below->start_index && edge == ConflictingSide::Bottom) {
            result.append({ maybe_row_group_below->row_group, Painting::PaintableBox::ConflictingElementKind::RowGroup, ConflictingSide::Top, maybe_row_group_below->start_index, {} });
        }
    }
}

void TableFormattingContext::BorderConflictFinder::collect_column_group_conflicting_edges(Vector<ConflictingEdge>& result, Cell const& cell, TableFormattingContext::ConflictingSide edge) const
{
    // Left edge of the column group.
    if (auto col_element = get_col_element(cell.column_index); col_element && edge == ConflictingSide::Left) {
        result.append({ col_element, Painting::PaintableBox::ConflictingElementKind::ColumnGroup, ConflictingSide::Left, {}, cell.column_index });
    }
    // Right edge of the column group to the left.
    if (auto col_element = get_col_element(cell.column_index - cell.column_span); col_element && cell.column_index >= cell.column_span && edge == ConflictingSide::Left) {
        result.append({ col_element, Painting::PaintableBox::ConflictingElementKind::ColumnGroup, ConflictingSide::Right, {}, cell.column_index - cell.column_span });
    }
    // Right edge of the column group.
    if (auto col_element = get_col_element(cell.column_index); col_element && edge == ConflictingSide::Right) {
        result.append({ col_element, Painting::PaintableBox::ConflictingElementKind::ColumnGroup, ConflictingSide::Right, {}, cell.column_index });
    }
    // Left edge of the column group to the right.
    if (auto col_element = get_col_element(cell.column_index - cell.column_span); col_element && edge == ConflictingSide::Right) {
        result.append({ col_element, Painting::PaintableBox::ConflictingElementKind::ColumnGroup, ConflictingSide::Left, {}, cell.column_index + cell.column_span });
    }
}

void TableFormattingContext::BorderConflictFinder::collect_table_box_conflicting_edges(Vector<ConflictingEdge>& result, Cell const& cell, TableFormattingContext::ConflictingSide edge) const
{
    // Top edge from column group or table. Left and right edges of the column group are handled in collect_column_group_conflicting_edges.
    if (cell.row_index == 0 && edge == ConflictingSide::Top) {
        if (auto col_element = get_col_element(cell.column_index); col_element) {
            result.append({ col_element, Painting::PaintableBox::ConflictingElementKind::ColumnGroup, ConflictingSide::Top, {}, cell.column_index });
        }
        result.append({ &m_context->table_box(), Painting::PaintableBox::ConflictingElementKind::Table, ConflictingSide::Top, {}, {} });
    }
    // Bottom edge from column group or table. Left and right edges of the column group are handled in collect_column_group_conflicting_edges.
    if (cell.row_index + cell.row_span == m_context->m_rows.size() && edge == ConflictingSide::Bottom) {
        if (auto col_element = get_col_element(cell.column_index); col_element) {
            result.append({ col_element, Painting::PaintableBox::ConflictingElementKind::ColumnGroup, ConflictingSide::Bottom, {}, cell.column_index });
        }
        result.append({ &m_context->table_box(), Painting::PaintableBox::ConflictingElementKind::Table, ConflictingSide::Bottom, {}, {} });
    }
    // Left edge from row group or table. Top and bottom edges of the row group are handled in collect_row_group_conflicting_edges.
    if (cell.column_index == 0 && edge == ConflictingSide::Left) {
        result.append({ m_context->m_rows[cell.row_index].box, Painting::PaintableBox::ConflictingElementKind::Row, ConflictingSide::Left, cell.row_index, {} });
        if (m_row_group_elements_by_index[cell.row_index].has_value()) {
            result.append({ m_row_group_elements_by_index[cell.row_index]->row_group, Painting::PaintableBox::ConflictingElementKind::RowGroup, ConflictingSide::Left, cell.row_index, {} });
        }
        result.append({ &m_context->table_box(), Painting::PaintableBox::ConflictingElementKind::Table, ConflictingSide::Left, {}, {} });
    }
    // Right edge from row group or table. Top and bottom edges of the row group are handled in collect_row_group_conflicting_edges.
    if (cell.column_index + cell.column_span == m_context->m_columns.size() && edge == ConflictingSide::Right) {
        result.append({ m_context->m_rows[cell.row_index].box, Painting::PaintableBox::ConflictingElementKind::Row, ConflictingSide::Right, cell.row_index, {} });
        if (m_row_group_elements_by_index[cell.row_index].has_value()) {
            result.append({ m_row_group_elements_by_index[cell.row_index]->row_group, Painting::PaintableBox::ConflictingElementKind::RowGroup, ConflictingSide::Right, cell.row_index, {} });
        }
        result.append({ &m_context->table_box(), Painting::PaintableBox::ConflictingElementKind::Table, ConflictingSide::Right, {}, {} });
    }
}

Vector<TableFormattingContext::ConflictingEdge> TableFormattingContext::BorderConflictFinder::conflicting_edges(
    Cell const& cell, TableFormattingContext::ConflictingSide edge) const
{
    Vector<ConflictingEdge> result = {};
    collect_cell_conflicting_edges(result, cell, edge);
    collect_row_conflicting_edges(result, cell, edge);
    collect_row_group_conflicting_edges(result, cell, edge);
    collect_column_group_conflicting_edges(result, cell, edge);
    collect_table_box_conflicting_edges(result, cell, edge);
    return result;
}

void TableFormattingContext::finish_grid_initialization(TableGrid const& table_grid)
{
    m_columns.resize(table_grid.column_count());
    m_cells_by_coordinate.resize(m_rows.size());
    for (auto& position_to_cell_row : m_cells_by_coordinate) {
        position_to_cell_row.resize(table_grid.column_count());
    }
    for (auto const& cell : m_cells) {
        m_cells_by_coordinate[cell.row_index][cell.column_index] = cell;
        m_columns[cell.column_index].has_originating_cells = true;
    }
}

void TableFormattingContext::run_until_width_calculation(AvailableSpace const& available_space)
{
    m_available_space = available_space;

    // Determine the number of rows/columns the table requires.
    finish_grid_initialization(TableGrid::calculate_row_column_grid(context_box(), m_cells, m_rows));

    border_conflict_resolution();

    // Compute the minimum width of each column.
    compute_cell_measures();
    compute_outer_content_sizes();
    compute_table_measures<Column>();

    // https://www.w3.org/TR/css-tables-3/#row-layout
    // Since during row layout the specified heights of cells in the row were ignored and cells that were spanning more than one rows
    // have not been sized correctly, their height will need to be eventually distributed to the set of rows they spanned. This is done
    // by running the same algorithm as the column measurement, with the span=1 value being initialized (for min-content) with the largest
    // of the resulting height of the previous row layout, the height specified on the corresponding table-row (if any), and the largest
    // height specified on cells that span this row only (the algorithm starts by considering cells of span 2 on top of that assignment).
    compute_table_measures<Row>();

    // Compute the width of the table.
    compute_table_width();
}

void TableFormattingContext::parent_context_did_dimension_child_root_box()
{
    if (m_layout_mode != LayoutMode::Normal)
        return;

    context_box().for_each_in_subtree_of_type<Box const>([&](Layout::Box const& box) {
        if (box.is_absolutely_positioned()) {
            // FIXME: calculate_static_position_rect() is not aware of how to correctly calculate static position for
            //        a box nested inside a table, but we need to set some value, so layout_absolutely_positioned_element()
            //        won't crash trying to access it.
            m_state.get_mutable(box).set_static_position_rect(calculate_static_position_rect(box));
        }

        if (formatting_context_type_created_by_box(box).has_value()) {
            return TraversalDecision::SkipChildrenAndContinue;
        }

        return TraversalDecision::Continue;
    });

    for (auto& child : context_box().contained_abspos_children()) {
        auto& box = as<Box>(*child);
        auto& cb_state = m_state.get(*box.containing_block());
        auto available_width = AvailableSize::make_definite(cb_state.content_width() + cb_state.padding_left + cb_state.padding_right);
        auto available_height = AvailableSize::make_definite(cb_state.content_height() + cb_state.padding_top + cb_state.padding_bottom);
        layout_absolutely_positioned_element(box, AvailableSpace(available_width, available_height));
    }
}

void TableFormattingContext::run(AvailableSpace const& available_space)
{
    m_available_space = available_space;

    auto total_captions_height = run_caption_layout(CSS::CaptionSide::Top);

    run_until_width_calculation(available_space);

    if (available_space.width.is_intrinsic_sizing_constraint() && !available_space.height.is_intrinsic_sizing_constraint()) {
        return;
    }

    // Distribute the width of the table among columns.
    distribute_width_to_columns();

    compute_table_height();

    distribute_height_to_rows();

    position_row_boxes();
    position_cell_boxes();

    m_state.get_mutable(table_box()).set_content_height(m_table_height);

    total_captions_height += run_caption_layout(CSS::CaptionSide::Bottom);

    // Table captions are positioned between the table margins and its borders (outside the grid box borders) as described in
    // https://www.w3.org/TR/css-tables-3/#bounding-box-assignment
    // A visual representation of this model can be found at https://www.w3.org/TR/css-tables-3/images/table_container.png
    m_state.get_mutable(table_box()).margin_bottom += total_captions_height;

    m_automatic_content_height = m_table_height;
}

CSSPixels TableFormattingContext::automatic_content_width() const
{
    return greatest_child_width(context_box());
}

CSSPixels TableFormattingContext::automatic_content_height() const
{
    return m_automatic_content_height;
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
    return cell.outer_min_height;
}

template<>
CSSPixels TableFormattingContext::cell_min_size<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_min_width;
}

template<>
CSSPixels TableFormattingContext::cell_max_size<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_max_height;
}

template<>
CSSPixels TableFormattingContext::cell_max_size<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.outer_max_width;
}

template<>
double TableFormattingContext::cell_percentage_contribution<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
    auto const& computed_values = cell.box->computed_values();
    auto max_height_percentage = computed_values.max_height().is_percentage() ? computed_values.max_height().percentage().value() : static_cast<double>(INFINITY);
    auto height_percentage = computed_values.height().is_percentage() ? computed_values.height().percentage().value() : 0;
    return min(height_percentage, max_height_percentage);
}

template<>
double TableFormattingContext::cell_percentage_contribution<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
    auto const& computed_values = cell.box->computed_values();
    auto max_width_percentage = computed_values.max_width().is_percentage() ? computed_values.max_width().percentage().value() : static_cast<double>(INFINITY);
    auto width_percentage = computed_values.width().is_percentage() ? computed_values.width().percentage().value() : 0;
    return min(width_percentage, max_width_percentage);
}

template<>
bool TableFormattingContext::cell_has_intrinsic_percentage<TableFormattingContext::Row>(TableFormattingContext::Cell const& cell)
{
    return cell.box->computed_values().height().is_percentage();
}

template<>
bool TableFormattingContext::cell_has_intrinsic_percentage<TableFormattingContext::Column>(TableFormattingContext::Cell const& cell)
{
    return cell.box->computed_values().width().is_percentage();
}

template<>
void TableFormattingContext::initialize_intrinsic_percentages_from_rows_or_columns<TableFormattingContext::Row>()
{
    for (auto& row : m_rows) {
        auto const& computed_values = row.box->computed_values();
        // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
        auto max_height_percentage = computed_values.max_height().is_percentage() ? computed_values.max_height().percentage().value() : static_cast<double>(INFINITY);
        auto height_percentage = computed_values.height().is_percentage() ? computed_values.height().percentage().value() : 0;
        row.has_intrinsic_percentage = computed_values.max_height().is_percentage() || computed_values.height().is_percentage();
        row.intrinsic_percentage = min(height_percentage, max_height_percentage);
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
            auto max_width_percentage = computed_values.max_width().is_percentage() ? computed_values.max_width().percentage().value() : static_cast<double>(INFINITY);
            auto width_percentage = computed_values.width().is_percentage() ? computed_values.width().percentage().value() : 0;
            m_columns[column_index].has_intrinsic_percentage = computed_values.max_width().is_percentage() || computed_values.width().is_percentage();
            m_columns[column_index].intrinsic_percentage = min(width_percentage, max_width_percentage);
            auto const& col_node = static_cast<HTML::HTMLTableColElement const&>(*column_box.dom_node());
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
    return border_spacing_vertical();
}

template<>
CSSPixels TableFormattingContext::border_spacing<TableFormattingContext::Column>()
{
    return border_spacing_horizontal();
}

CSSPixels TableFormattingContext::border_spacing_horizontal() const
{
    auto const& computed_values = table_box().computed_values();
    // When a table is laid out in collapsed-borders mode, the border-spacing of the table-root is ignored (as if it was set to 0px):
    // https://www.w3.org/TR/css-tables-3/#collapsed-style-overrides
    if (computed_values.border_collapse() == CSS::BorderCollapse::Collapse)
        return 0;
    return computed_values.border_spacing_horizontal().to_px(table_box());
}

CSSPixels TableFormattingContext::border_spacing_vertical() const
{
    auto const& computed_values = table_box().computed_values();
    // When a table is laid out in collapsed-borders mode, the border-spacing of the table-root is ignored (as if it was set to 0px):
    // https://www.w3.org/TR/css-tables-3/#collapsed-style-overrides
    if (computed_values.border_collapse() == CSS::BorderCollapse::Collapse)
        return 0;
    return computed_values.border_spacing_vertical().to_px(table_box());
}

StaticPositionRect TableFormattingContext::calculate_static_position_rect(Box const& box) const
{
    // FIXME: Implement static position calculation for table descendants instead of always returning a rectangle with zero position and size.
    StaticPositionRect static_position;
    auto offset_to_static_parent = content_box_rect_in_static_position_ancestor_coordinate_space(box, *box.containing_block());
    static_position.rect = { offset_to_static_parent.location(), { 0, 0 } };
    return static_position;
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
