/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::layout::node_facts::NodeShape;
use crate::painting::host::FfiNodeIdentity;
use crate::painting::paint_read::{PaintRead, PaintRow};

/// The DOM node a row stands for, named the way the host names one: by its StyleNodeID, or by 0
/// for a row that stands for no node of its own. An anonymous row stands for none, and so does
/// the viewport row, whose node is the document, which has no StyleNodeID.
pub(crate) fn row_dom_style_node(arena: &impl PaintRead, slot: NodeSlotId) -> u32 {
    match arena.node(slot) {
        Some(row) if row.is_dom_backed() && row.kind() != NodeKind::Viewport => {
            row.style_node().map_or(0, |style_node| style_node.raw())
        }
        _ => 0,
    }
}

impl HitTestList {
    /// The DOM node the item dispatches events to. A text fragment generated for a pseudo-element
    /// dispatches to the pseudo-element's generator.
    pub(crate) fn item_dispatch_target(&self, arena: &impl PaintRead, item_index: usize) -> FfiNodeIdentity {
        let item = &self.items[item_index];
        match item.kind {
            HitTestItemKind::TextFragment => fragment_layout_node_slot(arena, item)
                .map_or_else(FfiNodeIdentity::default, |slot| row_node_identity(arena, slot, true)),
            _ => event_dispatch_target_of_paintable(arena, item.paintable),
        }
    }

    pub(crate) fn resolve_hit(
        &self,
        arena: &impl PaintRead,
        item_index: usize,
        local_point: CssPixelPoint,
    ) -> crate::painting::host::FfiResolvedHit {
        let item = &self.items[item_index];
        let mut result = crate::painting::host::FfiResolvedHit {
            dispatch: self.item_dispatch_target(arena, item_index),
            ..Default::default()
        };
        match item.kind {
            HitTestItemKind::TextFragment => {
                result.fallback_dispatch = event_dispatch_target_of_paintable(arena, item.paintable);
                result.has_index_in_node = true;
                result.index_in_node = fragment_index_in_node_for_point(arena, item, local_point);
                result.is_text_fragment = true;
            }
            HitTestItemKind::EmptyEditable => {
                result.has_index_in_node = true;
            }
            _ => {}
        }
        result
    }
}

pub(crate) fn with_item_fragment<R>(
    arena: &impl PaintRead,
    item: &HitTestItem,
    f: impl FnOnce(&crate::painting::paintable_data::FragmentRecord) -> R,
) -> Option<R> {
    let fragment_index = item.text_fragment_index? as usize;
    arena
        .committed_side_data(item.paintable)
        .fragments()
        .get(fragment_index)
        .map(f)
}

pub(crate) fn fragment_layout_node_slot(arena: &impl PaintRead, item: &HitTestItem) -> Option<NodeSlotId> {
    with_item_fragment(arena, item, |fragment| fragment.layout_node)
}

/// The DOM node a hit on the paintable dispatches events to: the paintable's own node, or that of
/// the nearest paint ancestor that stands for one.
pub(crate) fn event_dispatch_target_of_paintable(arena: &impl PaintRead, slot: NodeSlotId) -> FfiNodeIdentity {
    let paintable_rows = arena;
    let mut current = paintable_rows.paintable_row_is_populated(slot).then_some(slot);
    while let Some(paintable) = current {
        if arena.node_flags_if_live(paintable) & NodeFlag::Anonymous as u32 == 0 {
            return row_node_identity(arena, paintable, false);
        }
        current = crate::painting::paint_order::paint_parent(paintable_rows, paintable);
    }
    FfiNodeIdentity::default()
}

/// The DOM node a row stands for: the document for the viewport, and the row's identity for any
/// other row built for a node. An anonymous row stands for none, unless `allow_pseudo_fallback`
/// lets a row generated for a pseudo-element stand for its generator.
pub(crate) fn row_node_identity(
    arena: &impl PaintRead,
    slot: NodeSlotId,
    allow_pseudo_fallback: bool,
) -> FfiNodeIdentity {
    let Some(row) = arena.node(slot) else {
        return FfiNodeIdentity::default();
    };
    if row.kind() == NodeKind::Viewport {
        return FfiNodeIdentity {
            style_node: 0,
            is_document: true,
        };
    }
    let is_anonymous = row.flags() & NodeFlag::Anonymous as u32 != 0;
    if is_anonymous && !(allow_pseudo_fallback && row.generated_for() != 0) {
        return FfiNodeIdentity::default();
    }
    FfiNodeIdentity {
        style_node: row.style_node().map_or(0, |style_node| style_node.raw()),
        is_document: false,
    }
}

pub(crate) fn fragment_index_in_node_for_point(
    arena: &impl PaintRead,
    item: &HitTestItem,
    local_point: CssPixelPoint,
) -> usize {
    with_item_fragment(arena, item, |fragment| {
        let paintable_rows = arena;
        crate::painting::text_fragment::index_in_node_for_point(paintable_rows, fragment, local_point)
    })
    .unwrap_or(0)
}
