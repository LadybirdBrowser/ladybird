/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::frame_inputs::{ScrollbarOfFrame, thumb_rect_kept_inside_track};
use libcompositing_rust::scrolling::overlay_fade::OverlayFade;
use libcompositing_rust::scrolling::{
    AccumulatedVisualContextTree, AsyncScrollNodeStableID, AsyncScrollTree, AsyncScrollbar, ClipBehavior,
    CompositorContextId, IncludeVisualViewportTransform, OverlayScrollbarVisibility, ScrollStateSnapshot,
    ScrollbarDraggedByCompositor, SpatialNodeIndex,
};
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{FloatPoint, FloatRect, IntRect, Orientation, enclosing_int_rect};

#[derive(Default)]
pub struct ScrollbarController {
    scrollbars: Vec<AsyncScrollbar>,
    // One for each scrollbar, in the same order.
    visibilities: Vec<Visibility>,
    hovered_scrollbar_index: Option<usize>,
    captured_scrollbar_index: Option<usize>,
    thumb_grab_position: f32,
    last_primary_position_of_captured_drag: f32,
    scrollbars_always_shown: bool,
}

#[derive(Default)]
pub struct Drag {
    pub scrollbar_index: usize,
    pub primary_position: f32,
    pub thumb_grab_position: f32,
    // Whether this is the press that began the drag, beside the thumb rather than on it.
    pub is_press_beside_thumb: bool,
}

#[derive(Default)]
pub struct ScrollOffset {
    pub scroll_node_index: SpatialNodeIndex,
    pub scroll_offset: FloatPoint,
}

#[derive(Default)]
struct Visibility {
    fade: OverlayFade,
    // The scroll offset of the scroller as of the last update, which a scroll changes.
    scroll_offset: Option<FloatPoint>,
    is_held_shown: bool,
    fade_out_time: Option<MonotonicTime>,
    // How far the scrollbar has grown from its thin geometry to its expanded one.
    expansion: OverlayFade,
    // Whether the main thread was last told that the scrollbar has faded out.
    reported_hidden: bool,
}

impl Clone for Visibility {
    fn clone(&self) -> Self {
        Self {
            fade: self.fade,
            scroll_offset: self.scroll_offset,
            is_held_shown: self.is_held_shown,
            fade_out_time: self.fade_out_time,
            expansion: self.expansion,
            reported_hidden: self.reported_hidden,
        }
    }
}

fn orientation_for_scrollbar(scrollbar: &AsyncScrollbar) -> Orientation {
    if scrollbar.vertical {
        Orientation::Vertical
    } else {
        Orientation::Horizontal
    }
}

// A scroll node index can be given to another scroller by the next display list, so a scrollbar is recognized by the
// stable id of its scroller wherever both display lists name one.
struct ScrollbarIdentity {
    scroller_stable_node_id: Option<AsyncScrollNodeStableID>,
    scroll_node_index: SpatialNodeIndex,
    vertical: bool,
}

fn scrollbar_identity(scrollbar: &AsyncScrollbar) -> ScrollbarIdentity {
    ScrollbarIdentity {
        scroller_stable_node_id: scrollbar.scroller_stable_node_id,
        scroll_node_index: scrollbar.scroll_node_index,
        vertical: scrollbar.vertical,
    }
}

fn scrollbar_identity_at(scrollbars: &[AsyncScrollbar], scrollbar_index: Option<usize>) -> Option<ScrollbarIdentity> {
    Some(scrollbar_identity(scrollbars.get(scrollbar_index?)?))
}

fn scrollbar_has_identity(scrollbar: &AsyncScrollbar, identity: &ScrollbarIdentity) -> bool {
    if scrollbar.vertical != identity.vertical {
        return false;
    }
    if let (Some(scroller), Some(identity)) = (scrollbar.scroller_stable_node_id, identity.scroller_stable_node_id) {
        return scroller == identity;
    }
    scrollbar.scroll_node_index == identity.scroll_node_index
}

fn find_scrollbar_index(scrollbars: &[AsyncScrollbar], identity: &ScrollbarIdentity) -> Option<usize> {
    scrollbars
        .iter()
        .position(|scrollbar| scrollbar_has_identity(scrollbar, identity))
}

