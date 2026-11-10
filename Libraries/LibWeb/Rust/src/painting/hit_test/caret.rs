/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::node_data::{DomPaintFact, NodeFlag, NodeKind};
use crate::layout::query::inline_cursor::{CaretPosition, InlineCursor};
use crate::painting::host::{FfiCaretAt, FfiCaretBoundaryKind, FfiHitTestQueryCallbacks, FfiResolvedCaret};
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::paintable_data::FragmentRecord;
use crate::painting::viewport_mapping::{ViewportMapping, ViewportSpace};
use crate::painting::visual_context::ClipBehavior;
use crate::painting::{node_painting, paintable_geometry, style_queries};

impl HitTestList {
    /// The caret position at `point`: the one the box the point hits gives it, as editing resolves a point, or with a
    /// `constraint_scope` box, the one that box gives it wherever the point is.
    pub(crate) fn caret_position_from_point(
        &self,
        arena: &impl PaintRead,
        tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        constraint_scope: NodeSlotId,
    ) -> Option<FfiCaretAt> {
        let resolver = PointResolver {
            arena,
            mapping: callbacks.viewport_mapping(tree),
            point,
        };
        if !constraint_scope.is_invalid() {
            let mut scope = constraint_scope;
            while !arena.paintable_row_is_populated(scope) {
                scope = arena.node_parent_if_live(scope)?;
            }
            return resolver.in_box(scope);
        }
        // A point that hits nothing, such as one past the edge of the viewport during a drag, goes to the viewport.
        let Some(hit) = self.find_topmost_item(arena, tree, callbacks, point) else {
            let mut viewport = self.items.first()?.paintable;
            while let Some(parent) = arena.node_parent_if_live(viewport) {
                viewport = parent;
            }
            return resolver.in_box(viewport);
        };
        let item = &self.items[hit.index];
        let caret = match item.kind {
            HitTestItemKind::TextFragment => resolver.in_lines_of(item.paintable),
            HitTestItemKind::Box | HitTestItemKind::EmptyEditable | HitTestItemKind::ChromeWidget => {
                resolver.in_box(item.paintable)
            }
            HitTestItemKind::SvgPath => Some(resolver.at_start_of(item.paintable)),
        };
        if let Some(caret) = caret.filter(|caret| caret.caret.has_position) {
            return Some(caret);
        }
        // Generated content, such as a list marker, gives no position, and the nearest box around it that stands for a
        // node takes the point.
        let mut node = arena.node_parent_if_live(item.paintable)?;
        while !arena.node_is_dom_backed(node) {
            node = arena.node_parent_if_live(node)?;
        }
        resolver.in_box(node)
    }
}

/// Resolves a point, in the viewport, to a caret position the way a box gives one for a point over it.
struct PointResolver<'a, A: PaintRead> {
    arena: &'a A,
    mapping: ViewportMapping<'a>,
    point: CssPixelPoint,
}

