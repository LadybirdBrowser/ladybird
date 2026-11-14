/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where a planned box goes among the children of its parent, and the anonymous block boxes that keep the in-flow
//! children of a block container all block-level or all inline-level.
//! https://drafts.csswg.org/css-display-3/#anonymous
//!
//! Each child list keeps counts of its inline-level and in-flow children, so the questions the rules ask of a parent's children
//! take constant time.

use super::{
    OneByOneRowSource, PlanNode, PlanTree, PlannedBox, RowCommittedOneByOne, anonymous_wrapper_facts, box_fact,
};
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::layout::node_facts;
use libcompositing_rust::node_slot_id::NodeSlotId;

/// Where a box goes among the children of its insertion parent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ChildPosition {
    /// After the children the parent has.
    Append,
    /// Before them, as a `::before` box or an inside marker goes.
    Prepend,
}

impl PlannedBox<'_> {
    /// Whether the box is SVG content, see [`node_facts::kind_is_svg_content`].
    pub(crate) fn is_svg_content(&self) -> bool {
        node_facts::kind_is_svg_content(self.kind())
    }

    fn is_inline_flow(&self) -> bool {
        self.has_fact(box_fact::INLINE_OUTSIDE) && self.has_fact(box_fact::FLOW_INSIDE)
    }

    fn lays_out_each_child_as_its_own_box(&self) -> bool {
        // NB: Flex, grid and MathML layout lay out each of their children as its own box.
        self.has_fact(box_fact::FLEX_OR_GRID_INSIDE | box_fact::MATH_INSIDE)
    }

    /// Whether the box is out of flow among the children of `parent`.
    fn is_out_of_flow_child_of(&self, parent: &PlannedBox<'_>) -> bool {
        if !self.is_out_of_flow() {
            return false;
        }
        // https://w3c.github.io/mathml-core/#css-styling
        // The float property does not create floating of elements whose parent's computed display value is block math or
        // inline math, and does not take them out-of-flow.
        !parent.has_fact(box_fact::MATH_INSIDE) || self.has_fact(box_fact::ABSOLUTELY_POSITIONED)
    }

    fn is_out_of_flow_table_internal_child_of_table_root(&self, parent: &PlannedBox<'_>) -> bool {
        parent.has_fact(box_fact::TABLE_INSIDE)
            && self.has_flag(NodeFlag::HasStyle)
            && !self.has_flag(NodeFlag::Anonymous)
            && self.is_out_of_flow()
            && !self.has_flag(NodeFlag::IsReplacedElement)
            && self.has_fact(box_fact::TABLE_NON_ROOT_BEFORE_BLOCKIFICATION)
    }

    /// Whether the box is an absolutely positioned box that would be inline-level in flow, laid out among the inline
    /// content of `parent`.
    fn is_inline_level_absolutely_positioned_box_in_flow_layout(&self, parent: &PlannedBox<'_>) -> bool {
        let parent_kind = parent.kind();
        crate::layout::tree_builder::node_kind_is_node_with_style(self.kind())
            && self.has_fact(box_fact::ABSOLUTELY_POSITIONED)
            && self.has_fact(box_fact::INLINE_OUTSIDE_BEFORE_BLOCKIFICATION)
            && !parent.lays_out_each_child_as_its_own_box()
            && !parent.has_fact(box_fact::TABULAR_CONTAINER)
            && !node_facts::kind_is_svg_box(parent_kind)
            && !matches!(parent_kind, NodeKind::SVGSVGBox | NodeKind::Viewport)
    }

    fn has_in_flow_block_children(&self) -> bool {
        !self.has_flag(NodeFlag::ChildrenAreInline) && self.has_fact(box_fact::HAS_IN_FLOW_BLOCK_CHILDREN)
    }

    fn has_inline_or_in_flow_block_children(&self) -> bool {
        self.has_fact(box_fact::HAS_INLINE_OR_IN_FLOW_CHILDREN)
    }
}

impl PlanTree {
    /// Whether `node` is out of flow among the children of `parent`.
    pub(crate) fn is_out_of_flow_child_of(&self, parent: NodeSlotId, node: NodeSlotId) -> bool {
        self.node(node).is_out_of_flow_child_of(&self.node(parent))
    }

    fn is_out_of_flow_table_internal_child_of_table_root(&self, parent: NodeSlotId, child: NodeSlotId) -> bool {
        self.node(child)
            .is_out_of_flow_table_internal_child_of_table_root(&self.node(parent))
    }

