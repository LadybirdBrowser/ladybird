/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeFlag;
use crate::painting::host::FfiCaretBoundaryKind;
use crate::painting::host::FfiResolvedCaret;
use crate::painting::paintable_data::SELECTION_STATE_START_AND_END;
use std::ffi::c_void;

impl HitTestList {
    pub(crate) fn item_target_shell(&self, arena: &LayoutNodeArena, item_index: usize) -> *mut c_void {
        let item = &self.items[item_index];
        let slot = match item.kind {
            HitTestItemKind::TextFragment => fragment_layout_node_slot(arena, item),
            HitTestItemKind::EmptyLine => Some(item.caret_node),
            _ => Some(item.paintable),
        };
        slot.map_or(std::ptr::null_mut(), |slot| arena.shell_if_live(slot))
    }

    pub(crate) fn item_dispatch_shell(&self, arena: &LayoutNodeArena, item_index: usize) -> (*mut c_void, bool) {
        let item = &self.items[item_index];
        match item.kind {
            HitTestItemKind::TextFragment => (
                fragment_layout_node_slot(arena, item).map_or(std::ptr::null_mut(), |slot| arena.shell_if_live(slot)),
                true,
            ),
            HitTestItemKind::EmptyLine => (arena.shell_if_live(item.caret_node), false),
            _ => (event_dispatch_shell_for_paintable(arena, item.paintable), false),
        }
    }

    pub(crate) fn resolve_hit(
        &self,
        arena: &LayoutNodeArena,
        item_index: usize,
        local_point: CssPixelPoint,
    ) -> crate::painting::host::FfiResolvedHit {
        let item = &self.items[item_index];
        let (dispatch_shell, allow_pseudo_fallback) = self.item_dispatch_shell(arena, item_index);
        let mut result = crate::painting::host::FfiResolvedHit {
            dispatch_shell,
            allow_pseudo_fallback,
            ..Default::default()
        };
        match item.kind {
            HitTestItemKind::TextFragment => {
                result.fallback_dispatch_shell = event_dispatch_shell_for_paintable(arena, item.paintable);
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
        arena: &LayoutNodeArena,
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
                        fragment.dom_end_offset_with_trailing_whitespace
                    }
                    crate::painting::hit_test::caret::CaretPositionType::Closest => {
                        let paintable_rows = arena.paintable_rows();
                        crate::painting::text_fragment::index_in_node_for_point(&paintable_rows, fragment, local_point)
                    }
                };
                let node_shell = arena.shell_if_live(fragment.layout_node);
                let affinity_is_upstream = offset >= fragment.dom_end_offset_in_node
                    && offset == fragment.dom_end_offset_with_trailing_whitespace;
                let debug_rect = fragment_caret_range_rect(arena, fragment, offset);
                FfiResolvedCaret {
                    has_position: true,
                    node_shell,
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
                node_shell: arena.shell_if_live(item.caret_node),
                boundary: FfiCaretBoundaryKind::Offset,
                offset: item.caret_offset,
                has_debug_rect: true,
                debug_rect: item.caret_rect.into(),
                ..Default::default()
            },
            HitTestItemKind::EmptyEditable => FfiResolvedCaret {
                has_position: true,
                node_shell: arena.shell_if_live(item.paintable),
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
                    node_shell: arena.shell_if_live(item.paintable),
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
    arena: &LayoutNodeArena,
    item: &HitTestItem,
    f: impl FnOnce(&crate::painting::paintable_data::FragmentRecord) -> R,
) -> Option<R> {
    let fragment_index = item.text_fragment_index? as usize;
    arena
        .paintable_side_data(item.paintable)
        .fragments
        .get(fragment_index)
        .map(f)
}

pub(crate) fn fragment_layout_node_slot(arena: &LayoutNodeArena, item: &HitTestItem) -> Option<NodeSlotId> {
    with_item_fragment(arena, item, |fragment| fragment.layout_node)
}

pub(crate) fn event_dispatch_shell_for_paintable(arena: &LayoutNodeArena, slot: NodeSlotId) -> *mut c_void {
    let paintable_rows = arena.paintable_rows();
    let mut current = paintable_rows.paintable_row_is_populated(slot).then_some(slot);
    while let Some(paintable) = current {
        if arena.node_flags_if_live(paintable) & NodeFlag::Anonymous as u32 == 0 {
            return arena.shell_if_live(paintable);
        }
        current = crate::painting::paint_order::paint_parent(&paintable_rows, paintable);
    }
    std::ptr::null_mut()
}

pub(crate) fn fragment_index_in_node_for_point(
    arena: &LayoutNodeArena,
    item: &HitTestItem,
    local_point: CssPixelPoint,
) -> usize {
    with_item_fragment(arena, item, |fragment| {
        let paintable_rows = arena.paintable_rows();
        crate::painting::text_fragment::index_in_node_for_point(&paintable_rows, fragment, local_point)
    })
    .unwrap_or(0)
}

fn fragment_caret_range_rect(
    arena: &LayoutNodeArena,
    fragment: &crate::painting::paintable_data::FragmentRecord,
    offset: usize,
) -> CssPixelRect {
    let paintable_rows = arena.paintable_rows();
    let Some(offsets) = crate::painting::text_fragment::compute_selection_offsets(
        &paintable_rows,
        fragment,
        SELECTION_STATE_START_AND_END,
        offset,
        offset,
    ) else {
        return CssPixelRect::default();
    };
    crate::painting::text_fragment::rect_for_selection_offsets(&paintable_rows, fragment, offsets, || {
        crate::painting::text_fragment::first_available_font(&paintable_rows, fragment)
    })
}
