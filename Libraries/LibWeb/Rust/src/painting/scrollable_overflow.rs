/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{flex_direction, flex_wrap, writing_mode};
use crate::css::css_pixels::CssPixelPoint;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;

pub(crate) struct PhysicalOverflowDirections {
    pub(crate) horizontal_axis_is_positive: bool,
    pub(crate) vertical_axis_is_positive: bool,
}

impl Default for PhysicalOverflowDirections {
    fn default() -> Self {
        Self {
            horizontal_axis_is_positive: true,
            vertical_axis_is_positive: true,
        }
    }
}

struct AxisDirection {
    is_horizontal: bool,
    is_reverse: bool,
}

// https://drafts.csswg.org/cssom-view/#overflow-directions
pub(crate) fn physical_overflow_directions(
    layout_arena: &LayoutNodeArena,
    node: NodeSlotId,
) -> PhysicalOverflowDirections {
    layout_arena.node_style_if_live(node).map_or_else(
        PhysicalOverflowDirections::default,
        physical_overflow_directions_of_style,
    )
}

pub(crate) fn physical_overflow_directions_of_style(
    style: crate::css::computed_value_views::ComputedValuesView<'_>,
) -> PhysicalOverflowDirections {
    // A scrolling box of a viewport or element has two overflow directions, which are the block-end and inline-end
    // directions for that viewport or element.
    let mut inline_axis = AxisDirection {
        is_horizontal: style.writing_mode() == writing_mode::HORIZONTAL_TB,
        is_reverse: style.inline_axis_is_reverse(),
    };
    let mut block_axis = AxisDirection {
        is_horizontal: !inline_axis.is_horizontal,
        is_reverse: style.block_axis_is_reverse(),
    };

    let display = style.display();
    if display.is_flex_inside() {
        let container_flex_direction = style.effective_flex_direction();
        let is_row_layout = matches!(
            container_flex_direction,
            flex_direction::ROW | flex_direction::ROW_REVERSE
        );
        let (main_axis, cross_axis) = if is_row_layout {
            (&mut inline_axis, &mut block_axis)
        } else {
            (&mut block_axis, &mut inline_axis)
        };

        if matches!(
            container_flex_direction,
            flex_direction::ROW_REVERSE | flex_direction::COLUMN_REVERSE
        ) {
            main_axis.is_reverse = !main_axis.is_reverse;
        }

        // AD-HOC: A legacy webkit box ignores `flex-wrap`, matching other engines.
        if !display.is_webkit_box_inside() && style.flex_wrap() == flex_wrap::WRAP_REVERSE {
            cross_axis.is_reverse = !cross_axis.is_reverse;
        }
    }

    let (horizontal_axis, vertical_axis) = if inline_axis.is_horizontal {
        (inline_axis, block_axis)
    } else {
        (block_axis, inline_axis)
    };
    PhysicalOverflowDirections {
        horizontal_axis_is_positive: !horizontal_axis.is_reverse,
        vertical_axis_is_positive: !vertical_axis.is_reverse,
    }
}

#[derive(Clone, Default)]
pub(crate) struct ScrollableOverflowState {
    pub(crate) recalculation_requests: Vec<NodeSlotId>,
    pub(crate) scroll_offset_clamp_candidates: Vec<NodeSlotId>,
    pub(crate) geometry_changed: bool,
    pub(crate) scrollability_changed: bool,
    pub(crate) recalculations: u64,
}

impl ScrollableOverflowState {
    pub(crate) fn change_awaits_preparation(&self) -> bool {
        self.geometry_changed || self.scrollability_changed
    }
}