    /// Makes an anonymous block container under `parent`, before `before`, that takes its style from `parent`.
    pub(crate) fn create_anonymous_wrapper(&mut self, parent: NodeSlotId, before: NodeSlotId) -> NodeSlotId {
        let wrapper = self.push(PlanNode::new(
            NodeKind::BlockContainer,
            node_facts::construction_flags(NodeKind::BlockContainer, true, 0),
            anonymous_wrapper_facts(),
        ));
        self.attach(parent, wrapper, before);
        let facts = self.node(wrapper).facts();
        self.rows_committed_one_by_one.push(RowCommittedOneByOne {
            slot: wrapper,
            facts,
            source: OneByOneRowSource::AnonymousWrapper { style_parent: parent },
        });
        wrapper
    }

    /// The anonymous wrapper at the end of the parent's children that an inline-level node joins, or at their start if
    /// the node is being prepended (as an inside ::marker is) — creating one there if the child at that end isn't one.
    fn edge_child_creating_anonymous_wrapper_if_needed(
        &mut self,
        parent: NodeSlotId,
        position: ChildPosition,
    ) -> NodeSlotId {
        let (edge_child, insert_wrapper_before) = if position == ChildPosition::Prepend {
            let first_child = self.node(parent).first_child();
            (first_child, first_child)
        } else {
            (self.node(parent).last_child(), NodeSlotId::INVALID)
        };
        if edge_child.is_invalid() {
            return self.create_anonymous_wrapper(parent, insert_wrapper_before);
        }
        let edge = self.node(edge_child);
        if !edge.has_flag(NodeFlag::Anonymous)
            || !edge.has_flag(NodeFlag::ChildrenAreInline)
            || edge.generated_for() != 0
        {
            return self.create_anonymous_wrapper(parent, insert_wrapper_before);
        }
        edge_child
    }

    // The insertion_parent_for_*() functions maintain the invariant that the in-flow children of
    // block-level boxes must be either all block-level or all inline-level.
    fn insertion_parent_for_inline_node(&mut self, parent: NodeSlotId, position: ChildPosition) -> NodeSlotId {
        let parent_box = self.node(parent);
        let kind = parent_box.kind();
        if matches!(kind, NodeKind::FieldSetBox | NodeKind::SVGForeignObjectBox) {
            return self.edge_child_creating_anonymous_wrapper_if_needed(parent, position);
        }

        // SVG layout ignores the inline/block distinction, and an anonymous wrapper would only hide
        // the child from SVGFormattingContext (e.g. a shape with a foreignObject sibling).
        if parent_box.is_svg_content() {
            return parent;
        }

        if parent_box.is_inline_flow() {
            return parent;
        }

        if parent_box.has_fact(box_fact::FLEX_OR_GRID_INSIDE) {
            return self.edge_child_creating_anonymous_wrapper_if_needed(parent, position);
        }

        if !parent_box.has_in_flow_block_children() || parent_box.has_flag(NodeFlag::ChildrenAreInline) {
            return parent;
        }

        // Parent has block-level children, insert into an anonymous wrapper block (and create it first if needed)
        self.edge_child_creating_anonymous_wrapper_if_needed(parent, position)
    }