// The rects of a scrollbar the compositor paints are in the viewport's space already.
fn position_in_space_of_scrollbar(
    scrollbar: &AsyncScrollbar,
    visual_context_tree: &AccumulatedVisualContextTree,
    scroll_state_snapshot: &ScrollStateSnapshot,
    position: FloatPoint,
    clip_behavior: ClipBehavior,
) -> Option<FloatPoint> {
    if scrollbar.is_painted_by_compositor {
        return Some(position);
    }
    if !visual_context_tree.context_is_valid(scrollbar.context) {
        return None;
    }
    visual_context_tree.transform_point_for_hit_test(scrollbar.context, position, scroll_state_snapshot, clip_behavior)
}

fn scrollbar_gutter_rect(scrollbar: &AsyncScrollbar, expanded: bool) -> IntRect {
    if expanded {
        scrollbar.expanded_gutter_rect
    } else {
        scrollbar.gutter_rect
    }
}

fn scrollbar_scroll_size(scrollbar: &AsyncScrollbar, expanded: bool) -> f64 {
    if expanded {
        scrollbar.expanded_scroll_size
    } else {
        scrollbar.scroll_size
    }
}

fn translated_thumb_rect(scrollbar: &AsyncScrollbar, scroll_offset: FloatPoint, expanded: bool) -> IntRect {
    let orientation = orientation_for_scrollbar(scrollbar);
    let mut thumb_rect = if expanded {
        scrollbar.expanded_thumb_rect
    } else {
        scrollbar.thumb_rect
    };
    thumb_rect.translate_primary_offset_for_orientation(
        orientation,
        (scroll_offset.primary_offset_for_orientation(orientation) as f64 * scrollbar_scroll_size(scrollbar, expanded))
            as i32,
    );
    thumb_rect
}

fn translated_thumb_rect_with_snapshot(
    scrollbar: &AsyncScrollbar,
    scroll_state_snapshot: &ScrollStateSnapshot,
    expanded: bool,
) -> IntRect {
    let mut thumb_rect = if expanded {
        scrollbar.expanded_thumb_rect
    } else {
        scrollbar.thumb_rect
    };
    let scroll_size = scrollbar_scroll_size(scrollbar, expanded);
    let device_offset = scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_node_index);
    if scrollbar.vertical {
        thumb_rect.y += ((-device_offset.y) as f64 * scroll_size) as i32;
    } else {
        thumb_rect.x += ((-device_offset.x) as f64 * scroll_size) as i32;
    }
    thumb_rect
}

fn scrollbar_hit_rect(scrollbar: &AsyncScrollbar, scroll_offset: FloatPoint) -> IntRect {
    const SCROLLBAR_HIT_SLOP: i32 = 4;
    let mut rect = translated_thumb_rect(scrollbar, scroll_offset, false).united(translated_thumb_rect(
        scrollbar,
        scroll_offset,
        true,
    ));
    let expanded_gutter_rect = scrollbar_gutter_rect(scrollbar, true);
    if !expanded_gutter_rect.is_empty() {
        rect = rect.united(expanded_gutter_rect);
    }
    rect.inflated(SCROLLBAR_HIT_SLOP, SCROLLBAR_HIT_SLOP)
}

impl ScrollbarController {
    // Overlay scrollbars are hidden until their scroller scrolls. A scrollbar stays shown for this long after its
    // scroller last scrolled or the pointer left it, and then fades out.
    pub const SHOWN_DURATION_AFTER_ACTIVITY: Duration = Duration::from_milliseconds(1000);

    // Tests see every scrollbar at all times, as when it is shown depends on the time.
    pub fn new(scrollbars_always_shown: bool) -> Self {
        Self {
            scrollbars_always_shown,
            ..Default::default()
        }
    }

    pub fn clear(&mut self) {
        self.scrollbars.clear();
        self.visibilities.clear();
        self.hovered_scrollbar_index = None;
        self.captured_scrollbar_index = None;
        self.thumb_grab_position = 0.0;
        self.last_primary_position_of_captured_drag = 0.0;
    }

