/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paint_read::{GeometryRead, PaintRow};

pub(crate) const fn has_paintable(kind: NodeKind) -> bool {
    !matches!(
        kind,
        NodeKind::Unset
            | NodeKind::BreakNode
            | NodeKind::GeneratedTextNode
            | NodeKind::Node
            | NodeKind::NodeWithStyle
            | NodeKind::TextNode
    )
}

pub(crate) fn is_fragmented_inline(arena: &impl GeometryRead, node: NodeSlotId) -> bool {
    arena.node(node).is_some_and(PaintRow::is_fragmented_inline)
}

pub(crate) fn has_lines(arena: &impl GeometryRead, node: NodeSlotId) -> bool {
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

pub(crate) const fn is_svg_paintable(kind: NodeKind) -> bool {
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
