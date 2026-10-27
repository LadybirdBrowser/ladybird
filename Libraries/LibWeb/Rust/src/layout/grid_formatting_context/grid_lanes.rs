/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Grid lanes layout, which lays out items into tracks in one axis and stacks them freely in the other.
//! https://drafts.csswg.org/css-grid-3/

use super::*;

/// The grid lanes layout of a grid lanes container.
// https://drafts.csswg.org/css-grid-3/#grid-lanes-model
// Grid lanes layout lays out items into pre-defined tracks similar to grid layout in one axis (called the grid axis),
// but flows them freely similar to flex layout in the other (called the stacking axis).
#[derive(Clone, Copy, Debug)]
pub(super) struct GridLanes {
    /// The physical axis of this formatting context that is the grid axis of the grid lanes container.
    pub(super) grid_axis: Axis,
}

impl GridLanes {
    pub(super) fn stacking_axis(self) -> Axis {
        self.grid_axis.opposite()
    }
}

/// Where a grid lanes item is placed in the stacking axis: the offset of its margin box from the start of the stacking
/// range, and the size of its alignment container, which is its margin box unless it is adjacent to a gap.
#[derive(Clone, Copy, Debug, Default)]
pub(super) struct StackingAxisArea {
    pub(super) offset: CssPixels,
    pub(super) size: CssPixels,
    pub(super) is_adjacent_to_gap: bool,
}

/// The intrinsic size contributions of the hypothetical copies of the auto-placed items that share a span size.
// https://drafts.csswg.org/css-grid-3/#track-sizing-performance
struct ItemGroup {
    span: usize,
    is_scroll_container: bool,
    /// One virtual grid item per start position the items could be placed at.
    virtual_items: Vec<ItemContribution>,
}

