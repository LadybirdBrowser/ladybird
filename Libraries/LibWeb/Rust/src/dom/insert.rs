/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Inserting nodes into a parent.
//! https://dom.spec.whatwg.org/#concept-node-insert

use super::hooks;
use super::hooks::InsertionStart;
use super::node::NodeCell;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::node_ref::RawNullableNodeRef;
use super::tree::for_each_shadow_including_inclusive_descendant;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// https://dom.spec.whatwg.org/#concept-node-insert
///
/// `nodes` are the nodes of step 1, which the caller collects into a span it roots in a frame below this one.
fn insert(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    nodes: &[NodeRef],
    parent: NodeRef,
    child: Option<NodeRef>,
    suppress_observers: bool,
) {
    // To insert a node node into a node parent before null or a node child, with an optional boolean suppressObservers
    // (default false):

    // 1. Let nodes be node’s children, if node is a DocumentFragment node; otherwise « node ».
    debug_assert!(node.node_type() == NodeType::DOCUMENT_FRAGMENT_NODE || nodes == [node]);

    // 2. Let count be nodes’s size.
    let count = nodes.len();

    // 3. If count is 0, then return.
    if count == 0 {
        return;
    }

    // 4. If node is a DocumentFragment node:
    //    1. Remove its children with suppressObservers set to true.
    //    2. Queue a tree mutation record for node with « », nodes, null, and null.
    //       NOTE: This step intentionally does not pay attention to suppressObservers.
    // 5. If child is non-null:
    //    1. For each live range whose start node is parent and start offset is greater than child’s index: increase its
    //       start offset by count.
    //    2. For each live range whose end node is parent and end offset is greater than child’s index: increase its end
    //       offset by count.
    // NB: The callback also consumes the deferred style change events of the documents, and answers whether the
    //     insertion affects the element children of parent, and which tree version the steps below start from.
    let start = hooks::begin_insertion(main_thread, parent, node, nodes, child);

    insert_nodes(main_thread, parent, node, nodes, child, suppress_observers, start);
}

/// Steps 6 and on of inserting `node`, whose nodes are `nodes`, into `parent` before `child`, which `start` begins.
/// https://dom.spec.whatwg.org/#concept-node-insert
fn insert_nodes(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: NodeRef,
    nodes: &[NodeRef],
    child: Option<NodeRef>,
    suppress_observers: bool,
    start: InsertionStart,
) {
    let InsertionStart {
        affects_elements,
        tree_version: tree_version_before_insertion_steps,
    } = start;

    // 6. Let previousSibling be child’s previous sibling or parent’s last child if child is null.
    let previous_sibling = match child {
        Some(child) => child.previous_sibling(),
        None => parent.last_child(),
    };

    // NB: Set by the insertion steps below. Any later change to the tree, such as by script run from the children
    //     changed steps, can add other nodes to the ones the post-connection steps are collected from.
    let mut any_node_has_post_connection_steps = false;

    // 7. For each node in nodes, in tree order:
    for &node in nodes {
        // 1. Adopt node into parent’s node document.
        // 2. If child is null, then append node to parent’s children.
        // 3. Otherwise, insert node into parent’s children before child’s index.
        // 4. If parent is a shadow host whose shadow root’s slot assignment is "named" and node is a slottable, then
        //    assign a slot for node.
        // 5. If parent’s root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run
        //    signal a slot change for parent.
        // 6. Run assign slottables for a tree with node’s root.
        // NB: The callback also records the arrival of the subtree for the style engine, and then calls
        //     rust_dom_run_insertion_steps() for the walk below, inside a C++ scope that serves the insertion steps of
        //     the subtree.
        hooks::insert_node_into_children(
            main_thread,
            parent,
            node,
            child,
            &mut any_node_has_post_connection_steps,
        );
    }

    // 8. If suppressObservers is false, then queue a tree mutation record for parent with nodes, « », previousSibling,
    //    and child.
    // 9. Run the children changed steps for parent.
    // NB: The callback also invalidates the collection caches, and marks the layout tree for the inserted nodes before
    //     the post-connection steps, which can run script that reads layout geometry. It answers which tree version
    //     the steps below start from.
    let tree_version = hooks::end_insertion(
        main_thread,
        parent,
        node,
        nodes,
        previous_sibling,
        child,
        suppress_observers,
        affects_elements,
    );

    // 10. Let staticNodeList be a list of nodes, initially « ».
    // 11. For each node of nodes, in tree order:
    //     1. For each shadow-including inclusive descendant inclusiveDescendant of node, in shadow-including tree
    //        order: append inclusiveDescendant to staticNodeList.
    // 12. For each node of staticNodeList: if node is connected, then run the post-connection steps with node.
    // OPTIMIZATION: Disconnected subtrees cannot have post-connection steps to run. If any root is connected, collect
    //               all nodes, since a callback could connect one of the initially detached subtrees. Unless some tree
    //               changed since the insertion steps began, the nodes to collect are the ones those steps ran for, so
    //               without one that has post-connection steps there is nothing to run.
    let tree_changed_since_insertion_steps = tree_version != tree_version_before_insertion_steps;
    let run_post_connection_steps = (any_node_has_post_connection_steps || tree_changed_since_insertion_steps)
        && nodes.iter().any(|node| node.is_connected());

    // NB: The callback also schedules the list item renumbering the insertion causes, and moves the tree version.
    hooks::finish_insertion(main_thread, parent, nodes, run_post_connection_steps);
}

