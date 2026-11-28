/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The writes of the tree state of DOM nodes: a node's links to its parent, children and siblings, and the root of its
//! tree, which every node of a subtree names. Rust is their only writer.
//! https://dom.spec.whatwg.org/#concept-tree

use super::node::NodeCell;
use super::node_ref::NodeRef;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// Inserts `node`, the root of a tree of its own, into `parent`'s children before `child`, or appends it if `child` is
/// null, and makes `parent`'s root the root of every node of `node`'s subtree.
///
/// NB: The links are written in place and nothing is called that could run script or allocate, so the read handles the
///     main thread token lends cannot live across it, which the token's mutable borrow says.
#[inline(always)]
pub(crate) fn insert_child(_main_thread: &mut MainThread<'_>, parent: NodeRef, node: NodeRef, child: Option<NodeRef>) {
    link_child(parent, node, child);
}

/// Removes `node` from `parent`'s children, and makes `node` the root of every node of its subtree.
#[inline(always)]
pub(crate) fn remove_child(_main_thread: &mut MainThread<'_>, parent: NodeRef, node: NodeRef) {
    unlink_child(parent, node);
}

#[inline]
fn link_child(parent: NodeRef, node: NodeRef, child: Option<NodeRef>) {
    assert!(node.parent().is_none(), "an inserted node has no parent");
    let node_core = node.core();
    match child {
        // https://dom.spec.whatwg.org/#concept-node-append
        None => {
            let last_child = parent.core().last_child();
            match parent.last_child() {
                Some(last_child) => last_child.core().set_next_sibling(Some(node.pointer())),
                None => parent.core().set_first_child(Some(node.pointer())),
            }
            node_core.set_previous_sibling(last_child);
            parent.core().set_last_child(Some(node.pointer()));
        }
        Some(child) => {
            assert!(
                child.parent() == Some(parent),
                "the child to insert before is a child of the parent"
            );
            let previous_sibling = child.core().previous_sibling();
            node_core.set_previous_sibling(previous_sibling);
            node_core.set_next_sibling(Some(child.pointer()));
            match child.previous_sibling() {
                Some(previous_sibling) => previous_sibling.core().set_next_sibling(Some(node.pointer())),
                None => parent.core().set_first_child(Some(node.pointer())),
            }
            child.core().set_previous_sibling(Some(node.pointer()));
        }
    }
    node_core.set_parent(Some(parent.pointer()));
    set_root_for_subtree(node, parent.core().root());
}

#[inline]
fn unlink_child(parent: NodeRef, node: NodeRef) {
    assert!(node.parent() == Some(parent), "a removed node is a child of the parent");
    let node_core = node.core();
    let previous_sibling = node_core.previous_sibling();
    let next_sibling = node_core.next_sibling();
    match node.next_sibling() {
        Some(next_sibling) => next_sibling.core().set_previous_sibling(previous_sibling),
        None => parent.core().set_last_child(previous_sibling),
    }
    match node.previous_sibling() {
        Some(previous_sibling) => previous_sibling.core().set_next_sibling(next_sibling),
        None => parent.core().set_first_child(next_sibling),
    }
    node_core.set_next_sibling(None);
    node_core.set_previous_sibling(None);
    node_core.set_parent(None);
    set_root_for_subtree(node, node.pointer());
}

/// Makes `root` the root of every node of `node`'s inclusive subtree, in tree order.
/// https://dom.spec.whatwg.org/#concept-tree-root
fn set_root_for_subtree(node: NodeRef, root: super::node_core::NodePointer) {
    let mut current = node;
    loop {
        current.core().set_root(root);
        if let Some(first_child) = current.first_child() {
            current = first_child;
            continue;
        }
        loop {
            if current == node {
                return;
            }
            if let Some(next_sibling) = current.next_sibling() {
                current = next_sibling;
                break;
            }
            current = current.parent().expect("a node below the subtree's root has a parent");
        }
    }
}

/// Inserts `node` into `parent`'s children before `child`, or appends it if `child` is null, for DOM::Node.
///
/// # Safety
///
/// The nodes must be live, `node` must have no parent, `child` must be a child of `parent`, and the call must be made on
/// the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_insert_child(parent: &NodeCell, node: &NodeCell, child: Option<&NodeCell>) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (parent, node, child) = (reference(parent), reference(node), child.map(reference));
    insert_child(&mut main_thread, parent, node, child);
}