    pub fn set_scrollbars(&mut self, scrollbars: &[AsyncScrollbar]) {
        let hovered_scrollbar_identity = scrollbar_identity_at(&self.scrollbars, self.hovered_scrollbar_index);
        let captured_scrollbar_identity = scrollbar_identity_at(&self.scrollbars, self.captured_scrollbar_index);

        let mut visibilities = Vec::with_capacity(scrollbars.len());
        for scrollbar in scrollbars {
            let previous_index = find_scrollbar_index(&self.scrollbars, &scrollbar_identity(scrollbar));
            visibilities
                .push(previous_index.map_or_else(Visibility::default, |index| self.visibilities[index].clone()));
        }

        self.scrollbars = scrollbars.to_vec();
        self.visibilities = visibilities;
        self.hovered_scrollbar_index =
            hovered_scrollbar_identity.and_then(|identity| find_scrollbar_index(&self.scrollbars, &identity));
        self.captured_scrollbar_index =
            captured_scrollbar_identity.and_then(|identity| find_scrollbar_index(&self.scrollbars, &identity));
        if self.captured_scrollbar_index.is_none() {
            self.thumb_grab_position = 0.0;
        }
    }

    pub fn is_empty(&self) -> bool {
        self.scrollbars.is_empty()
    }
    pub fn scrollbars(&self) -> &[AsyncScrollbar] {
        &self.scrollbars
    }
    pub fn has_captured_scrollbar(&self) -> bool {
        self.captured_scrollbar_index.is_some()
    }
    pub fn captured_scrollbar_index(&self) -> Option<usize> {
        self.captured_scrollbar_index
    }
    // Whether the scrollbar is shown, rather than hidden until its scroller next scrolls.
    pub fn is_shown(&self, scrollbar_index: usize) -> bool {
        self.opacity(scrollbar_index) > 0.0
    }

    // The main thread still takes the mouse events of a drag that holds a scrollbar the display list paints. The
    // context is the one this controller belongs to.
    pub fn captured_scrollbar_painted_by_display_list(
        &self,
        context_id: CompositorContextId,
    ) -> Option<ScrollbarDraggedByCompositor> {
        let scrollbar = self.scrollbars.get(self.captured_scrollbar_index?)?;
        if scrollbar.is_painted_by_compositor || scrollbar.scroller_stable_node_id.is_none() {
            return None;
        }
        Some(ScrollbarDraggedByCompositor {
            context_id,
            scroller_stable_node_id: scrollbar.scroller_stable_node_id?,
        })
    }

