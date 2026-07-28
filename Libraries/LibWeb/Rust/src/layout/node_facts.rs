/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Content-derived sizing data that genuinely lives on the C++ side (image
/// decode state, media metadata, font-resolved default control sizes). Fetched
/// lazily once per pass, and only for node kinds that can produce any of it.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiReplacedContentFacts {
    pub has_auto_content_width: bool,
    pub auto_content_width: CssPixels,
    pub has_auto_content_height: bool,
    pub auto_content_height: CssPixels,
    /// A zero denominator means the auto content box size carries no aspect
    /// ratio; a valid ratio can never have a zero denominator.
    pub auto_content_aspect_ratio_numerator: CssPixels,
    pub auto_content_aspect_ratio_denominator: CssPixels,
    pub has_default_preferred_width: bool,
    pub default_preferred_width: CssPixels,
    pub has_default_preferred_height: bool,
    pub default_preferred_height: CssPixels,
}

/// Marker linkage and font-measured marker metrics, fetched lazily once per
/// pass for list items and their marker boxes only. The marker slot id is
/// read from the C++ WeakPtr at fetch time, so a marker detached before the
/// pass reads as INVALID instead of tripping a stale-slot assertion.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiListItemFacts {
    pub marker: NodeSlotId,
    pub has_css_marker_content: bool,
    pub marker_content_inline_size: CssPixels,
    pub marker_content_block_size: CssPixels,
    pub marker_distance: CssPixels,
    pub marker_list_style_position: u8,
}

impl Default for FfiListItemFacts {
    fn default() -> Self {
        Self {
            marker: NodeSlotId::INVALID,
            has_css_marker_content: false,
            marker_content_inline_size: CssPixels::default(),
            marker_content_block_size: CssPixels::default(),
            marker_distance: CssPixels::default(),
            marker_list_style_position: 0,
        }
    }
}

pub(crate) fn node_may_have_replaced_content_facts(data: &NodeData) -> bool {
    kind_is_replaced_box(data.kind)
        || matches!(
            data.kind,
            NodeKind::RangeInputBox | NodeKind::TextAreaBox | NodeKind::TextInputBox
        )
        || has_flag(data, NodeFlag::IsHtmlInputElement)
}

pub(crate) fn node_may_have_list_item_facts(data: &NodeData) -> bool {
    matches!(data.kind, NodeKind::ListItemBox | NodeKind::ListItemMarkerBox)
}

pub(crate) fn node_is_out_of_flow(data: &NodeData) -> bool {
    (has_display_flag(data, NodeDisplayFlag::Floating) && !has_flag(data, NodeFlag::IsFlexItem))
        || has_display_flag(data, NodeDisplayFlag::AbsolutelyPositioned)
}

