/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How nodes stand in the style engine's tree, as the DOM's cores have it: the engine's tree is the DOM without the
//! nodes that have not taken their identity yet, so each relation names the nearest node that has.

use super::bridge::FfiTreeRelations;
use super::tree::{StyleNodeID, TreeScopeID};
use crate::dom::element::Element;
use crate::dom::node::Node;

fn raw(node: Option<StyleNodeID>) -> u32 {
    node.map_or(0, StyleNodeID::raw)
}

/// The identity of the node, which only an element, a text node, a shadow root or a document takes.
fn identity(node: Node<'_>) -> Option<StyleNodeID> {
    node.core().style_node_id()
}

/// The style-tree parent of an element: its parent element, or the shadow root it is a top-level child of.
pub(super) fn style_tree_parent_of(element: Element<'_>) -> Option<StyleNodeID> {
    let parent = element.node().parent()?;
    match parent.is_element() || parent.is_shadow_root() {
        true => identity(parent),
        false => None,
    }
}

/// The nearest element at or before `element` among its siblings that has an identity.
pub(super) fn arrived_element_at_or_before(element: Option<Node<'_>>) -> Option<StyleNodeID> {
    core::iter::successors(element, |node| node.previous_sibling())
        .filter(|node| node.is_element())
        .find_map(identity)
}

/// The nearest element at or after `element` among its siblings that has an identity.
pub(super) fn arrived_element_at_or_after(element: Option<Node<'_>>) -> Option<StyleNodeID> {
    core::iter::successors(element, |node| node.next_sibling())
        .filter(|node| node.is_element())
        .find_map(identity)
}

/// The scope of the tree whose root is `root`: a shadow root's own, or the document's. The host numbers a shadow root's
/// scope as the first node in its tree arrives, and keeps it while the root stays in the document.
pub(super) fn tree_scope_of(root: Node<'_>) -> TreeScopeID {
    match root.is_shadow_root() {
        true => {
            let tree_scope = root.style_engine_tree_scope();
            debug_assert_ne!(
                tree_scope,
                TreeScopeID::DOCUMENT,
                "a shadow tree in the engine's tree has its scope"
            );
            tree_scope
        }
        false => TreeScopeID::DOCUMENT,
    }
}

/// Whether the previous siblings of an element still stand in the engine's tree. A subtree leaves in tree order, so
/// below its root every previous sibling has already departed and none is left to name, and scanning past them would
/// make removing a wide subtree quadratic.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum PreviousSiblings {
    Arrived,
    Departed,
}

/// How the element stands in the engine's tree, assigned to the slot whose identity is `assigned_slot`, or zero.
pub(super) fn relations_of(
    element: Element<'_>,
    previous_siblings: PreviousSiblings,
    assigned_slot: u32,
) -> FfiTreeRelations {
    let node = element.node();
    FfiTreeRelations {
        parent: raw(style_tree_parent_of(element)),
        previous_element_sibling: match previous_siblings {
            PreviousSiblings::Arrived => raw(arrived_element_at_or_before(node.previous_sibling())),
            PreviousSiblings::Departed => 0,
        },
        next_element_sibling: raw(arrived_element_at_or_after(node.next_sibling())),
        tree_scope: tree_scope_of(node.root()).0,
        assigned_slot,
    }
}

