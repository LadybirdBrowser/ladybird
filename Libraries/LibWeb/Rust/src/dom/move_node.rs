/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Moving a node into a new parent, which keeps the state that removing and inserting it would lose.
//! https://dom.spec.whatwg.org/#move

use super::hooks;
use super::hooks::IsSubtreeRoot;
use super::hooks::MoveScope;
use super::links;
use super::node::Node;
use super::node::NodeCell;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::node_ref::RawNullableNodeRef;
use super::pre_insert::is_character_data;
use super::pre_insert::is_doctype;
use super::pre_insert::is_text;
use super::range::adjust_live_ranges_for_insertion;
use super::range::live_range_pre_remove_steps;
use super::steps::moving_steps_do_nothing;
use super::traversal::run_node_iterator_pre_remove_steps;
use super::tree::for_each_shadow_including_inclusive_descendant;
use super::tree_version::bump_dom_tree_version;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// Whether a node can be moved, or else which DOMException to throw and why.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MoveValidity {
    Valid,
    /// A "HierarchyRequestError" DOMException: the new parent's shadow-including root is not the node's.
    NotInTheSameShadowIncludingRoot,
    /// A "HierarchyRequestError" DOMException: the node is a host-including inclusive ancestor of the new parent.
    NodeIsAnAncestorOfNewParent,
    /// A "NotFoundError" DOMException: the child is not a child of the new parent.
    ChildIsNotAChildOfNewParent,
    /// A "HierarchyRequestError" DOMException: the new parent cannot hold the node, of its type, there.
    InvalidNodeType,
}

/// Steps 1 to 6 of moving `node` into `new_parent` before `child`, the pre-move validity checks.
/// https://dom.spec.whatwg.org/#move
fn ensure_move_validity<'a>(node: Node<'a>, new_parent: Node<'a>, child: Option<Node<'a>>) -> MoveValidity {
    use MoveValidity::*;

    // 1. If newParent’s shadow-including root is not the same as node’s shadow-including root, then throw a
    //    "HierarchyRequestError" DOMException.
    //    NOTE: This has the side effect of ensuring that a move is only performed if newParent’s connected is node’s
    //          connected.
    if new_parent.shadow_including_root() != node.shadow_including_root() {
        return NotInTheSameShadowIncludingRoot;
    }

    // 2. If node is a host-including inclusive ancestor of newParent, then throw a "HierarchyRequestError"
    //    DOMException.
    if node.is_host_including_inclusive_ancestor_of(new_parent) {
        return NodeIsAnAncestorOfNewParent;
    }

    // 3. If child is non-null and its parent is not newParent, then throw a "NotFoundError" DOMException.
    if let Some(child) = child
        && child.parent() != Some(new_parent)
    {
        return ChildIsNotAChildOfNewParent;
    }

    // 4. If node is not an Element or a CharacterData node, then throw a "HierarchyRequestError" DOMException.
    if !node.is_element() && !is_character_data(node) {
        return InvalidNodeType;
    }

    // 5. If node is a Text node and newParent is a document, then throw a "HierarchyRequestError" DOMException.
    let new_parent_is_a_document = new_parent.node_type() == NodeType::DOCUMENT_NODE;
    if is_text(node) && new_parent_is_a_document {
        return InvalidNodeType;
    }

    // 6. If newParent is a document, node is an Element node, and either newParent has an element child, child is a
    //    doctype, or child is non-null and a doctype is following child then throw a "HierarchyRequestError"
    //    DOMException.
    if new_parent_is_a_document
        && node.is_element()
        && (new_parent.children().any(Node::is_element)
            || child.is_some_and(is_doctype)
            || child.is_some_and(|child| child.following().any(is_doctype)))
    {
        return InvalidNodeType;
    }

    Valid
}

/// The pre-move validity checks of nodes that code which changes the DOM holds.
fn ensure_move_validity_of(
    main_thread: &MainThread<'_>,
    node: NodeRef,
    new_parent: NodeRef,
    child: Option<NodeRef>,
) -> MoveValidity {
    let child = child.as_ref();
    // SAFETY: The nodes are live, and the shared borrow of the main thread token keeps anything from changing the DOM
    //         while the checks read it.
    unsafe {
        ensure_move_validity(
            Node::from_cell(node.cell(), main_thread),
            Node::from_cell(new_parent.cell(), main_thread),
            child.map(|child| Node::from_cell(child.cell(), main_thread)),
        )
    }
}