impl LayoutNodeArena {
    pub(crate) fn note_scrollable_overflow_change(
        &mut self,
        node: NodeSlotId,
        had_scrollable_overflow: bool,
        has_scrollable_overflow: bool,
        changed_outside_layout: bool,
    ) {
        use crate::painting::record::damage::PaintDamage;
        if changed_outside_layout {
            self.scrollable_overflow.geometry_changed = true;
        }
        self.push_paint_damage(
            node,
            PaintDamage::SCROLL_METADATA | PaintDamage::DRAW_OVERLAY | PaintDamage::HIT_OVERLAY,
        );
        // The root background covers the viewport united with the root's overflow.
        if self.node_kind_if_live(node) == Some(NodeKind::Viewport)
            && let Some(source) = self.paint_state().root_background_source
        {
            self.push_paint_damage(source.root_layout_node, PaintDamage::DRAW_BACKGROUND);
        }
        if had_scrollable_overflow != has_scrollable_overflow {
            self.scrollable_overflow.scrollability_changed = true;
            self.push_paint_damage(node, PaintDamage::ALL_PRODUCERS);
            self.paintable_rows_mut()
                .push_paint_damage_to_paint_subtree(node, PaintDamage::SCROLL_METADATA);
            self.note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::ScrollableOverflowFlipped);
        }
    }

    pub(crate) fn note_scroll_offset_may_leave_range(&mut self, node: NodeSlotId) {
        if box_holds_scroll_state(self, node) {
            self.scrollable_overflow.scroll_offset_clamp_candidates.push(node);
        }
    }

    pub(crate) fn scrollable_overflow_is_settled(&self) -> bool {
        self.scrollable_overflow.recalculation_requests.is_empty()
    }

    pub(crate) fn settle_scrollable_overflow(&mut self) {
        self.recalculate_requested_fragment_overflow();
    }
}

/// Whether a box is a scroll container, or a box whose element or pseudo-element stores a scroll offset that new
/// overflow may have to clamp. The arena sets `NodeFlag::HasScrollOffset` whenever the offset a box holds changes and
/// whenever a box becomes an element's or pseudo-element's box, so the answer is one style query and one flag read.
fn box_holds_scroll_state(arena: &LayoutNodeArena, slot: NodeSlotId) -> bool {
    crate::painting::style_queries::is_scroll_container(arena, slot)
        || arena.node_flags_if_live(slot) & NodeFlag::HasScrollOffset as u32 != 0
}

/// Settles the requested overflow recalculation. Answers the scroll offsets new overflow moved out of range, each
/// clamped into it, for the document to store.
#[must_use = "the document stores the clamped scroll offsets"]
pub(crate) fn update_scrollable_overflow(arena: &mut LayoutNodeArena) -> Vec<(NodeSlotId, CssPixelPoint)> {
    arena.settle_scrollable_overflow();
    let mut clamped = Vec::new();
    let candidates = std::mem::take(&mut arena.scrollable_overflow.scroll_offset_clamp_candidates);
    let mut met = crate::fast_hash::FastSet::default();
    for slot in candidates {
        if !met.insert(slot) || !arena.slot_is_live(slot) || !arena.paintable_row_is_populated(slot) {
            continue;
        }
        let offset = arena.row_scroll_offset(slot);
        if offset == CssPixelPoint::default() {
            continue;
        }
        let Some((minimum, maximum)) =
            crate::painting::chrome_geometry::scroll_offset_bounds(&arena.paintable_rows(), slot)
        else {
            continue;
        };
        let offset_in_range = CssPixelPoint::new(
            offset.x.clamp(minimum.x, maximum.x),
            offset.y.clamp(minimum.y, maximum.y),
        );
        if offset_in_range != offset {
            clamped.push((slot, offset_in_range));
        }
    }
    clamped
}

/// Retain the last published transform group, overflow values and overflow directions so a style change can invalidate overflow
/// even after the DOM or an animation has released its previous style record. The snapshot owns
/// its reference to the group, as a style record does, and shares the immutable payload.
pub(crate) struct OverflowStyle {
    transform: crate::css::host_shared::HostShared<crate::css::computed_value_types::TransformValues>,
    overflow: (u8, u8),
    overflow_directions_are_positive: (bool, bool),
}

fn overflow_directions_are_positive(style: crate::css::computed_value_views::ComputedValuesView<'_>) -> (bool, bool) {
    let directions = physical_overflow_directions_of_style(style);
    (
        directions.horizontal_axis_is_positive,
        directions.vertical_axis_is_positive,
    )
}