    pub fn hit_test_scrollbar_painted_by_compositor(
        &self,
        async_scroll_tree: &AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        position: FloatPoint,
    ) -> Option<usize> {
        for (i, scrollbar) in self.scrollbars.iter().enumerate() {
            if !scrollbar.is_painted_by_compositor {
                continue;
            }
            let Some(scroll_offset) =
                async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_index, scroll_state_snapshot)
            else {
                continue;
            };
            if scrollbar_hit_rect(scrollbar, scroll_offset)
                .to_float()
                .contains_point(position)
            {
                return Some(i);
            }
        }
        None
    }

    // A scrollbar the display list paints takes the pointer inside the rect of the scrollbar as it is presented now, and only
    // with nothing that takes pointer input painted over it there. A scrollbar that has faded out takes none, as the main
    // thread passes it by.
    // A scrollbar the display list paints takes the pointer where it is presented, unless something that takes pointer
    // input is painted over it there.
    pub fn hit_test_scrollbar_painted_by_display_list(
        &self,
        async_scroll_tree: &AsyncScrollTree,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        position: FloatPoint,
    ) -> Option<usize> {
        for (i, scrollbar) in self.scrollbars.iter().enumerate().rev() {
            if scrollbar.is_painted_by_compositor || scrollbar.scroller_stable_node_id.is_none() || !self.is_shown(i) {
                continue;
            }
            if async_scroll_tree
                .scroll_offset_for_node(scrollbar.scroll_node_index, scroll_state_snapshot)
                .is_none()
            {
                continue;
            }

            let Some(position_in_scrollbar_space) = position_in_space_of_scrollbar(
                scrollbar,
                visual_context_tree,
                scroll_state_snapshot,
                position,
                ClipBehavior::Respect,
            ) else {
                continue;
            };
            let presented_scrollbar_rect = if self.expansion(i) > 0.0 {
                scrollbar.expanded_gutter_rect
            } else {
                scrollbar.track_rect
            };
            if !presented_scrollbar_rect
                .to_float()
                .contains_point(position_in_scrollbar_space)
            {
                continue;
            }

            if async_scroll_tree.is_covered_by_hit_test_target_painted_after(
                scrollbar.paint_order_index,
                visual_context_tree,
                position,
            ) {
                return None;
            }
            return Some(i);
        }
        None
    }

    pub fn begin_drag(
        &mut self,
        async_scroll_tree: &AsyncScrollTree,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        position: FloatPoint,
    ) -> Option<Drag> {
        let mut scrollbar_index =
            self.hit_test_scrollbar_painted_by_compositor(async_scroll_tree, scroll_state_snapshot, position);
        if scrollbar_index.is_none() {
            scrollbar_index = self.hit_test_scrollbar_painted_by_display_list(
                async_scroll_tree,
                visual_context_tree,
                scroll_state_snapshot,
                position,
            );
        }
        // INTEROP: A scrollbar that has faded out takes no press, as in Chromium.
        let scrollbar_index = scrollbar_index?;
        if self.opacity(scrollbar_index) <= 0.0 {
            return None;
        }

        let scrollbar = &self.scrollbars[scrollbar_index];
        let scroll_offset =
            async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_index, scroll_state_snapshot);
        assert!(scroll_offset.is_some());
        let position_in_scrollbar_space = position_in_space_of_scrollbar(
            scrollbar,
            visual_context_tree,
            scroll_state_snapshot,
            position,
            ClipBehavior::Respect,
        );
        assert!(position_in_scrollbar_space.is_some());

        let orientation = orientation_for_scrollbar(scrollbar);
        // A successful press captures and expands the scrollbar before its drag delta is applied.
        let expanded = true;
        let thumb_rect = translated_thumb_rect(scrollbar, scroll_offset.unwrap(), expanded);
        let thumb_hit_rect = thumb_rect.to_float();

        let primary_position = position_in_scrollbar_space
            .unwrap()
            .primary_offset_for_orientation(orientation);
        let position_is_along_thumb = if orientation == Orientation::Vertical {
            thumb_hit_rect.contains_vertically(primary_position)
        } else {
            thumb_hit_rect.contains_horizontally(primary_position)
        };
        let thumb_grab_position = if position_is_along_thumb {
            primary_position - thumb_rect.primary_offset_for_orientation(orientation) as f32
        } else {
            let gutter_rect = scrollbar_gutter_rect(scrollbar, true);
            let thumb_size = thumb_rect.primary_size_for_orientation(orientation) as f32;
            let gutter_start = gutter_rect.primary_offset_for_orientation(orientation) as f32;
            let gutter_size = gutter_rect.primary_size_for_orientation(orientation) as f32;
            let offset_relative_to_gutter = primary_position - gutter_start;
            max(
                min(offset_relative_to_gutter, thumb_size / 2.0),
                offset_relative_to_gutter - gutter_size + thumb_size,
            )
        };

        self.captured_scrollbar_index = Some(scrollbar_index);
        self.hovered_scrollbar_index = Some(scrollbar_index);
        self.thumb_grab_position = thumb_grab_position;
        self.last_primary_position_of_captured_drag = primary_position;
        Some(Drag {
            scrollbar_index,
            primary_position,
            thumb_grab_position,
            is_press_beside_thumb: !position_is_along_thumb,
        })
    }

    // A drag usually leaves the clip of its scrollbar, and the spaces above the scrollbar may move while it lasts, so the
    // position is mapped anew for every event without regard to clips. A position that cannot be mapped moves nothing.
    fn primary_position_of_captured_drag(
        &mut self,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        position: FloatPoint,
    ) -> f32 {
        if let Some(scrollbar) = self
            .captured_scrollbar_index
            .and_then(|index| self.scrollbars.get(index))
            && let Some(position_in_scrollbar_space) = position_in_space_of_scrollbar(
                scrollbar,
                visual_context_tree,
                scroll_state_snapshot,
                position,
                ClipBehavior::Ignore,
            )
        {
            self.last_primary_position_of_captured_drag =
                position_in_scrollbar_space.primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
        }
        self.last_primary_position_of_captured_drag
    }

    pub fn captured_drag(
        &mut self,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        position: FloatPoint,
    ) -> Option<Drag> {
        let scrollbar_index = self.captured_scrollbar_index?;
        let primary_position =
            self.primary_position_of_captured_drag(visual_context_tree, scroll_state_snapshot, position);
        Some(Drag {
            scrollbar_index,
            primary_position,
            thumb_grab_position: self.thumb_grab_position,
            ..Default::default()
        })
    }

    pub fn release_captured_drag(
        &mut self,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        position: FloatPoint,
    ) -> Option<Drag> {
        let scrollbar_index = self.captured_scrollbar_index?;
        let thumb_grab_position = self.thumb_grab_position;
        let primary_position =
            self.primary_position_of_captured_drag(visual_context_tree, scroll_state_snapshot, position);
        self.captured_scrollbar_index = None;
        self.thumb_grab_position = 0.0;
        Some(Drag {
            scrollbar_index,
            primary_position,
            thumb_grab_position,
            ..Default::default()
        })
    }

    pub fn set_hovered_scrollbar(&mut self, scrollbar_index: Option<usize>) -> bool {
        if self.hovered_scrollbar_index == scrollbar_index {
            return false;
        }
        self.hovered_scrollbar_index = scrollbar_index;
        true
    }

    pub fn scroll_offset_for_drag(
        &self,
        async_scroll_tree: &AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        drag: &Drag,
    ) -> Option<ScrollOffset> {
        let scrollbar = self.scrollbars.get(drag.scrollbar_index)?;
        let expanded = self.is_expanded(drag.scrollbar_index);
        let scroll_size = scrollbar_scroll_size(scrollbar, expanded);
        if scroll_size == 0.0 {
            return None;
        }

        let current_scroll_offset =
            async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_index, scroll_state_snapshot)?;
        let orientation = orientation_for_scrollbar(scrollbar);
        let thumb_rect = if expanded {
            scrollbar.expanded_thumb_rect
        } else {
            scrollbar.thumb_rect
        };
        let zero_offset_thumb_position = thumb_rect.primary_offset_for_orientation(orientation) as f32;
        let min_thumb_position = zero_offset_thumb_position + scrollbar.min_scroll_offset * scroll_size as f32;
        let max_thumb_position = zero_offset_thumb_position + scrollbar.max_scroll_offset * scroll_size as f32;
        if min_thumb_position.is_nan() || max_thumb_position.is_nan() || max_thumb_position < min_thumb_position {
            return None;
        }
        let target_thumb_position = clamp(
            drag.primary_position - drag.thumb_grab_position,
            min_thumb_position,
            max_thumb_position,
        );
        let mut target_scroll_offset = (target_thumb_position - zero_offset_thumb_position) / scroll_size as f32;
        // A thumb at either end of its track scrolls exactly to that end, whatever rounding the division above suffers.
        if target_thumb_position == min_thumb_position {
            target_scroll_offset = scrollbar.min_scroll_offset;
        } else if target_thumb_position == max_thumb_position {
            target_scroll_offset = scrollbar.max_scroll_offset;
        }

        let mut scroll_offset = current_scroll_offset;
        scroll_offset.set_primary_offset_for_orientation(orientation, target_scroll_offset);
        Some(ScrollOffset {
            scroll_node_index: scrollbar.scroll_node_index,
            scroll_offset,
        })
    }

    // Which way a press beside the thumb pages: -1 or 1 toward the pointer from where the thumb is at the given scroll
    // offset, or 0 when the thumb is under the pointer.
    pub fn paging_direction(&self, scrollbar_index: usize, primary_position: f32, scroll_offset: FloatPoint) -> i32 {
        let Some(scrollbar) = self.scrollbars.get(scrollbar_index) else {
            return 0;
        };
        let orientation = orientation_for_scrollbar(scrollbar);
        let thumb_rect = translated_thumb_rect(scrollbar, scroll_offset, self.is_expanded(scrollbar_index));
        if primary_position < thumb_rect.primary_offset_for_orientation(orientation) as f32 {
            return -1;
        }
        if primary_position
            >= (thumb_rect.primary_offset_for_orientation(orientation)
                + thumb_rect.primary_size_for_orientation(orientation)) as f32
        {
            return 1;
        }
        0
    }

    // Shows each scrollbar whose scroller scrolled since the last call, keeps a shown scrollbar that is hovered or
    // dragged shown, and starts the wait before a scrollbar that is no longer hovered or dragged fades out. A hovered or
    // dragged scrollbar that is shown grows to its expanded geometry, and keeps it until it has faded out.
    pub fn update_visibility(&mut self, scroll_state_snapshot: &ScrollStateSnapshot, now: MonotonicTime) {
        for i in 0..self.scrollbars.len() {
            let expanded = self.is_expanded(i);
            let visibility = &mut self.visibilities[i];
            let scrollbar = &self.scrollbars[i];
            let scroll_offset = scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_node_index);
            let scrolled = visibility.scroll_offset.is_some_and(|offset| offset != scroll_offset);
            visibility.scroll_offset = Some(scroll_offset);

            let was_held_shown = visibility.is_held_shown;
            // Hovering a hidden scrollbar does not show it. Only a scroll does.
            visibility.is_held_shown = expanded && (scrolled || visibility.fade.opacity() > 0.0);
            if visibility.is_held_shown {
                visibility.fade.jump_to(1.0);
                visibility.fade_out_time = None;
                visibility.expansion.fade_to(1.0, now);
                continue;
            }
            if scrolled {
                visibility.fade.jump_to(1.0);
            }
            if scrolled || was_held_shown {
                visibility.fade_out_time = Some(now + Self::SHOWN_DURATION_AFTER_ACTIVITY);
            }
        }
    }

    // When the next scrollbar starts to fade out.
    pub fn next_fade_out_time(&self) -> Option<MonotonicTime> {
        let mut next_fade_out_time = None;
        for visibility in &self.visibilities {
            if let Some(time) = visibility.fade_out_time
                && next_fade_out_time.is_none_or(|next| time < next)
            {
                next_fade_out_time = Some(time);
            }
        }
        next_fade_out_time
    }

    // The scrollbars that faded out or were shown again since the last call. The main thread hit tests the scrollbars
    // too, and passes by the ones that have faded out.
    pub fn take_visibility_changes(&mut self) -> Vec<OverlayScrollbarVisibility> {
        let mut changes = Vec::new();
        for i in 0..self.scrollbars.len() {
            let hidden = self.opacity(i) <= 0.0;
            let scrollbar = &self.scrollbars[i];
            let visibility = &mut self.visibilities[i];
            let Some(scroller_stable_node_id) = scrollbar.scroller_stable_node_id else {
                continue;
            };
            if hidden == visibility.reported_hidden {
                continue;
            }
            visibility.reported_hidden = hidden;
            changes.push(OverlayScrollbarVisibility {
                scroller_stable_node_id,
                vertical: scrollbar.vertical,
                hidden,
            });
        }
        changes
    }

    pub fn fades_need_frame(&self, now: MonotonicTime) -> bool {
        for visibility in &self.visibilities {
            if visibility.fade.is_fading()
                || visibility.expansion.is_fading()
                || visibility.fade_out_time.is_some_and(|time| time <= now)
            {
                return true;
            }
        }
        false
    }

    // Returns the rect in the viewport of the scrollbars whose opacity or geometry changed.
    pub fn advance_fades(
        &mut self,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        frame_time: MonotonicTime,
    ) -> IntRect {
        let mut damage_rect = IntRect::default();
        for i in 0..self.scrollbars.len() {
            let visibility = &mut self.visibilities[i];
            if visibility.fade_out_time.is_some_and(|time| time <= frame_time) {
                visibility.fade_out_time = None;
                visibility.fade.fade_to(0.0, frame_time);
            }
            let mut changed = visibility.fade.advance_to(frame_time);
            // A scrollbar that has faded out is thin again when it is next shown.
            if visibility.fade.opacity() <= 0.0 {
                visibility.expansion.jump_to(0.0);
            }
            changed |= visibility.expansion.advance_to(frame_time);
            if !changed {
                continue;
            }
            if let Some(rect) = self.rect_in_viewport(i, visual_context_tree, scroll_state_snapshot) {
                damage_rect = damage_rect.united(rect);
            }
        }
        damage_rect
    }

    fn rect_in_viewport(
        &self,
        scrollbar_index: usize,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> Option<IntRect> {
        let scrollbar = self.scrollbars.get(scrollbar_index)?;
        let rect = scrollbar
            .gutter_rect
            .united(scrollbar.track_rect)
            .united(scrollbar.expanded_gutter_rect);
        if scrollbar.is_painted_by_compositor {
            return Some(rect.inflated(2, 2));
        }
        if !visual_context_tree.context_is_valid(scrollbar.context) {
            return None;
        }
        let rect_in_viewport = visual_context_tree.transform_rect_to_viewport(
            scrollbar.context.spatial,
            rect.to_float(),
            scroll_state_snapshot,
            IncludeVisualViewportTransform::Yes,
        );
        if !rect_in_viewport.x.is_finite()
            || !rect_in_viewport.y.is_finite()
            || !rect_in_viewport.width.is_finite()
            || !rect_in_viewport.height.is_finite()
        {
            return None;
        }
        Some(enclosing_int_rect(rect_in_viewport).inflated(2, 2))
    }
}

