/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Re-snapping a scroll container to the areas it was snapped to before a content change.

use crate::css::css_pixels::{CssPixelPoint, CssPixels};
use libcompositing_rust::scrolling::scroll_snap_selection::*;
use libcompositing_rust::scrolling::{SnapAreaIdentity, SnapDestination, SnappedAreas};

pub(crate) trait SnapAreaDomRelations {
    fn area_contains_focused_node(&self, area: SnapAreaIdentity) -> bool;
    fn area_contains_targeted_element(&self, area: SnapAreaIdentity) -> bool;
    fn element_of_area_is_ancestor_of_element_of(&self, area: SnapAreaIdentity, other_area: SnapAreaIdentity) -> bool;
}

fn resnap_offset_for_candidate(candidate: &SnapPositionCandidate, axis_current_offset: CssPixels) -> CssPixels {
    // A scroll container resting at one of its snapped area's valid snap positions is still snapped to that area and
    // stays where it is.
    if candidate_has_snap_position_at(candidate, axis_current_offset) {
        return axis_current_offset;
    }
    candidate.offset
}

// The box each axis is snapped to, as an index into that axis's candidate list.
struct SnappedAxisBoxes {
    x_candidate: Option<usize>,
    y_candidate: Option<usize>,
}

// https://drafts.csswg.org/css-scroll-snap-1/#multiple-aligned-snap-areas
// When snapping to a scroll position that is aligned with multiple scroll snap areas, the following algorithm procedure
// is used to determined which box is snapped on the block and inline axes for a particular scroll container:
// NB: Every step treats the block and inline lists alike, so the steps are carried out on the physical axes directly.
fn select_between_multiple_aligned_snap_areas(
    candidates: &SnapAxisCandidates,
    selection: &SnappedAreas,
    relations: &impl SnapAreaDomRelations,
    scroll_position: CssPixelPoint,
) -> SnappedAxisBoxes {
    // 1. Let scroll position be the scroll position of the scroll container
    // NB: The scroll position is the offset the content change left the container resting at.

    // 2. Let inline be the set of boxes whose scroll snap areas are aligned at this scroll position in the inline axis.
    // 3. Let block be the set of boxes whose scroll snap areas are aligned at this scroll position in the block axis.
    // AD-HOC: Only the snap areas the container was snapped to before the content change take part, since a re-snap
    //         must return the container to those same areas rather than to whichever areas the change left aligned. An
    //         area whose snap position would leave it outside the snapport in the other axis no longer offers a valid
    //         snap position and does not take part either.
    // NB: A box is held as the index of its snap position candidate in its axis's candidate list, which holds its
    //     candidates in tree order.
    let aligned_boxes = |axis_candidates: &[SnapPositionCandidate],
                         snapped_areas: &[SnapAreaIdentity],
                         cross_axis_offset: CssPixels| {
        let mut boxes = Vec::new();
        for (candidate_index, candidate) in axis_candidates.iter().enumerate() {
            if !candidate.area.is_valid() || !snapped_areas.contains(&candidate.area) {
                continue;
            }
            if !snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset) {
                continue;
            }
            boxes.push(candidate_index);
        }
        boxes
    };
    let mut x_boxes = aligned_boxes(&candidates.x_candidates, &selection.x, scroll_position.y);
    let mut y_boxes = aligned_boxes(&candidates.y_candidates, &selection.y, scroll_position.x);

    // 4. For each list of block and inline:
    let remove_superseded_boxes = |boxes: &mut Vec<usize>, axis_candidates: &[SnapPositionCandidate]| {
        let mut keep_only_boxes_matching = |predicate: &dyn Fn(SnapAreaIdentity) -> bool| {
            if !boxes
                .iter()
                .any(|&candidate_index| predicate(axis_candidates[candidate_index].area))
            {
                return false;
            }
            boxes.retain(|&candidate_index| predicate(axis_candidates[candidate_index].area));
            true
        };

        // 1. If list contains one or more boxes that are focused or have a focused descendant, remove all other boxes
        //    from list
        let kept_focused_boxes = keep_only_boxes_matching(&|area| relations.area_contains_focused_node(area));

        // 2. Else if list contains one or more boxes that are targetted or have a targetted descendant, remove all
        //    other boxes from list.
        if !kept_focused_boxes {
            keep_only_boxes_matching(&|area| relations.area_contains_targeted_element(area));
        }

        // 3. For each box in list:
        // 1. Remove any box from list which is an ancestor of box.
        let boxes_before_ancestor_removal = boxes.clone();
        boxes.retain(|&candidate_index| {
            let area = axis_candidates[candidate_index].area;
            !boxes_before_ancestor_removal.iter().any(|&other_index| {
                relations.element_of_area_is_ancestor_of_element_of(area, axis_candidates[other_index].area)
            })
        });
    };
    remove_superseded_boxes(&mut x_boxes, &candidates.x_candidates);
    remove_superseded_boxes(&mut y_boxes, &candidates.y_candidates);

    let boxes_contain_area = |boxes: &[usize], axis_candidates: &[SnapPositionCandidate], area: SnapAreaIdentity| {
        boxes
            .iter()
            .any(|&candidate_index| axis_candidates[candidate_index].area == area)
    };
    let axis_sets_overlap = x_boxes.iter().any(|&candidate_index| {
        boxes_contain_area(
            &y_boxes,
            &candidates.y_candidates,
            candidates.x_candidates[candidate_index].area,
        )
    });
    // 5. If inline and block are overlapping sets:
    if axis_sets_overlap {
        // 1. Replace inline with the intersection of inline and block.
        x_boxes.retain(|&candidate_index| {
            boxes_contain_area(
                &y_boxes,
                &candidates.y_candidates,
                candidates.x_candidates[candidate_index].area,
            )
        });

        // 2. Replace block with the intersection of inline and block.
        // NB: The intersection is unchanged by the step before it, so the narrowed list gives the same result the
        //     original one would.
        y_boxes.retain(|&candidate_index| {
            boxes_contain_area(
                &x_boxes,
                &candidates.x_candidates,
                candidates.y_candidates[candidate_index].area,
            )
        });
    }

    // 6. Select the first element in tree order from inline as the snapped inline axis box.
    // 7. Select the first element in tree order from block as the snapped block axis box.
    SnappedAxisBoxes {
        x_candidate: x_boxes.first().copied(),
        y_candidate: y_boxes.first().copied(),
    }
}

