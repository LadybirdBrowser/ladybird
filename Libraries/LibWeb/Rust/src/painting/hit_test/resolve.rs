/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::layout::node_facts::NodeShape;
use crate::painting::host::{FfiCaretBoundaryKind, FfiNodeIdentity, FfiResolvedCaret};
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::paintable_data::SELECTION_STATE_START_AND_END;

pub(crate) fn empty_line_is_anchored_to_its_forced_break(arena: &impl PaintRead, item: &HitTestItem) -> bool {
    arena.node_kind_if_live(item.caret_node) == Some(crate::layout::node_data::NodeKind::BreakNode)
}

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
    /// The DOM node the item stands for.
    pub(crate) fn item_target(&self, arena: &impl PaintRead, item_index: usize) -> FfiNodeIdentity {
        let item = &self.items[item_index];
        let slot = match item.kind {
            HitTestItemKind::TextFragment => fragment_layout_node_slot(arena, item),
            HitTestItemKind::EmptyLine => Some(item.caret_node),
            _ => Some(item.paintable),
        };
        slot.map_or_else(FfiNodeIdentity::default, |slot| row_node_identity(arena, slot, false))
    }

    /// The DOM node the item dispatches events to. A text fragment generated for a pseudo-element
    /// dispatches to the pseudo-element's generator.
    pub(crate) fn item_dispatch_target(&self, arena: &impl PaintRead, item_index: usize) -> FfiNodeIdentity {
        let item = &self.items[item_index];
        match item.kind {
            HitTestItemKind::TextFragment => fragment_layout_node_slot(arena, item)
                .map_or_else(FfiNodeIdentity::default, |slot| row_node_identity(arena, slot, true)),
            HitTestItemKind::EmptyLine => row_node_identity(arena, item.caret_node, false),
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

    pub(crate) fn resolve_caret(
        &self,
        arena: &impl PaintRead,
        item_index: usize,
        local_point: CssPixelPoint,
        position_type: crate::painting::hit_test::caret::CaretPositionType,
    ) -> FfiResolvedCaret {
        let item = &self.items[item_index];
        match item.kind {
            HitTestItemKind::TextFragment => with_item_fragment(arena, item, |fragment| {
                let offset = match position_type {
                    crate::painting::hit_test::caret::CaretPositionType::Before => fragment.dom_start_offset_in_node,
                    crate::painting::hit_test::caret::CaretPositionType::After => {
                        // INTEROP: Fully collapsed whitespace at the end of a text run is not a caret stop.
                        //          Keep the whitespace boundary at soft wraps for upstream affinity.
                        let end = fragment.start
                            + fragment.length_in_code_units
                            + fragment.trailing_whitespace_length_in_code_units;
                        if arena
                            .rendered_text(fragment.layout_node)
                            .is_some_and(|rendered| end < rendered.text.len())
                        {
                            fragment.dom_end_offset_with_trailing_whitespace
                        } else {
                            fragment.dom_end_offset_in_node
                        }
                    }
                    crate::painting::hit_test::caret::CaretPositionType::Closest => {
                        let paintable_rows = arena;
                        crate::painting::text_fragment::index_in_node_for_point(paintable_rows, fragment, local_point)
                    }
                };
                let node = row_node_identity(arena, fragment.layout_node, false);
                let affinity_is_upstream = offset >= fragment.dom_end_offset_in_node
                    && offset == fragment.dom_end_offset_with_trailing_whitespace;
                let debug_rect = fragment_caret_range_rect(arena, fragment, offset);
                FfiResolvedCaret {
                    has_position: true,
                    node,
                    boundary: FfiCaretBoundaryKind::Offset,
                    offset,
                    affinity_is_upstream,
                    has_debug_rect: true,
                    debug_rect: debug_rect.into(),
                }
            })
            .unwrap_or_default(),
            HitTestItemKind::EmptyLine => FfiResolvedCaret {
                has_position: true,
                node: row_node_identity(arena, item.caret_node, false),
                boundary: if empty_line_is_anchored_to_its_forced_break(arena, item) {
                    FfiCaretBoundaryKind::IndexOfNodeInParent
                } else {
                    FfiCaretBoundaryKind::Offset
                },
                offset: item.caret_offset,
                has_debug_rect: true,
                debug_rect: item.caret_rect.into(),
                ..Default::default()
            },
            HitTestItemKind::EmptyEditable => FfiResolvedCaret {
                has_position: true,
                node: row_node_identity(arena, item.paintable, false),
                boundary: FfiCaretBoundaryKind::Offset,
                has_debug_rect: true,
                debug_rect: item.caret_rect.into(),
                ..Default::default()
            },
            HitTestItemKind::Box => {
                let is_before = match position_type {
                    crate::painting::hit_test::caret::CaretPositionType::Before => true,
                    crate::painting::hit_test::caret::CaretPositionType::After => false,
                    crate::painting::hit_test::caret::CaretPositionType::Closest => {
                        self.box_point_is_before(item_index, local_point)
                    }
                };
                FfiResolvedCaret {
                    has_position: true,
                    node: row_node_identity(arena, item.paintable, false),
                    boundary: if is_before {
                        FfiCaretBoundaryKind::BeforeNode
                    } else {
                        FfiCaretBoundaryKind::AfterNode
                    },
                    has_debug_rect: true,
                    debug_rect: item.caret_rect.into(),
                    ..Default::default()
                }
            }
            HitTestItemKind::SvgPath | HitTestItemKind::ChromeWidget => Default::default(),
        }
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

fn fragment_caret_range_rect(
    arena: &impl PaintRead,
    fragment: &crate::painting::paintable_data::FragmentRecord,
    offset: usize,
) -> CssPixelRect {
    let paintable_rows = arena;
    let Some(offsets) = crate::painting::text_fragment::compute_selection_offsets(
        paintable_rows,
        fragment,
        SELECTION_STATE_START_AND_END,
        offset,
        offset,
    ) else {
        return CssPixelRect::default();
    };
    crate::painting::text_fragment::rect_for_selection_offsets(paintable_rows, fragment, offsets, || {
        crate::painting::text_fragment::first_available_font(paintable_rows, fragment)
    })
}