pub(crate) fn node_can_have_children(data: &NodeData) -> bool {
    match data.kind {
        NodeKind::BreakNode | NodeKind::ListItemMarkerBox => false,
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
// The computed-style-only terms live in the stamped
// OwnStyleEstablishesBlockFormattingContext flag; this composite adds the terms
// that need the node kind, stamped DOM identity, the live IsFlexItem flag, or
// the parent's display.
pub(crate) fn node_creates_block_formatting_context(data: &NodeData, parent_data: Option<&NodeData>) -> bool {
    if kind_is_replaced_box(data.kind) {
        return false;
    }
    if data.kind == NodeKind::SVGForeignObjectBox {
        return true;
    }
    if data.table_display == FfiTableDisplay::TableRoot
        || has_display_flag(data, NodeDisplayFlag::FlexInside)
        || has_display_flag(data, NodeDisplayFlag::GridInside)
    {
        return false;
    }
    if has_flag(data, NodeFlag::IsHtmlHtmlElement)
        || (has_display_flag(data, NodeDisplayFlag::Floating) && !has_flag(data, NodeFlag::IsFlexItem))
        || has_flag(data, NodeFlag::OwnStyleEstablishesBlockFormattingContext)
        || data.kind == NodeKind::FieldSetBox
        || has_flag(data, NodeFlag::UsesButtonLayout)
    {
        return true;
    }
    parent_data.is_some_and(|parent| {
        has_display_flag(parent, NodeDisplayFlag::FlexInside) || has_display_flag(parent, NodeDisplayFlag::GridInside)
    })
}

pub(crate) fn has_flag(data: &NodeData, flag: NodeFlag) -> bool {
    data.flags & flag as u32 != 0
}

pub(crate) fn has_display_flag(data: &NodeData, flag: NodeDisplayFlag) -> bool {
    data.display_bits & flag as u8 != 0
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
    use crate::layout::node_data::{NodeData, NodeDisplayFlag, NodeFlag, NodeKind};
    use crate::layout::{node_can_have_children, node_creates_block_formatting_context};

    fn data_with_kind(kind: NodeKind) -> NodeData {
        NodeData {
            kind,
            ..NodeData::default()
        }
    }

    #[test]
    fn child_policy_follows_kind_and_replaced_shadow_root_flag() {
        assert!(!node_can_have_children(&data_with_kind(NodeKind::BreakNode)));
        assert!(!node_can_have_children(&data_with_kind(NodeKind::ListItemMarkerBox)));
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

    #[test]
    fn block_formatting_context_composite_matches_the_retired_predicate() {
        let mut replaced = data_with_kind(NodeKind::ImageBox);
        replaced.flags = NodeFlag::OwnStyleEstablishesBlockFormattingContext as u32;
        assert!(!node_creates_block_formatting_context(&replaced, None));

        assert!(node_creates_block_formatting_context(
            &data_with_kind(NodeKind::SVGForeignObjectBox),
            None
        ));

        let mut table_root = data_with_kind(NodeKind::BlockContainer);
        table_root.table_display = crate::layout::node_data::FfiTableDisplay::TableRoot;
        table_root.flags = NodeFlag::OwnStyleEstablishesBlockFormattingContext as u32;
        assert!(!node_creates_block_formatting_context(&table_root, None));

        let mut flex_container = data_with_kind(NodeKind::BlockContainer);
        flex_container.display_bits = NodeDisplayFlag::FlexInside as u8;
        flex_container.flags = NodeFlag::OwnStyleEstablishesBlockFormattingContext as u32;
        assert!(!node_creates_block_formatting_context(&flex_container, None));

        let mut root_element = data_with_kind(NodeKind::BlockContainer);
        root_element.flags = NodeFlag::IsHtmlHtmlElement as u32;
        assert!(node_creates_block_formatting_context(&root_element, None));

        let mut floated = data_with_kind(NodeKind::BlockContainer);
        floated.display_bits = NodeDisplayFlag::Floating as u8;
        assert!(node_creates_block_formatting_context(&floated, None));
        floated.flags = NodeFlag::IsFlexItem as u32;
        assert!(!node_creates_block_formatting_context(&floated, None));

        assert!(node_creates_block_formatting_context(
            &data_with_kind(NodeKind::FieldSetBox),
            None
        ));

        let mut button = data_with_kind(NodeKind::BlockContainer);
        button.flags = NodeFlag::UsesButtonLayout as u32;
        assert!(node_creates_block_formatting_context(&button, None));

        let mut own_style = data_with_kind(NodeKind::BlockContainer);
        own_style.flags = NodeFlag::OwnStyleEstablishesBlockFormattingContext as u32;
        assert!(node_creates_block_formatting_context(&own_style, None));

        let plain = data_with_kind(NodeKind::BlockContainer);
        let mut grid_parent = data_with_kind(NodeKind::BlockContainer);
        grid_parent.display_bits = NodeDisplayFlag::GridInside as u8;
        assert!(!node_creates_block_formatting_context(&plain, None));
        assert!(!node_creates_block_formatting_context(
            &plain,
            Some(&data_with_kind(NodeKind::BlockContainer))
        ));
        assert!(node_creates_block_formatting_context(&plain, Some(&grid_parent)));
    }
}