    fn insertion_parent_for_block_node(
        &mut self,
        parent: NodeSlotId,
        node: NodeSlotId,
        position: ChildPosition,
    ) -> NodeSlotId {
        // Inline is fine for in-flow block children (interrupting blocks) and for out-of-flow children;
        // the inline formatting context emits items for both, in order. That holds for a pseudo-element's box as for a
        // DOM child's: a block-level ::before or ::after interrupts its originating inline box where it is generated.
        let parent_box = self.node(parent);
        if parent_box.is_inline_flow() {
            return parent;
        }

        // SVG host ignores the inline/block distinction; wrapping existing inline-level siblings
        // (e.g. shapes next to a foreignObject) would only hide them from SVGFormattingContext.
        if parent_box.is_svg_content() {
            return parent;
        }

        // Make sure we're not inserting into an inline node, since those do not support block nodes.
        let mut new_parent = parent;
        let mut new_parent_box = parent_box;
        while new_parent_box.kind() == NodeKind::InlineNode {
            new_parent = new_parent_box.parent();
            assert!(
                !new_parent.is_invalid(),
                "an inline box the plan climbs out of has a planned parent"
            );
            new_parent_box = self.node(new_parent);
        }

        // If the parent block has no children, insert this block into parent.
        if !new_parent_box.has_inline_or_in_flow_block_children() {
            return new_parent;
        }

        // Table-internal boxes may have been blockified before insertion, but table fixup still needs to see them as
        // direct table children instead of grouping them with neighboring table whitespace.
        let node_box = self.node(node);
        if node_box.is_out_of_flow_table_internal_child_of_table_root(&new_parent_box) {
            return new_parent;
        }

        // If the block is out-of-flow,
        if node_box.is_out_of_flow_child_of(&new_parent_box) {
            let last_child = new_parent_box.last_child();
            assert!(!last_child.is_invalid());
            let last_child_data = self.node(last_child);

            // And we're appending while the parent's last child is an anonymous block, join that
            // anonymous block. Prepended boxes (e.g. an absolutely positioned ::before) belong at the
            // very start of the parent, not at the start of its trailing inline run.
            if position == ChildPosition::Append
                && !new_parent_box.lays_out_each_child_as_its_own_box()
                && last_child_data.kind() == NodeKind::BlockContainer
                && last_child_data.generated_for() == 0
                && last_child_data.has_flag(NodeFlag::Anonymous)
                && last_child_data.has_flag(NodeFlag::ChildrenAreInline)
            {
                return last_child;
            }

            // Otherwise, insert this block into parent.
            return new_parent;
        }

        // If the parent block has block-level children, insert this block into parent.
        if !new_parent_box.has_flag(NodeFlag::ChildrenAreInline) {
            return new_parent;
        }

        // Parent block has inline-level children (our siblings); wrap these siblings into an anonymous wrapper block.
        if new_parent_box.lays_out_each_child_as_its_own_box() {
            // NB: An out-of-flow child of a parent that lays out each of its children as its own box stays a child of
            //     that parent, so that it is positioned as the parent's own abspos child instead of becoming part of an
            //     anonymous wrapper. Each run of in-flow siblings between them gets its own wrapper, in DOM order.
            let mut child = self.node(new_parent).first_child();
            let mut run_start = NodeSlotId::INVALID;
            while !child.is_invalid() {
                let next = self.node(child).next_sibling();
                if self.is_out_of_flow_child_of(new_parent, child) {
                    if !run_start.is_invalid() {
                        self.wrap_run_in_anonymous_wrapper(new_parent, run_start, child);
                        run_start = NodeSlotId::INVALID;
                    }
                } else if run_start.is_invalid() {
                    run_start = child;
                }
                child = next;
            }
            if !run_start.is_invalid() {
                self.wrap_run_in_anonymous_wrapper(new_parent, run_start, NodeSlotId::INVALID);
            }
        } else {
            // NB: The wrapper goes after the children it leaves where they are, before which the children it takes
            //     were: then their pre-order positions are out of order.
            let mut child = self.node(new_parent).first_child();
            let mut keeps_children = false;
            while !child.is_invalid() && !keeps_children {
                keeps_children = self.is_out_of_flow_table_internal_child_of_table_root(new_parent, child);
                child = self.node(child).next_sibling();
            }
            let wrapper = if keeps_children {
                self.pre_order_positions_out_of_order = true;
                self.create_anonymous_wrapper(new_parent, NodeSlotId::INVALID)
            } else {
                let first_child = self.node(new_parent).first_child();
                self.create_anonymous_wrapper(new_parent, first_child)
            };
            self.set_flag(wrapper, NodeFlag::ChildrenAreInline, true);
            let mut child = self.node(new_parent).first_child();
            while !child.is_invalid() {
                let next = self.node(child).next_sibling();
                if child != wrapper && !self.is_out_of_flow_table_internal_child_of_table_root(new_parent, child) {
                    self.move_child(child, wrapper);
                }
                child = next;
            }
        }
        self.set_flag(new_parent, NodeFlag::ChildrenAreInline, false);

        // Then it's safe to insert this block into parent.
        new_parent
    }

