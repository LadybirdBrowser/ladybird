/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Scroll snap position selection over geometry that has already been collected from layout, so that the same selection runs on the main thread and in the compositor process. Everything here is in CSS pixels.

use super::{SnapAreaIdentity, SnapDestination, SnappedAreas};
use crate::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
use ak::math;

/// The values of the scroll-snap-align and scroll-snap-type properties that reach here, in the order the CSS enums
/// declare them; the recorder writes them as bytes and the main thread checks the two stay in step.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum SnapAlign {
    #[default]
    None,
    Start,
    End,
    Center,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum SnapStrictness {
    #[default]
    None,
    Proximity,
    Mandatory,
}

// The box a snap area belongs to, which identifies the area across relayouts and across processes. A node's unique ID
// is never reused, so an area removed from the document keeps an identity no live area ever takes again and is no
// longer a snap area the container can be returned to.
impl SnapAreaIdentity {
    pub fn is_valid(&self) -> bool {
        self.node_id.0 != 0
    }

    // 0 for the element's own box; otherwise the pseudo-element the box is generated for, plus one.
    pub fn is_pseudo_element(&self) -> bool {
        self.pseudo_element_type != 0
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SnapAxes {
    pub x: bool,
    pub y: bool,
}

impl SnapAxes {
    pub fn is_empty(self) -> bool {
        !self.x && !self.y
    }
}

/// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-area
#[derive(Default)]
pub struct SnapAreaGeometry {
    pub identity: SnapAreaIdentity,
    /// The transformed border box in the snap container's coordinate space, with the scroll margin added.
    pub rect: CssPixelRect,
    /// The alignment along each physical axis, resolved from the writing mode it applies in.
    pub align_x: SnapAlign,
    pub align_y: SnapAlign,
    pub always_stop: bool,
}

/// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-container
pub struct SnapContainerGeometry {
    /// https://drafts.csswg.org/css-scroll-snap-1/#scroll-padding
    pub snapport: CssPixelRect,
    pub min_scroll_offset: CssPixelPoint,
    pub max_scroll_offset: CssPixelPoint,
    pub strictness: SnapStrictness,
    pub axes: SnapAxes,
    pub horizontal_writing_mode: bool,
}

impl Default for SnapContainerGeometry {
    fn default() -> Self {
        Self {
            snapport: Default::default(),
            min_scroll_offset: Default::default(),
            max_scroll_offset: Default::default(),
            strictness: SnapStrictness::None,
            axes: Default::default(),
            horizontal_writing_mode: true,
        }
    }
}

/// A closed range of scroll offsets that are all valid snap positions.
#[derive(Default)]
pub struct CoveringRange {
    pub start: CssPixels,
    pub end: CssPixels,
}

/// A candidate scroll offset in one axis, produced by aligning a snap area with the snapport.
#[derive(Default)]
pub struct SnapPositionCandidate {
    pub offset: CssPixels,
    pub area: SnapAreaIdentity,
    pub covering_ranges: Vec<CoveringRange>,
    /// The open range of scroll offsets in the container's other axis at which the snap area overlaps the snapport.
    pub cross_axis_visible_range_start: CssPixels,
    pub cross_axis_visible_range_end: CssPixels,
    pub always_stop: bool,
}

#[derive(Default)]
pub struct SnapAxisCandidates {
    pub x_candidates: Vec<SnapPositionCandidate>,
    pub y_candidates: Vec<SnapPositionCandidate>,
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum SnapSelectionStrategyType {
    /// An absolute scroll, or any other operation with only an intended end position.
    #[default]
    EndPosition,
    /// A relative scroll with only an intended direction, such as a mouse wheel step or an arrow key press.
    Direction,
    /// A relative scroll with both an intended direction and end position, such as scrollBy().
    EndPositionAndDirection,
}

/// https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
#[derive(Default)]
pub struct SnapSelectionStrategy {
    pub r#type: SnapSelectionStrategyType,
    /// The scroll offset the operation travels from; a snap position with `scroll-snap-stop: always` must not be
    /// passed over on the way from there to the selected snap position.
    pub start_offset: Option<CssPixelPoint>,
    /// The net offset change the operation's input produced; an axis the operation did not travel in selects no snap
    /// position.
    pub displacement: CssPixelPoint,
    /// Snap positions short of this offset in the direction of travel are not selected; it defaults to the start
    /// offset.
    pub starting_positions_boundary: Option<CssPixelPoint>,
}

// The snap areas a scroll container is snapped to in each axis, so that it can be re-snapped to those same snap areas
// after a content change.
impl SnappedAreas {
    pub fn is_empty(&self) -> bool {
        self.x.is_empty() && self.y.is_empty()
    }
}

/// The selection a scroll makes in one axis.
#[derive(Default)]
pub struct SnapAxisSelection {
    pub destination: CssPixels,
    pub start: CssPixels,
    pub direction: CssPixels,
    pub starting_positions_boundary: Option<CssPixels>,
}

#[derive(Default)]
pub struct SnapAxisChoice {
    pub offset: CssPixels,
    pub area: SnapAreaIdentity,
}

struct SnapAxisGeometry {
    snapport_start: CssPixels,
    snapport_size: CssPixels,
    min_offset: CssPixels,
    max_offset: CssPixels,
}

fn abs(value: CssPixels) -> CssPixels {
    if value < CssPixels::default() { -value } else { value }
}

fn x_axis_geometry(geometry: &SnapContainerGeometry) -> SnapAxisGeometry {
    SnapAxisGeometry {
        snapport_start: geometry.snapport.left(),
        snapport_size: geometry.snapport.width,
        min_offset: geometry.min_scroll_offset.x,
        max_offset: geometry.max_scroll_offset.x,
    }
}

fn y_axis_geometry(geometry: &SnapContainerGeometry) -> SnapAxisGeometry {
    SnapAxisGeometry {
        snapport_start: geometry.snapport.top(),
        snapport_size: geometry.snapport.height,
        min_offset: geometry.min_scroll_offset.y,
        max_offset: geometry.max_scroll_offset.y,
    }
}

fn snap_position_candidate_for_axis(
    alignment: SnapAlign,
    area_start: CssPixels,
    area_size: CssPixels,
    geometry: &SnapAxisGeometry,
) -> Option<SnapPositionCandidate> {
    let offset = match alignment {
        SnapAlign::None => return None,
        SnapAlign::Start => area_start - geometry.snapport_start,
        SnapAlign::End => area_start + area_size - (geometry.snapport_start + geometry.snapport_size),
        SnapAlign::Center => area_start + area_size / 2 - (geometry.snapport_start + geometry.snapport_size / 2),
    };

    // https://drafts.csswg.org/css-scroll-snap-1/#unreachable
    // If a snap position is unreachable as specified, such that aligning to it would require scrolling the scroll
    // container's viewport past the edge of its scrollable overflow area, the used snap position for this snap area is
    // the position resulting from scrolling as much as possible in each relevant axis toward the desired snap position.
    let mut candidate = SnapPositionCandidate {
        offset: offset.clamp(geometry.min_offset, geometry.max_offset),
        ..Default::default()
    };
    if area_size > geometry.snapport_size {
        candidate.covering_ranges.push(CoveringRange {
            start: (area_start - geometry.snapport_start).clamp(geometry.min_offset, geometry.max_offset),
            end: (area_start + area_size - (geometry.snapport_start + geometry.snapport_size))
                .clamp(geometry.min_offset, geometry.max_offset),
        });
    }
    Some(candidate)
}

fn restrict_covering_ranges_to_valid_snap_positions(
    candidates: &mut [SnapPositionCandidate],
    snapport_size: CssPixels,
) {
    let mut snap_positions = Vec::with_capacity(candidates.len());
    for candidate in candidates.iter() {
        snap_positions.push(candidate.offset);
    }
    snap_positions.sort_unstable();
    for candidate in candidates {
        if candidate.covering_ranges.is_empty() {
            continue;
        }
        let covering_range = candidate.covering_ranges.remove(0);
        candidate.covering_ranges.clear();
        let mut append_valid_offsets_between = |start: CssPixels, end: CssPixels| {
            let start = start.max(covering_range.start);
            let end = end.min(covering_range.end);
            if start <= end {
                candidate.covering_ranges.push(CoveringRange { start, end });
            }
        };
        append_valid_offsets_between(covering_range.start, snap_positions[0]);
        for i in 1..snap_positions.len() {
            if snap_positions[i] - snap_positions[i - 1] > snapport_size {
                append_valid_offsets_between(snap_positions[i - 1], snap_positions[i]);
            }
        }
        append_valid_offsets_between(snap_positions[snap_positions.len() - 1], covering_range.end);
    }
}

fn is_beyond_in_direction(offset: CssPixels, boundary: CssPixels, direction: CssPixels) -> bool {
    // AD-HOC: Offsets within one pixel of the offset a scroll travels from count as being at it, so that a fractional
    //         scroll offset cannot re-select the snap position the scroll started from. This matches other engines.
    #[allow(non_snake_case)]
    let SNAP_POSITION_BOUNDARY_TOLERANCE: CssPixels = CssPixels::from_integer(1);
    if direction > CssPixels::default() {
        offset >= boundary + SNAP_POSITION_BOUNDARY_TOLERANCE
    } else {
        offset <= boundary - SNAP_POSITION_BOUNDARY_TOLERANCE
    }
}

fn is_at_or_beyond_in_direction(offset: CssPixels, boundary: CssPixels, direction: CssPixels) -> bool {
    if direction > CssPixels::default() {
        offset >= boundary
    } else {
        offset <= boundary
    }
}

pub fn snap_area_is_visible_at_cross_axis_offset(
    candidate: &SnapPositionCandidate,
    cross_axis_offset: CssPixels,
) -> bool {
    cross_axis_offset > candidate.cross_axis_visible_range_start
        && cross_axis_offset < candidate.cross_axis_visible_range_end
}

/// Every offset within a covering range is a valid snap position of the area that contributes it.
pub fn candidate_has_snap_position_at(candidate: &SnapPositionCandidate, offset: CssPixels) -> bool {
    if candidate.offset == offset {
        return true;
    }
    candidate
        .covering_ranges
        .iter()
        .any(|range| offset >= range.start && offset <= range.end)
}

fn chosen_offset_is_visible_at_cross_axis_offset(
    candidates: &[SnapPositionCandidate],
    offset: CssPixels,
    cross_axis_offset: CssPixels,
) -> bool {
    candidates.iter().any(|candidate| {
        snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset)
            && candidate_has_snap_position_at(candidate, offset)
    })
}

pub fn snap_destination_for(
    unsnapped_destination: CssPixelPoint,
    x_offset: Option<CssPixels>,
    y_offset: Option<CssPixels>,
    evaluated_axes: SnapAxes,
) -> SnapDestination {
    SnapDestination {
        position: CssPixelPoint {
            x: x_offset.unwrap_or(unsnapped_destination.x),
            y: y_offset.unwrap_or(unsnapped_destination.y),
        },
        snapped_x: x_offset.is_some(),
        snapped_y: y_offset.is_some(),
        evaluated_x: evaluated_axes.x,
        evaluated_y: evaluated_axes.y,
        snapped_areas: Default::default(),
    }
}

pub fn chosen_offsets_are_mutually_visible(
    candidates: &SnapAxisCandidates,
    x_offset: CssPixels,
    y_offset: CssPixels,
) -> bool {
    chosen_offset_is_visible_at_cross_axis_offset(&candidates.x_candidates, x_offset, y_offset)
        && chosen_offset_is_visible_at_cross_axis_offset(&candidates.y_candidates, y_offset, x_offset)
}

pub fn choose_snap_offset_for_axis(
    candidates: &[SnapPositionCandidate],
    selection: &SnapAxisSelection,
    snapport_size: CssPixels,
    strictness: SnapStrictness,
    cross_axis_offset: Option<CssPixels>,
    only_area: Option<SnapAreaIdentity>,
) -> Option<SnapAxisChoice> {
    // AD-HOC: The parameters under which a proximity snap container snaps are left to the user agent. Match the
    //         threshold used by other engines, one third of the snapport size in the snapping axis.
    let proximity_range = snapport_size / 3;
    #[allow(non_snake_case)]
    let SNAP_POSITION_BOUNDARY_TOLERANCE: CssPixels = CssPixels::from_integer(1);
    let mut best_choice = None;
    let mut best_distance = CssPixels::default();
    let mut consider_candidate = |offset: CssPixels, candidate: &SnapPositionCandidate| {
        let distance = abs(offset - selection.destination);
        if strictness == SnapStrictness::Proximity && distance > proximity_range {
            return;
        }
        if best_choice.is_none() || distance < best_distance {
            best_choice = Some(SnapAxisChoice {
                offset,
                area: candidate.area,
            });
            best_distance = distance;
        }
    };
    let mut first_always_stop_choice: Option<SnapAxisChoice> = None;
    let mut track_always_stop_candidate = |candidate: &SnapPositionCandidate| {
        if first_always_stop_choice
            .as_ref()
            .is_none_or(|choice| abs(candidate.offset - selection.start) < abs(choice.offset - selection.start))
        {
            first_always_stop_choice = Some(SnapAxisChoice {
                offset: candidate.offset,
                area: candidate.area,
            });
        }
    };
    for candidate in candidates {
        if only_area.as_ref().is_some_and(|area| candidate.area != *area) {
            continue;
        }

        // https://drafts.csswg.org/css-scroll-snap-1/#snap-scope
        // Since the purpose of scroll snapping is to align content within the scrollport for optimal viewing, a
        // scroll position cannot be considered a valid snap position if snapping to it would leave the contributing
        // snap area entirely outside the snapport, even if it otherwise satisfies the required alignment of the snap
        // area.
        if cross_axis_offset.is_some_and(|offset| !snap_area_is_visible_at_cross_axis_offset(candidate, offset)) {
            continue;
        }
        if selection.direction != CssPixels::default()
            && candidate.always_stop
            && is_beyond_in_direction(candidate.offset, selection.start, selection.direction)
        {
            track_always_stop_candidate(candidate);
        }
        if selection.starting_positions_boundary.is_none_or(|boundary| {
            is_beyond_in_direction(candidate.offset, selection.start, selection.direction)
                && is_at_or_beyond_in_direction(candidate.offset, boundary, selection.direction)
        }) {
            consider_candidate(candidate.offset, candidate);
        }

        // NB: Every offset in a covering range is a valid snap position, so such a range contributes the offset in it
        //     nearest the destination. A relative scroll may not select the part of a range it has already traveled
        //     past, which is the part at or behind the offset it started from.
        // FIXME: Limit covering ranges to the offsets at which no snap area with scroll-snap-stop: always has entered
        //        the snapport yet, so a scroll within a covering snap area still stops ahead of such an area.
        for covering_range in &candidate.covering_ranges {
            let mut range_start = covering_range.start;
            let mut range_end = covering_range.end;
            if selection.starting_positions_boundary.is_some() {
                if selection.direction > CssPixels::default() {
                    range_start = range_start.max(selection.start + SNAP_POSITION_BOUNDARY_TOLERANCE);
                } else if selection.direction < CssPixels::default() {
                    range_end = range_end.min(selection.start - SNAP_POSITION_BOUNDARY_TOLERANCE);
                }
            }
            if range_start > range_end {
                continue;
            }
            consider_candidate(selection.destination.clamp(range_start, range_end), candidate);
        }
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#valdef-scroll-snap-type-mandatory
    // If a valid snap position exists then the scroll container must snap at the termination of a scroll (if none
    // exist then no snapping occurs).
    // NB: A mandatory container whose scroll has no snap position ahead of it in the direction of travel therefore
    //     falls back to the snap position nearest the destination.
    if best_choice.is_none()
        && selection.starting_positions_boundary.is_some()
        && strictness == SnapStrictness::Mandatory
    {
        let fallback_selection = SnapAxisSelection {
            destination: selection.destination,
            start: selection.destination,
            direction: CssPixels::default(),
            starting_positions_boundary: None,
        };
        best_choice = choose_snap_offset_for_axis(
            candidates,
            &fallback_selection,
            snapport_size,
            strictness,
            cross_axis_offset,
            only_area,
        );
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-stop
    // always
    //     The scroll container must not pass over a snap position defined by this element during the execution of a
    //     scrolling operation; it must instead snap to the first of this element's snap positions.
    if let (Some(best), Some(first)) = (best_choice.as_ref(), first_always_stop_choice.as_ref()) {
        let always_stop_position_is_passed_over = if selection.direction > CssPixels::default() {
            best.offset > first.offset
        } else {
            best.offset < first.offset
        };
        if always_stop_position_is_passed_over {
            best_choice = first_always_stop_choice;
        }
    }
    best_choice
}

/// The axes a scroll selects a snap position in: those the container snaps in that the scroll traveled in, or every
/// axis the container snaps in when the scroll traveled in none.
pub fn axes_to_evaluate(container_axes: SnapAxes, strategy: &SnapSelectionStrategy) -> SnapAxes {
    // NB: A scroll selects a snap position only in the axes it traveled in, so that the offset of an axis its input
    //     never moved is left where it is. A scroll that traveled in no axis, such as one with only an intended end
    //     position, selects a snap position in every axis the container snaps in.
    let snaps_in_axis = |container_snaps_in_axis: bool, axis_displacement: CssPixels| {
        container_snaps_in_axis && (strategy.displacement.is_zero() || axis_displacement != CssPixels::default())
    };
    SnapAxes {
        x: snaps_in_axis(container_axes.x, strategy.displacement.x),
        y: snaps_in_axis(container_axes.y, strategy.displacement.y),
    }
}

pub fn build_snap_candidates(
    geometry: &SnapContainerGeometry,
    areas: &[SnapAreaGeometry],
    collect_axes: SnapAxes,
) -> SnapAxisCandidates {
    let x_geometry = x_axis_geometry(geometry);
    let y_geometry = y_axis_geometry(geometry);
    let mut candidates = SnapAxisCandidates::default();
    for area in areas {
        let area_rect = &area.rect;
        if collect_axes.x
            && let Some(mut candidate) =
                snap_position_candidate_for_axis(area.align_x, area_rect.left(), area_rect.width, &x_geometry)
        {
            candidate.area = area.identity;
            candidate.always_stop = area.always_stop;
            candidate.cross_axis_visible_range_start =
                area_rect.top() - y_geometry.snapport_start - y_geometry.snapport_size;
            candidate.cross_axis_visible_range_end = area_rect.bottom() - y_geometry.snapport_start;
            candidates.x_candidates.push(candidate);
        }
        if collect_axes.y
            && let Some(mut candidate) =
                snap_position_candidate_for_axis(area.align_y, area_rect.top(), area_rect.height, &y_geometry)
        {
            candidate.area = area.identity;
            candidate.always_stop = area.always_stop;
            candidate.cross_axis_visible_range_start =
                area_rect.left() - x_geometry.snapport_start - x_geometry.snapport_size;
            candidate.cross_axis_visible_range_end = area_rect.right() - x_geometry.snapport_start;
            candidates.y_candidates.push(candidate);
        }
    }
    if collect_axes.x {
        restrict_covering_ranges_to_valid_snap_positions(&mut candidates.x_candidates, x_geometry.snapport_size);
    }
    if collect_axes.y {
        restrict_covering_ranges_to_valid_snap_positions(&mut candidates.y_candidates, y_geometry.snapport_size);
    }
    candidates
}

pub fn snap_areas_at_offset(
    candidates: &[SnapPositionCandidate],
    offset: CssPixels,
    cross_axis_offset: CssPixels,
) -> Vec<SnapAreaIdentity> {
    let mut areas = Vec::new();
    for candidate in candidates {
        if !snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset) {
            continue;
        }
        if candidate_has_snap_position_at(candidate, offset) && candidate.area.is_valid() {
            areas.push(candidate.area);
        }
    }
    areas
}

/// https://drafts.csswg.org/css-scroll-snap-1/#choosing
pub fn select_snap_destination_with_candidates(
    geometry: &SnapContainerGeometry,
    candidates: &SnapAxisCandidates,
    destination: CssPixelPoint,
    strategy: &SnapSelectionStrategy,
    evaluated_axes: SnapAxes,
) -> SnapDestination {
    let snaps_x = evaluated_axes.x;
    let snaps_y = evaluated_axes.y;
    if !snaps_x && !snaps_y {
        return SnapDestination {
            position: destination,
            ..Default::default()
        };
    }
    let snapport = &geometry.snapport;
    let strictness = geometry.strictness;
    let axis_selection = |axis_destination: CssPixels,
                          axis_displacement: CssPixels,
                          axis_start: Option<CssPixels>,
                          axis_boundary: Option<CssPixels>| {
        let Some(axis_start) = axis_start.filter(|_| axis_displacement != CssPixels::default()) else {
            return SnapAxisSelection {
                destination: axis_destination,
                start: axis_destination,
                direction: CssPixels::default(),
                starting_positions_boundary: None,
            };
        };
        let mut boundary = None;
        if strategy.r#type != SnapSelectionStrategyType::EndPosition {
            boundary = Some(axis_boundary.unwrap_or(axis_start));
        }
        SnapAxisSelection {
            destination: axis_destination,
            start: axis_start,
            direction: axis_displacement,
            starting_positions_boundary: boundary,
        }
    };
    let axis_of = |offset: Option<CssPixelPoint>, horizontal: bool| {
        offset.map(|offset| if horizontal { offset.x } else { offset.y })
    };
    let x_selection = axis_selection(
        destination.x,
        strategy.displacement.x,
        axis_of(strategy.start_offset, true),
        axis_of(strategy.starting_positions_boundary, true),
    );
    let y_selection = axis_selection(
        destination.y,
        strategy.displacement.y,
        axis_of(strategy.start_offset, false),
        axis_of(strategy.starting_positions_boundary, false),
    );
    let choose_x = |cross_axis_offset: Option<CssPixels>, only_area: Option<SnapAreaIdentity>| {
        choose_snap_offset_for_axis(
            &candidates.x_candidates,
            &x_selection,
            snapport.width,
            strictness,
            cross_axis_offset,
            only_area,
        )
    };
    let choose_y = |cross_axis_offset: Option<CssPixels>, only_area: Option<SnapAreaIdentity>| {
        choose_snap_offset_for_axis(
            &candidates.y_candidates,
            &y_selection,
            snapport.height,
            strictness,
            cross_axis_offset,
            only_area,
        )
    };
    let mut x_choice = None;
    let mut y_choice = None;
    if snaps_x && snaps_y {
        x_choice = choose_x(None, None);
        y_choice = choose_y(None, None);
        if let (Some(x), Some(y)) = (x_choice.as_ref(), y_choice.as_ref())
            && !chosen_offsets_are_mutually_visible(candidates, x.offset, y.offset)
        {
            // AD-HOC: A snap area is visible at its own snap positions, so one axis keeps the position it chose while
            //         the other gives up its own and takes the one the same area offers. Of the two areas, the one
            //         leaving the scroll container nearest its destination is followed. This matches other engines.
            let mut y_of_x_area = None;
            let mut x_of_y_area = None;
            if x.area.is_valid() {
                y_of_x_area = choose_y(Some(x.offset), Some(x.area));
            }
            if y.area.is_valid() {
                x_of_y_area = choose_x(Some(y.offset), Some(y.area));
            }
            let distance_to_destination = |x_axis_choice: &SnapAxisChoice, y_axis_choice: &SnapAxisChoice| {
                math::hypot(
                    (x_axis_choice.offset - x_selection.destination).to_double(),
                    (y_axis_choice.offset - y_selection.destination).to_double(),
                )
            };
            let mut follows_x_area = y_of_x_area.is_some();
            if let (Some(y_of_x), Some(x_of_y)) = (y_of_x_area.as_ref(), x_of_y_area.as_ref()) {
                follows_x_area = distance_to_destination(x, y_of_x) < distance_to_destination(x_of_y, y);
            }
            if follows_x_area {
                y_choice = y_of_x_area;
            } else if x_of_y_area.is_some() {
                x_choice = x_of_y_area;
            } else {
                // NB: Neither area offers a snap position in both axes, so the axis whose chosen offset is farther
                //     from its destination is chosen again from the positions visible at the other axis's offset.
                if abs(x.offset - x_selection.destination) <= abs(y.offset - y_selection.destination) {
                    y_choice = choose_y(Some(x.offset), None);
                } else {
                    x_choice = choose_x(Some(y.offset), None);
                }
            }
        }
        if x_choice.is_some() && y_choice.is_none() {
            x_choice = choose_x(Some(destination.y), None);
        } else if y_choice.is_some() && x_choice.is_none() {
            y_choice = choose_y(Some(destination.x), None);
        }
    } else if snaps_x {
        x_choice = choose_x(Some(destination.y), None);
    } else if snaps_y {
        y_choice = choose_y(Some(destination.x), None);
    }
    let x_offset = x_choice.map(|choice| choice.offset);
    let y_offset = y_choice.map(|choice| choice.offset);
    let mut snap_destination =
        snap_destination_for(destination, x_offset, y_offset, SnapAxes { x: snaps_x, y: snaps_y });
    if let Some(x_offset) = x_offset {
        snap_destination.snapped_areas.x =
            snap_areas_at_offset(&candidates.x_candidates, x_offset, snap_destination.position.y);
    }
    if let Some(y_offset) = y_offset {
        snap_destination.snapped_areas.y =
            snap_areas_at_offset(&candidates.y_candidates, y_offset, snap_destination.position.x);
    }
    snap_destination
}

/// https://drafts.csswg.org/css-scroll-snap-1/#choosing
pub fn select_snap_destination(
    geometry: &SnapContainerGeometry,
    areas: &[SnapAreaGeometry],
    destination: CssPixelPoint,
    strategy: &SnapSelectionStrategy,
) -> SnapDestination {
    let evaluated_axes = axes_to_evaluate(geometry.axes, strategy);
    if evaluated_axes.is_empty() {
        return SnapDestination {
            position: destination,
            ..Default::default()
        };
    }
    let candidates = build_snap_candidates(geometry, areas, evaluated_axes);
    select_snap_destination_with_candidates(geometry, &candidates, destination, strategy, evaluated_axes)
}

// The largest share of a momentum delta that the delta after it may keep for the momentum to be considered decaying.
const MAXIMUM_MOMENTUM_DECAY_SHARE: f64 = 0.96;

// The share a momentum delta keeps of the one before it is treated as no larger than this, so that momentum which
// barely decays is predicted to travel a hundred times the distance of its latest delta rather than forever.
const MAXIMUM_SLOW_MOMENTUM_DECAY_SHARE: f64 = 0.99;

// The number of consecutively smaller momentum deltas after which momentum that decays only slowly is predicted from
// anyway.
const DECAYING_DELTAS_BEFORE_SLOW_DECAY_IS_PREDICTED_FROM: u32 = 3;

/// The displacement the momentum of a flick has left to travel, estimated from the deltas that momentum has produced
/// so far.
#[derive(Default)]
pub struct MomentumFlingEstimator {
    previous_momentum_delta: Option<CssPixelPoint>,
    consecutively_decaying_momentum_deltas: u32,
}

impl MomentumFlingEstimator {
    pub fn reset(&mut self) {
        self.previous_momentum_delta = None;
        self.consecutively_decaying_momentum_deltas = 0;
    }

    /// The displacement left to travel, including the delta given; momentum that has not yet decayed far enough to
    /// tell where it is headed reports no estimate.
    pub fn estimate_remaining_displacement(&mut self, momentum_delta: CssPixelPoint) -> Option<CssPixelPoint> {
        let previous_momentum_delta = self.previous_momentum_delta;
        self.previous_momentum_delta = Some(momentum_delta);

        // The share the delta keeps of the one before it is what the momentum decays by, so the first delta of a flick
        // says nothing about where it is headed.
        let previous_momentum_delta = previous_momentum_delta?;
        let distance = math::hypot(momentum_delta.x.to_double(), momentum_delta.y.to_double());
        let previous_distance = math::hypot(
            previous_momentum_delta.x.to_double(),
            previous_momentum_delta.y.to_double(),
        );
        if previous_distance <= 0.0 {
            return None;
        }
        let decay_share = distance / previous_distance;
        if decay_share < 1.0 {
            self.consecutively_decaying_momentum_deltas = self.consecutively_decaying_momentum_deltas.wrapping_add(1);
        } else {
            self.consecutively_decaying_momentum_deltas = 0;
        }
        let momentum_is_decaying = decay_share < MAXIMUM_MOMENTUM_DECAY_SHARE
            || (self.consecutively_decaying_momentum_deltas >= DECAYING_DELTAS_BEFORE_SLOW_DECAY_IS_PREDICTED_FROM
                && decay_share < 1.0);
        if !momentum_is_decaying {
            return None;
        }

        // Each delta keeps the same share of the one before it, so the deltas still to come sum to the delta given
        // divided by the share it loses each time.
        let remaining_distance_factor = 1.0 / (1.0 - super::min(decay_share, MAXIMUM_SLOW_MOMENTUM_DECAY_SHARE));
        Some(CssPixelPoint {
            x: CssPixels::nearest_value_for(momentum_delta.x.to_double() * remaining_distance_factor),
            y: CssPixels::nearest_value_for(momentum_delta.y.to_double() * remaining_distance_factor),
        })
    }
}