/// Step 7.7 of inserting nodes: the walk over the shadow-including inclusive descendants of `node`, which has just
/// joined its parent's children.
/// https://dom.spec.whatwg.org/#concept-node-insert
fn run_insertion_steps_for_subtree(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    any_node_has_post_connection_steps: &mut bool,
) {
    // 7. For each shadow-including inclusive descendant inclusiveDescendant of node, in shadow-including tree order:
    //    1. If inclusiveDescendant is connected, inclusiveDescendant is an element, and inclusiveDescendant’s custom
    //       element registry is non-null:
    //       1. If inclusiveDescendant’s custom element registry’s is scoped is true, then append inclusiveDescendant’s
    //          node document to inclusiveDescendant’s custom element registry’s scoped document set.
    //       2. If inclusiveDescendant is custom, then enqueue a custom element callback reaction with
    //          inclusiveDescendant, callback name "connectedCallback", and « ».
    //       3. Otherwise, try to upgrade inclusiveDescendant.
    //    2. Otherwise, if inclusiveDescendant is connected, inclusiveDescendant is a shadow root, inclusiveDescendant’s
    //       custom element registry is non-null, and inclusiveDescendant’s custom element registry’s is scoped is true,
    //       then append inclusiveDescendant’s node document to inclusiveDescendant’s custom element registry’s scoped
    //       document set.
    //    3. Run the insertion steps with inclusiveDescendant.
    // AD-HOC: The insertion steps run first, and inclusiveDescendant's connectedness is read after them.
    for_each_shadow_including_inclusive_descendant(node, &mut |inclusive_descendant| {
        if hooks::run_insertion_steps(main_thread, inclusive_descendant) {
            *any_node_has_post_connection_steps = true;
        }
    });
}

/// Inserts `node`, whose nodes are `nodes`, into `parent` before `child`, for DOM::Node.
///
/// # Safety
///
/// The nodes must be live and rooted for the call, `nodes` must be `node`'s children if it is a DocumentFragment and
/// « node » otherwise, `child` must be a child of `parent`, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_insert(
    parent: &NodeCell,
    node: &NodeCell,
    nodes: ak::Span<'_, NodeRef>,
    child: RawNullableNodeRef,
    suppress_observers: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (parent, node) = (reference(parent), reference(node));
    insert(
        &mut main_thread,
        node,
        nodes.as_slice(),
        parent,
        child.into(),
        suppress_observers,
    );
}

/// Inserts `nodes`, the nodes that replace all of `parent`'s children, which it has none of anymore, into `parent`
/// before null with suppressObservers set to true, for DOM::Node, which has run the steps `start` answers for.
///
/// # Safety
///
/// The nodes must be live and rooted for the call, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_insert_nodes_replacing_all(
    parent: &NodeCell,
    nodes: ak::Span<'_, NodeRef>,
    start: InsertionStart,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let parent = unsafe { NodeRef::from_cell(parent, &main_thread) };
    insert_nodes(&mut main_thread, parent, parent, nodes.as_slice(), None, true, start);
}

/// Runs step 7.7 of inserting nodes for `node`, which has just joined its parent's children, setting
/// `any_node_has_post_connection_steps` if a node it ran the insertion steps for has post-connection steps, for the
/// callback that serves step 7 for a node.
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_run_insertion_steps(node: &NodeCell, any_node_has_post_connection_steps: &mut bool) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    run_insertion_steps_for_subtree(&mut main_thread, node, any_node_has_post_connection_steps);
}
