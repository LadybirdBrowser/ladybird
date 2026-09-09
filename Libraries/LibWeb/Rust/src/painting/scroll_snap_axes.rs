/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{scroll_snap_axis, scroll_snap_strictness, writing_mode};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::painting::host::FfiSnapAxes;

pub(crate) fn snap_axes_of_scroll_container(arena: &LayoutNodeArena, snap_container: NodeSlotId) -> FfiSnapAxes {
    let style_source = if arena.node_kind_if_live(snap_container) == Some(NodeKind::Viewport) {
        document_element_box_under(arena, snap_container)
    } else {
        Some(snap_container)
    };
    let Some(style) = style_source.and_then(|slot| arena.node_style_if_live(slot)) else {
        return FfiSnapAxes::default();
    };
    let snap_type = style.misc_reset();
    if snap_type.scroll_snap_strictness == scroll_snap_strictness::NONE {
        return FfiSnapAxes::default();
    }
    let horizontal_writing_mode = style.writing_mode() == writing_mode::HORIZONTAL_TB;
    match snap_type.scroll_snap_axis {
        scroll_snap_axis::X => FfiSnapAxes { x: true, y: false },
        scroll_snap_axis::Y => FfiSnapAxes { x: false, y: true },
        scroll_snap_axis::INLINE => FfiSnapAxes {
            x: horizontal_writing_mode,
            y: !horizontal_writing_mode,
        },
        scroll_snap_axis::BLOCK => FfiSnapAxes {
            x: !horizontal_writing_mode,
            y: horizontal_writing_mode,
        },
        _ => FfiSnapAxes { x: true, y: true },
    }
}

fn document_element_box_under(arena: &LayoutNodeArena, parent: NodeSlotId) -> Option<NodeSlotId> {
    let mut child = arena.data(parent).first_child.get();
    while !child.is_invalid() {
        let flags = arena.node_flags_if_live(child);
        if flags & NodeFlag::IsDocumentElement as u32 != 0 {
            return Some(child);
        }
        if flags & NodeFlag::Anonymous as u32 != 0
            && let Some(found) = document_element_box_under(arena, child)
        {
            return Some(found);
        }
        child = arena.data(child).next_sibling.get();
    }
    None
}
