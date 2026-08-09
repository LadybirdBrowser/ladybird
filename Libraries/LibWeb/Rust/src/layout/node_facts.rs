/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub(crate) fn node_may_have_replaced_content_facts(data: &NodeData) -> bool {
    kind_is_replaced_box(data.kind)
        || matches!(
            data.kind,
            NodeKind::RangeInputBox | NodeKind::TextAreaBox | NodeKind::TextInputBox
        )
        || has_flag(data, NodeFlag::IsHtmlInputElement)
}

// Size containment gives any box an auto content box size of zero, so
// size-contained boxes join the replaced kinds in publishing real facts.
// C++ enrollment and the Rust sync assertion both evaluate this predicate.
pub(crate) fn node_may_have_replaced_content_facts_including_size_containment(data: &NodeData) -> bool {
    if node_may_have_replaced_content_facts(data) {
        return true;
    }
    if !kind_is_box(data.kind) || data.style.is_null() {
        return false;
    }
    // SAFETY: A non-null style pointer addresses the container's group
    // pointer array, which FfiStylePayloads mirrors exactly.
    let payloads = unsafe { &*data.style.cast::<crate::layout::FfiStylePayloads>() };
    let style = ComputedValuesView::new(&payloads.groups);
    style.has_size_containment() || style.is_size_container()
}

pub(crate) fn node_is_out_of_flow(data: &NodeData, style: Option<ComputedValuesView<'_>>) -> bool {
    let Some(style) = style else {
        return false;
    };
    (style.is_floating() && !has_flag(data, NodeFlag::IsFlexItem)) || style.is_absolutely_positioned()
}

pub(crate) fn node_can_have_children(data: &NodeData) -> bool {
    match data.kind {
        NodeKind::BreakNode => false,
        NodeKind::AudioBox | NodeKind::VideoBox => has_flag(data, NodeFlag::ReplacedBoxCanHaveChildren),
        NodeKind::SVGSVGBox => true,
        kind if kind_is_replaced_box(kind) => false,
        _ => true,
    }
}

pub(crate) fn node_has_auto_content_box_size(data: &NodeData) -> bool {
    (kind_is_replaced_box(data.kind) && data.kind != NodeKind::AudioBox)
        || matches!(
            data.kind,
            NodeKind::RangeInputBox | NodeKind::TextAreaBox | NodeKind::TextInputBox
        )
}

// https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
// ComputedValuesView::own_style_establishes_block_formatting_context covers
// computed-style-only terms; this composite adds the terms that need the node
// kind, stamped DOM identity, the live IsFlexItem flag, or the parent's
// display.
pub(crate) fn node_creates_block_formatting_context(
    data: &NodeData,
    style: Option<ComputedValuesView<'_>>,
    parent_style: Option<ComputedValuesView<'_>>,
) -> bool {
    if kind_is_replaced_box(data.kind) {
        return false;
    }
    if data.kind == NodeKind::SVGForeignObjectBox {
        return true;
    }
    if let Some(style) = style {
        let display = style.display();
        if display.is_table_inside() || display.is_flex_inside() || display.is_grid_inside() {
            return false;
        }
        if (style.is_floating() && !has_flag(data, NodeFlag::IsFlexItem))
            || style.own_style_establishes_block_formatting_context()
        {
            return true;
        }
    }
    if has_flag(data, NodeFlag::IsHtmlHtmlElement)
        || data.kind == NodeKind::FieldSetBox
        // https://drafts.csswg.org/css-lists-3/#list-style-position-outside
        // "If the list item is a block container: the marker box is a block container"
        || data.kind == NodeKind::ListItemMarkerBox
        || has_flag(data, NodeFlag::UsesButtonLayout)
    {
        return true;
    }
    parent_style.is_some_and(|parent| parent.display().is_flex_inside() || parent.display().is_grid_inside())
}

pub(crate) fn has_flag(data: &NodeData, flag: NodeFlag) -> bool {
    data.flags & flag as u32 != 0
}

pub(crate) fn kind_is_text(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::GeneratedTextNode | NodeKind::TextNode | NodeKind::TextSliceNode
    )
}

pub(crate) fn kind_is_box(kind: NodeKind) -> bool {
    !matches!(
        kind,
        NodeKind::Unset
            | NodeKind::BreakNode
            | NodeKind::InlineNode
            | NodeKind::Node
            | NodeKind::NodeWithStyle
            | NodeKind::NodeWithStyleAndBoxModelMetrics
            | NodeKind::GeneratedTextNode
            | NodeKind::TextNode
            | NodeKind::TextSliceNode
    )
}

pub(crate) fn kind_is_block_container(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::BlockContainer
            | NodeKind::FieldSetBox
            | NodeKind::LegendBox
            | NodeKind::ListItemBox
            | NodeKind::ListItemMarkerBox
            | NodeKind::RangeInputBox
            | NodeKind::SVGForeignObjectBox
            | NodeKind::TableWrapper
            | NodeKind::TextAreaBox
            | NodeKind::TextInputBox
            | NodeKind::Viewport
    )
}

pub(crate) fn kind_is_replaced_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::AudioBox
            | NodeKind::CanvasBox
            | NodeKind::CheckBox
            | NodeKind::ImageBox
            | NodeKind::NavigableContainerViewport
            | NodeKind::RadioButton
            | NodeKind::ReplacedBox
            | NodeKind::SVGSVGBox
            | NodeKind::VideoBox
    )
}

fn kind_is_svg_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGBox
            | NodeKind::SVGClipBox
            | NodeKind::SVGGeometryBox
            | NodeKind::SVGGraphicsBox
            | NodeKind::SVGImageBox
            | NodeKind::SVGMaskBox
            | NodeKind::SVGPatternBox
            | NodeKind::SVGTextBox
            | NodeKind::SVGTextPathBox
    )
}

#[cfg(test)]
mod node_facts_tests {
    use crate::layout::node_data::{NodeData, NodeFlag, NodeKind};
    use crate::layout::node_can_have_children;

    fn data_with_kind(kind: NodeKind) -> NodeData {
        NodeData {
            kind,
            ..NodeData::default()
        }
    }

    #[test]
    fn child_policy_follows_kind_and_replaced_shadow_root_flag() {
        assert!(!node_can_have_children(&data_with_kind(NodeKind::BreakNode)));
        assert!(node_can_have_children(&data_with_kind(NodeKind::ListItemMarkerBox)));
        assert!(!node_can_have_children(&data_with_kind(NodeKind::ImageBox)));
        assert!(!node_can_have_children(&data_with_kind(NodeKind::ReplacedBox)));
        assert!(!node_can_have_children(&data_with_kind(NodeKind::NavigableContainerViewport)));
        assert!(node_can_have_children(&data_with_kind(NodeKind::SVGSVGBox)));
        assert!(node_can_have_children(&data_with_kind(NodeKind::BlockContainer)));
        assert!(node_can_have_children(&data_with_kind(NodeKind::InlineNode)));
        assert!(node_can_have_children(&data_with_kind(NodeKind::TextNode)));

        let mut media = data_with_kind(NodeKind::AudioBox);
        assert!(!node_can_have_children(&media));
        media.flags = NodeFlag::ReplacedBoxCanHaveChildren as u32;
        assert!(node_can_have_children(&media));
        media.kind = NodeKind::VideoBox;
        assert!(node_can_have_children(&media));
    }
}