impl GridFormattingContext<'_> {
    pub(super) fn grid_lanes_for_container(&self) -> Option<GridLanes> {
        if !self.facts(self.run.box_).display().is_grid_lanes_inside() {
            return None;
        }
        // https://drafts.csswg.org/css-grid-3/#grid-lanes-orientation
        // The initial value of this property is normal, which determines the orientation from the grid-template-*
        // properties:
        // * If grid-template-columns is none and grid-template-rows is not none, the grid container’s block axis is
        //   the grid axis (establishing rows).
        // * Otherwise (thus by default), the grid container’s inline axis is the grid axis (establishing columns).
        let grid_style = self.grid_style(self.run.box_);
        let is_none = |list: ComputedGridTrackList| !list.is_subgrid && list.first_entry == GRID_NO_INDEX;
        let establishes_rows = is_none(grid_style.template_columns) && !is_none(grid_style.template_rows);
        // NB: This formatting context's Column axis is the grid container's inline axis, unless the grid's axes are
        //     transposed in it.
        let grid_axis = if establishes_rows != self.grid_axes_are_transposed {
            Axis::Row
        } else {
            Axis::Column
        };
        Some(GridLanes { grid_axis })
    }

    pub(super) fn is_grid_lanes_stacking_axis(&self, axis: Axis) -> bool {
        self.lanes.is_some_and(|lanes| lanes.stacking_axis() == axis)
    }

    /// The size of the grid lanes container's content box in the stacking axis, which forms the containing block of
    /// its items in that axis.
    // https://drafts.csswg.org/css-grid-3/#containing-block
    // The containing block for a grid item participating in grid lanes layout is formed by its grid area in the grid
    // axis and the grid container's content box in the stacking axis.
    pub(super) fn grid_lanes_stacking_containing_block_size(&self, axis: Axis) -> Option<CssPixels> {
        match self.axis_available(axis) {
            AvailableSize::Definite(size) => Some(size),
            _ => None,
        }
    }

    /// The available space an item is sized against in the stacking axis, where it is sized as for fit-content.
    pub(super) fn grid_lanes_stacking_available_size(&self, axis: Axis) -> CssPixels {
        match self.axis_available(axis) {
            AvailableSize::Definite(size) => size,
            AvailableSize::MinContent => CssPixels::default(),
            AvailableSize::MaxContent | AvailableSize::Indefinite => {
                clamp_to_max_dimension_value(CssPixels::from_raw(i32::MAX))
            }
        }
    }

    /// The tie threshold, or None if it is infinite.
    // https://drafts.csswg.org/css-grid-3/#placement-tolerance
    fn tie_threshold(&self, grid_axis: Axis) -> Option<CssPixels> {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let value = &self.grid_style(self.run.box_).fit_tolerance_style_value;
        match value.style_value() {
            // normal: Resolves to a used value of 1em in grid lanes layout and a used value of 0 in all other layout
            //         modes.
            None => Some(self.style(self.run.box_).font_size()),
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL => {
                Some(self.style(self.run.box_).font_size())
            }
            // infinite: Specifies an infinite tie threshold.
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::INFINITE => None,
            // <length-percentage>: Specifies the tie threshold for the grid lanes container.
            // Percentages: relative to the grid-axis content box size of the grid lanes container
            Some(_) => {
                let reference = match self.axis_available(grid_axis) {
                    AvailableSize::Definite(size) => size,
                    _ => self.track_sum(grid_axis),
                };
                Some(
                    value
                        .length_percentage()
                        .map_or(CssPixels::default(), |value| value.to_px(reference)),
                )
            }
        }
    }

    /// Places the items in the grid axis as far as that is possible before layout: items with a definite grid position
    /// in the grid axis are placed there, and every item is placed in the single track of the stacking axis.
    // https://drafts.csswg.org/css-grid-3/#grid-lanes-track-placement
    // In the grid axis, items can be explicitly placed into tracks and span them using the familiar grid-placement
    // properties’ syntax. Auto-placement, however, uses the Grid Lanes Layout and Placement Algorithm, placing each
    // item with an automatic grid position into the “shortest” track available.
    pub(super) fn place_grid_lanes_items(&mut self, lanes: GridLanes) {
        let (nodes, inputs) = self.collect_grid_items();
        let grid_axis = lanes.grid_axis;
        let resolved = |input: &PlacementInput| grid_axis.select(input.column, input.row);
        let subgridded = self.container_is_subgridded(grid_axis);
        let lines = grid_axis.select(&self.column_lines, &self.row_lines);
        let explicit_track_count = lines.len().saturating_sub(1) as i32;

        // https://drafts.csswg.org/css-grid-3/#grid-lanes-track-templates
        // The explicit grid and implicit grid are formed in the same way as for a regular grid container.
        let mut first_line = 0i32;
        let mut last_line = explicit_track_count;
        for input in &inputs {
            let placement = resolved(input);
            match placement.start {
                Some(start) => {
                    first_line = first_line.min(start);
                    last_line = last_line.max(start.saturating_add(placement.span as i32));
                }
                None => last_line = last_line.max(placement.span as i32),
            }
        }
        if subgridded {
            first_line = 0;
            last_line = explicit_track_count;
        }
        let explicit_start = first_line.unsigned_abs() as usize;
        let track_count = (last_line - first_line).max(0) as usize;

        // https://drafts.csswg.org/css-grid-3/#grid-lanes-layout-algorithm
        // For each item in order-modified document order:
        let mut ordered_indices = (0..inputs.len()).collect::<Vec<_>>();
        ordered_indices.sort_by_key(|&index| (inputs[index].order, index));
        for index in ordered_indices {
            let input = inputs[index];
            let placement = resolved(&input);
            let mut span = placement.span;
            let mut start = placement.start.map_or(0, |start| start - first_line);
            if subgridded {
                Self::clamp_area_to_subgrid(&mut start, &mut span, track_count);
            }
            let mut item = self.new_grid_item(nodes[input.id]);
            item.set_position(grid_axis, start, span);
            item.set_position(lanes.stacking_axis(), 0, 1);
            item.is_auto_placed_in_grid_lanes = placement.start.is_none();
            self.items.push(item);
        }

        let (explicit_start_field, lines) = grid_axis.select(
            (&mut self.explicit_column_start, &mut self.column_lines),
            (&mut self.explicit_row_start, &mut self.row_lines),
        );
        *explicit_start_field = explicit_start;
        if explicit_start > 0 {
            let mut prepended = Vec::with_capacity(lines.len() + explicit_start);
            prepended.resize_with(explicit_start, Vec::new);
            prepended.append(lines);
            *lines = prepended;
        }
        lines.resize_with(track_count.saturating_add(1), Vec::new);
    }

    /// The tracks considered occupied for collapsing the empty tracks of a repeat(auto-fit) in the grid axis.
    // https://drafts.csswg.org/css-grid-3/#repeat-auto-fit
    // However, because placement occurs after track sizing, grid lanes containers use a heuristic to determine if a
    // track will be occupied:
    pub(super) fn grid_lanes_occupied_tracks(&self, axis: Axis) -> Vec<bool> {
        let track_count = self.axis_tracks(axis).len();
        let mut occupied = vec![false; track_count];
        let mut auto_placed_span_sum = 0usize;
        for item in &self.items {
            if item.is_auto_placed_in_grid_lanes {
                auto_placed_span_sum = auto_placed_span_sum.saturating_add(item.span(axis));
                continue;
            }
            // * All tracks occupied by explicitly placed items are considered occupied.
            let start = item.position(axis).max(0) as usize;
            let end = start.saturating_add(item.span(axis)).min(track_count);
            for track in occupied.iter_mut().take(end).skip(start) {
                *track = true;
            }
        }
        // * With the sum of the spans of all auto-placed items as N, all unoccupied tracks up to the Nth such track
        //   are considered occupied.
        for track in occupied
            .iter_mut()
            .filter(|occupied| !**occupied)
            .take(auto_placed_span_sum)
        {
            *track = true;
        }
        occupied
    }

    // https://drafts.csswg.org/css-grid-3/#track-sizing
    // Track sizing works the same as in CSS Grid, except that when considering which items contribute to intrinsic
    // sizes:
    // * All items explicitly placed in that track contribute, and
    // * All items with an automatic grid position contribute (regardless of whether they are ultimately placed in that
    //   track).
    //
    // In the case of spanning items with an automatic grid position, they are assumed to be placed at every possible
    // start position, and contribute accordingly.
    pub(super) fn grid_lanes_item_contributions_to_track_sizing(&self, axis: Axis) -> Vec<ItemContribution> {
        let track_count = self.axis_tracks(axis).len();
        let mut result = Vec::new();
        // https://drafts.csswg.org/css-grid-3/#track-sizing-performance
        // Track sizing can be optimized by aggregating items that have the same span size and placement into a single
        // virtual item as follows:
        // 1. Separate all the grid items into item groups, according to the following properties:
        //    * the span of the item
        //    * the placement of the item, i.e. which tracks it is allowed to be placed in
        //    * the item's baseline-sharing group
        // 2. For each item group, synthesize a virtual grid item that has the maximum of every intrinsic size
        //    contribution among the items in that group.
        // 3. Place hypothetical copies of each virtual grid item into the grid axis tracks in every position that the
        //    item could potentially occupy, and run the track sizing algorithm with those items.
        // NB: Explicitly placed items keep their own contributions, since each is placed in only one position. The
        //     contributions of an auto-placed item can depend on the tracks it spans, so each copy of a virtual item
        //     takes the maximum of the contributions of its items placed at that position. Scroll containers form
        //     their own groups, since track sizing treats their contributions differently.
        let mut groups: Vec<ItemGroup> = Vec::new();
        for item in self.items.iter().copied() {
            if !item.is_auto_placed_in_grid_lanes {
                if self.grid_item_is_subgridded(item, axis) {
                    result.extend(self.subgrid_item_contributions_to_track_sizing(item, axis));
                } else {
                    result.push(self.item_contribution(item, axis, track_count));
                }
                continue;
            }
            let span = item.span(axis);
            let start_count = track_count.saturating_sub(span).saturating_add(1);
            if self.grid_item_is_subgridded(item, axis) {
                // https://drafts.csswg.org/css-grid-3/#track-sizing-subgrid
                // When sizing the tracks of a grid lanes container, a subgrid with an automatic grid position (whether
                // a regular grid subgrid or grid lanes subgrid) also has special handling of its items:
                // * Every item is placed into every possible parent grid track that could be spanned by the subgrid,
                //   (ignoring any explicit placement of the item).
                // FIXME: Ignore the explicit placement of the subgrid's items.
                for start in 0..start_count {
                    let mut placed = item;
                    placed.set_position(axis, start as i32, span);
                    result.extend(self.subgrid_item_contributions_to_track_sizing(placed, axis));
                }
                continue;
            }
            let is_scroll_container = self.facts(item.box_).is_scroll_container();
            let group_index = match groups
                .iter()
                .position(|group| group.span == span && group.is_scroll_container == is_scroll_container)
            {
                Some(index) => index,
                None => {
                    groups.push(ItemGroup {
                        span,
                        is_scroll_container,
                        virtual_items: Vec::with_capacity(start_count),
                    });
                    groups.len() - 1
                }
            };
            for start in 0..start_count {
                let mut placed = item;
                placed.set_position(axis, start as i32, span);
                let contribution = self.item_contribution(placed, axis, track_count);
                let virtual_items = &mut groups[group_index].virtual_items;
                match virtual_items.get_mut(start) {
                    Some(virtual_item) => {
                        virtual_item.minimum = virtual_item.minimum.max(contribution.minimum);
                        virtual_item.min_content = virtual_item.min_content.max(contribution.min_content);
                        virtual_item.limited_min_content =
                            virtual_item.limited_min_content.max(contribution.limited_min_content);
                        virtual_item.max_content = virtual_item.max_content.max(contribution.max_content);
                        virtual_item.limited_max_content =
                            virtual_item.limited_max_content.max(contribution.limited_max_content);
                    }
                    None => virtual_items.push(contribution),
                }
            }
        }
        for group in groups {
            result.extend(group.virtual_items);
        }
        result
    }

    /// Sizes the tracks of the grid axis and places and sizes every item.
    pub(super) fn run_grid_lanes_track_sizing_and_placement(&mut self, lanes: GridLanes) {
        let stacking_axis = lanes.stacking_axis();
        // NB: The single track of the stacking axis stands for the content box of the grid lanes container while the
        //     items are sized, since that forms their containing block in that axis.
        let containing_block_size = self
            .grid_lanes_stacking_containing_block_size(stacking_axis)
            .unwrap_or_default();
        *axis_tracks_mut(self, stacking_axis) = vec![Track::fixed(containing_block_size)];

        if lanes.grid_axis.is_column() {
            self.resolve_item_metrics(Axis::Column);
            self.run_track_sizing(Axis::Column);
        } else {
            // NB: An item's inline size in the stacking axis does not depend on where it is placed, but its block size
            //     contributions to the rows depend on it.
            self.resolve_item_metrics(Axis::Column);
            self.resolve_item_sizes(Axis::Column);
            self.resolve_item_metrics(Axis::Row);
            self.run_track_sizing(Axis::Row);
        }
        self.run_grid_lanes_layout_and_placement_algorithm(lanes);
    }

    // https://drafts.csswg.org/css-grid-3/#grid-lanes-layout-algorithm
    fn run_grid_lanes_layout_and_placement_algorithm(&mut self, lanes: GridLanes) {
        let grid_axis = lanes.grid_axis;
        let stacking_axis = lanes.stacking_axis();
        let track_count = self.axis_tracks(grid_axis).len();
        let tie_threshold = self.tie_threshold(grid_axis);
        let gap = self.resolved_gap(stacking_axis, self.axis_available(stacking_axis));
        let collapsed = self
            .axis_tracks(grid_axis)
            .iter()
            .map(|track| track.is_collapsed)
            .collect::<Vec<_>>();
        let dense = self.style(self.run.box_).grid_auto_flow_dense();
        let mut skipped_spaces = vec![Vec::<SkippedSpace>::new(); track_count];

        // For each of the tracks in the grid axis, keep a running position initialized to zero. Maintain also a
        // auto-placement cursor, initially pointing to the first line.
        let mut running_positions = vec![CssPixels::default(); track_count];
        let mut items_in_tracks = vec![Vec::new(); track_count];
        let mut auto_placement_cursor = 0usize;

        let max_running_position = |running_positions: &[CssPixels], start: usize, span: usize| {
            running_positions[start..start + span]
                .iter()
                .copied()
                .max()
                .unwrap_or_default()
        };

        // For each item in order-modified document order:
        for item_index in 0..self.items.len() {
            let mut item = self.items[item_index];
            let span = item.span(grid_axis).min(track_count);
            if track_count == 0 || span == 0 {
                continue;
            }
            let auto_placement_cursor_before_placement = auto_placement_cursor;
            // 1. If the item has a definite grid position in the grid axis, use that placement.
            //    Otherwise, resolve its grid axis placement using these substeps:
            if item.is_auto_placed_in_grid_lanes {
                // 1. Starting at the first grid axis line in the implicit grid, find the largest running position of
                //    the grid axis tracks that the item would span if it were placed at this line, and call this
                //    position max_pos.
                // 2. Repeat the previous step for each successive line number until the item would no longer fit
                //    inside the grid.
                // NB: A collapsed grid track cannot accept placement of auto-placed items.
                let candidates = (0..=track_count - span)
                    .filter(|&line| !collapsed[line..line + span].iter().any(|&collapsed| collapsed))
                    .map(|line| (line, max_running_position(&running_positions, line, span)))
                    .collect::<Vec<_>>();
                // 3. Let possible lines be the line that resulted in the smallest max_pos, and all lines that result
                //    in a max_pos within the tie threshold of this max_pos.
                let smallest_max_pos = candidates.iter().map(|&(_, max_pos)| max_pos).min();
                let is_possible = |max_pos: CssPixels| {
                    tie_threshold.is_none_or(|threshold| {
                        smallest_max_pos.is_some_and(|smallest| max_pos - smallest <= threshold)
                    })
                };
                let possible_lines = candidates
                    .iter()
                    .filter(|&&(_, max_pos)| is_possible(max_pos))
                    .map(|&(line, _)| line);
                // 4. Choose the first line in possible lines greater than or equal to the auto-placement cursor as the
                //    item's position in the grid axis; or if there are none such, choose the first one.
                let line = possible_lines
                    .clone()
                    .find(|&line| line >= auto_placement_cursor)
                    .or_else(|| possible_lines.clone().next())
                    .unwrap_or(0);
                // 5. Update the auto-placement cursor to point to item's last line.
                auto_placement_cursor = line + span;
                item.set_position(grid_axis, line as i32, span);
            }
            let start = (item.position(grid_axis).max(0) as usize).min(track_count - span);

            // 2. Place the item in its grid axis tracks at the maximum of the running positions of the tracks it spans.
            let max_pos = max_running_position(&running_positions, start, span);
            self.items[item_index] = item;

            // 3. Calculate the size of the item's containing block and then layout the item. Set the running position
            //    of the spanned grid axis tracks to max_pos + outer size + grid-gap.
            if grid_axis.is_column() {
                self.resolve_metrics_of_item(item_index, Axis::Column);
                self.resolve_size_of_item(item_index, Axis::Column);
            }
            self.resolve_metrics_of_item(item_index, Axis::Row);
            self.resolve_size_of_item(item_index, Axis::Row);
            let item = self.items[item_index];
            let content_size = axis_content_size(self.used(item), stacking_axis);
            // For this purpose, the outer sizes of the box are floored at zero.
            let outer_size = self
                .add_outer_size(item, stacking_axis, content_size)
                .max(CssPixels::default());

            // 4. If the grid lanes container uses dense packing, and there exists skipped spaces in the layout (e.g.
            //    due to spanning items) into which the item, as it is sized now, could have fit if it were placed
            //    earlier, and where the spanned tracks have the same total used size as the tracks into which it is
            //    currently placed, then instead place it into the highest such space. If there are multiple valid
            //    spaces within the tie threshold of the highest space, place it in the start-most of them. Rewind the
            //    auto-placement cursor and the running position to their values before this item’s placement.
            let dense_placement = dense
                .then(|| {
                    let span_size = |line: usize| self.grid_lanes_span_size(grid_axis, line, span);
                    let current_size = span_size(start);
                    let lines = if item.is_auto_placed_in_grid_lanes {
                        0..track_count - span + 1
                    } else {
                        start..start + 1
                    };
                    let lines = lines.filter(|&line| {
                        !collapsed[line..line + span].iter().any(|&collapsed| collapsed)
                            && span_size(line) == current_size
                    });
                    find_dense_placement(&skipped_spaces, lines, span, outer_size, gap, tie_threshold)
                        .filter(|&(_, position)| position < max_pos)
                })
                .flatten();
            let (line, position) = match dense_placement {
                Some((line, position)) => {
                    auto_placement_cursor = auto_placement_cursor_before_placement;
                    for spaces in &mut skipped_spaces[line..line + span] {
                        fill_skipped_space(spaces, position, position + outer_size + gap);
                    }
                    self.items[item_index].set_position(grid_axis, line as i32, span);
                    (line, position)
                }
                None => {
                    for track in start..start + span {
                        if dense && running_positions[track] < max_pos {
                            skipped_spaces[track].push(SkippedSpace {
                                start: running_positions[track],
                                end: max_pos,
                            });
                        }
                        running_positions[track] = max_pos + outer_size + gap;
                    }
                    (start, max_pos)
                }
            };
            for items in &mut items_in_tracks[line..line + span] {
                items.push(item_index);
            }
            self.items[item_index].grid_lanes_stacking_area = Some(StackingAxisArea {
                offset: position,
                size: outer_size,
                is_adjacent_to_gap: false,
            });
        }
        // NB: Dense packing places items before the items placed earlier in their tracks.
        if dense {
            for items in &mut items_in_tracks {
                items.sort_by_key(|&index| {
                    self.items[index]
                        .grid_lanes_stacking_area
                        .map_or(CssPixels::default(), |area| area.offset)
                });
            }
        }

        // https://drafts.csswg.org/css-grid-3/#stacking-range
        // The stacking range is the range between the startmost outer edge among the first items of each track and the
        // endmost outer edge among the last items of each track.
        let stacking_range = running_positions
            .iter()
            .zip(&items_in_tracks)
            .filter(|&(_, items)| !items.is_empty())
            .map(|(&position, _)| position - gap)
            .max()
            .unwrap_or_default()
            .max(CssPixels::default());
        *axis_tracks_mut(self, stacking_axis) = vec![Track::fixed(stacking_range)];
        self.align_grid_lanes_items_in_gaps(stacking_axis, &items_in_tracks, stacking_range, gap);
    }

    // https://drafts.csswg.org/css-grid-3/#stacking-self-alignment
    // In the stacking axis, the self-alignment properties only apply to items that are adjacent to a “gap” in the
    // layout, i.e. placed in their grid track(s) either immediately before a spanning item or as the last item. The
    // alignment subject is the item's margin box, and the alignment container is that box plus the adjacent “gap”. For
    // the last item in a track, the alignment container extends to the lowest bottom outer edge among all the last
    // items in all tracks.
    fn align_grid_lanes_items_in_gaps(
        &mut self,
        stacking_axis: Axis,
        items_in_tracks: &[Vec<usize>],
        stacking_range: CssPixels,
        gap: CssPixels,
    ) {
        let mut alignment_container_ends = vec![stacking_range; self.items.len()];
        for items in items_in_tracks {
            for pair in items.windows(2) {
                let next_offset = self.items[pair[1]]
                    .grid_lanes_stacking_area
                    .map_or(CssPixels::default(), |area| area.offset);
                let end = &mut alignment_container_ends[pair[0]];
                *end = (*end).min(next_offset - gap);
            }
        }
        for (item_index, end) in alignment_container_ends.into_iter().enumerate() {
            let Some(mut area) = self.items[item_index].grid_lanes_stacking_area else {
                continue;
            };
            if end - area.offset <= area.size {
                continue;
            }
            area.size = end - area.offset;
            area.is_adjacent_to_gap = true;
            self.items[item_index].grid_lanes_stacking_area = Some(area);
            // NB: Alignment of an item does not affect the size or position of other items' alignment containers.
            self.resolve_size_of_item(item_index, stacking_axis);
        }
    }

    /// The alignment of an item in the stacking axis, where Start and End are the left/top and right/bottom sides of
    /// its alignment container.
    pub(super) fn grid_lanes_stacking_axis_item_alignment(&self, item: GridItem, axis: Axis) -> Alignment {
        let start = if self.axis_is_reversed(axis) {
            Alignment::End
        } else {
            Alignment::Start
        };
        if !item
            .grid_lanes_stacking_area
            .is_some_and(|area| area.is_adjacent_to_gap)
        {
            return start;
        }
        match self.item_physical_alignment_for_node(item.box_, axis) {
            // https://drafts.csswg.org/css-grid-3/#grid-lanes-baseline-alignment
            // Baseline alignment is not supported in the stacking axis. The baseline alignment values of the
            // self-alignment properties on grid items are treated as their fallback alignment in this axis.
            // NB: Normal alignment does not stretch an item in the stacking axis.
            Alignment::Normal | Alignment::Baseline => start,
            alignment => alignment,
        }
    }

    // https://drafts.csswg.org/css-grid-3/#grid-lanes-baseline-alignment
    // The first baseline set of the grid container in this axis is generated from the highest alignment baseline among
    // the grid items placed first in each track, and the last baseline set from the lowest alignment baseline among the
    // grid items placed last in each track.
    pub(super) fn grid_lanes_baseline_of_items(
        &self,
        lanes: GridLanes,
        baseline_set: formatting_context::BaselineSet,
    ) -> Option<CssPixels> {
        let grid_axis = lanes.grid_axis;
        let track_count = self.axis_tracks(grid_axis).len();
        // NB: Whether an item is first or last in its tracks depends only on placement, including dense packing
        //     adjustments, so the item that is placed highest (lowest) in a track is its first (last) item.
        let mut items_by_track: Vec<Option<(CssPixels, GridItem)>> = vec![None; track_count];
        for item in &self.items {
            let Some(area) = item.grid_lanes_stacking_area else {
                continue;
            };
            let start = (item.position(grid_axis).max(0) as usize).min(track_count);
            let end = start.saturating_add(item.span(grid_axis)).min(track_count);
            for slot in &mut items_by_track[start..end] {
                let replaces = slot.is_none_or(|(offset, _)| match baseline_set {
                    formatting_context::BaselineSet::First => area.offset < offset,
                    formatting_context::BaselineSet::Last => area.offset >= offset,
                });
                if replaces {
                    *slot = Some((area.offset, *item));
                }
            }
        }
        let container_writing_mode = self.style(self.run.box_).writing_mode();
        let baselines = items_by_track.into_iter().flatten().filter_map(|(_, item)| {
            // NB: An item in another writing mode has no baseline in the grid's inline axis.
            let source = if self.style(item.box_).writing_mode() != container_writing_mode {
                formatting_context::ChildBaselineSource::Synthesized
            } else {
                formatting_context::ChildBaselineSource::OwnOrSynthesized
            };
            formatting_context::baseline_of_child(
                self.run.records,
                &self.run.callbacks,
                item.box_,
                baseline_set,
                source,
            )
        });
        match baseline_set {
            formatting_context::BaselineSet::First => baselines.min(),
            formatting_context::BaselineSet::Last => baselines.max(),
        }
    }

    /// The size of the tracks from the given line spanning the given number of tracks in the grid axis, with the
    /// gutters between them.
    fn grid_lanes_span_size(&self, axis: Axis, line: usize, span: usize) -> CssPixels {
        let tracks = self.axis_tracks(axis);
        let gaps = self.axis_gaps(axis);
        let mut size = CssPixels::default();
        for (index, track) in tracks[line..line + span].iter().enumerate() {
            size += track.base_size;
            if index + 1 < span {
                size += gaps.get(line + index).map_or(CssPixels::default(), |gap| gap.base_size);
            }
        }
        size
    }

    /// The offset and size of an item's alignment container in the stacking axis, from the content edge the tracks
    /// start at.
    pub(super) fn grid_lanes_stacking_axis_area(&self, area: StackingAxisArea, axis: Axis) -> (CssPixels, CssPixels) {
        // https://drafts.csswg.org/css-grid-3/#stacking-content-alignment
        // In the stacking axis, content-distribution is applied to the content as a whole, similarly to how it behaves
        // in block containers. More specifically, the alignment subject is the stacking range.
        let start = content_start_offset(
            self.content_alignment(axis),
            self.grid_container_alignment_size(axis),
            self.track_sum(axis),
        );
        (start + area.offset, area.size)
    }
}