    /// Wraps the children of `parent` from `first` up to `end` (or the last one) in a new anonymous wrapper, which goes
    /// where `first` was.
    fn wrap_run_in_anonymous_wrapper(&mut self, parent: NodeSlotId, first: NodeSlotId, end: NodeSlotId) {
        let wrapper = self.create_anonymous_wrapper(parent, first);
        self.set_flag(wrapper, NodeFlag::ChildrenAreInline, true);
        let mut child = first;
        while child != end {
            let next = self.node(child).next_sibling();
            self.move_child(child, wrapper);
            child = next;
        }
    }

    /// Makes an anonymous block container of the given display that the commit styles, to wrap the contents of a
    /// button or a fieldset, see [`super::OneByOneRowSource::ContentsWrapper`].
    fn push_contents_wrapper(&mut self, inside: u8) -> NodeSlotId {
        let display =
            crate::layout::FfiDisplay::outside_and_inside(crate::css::css_enums::display_outside::BLOCK, inside, false);
        self.push(PlanNode::new(
            NodeKind::BlockContainer,
            node_facts::construction_flags(NodeKind::BlockContainer, true, 0),
            super::box_facts_of_display(display) | box_fact::HAS_STYLE,
        ))
    }

    /// Moves every child of `parent` but `except` to the end of the children of `new_parent`, in order.
    fn move_children(&mut self, parent: NodeSlotId, new_parent: NodeSlotId, except: NodeSlotId) {
        let mut child = self.node(parent).first_child();
        while !child.is_invalid() {
            let next = self.node(child).next_sibling();
            if child != except && child != new_parent {
                self.move_child(child, new_parent);
            }
            child = next;
        }
    }

    /// Wraps the children of `button`, a box that uses button layout and whose display is neither flex nor grid, in an
    /// anonymous flex wrapper holding an anonymous button content box, and answers both.
    pub(crate) fn wrap_button_contents(&mut self, button: NodeSlotId) -> (NodeSlotId, NodeSlotId) {
        let button_box = self.node(button);
        let children_are_inline = button_box.has_flag(NodeFlag::ChildrenAreInline);
        let first_child = button_box.first_child();
        // NB: The wrappers go before the children they take, so the pre-order positions of all of them stay in order.
        let flex_wrapper = self.push_contents_wrapper(crate::css::css_enums::display_inside::FLEX);
        self.attach(button, flex_wrapper, first_child);
        self.derive_subtree_on_commit(flex_wrapper);
        let content_box = self.push_contents_wrapper(crate::css::css_enums::display_inside::FLOW);
        self.attach(flex_wrapper, content_box, NodeSlotId::INVALID);
        self.set_flag(content_box, NodeFlag::ChildrenAreInline, children_are_inline);
        self.move_children(button, content_box, flex_wrapper);
        self.set_flag(button, NodeFlag::ChildrenAreInline, false);
        (flex_wrapper, content_box)
    }

    /// Wraps the children of `fieldset`, a fieldset's box, but for its rendered legend `legend`, if it has one, in an
    /// anonymous fieldset content box that goes after the legend, and answers the box.
    pub(crate) fn wrap_fieldset_contents(
        &mut self,
        fieldset: NodeSlotId,
        legend: NodeSlotId,
        is_flex: bool,
    ) -> NodeSlotId {
        let first_child = self.node(fieldset).first_child();
        let before = if legend.is_invalid() {
            first_child
        } else {
            // NB: The children before the legend go into the wrapper after it.
            if legend != first_child {
                self.pre_order_positions_out_of_order = true;
            }
            self.node(legend).next_sibling()
        };
        let wrapper = self.push_contents_wrapper(if is_flex {
            crate::css::css_enums::display_inside::FLEX
        } else {
            crate::css::css_enums::display_inside::FLOW_ROOT
        });
        self.attach(fieldset, wrapper, before);
        self.derive_subtree_on_commit(wrapper);
        self.move_children(fieldset, wrapper, legend);
        wrapper
    }

    /// The rendered legend of `fieldset`, a fieldset's box, if it has one.
    // https://html.spec.whatwg.org/multipage/rendering.html#rendered-legend
    // The rendered legend is the first legend child whose used 'float' is 'none' and whose used
    // 'position' is neither 'absolute' nor 'fixed'.
    pub(crate) fn rendered_legend(&self, fieldset: NodeSlotId) -> NodeSlotId {
        let mut child = self.node(fieldset).first_child();
        while !child.is_invalid() {
            let child_box = self.node(child);
            if child_box.kind() == NodeKind::LegendBox && !child_box.is_out_of_flow() {
                return child;
            }
            child = child_box.next_sibling();
        }
        NodeSlotId::INVALID
    }