/// Removes `node` from `parent`'s children, for DOM::Node.
///
/// # Safety
///
/// The nodes must be live, `node` must be a child of `parent`, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_child(parent: &NodeCell, node: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (parent, node) = (reference(parent), reference(node));
    remove_child(&mut main_thread, parent, node);
}

/// Appends `node`, the root of a tree of its own, to `parent`'s children, for a test tree, whose read handles stay
/// across the write.
#[cfg(test)]
pub(crate) fn append_child_for_test(parent: NodeRef, node: NodeRef) {
    link_child(parent, node, None);
}

#[cfg(test)]
mod tests {
    use super::super::node_core::NodeType;
    use super::super::test_tree::node;
    use super::*;

    fn nodes<const N: usize>(node_type: NodeType) -> [NodeRef; N] {
        let main_thread = MainThread::for_test();
        core::array::from_fn(|_| NodeRef::from(node(&main_thread, node_type)))
    }

    fn children(parent: NodeRef) -> Vec<NodeRef> {
        core::iter::successors(parent.first_child(), |child| child.next_sibling()).collect()
    }

    fn children_backwards(parent: NodeRef) -> Vec<NodeRef> {
        let mut children: Vec<NodeRef> =
            core::iter::successors(parent.last_child(), |child| child.previous_sibling()).collect();
        children.reverse();
        children
    }

    #[test]
    fn insertion_links_a_node_among_its_siblings() {
        let mut main_thread = MainThread::for_test();
        let [parent, a, b, c, d] = nodes(NodeType::ELEMENT_NODE);
        insert_child(&mut main_thread, parent, b, None);
        insert_child(&mut main_thread, parent, a, Some(b));
        insert_child(&mut main_thread, parent, d, None);
        insert_child(&mut main_thread, parent, c, Some(d));
        assert_eq!(children(parent), [a, b, c, d]);
        assert_eq!(children_backwards(parent), [a, b, c, d]);
        for child in [a, b, c, d] {
            assert_eq!(child.parent(), Some(parent));
            assert_eq!(child.root(), parent);
        }
    }

    #[test]
    fn removal_unlinks_a_node_from_its_siblings() {
        let mut main_thread = MainThread::for_test();
        let [parent, a, b, c] = nodes(NodeType::ELEMENT_NODE);
        for child in [a, b, c] {
            insert_child(&mut main_thread, parent, child, None);
        }
        remove_child(&mut main_thread, parent, b);
        assert_eq!(children(parent), [a, c]);
        assert_eq!(children_backwards(parent), [a, c]);
        assert_eq!((b.parent(), b.previous_sibling(), b.next_sibling()), (None, None, None));
        assert_eq!(b.root(), b);

        remove_child(&mut main_thread, parent, a);
        remove_child(&mut main_thread, parent, c);
        assert_eq!(children(parent), []);
        assert_eq!((parent.first_child(), parent.last_child()), (None, None));
    }

    #[test]
    fn a_subtree_takes_the_root_of_the_tree_it_joins_and_heads_its_own_when_it_leaves() {
        let mut main_thread = MainThread::for_test();
        let [root, outer, inner, deepest, sibling] = nodes(NodeType::ELEMENT_NODE);
        insert_child(&mut main_thread, inner, deepest, None);
        insert_child(&mut main_thread, outer, inner, None);
        insert_child(&mut main_thread, outer, sibling, None);
        assert_eq!(deepest.root(), outer);

        insert_child(&mut main_thread, root, outer, None);
        for node in [outer, inner, deepest, sibling] {
            assert_eq!(node.root(), root);
        }

        remove_child(&mut main_thread, outer, inner);
        assert_eq!([inner.root(), deepest.root()], [inner, inner]);
        assert_eq!([outer.root(), sibling.root()], [root, root]);
    }

    #[test]
    #[should_panic(expected = "an inserted node has no parent")]
    fn a_node_with_a_parent_cannot_be_inserted() {
        let mut main_thread = MainThread::for_test();
        let [first_parent, second_parent, child] = nodes(NodeType::ELEMENT_NODE);
        insert_child(&mut main_thread, first_parent, child, None);
        insert_child(&mut main_thread, second_parent, child, None);
    }
}