/// https://dom.spec.whatwg.org/#move
///
/// `scope` is the C++ state the callbacks keep across the move. Answers which DOMException to throw where the pre-move
/// validity checks throw one.
pub(crate) fn move_node(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    new_parent: NodeRef,
    child: Option<NodeRef>,
    scope: &mut MoveScope,
) -> MoveValidity {
    // To move a node node into a node newParent before null or a node child:

    // 1. ... 6.
    let validity = ensure_move_validity_of(main_thread, node, new_parent, child);
    if validity != MoveValidity::Valid {
        return validity;
    }

    // 7. Let oldParent be node’s parent.
    let old_parent = node.parent();

    // 8. Assert: oldParent is non-null.
    let old_parent = old_parent.expect("a moved node has a parent");

    let old_parent_root = old_parent.root();

    // NB: The callback does what the move does for the page, the style engine, layout, slots and the C++ state that
    //     follows the children of oldParent while the subtree still stands where it did, see hooks::begin_move().
    hooks::begin_move(main_thread, scope, node, old_parent, old_parent_root, new_parent);

    // 9. Run the live range pre-remove steps, given node.
    live_range_pre_remove_steps(main_thread, node);

    // 10. For each NodeIterator object iterator whose root’s node document is node’s node document: run the
    //     NodeIterator pre-remove steps given node and iterator.
    run_node_iterator_pre_remove_steps(main_thread, node);

    // 11. Let oldPreviousSibling be node’s previous sibling.
    let old_previous_sibling = node.previous_sibling();

    // 12. Let oldNextSibling be node’s next sibling.
    let old_next_sibling = node.next_sibling();

    // 13. Remove node from oldParent’s children.
    links::remove_child(main_thread, old_parent, node);

    // 14. If node is assigned, then run assign slottables for node’s assigned slot.
    // 15. If oldParent’s root is a shadow root, and oldParent is a slot whose assigned nodes is empty, then run signal
    //     a slot change for oldParent.
    // 16. If node has an inclusive descendant that is a slot:
    //     1. Run assign slottables for a tree with oldParent’s root.
    //     2. Run assign slottables for a tree with node.
    // 17. Add transient registered observers given node and oldParent.
    hooks::after_move_unlink(main_thread, scope, node, old_parent, old_parent_root);

    // 18. If child is non-null:
    //     1. For each live range whose start node is newParent and start offset is greater than child’s index:
    //        increase its start offset by 1.
    //     2. For each live range whose end node is newParent and end offset is greater than child’s index: increase
    //        its end offset by 1.
    if let Some(child) = child {
        adjust_live_ranges_for_insertion(main_thread, new_parent, child, 1);
    }

    // 19. Let newPreviousSibling be child’s previous sibling if child is non-null, and newParent’s last child
    //     otherwise.
    let new_previous_sibling = match child {
        Some(child) => child.previous_sibling(),
        None => new_parent.last_child(),
    };

    // 20. If child is null, then append node to newParent’s children.
    // 21. Otherwise, insert node into newParent’s children before child’s index.
    links::insert_child(main_thread, new_parent, node, child);

    // 22. If newParent is a shadow host whose shadow root’s slot assignment is "named" and node is a slottable, then
    //     assign a slot for node.
    // 23. If newParent’s root is a shadow root, and newParent is a slot whose assigned nodes is empty, then run signal
    //     a slot change for newParent.
    // 24. Run assign slottables for a tree with node’s root.
    // NB: The callback also does what the move does for the C++ state that follows the children of newParent, the
    //     style engine and layout, see hooks::after_move_link().
    hooks::after_move_link(main_thread, scope, node, old_parent, old_parent_root, new_parent);

    // 25. For each shadow-including inclusive descendant inclusiveDescendant of node, in shadow-including tree order:
    for_each_shadow_including_inclusive_descendant(node, &mut |inclusive_descendant| {
        // 1. Let isSubtreeRoot be true if inclusiveDescendant is node; otherwise false.
        let is_subtree_root = match inclusive_descendant == node {
            true => IsSubtreeRoot::Yes,
            false => IsSubtreeRoot::No,
        };

        // OPTIMIZATION: The cores tell which nodes the moving steps have nothing to do for, see steps.rs. Neither is
        //               such a node custom with a connected newParent.
        if moving_steps_do_nothing(inclusive_descendant, is_subtree_root) {
            return;
        }

        // 2. If inclusiveDescendant is custom and newParent is connected, then enqueue a custom element callback
        //    reaction with inclusiveDescendant, callback name "connectedMoveCallback", and « ».
        // 3. Run the moving steps with inclusiveDescendant, isSubtreeRoot, and oldParent.
        //    NOTE: Because the move algorithm is a separate primitive from insert and remove, it does not invoke the
        //          insertion steps or removing steps for inclusiveDescendant.
        hooks::run_moving_steps(
            main_thread,
            inclusive_descendant,
            is_subtree_root,
            old_parent,
            new_parent,
        );
    });

    // 26. Queue a tree mutation record for oldParent with « », « node », oldPreviousSibling, and oldNextSibling.
    // 27. Queue a tree mutation record for newParent with « node », « », newPreviousSibling, and child.
    // NB: The callback also does what the move does for the styles it changed, and for an <html> or media element
    //     parent, see hooks::end_move().
    hooks::end_move(
        main_thread,
        scope,
        node,
        old_parent,
        new_parent,
        old_previous_sibling,
        old_next_sibling,
        new_previous_sibling,
        child,
    );

    bump_dom_tree_version(main_thread, old_parent);
    bump_dom_tree_version(main_thread, new_parent);

    MoveValidity::Valid
}