impl<A: PaintRead> PointResolver<'_, A> {
    /// The point in the space `node` lays out its descendants in.
    fn point_in_descendants_space_of(&self, node: NodeSlotId) -> Option<CssPixelPoint> {
        let context = self
            .arena
            .paintable_data(node)
            .accumulated_visual_context_for_descendants;
        self.mapping
            .point_to_local(context, self.point, ViewportSpace::VisualViewport, ClipBehavior::Ignore)
            .map(to_css_point)
    }

    fn in_lines_of(&self, root: NodeSlotId) -> Option<FfiCaretAt> {
        let arena = self.arena;
        let point = self.point_in_descendants_space_of(root)?;
        let produces_caret = |fragment: &FragmentRecord| arena.node_is_dom_backed(fragment.layout_node);
        let position = InlineCursor::new(arena).position_for_point(root, point, &produces_caret)?;
        Some(self.caret_at(root, position))
    }

    /// The caret position the box `node` gives the point: in its lines, beside it where it is replaced, in the block
    /// child the point is over or next to, or at its start.
    fn in_box(&self, node: NodeSlotId) -> Option<FfiCaretAt> {
        let arena = self.arena;
        if style_queries::is_replaced_box(arena, node) {
            return self.beside_replaced_box(node);
        }
        if node_painting::is_fragmented_inline(arena, node) {
            return self.in_box(arena.node_containing_block_if_live(node)?);
        }
        if arena.node_flags_if_live(node) & NodeFlag::ChildrenAreInline as u32 != 0 {
            return self.in_lines_of(node).or_else(|| Some(self.at_start_of(node)));
        }
        if let Some(caret) = self.in_block_child_of(node) {
            return Some(caret);
        }
        Some(self.at_start_of(node))
    }

    // A point before the line of a replaced box in the block direction, or over its first half, is before it.
    fn beside_replaced_box(&self, node: NodeSlotId) -> Option<FfiCaretAt> {
        let arena = self.arena;
        let context = arena.paintable_data(node).accumulated_visual_context;
        let point = self
            .mapping
            .point_to_local(context, self.point, ViewportSpace::VisualViewport, ClipBehavior::Ignore)
            .map(to_css_point)?;
        let rect = paintable_geometry::absolute_rect(arena, node);
        let style = arena.node_style_if_live(node)?;
        let horizontal = style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
        let (inline, inline_start, inline_size, block, block_start, block_end) = if horizontal {
            (point.x, rect.x, rect.width, point.y, rect.y, rect.bottom())
        } else {
            (point.y, rect.y, rect.height, point.x, rect.x, rect.right())
        };
        let after = if block < block_start {
            style.block_axis_is_reverse()
        } else if block >= block_end {
            !style.block_axis_is_reverse()
        } else {
            (inline - inline_start > inline_size.scaled(0.5)) != style.inline_axis_is_reverse()
        };
        Some(self.caret_at(node, CaretPosition::BesideAtomicInline { node, after }))
    }

    /// The caret position the block child of `node` that the point is over, or the last one it is past, gives it. A
    /// child whose editability differs from that of `node` takes it beside itself instead.
    fn in_block_child_of(&self, node: NodeSlotId) -> Option<FfiCaretAt> {
        let arena = self.arena;
        let point = self.point_in_descendants_space_of(node)?;
        let style = arena.node_style_if_live(node)?;
        let horizontal = style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
        let block_axis_is_reverse = style.block_axis_is_reverse();
        // Block coordinates that grow in the block direction.
        let block_extent = |rect: CssPixelRect| match (horizontal, block_axis_is_reverse) {
            (true, _) => (rect.y, rect.bottom()),
            (false, false) => (rect.x, rect.right()),
            (false, true) => (-rect.right(), -rect.x),
        };
        let inline_extent = |rect: &CssPixelRect| {
            if horizontal {
                (rect.x, rect.right())
            } else {
                (rect.y, rect.bottom())
            }
        };
        let fragments_of = |child: NodeSlotId| {
            (0..paintable_geometry::fragment_count(arena, child))
                .map(move |index| (child, paintable_geometry::absolute_fragment_rect(arena, child, index)))
        };
        let children = || {
            arena
                .children(node)
                .filter(|&child| arena.paintable_row_is_populated(child))
        };
        let mut candidates: Vec<(NodeSlotId, CssPixelRect)> = children()
            .filter(|&child| {
                arena
                    .node(child)
                    .is_some_and(|row| !row.is_out_of_flow() && !row.is_floating())
            })
            .flat_map(fragments_of)
            .filter(|(_, rect)| !rect.is_empty())
            .collect();
        // A block flow takes the first child that ends past the point in the block direction, or the last one where the
        // point is past its start; other boxes take the child nearest the point.
        let is_block_flow = !matches!(style.display(), display if display.is_flex_inside() || display.is_grid_inside() || display.is_table_inside());
        // Children laid out in columns take the point in the column it is in, or else the one nearest to it.
        let inline = if horizontal { point.x } else { point.y };
        // Children are in columns where two of them side by side overlap in the block direction.
        let mut extents: Vec<_> = candidates
            .iter()
            .map(|(_, rect)| (block_extent(*rect), inline_extent(rect)))
            .collect();
        extents.sort_by_key(|((block_start, _), _)| *block_start);
        let in_columns = is_block_flow
            && extents.windows(2).any(|pair| {
                let (
                    ((_, first_block_end), (first_start, first_end)),
                    ((second_block_start, _), (second_start, second_end)),
                ) = (pair[0], pair[1]);
                second_block_start < first_block_end && (second_start >= first_end || first_start >= second_end)
            });
        if in_columns {
            let distance = |rect: &CssPixelRect| {
                let (start, end) = inline_extent(rect);
                (start - inline)
                    .max(inline - end + CssPixels::from_integer(1))
                    .max(CssPixels::default())
            };
            let nearest = candidates
                .iter()
                .map(|(_, rect)| distance(rect))
                .min()
                .unwrap_or_default();
            candidates.retain(|(_, rect)| distance(rect) == nearest);
        }
        // The point covers the pixel after it.
        let one = CssPixels::from_integer(1);
        let point_rect = CssPixelRect::new(point.x, point.y, one, one);
        let block = block_extent(point_rect).0;
        let distance = |rect: &CssPixelRect| {
            let gap = |start: CssPixels, end: CssPixels, point_start: CssPixels, point_end: CssPixels| {
                (start - point_start)
                    .max(point_end - end)
                    .max(CssPixels::default())
                    .to_double()
            };
            let horizontal_gap = gap(rect.x, rect.right(), point_rect.x, point_rect.right());
            let vertical_gap = gap(rect.y, rect.bottom(), point_rect.y, point_rect.bottom());
            horizontal_gap * horizontal_gap + vertical_gap * vertical_gap
        };
        if candidates.is_empty() {
            return self.in_nearest_positioned_child_of(node, point, &distance);
        }
        let &(child, rect) = if is_block_flow {
            candidates
                .last()
                .filter(|(_, rect)| block >= block_extent(*rect).0)
                .or_else(|| candidates.iter().find(|(_, rect)| block < block_extent(*rect).1))?
        } else {
            candidates
                .iter()
                .min_by(|(_, a), (_, b)| distance(a).total_cmp(&distance(b)))?
        };
        let is_editable = |node| arena.node_has_dom_paint_fact(node, DomPaintFact::EditableOrEditingHost);
        let is_root_element = arena
            .node_parent_if_live(node)
            .is_some_and(|parent| arena.node_kind_if_live(parent) == Some(NodeKind::Viewport));
        // A column stands for no node, and so the boxes in it keep the point whatever their editability.
        if arena.node_is_dom_backed(child)
            && arena.node_is_dom_backed(node)
            && !is_root_element
            && !in_columns
            && is_editable(child) != is_editable(node)
        {
            let (inline, inline_start, inline_size) = if horizontal {
                (point.x, rect.x, rect.width)
            } else {
                (point.y, rect.y, rect.height)
            };
            let after = inline - inline_start >= inline_size.scaled(0.5);
            return Some(self.caret_at(node, CaretPosition::BesideAtomicInline { node: child, after }));
        }
        self.in_box(child)
    }

    // INTEROP: A box with no in-flow child that has a size takes the point in the child nearest to it, of any kind, by
    //          its geometry before its transform.
    fn in_nearest_positioned_child_of(
        &self,
        node: NodeSlotId,
        point: CssPixelPoint,
        distance: &dyn Fn(&CssPixelRect) -> f64,
    ) -> Option<FfiCaretAt> {
        let arena = self.arena;
        let child = arena
            .children(node)
            .filter(|&child| arena.paintable_row_is_populated(child) && arena.node_is_dom_backed(child))
            .min_by(|&a, &b| {
                distance(&paintable_geometry::absolute_rect(arena, a))
                    .total_cmp(&distance(&paintable_geometry::absolute_rect(arena, b)))
            })?;
        let produces_caret = |fragment: &FragmentRecord| arena.node_is_dom_backed(fragment.layout_node);
        let position = node_painting::has_lines(arena, child)
            .then(|| InlineCursor::new(arena).position_for_point(child, point, &produces_caret))
            .flatten()
            .unwrap_or(CaretPosition::StartOfBox { node: child });
        Some(self.caret_at(child, position))
    }

    fn at_start_of(&self, node: NodeSlotId) -> FfiCaretAt {
        self.caret_at(node, CaretPosition::StartOfBox { node })
    }

    fn caret_at(&self, paintable: NodeSlotId, position: CaretPosition) -> FfiCaretAt {
        let arena = self.arena;
        let identity = |node| resolve::row_node_identity(arena, node, false);
        let mut caret = match position {
            CaretPosition::Text {
                text,
                offset,
                downstream,
            } => FfiResolvedCaret {
                node: identity(text),
                boundary: FfiCaretBoundaryKind::Offset,
                offset,
                affinity_is_upstream: !downstream,
                ..Default::default()
            },
            CaretPosition::BesideAtomicInline { node, after } => FfiResolvedCaret {
                node: identity(node),
                boundary: if after {
                    FfiCaretBoundaryKind::AfterNode
                } else {
                    FfiCaretBoundaryKind::BeforeNode
                },
                ..Default::default()
            },
            CaretPosition::BeforeForcedBreak { node } => FfiResolvedCaret {
                node: identity(node),
                boundary: FfiCaretBoundaryKind::IndexOfNodeInParent,
                ..Default::default()
            },
            CaretPosition::StartOfBox { node } => FfiResolvedCaret {
                node: identity(node),
                boundary: FfiCaretBoundaryKind::Offset,
                ..Default::default()
            },
        };
        caret.has_position = !caret.node.is_none();
        if let Some(rect) = InlineCursor::new(arena).caret_rect(position) {
            caret.has_debug_rect = true;
            caret.debug_rect = if rect.owner == rect.node {
                self.mapping
                    .rect_to_viewport(arena, rect.owner, rect.rect, ViewportSpace::VisualViewport)
            } else {
                self.mapping.line_content_rect_to_viewport(
                    arena,
                    rect.owner,
                    rect.node,
                    rect.rect,
                    ViewportSpace::VisualViewport,
                )
            }
            .into();
        }
        FfiCaretAt { paintable, caret }
    }
}