pub(crate) fn select_resnap_destination(
    geometry: &SnapContainerGeometry,
    areas: &[SnapAreaGeometry],
    current_offset: CssPixelPoint,
    selection: &SnappedAreas,
    relations: &impl SnapAreaDomRelations,
) -> SnapDestination {
    if geometry.axes.is_empty() {
        return SnapDestination {
            position: current_offset,
            ..Default::default()
        };
    }
    let snap_axes = geometry.axes;

    let candidates = build_snap_candidates(geometry, areas, snap_axes);

    // NB: A container that was not snapped before the change, or whose snapped areas are all gone, re-snaps the way a
    //     fresh scroll to the current position would.
    let resnap_afresh = || {
        select_snap_destination_with_candidates(
            geometry,
            &candidates,
            current_offset,
            &SnapSelectionStrategy::default(),
            snap_axes,
        )
    };
    if selection.is_empty() {
        return resnap_afresh();
    }

    let SnappedAxisBoxes {
        x_candidate: mut x_chosen_candidate,
        y_candidate: mut y_chosen_candidate,
    } = select_between_multiple_aligned_snap_areas(&candidates, selection, relations, current_offset);

    if x_chosen_candidate.is_none() && y_chosen_candidate.is_none() {
        return resnap_afresh();
    }

    let fallback_offset_for_axis = |axis_candidates: &[SnapPositionCandidate],
                                    axis_current_offset: CssPixels,
                                    axis_snapport_size: CssPixels,
                                    cross_axis_offset: CssPixels| {
        let axis_selection = SnapAxisSelection {
            destination: axis_current_offset,
            start: axis_current_offset,
            direction: CssPixels::default(),
            starting_positions_boundary: None,
        };
        let choice = choose_snap_offset_for_axis(
            axis_candidates,
            &axis_selection,
            axis_snapport_size,
            geometry.strictness,
            Some(cross_axis_offset),
            None,
        );
        choice.map(|axis_choice| axis_choice.offset)
    };

    // An axis whose snapped areas are all gone re-snaps afresh, from the snap positions reachable while the other
    // axis follows its own snapped area.
    let mut x_offset = None;
    let mut y_offset = None;
    if let Some(index) = x_chosen_candidate {
        x_offset = Some(resnap_offset_for_candidate(
            &candidates.x_candidates[index],
            current_offset.x,
        ));
    }
    if let Some(index) = y_chosen_candidate {
        y_offset = Some(resnap_offset_for_candidate(
            &candidates.y_candidates[index],
            current_offset.y,
        ));
    }
    if snap_axes.x && x_chosen_candidate.is_none() {
        x_offset = fallback_offset_for_axis(
            &candidates.x_candidates,
            current_offset.x,
            geometry.snapport.width,
            y_offset.unwrap_or(current_offset.y),
        );
    }
    if snap_axes.y && y_chosen_candidate.is_none() {
        y_offset = fallback_offset_for_axis(
            &candidates.y_candidates,
            current_offset.y,
            geometry.snapport.height,
            x_offset.unwrap_or(current_offset.x),
        );
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#re-snap
    // If it is not possible to snap to both (e.g. if snapping to one resulted in the other being offscreen), it must
    // prefer the focused box, followed by the targeted box, followed by the block axis if neither box is focused or
    // targeted.
    // NB: The preferred box is snapped in both axes: the other axis takes the box's own snap position when it defines
    //     one, and otherwise re-snaps among the positions at which the preferred box remains visible.
    if let (Some(x), Some(y)) = (x_offset, y_offset)
        && !chosen_offsets_are_mutually_visible(&candidates, x, y)
    {
        let block_axis_is_y = geometry.horizontal_writing_mode;

        struct ResnapAxis<'a> {
            candidates: &'a [SnapPositionCandidate],
            chosen_candidate: &'a mut Option<usize>,
            offset: &'a mut Option<CssPixels>,
            area: Option<SnapAreaIdentity>,
            current_offset: CssPixels,
            snapport_size: CssPixels,
        }
        let x_area = x_chosen_candidate.map(|candidate_index| candidates.x_candidates[candidate_index].area);
        let y_area = y_chosen_candidate.map(|candidate_index| candidates.y_candidates[candidate_index].area);
        let mut x_axis = ResnapAxis {
            candidates: &candidates.x_candidates,
            chosen_candidate: &mut x_chosen_candidate,
            offset: &mut x_offset,
            area: x_area,
            current_offset: current_offset.x,
            snapport_size: geometry.snapport.width,
        };
        let mut y_axis = ResnapAxis {
            candidates: &candidates.y_candidates,
            chosen_candidate: &mut y_chosen_candidate,
            offset: &mut y_offset,
            area: y_area,
            current_offset: current_offset.y,
            snapport_size: geometry.snapport.height,
        };

        let area_contains_focus =
            |area: Option<SnapAreaIdentity>| area.is_some_and(|area| relations.area_contains_focused_node(area));
        let area_contains_target =
            |area: Option<SnapAreaIdentity>| area.is_some_and(|area| relations.area_contains_targeted_element(area));

        let preferred_axis_is_x = if area_contains_focus(x_axis.area) || area_contains_focus(y_axis.area) {
            area_contains_focus(x_axis.area)
        } else if area_contains_target(x_axis.area) || area_contains_target(y_axis.area) {
            area_contains_target(x_axis.area)
        } else if x_axis.area.is_some() != y_axis.area.is_some() {
            x_axis.area.is_some()
        } else {
            !block_axis_is_y
        };
        let (preferred_axis, other_axis) = if preferred_axis_is_x {
            (&x_axis, &mut y_axis)
        } else {
            (&y_axis, &mut x_axis)
        };

        let preferred_area_candidate_in_other_axis = other_axis
            .candidates
            .iter()
            .position(|candidate| candidate.area == preferred_axis.area.unwrap());
        if let Some(index) = preferred_area_candidate_in_other_axis {
            *other_axis.offset = Some(resnap_offset_for_candidate(
                &other_axis.candidates[index],
                other_axis.current_offset,
            ));
            *other_axis.chosen_candidate = Some(index);
        } else {
            *other_axis.offset = fallback_offset_for_axis(
                other_axis.candidates,
                other_axis.current_offset,
                other_axis.snapport_size,
                preferred_axis.offset.unwrap(),
            );
            *other_axis.chosen_candidate = None;
        }
    }

    let mut snap_destination = snap_destination_for(current_offset, x_offset, y_offset, snap_axes);

    // An axis a re-snap left where it was selected nothing, so an area that happens to have become aligned at the
    // kept position does not take the place of the areas the container was snapped to.
    let record_snapped_areas = |chosen_candidate: Option<usize>,
                                axis_candidates: &[SnapPositionCandidate],
                                previously_snapped_areas: &[SnapAreaIdentity],
                                axis_offset: CssPixels,
                                axis_current_offset: CssPixels,
                                cross_axis_offset: CssPixels| {
        let mut areas = snap_areas_at_offset(axis_candidates, axis_offset, cross_axis_offset);
        if let Some(index) = chosen_candidate
            && axis_offset == axis_current_offset
        {
            let chosen_area = axis_candidates[index].area;
            areas.retain(|area| *area == chosen_area || previously_snapped_areas.contains(area));
        }
        areas
    };
    if let Some(x_offset) = x_offset {
        snap_destination.snapped_areas.x = record_snapped_areas(
            x_chosen_candidate,
            &candidates.x_candidates,
            &selection.x,
            x_offset,
            current_offset.x,
            snap_destination.position.y,
        );
    }
    if let Some(y_offset) = y_offset {
        snap_destination.snapped_areas.y = record_snapped_areas(
            y_chosen_candidate,
            &candidates.y_candidates,
            &selection.y,
            y_offset,
            current_offset.y,
            snap_destination.position.x,
        );
    }
    snap_destination
}
