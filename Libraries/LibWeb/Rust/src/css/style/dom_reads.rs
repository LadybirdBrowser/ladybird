/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

// The DOM shape layout, painting and style computation read from the style mirror. Every answer
// is an ID or a value, so readers do not depend on how the mirror stores its relations.
impl RetainedState {
    /// The DOM parent of an element, shadow root or the document element: an element, a shadow
    /// root or the document.
    #[must_use]
    pub fn dom_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.parent(node)
    }

    /// The DOM parent of an element, where a shadow root stands for its host.
    #[must_use]
    pub fn parent_element_or_host(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection
            .parent(node)
            .map(|parent| self.projection.host_of(parent).unwrap_or(parent))
    }

    /// The DOM parent of a node, or the host of a shadow root.
    #[must_use]
    pub fn parent_or_shadow_host(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.parent_or_shadow_host(node)
    }

    /// The tree scope the element is in.
    #[must_use]
    pub fn element_tree_scope(&self, node: StyleNodeID) -> TreeScopeID {
        self.projection.tree_scope(node)
    }

    #[must_use]
    pub fn first_element_child(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.first_element_child(node)
    }

    #[must_use]
    pub fn next_element_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.next_element_sibling(node)
    }

    /// Whether the node stands in the mirror only to carry relations, as the document and a
    /// shadow root do, rather than being an element.
    #[must_use]
    pub fn is_relation_only(&self, node: StyleNodeID) -> bool {
        self.tree.is_relation_only(node)
    }

    /// Whether `node` is `root` or is under it, crossing into shadow trees.
    #[must_use]
    pub fn is_in_shadow_including_subtree_of(&self, node: StyleNodeID, root: StyleNodeID) -> bool {
        if node == root {
            return true;
        }
        // A text node has no projection row, so it is answered for by the element it is linked
        // under.
        let element = match node.text_index() {
            Some(_) => match self.tree.text_parent(node) {
                Some(parent) => parent,
                None => return false,
            },
            None => node,
        };
        self.projection.is_in_shadow_including_subtree_of(element, root)
    }

    /// The shadow root the element `host` hosts, if any.
    #[must_use]
    pub fn shadow_root_of(&self, host: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.shadow_root_of(host)
    }

    /// The host of the shadow root `shadow_root`, or none for any other node.
    #[must_use]
    pub fn host_of(&self, shadow_root: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.host_of(shadow_root)
    }

    /// The host of the shadow tree the node is in, if any.
    #[must_use]
    pub fn shadow_host_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.shadow_host_of(node)
    }

    /// The slot the element or text node is assigned to, if any.
    #[must_use]
    pub fn assigned_slot_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.assigned_slot_of(node)
    }

    /// How many nodes are assigned to the slot.
    #[must_use]
    pub fn assigned_node_count(&self, slot: StyleNodeID) -> usize {
        self.projection.assigned_nodes_of(slot).len()
    }

    /// The node at `index` among those assigned to the slot, in assignment order.
    #[must_use]
    pub fn assigned_node(&self, slot: StyleNodeID, index: usize) -> StyleNodeID {
        self.projection.assigned_nodes_of(slot)[index]
    }

    /// The nodes assigned to the slot, in assignment order.
    #[must_use]
    pub fn assigned_nodes_of(&self, slot: StyleNodeID) -> &[StyleNodeID] {
        self.projection.assigned_nodes_of(slot)
    }

    /// The element a node inherits from: the slot it is assigned to, its parent, or the host of the shadow root it is a
    /// child of.
    #[must_use]
    pub fn inheritance_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if node.text_index().is_none() {
            return self.projection.inheritance_parent(node);
        }
        if let Some(slot) = self.assigned_slot_of(node) {
            return Some(slot);
        }
        let parent = self.text_parent(node)?;
        Some(self.host_of(parent).unwrap_or(parent))
    }

    /// Whether the text node is in a user agent shadow tree.
    #[must_use]
    pub fn text_is_in_user_agent_shadow_tree(&self, node: StyleNodeID) -> bool {
        self.tree.text_is_in_user_agent_shadow_tree(node)
    }

    /// The characters the text node holds.
    #[must_use]
    pub fn text_data(&self, node: StyleNodeID) -> Option<&ak::Utf16String> {
        self.tree.text_data(node)
    }

    /// The node's DOM children in order, text nodes included.
    pub fn dom_children(&self, node: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.tree.dom_children(node)
    }

    #[must_use]
    pub fn next_sibling_in_dom_order(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.tree.next_sibling_in_dom_order(node)
    }

    #[must_use]
    pub fn previous_sibling_in_dom_order(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.tree.previous_sibling_in_dom_order(node)
    }

    /// The DOM parent of a text node, or none for anything else.
    #[must_use]
    pub fn text_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.tree.text_parent(node)
    }

    /// The element at `index` in the top layer, in the order the elements were added.
    #[must_use]
    pub fn top_layer_element(&self, index: usize) -> Option<StyleNodeID> {
        self.tree.top_layer().get(index).copied()
    }
}