    /// Places `node`, which has no parent, among the descendants of `nearest_insertion_ancestor` the way the arena's
    /// builder places a box, and answers where it went.
    pub(crate) fn insert_node_into_inline_or_block_ancestor(
        &mut self,
        nearest_insertion_ancestor: NodeSlotId,
        node: NodeSlotId,
        is_inline_outside: bool,
        position: ChildPosition,
    ) -> NodeSlotId {
        let ancestor_box = self.node(nearest_insertion_ancestor);
        let ancestor_is_inline_node = ancestor_box.kind() == NodeKind::InlineNode;
        let is_inline_outside = is_inline_outside
            || self
                .node(node)
                .is_inline_level_absolutely_positioned_box_in_flow_layout(&ancestor_box);

        let insertion_point = if is_inline_outside {
            self.insertion_parent_for_inline_node(nearest_insertion_ancestor, position)
        } else {
            self.insertion_parent_for_block_node(nearest_insertion_ancestor, node, position)
        };
        if insertion_point != nearest_insertion_ancestor && ancestor_is_inline_node {
            self.lifted_out_of_inline_boxes.push((node, nearest_insertion_ancestor));
        }

        let before = match position {
            ChildPosition::Append => NodeSlotId::INVALID,
            ChildPosition::Prepend => self.node(insertion_point).first_child(),
        };
        self.attach(insertion_point, node, before);

        let point_box = self.node(insertion_point);
        let children_are_inline = if is_inline_outside {
            // After inserting an inline-level box into a parent, mark the parent as having inline children.
            Some(true)
        } else if !self.node(node).is_out_of_flow_child_of(&point_box) && !point_box.is_inline_flow() {
            // Inline-flow parents keep their inline children flag; their IFC may contain interrupting blocks.
            Some(false)
        } else {
            None
        };
        if let Some(children_are_inline) = children_are_inline
            && point_box.has_flag(NodeFlag::ChildrenAreInline) != children_are_inline
        {
            self.set_flag(insertion_point, NodeFlag::ChildrenAreInline, children_are_inline);
        }
        insertion_point
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_enums::{display_inside, display_outside};
    use crate::layout::FfiDisplay;
    use crate::layout::tree_plan::box_facts_of_display;

    fn display(outside: u8, inside: u8) -> u32 {
        box_facts_of_display(FfiDisplay::outside_and_inside(outside, inside, false)) | box_fact::HAS_STYLE
    }

    fn block() -> u32 {
        display(display_outside::BLOCK, display_inside::FLOW)
    }

    fn inline() -> u32 {
        display(display_outside::INLINE, display_inside::FLOW)
    }

    fn element(tree: &mut PlanTree, kind: NodeKind, facts: u32) -> NodeSlotId {
        let mut facts = facts;
        if kind == NodeKind::Box && facts & box_fact::FLEX_OR_GRID_INSIDE != 0 {
            facts |= box_fact::ESTABLISHES_FLEX_ITEMS;
        }
        tree.push(PlanNode::new(
            kind,
            node_facts::construction_flags(kind, false, 0),
            facts,
        ))
    }

    fn text(tree: &mut PlanTree) -> NodeSlotId {
        tree.push(PlanNode::new(
            NodeKind::TextNode,
            node_facts::construction_flags(NodeKind::TextNode, false, 0),
            box_fact::INLINE_OUTSIDE,
        ))
    }

    fn root(tree: &mut PlanTree, kind: NodeKind, facts: u32) -> NodeSlotId {
        element(tree, kind, facts)
    }

    fn append(tree: &mut PlanTree, parent: NodeSlotId, node: NodeSlotId) -> NodeSlotId {
        let is_inline = tree.node(node).has_fact(box_fact::INLINE_OUTSIDE);
        tree.insert_node_into_inline_or_block_ancestor(parent, node, is_inline, ChildPosition::Append)
    }

    fn children(tree: &PlanTree, parent: NodeSlotId) -> Vec<NodeSlotId> {
        let mut children = Vec::new();
        let mut child = tree.node(parent).first_child();
        while !child.is_invalid() {
            assert_eq!(tree.node(child).parent(), parent);
            children.push(child);
            child = tree.node(child).next_sibling();
        }
        children
    }

    fn is_wrapper(tree: &PlanTree, node: NodeSlotId) -> bool {
        let node = tree.node(node);
        node.has_flag(NodeFlag::Anonymous) && node.kind() == NodeKind::BlockContainer && node.generated_for() == 0
    }

    fn out_of_flow(facts: u32) -> u32 {
        facts | box_fact::FLOATING
    }

    #[test]
    fn inline_children_of_an_empty_block_go_into_the_block() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let first = text(&mut tree);
        let second = element(&mut tree, NodeKind::InlineNode, inline());
        assert_eq!(append(&mut tree, parent, first), parent);
        assert_eq!(append(&mut tree, parent, second), parent);
        assert_eq!(children(&tree, parent), [first, second]);
        assert!(tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn inline_children_of_an_inline_box_stay_in_it() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let span = element(&mut tree, NodeKind::InlineNode, inline());
        append(&mut tree, parent, span);
        let inner = text(&mut tree);
        assert_eq!(append(&mut tree, span, inner), span);
        assert!(tree.node(span).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn inline_child_after_a_block_child_opens_a_wrapper_which_later_inline_children_join() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let block_child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, block_child);
        assert!(!tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
        let first = text(&mut tree);
        let wrapper = append(&mut tree, parent, first);
        assert!(is_wrapper(&tree, wrapper));
        assert!(tree.node(wrapper).has_flag(NodeFlag::ChildrenAreInline));
        let second = element(&mut tree, NodeKind::InlineNode, inline());
        assert_eq!(append(&mut tree, parent, second), wrapper);
        assert_eq!(children(&tree, parent), [block_child, wrapper]);
        assert_eq!(children(&tree, wrapper), [first, second]);
    }

    #[test]
    fn inline_child_of_a_flex_container_gets_a_wrapper_each_run() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::Box,
            display(display_outside::BLOCK, display_inside::FLEX),
        );
        let first = text(&mut tree);
        let wrapper = append(&mut tree, parent, first);
        assert!(is_wrapper(&tree, wrapper));
        let second = text(&mut tree);
        assert_eq!(append(&mut tree, parent, second), wrapper);
        let item = element(&mut tree, NodeKind::BlockContainer, block());
        assert_eq!(append(&mut tree, parent, item), parent);
        assert!(tree.node(item).has_flag(NodeFlag::IsFlexItem));
        let third = text(&mut tree);
        let second_wrapper = append(&mut tree, parent, third);
        assert_ne!(second_wrapper, wrapper);
        assert_eq!(children(&tree, parent), [wrapper, item, second_wrapper]);
        assert!(!tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn inline_child_of_a_fieldset_goes_into_a_wrapper() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::FieldSetBox,
            display(display_outside::BLOCK, display_inside::FLOW_ROOT),
        );
        let child = text(&mut tree);
        let wrapper = append(&mut tree, parent, child);
        assert!(is_wrapper(&tree, wrapper));
        assert_eq!(children(&tree, parent), [wrapper]);
    }

