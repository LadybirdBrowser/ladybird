/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::positioning;
use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts::{self, NodeShape};
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::style_queries;
use crate::painting::viewport_mapping::{ViewportMapping, ViewportSpace};

// https://www.w3.org/TR/intersection-observer/#compute-the-intersection
pub(crate) fn intersection_rect(
    arena: &impl PaintRead,
    target: NodeSlotId,
    target_rect: CssPixelRect,
    intersection_root: NodeSlotId,
    root_bounds: CssPixelRect,
    viewport_mapping: &ViewportMapping<'_>,
    inflate_scroll_container_clip_rect_by_scroll_margin: impl FnMut(CssPixelRect) -> CssPixelRect,
) -> CssPixelRect {
    // 1. Let intersectionRect be the result of getting the bounding box for target.
    // 2. Let container be the containing block of target.
    // 3. While container is not root:
    // FIXME: 3.1. If container is the document of a nested browsing context, update intersectionRect by clipping to
    //             the viewport of the document, and update container to be the browsing context container of
    //             container.
    // NOTE: Steps 3.2 (map to container coordinate space) and 3.5 (update container) are unnecessary here because the
    //       bounding box and every clip are mapped into the viewport.
    // 3.3. If container is a scroll container, apply the observer’s [[scrollMargin]] to the container’s clip rect.
    // 3.4. If container has a content clip or a css clip-path property, update intersectionRect by applying
    //      container’s clip.
    let intersection_rect = viewport_mapping.visible_rect_in_ancestor(
        arena,
        target,
        target_rect,
        intersection_root,
        ViewportSpace::LayoutViewport,
        inflate_scroll_container_clip_rect_by_scroll_margin,
    );

    // FIXME: 4. Map intersectionRect to the coordinate space of root.

    // 5. Update intersectionRect by intersecting it with the root intersection rectangle.
    // FIXME: 6. Map intersectionRect to the coordinate space of the viewport of the document containing target.
    // 7. Return intersectionRect.
    intersection_rect.intersected(root_bounds)
}

pub(crate) fn transform_subtree_is_clipped_outside(
    arena: &impl PaintRead,
    target: NodeSlotId,
    root_bounds: CssPixelRect,
    viewport_mapping: &ViewportMapping<'_>,
) -> bool {
    if !arena.paintable_row_is_populated(target) || root_bounds.is_empty() {
        return false;
    }
    let target_kind = arena.node_kind_if_live(target).unwrap_or(NodeKind::Unset);
    if node_facts::kind_is_box(target_kind)
        && (style_queries::is_fixed_position(arena, target)
            || arena.node_flags_if_live(target) & NodeFlag::AbsposDescendantEscapes as u32 != 0)
    {
        return false;
    }

    let mut has_disjoint_clip = false;
    let mut ancestor = arena.node_parent_if_live(target).unwrap_or(NodeSlotId::INVALID);
    while let Some(ancestor_row) = arena.node(ancestor) {
        let parent = ancestor_row.parent();
        if node_facts::kind_is_box(ancestor_row.kind()) {
            if !arena.paintable_row_is_populated(ancestor) {
                return false;
            }
            let Some(style) = ancestor_row.style() else {
                ancestor = parent;
                continue;
            };
            if style.position() == positioning::FIXED {
                return false;
            }

            if let Some(clip_rect) = viewport_mapping.content_clip_rect(arena, ancestor, ViewportSpace::LayoutViewport)
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
