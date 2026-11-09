/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::css_pixels::CssPixels;
use crate::css::style::bridge::{FfiFrameSetDimension, FfiFrameSetDimensionUnit};

// NB: Pixel values are integers, as in other browsers. This bound keeps the products below in range of an i64.
const MAXIMUM_PIXEL_VALUE: i64 = i32::MAX as i64;

fn integer_pixels(value: f64) -> i64 {
    value.floor().clamp(0.0, MAXIMUM_PIXEL_VALUE as f64) as i64
}

fn indices_with_unit(input_list: &[FfiFrameSetDimension], unit: FfiFrameSetDimensionUnit) -> Vec<usize> {
    (0..input_list.len())
        .filter(|&index| input_list[index].unit == unit)
        .collect()
}

/// Sets the entries to their share of `space`, in proportion to their values, and answers how much of it they took.
fn scale_entries_to_space(output_list: &mut [i64], indices: &[usize], total: i64, space: i64) -> i64 {
    let mut taken = 0;
    for &index in indices {
        output_list[index] = output_list[index] * space / total;
        taken += output_list[index];
    }
    taken
}

/// Grows the entries by their share of `space`, in proportion to their values, and answers how much of it they took.
fn grow_entries_by_share_of_space(output_list: &mut [i64], indices: &[usize], total: i64, space: i64) -> i64 {
    let mut taken = 0;
    for &index in indices {
        let growth = output_list[index] * space / total;
        output_list[index] += growth;
        taken += growth;
    }
    taken
}

/// Grows each entry by the same amount, and answers how much of `space` they took.
fn grow_entries_equally(output_list: &mut [i64], indices: &[usize], space: i64) -> i64 {
    let growth = space / indices.len() as i64;
    for &index in indices {
        output_list[index] += growth;
    }
    growth * indices.len() as i64
}

// https://html.spec.whatwg.org/multipage/rendering.html#convert-a-list-of-dimensions-to-a-list-of-pixel-values
fn convert_a_list_of_dimensions_to_a_list_of_pixel_values(
    input_list: &[FfiFrameSetDimension],
    input_dimension: i64,
) -> Vec<i64> {
    // 1. Let input list be the list of numbers and units passed to the algorithm.
    //    Let output list be a list of numbers the same length as input list, all zero.
    //    Entries in output list correspond to the entries in input list that have the same position.
    let mut output_list = vec![0i64; input_list.len()];

    // 2. Let input dimension be the size passed to the algorithm.
    let absolute_indices = indices_with_unit(input_list, FfiFrameSetDimensionUnit::Absolute);
    let percentage_indices = indices_with_unit(input_list, FfiFrameSetDimensionUnit::Percentage);
    let relative_indices = indices_with_unit(input_list, FfiFrameSetDimensionUnit::Relative);

    // 3. Let total percentage be the sum of all the numbers in input list whose unit is percentage.
    //    Let total relative be the sum of all the numbers in input list whose unit is relative.
    //    Let total absolute be the sum of all the numbers in input list whose unit is absolute.
    //    Let remaining space be the value of input dimension.
    // INTEROP: The totals of absolute and percentage entries are totals of their sizes in integer pixels, and each
    //          relative entry weighs at least one.
    for &index in &absolute_indices {
        output_list[index] = integer_pixels(input_list[index].value);
    }
    for &index in &percentage_indices {
        output_list[index] = integer_pixels(input_list[index].value * input_dimension as f64 / 100.0);
    }
    let relative_weight = |index: usize| integer_pixels(input_list[index].value.max(1.0));
    let total_absolute: i64 = absolute_indices.iter().map(|&index| output_list[index]).sum();
    let total_percentage: i64 = percentage_indices.iter().map(|&index| output_list[index]).sum();
    let total_relative: i64 = relative_indices.iter().map(|&index| relative_weight(index)).sum();
    let mut remaining_space = input_dimension;

    // 4. If total absolute is greater than remaining space, then for each entry in input list whose unit is absolute,
    //    set the corresponding value in output list to the number of the entry in input list multiplied by remaining
    //    space and divided by total absolute. Then, set remaining space to zero.
    //    Otherwise, for each entry in input list whose unit is absolute, set the corresponding value in output list to
    //    the number of the entry in input list. Then, decrement remaining space by total absolute.
    if total_absolute > remaining_space {
        remaining_space -= scale_entries_to_space(&mut output_list, &absolute_indices, total_absolute, remaining_space);
    } else {
        remaining_space -= total_absolute;
    }

    // 5. If total percentage multiplied by the input dimension and divided by 100 is greater than remaining space, then
    //    for each entry in input list whose unit is percentage, set the corresponding value in output list to the
    //    number of the entry in input list multiplied by remaining space and divided by total percentage. Then, set
    //    remaining space to zero.
    //    Otherwise, for each entry in input list whose unit is percentage, set the corresponding value in output list
    //    to the number of the entry in input list multiplied by the input dimension and divided by 100. Then, decrement
    //    remaining space by total percentage multiplied by the input dimension and divided by 100.
    if total_percentage > remaining_space {
        remaining_space -=
            scale_entries_to_space(&mut output_list, &percentage_indices, total_percentage, remaining_space);
    } else {
        remaining_space -= total_percentage;
    }

    // 6. For each entry in input list whose unit is relative, set the corresponding value in output list to the number
    //    of the entry in input list multiplied by remaining space and divided by total relative.
    if let Some(&last_relative_index) = relative_indices.last() {
        let space_for_relative_entries = remaining_space;
        for &index in &relative_indices {
            output_list[index] = relative_weight(index) * space_for_relative_entries / total_relative;
            remaining_space -= output_list[index];
        }
        // User agents working with integer values for frame widths (as opposed to user agents that can lay frames out
        // with subpixel accuracy) are expected to distribute the remainder first to the last entry whose unit is
        // relative, then equally (not proportionally) to each entry whose unit is percentage, then equally (not
        // proportionally) to each entry whose unit is absolute, and finally, failing all else, to the last entry.
        output_list[last_relative_index] += remaining_space;
        remaining_space = 0;
    }

    // INTEROP: Space that no relative entry takes goes to the percentage entries in proportion to their sizes, or
    //          failing that, to the absolute entries in proportion to theirs. Only what rounding leaves over is
    //          distributed equally.
    if remaining_space > 0 && total_percentage > 0 {
        let space = remaining_space;
        remaining_space -=
            grow_entries_by_share_of_space(&mut output_list, &percentage_indices, total_percentage, space);
    } else if remaining_space > 0 && total_absolute > 0 {
        let space = remaining_space;
        remaining_space -= grow_entries_by_share_of_space(&mut output_list, &absolute_indices, total_absolute, space);
    }
    if remaining_space > 0 && !percentage_indices.is_empty() {
        remaining_space -= grow_entries_equally(&mut output_list, &percentage_indices, remaining_space);
    } else if remaining_space > 0 && !absolute_indices.is_empty() {
        remaining_space -= grow_entries_equally(&mut output_list, &absolute_indices, remaining_space);
    }
    if remaining_space > 0
        && let Some(last) = output_list.last_mut()
    {
        *last += remaining_space;
    }

    // 7. Return output list.
    output_list
}