    #[test]
    fn inline_child_of_an_svg_box_goes_into_it() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::SVGSVGBox,
            display(display_outside::INLINE, display_inside::FLOW_ROOT),
        );
        let block_child = element(&mut tree, NodeKind::SVGGraphicsBox, block());
        assert_eq!(append(&mut tree, parent, block_child), parent);
        let child = text(&mut tree);
        assert_eq!(append(&mut tree, parent, child), parent);
        assert_eq!(children(&tree, parent), [block_child, child]);
    }

    #[test]
    fn prepended_inline_child_joins_or_opens_a_wrapper_at_the_start() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let block_child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, block_child);
        let marker = element(&mut tree, NodeKind::InlineNode, inline());
        let wrapper = tree.insert_node_into_inline_or_block_ancestor(parent, marker, true, ChildPosition::Prepend);
        assert!(is_wrapper(&tree, wrapper));
        assert_eq!(children(&tree, parent), [wrapper, block_child]);
        let another = element(&mut tree, NodeKind::InlineNode, inline());
        assert_eq!(
            tree.insert_node_into_inline_or_block_ancestor(parent, another, true, ChildPosition::Prepend),
            wrapper
        );
        assert_eq!(children(&tree, wrapper), [another, marker]);
    }

    #[test]
    fn a_pseudo_element_wrapper_is_not_joined() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let block_child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, block_child);
        let first = text(&mut tree);
        let wrapper = append(&mut tree, parent, first);
        tree.set_generated_for(wrapper, 1);
        let second = text(&mut tree);
        let second_wrapper = append(&mut tree, parent, second);
        assert_ne!(wrapper, second_wrapper);
    }

    #[test]
    fn block_child_of_an_empty_block_goes_into_it() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let child = element(&mut tree, NodeKind::BlockContainer, block());
        assert_eq!(append(&mut tree, parent, child), parent);
        assert!(!tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn block_child_of_an_inline_box_stays_in_it() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let span = element(&mut tree, NodeKind::InlineNode, inline());
        append(&mut tree, parent, span);
        let first = text(&mut tree);
        append(&mut tree, span, first);
        let child = element(&mut tree, NodeKind::BlockContainer, block());
        assert_eq!(append(&mut tree, span, child), span);
        assert_eq!(children(&tree, span), [first, child]);
        assert!(tree.node(span).has_flag(NodeFlag::ChildrenAreInline));
        assert!(tree.lifted_out_of_inline_boxes.is_empty());
    }

    #[test]
    fn block_pseudo_element_box_of_an_inline_box_stays_in_it() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let span = element(&mut tree, NodeKind::InlineNode, inline());
        append(&mut tree, parent, span);
        let inner_span = element(&mut tree, NodeKind::InlineNode, inline());
        append(&mut tree, span, inner_span);
        let pseudo = element(&mut tree, NodeKind::BlockContainer, block());
        tree.set_flag(pseudo, NodeFlag::Anonymous, true);
        tree.set_generated_for(pseudo, 1);
        assert_eq!(append(&mut tree, inner_span, pseudo), inner_span);
        assert_eq!(children(&tree, parent), [span]);
        assert!(tree.lifted_out_of_inline_boxes.is_empty());
    }

    #[test]
    fn absolutely_positioned_pseudo_element_stays_in_its_inline_box() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let span = element(&mut tree, NodeKind::InlineNode, inline());
        append(&mut tree, parent, span);
        let pseudo = element(
            &mut tree,
            NodeKind::BlockContainer,
            block() | box_fact::ABSOLUTELY_POSITIONED,
        );
        tree.set_flag(pseudo, NodeFlag::Anonymous, true);
        tree.set_generated_for(pseudo, 1);
        assert_eq!(append(&mut tree, span, pseudo), span);
    }

    #[test]
    fn first_block_wraps_every_earlier_child_in_one_wrapper() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let first = text(&mut tree);
        append(&mut tree, parent, first);
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        assert_eq!(append(&mut tree, parent, float), parent);
        let second = text(&mut tree);
        append(&mut tree, parent, second);
        let child = element(&mut tree, NodeKind::BlockContainer, block());
        assert_eq!(append(&mut tree, parent, child), parent);
        let parent_children = children(&tree, parent);
        assert_eq!(parent_children.len(), 2);
        let wrapper = parent_children[0];
        assert!(is_wrapper(&tree, wrapper));
        assert!(tree.node(wrapper).has_flag(NodeFlag::ChildrenAreInline));
        assert_eq!(children(&tree, wrapper), [first, float, second]);
        assert_eq!(parent_children[1], child);
        assert!(!tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn first_block_in_a_flex_container_wraps_each_run_between_out_of_flow_children() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::Box,
            display(display_outside::BLOCK, display_inside::FLEX),
        );
        // An inline-level box only ever reaches a flex container's own children list as a wrapper's child, so build
        // the inline mode by hand, as an inline flow parent of mixed children would have it.
        tree.set_flag(parent, NodeFlag::ChildrenAreInline, true);
        let first = text(&mut tree);
        tree.attach(parent, first, NodeSlotId::INVALID);
        let abspos = element(
            &mut tree,
            NodeKind::BlockContainer,
            block() | box_fact::ABSOLUTELY_POSITIONED,
        );
        tree.attach(parent, abspos, NodeSlotId::INVALID);
        let second = text(&mut tree);
        tree.attach(parent, second, NodeSlotId::INVALID);
        let child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, child);
        let parent_children = children(&tree, parent);
        assert_eq!(parent_children.len(), 4);
        assert!(is_wrapper(&tree, parent_children[0]));
        assert_eq!(children(&tree, parent_children[0]), [first]);
        assert_eq!(parent_children[1], abspos);
        assert!(is_wrapper(&tree, parent_children[2]));
        assert_eq!(children(&tree, parent_children[2]), [second]);
        assert_eq!(parent_children[3], child);
    }

    #[test]
    fn out_of_flow_block_joins_the_trailing_inline_run() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let block_child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, block_child);
        let first = text(&mut tree);
        let wrapper = append(&mut tree, parent, first);
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        assert_eq!(append(&mut tree, parent, float), wrapper);
        assert!(tree.node(wrapper).has_flag(NodeFlag::ChildrenAreInline));
        // A prepended one goes to the parent's start instead.
        let prepended = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        assert_eq!(
            tree.insert_node_into_inline_or_block_ancestor(parent, prepended, false, ChildPosition::Prepend),
            parent
        );
        assert_eq!(children(&tree, parent), [prepended, block_child, wrapper]);
    }

    #[test]
    fn out_of_flow_block_after_a_block_goes_into_the_parent() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let block_child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, block_child);
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        assert_eq!(append(&mut tree, parent, float), parent);
        assert!(!tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn out_of_flow_block_among_inline_children_stays_beside_them() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let first = text(&mut tree);
        append(&mut tree, parent, first);
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        assert_eq!(append(&mut tree, parent, float), parent);
        assert!(tree.node(parent).has_flag(NodeFlag::ChildrenAreInline));
        assert_eq!(children(&tree, parent), [first, float]);
    }

    #[test]
    fn only_out_of_flow_children_count_as_none() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        append(&mut tree, parent, float);
        let child = element(&mut tree, NodeKind::BlockContainer, block());
        assert_eq!(append(&mut tree, parent, child), parent);
        assert_eq!(children(&tree, parent), [float, child]);
    }

    #[test]
    fn floated_flex_item_is_in_flow() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::Box,
            display(display_outside::BLOCK, display_inside::FLEX),
        );
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        append(&mut tree, parent, float);
        assert!(tree.node(float).has_flag(NodeFlag::IsFlexItem));
        assert!(!tree.node(float).is_out_of_flow());
        assert!(tree.node(parent).has_fact(box_fact::HAS_IN_FLOW_BLOCK_CHILDREN));
    }

    #[test]
    fn inline_level_absolutely_positioned_box_is_placed_as_inline() {
        let mut tree = PlanTree::default();
        let parent = root(&mut tree, NodeKind::BlockContainer, block());
        let block_child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, block_child);
        let abspos = element(
            &mut tree,
            NodeKind::BlockContainer,
            block() | box_fact::ABSOLUTELY_POSITIONED | box_fact::INLINE_OUTSIDE_BEFORE_BLOCKIFICATION,
        );
        let wrapper = append(&mut tree, parent, abspos);
        assert!(is_wrapper(&tree, wrapper));
        assert!(tree.node(wrapper).has_flag(NodeFlag::ChildrenAreInline));
    }

    #[test]
    fn out_of_flow_table_internal_child_of_a_table_stays_out_of_the_wrapper() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::Box,
            display(display_outside::BLOCK, display_inside::TABLE),
        );
        let first = text(&mut tree);
        append(&mut tree, parent, first);
        let cell = element(
            &mut tree,
            NodeKind::BlockContainer,
            block() | box_fact::FLOATING | box_fact::TABLE_NON_ROOT_BEFORE_BLOCKIFICATION,
        );
        assert_eq!(append(&mut tree, parent, cell), parent);
        let second = text(&mut tree);
        append(&mut tree, parent, second);
        let child = element(&mut tree, NodeKind::BlockContainer, block());
        append(&mut tree, parent, child);
        let parent_children = children(&tree, parent);
        assert_eq!(parent_children.len(), 3);
        assert_eq!(parent_children[0], cell);
        assert!(is_wrapper(&tree, parent_children[1]));
        assert_eq!(children(&tree, parent_children[1]), [first, second]);
        assert_eq!(parent_children[2], child);
    }

    #[test]
    fn floats_under_math_are_in_flow() {
        let mut tree = PlanTree::default();
        let parent = root(
            &mut tree,
            NodeKind::BlockContainer,
            display(display_outside::BLOCK, display_inside::MATH),
        );
        let float = element(&mut tree, NodeKind::BlockContainer, out_of_flow(block()));
        append(&mut tree, parent, float);
        assert!(!tree.is_out_of_flow_child_of(parent, float));
    }
}
