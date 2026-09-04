/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{overflow, positioning};
use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::rect_to_viewport_transform::{RectToViewportTransform, transform_rect_to_viewport_or_identity};
use crate::painting::style_queries;

fn content_clip_rect_in_viewport_space(
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    style: ComputedValuesView<'_>,
    rect_to_viewport_transform: Option<&RectToViewportTransform<'_>>,
) -> Option<CssPixelRect> {
    let has_content_clip = style.overflow_x() != overflow::VISIBLE || style.overflow_y() != overflow::VISIBLE;
    if !has_content_clip {
        return None;
    }
    Some(transform_rect_to_viewport_or_identity(
        rect_to_viewport_transform,
        arena,
        node,
        paintable_geometry::absolute_padding_box_rect(arena, node),
    ))
}

// https://www.w3.org/TR/intersection-observer/#compute-the-intersection
pub(crate) fn intersection_rect(
    arena: &impl PaintableRowsRead,
    target: NodeSlotId,
    target_rect: CssPixelRect,
    intersection_root: NodeSlotId,
    root_bounds: CssPixelRect,
    rect_to_viewport_transform: Option<&RectToViewportTransform<'_>>,
    mut inflate_scroll_container_clip_rect_by_scroll_margin: impl FnMut(CssPixelRect) -> CssPixelRect,
) -> CssPixelRect {
    // 1. Let intersectionRect be the result of getting the bounding box for target.
    let mut intersection_rect = target_rect;

    // 2. Let container be the containing block of target.
    // 3. While container is not root:
    if arena.paintable_row_is_populated(target) {
        let mut container = arena.node_containing_block_if_live(target);
        while let Some(container_slot) = container {
            // Stop when we reach the intersection root.
            if container_slot == intersection_root {
                break;
            }
            if !arena.paintable_row_is_populated(container_slot) {
                break;
            }

            // FIXME: 3.1. If container is the document of a nested browsing context, update
            //             intersectionRect by clipping to the viewport of the document, and update
            //             container to be the browsing context container of container.

            // NOTE: Steps 3.2 (map to container coordinate space) and 3.5 (update container) are
            //       unnecessary here because get_bounding_client_rect() and transform_rect_to_viewport()
            //       already produce viewport-relative coordinates.

            // 3.3. If container is a scroll container, apply the observer’s [[scrollMargin]]
            //      to the container’s clip rect.
            // 3.4. If container has a content clip or a css clip-path property, update intersectionRect
            //      by applying container’s clip.
            // FIXME: Handle clip-path.
            if let Some(style) = arena.node_style_if_live(container_slot)
                && let Some(mut clip_rect) =
                    content_clip_rect_in_viewport_space(arena, container_slot, style, rect_to_viewport_transform)
            {
                // Apply scroll margin to expand the scrollport for scroll containers.
                let container_kind = arena.data(container_slot).kind.get();
                if node_facts::kind_and_style_make_scroll_container(container_kind, Some(style)) {
                    clip_rect = inflate_scroll_container_clip_rect_by_scroll_margin(clip_rect);
                }

                intersection_rect = intersection_rect.intersected(clip_rect);
            }
            container = arena.node_containing_block_if_live(container_slot);
        }
    }

    // FIXME: 4. Map intersectionRect to the coordinate space of root.

    // 5. Update intersectionRect by intersecting it with the root intersection rectangle.
    intersection_rect = intersection_rect.intersected(root_bounds);

    // FIXME: 6. Map intersectionRect to the coordinate space of the viewport of the document containing target.

    // 7. Return intersectionRect.
    intersection_rect
}

pub(crate) fn transform_subtree_is_clipped_outside(
    arena: &impl PaintableRowsRead,
    target: NodeSlotId,
    root_bounds: CssPixelRect,
    rect_to_viewport_transform: Option<&RectToViewportTransform<'_>>,
) -> bool {
    if !arena.paintable_row_is_populated(target) || root_bounds.is_empty() {
        return false;
    }
    let target_data = arena.data(target);
    if node_facts::kind_is_box(target_data.kind.get())
        && (style_queries::is_fixed_position(arena, target)
            || node_facts::has_flag(target_data, NodeFlag::AbsposDescendantEscapes))
    {
        return false;
    }

    let mut has_disjoint_clip = false;
    let mut ancestor = target_data.parent.get();
    while let Some(ancestor_data) = arena.node_data_if_live(ancestor) {
        let parent = ancestor_data.parent.get();
        if node_facts::kind_is_box(ancestor_data.kind.get()) {
            if !arena.paintable_row_is_populated(ancestor) {
                return false;
            }
            let Some(style) = arena.node_style_if_live(ancestor) else {
                ancestor = parent;
                continue;
            };
            if style.box_values().position == positioning::FIXED {
                return false;
            }

            if let Some(clip_rect) =
                content_clip_rect_in_viewport_space(arena, ancestor, style, rect_to_viewport_transform)
                && !clip_rect.edge_adjacent_intersects(root_bounds)
            {
                has_disjoint_clip = true;
            }

            // A transform outside the disjoint clip could move the clip into the observation root without updating
            // its main-thread geometry. Transforms inside the clip can only move content within the clipped region.
            if has_disjoint_clip
                && (style_queries::has_css_transform(arena, ancestor, style)
                    || style_queries::is_sticky_position(style))
            {
                return false;
            }
        }
        ancestor = parent;
    }
    has_disjoint_clip
}