/// The sizes of a frameset's tracks along an axis of its surface.
// INTEROP: The specification draws borders on the rectangles. In other browsers, the borders between tracks take their
//          space from the tracks.
pub(crate) fn frameset_track_sizes(
    dimensions: &[FfiFrameSetDimension],
    surface_size: CssPixels,
    border_thickness: u32,
) -> Vec<CssPixels> {
    let borders = (dimensions.len() as i64 - 1).max(0) * i64::from(border_thickness);
    let input_dimension = (i64::from(surface_size.to_int()) - borders).max(0);
    convert_a_list_of_dimensions_to_a_list_of_pixel_values(dimensions, input_dimension)
        .into_iter()
        .map(CssPixels::from_integer)
        .collect()
}

fn layout_frameset_child(
    run: &formatting_context::FormattingContextRun,
    child: Node,
    size: (CssPixels, CssPixels),
    offset: used_values::FfiCssPixelPoint,
) {
    let (inline_size, block_size) = size;
    let constraints = geometry::ContainingBlockConstraints {
        percentage_basis_inline_size: Some(inline_size),
        percentage_basis_block_size: Some(block_size),
        quirks_mode_percentage_basis_block_size: None,
    };
    let mut cells = used_values::cells_before_sizing(&run.pass, child, constraints);
    let style = StyleValues::for_node(&run.pass, child);
    cells.border_left = style.border_left_width();
    cells.border_right = style.border_right_width();
    cells.border_top = style.border_top_width();
    cells.border_bottom = style.border_bottom_width();
    cells.padding_left = style.padding_left().to_px(inline_size);
    cells.padding_right = style.padding_right().to_px(inline_size);
    cells.padding_top = style.padding_top().to_px(inline_size);
    cells.padding_bottom = style.padding_bottom().to_px(inline_size);
    cells.set_content_inline_size(
        (inline_size - cells.border_left - cells.padding_left - cells.padding_right - cells.border_right)
            .max(CssPixels::default()),
    );
    cells.set_content_block_size(
        (block_size - cells.border_top - cells.padding_top - cells.padding_bottom - cells.border_bottom)
            .max(CssPixels::default()),
    );
    let offset = used_values::FfiCssPixelPoint {
        x: offset.x + cells.border_left + cells.padding_left,
        y: offset.y + cells.border_top + cells.padding_top,
    };
    let plan = formatting_context::ChildRunPlan {
        fc_type: formatting_context::independent_formatting_context_type(child, &run.pass),
        root_containing_block: run.pass.in_flow_containing_block(child),
        input: geometry::LayoutInput {
            available_space: geometry::AvailableSpace {
                inline_size: geometry::AvailableSize::definite(inline_size),
                block_size: geometry::AvailableSize::definite(block_size),
            },
            containing_block_constraints: constraints,
            sizing: geometry::RootSizingDirectives::default(),
            participation: geometry::ParticipationInParentFormattingContext::Item,
        },
    };
    let outcome = formatting_context::run_formatting_context(
        run.pass,
        formatting_context::RunRequest::of_child(run, child, plan, cells),
        formatting_context::RunSideInputs::default(),
    );
    formatting_context::place_child(
        run,
        child,
        outcome.placed_child(),
        offset,
        None,
        outcome.into_fragment_arrival(run),
    );
}