fn interpolated_rect(from: IntRect, to: IntRect, progress: f32) -> FloatRect {
    let interpolate = |from: i32, to: i32| from as f32 + (to - from) as f32 * progress;
    FloatRect::new(
        interpolate(from.x, to.x),
        interpolate(from.y, to.y),
        interpolate(from.width, to.width),
        interpolate(from.height, to.height),
    )
}

impl ScrollbarController {
    pub fn scrollbars_of_frame(&self, scroll_state_snapshot: &ScrollStateSnapshot) -> Vec<ScrollbarOfFrame> {
        let mut scrollbars_of_frame = Vec::with_capacity(self.scrollbars.len());
        for (i, scrollbar) in self.scrollbars.iter().enumerate() {
            let progress = self.expansion(i);
            let mut thin_thumb_rect = thumb_rect_kept_inside_track(
                translated_thumb_rect_with_snapshot(scrollbar, scroll_state_snapshot, false),
                scrollbar.track_rect,
                scrollbar.vertical,
            );
            let mut expanded_thumb_rect = thumb_rect_kept_inside_track(
                translated_thumb_rect_with_snapshot(scrollbar, scroll_state_snapshot, true),
                scrollbar.expanded_gutter_rect,
                scrollbar.vertical,
            );
            // An empty thumb has nothing to grow from or to.
            if thin_thumb_rect.is_empty() {
                thin_thumb_rect = expanded_thumb_rect;
            }
            if expanded_thumb_rect.is_empty() {
                expanded_thumb_rect = thin_thumb_rect;
            }

            scrollbars_of_frame.push(ScrollbarOfFrame {
                scroll_node_index: scrollbar.scroll_node_index,
                vertical: scrollbar.vertical,
                is_painted_by_compositor: scrollbar.is_painted_by_compositor,
                gutter_rect: interpolated_rect(scrollbar.track_rect, scrollbar.expanded_gutter_rect, progress),
                thumb_rect: interpolated_rect(thin_thumb_rect, expanded_thumb_rect, progress),
                expansion: progress,
                opacity: self.opacity(i),
                thumb_color: scrollbar.thumb_color,
                thumb_outline_color: scrollbar.thumb_outline_color,
                track_color: scrollbar.track_color,
                thin_and_expanded_gutter_rect: scrollbar.gutter_rect.united(scrollbar.expanded_gutter_rect),
            });
        }
        scrollbars_of_frame
    }

    fn is_expanded(&self, scrollbar_index: usize) -> bool {
        self.hovered_scrollbar_index == Some(scrollbar_index) || self.captured_scrollbar_index == Some(scrollbar_index)
    }

    fn opacity(&self, scrollbar_index: usize) -> f32 {
        if self.scrollbars_always_shown {
            return 1.0;
        }
        self.visibilities
            .get(scrollbar_index)
            .map_or(0.0, |visibility| visibility.fade.opacity())
    }

    fn expansion(&self, scrollbar_index: usize) -> f32 {
        if self.scrollbars_always_shown {
            return if self.is_expanded(scrollbar_index) { 1.0 } else { 0.0 };
        }
        self.visibilities
            .get(scrollbar_index)
            .map_or(0.0, |visibility| visibility.expansion.opacity())
    }
}

fn min(a: f32, b: f32) -> f32 {
    if b < a { b } else { a }
}
fn max(a: f32, b: f32) -> f32 {
    if a < b { b } else { a }
}
fn clamp(value: f32, min: f32, max: f32) -> f32 {
    assert!(max >= min);
    if value > max {
        max
    } else if value < min {
        min
    } else {
        value
    }
}
