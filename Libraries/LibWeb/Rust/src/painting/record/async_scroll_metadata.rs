/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::*;
use crate::painting::paintable_data::*;
use crate::painting::paintable_geometry;
use crate::painting::record::PaintRecorder;
use crate::painting::style_queries;
use crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
use libgfx_rust::{FloatPoint, FloatRect, FloatSize, IntRect};

fn css_point_to_device_point(point: CssPixelPoint, device_pixels_per_css_pixel: f64) -> FloatPoint {
    let scale = device_pixels_per_css_pixel as f32;
    FloatPoint {
        x: point.x.to_float() * scale,
        y: point.y.to_float() * scale,
    }
}

fn css_size_to_device_size(size: CssPixelSize, device_pixels_per_css_pixel: f64) -> FloatSize {
    let scale = device_pixels_per_css_pixel as f32;
    FloatSize {
        width: size.width.to_float() * scale,
        height: size.height.to_float() * scale,
    }
}

fn css_rect_to_device_rect(rect: CssPixelRect, device_pixels_per_css_pixel: f64) -> FloatRect {
    let location = css_point_to_device_point(rect.location(), device_pixels_per_css_pixel);
    let size = css_size_to_device_size(rect.size(), device_pixels_per_css_pixel);
    FloatRect {
        x: location.x,
        y: location.y,
        width: size.width,
        height: size.height,
    }
}

fn css_inset_to_device_inset(inset: Option<CssPixels>, device_pixels_per_css_pixel: f64) -> OptionalF32 {
    OptionalF32::from(inset.map(|inset| inset.to_float() * device_pixels_per_css_pixel as f32))
}