impl OverflowStyle {
    pub(crate) fn new(style: crate::css::computed_value_views::ComputedValuesView<'_>) -> Self {
        let values = std::ptr::from_ref(style.transform());
        crate::css::computed_values::retain_group_payload(
            crate::css::computed_value_types::STYLE_GROUP_INDEX_TRANSFORM,
            values.cast(),
        );
        Self {
            transform: crate::css::host_shared::HostShared::new(values),
            overflow: (style.overflow_x(), style.overflow_y()),
            overflow_directions_are_positive: overflow_directions_are_positive(style),
        }
    }

    fn transform_matches(&self, style: crate::css::computed_value_views::ComputedValuesView<'_>) -> bool {
        let new = style.transform();
        if std::ptr::eq(self.transform.as_ptr().cast(), new) {
            return true;
        }
        // SAFETY: This snapshot retains the immutable group until it is replaced or dropped.
        let old = unsafe { self.transform.deref() };
        old.transformations == new.transformations
            && old.translate == new.translate
            && old.rotate == new.rotate
            && old.scale == new.scale
            && old.transform_box == new.transform_box
            && old.transform_origin_x == new.transform_origin_x
            && old.transform_origin_y == new.transform_origin_y
            && old.transform_origin_z == new.transform_origin_z
            && old.has_offset_path == new.has_offset_path
            && old.offset_path == new.offset_path
            && old.offset_distance == new.offset_distance
            && old.offset_position == new.offset_position
            && old.offset_anchor == new.offset_anchor
            && old.offset_rotate == new.offset_rotate
    }

    fn overflow_matches(&self, style: crate::css::computed_value_views::ComputedValuesView<'_>) -> bool {
        self.overflow == (style.overflow_x(), style.overflow_y())
            && self.overflow_directions_are_positive == overflow_directions_are_positive(style)
    }
}

/// A fork's snapshot holds its own reference to the group.
impl Clone for OverflowStyle {
    fn clone(&self) -> Self {
        crate::css::computed_values::retain_group_payload(
            crate::css::computed_value_types::STYLE_GROUP_INDEX_TRANSFORM,
            self.transform.as_ptr().cast(),
        );
        Self {
            transform: self.transform,
            overflow: self.overflow,
            overflow_directions_are_positive: self.overflow_directions_are_positive,
        }
    }
}

impl Drop for OverflowStyle {
    fn drop(&mut self) {
        crate::css::computed_values::release_group_payload(
            crate::css::computed_value_types::STYLE_GROUP_INDEX_TRANSFORM,
            self.transform.as_ptr().cast(),
        );
    }
}

impl LayoutNodeArena {
    pub(crate) fn invalidate_overflow_after_style_change(&mut self, slot: NodeSlotId) {
        if !self.paintable_row_is_populated(slot) {
            return;
        }
        let Some(style) = self.node_style_if_live(slot) else {
            return;
        };
        let (transform_changed, overflow_changed) = self
            .paintable_side_data(slot)
            .overflow_style
            .as_ref()
            .map_or((false, false), |previous| {
                (!previous.transform_matches(style), !previous.overflow_matches(style))
            });
        let overflow_style = OverflowStyle::new(style);
        self.paintable_side_data_mut(slot).overflow_style = Some(overflow_style);
        if !transform_changed && !overflow_changed {
            return;
        }
        if self.node_kind_if_live(slot).is_some_and(node_facts::kind_is_svg_box) {
            // SVG consumes transforms during layout, unlike CSS box overflow measurement.
            if transform_changed {
                self.set_needs_layout_update(slot, true);
            }
            return;
        }
        // NB: No cached layout of the box or its ancestors may hold the overflow the style change made stale.
        self.invalidate_fragments_of_self_and_ancestors_for_change_that_sizes_no_box(slot);
        if overflow_changed {
            self.request_fragment_overflow_recalculation(slot);
        }
        self.request_fragment_overflow_recalculation_of_containing_block(slot);
    }
}
