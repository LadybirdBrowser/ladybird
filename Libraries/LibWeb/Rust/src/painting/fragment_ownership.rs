/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::*;
use crate::painting::stacking_context::NO_STACKING_CONTEXT;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FragmentRange {
    pub begin: u32,
    pub end: u32,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FragmentOwnershipFilter {
    pub include_everything: bool,
    /// Sorted by begin and disjoint.
    pub included: Vec<FragmentRange>,
    pub excluded: Vec<FragmentRange>,
}

impl FragmentOwnershipFilter {
    pub fn everything() -> Self {
        Self {
            include_everything: true,
            included: Vec::new(),
            excluded: Vec::new(),
        }
    }

    pub fn for_each_owned_fragment_index(&self, fragment_count: usize, mut callback: impl FnMut(usize)) {
        let mut excluded_cursor = 0;
        let mut is_excluded = |index: usize| {
            while excluded_cursor < self.excluded.len() && (self.excluded[excluded_cursor].end as usize) <= index {
                excluded_cursor += 1;
            }
            excluded_cursor < self.excluded.len() && index >= self.excluded[excluded_cursor].begin as usize
        };
        let mut visit_range = |begin: usize, end: usize| {
            for index in begin..end {
                if !is_excluded(index) {
                    callback(index);
                }
            }
        };
        if self.include_everything {
            visit_range(0, fragment_count);
            return;
        }
        for range in &self.included {
            visit_range(range.begin as usize, range.end as usize);
        }
    }
}

pub fn is_self_painting_inline(data: &PaintableData) -> bool {
    // Whether this box paints its own foreground (fragments and caret) instead of the
    // containing block: it forms a group that content must be recorded inside.
    data.kind == PaintableKind::InlinePaintable
        && (data.stacking_context != NO_STACKING_CONTEXT || data.has_flag(PaintableFlag::Positioned))
}

pub(crate) fn node_is_fragmented_inline(layout_arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    match layout_arena.node_kind_if_live(node) {
        Some(NodeKind::InlineNode) => true,
        Some(NodeKind::ListItemBox) => layout_arena
            .node_style_if_live(node)
            .is_some_and(|style| style.display().is_inline_outside() && style.display().is_flow_inside()),
        _ => false,
    }
}

pub(crate) fn nearest_fragmented_inline_ancestor(
    layout_arena: &LayoutNodeArena,
    node: NodeSlotId,
) -> Option<NodeSlotId> {
    let mut ancestor = layout_arena.node_parent_if_live(node);
    while let Some(candidate) = ancestor {
        let display = layout_arena.node_style_if_live(candidate).map(|style| style.display());
        if !display.is_some_and(|display| display.is_inline_outside() && display.is_flow_inside()) {
            break;
        }
        if node_is_fragmented_inline(layout_arena, candidate) {
            return Some(candidate);
        }
        ancestor = layout_arena.node_parent_if_live(candidate);
    }
    None
}

pub(crate) fn nearest_self_painting_inline_box(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    node: NodeSlotId,
) -> Option<PaintableSlotId> {
    let mut ancestor = nearest_fragmented_inline_ancestor(layout_arena, node);
    while let Some(candidate) = ancestor {
        let paintable = paintables.paintable_of_node(candidate);
        if !paintable.is_invalid()
            && paintables.is_live(paintable)
            && is_self_painting_inline(&paintables.data_ref(paintable))
        {
            return Some(paintable);
        }
        ancestor = nearest_fragmented_inline_ancestor(layout_arena, candidate);
    }
    None
}

pub(crate) fn assign_fragment_ownership(
    layout_arena: &LayoutNodeArena,
    paintables: &mut PaintableArena,
    viewport: PaintableSlotId,
) {
    // Whether a box is self-painting depends on the stacking context tree, so this runs
    // whenever that tree is rebuilt.
    let mut stack = vec![viewport];
    while let Some(current) = stack.pop() {
        if let Some(next) = paintables.next_sibling(current)
            && current != viewport
        {
            stack.push(next);
        }
        if let Some(first_child) = paintables.first_child(current) {
            stack.push(first_child);
        }
        let data = paintables.data_ref(current);
        if data.kind.has_lines() && !paintables.side(current).inline_box_pieces.is_empty() {
            assign_for_block(layout_arena, paintables, current);
        }
    }
}

fn assign_for_block(layout_arena: &LayoutNodeArena, paintables: &mut PaintableArena, block: PaintableSlotId) {
    let pieces = paintables.side(block).inline_box_pieces.clone();
    let mut block_filter = FragmentOwnershipFilter::everything();

    let piece_paintable_of = |paintables: &PaintableArena, node: NodeSlotId| -> Option<PaintableSlotId> {
        if node.is_invalid() || layout_arena.shell_if_live(node).is_null() {
            return None;
        }
        let paintable = paintables.paintable_of_node(node);
        (!paintable.is_invalid()
            && paintables.is_live(paintable)
            && paintables.data_ref(paintable).kind == PaintableKind::InlinePaintable)
            .then_some(paintable)
    };

    // Start every piece's box from a clean slate.
    for piece in &pieces {
        if let Some(paintable) = piece_paintable_of(paintables, piece.node) {
            paintables.side_mut(paintable).fragment_ownership = None;
        }
    }

    let mut owners: Vec<PaintableSlotId> = Vec::new();
    let mut filters: Vec<FragmentOwnershipFilter> = Vec::new();
    let filter_index = |owners: &mut Vec<PaintableSlotId>,
                        filters: &mut Vec<FragmentOwnershipFilter>,
                        owner: PaintableSlotId|
     -> usize {
        if let Some(index) = owners.iter().position(|candidate| *candidate == owner) {
            return index;
        }
        owners.push(owner);
        filters.push(FragmentOwnershipFilter::default());
        owners.len() - 1
    };

    for piece in &pieces {
        if piece.fragment_count == 0 {
            continue;
        }
        let Some(piece_paintable) = piece_paintable_of(paintables, piece.node) else {
            continue;
        };
        if !is_self_painting_inline(&paintables.data_ref(piece_paintable)) {
            continue;
        }
        let range = FragmentRange {
            begin: piece.first_fragment_index,
            end: piece.first_fragment_index + piece.fragment_count,
        };
        let index = filter_index(&mut owners, &mut filters, piece_paintable);
        filters[index].included.push(range);
        // The nearest self-painting box above owns the surrounding content but must not paint this
        // box's subtree; content outside any such box falls to the block.
        if let Some(enclosing_owner) = nearest_self_painting_inline_box(layout_arena, paintables, piece.node) {
            let index = filter_index(&mut owners, &mut filters, enclosing_owner);
            filters[index].excluded.push(range);
        } else {
            block_filter.excluded.push(range);
        }
    }

    block_filter.excluded.sort_by_key(|range| range.begin);
    paintables.side_mut(block).fragment_ownership = Some(block_filter);
    for (owner, mut filter) in owners.into_iter().zip(filters) {
        filter.excluded.sort_by_key(|range| range.begin);
        paintables.side_mut(owner).fragment_ownership = Some(filter);
    }
}

pub fn effective_filter(
    paintables: &PaintableArena,
    paintable: PaintableSlotId,
) -> std::borrow::Cow<'_, FragmentOwnershipFilter> {
    if let Some(filter) = &paintables.side(paintable).fragment_ownership {
        return std::borrow::Cow::Borrowed(filter);
    }
    if paintables.data_ref(paintable).kind.has_lines() {
        return std::borrow::Cow::Owned(FragmentOwnershipFilter::everything());
    }
    std::borrow::Cow::Owned(FragmentOwnershipFilter::default())
}
