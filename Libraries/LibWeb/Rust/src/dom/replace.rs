/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Replacing a child of a parent with a node, and all of a parent's children with a node.
//! https://dom.spec.whatwg.org/#concept-node-replace
//! https://dom.spec.whatwg.org/#concept-node-replace-all

use super::adopt::adopt;
use super::hooks;
use super::insert::insert;
use super::insert::insert_as_children_of_unobservable_fragment;
use super::node::NodeCell;
use super::node_ref::NodeRef;
use super::node_ref::RawNullableNodeRef;
use super::pre_insert::ChildrenToExclude;
use super::pre_insert::PreInsertValidity;
use super::pre_insert::ensure_pre_insert_validity_of;
use super::remove::remove;
use super::remove::remove_all_children;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// https://dom.spec.whatwg.org/#concept-node-replace
///
/// `nodes` are the nodes of step 8, which the caller collects into a span it roots in a frame below this one. Answers
/// which DOMException to throw where step 1 throws one.
fn replace(
    main_thread: &mut MainThread<'_>,
    child: NodeRef,
    node: NodeRef,
    parent: NodeRef,
    nodes: &[NodeRef],
) -> PreInsertValidity {
    // To replace a node child with a node node within a node parent:

    // 1. Ensure pre-insert validity given node, parent, child, and « child ».
    let validity = ensure_pre_insert_validity_of(main_thread, node, parent, Some(child), ChildrenToExclude::Child);
    if validity != PreInsertValidity::Valid {
        return validity;
    }

    // 2. Let referenceChild be child’s next sibling.
    let mut reference_child = child.next_sibling();

    // 3. If referenceChild is node, then set referenceChild to node’s next sibling.
    if reference_child == Some(node) {
        reference_child = node.next_sibling();
    }

    // 4. Let previousSibling be child’s previous sibling.
    let previous_sibling = child.previous_sibling();

    // 5. Let removedNodes be the empty set.
    let mut removed_nodes: &[NodeRef] = &[];

    // 6. Adopt node into parent’s node document.
    adopt(main_thread, node, parent.document());

    // 7. If child’s parent is non-null:
    //    NOTE: The above can only be false if child is node.
    let child_list = [child];
    if child.parent().is_some() {
        // 1. Set removedNodes to « child ».
        removed_nodes = &child_list;

        // 2. Remove child with suppressObservers set to true.
        remove(main_thread, child, true);
    }

    // 8. Let nodes be node’s children if node is a DocumentFragment node; otherwise « node ».
    // NB: The caller collected them before step 1. Nothing the steps since then run can change the children of a
    //     DocumentFragment, which is in no tree the removal of child touches.

    // AD-HOC: Since removing the child may have executed arbitrary code, we have to verify the sanity of inserting
    //         node before referenceChild again, as well as child not being reinserted elsewhere.
    if reference_child
        .is_none_or(|reference_child| reference_child.parent() == Some(parent) && child.parent().is_none())
    {
        // 9. Insert node into parent before referenceChild with suppressObservers set to true.
        insert(main_thread, node, nodes, parent, reference_child, true);
    }

    // 10. Queue a tree mutation record for parent with nodes, removedNodes, previousSibling, and referenceChild.
    hooks::queue_tree_mutation_record(
        main_thread,
        parent,
        nodes,
        removed_nodes,
        previous_sibling,
        reference_child,
    );

    // 11. Return child.
    PreInsertValidity::Valid
}

/// https://dom.spec.whatwg.org/#concept-node-replace-all
///
/// `added_nodes` and `removed_nodes` are the lists of steps 1 to 4, which the caller collects into spans it roots in a
/// frame below this one.
///
/// AD-HOC: With a null `node` and `added_nodes` that are not empty, inserts `added_nodes`, which have no parent and are
///         no DocumentFragment, as if `node` were a DocumentFragment that holds them and that no one can observe,
///         without making one.
fn replace_all(
    main_thread: &mut MainThread<'_>,
    node: Option<NodeRef>,
    parent: NodeRef,
    added_nodes: &[NodeRef],
    removed_nodes: &[NodeRef],
) {
    // To replace all with a node or null node within a node parent:

    // 1. Let removedNodes be parent’s children.
    // 2. Let addedNodes be the empty set.
    // 3. If node is a DocumentFragment node, then set addedNodes to node’s children.
    // 4. Otherwise, if node is non-null, set addedNodes to « node ».
    // NB: The caller collected both lists.

    // 5. Remove all parent’s children, in tree order, with suppressObservers set to true.
    remove_all_children(main_thread, parent, true);

    // 6. If node is non-null, then insert node into parent before null with suppressObservers set to true.
    match node {
        Some(node) => insert(main_thread, node, added_nodes, parent, None, true),
        None => insert_as_children_of_unobservable_fragment(main_thread, added_nodes, parent),
    }

    // 7. If either addedNodes or removedNodes is not empty, then queue a tree mutation record for parent with
    //    addedNodes, removedNodes, null, and null.
    if !added_nodes.is_empty() || !removed_nodes.is_empty() {
        hooks::queue_tree_mutation_record(main_thread, parent, added_nodes, removed_nodes, None, None);
    }
}

/// Replaces `child` with `node` within `parent`, for DOM::Node, and answers which DOMException to throw if it throws
/// one.
///
/// # Safety
///
/// The nodes must be live and rooted for the call, `nodes` must be `node`'s children if it is a DocumentFragment and
/// « node » otherwise, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_replace(
    parent: &NodeCell,
    node: &NodeCell,
    nodes: ak::Span<'_, NodeRef>,
    child: &NodeCell,
) -> PreInsertValidity {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (parent, node, child) = (reference(parent), reference(node), reference(child));
    replace(&mut main_thread, child, node, parent, nodes.as_slice())
}

/// Replaces all of `parent`'s children with `node`, for DOM::Node. With a null `node`, inserts `added_nodes` as the
/// children of a DocumentFragment no one can observe, see replace_all().
///
/// # Safety
///
/// The nodes must be live and rooted for the call, `removed_nodes` must be `parent`'s children, `added_nodes` must be
/// `node`'s children if it is a DocumentFragment, « node » if it is another node, and nodes that have no parent and
/// are no DocumentFragment otherwise, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_replace_all(
    parent: &NodeCell,
    node: RawNullableNodeRef,
    added_nodes: ak::Span<'_, NodeRef>,
    removed_nodes: ak::Span<'_, NodeRef>,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let parent = unsafe { NodeRef::from_cell(parent, &main_thread) };
    replace_all(
        &mut main_thread,
        node.into(),
        parent,
        added_nodes.as_slice(),
        removed_nodes.as_slice(),
    );
}
