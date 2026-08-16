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

pub(crate) fn kind_is_svg_box(kind: NodeKind) -> bool {
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


/// A pass-scoped lens over one node's facts. Pure classification reads the
/// arena NodeData directly; style-derived answers read the per-slot style
/// snapshot; content-derived answers read the kind-gated lazy fact stores.
/// Nothing is materialized per node, so every answer is as live as its source.
#[derive(Clone, Copy)]
pub(crate) struct NodeFacts<'pass> {
    callbacks: &'pass FfiLayoutFcCallbacks,
    node: Node,
}

impl<'pass> NodeFacts<'pass> {
    #[inline]
    pub(crate) fn new(callbacks: &'pass FfiLayoutFcCallbacks, node: Node) -> Self {
        Self { callbacks, node }
    }

    fn data(&self) -> &'pass NodeData {
        self.callbacks.node_data(self.node)
    }

    fn parent_data(&self) -> Option<&'pass NodeData> {
        let parent = self.data().parent;
        (!parent.is_invalid()).then(|| self.callbacks.node_data(parent))
    }

    fn style(&self) -> StyleValues<'pass> {
        StyleValues::for_node(self.callbacks, self.node)
    }

    fn computed_values_view_if_styled(&self) -> Option<ComputedValuesView<'pass>> {
        self.callbacks.computed_values_view_if_styled(self.node)
    }

    fn parent_computed_values_view_if_styled(&self) -> Option<ComputedValuesView<'pass>> {
        let parent = self.data().parent;
        if parent.is_invalid() {
            return None;
        }
        self.callbacks.computed_values_view_if_styled(parent)
    }

    fn replaced_content(&self) -> crate::layout::FfiReplacedContentFacts {
        let Some(facts) = self.callbacks.replaced_content_facts(self.node) else {
            // The kind check is cheap enough for release builds; the style
            // half of the enrollment predicate is debug-only because this
            // miss path is the steady state for ordinary boxes.
            assert!(
                !crate::layout::node_may_have_replaced_content_facts(self.data()),
                "replaced content facts were not synced to the arena before layout"
            );
            debug_assert!(
                !crate::layout::node_may_have_replaced_content_facts_including_size_containment(self.data()),
                "replaced content facts were not synced to the arena before layout"
            );
            return crate::layout::FfiReplacedContentFacts::default();
        };
        facts
    }

    pub(crate) fn is_text_node(&self) -> bool {
        crate::layout::kind_is_text(self.data().kind)
    }

    pub(crate) fn is_break_node(&self) -> bool {
        self.data().kind == NodeKind::BreakNode
    }

    pub(crate) fn is_box(&self) -> bool {
        crate::layout::kind_is_box(self.data().kind)
    }

    pub(crate) fn is_block_container(&self) -> bool {
        crate::layout::kind_is_block_container(self.data().kind)
    }

    pub(crate) fn is_replaced_box(&self) -> bool {
        crate::layout::kind_is_replaced_box(self.data().kind)
    }

    pub(crate) fn is_replaced_box_with_children(&self) -> bool {
        let data = self.data();
        crate::layout::kind_is_replaced_box(data.kind) && crate::layout::node_can_have_children(data)
    }

    pub(crate) fn is_floating(&self) -> bool {
        self.computed_values_view_if_styled().is_some_and(|style| style.is_floating())
    }

    pub(crate) fn is_absolutely_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.is_absolutely_positioned())
    }

    pub(crate) fn is_relatively_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.position() == crate::css::css_enums::positioning::RELATIVE)
    }

    pub(crate) fn is_in_flow(&self) -> bool {
        !crate::layout::node_is_out_of_flow(self.data(), self.computed_values_view_if_styled())
    }

    pub(crate) fn is_floating_or_absolutely_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.is_floating() || style.is_absolutely_positioned())
    }

    pub(crate) fn is_inline(&self) -> bool {
        crate::layout::kind_is_text(self.data().kind)
            || self
                .computed_values_view_if_styled()
                .is_some_and(|style| style.display().is_inline_outside())
    }

    pub(crate) fn is_atomic_inline(&self) -> bool {
        let data = self.data();
        crate::layout::has_flag(data, NodeFlag::IsReplacedElement)
            || data.kind == NodeKind::ListItemMarkerBox
            || self.computed_values_view_if_styled().is_some_and(|style| {
                let display = style.display();
                display.is_inline_outside() && !display.is_flow_inside()
            })
    }

    pub(crate) fn has_box_model_metrics(&self) -> bool {
        !matches!(
            self.data().kind,
            NodeKind::Unset
                | NodeKind::Node
                | NodeKind::NodeWithStyle
                | NodeKind::GeneratedTextNode
                | NodeKind::TextNode
                | NodeKind::TextSliceNode
        )
    }

    pub(crate) fn is_fragmented_inline(&self) -> bool {
        let data = self.data();
        data.kind == NodeKind::InlineNode
            || (data.kind == NodeKind::ListItemBox
                && self.computed_values_view_if_styled().is_some_and(|style| {
                    let display = style.display();
                    display.is_inline_outside() && display.is_flow_inside()
                }))
    }

    /// A box whose committed geometry is its own single box record; a
    /// fragmented inline's committed geometry is its line pieces instead.
    pub(crate) fn is_non_fragmented_box(&self) -> bool {
        self.is_box() && !self.is_fragmented_inline()
    }

    pub(crate) fn is_inline_flow_interrupting_block(&self) -> bool {
        if !self.has_box_model_metrics() {
            return false;
        }
        let data = self.data();
        let Some(parent) = self.parent_data() else {
            return false;
        };
        let parent_is_inline_flow = self.parent_computed_values_view_if_styled().is_some_and(|style| {
            let display = style.display();
            display.is_inline_outside() && display.is_flow_inside()
        });
        if !parent_is_inline_flow {
            return false;
        }
        let style = self.computed_values_view_if_styled();
        if style.is_some_and(|style| style.display().is_inline_outside())
            || crate::layout::node_is_out_of_flow(data, style)
        {
            return false;
        }
        let display = self.display();
        if display.is_contents() {
            return false;
        }
        if display.is_internal_table() || display.is_table_caption() {
            return false;
        }
        if parent.kind == NodeKind::SVGForeignObjectBox {
            return false;
        }
        if crate::layout::kind_is_svg_box(data.kind) || data.kind == NodeKind::SVGForeignObjectBox {
            return false;
        }
        if data.kind == NodeKind::SVGSVGBox
            && (crate::layout::kind_is_svg_box(parent.kind) || parent.kind == NodeKind::SVGSVGBox)
        {
            return false;
        }
        if crate::layout::kind_is_replaced_box(parent.kind) && crate::layout::node_can_have_children(parent) {
            return false;
        }
        true
    }

    pub(crate) fn is_list_item_marker_box(&self) -> bool {
        self.data().kind == NodeKind::ListItemMarkerBox
    }

    pub(crate) fn is_list_item_box(&self) -> bool {
        self.data().kind == NodeKind::ListItemBox
    }

    pub(crate) fn is_svg_mask_box(&self) -> bool {
        self.data().kind == NodeKind::SVGMaskBox
    }

    pub(crate) fn is_svg_clip_box(&self) -> bool {
        self.data().kind == NodeKind::SVGClipBox
    }

    pub(crate) fn is_flow_layout_participant(&self) -> bool {
        self.is_box()
            && !self.is_absolutely_positioned()
            && !self.is_list_item_marker_box()
            && !self.is_svg_mask_box()
            && !self.is_svg_clip_box()
    }

    pub(crate) fn display_before_box_type_transformation_is_block_outside(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.display_before_box_type_transformation().is_block_outside())
    }

    pub(crate) fn inline_axis_is_reverse(&self) -> bool {
        let style = self.style();
        match style.writing_mode() {
            writing_mode::HORIZONTAL_TB
            | writing_mode::VERTICAL_RL
            | writing_mode::VERTICAL_LR
            | writing_mode::SIDEWAYS_RL => style.direction() == 1,
            writing_mode::SIDEWAYS_LR => style.direction() == 0,
            _ => unreachable!("invalid writing mode"),
        }
    }

    pub(crate) fn has_dom_node(&self) -> bool {
        !crate::layout::has_flag(self.data(), NodeFlag::Anonymous)
    }

    pub(crate) fn is_generated_for_pseudo_element(&self) -> bool {
        self.data().generated_for != 0
    }

    pub(crate) fn children_are_inline(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::ChildrenAreInline)
    }

    pub(crate) fn is_anonymous(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::Anonymous)
    }

    pub(crate) fn has_anchor_names(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::HasAnchorNames)
    }

    pub(crate) fn can_have_children(&self) -> bool {
        crate::layout::node_can_have_children(self.data())
    }

    pub(crate) fn has_replaced_element_table_display_adjustment(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsReplacedElement)
            && self
                .computed_values_view_if_styled()
                .is_some_and(|style| {
                    let display = style.display_before_box_type_transformation();
                    display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
                })
    }

    pub(crate) fn creates_block_formatting_context(&self) -> bool {
        crate::layout::node_creates_block_formatting_context(
            self.data(),
            self.computed_values_view_if_styled(),
            self.parent_computed_values_view_if_styled(),
        )
    }

    pub(crate) fn is_editing_host(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsEditingHost)
    }

    pub(crate) fn produces_line_box_fragment_when_empty(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::ProducesLineBoxFragmentWhenEmpty)
    }

    pub(crate) fn uses_button_layout(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::UsesButtonLayout)
    }

    pub(crate) fn vertical_align_applies(&self) -> bool {
        let data = self.data();
        crate::layout::kind_is_box(data.kind)
            && !crate::layout::has_flag(data, NodeFlag::IsFlexItem)
            && !crate::layout::has_flag(data, NodeFlag::IsGridItem)
    }

    pub(crate) fn is_html_input_element(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsHtmlInputElement)
    }

    pub(crate) fn is_fieldset_box(&self) -> bool {
        self.data().kind == NodeKind::FieldSetBox
    }

    pub(crate) fn rendered_legend(&self) -> Node {
        let data = self.data();
        if data.kind != NodeKind::FieldSetBox {
            return Node::INVALID;
        }
        let mut child = data.first_child;
        while !child.is_invalid() {
            let child_data = self.callbacks.node_data(child);
            if child_data.kind == NodeKind::LegendBox
                && !crate::layout::node_is_out_of_flow(child_data, self.callbacks.computed_values_view_if_styled(child))
            {
                return child;
            }
            child = child_data.next_sibling;
        }
        Node::INVALID
    }

    pub(crate) fn list_item_marker(&self) -> Node {
        if !self.is_list_item_box() {
            return Node::INVALID;
        }
        // Outside markers are direct children. Inside markers participate in
        // an inline run and can therefore move below anonymous block wrappers.
        // Walk those wrappers, but not nested authored or generated list items.
        let mut candidate = self.data().first_child;
        while !candidate.is_invalid() {
            let data = self.callbacks.node_data(candidate);
            if data.kind == NodeKind::ListItemMarkerBox {
                return candidate;
            }
            if data.kind == NodeKind::BlockContainer
                && crate::layout::has_flag(data, NodeFlag::Anonymous)
                && data.generated_for == 0
                && !data.first_child.is_invalid()
            {
                candidate = data.first_child;
                continue;
            }
            let mut current_data = data;
            loop {
                if !current_data.next_sibling.is_invalid() {
                    candidate = current_data.next_sibling;
                    break;
                }
                if current_data.parent == self.node {
                    return Node::INVALID;
                }
                current_data = self.callbacks.node_data(current_data.parent);
            }
        }
        Node::INVALID
    }

    pub(crate) fn list_marker_is_inside(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::ListMarkerIsInside)
    }

    pub(crate) fn has_auto_content_width(&self) -> bool {
        self.replaced_content().has_auto_content_width
    }

    pub(crate) fn auto_content_width(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_width
    }

    pub(crate) fn has_auto_content_height(&self) -> bool {
        self.replaced_content().has_auto_content_height
    }

    pub(crate) fn auto_content_height(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_height
    }

    pub(crate) fn has_auto_content_aspect_ratio(&self) -> bool {
        self.replaced_content().auto_content_aspect_ratio_denominator != crate::layout::CssPixels::default()
    }

    pub(crate) fn auto_content_aspect_ratio_numerator(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_aspect_ratio_numerator
    }

    pub(crate) fn auto_content_aspect_ratio_denominator(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_aspect_ratio_denominator
    }

    pub(crate) fn has_auto_content_box_size(&self) -> bool {
        crate::layout::node_has_auto_content_box_size(self.data())
    }

    pub(crate) fn node_has_size_containment(&self) -> bool {
        let display = self.display();
        if display.is_table_inside() || display.is_internal_table() {
            return false;
        }
        let style = self.style();
        style.has_size_containment() || style.is_size_container()
    }

    pub(crate) fn has_preferred_aspect_ratio(&self) -> bool {
        self.preferred_aspect_ratio().is_some()
    }

    pub(crate) fn preferred_aspect_ratio(&self) -> Option<PixelFraction> {
        let style = self.style();
        if !self.node_has_size_containment() && style.aspect_ratio_uses_natural_when_available() {
            let replaced = self.replaced_content();
            if replaced.auto_content_aspect_ratio_denominator != crate::layout::CssPixels::default() {
                return Some(PixelFraction {
                    numerator: replaced.auto_content_aspect_ratio_numerator,
                    denominator: replaced.auto_content_aspect_ratio_denominator,
                });
            }
        }
        let (numerator, denominator) = style.css_preferred_aspect_ratio();
        (denominator != crate::layout::CssPixels::default()).then_some(PixelFraction { numerator, denominator })
    }

    pub(crate) fn has_default_preferred_width(&self) -> bool {
        self.replaced_content().has_default_preferred_width
    }

    pub(crate) fn default_preferred_width(&self) -> crate::layout::CssPixels {
        self.replaced_content().default_preferred_width
    }

    pub(crate) fn has_default_preferred_height(&self) -> bool {
        self.replaced_content().has_default_preferred_height
    }

    pub(crate) fn default_preferred_height(&self) -> crate::layout::CssPixels {
        self.replaced_content().default_preferred_height
    }

    pub(crate) fn initial_containing_block_inline_size(&self) -> crate::layout::CssPixels {
        self.callbacks.initial_containing_block_inline_size
    }

    pub(crate) fn is_scroll_container(&self) -> bool {
        if self.data().kind == NodeKind::Viewport {
            return true;
        }
        let style = self.style();
        let overflow_value_makes_box_a_scroll_container =
            |overflow: u8| matches!(overflow, overflow::AUTO | overflow::HIDDEN | overflow::SCROLL);
        overflow_value_makes_box_a_scroll_container(style.overflow_x())
            || overflow_value_makes_box_a_scroll_container(style.overflow_y())
    }

    pub(crate) fn display(&self) -> crate::layout::FfiDisplay {
        if self.data().style.is_null() {
            return crate::layout::FfiDisplay::block();
        }
        self.style().display()
    }

    pub(crate) fn is_svg_box(&self) -> bool {
        crate::layout::kind_is_svg_box(self.data().kind)
    }

    pub(crate) fn is_svg_svg_box(&self) -> bool {
        self.data().kind == NodeKind::SVGSVGBox
    }

    pub(crate) fn is_table_box(&self) -> bool {
        self.display().is_table_inside()
    }

    pub(crate) fn is_table_wrapper(&self) -> bool {
        self.data().kind == NodeKind::TableWrapper
    }

    pub(crate) fn is_table_row_group(&self) -> bool {
        self.display().is_table_row_group()
    }

    pub(crate) fn is_table_header_group(&self) -> bool {
        self.display().is_table_header_group()
    }

    pub(crate) fn is_table_footer_group(&self) -> bool {
        self.display().is_table_footer_group()
    }

    pub(crate) fn is_table_row_group_kind(&self) -> bool {
        self.display().is_table_row_group_kind()
    }

    pub(crate) fn is_table_row(&self) -> bool {
        self.display().is_table_row()
    }

    pub(crate) fn is_table_cell(&self) -> bool {
        self.display().is_table_cell()
    }

    pub(crate) fn is_table_column_group(&self) -> bool {
        self.display().is_table_column_group()
    }

    pub(crate) fn is_table_column(&self) -> bool {
        self.display().is_table_column()
    }

    pub(crate) fn is_table_caption(&self) -> bool {
        self.display().is_table_caption()
    }

    pub(crate) fn is_viewport(&self) -> bool {
        self.data().kind == NodeKind::Viewport
    }

    pub(crate) fn document_in_quirks_mode(&self) -> bool {
        self.callbacks.document_in_quirks_mode
    }

    pub(crate) fn is_in_user_agent_shadow_tree(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsInUserAgentShadowTree)
    }

    pub(crate) fn is_html_html_element(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsHtmlHtmlElement)
    }

    pub(crate) fn is_html_body_element(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsBody)
    }
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