// https://html.spec.whatwg.org/multipage/rendering.html#frames-and-framesets
/// The formatting context of a frameset, which lays its frames out in the grid of its rows and columns.
#[derive(Default)]
pub(crate) struct FrameSetFormattingContext;

impl<'pass> formatting_context::FormattingContextAlgorithm<'pass> for FrameSetFormattingContext {
    fn lay_out(
        &mut self,
        run: &formatting_context::FormattingContextRun<'pass>,
        input: formatting_context::AlgorithmInput<'_>,
    ) -> formatting_context::AlgorithmOutput {
        let mut root = input.root_cells;
        let result = layout_frameset(run, &mut root);
        formatting_context::AlgorithmOutput::of_root(result, root)
    }
}

fn layout_frameset(
    run: &formatting_context::FormattingContextRun,
    root: &mut used_values::RootInputCells,
) -> formatting_context::ChildLayoutResult {
    // NB: The UA style sheet gives a frameset the size of the viewport, and a nested frameset has both sizes from its
    //     rectangle.
    let (surface_inline_size, surface_block_size) = (root.content_inline_size, root.content_block_size);
    root.has_definite_inline_size = true;
    root.has_definite_block_size = true;
    let result = formatting_context::ChildLayoutResult {
        automatic_content_block_size: surface_block_size,
        ..formatting_context::ChildLayoutResult::default()
    };

    let layout = run
        .pass
        .arena()
        .replaced_paint_facts(run.box_)
        .and_then(|facts| facts.frameset());
    let mut children = run.pass.children(run.box_);

    if let Some(layout) = layout {
        // 4. Invoke the algorithm defined below to convert a list of dimensions to a list of pixel values using cols as
        //    the input list, and the width of the surface that the frameset is being rendered into, in CSS pixels, as
        //    the input dimension. Let sized cols be the resulting list.
        //    Invoke the algorithm defined below to convert a list of dimensions to a list of pixel values using rows as
        //    the input list, and the height of the surface that the frameset is being rendered into, in CSS pixels, as
        //    the input dimension. Let sized rows be the resulting list.
        let sized_cols = frameset_track_sizes(&layout.columns, surface_inline_size, layout.border_thickness);
        let sized_rows = frameset_track_sizes(&layout.rows, surface_block_size, layout.border_thickness);
        let border_thickness = CssPixels::from_integer(i64::from(layout.border_thickness));

        // 5. Split the surface into a grid of w×h rectangles, where w is the number of entries in sized cols and h is
        //    the number of entries in sized rows.
        //    Size the columns so that each column in the grid is as many CSS pixels wide as the corresponding entry in
        //    the sized cols list.
        //    Size the rows so that each row in the grid is as many CSS pixels high as the corresponding entry in the
        //    sized rows list.
        // 6. Let children be the list of frame and frameset elements that are children of the frameset element for which
        //    the algorithm was invoked.
        // 7. For each row of the grid of rectangles created in the previous step, from top to bottom, run these substeps:
        let mut y = CssPixels::default();
        'rows: for &row_size in &sized_rows {
            // 1. For each rectangle in the row, from left to right, run these substeps:
            let mut x = CssPixels::default();
            for &column_size in &sized_cols {
                // 1. If there are any elements left in children, take the first element in the list, and assign it to
                //    the rectangle.
                //    If this is a frameset element, then recurse the entire frameset layout algorithm for that frameset
                //    element, with the rectangle as the surface.
                //    Otherwise, it is a frame element; render its content navigable, positioned and sized to fit the
                //    rectangle.
                // 2. If there are any elements left in children, remove the first element from children.
                let Some(child) = children.next() else {
                    break 'rows;
                };
                layout_frameset_child(
                    run,
                    child,
                    (column_size, row_size),
                    used_values::FfiCssPixelPoint { x, y },
                );
                x += column_size + border_thickness;
            }
            y += row_size + border_thickness;
        }
    }

    // NB: Children that the grid has no rectangle for are not rendered. They still get a box, of no size.
    for child in children {
        layout_frameset_child(
            run,
            child,
            (CssPixels::default(), CssPixels::default()),
            used_values::FfiCssPixelPoint::default(),
        );
    }
    result
}