impl PaintRecorder<'_> {
    fn could_be_scrolled_by_wheel_event(&mut self, paintable: NodeSlotId) -> bool {
        let facts = self.hit_test_facts(paintable);
        facts.could_be_scrolled_horizontally || facts.could_be_scrolled_vertically
    }

    fn nearest_scrollable_ancestor(&mut self, paintable: NodeSlotId) -> Option<NodeSlotId> {
        let mut candidate = self.data(paintable).containing_block;
        while !candidate.is_invalid() && self.layout_arena.paintable_row_is_populated(candidate) {
            if self.could_be_scrolled_by_wheel_event(candidate) {
                return Some(candidate);
            }
            if style_queries::is_fixed_position(self.layout_arena, candidate) {
                return None;
            }
            candidate = self.data(candidate).containing_block;
        }
        None
    }

    fn minimum_scroll_offset(&self, paintable: NodeSlotId) -> CssPixelPoint {
        let Some(overflow) = paintable_geometry::scrollable_overflow_rect(self.layout_arena, paintable) else {
            return CssPixelPoint::default();
        };
        let scrollport = paintable_geometry::absolute_padding_box_rect(self.layout_arena, paintable);
        let zero = CssPixels::from_raw(0);
        CssPixelPoint::new(
            (overflow.left() - scrollport.left()).min(zero),
            (overflow.top() - scrollport.top()).min(zero),
        )
    }

    fn maximum_scroll_offset(&self, paintable: NodeSlotId) -> CssPixelPoint {
        let Some(overflow) = paintable_geometry::scrollable_overflow_rect(self.layout_arena, paintable) else {
            return CssPixelPoint::default();
        };
        let scrollport = paintable_geometry::absolute_padding_box_rect(self.layout_arena, paintable);
        let zero = CssPixels::from_raw(0);
        CssPixelPoint::new(
            (overflow.right() - scrollport.right()).max(zero),
            (overflow.bottom() - scrollport.bottom()).max(zero),
        )
    }

    fn wheel_hit_test_target_scroll_node_index_for(&mut self, paintable: NodeSlotId) -> usize {
        let mut paintables_to_cache: Vec<NodeSlotId> = Vec::new();
        let mut target = 0usize;
        let mut current = paintable;
        loop {
            if let Some(cached) = self.wheel_hit_test_target_cache.get(&current) {
                target = *cached;
                break;
            }

            paintables_to_cache.push(current);
            let own = self.data(current).own_scroll_node_index;
            if own != 0 && self.could_be_scrolled_by_wheel_event(current) {
                target = own;
                break;
            }

            let raw_containing_block = self.data(current).containing_block;
            let mut containing_block = (!raw_containing_block.is_invalid()
                && self.layout_arena.paintable_row_is_populated(raw_containing_block))
            .then_some(raw_containing_block);
            if let Some(block) = containing_block
                && style_queries::is_fixed_position(self.layout_arena, block)
            {
                let block_own = self.data(block).own_scroll_node_index;
                if block_own != 0 && self.could_be_scrolled_by_wheel_event(block) {
                    target = block_own;
                }
                if target != 0 {
                    break;
                }
                containing_block = None;
            }

            if let Some(block) = containing_block {
                current = block;
                continue;
            }

            let viewport = self.viewport;
            if viewport.is_invalid() || viewport == current {
                break;
            }
            current = viewport;
        }

        for slot in paintables_to_cache {
            self.wheel_hit_test_target_cache.insert(slot, target);
        }
        target
    }

    fn record_wheel_hit_test_target(&mut self, paintable: NodeSlotId) {
        // OPTIMIZATION: The compositor falls back to the viewport when there are no explicit
        // targets. Avoid generating redundant per-box targets when no non-viewport scroller
        // could need one.
        if !self
            .paint_state
            .visual_context
            .scroll_state
            .has_non_viewport_wheel_scroll_target_candidate
        {
            return;
        }
        if !self.is_visible(paintable) || !self.visible_for_hit_testing(paintable) {
            return;
        }
        let scale = self.inputs.device_pixels_per_css_pixel;
        let rect = css_rect_to_device_rect(
            paintable_geometry::absolute_border_box_rect(self.layout_arena, paintable),
            scale,
        );
        if rect.is_empty() {
            return;
        }
        let target_scroll_node_index = self.wheel_hit_test_target_scroll_node_index_for(paintable);
        let corner_radii = self.border_radii(paintable).as_corners(&self.converter);
        let document_id = UniqueNodeId(self.inputs.document_id);
        if corner_radii.has_any_radius() {
            self.recorder.compositor_wheel_hit_test_target_with_corner_radii(
                CompositorWheelHitTestTargetWithCornerRadii {
                    document_id,
                    target_scroll_node_index: VisualContextIndex(target_scroll_node_index),
                    rect,
                    corner_radii,
                },
            );
            return;
        }
        self.recorder
            .compositor_wheel_hit_test_target(CompositorWheelHitTestTarget {
                document_id,
                target_scroll_node_index: VisualContextIndex(target_scroll_node_index),
                rect,
            });
    }

    fn record_blocking_wheel_event_region(
        &mut self,
        paintable: NodeSlotId,
        facts: &crate::painting::host::FfiAsyncScrollFacts,
    ) {
        if self.inputs.has_blocking_wheel_event_region_covering_viewport {
            return;
        }
        if !self.is_visible(paintable) || !self.visible_for_hit_testing(paintable) {
            return;
        }
        if !facts.inside_blocking_wheel_event_handler {
            return;
        }
        let rect = css_rect_to_device_rect(
            paintable_geometry::absolute_border_box_rect(self.layout_arena, paintable),
            self.inputs.device_pixels_per_css_pixel,
        );
        if rect.is_empty() {
            return;
        }
        self.prevent_descendant_subtree_caching();
        self.has_blocking_wheel_event_listeners = true;
        self.recorder
            .compositor_blocking_wheel_event_region(CompositorBlockingWheelEventRegion { rect });
    }

    fn record_main_thread_wheel_event_region(&mut self, paintable: NodeSlotId) {
        let rect = css_rect_to_device_rect(
            paintable_geometry::absolute_border_box_rect(self.layout_arena, paintable),
            self.inputs.device_pixels_per_css_pixel,
        );
        if rect.is_empty() {
            return;
        }
        self.recorder
            .compositor_main_thread_wheel_event_region(CompositorMainThreadWheelEventRegion { rect });
    }

    fn record_scroll_node(&mut self, paintable: NodeSlotId, facts: &crate::painting::host::FfiAsyncScrollFacts) {
        let scroll_node_kind = match facts.scroll_node_kind {
            crate::painting::host::FfiScrollNodeKind::Viewport => CompositorScrollNodeKind::Viewport,
            crate::painting::host::FfiScrollNodeKind::Element => CompositorScrollNodeKind::Element,
            crate::painting::host::FfiScrollNodeKind::PseudoElement => CompositorScrollNodeKind::PseudoElement,
            crate::painting::host::FfiScrollNodeKind::None => return,
        };
        let parent_scroll_node_index = match self.nearest_scrollable_ancestor(paintable) {
            Some(ancestor) => self.data(ancestor).own_scroll_node_index,
            None => 0,
        };
        let is_viewport = self.data(paintable).kind == PaintableKind::ViewportPaintable;
        let scrollport_rect = if is_viewport {
            IntRect::new(
                0,
                0,
                self.inputs.device_viewport_rect.width,
                self.inputs.device_viewport_rect.height,
            )
        } else {
            self.converter
                .rounded_device_rect(paintable_geometry::absolute_padding_box_rect(
                    self.layout_arena,
                    paintable,
                ))
        };
        let scale = self.inputs.device_pixels_per_css_pixel;
        let hit_test_facts = self.hit_test_facts(paintable);
        self.recorder.compositor_scroll_node(CompositorScrollNode {
            document_id: UniqueNodeId(self.inputs.document_id),
            scrollable_node_id: UniqueNodeId(facts.scrollable_node_id),
            scroll_node_index: VisualContextIndex(self.data(paintable).own_scroll_node_index),
            parent_scroll_node_index: VisualContextIndex(parent_scroll_node_index),
            scrollport_rect,
            min_scroll_offset: css_point_to_device_point(self.minimum_scroll_offset(paintable), scale),
            max_scroll_offset: css_point_to_device_point(self.maximum_scroll_offset(paintable), scale),
            scroll_node_kind,
            pseudo_element_type: facts.pseudo_element_type,
            is_viewport,
            can_be_wheel_scrolled_horizontally: hit_test_facts.could_be_scrolled_horizontally,
            can_be_wheel_scrolled_vertically: hit_test_facts.could_be_scrolled_vertically,
        });
    }

    fn record_viewport_scrollbar_state(
        &mut self,
        paintable: NodeSlotId,
        facts: &crate::painting::host::FfiAsyncScrollFacts,
    ) {
        if !facts.records_viewport_scrollbars {
            return;
        }
        let scale = self.inputs.device_pixels_per_css_pixel;
        let min_scroll_offset = css_point_to_device_point(self.minimum_scroll_offset(paintable), scale);
        let max_scroll_offset = css_point_to_device_point(self.maximum_scroll_offset(paintable), scale);
        let scroll_node_index = VisualContextIndex(self.data(paintable).own_scroll_node_index);
        for (index, scrollbar) in facts.viewport_scrollbars.iter().enumerate() {
            if !scrollbar.present {
                continue;
            }
            let vertical = index == 0;
            self.recorder
                .compositor_viewport_scrollbar(CompositorViewportScrollbar {
                    document_id: UniqueNodeId(self.inputs.document_id),
                    scroll_node_index,
                    gutter_rect: self
                        .converter
                        .rounded_device_rect(CssPixelRect::from(scrollbar.gutter_rect)),
                    thumb_rect: self
                        .converter
                        .rounded_device_rect(CssPixelRect::from(scrollbar.thumb_rect)),
                    expanded_gutter_rect: self
                        .converter
                        .rounded_device_rect(CssPixelRect::from(scrollbar.expanded_gutter_rect)),
                    expanded_thumb_rect: self
                        .converter
                        .rounded_device_rect(CssPixelRect::from(scrollbar.expanded_thumb_rect)),
                    scroll_size: scrollbar.scroll_size,
                    expanded_scroll_size: scrollbar.expanded_scroll_size,
                    min_scroll_offset: if vertical {
                        min_scroll_offset.y
                    } else {
                        min_scroll_offset.x
                    },
                    max_scroll_offset: if vertical {
                        max_scroll_offset.y
                    } else {
                        max_scroll_offset.x
                    },
                    thumb_color: scrollbar.thumb_color,
                    track_color: scrollbar.track_color,
                    vertical,
                });
        }
    }

    pub(crate) fn record_async_scrolling_metadata(&mut self, paintable: NodeSlotId) {
        if !self.inputs.is_recording_async_scrolling_metadata {
            return;
        }
        let facts = self.paint_host.async_scroll_facts(self.layout_node_shell(paintable));

        self.record_wheel_hit_test_target(paintable);
        self.record_blocking_wheel_event_region(paintable, &facts);

        if facts.is_nested_navigable_container {
            self.record_main_thread_wheel_event_region(paintable);
        } else if self.data(paintable).own_scroll_node_index != 0 && self.could_be_scrolled_by_wheel_event(paintable) {
            self.record_scroll_node(paintable, &facts);
        }
        self.record_viewport_scrollbar_state(paintable, &facts);

        let sticky_node_index = self.data(paintable).enclosing_scroll_node_index;
        if style_queries::is_sticky_position(self.layout_arena, paintable) && sticky_node_index != 0 {
            let Some(tree) = &self.paint_state.visual_context.tree else {
                return;
            };
            let scroll_state = &self.paint_state.visual_context.scroll_state;
            let sticky_slot = tree.scroll_state_slot_for_node(sticky_node_index);
            if sticky_slot == NO_SCROLL_STATE_SLOT {
                return;
            }
            let state = scroll_state.state_at_slot(sticky_slot);
            if state.is_sticky
                && let Some(constraints) = &state.sticky_constraints
            {
                let scale = self.inputs.device_pixels_per_css_pixel;
                let insets = constraints.insets;
                let inset =
                    |value: CssPixels, present: bool| css_inset_to_device_inset(present.then_some(value), scale);
                let area = CompositorStickyArea {
                    document_id: UniqueNodeId(self.inputs.document_id),
                    scroll_node_index: VisualContextIndex(sticky_node_index),
                    parent_scroll_node_index: VisualContextIndex(scroll_state.node_index_for_slot(state.parent_slot)),
                    nearest_scrolling_ancestor_index: VisualContextIndex(
                        scroll_state.node_index_for_slot(scroll_state.nearest_scrolling_ancestor_slot(sticky_slot)),
                    ),
                    position_relative_to_scroll_ancestor: css_point_to_device_point(
                        constraints.position_relative_to_scroll_ancestor,
                        scale,
                    ),
                    border_box_size: css_size_to_device_size(constraints.border_box_size, scale),
                    scrollport_size: css_size_to_device_size(constraints.scrollport_size, scale),
                    containing_block_region: css_rect_to_device_rect(constraints.containing_block_region, scale),
                    needs_parent_offset_adjustment: constraints.needs_parent_offset_adjustment,
                    inset_top: inset(insets.top, insets.has_top),
                    inset_right: inset(insets.right, insets.has_right),
                    inset_bottom: inset(insets.bottom, insets.has_bottom),
                    inset_left: inset(insets.left, insets.has_left),
                };
                self.recorder.compositor_sticky_area(area);
            }
        }
    }
}