/// The link that places an element or a text node in its parent's child sequence of the engine's tree: the node, the
/// parent, and the nearest previous sibling that has an identity, each as an identity or zero.
///
/// Only elements, text nodes, shadow roots and documents take identities, so the nodes of a child sequence are the
/// elements and text nodes that have one, and its parent is an element, a shadow root or a document.
pub(super) fn dom_order_link_of(node: Node<'_>) -> [u32; 3] {
    [
        raw(identity(node)),
        raw(node.parent().and_then(identity)),
        raw(core::iter::successors(node.previous_sibling(), |sibling| sibling.previous_sibling()).find_map(identity)),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dom::node_core::NodeType;
    use crate::dom::test_tree::{append_child, node, set_style_engine_tree_scope, set_style_node_id, shadow_root};
    use crate::stage::MainThread;

    fn identify(node: Node<'_>, raw: u32) {
        set_style_node_id(node, StyleNodeID::from_raw(raw));
    }

    #[test]
    fn element_relations_name_the_nearest_arrived_element_siblings() {
        let main_thread = MainThread::for_test();
        let new = |node_type| node(&main_thread, node_type);
        let parent = new(NodeType::ELEMENT_NODE);
        let children = [
            new(NodeType::ELEMENT_NODE),
            new(NodeType::TEXT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::COMMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
        ];
        for child in children {
            append_child(parent, child);
        }
        let [first, text, _, middle, _, _, last] = children;
        identify(parent, 1);
        identify(first, 2);
        identify(text, 3 | (1 << 31));
        identify(middle, 4);
        identify(last, 5);

        let relations = relations_of(middle.as_element().unwrap(), PreviousSiblings::Arrived, 9);
        assert_eq!(relations.parent, 1);
        assert_eq!(relations.previous_element_sibling, 2);
        assert_eq!(relations.next_element_sibling, 5);
        assert_eq!(relations.tree_scope, TreeScopeID::DOCUMENT.0);
        assert_eq!(relations.assigned_slot, 9);

        let departing = relations_of(middle.as_element().unwrap(), PreviousSiblings::Departed, 0);
        assert_eq!(departing.previous_element_sibling, 0);
        assert_eq!(departing.next_element_sibling, 5);

        let first_relations = relations_of(first.as_element().unwrap(), PreviousSiblings::Arrived, 0);
        assert_eq!(first_relations.previous_element_sibling, 0);
        assert_eq!(first_relations.next_element_sibling, 4);
        let last_relations = relations_of(last.as_element().unwrap(), PreviousSiblings::Arrived, 0);
        assert_eq!(last_relations.previous_element_sibling, 4);
        assert_eq!(last_relations.next_element_sibling, 0);
    }

    #[test]
    fn only_an_element_or_a_shadow_root_is_a_style_tree_parent() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let html = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(document, html);
        identify(document, 1);
        identify(html, 2);
        assert_eq!(
            relations_of(html.as_element().unwrap(), PreviousSiblings::Arrived, 0).parent,
            0
        );

        let fragment = node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
        let in_fragment = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(fragment, in_fragment);
        assert_eq!(style_tree_parent_of(in_fragment.as_element().unwrap()), None);

        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        let root = shadow_root(&main_thread, host);
        let in_shadow_tree = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(root, in_shadow_tree);
        identify(root, 7);
        set_style_engine_tree_scope(root, TreeScopeID(3));
        let relations = relations_of(in_shadow_tree.as_element().unwrap(), PreviousSiblings::Arrived, 0);
        assert_eq!(relations.parent, 7);
        assert_eq!(relations.tree_scope, 3);
    }

    #[test]
    fn a_dom_order_link_names_the_nearest_previous_sibling_with_an_identity() {
        let main_thread = MainThread::for_test();
        let new = |node_type| node(&main_thread, node_type);
        let document = new(NodeType::DOCUMENT_NODE);
        let doctype = new(NodeType::DOCUMENT_TYPE_NODE);
        let html = new(NodeType::ELEMENT_NODE);
        append_child(document, doctype);
        append_child(document, html);
        let text = new(NodeType::TEXT_NODE);
        let comment = new(NodeType::COMMENT_NODE);
        let body = new(NodeType::ELEMENT_NODE);
        for child in [text, comment, body] {
            append_child(html, child);
        }
        identify(document, 1);
        identify(html, 2);
        identify(text, 3 | (1 << 31));
        identify(body, 4);

        assert_eq!(dom_order_link_of(html), [2, 1, 0]);
        assert_eq!(dom_order_link_of(text), [3 | (1 << 31), 2, 0]);
        assert_eq!(dom_order_link_of(body), [4, 2, 3 | (1 << 31)]);
        identify(text, 0);
        assert_eq!(dom_order_link_of(body), [4, 2, 0]);
    }
}
