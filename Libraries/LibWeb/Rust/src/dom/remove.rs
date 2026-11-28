/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Removing a node from its parent.
//! https://dom.spec.whatwg.org/#concept-node-remove

use super::hooks;
use super::hooks::CustomElementCallback;
use super::hooks::IsSubtreeRoot;
use super::node::NodeCell;
use super::node_ref::NodeRef;
use super::tree::for_each_shadow_including_descendant;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

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
    // AD-HOC: The removing steps run first, and isParentConnected is read after them. The same goes for each
    //         descendant below, whose removing steps run before its reaction is enqueued.
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
    if is_parent_connected && node.is_element() {
        hooks::enqueue_custom_element_callback_reaction(main_thread, node, CustomElementCallback::Disconnected);
    }

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