/// A space in a track that placement skipped over, which an item placed at an offset in it fits in if the item and the
/// gutter after it end by its end.
#[derive(Clone, Copy, Debug)]
struct SkippedSpace {
    start: CssPixels,
    end: CssPixels,
}

/// The highest line and offset at which an item fits in the skipped spaces of the tracks it would span, or the
/// start-most of those within the tie threshold of the highest.
fn find_dense_placement(
    skipped_spaces: &[Vec<SkippedSpace>],
    lines: impl Iterator<Item = usize>,
    span: usize,
    outer_size: CssPixels,
    gap: CssPixels,
    tie_threshold: Option<CssPixels>,
) -> Option<(usize, CssPixels)> {
    let mut placements = Vec::new();
    for line in lines {
        let tracks = &skipped_spaces[line..line + span];
        let mut offsets = tracks.iter().flatten().map(|space| space.start).collect::<Vec<_>>();
        offsets.sort();
        let fits = |offset: CssPixels| {
            tracks.iter().all(|spaces| {
                spaces
                    .iter()
                    .any(|space| space.start <= offset && offset + outer_size + gap <= space.end)
            })
        };
        if let Some(offset) = offsets.into_iter().find(|&offset| fits(offset)) {
            placements.push((line, offset));
        }
    }
    let highest = placements.iter().map(|&(_, offset)| offset).min()?;
    placements
        .into_iter()
        .filter(|&(_, offset)| tie_threshold.is_none_or(|threshold| offset - highest <= threshold))
        .min_by_key(|&(line, _)| line)
}

/// Removes the range an item fills, with the gutter after it, from the skipped space that holds it.
fn fill_skipped_space(spaces: &mut Vec<SkippedSpace>, start: CssPixels, end: CssPixels) {
    let Some(index) = spaces.iter().position(|space| space.start <= start && end <= space.end) else {
        return;
    };
    let space = spaces.remove(index);
    if space.start < start {
        spaces.push(SkippedSpace {
            start: space.start,
            end: start,
        });
    }
    if end < space.end {
        spaces.push(SkippedSpace {
            start: end,
            end: space.end,
        });
    }
}

fn axis_tracks_mut<'context, 'pass>(
    context: &'context mut GridFormattingContext<'pass>,
    axis: Axis,
) -> &'context mut Vec<Track<'pass>> {
    axis.select(&mut context.columns, &mut context.rows)
}

fn axis_content_size(used: &UsedValues, axis: Axis) -> CssPixels {
    axis.select(used.content_inline_size.get(), used.content_block_size.get())
}
