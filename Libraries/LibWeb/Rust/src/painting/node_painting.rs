/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts;

pub(crate) const fn has_paintable(kind: NodeKind) -> bool {
    !matches!(
        kind,
        NodeKind::Unset
            | NodeKind::BreakNode
            | NodeKind::GeneratedTextNode
            | NodeKind::Node
            | NodeKind::NodeWithStyle
            | NodeKind::TextNode
            | NodeKind::TextSliceNode
    )
}

pub(crate) fn is_fragmented_inline(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    arena
        .node_data_if_live(node)
        .is_some_and(|data| node_facts::node_is_fragmented_inline(data, arena.node_style_if_live(node)))
}

pub(crate) fn is_inline(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    is_fragmented_inline(arena, node)
}

pub(crate) fn has_lines(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let Some(kind) = arena.node_kind_if_live(node) else {
        return false;
    };
    match kind {
        NodeKind::Viewport
        | NodeKind::BlockContainer
        | NodeKind::LegendBox
        | NodeKind::TableWrapper
        | NodeKind::TextAreaBox
        | NodeKind::TextInputBox
        | NodeKind::RangeInputBox
        | NodeKind::ListItemMarkerBox
        | NodeKind::SVGForeignObjectBox => true,
        NodeKind::ListItemBox => !is_fragmented_inline(arena, node),
        _ => false,
    }
}

pub(crate) const fn is_svg(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGGraphicsBox
            | NodeKind::SVGGeometryBox
            | NodeKind::SVGTextBox
            | NodeKind::SVGTextPathBox
            | NodeKind::SVGImageBox
            | NodeKind::SVGMaskBox
            | NodeKind::SVGClipBox
            | NodeKind::SVGPatternBox
    )
}

pub(crate) const fn is_svg_path(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGGeometryBox | NodeKind::SVGTextBox | NodeKind::SVGTextPathBox
    )
}

pub(crate) const fn supports_svg_masking(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGGraphicsBox
            | NodeKind::SVGGeometryBox
            | NodeKind::SVGTextBox
            | NodeKind::SVGTextPathBox
            | NodeKind::SVGImageBox
            | NodeKind::SVGMaskBox
            | NodeKind::SVGForeignObjectBox
    )
}

pub(crate) const fn forms_unconnected_subtree(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGMaskBox | NodeKind::SVGClipBox | NodeKind::SVGPatternBox
    )
}

pub(crate) const fn foreground_is_never_cached(kind: NodeKind) -> bool {
    matches!(kind, NodeKind::NavigableContainerViewport)
}

pub(crate) fn paints_box_decorations(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let Some(kind) = arena.node_kind_if_live(node) else {
        return false;
    };
    !is_inline(arena, node) && !is_svg_path(kind) && kind != NodeKind::SVGImageBox
}
