/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <LibWeb/Compositor/ScrollSnapSelection.h>

namespace Web::Compositor {

struct SnapAxisGeometry {
    CSSPixels snapport_start;
    CSSPixels snapport_size;
    CSSPixels min_offset;
    CSSPixels max_offset;
};

static SnapAxisGeometry x_axis_geometry(SnapContainerGeometry const& geometry)
{
    return {
        .snapport_start = geometry.snapport.left(),
        .snapport_size = geometry.snapport.width(),
        .min_offset = geometry.min_scroll_offset.x(),
        .max_offset = geometry.max_scroll_offset.x(),
    };
}

static SnapAxisGeometry y_axis_geometry(SnapContainerGeometry const& geometry)
{
    return {
        .snapport_start = geometry.snapport.top(),
        .snapport_size = geometry.snapport.height(),
        .min_offset = geometry.min_scroll_offset.y(),
        .max_offset = geometry.max_scroll_offset.y(),
    };
}

static Optional<SnapPositionCandidate> snap_position_candidate_for_axis(CSS::ScrollSnapAlign alignment, CSSPixels area_start, CSSPixels area_size, SnapAxisGeometry const& geometry)
{
    CSSPixels offset;
    switch (alignment) {
    case CSS::ScrollSnapAlign::None:
        return {};
    case CSS::ScrollSnapAlign::Start:
        offset = area_start - geometry.snapport_start;
        break;
    case CSS::ScrollSnapAlign::End:
        offset = area_start + area_size - (geometry.snapport_start + geometry.snapport_size);
        break;
    case CSS::ScrollSnapAlign::Center:
        offset = area_start + area_size / 2 - (geometry.snapport_start + geometry.snapport_size / 2);
        break;
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#unreachable
    // If a snap position is unreachable as specified, such that aligning to it would require scrolling the scroll
    // container's viewport past the edge of its scrollable overflow area, the used snap position for this snap area is
    // the position resulting from scrolling as much as possible in each relevant axis toward the desired snap position.
    SnapPositionCandidate candidate { .offset = clamp(offset, geometry.min_offset, geometry.max_offset), .covering_ranges = {} };

    if (area_size > geometry.snapport_size) {
        candidate.covering_ranges.append({
            .start = clamp(area_start - geometry.snapport_start, geometry.min_offset, geometry.max_offset),
            .end = clamp(area_start + area_size - (geometry.snapport_start + geometry.snapport_size), geometry.min_offset, geometry.max_offset),
        });
    }

    return candidate;
}

static void restrict_covering_ranges_to_valid_snap_positions(Vector<SnapPositionCandidate>& candidates, CSSPixels snapport_size)
{
    Vector<CSSPixels> snap_positions;
    snap_positions.ensure_capacity(candidates.size());
    for (auto const& candidate : candidates)
        snap_positions.unchecked_append(candidate.offset);
    quick_sort(snap_positions);

    for (auto& candidate : candidates) {
        if (candidate.covering_ranges.is_empty())
            continue;

        auto covering_range = candidate.covering_ranges.first();
        candidate.covering_ranges.clear_with_capacity();

        auto append_valid_offsets_between = [&](CSSPixels start, CSSPixels end) {
            start = max(start, covering_range.start);
            end = min(end, covering_range.end);
            if (start <= end)
                candidate.covering_ranges.append({ .start = start, .end = end });
        };

        append_valid_offsets_between(covering_range.start, snap_positions.first());
        for (size_t i = 1; i < snap_positions.size(); ++i) {
            if (snap_positions[i] - snap_positions[i - 1] > snapport_size)
                append_valid_offsets_between(snap_positions[i - 1], snap_positions[i]);
        }
        append_valid_offsets_between(snap_positions.last(), covering_range.end);
    }
}

// AD-HOC: Offsets within one pixel of the offset a scroll travels from count as being at it, so that a fractional
//         scroll offset cannot re-select the snap position the scroll started from. This matches other engines.
static constexpr CSSPixels SNAP_POSITION_BOUNDARY_TOLERANCE = 1;

static bool is_beyond_in_direction(CSSPixels offset, CSSPixels boundary, CSSPixels direction)
{
    if (direction > 0)
        return offset >= boundary + SNAP_POSITION_BOUNDARY_TOLERANCE;
    return offset <= boundary - SNAP_POSITION_BOUNDARY_TOLERANCE;
}

static bool is_at_or_beyond_in_direction(CSSPixels offset, CSSPixels boundary, CSSPixels direction)
{
    if (direction > 0)
        return offset >= boundary;
    return offset <= boundary;
}

bool snap_area_is_visible_at_cross_axis_offset(SnapPositionCandidate const& candidate, CSSPixels cross_axis_offset)
{
    return cross_axis_offset > candidate.cross_axis_visible_range_start && cross_axis_offset < candidate.cross_axis_visible_range_end;
}

// Every offset within a covering range is a valid snap position of the area that contributes it.
bool candidate_has_snap_position_at(SnapPositionCandidate const& candidate, CSSPixels offset)
{
    if (candidate.offset == offset)
        return true;
    return any_of(candidate.covering_ranges, [&](auto const& covering_range) { return offset >= covering_range.start && offset <= covering_range.end; });
}

static bool chosen_offset_is_visible_at_cross_axis_offset(Vector<SnapPositionCandidate> const& candidates, CSSPixels offset, CSSPixels cross_axis_offset)
{
    return any_of(candidates, [&](auto const& candidate) {
        return snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset) && candidate_has_snap_position_at(candidate, offset);
    });
}

SnapDestination snap_destination_for(CSSPixelPoint unsnapped_destination, Optional<CSSPixels> x_offset, Optional<CSSPixels> y_offset, SnapAxes evaluated_axes)
{
    return {
        .position = { x_offset.value_or(unsnapped_destination.x()), y_offset.value_or(unsnapped_destination.y()) },
        .snapped_x = x_offset.has_value(),
        .snapped_y = y_offset.has_value(),
        .evaluated_x = evaluated_axes.x,
        .evaluated_y = evaluated_axes.y,
    };
}

bool chosen_offsets_are_mutually_visible(SnapAxisCandidates const& candidates, CSSPixels x_offset, CSSPixels y_offset)
{
    return chosen_offset_is_visible_at_cross_axis_offset(candidates.x_candidates, x_offset, y_offset)
        && chosen_offset_is_visible_at_cross_axis_offset(candidates.y_candidates, y_offset, x_offset);
}

Optional<SnapAxisChoice> choose_snap_offset_for_axis(Vector<SnapPositionCandidate> const& candidates, SnapAxisSelection const& selection, CSSPixels snapport_size, CSS::ScrollSnapStrictness strictness, Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaIdentity> only_area)
{
    // AD-HOC: The parameters under which a proximity snap container snaps are left to the user agent. Match the
    //         threshold used by other engines, one third of the snapport size in the snapping axis.
    auto proximity_range = snapport_size / 3;

    Optional<SnapAxisChoice> best_choice;
    CSSPixels best_distance = 0;
    auto consider_candidate = [&](CSSPixels offset, SnapPositionCandidate const& candidate) {
        auto distance = abs(offset - selection.destination);
        if (strictness == CSS::ScrollSnapStrictness::Proximity && distance > proximity_range)
            return;
        if (!best_choice.has_value() || distance < best_distance) {
            best_choice = SnapAxisChoice { offset, candidate.area };
            best_distance = distance;
        }
    };

    Optional<SnapAxisChoice> first_always_stop_choice;
    auto track_always_stop_candidate = [&](SnapPositionCandidate const& candidate) {
        if (!first_always_stop_choice.has_value() || abs(candidate.offset - selection.start) < abs(first_always_stop_choice->offset - selection.start))
            first_always_stop_choice = SnapAxisChoice { candidate.offset, candidate.area };
    };

    for (auto const& candidate : candidates) {
        if (only_area.has_value() && candidate.area != *only_area)
            continue;

        // https://drafts.csswg.org/css-scroll-snap-1/#snap-scope
        // Since the purpose of scroll snapping is to align content within the scrollport for optimal viewing, a
        // scroll position cannot be considered a valid snap position if snapping to it would leave the contributing
        // snap area entirely outside the snapport, even if it otherwise satisfies the required alignment of the snap
        // area.
        if (cross_axis_offset.has_value() && !snap_area_is_visible_at_cross_axis_offset(candidate, *cross_axis_offset))
            continue;

        if (selection.direction != 0 && candidate.always_stop && is_beyond_in_direction(candidate.offset, selection.start, selection.direction))
            track_always_stop_candidate(candidate);

        if (!selection.starting_positions_boundary.has_value()
            || (is_beyond_in_direction(candidate.offset, selection.start, selection.direction)
                && is_at_or_beyond_in_direction(candidate.offset, *selection.starting_positions_boundary, selection.direction)))
            consider_candidate(candidate.offset, candidate);

        // NB: Every offset in a covering range is a valid snap position, so such a range contributes the offset in it
        //     nearest the destination. A relative scroll may not select the part of a range it has already traveled
        //     past, which is the part at or behind the offset it started from.
        // FIXME: Limit covering ranges to the offsets at which no snap area with scroll-snap-stop: always has entered
        //        the snapport yet, so a scroll within a covering snap area still stops ahead of such an area.
        for (auto const& covering_range : candidate.covering_ranges) {
            auto range_start = covering_range.start;
            auto range_end = covering_range.end;
            if (selection.starting_positions_boundary.has_value()) {
                if (selection.direction > 0) {
                    range_start = max(range_start, selection.start + SNAP_POSITION_BOUNDARY_TOLERANCE);
                } else if (selection.direction < 0) {
                    range_end = min(range_end, selection.start - SNAP_POSITION_BOUNDARY_TOLERANCE);
                }
            }
            if (range_start > range_end)
                continue;

            consider_candidate(clamp(selection.destination, range_start, range_end), candidate);
        }
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#valdef-scroll-snap-type-mandatory
    // If a valid snap position exists then the scroll container must snap at the termination of a scroll (if none
    // exist then no snapping occurs).
    // NB: A mandatory container whose scroll has no snap position ahead of it in the direction of travel therefore
    //     falls back to the snap position nearest the destination.
    if (!best_choice.has_value() && selection.starting_positions_boundary.has_value() && strictness == CSS::ScrollSnapStrictness::Mandatory) {
        SnapAxisSelection fallback_selection {
            .destination = selection.destination,
            .start = selection.destination,
            .direction = 0,
            .starting_positions_boundary = {},
        };
        best_choice = choose_snap_offset_for_axis(candidates, fallback_selection, snapport_size, strictness, cross_axis_offset, only_area);
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-stop
    // always
    //     The scroll container must not pass over a snap position defined by this element during the execution of a
    //     scrolling operation; it must instead snap to the first of this element's snap positions.
    if (best_choice.has_value() && first_always_stop_choice.has_value()) {
        bool always_stop_position_is_passed_over = selection.direction > 0
            ? best_choice->offset > first_always_stop_choice->offset
            : best_choice->offset < first_always_stop_choice->offset;
        if (always_stop_position_is_passed_over)
            best_choice = first_always_stop_choice;
    }

    return best_choice;
}

SnapAxes axes_to_evaluate(SnapAxes container_axes, SnapSelectionStrategy const& strategy)
{
    // NB: A scroll selects a snap position only in the axes it traveled in, so that the offset of an axis its input
    //     never moved is left where it is. A scroll that traveled in no axis, such as one with only an intended end
    //     position, selects a snap position in every axis the container snaps in.
    auto snaps_in_axis = [&](bool container_snaps_in_axis, CSSPixels axis_displacement) {
        return container_snaps_in_axis && (strategy.displacement.is_zero() || axis_displacement != 0);
    };
    return {
        .x = snaps_in_axis(container_axes.x, strategy.displacement.x()),
        .y = snaps_in_axis(container_axes.y, strategy.displacement.y()),
    };
}

SnapAxisCandidates build_snap_candidates(SnapContainerGeometry const& geometry, ReadonlySpan<SnapAreaGeometry> areas, SnapAxes collect_axes)
{
    auto x_geometry = x_axis_geometry(geometry);
    auto y_geometry = y_axis_geometry(geometry);

    SnapAxisCandidates candidates;
    for (auto const& area : areas) {
        auto const& area_rect = area.rect;
        if (collect_axes.x) {
            if (auto candidate = snap_position_candidate_for_axis(area.align_x, area_rect.left(), area_rect.width(), x_geometry); candidate.has_value()) {
                candidate->area = area.identity;
                candidate->always_stop = area.always_stop;
                candidate->cross_axis_visible_range_start = area_rect.top() - y_geometry.snapport_start - y_geometry.snapport_size;
                candidate->cross_axis_visible_range_end = area_rect.bottom() - y_geometry.snapport_start;
                candidates.x_candidates.append(*candidate);
            }
        }
        if (collect_axes.y) {
            if (auto candidate = snap_position_candidate_for_axis(area.align_y, area_rect.top(), area_rect.height(), y_geometry); candidate.has_value()) {
                candidate->area = area.identity;
                candidate->always_stop = area.always_stop;
                candidate->cross_axis_visible_range_start = area_rect.left() - x_geometry.snapport_start - x_geometry.snapport_size;
                candidate->cross_axis_visible_range_end = area_rect.right() - x_geometry.snapport_start;
                candidates.y_candidates.append(*candidate);
            }
        }
    }

    if (collect_axes.x)
        restrict_covering_ranges_to_valid_snap_positions(candidates.x_candidates, x_geometry.snapport_size);
    if (collect_axes.y)
        restrict_covering_ranges_to_valid_snap_positions(candidates.y_candidates, y_geometry.snapport_size);

    return candidates;
}

Vector<SnapAreaIdentity> snap_areas_at_offset(Vector<SnapPositionCandidate> const& candidates, CSSPixels offset, CSSPixels cross_axis_offset)
{
    Vector<SnapAreaIdentity> areas;
    for (auto const& candidate : candidates) {
        if (!snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset))
            continue;
        if (candidate_has_snap_position_at(candidate, offset) && candidate.area.is_valid())
            areas.append(candidate.area);
    }
    return areas;
}

// https://drafts.csswg.org/css-scroll-snap-1/#choosing
SnapDestination select_snap_destination(SnapContainerGeometry const& geometry, SnapAxisCandidates const& candidates, CSSPixelPoint destination, SnapSelectionStrategy const& strategy, SnapAxes evaluated_axes)
{
    bool snaps_x = evaluated_axes.x;
    bool snaps_y = evaluated_axes.y;
    if (!snaps_x && !snaps_y)
        return { destination };

    auto const& snapport = geometry.snapport;
    auto strictness = geometry.strictness;

    auto axis_selection = [&](CSSPixels axis_destination, CSSPixels axis_displacement, Optional<CSSPixels> axis_start, Optional<CSSPixels> axis_boundary) {
        if (!axis_start.has_value() || axis_displacement == 0) {
            return SnapAxisSelection {
                .destination = axis_destination,
                .start = axis_destination,
                .direction = 0,
                .starting_positions_boundary = {},
            };
        }

        Optional<CSSPixels> boundary;
        if (strategy.type != SnapSelectionStrategy::Type::EndPosition)
            boundary = axis_boundary.value_or(*axis_start);

        return SnapAxisSelection {
            .destination = axis_destination,
            .start = *axis_start,
            .direction = axis_displacement,
            .starting_positions_boundary = boundary,
        };
    };
    auto axis_of = [](Optional<CSSPixelPoint> const& offset, bool horizontal) -> Optional<CSSPixels> {
        if (!offset.has_value())
            return {};
        return horizontal ? offset->x() : offset->y();
    };
    auto x_selection = axis_selection(destination.x(), strategy.displacement.x(), axis_of(strategy.start_offset, true), axis_of(strategy.starting_positions_boundary, true));
    auto y_selection = axis_selection(destination.y(), strategy.displacement.y(), axis_of(strategy.start_offset, false), axis_of(strategy.starting_positions_boundary, false));

    auto choose_x = [&](Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaIdentity> only_area = {}) {
        return choose_snap_offset_for_axis(candidates.x_candidates, x_selection, snapport.width(), strictness, cross_axis_offset, only_area);
    };
    auto choose_y = [&](Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaIdentity> only_area = {}) {
        return choose_snap_offset_for_axis(candidates.y_candidates, y_selection, snapport.height(), strictness, cross_axis_offset, only_area);
    };

    Optional<SnapAxisChoice> x_choice;
    Optional<SnapAxisChoice> y_choice;
    if (snaps_x && snaps_y) {
        x_choice = choose_x({});
        y_choice = choose_y({});
        if (x_choice.has_value() && y_choice.has_value() && !chosen_offsets_are_mutually_visible(candidates, x_choice->offset, y_choice->offset)) {
            // AD-HOC: A snap area is visible at its own snap positions, so one axis keeps the position it chose while
            //         the other gives up its own and takes the one the same area offers. Of the two areas, the one
            //         leaving the scroll container nearest its destination is followed. This matches other engines.
            Optional<SnapAxisChoice> y_of_x_area;
            Optional<SnapAxisChoice> x_of_y_area;
            if (x_choice->area.is_valid())
                y_of_x_area = choose_y(x_choice->offset, x_choice->area);
            if (y_choice->area.is_valid())
                x_of_y_area = choose_x(y_choice->offset, y_choice->area);

            auto distance_to_destination = [&](SnapAxisChoice const& x_axis_choice, SnapAxisChoice const& y_axis_choice) {
                return AK::hypot((x_axis_choice.offset - x_selection.destination).to_double(), (y_axis_choice.offset - y_selection.destination).to_double());
            };
            bool follows_x_area = y_of_x_area.has_value();
            if (follows_x_area && x_of_y_area.has_value())
                follows_x_area = distance_to_destination(*x_choice, *y_of_x_area) < distance_to_destination(*x_of_y_area, *y_choice);

            if (follows_x_area) {
                y_choice = y_of_x_area;
            } else if (x_of_y_area.has_value()) {
                x_choice = x_of_y_area;
            } else {
                // NB: Neither area offers a snap position in both axes, so the axis whose chosen offset is farther
                //     from its destination is chosen again from the positions visible at the other axis's offset.
                if (abs(x_choice->offset - x_selection.destination) <= abs(y_choice->offset - y_selection.destination)) {
                    y_choice = choose_y(x_choice->offset);
                } else {
                    x_choice = choose_x(y_choice->offset);
                }
            }
        }
        if (x_choice.has_value() && !y_choice.has_value()) {
            x_choice = choose_x(destination.y());
        } else if (y_choice.has_value() && !x_choice.has_value()) {
            y_choice = choose_y(destination.x());
        }
    } else if (snaps_x) {
        x_choice = choose_x(destination.y());
    } else if (snaps_y) {
        y_choice = choose_y(destination.x());
    }

    auto x_offset = x_choice.map([](auto const& choice) { return choice.offset; });
    auto y_offset = y_choice.map([](auto const& choice) { return choice.offset; });

    auto snap_destination = snap_destination_for(destination, x_offset, y_offset, { snaps_x, snaps_y });
    if (x_offset.has_value())
        snap_destination.snapped_areas.x = snap_areas_at_offset(candidates.x_candidates, *x_offset, snap_destination.position.y());
    if (y_offset.has_value())
        snap_destination.snapped_areas.y = snap_areas_at_offset(candidates.y_candidates, *y_offset, snap_destination.position.x());
    return snap_destination;
}

SnapDestination select_snap_destination(SnapContainerGeometry const& geometry, ReadonlySpan<SnapAreaGeometry> areas, CSSPixelPoint destination, SnapSelectionStrategy const& strategy)
{
    auto evaluated_axes = axes_to_evaluate(geometry.axes, strategy);
    if (evaluated_axes.is_empty())
        return { destination };
    auto candidates = build_snap_candidates(geometry, areas, evaluated_axes);
    return select_snap_destination(geometry, candidates, destination, strategy, evaluated_axes);
}

// The largest share of a momentum delta that the delta after it may keep for the momentum to be considered decaying.
static constexpr double maximum_momentum_decay_share = 0.96;

// The share a momentum delta keeps of the one before it is treated as no larger than this, so that momentum which
// barely decays is predicted to travel a hundred times the distance of its latest delta rather than forever.
static constexpr double maximum_slow_momentum_decay_share = 0.99;

// The number of consecutively smaller momentum deltas after which momentum that decays only slowly is predicted from
// anyway.
static constexpr u32 decaying_deltas_before_slow_decay_is_predicted_from = 3;

void MomentumFlingEstimator::reset()
{
    m_previous_momentum_delta = {};
    m_consecutively_decaying_momentum_deltas = 0;
}

Optional<CSSPixelPoint> MomentumFlingEstimator::estimate_remaining_displacement(CSSPixelPoint momentum_delta)
{
    auto previous_momentum_delta = m_previous_momentum_delta;
    m_previous_momentum_delta = momentum_delta;

    // The share the delta keeps of the one before it is what the momentum decays by, so the first delta of a flick
    // says nothing about where it is headed.
    if (!previous_momentum_delta.has_value())
        return {};
    auto distance = AK::hypot(momentum_delta.x().to_double(), momentum_delta.y().to_double());
    auto previous_distance = AK::hypot(previous_momentum_delta->x().to_double(), previous_momentum_delta->y().to_double());
    if (previous_distance <= 0)
        return {};
    auto decay_share = distance / previous_distance;

    if (decay_share < 1) {
        ++m_consecutively_decaying_momentum_deltas;
    } else {
        m_consecutively_decaying_momentum_deltas = 0;
    }

    auto momentum_is_decaying = decay_share < maximum_momentum_decay_share
        || (m_consecutively_decaying_momentum_deltas >= decaying_deltas_before_slow_decay_is_predicted_from && decay_share < 1);
    if (!momentum_is_decaying)
        return {};

    // Each delta keeps the same share of the one before it, so the deltas still to come sum to the delta given
    // divided by the share it loses each time.
    auto remaining_distance_factor = 1 / (1 - min(decay_share, maximum_slow_momentum_decay_share));
    return CSSPixelPoint {
        CSSPixels::nearest_value_for(momentum_delta.x().to_double() * remaining_distance_factor),
        CSSPixels::nearest_value_for(momentum_delta.y().to_double() * remaining_distance_factor),
    };
}

}
