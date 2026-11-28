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
use super::steps::assign_slottables_after_removal_does_nothing;
use super::steps::removing_steps_do_nothing;
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
    let facts = hooks::prepare_removal(main_thread, node, parent, None);

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
    // OPTIMIZATION: The cores and the facts tell when the callback has nothing to do, see steps.rs.
    if !assign_slottables_after_removal_does_nothing(node, parent_root, facts) {
        hooks::assign_slottables_after_removal(main_thread, node, parent, parent_root, facts);
    }

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
        false,
    );
}

/// Removes all of `parent`'s children, in tree order, with `suppress_observers`.
/// https://dom.spec.whatwg.org/#concept-node-remove
pub(crate) fn remove_all_children(main_thread: &mut MainThread<'_>, parent: NodeRef, suppress_observers: bool) {
    // AD-HOC: The remove algorithm for each child in tree order. The steps that concern only the parent run once for
    //         all of the children, as noted under each. Nothing that runs between two removals can run script.

    if parent.first_child().is_none() {
        return;
    }

    // 3. Run the live range pre-remove steps, given node.
    // NB: Run once for all of the children before any is removed, by the callback, which also tells the editing
    //     history and consumes the deferred style change events. Each child is the first child when its turn comes,
    //     and the steps for every child together leave each boundary point that was in a child or on parent at
    //     (parent, 0).
    let mut children_removal = hooks::begin_removing_all_children(main_thread, parent);
    let parent_root = parent.root();

    while let Some(node) = parent.first_child() {
        // 1. Let parent be node’s parent.
        // 2. Assert: parent is non-null.
        // NB: The parent is the parent of every child removed here.

        // 4. For each NodeIterator object iterator whose root’s node document is node’s node document: run the
        //    NodeIterator pre-remove steps given node and iterator.
        // NB: The callback also does what the removal does for the page, list numbering, slots, layout and the style
        //     engine while the subtree still stands where it did, and keeps what every child shares in
        //     children_removal.
        let facts = hooks::prepare_removal(main_thread, node, parent, Some(&mut children_removal));

        // 5. Let oldPreviousSibling be node’s previous sibling.
        // NB: The child removed is always the first, so oldPreviousSibling is always null.

        // 6. Let oldNextSibling be node’s next sibling.
        let old_next_sibling = node.next_sibling();

        // 7. Remove node from its parent’s children.
        links::remove_child(main_thread, parent, node);

        // 8. If node is assigned, then run assign slottables for node’s assigned slot.
        // 9. If parent’s root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run
        //    signal a slot change for parent.
        // 10. If node has an inclusive descendant that is a slot:
        //     1. Run assign slottables for a tree with parent’s root.
        //     2. Run assign slottables for a tree with node.
        // OPTIMIZATION: The cores and the facts tell when the callback has nothing to do, see steps.rs.
        if !assign_slottables_after_removal_does_nothing(node, parent_root, facts) {
            hooks::assign_slottables_after_removal(main_thread, node, parent, parent_root, facts);
        }

        // 11. Let isParentConnected be parent’s connected.
        // 12. If node is custom and isParentConnected is true, then enqueue a custom element callback reaction with
        //     node, callback name "disconnectedCallback", and « ».
        // 13. Run the removing steps with node, true, and parent.
        // 14. For each shadow-including descendant descendant of node, in shadow-including tree order: ...
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
        // NB: Step 17 runs once after the last child has left, by the callback below. The runs in between would only
        //     redo work on children that are about to leave.
        hooks::finish_removal(
            main_thread,
            node,
            parent,
            None,
            old_next_sibling,
            suppress_observers,
            true,
        );
    }

    // 17. Run the children changed steps for parent.
    hooks::end_removing_all_children(main_thread, parent, children_removal);
}

/// https://dom.spec.whatwg.org/#concept-node-pre-remove
///
/// Answers false where it would throw a "NotFoundError" DOMException.
fn pre_remove(main_thread: &mut MainThread<'_>, child: NodeRef, parent: NodeRef) -> bool {
    // To pre-remove a node child from a node parent:

    // 1. If child’s parent is not parent, then throw a "NotFoundError" DOMException.
    if child.parent() != Some(parent) {
        return false;
    }

    // 2. Remove child.
    remove(main_thread, child, false);

    // 3. Return child.
    true
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
    // OPTIMIZATION: The cores tell which nodes of a subtree the style engine did not track the removing steps have
    //               nothing to do for, see steps.rs. Neither is such a node custom with a connected parent.
    let steps_may_do_something =
        |node| was_tracked_by_style_engine || !removing_steps_do_nothing(node, parent, parent_root);
    if steps_may_do_something(node) {
        hooks::run_removing_steps(
            main_thread,
            node,
            IsSubtreeRoot::Yes,
            parent,
            parent_root,
            was_tracked_by_style_engine,
            false,
        );
    }
    let is_parent_connected = parent.is_connected();

    // 14. For each shadow-including descendant descendant of node, in shadow-including tree order:
    //     1. If descendant is custom and isParentConnected is true, then enqueue a custom element callback reaction
    //        with descendant, callback name "disconnectedCallback", and « ».
    //     2. Run the removing steps with descendant, false, and parent.
    for_each_shadow_including_descendant(node, &mut |descendant| {
        if !steps_may_do_something(descendant) {
            return;
        }
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

/// Removes all of `parent`'s children, for DOM::Node.
///
/// # Safety
///
/// `parent` must be a live node, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_all_children(parent: &NodeCell, suppress_observers: bool) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let parent = unsafe { NodeRef::from_cell(parent, &main_thread) };
    remove_all_children(&mut main_thread, parent, suppress_observers);
}

/// Pre-removes `child` from `parent`, for DOM::Node, and answers false where that throws a "NotFoundError"
/// DOMException.
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_pre_remove(parent: &NodeCell, child: &NodeCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (parent, child) = (reference(parent), reference(child));
    pre_remove(&mut main_thread, child, parent)
}
