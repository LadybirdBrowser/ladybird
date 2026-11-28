/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Removing a node from its parent.
//! https://dom.spec.whatwg.org/#concept-node-remove

use super::hooks;
use super::hooks::IsSubtreeRoot;
use super::links;
use super::node::NodeCell;
use super::node_ref::NodeRef;
use super::tree::for_each_shadow_including_descendant;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// https://dom.spec.whatwg.org/#concept-node-remove
pub(crate) fn remove(main_thread: &mut MainThread<'_>, node: NodeRef, suppress_observers: bool) {
    // To remove a node node, with an optional boolean suppressObservers (default false):

    // 1. Let parent be node’s parent.
    let parent = node.parent();

    // 2. Assert: parent is non-null.
    let parent = parent.expect("a removed node has a parent");

    // 3. Run the live range pre-remove steps, given node.
    // 4. For each NodeIterator object iterator whose root’s node document is node’s node document: run the NodeIterator
    //    pre-remove steps given node and iterator.
    // NB: The callback also does what the removal does for the editing history, the page, list numbering, slots, layout
    //     and the style engine while the subtree still stands where it did, see hooks::prepare_removal().
    let facts = hooks::prepare_removal(main_thread, node, parent);

    // 5. Let oldPreviousSibling be node’s previous sibling.
    let old_previous_sibling = node.previous_sibling();

    // 6. Let oldNextSibling be node’s next sibling.
    let old_next_sibling = node.next_sibling();

    let parent_root = parent.root();

    // 7. Remove node from its parent’s children.
    links::remove_child(main_thread, parent, node);

    // 8. If node is assigned, then run assign slottables for node’s assigned slot.
    // 9. If parent’s root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run signal
    //    a slot change for parent.
    // 10. If node has an inclusive descendant that is a slot:
    //     1. Run assign slottables for a tree with parent’s root.
    //     2. Run assign slottables for a tree with node.
    hooks::assign_slottables_after_removal(main_thread, node, parent, parent_root, facts);

    run_removing_steps_for_subtree(
        main_thread,
        node,
        parent,
        parent_root,
        facts.was_tracked_by_style_engine,
    );

    // 15. Add transient registered observers given node and parent.
    // 16. If suppressObservers is false, then queue a tree mutation record for parent with « », « node »,
    //     oldPreviousSibling, and oldNextSibling.
    // 17. Run the children changed steps for parent.
    hooks::finish_removal(
        main_thread,
        node,
        parent,
        old_previous_sibling,
        old_next_sibling,
        suppress_observers,
    );
}

/// The steps of removing `node`, which has left `parent`'s children, that run the removing steps for it and its
/// shadow-including descendants and enqueue their "disconnectedCallback" reactions.
/// https://dom.spec.whatwg.org/#concept-node-remove
fn run_removing_steps_for_subtree(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    parent_root: NodeRef,
    was_tracked_by_style_engine: bool,
) {
    // 11. Let isParentConnected be parent’s connected.
    // 12. If node is custom and isParentConnected is true, then enqueue a custom element callback reaction with node,
    //     callback name "disconnectedCallback", and « ».
    // 13. Run the removing steps with node, true, and parent.
    // AD-HOC: The removing steps run first, and isParentConnected is read after them, by the callback for node. The
    //         same goes for each descendant below, whose removing steps run before its reaction is enqueued.
    hooks::run_removing_steps(
        main_thread,
        node,
        IsSubtreeRoot::Yes,
        parent,
        parent_root,
        was_tracked_by_style_engine,
        false,
    );
    let is_parent_connected = parent.is_connected();

    // 14. For each shadow-including descendant descendant of node, in shadow-including tree order:
    //     1. If descendant is custom and isParentConnected is true, then enqueue a custom element callback reaction
    //        with descendant, callback name "disconnectedCallback", and « ».
    //     2. Run the removing steps with descendant, false, and parent.
    for_each_shadow_including_descendant(node, &mut |descendant| {
        hooks::run_removing_steps(
            main_thread,
            descendant,
            IsSubtreeRoot::No,
            parent,
            parent_root,
            was_tracked_by_style_engine,
            is_parent_connected,
        );
    });
}

/// Removes `node` from its parent, for DOM::Node.
///
/// # Safety
///
/// `node` must be a live node that has a parent, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove(node: &NodeCell, suppress_observers: bool) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    remove(&mut main_thread, node, suppress_observers);
}

/// Runs the removing steps for `node`, which has left `parent`'s children, and its shadow-including descendants, for
/// DOM::Node.
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_run_removing_steps(
    node: &NodeCell,
    parent: &NodeCell,
    parent_root: &NodeCell,
    was_tracked_by_style_engine: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (node, parent, parent_root) = (reference(node), reference(parent), reference(parent_root));
    run_removing_steps_for_subtree(&mut main_thread, node, parent, parent_root, was_tracked_by_style_engine);
}