/// Moves `node` into `new_parent` before `child`, for DOM::Node, with `scope`, the C++ state the callbacks keep across
/// the move, and answers which DOMException to throw if it throws one.
///
/// # Safety
///
/// The nodes must be live, `scope` must be a DOM::MoveScope that lives for the call, and the call must be made on the
/// main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_move(
    node: &NodeCell,
    new_parent: &NodeCell,
    child: RawNullableNodeRef,
    scope: &mut MoveScope,
) -> MoveValidity {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (node, new_parent) = (reference(node), reference(new_parent));
    move_node(&mut main_thread, node, new_parent, child.into(), scope)
}

#[cfg(test)]
mod tests {
    use super::super::test_tree::append_child;
    use super::super::test_tree::node;
    use super::super::test_tree::shadow_root;
    use super::MoveValidity::*;
    use super::*;

    #[test]
    fn a_node_moves_only_within_its_shadow_including_root() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let html = node(&main_thread, NodeType::ELEMENT_NODE);
        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        let moved = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(document, html);
        append_child(html, host);
        append_child(html, moved);
        let shadow_root = shadow_root(&main_thread, host);
        let in_shadow_tree = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(shadow_root, in_shadow_tree);
        let elsewhere = node(&main_thread, NodeType::ELEMENT_NODE);

        assert_eq!(in_shadow_tree.shadow_including_root(), document);
        assert_eq!(ensure_move_validity(moved, in_shadow_tree, None), Valid);
        assert_eq!(
            ensure_move_validity(moved, host, Some(moved)),
            ChildIsNotAChildOfNewParent
        );
        assert_eq!(
            ensure_move_validity(moved, elsewhere, None),
            NotInTheSameShadowIncludingRoot
        );
        assert_eq!(
            ensure_move_validity(html, in_shadow_tree, None),
            NodeIsAnAncestorOfNewParent
        );
    }

    #[test]
    fn only_elements_and_character_data_move_and_a_document_keeps_one_element() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let doctype = node(&main_thread, NodeType::DOCUMENT_TYPE_NODE);
        let html = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(document, doctype);
        append_child(document, html);
        let [element, text, comment, fragment] = [
            NodeType::ELEMENT_NODE,
            NodeType::TEXT_NODE,
            NodeType::COMMENT_NODE,
            NodeType::DOCUMENT_FRAGMENT_NODE,
        ]
        .map(|node_type| {
            let moved = node(&main_thread, node_type);
            append_child(html, moved);
            moved
        });

        assert_eq!(ensure_move_validity(comment, document, None), Valid);
        assert_eq!(ensure_move_validity(fragment, html, None), InvalidNodeType);
        assert_eq!(ensure_move_validity(doctype, html, None), InvalidNodeType);
        assert_eq!(ensure_move_validity(text, document, None), InvalidNodeType);
        assert_eq!(ensure_move_validity(element, document, None), InvalidNodeType);
        assert_eq!(ensure_move_validity(element, html, Some(text)), Valid);
    }
}
