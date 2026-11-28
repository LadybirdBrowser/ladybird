/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The table of C++ callbacks through which the DOM algorithms in Rust run a node's virtual hooks, such as its insertion
//! and removing steps, and the steps of the systems whose state is still C++'s: live ranges, NodeIterators, slots,
//! mutation records, custom element reactions, the style engine's records, layout tree updates and collection caches.
//!
//! C++ registers the table once, before any node exists. Every wrapper takes the main thread token mutably, so no read
//! handle lives across a call into C++. The documentation of each wrapper says whether its callback can run script,
//! allocate or change the tree; code that calls one that can holds its nodes as `NodeRef`s and reads every link it
//! needs again after it returns.

use core::cell::UnsafeCell;

use super::node_ref::NodeRef;
use crate::stage::MainThread;

/// Whether a node a removing step runs for is the root of the subtree that is removed, laid out as C++'s
/// DOM::Node::IsSubtreeRoot, which is this type.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IsSubtreeRoot {
    No,
    Yes,
}

/// A custom element callback reaction the DOM algorithms enqueue, as HTML::CustomElementReactionNames names it.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CustomElementCallback {
    /// "disconnectedCallback"
    Disconnected,
}

/// The callbacks, as DOM/Node.cpp registers them. Each takes the nodes it acts on as DOM::Node pointers.
#[repr(C)]
pub struct DomHooks {
    /// See [`run_removing_steps`].
    pub run_removing_steps: unsafe extern "C" fn(
        node: RawNodeRef,
        is_subtree_root: IsSubtreeRoot,
        parent: RawNodeRef,
        parent_root: RawNodeRef,
        was_tracked_by_style_engine: bool,
        is_parent_connected: bool,
    ),
    /// See [`enqueue_custom_element_callback_reaction`].
    pub enqueue_custom_element_callback_reaction: unsafe extern "C" fn(node: NodeRef, callback: CustomElementCallback),
}

/// The registered table, which C++ sets once, before any node exists, and never changes after that.
#[repr(transparent)]
struct RegisteredHooks(UnsafeCell<Option<&'static DomHooks>>);

// SAFETY: The table is set once, before any node exists, and only the main thread, which made every node since, reads
//         it.
unsafe impl Sync for RegisteredHooks {}

static HOOKS: RegisteredHooks = RegisteredHooks(UnsafeCell::new(None));

#[inline(always)]
fn hooks(_main_thread: &mut MainThread<'_>) -> &'static DomHooks {
    // SAFETY: Nothing writes the table after the first node exists, see RegisteredHooks.
    unsafe { *HOOKS.0.get() }.expect("the DOM hooks are registered before any node exists")
}

/// Registers the DOM's callbacks.
///
/// # Safety
///
/// No node may exist yet, and the call must be made on the main thread, before any other thread starts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_register_hooks(hooks: &'static DomHooks) {
    // SAFETY: Guaranteed by the caller.
    unsafe { *HOOKS.0.get() = Some(hooks) };
}

/// Runs the removing steps with `node`, `is_subtree_root` and `parent`, as Node::removed_from() does, after cancelling
/// the CSS animations and transitions of an element the style engine tracked. Then, if `is_parent_connected`, enqueues
/// a "disconnectedCallback" custom element callback reaction for `node` if it is custom.
///
/// Can run script, allocate and change the tree.
#[allow(clippy::too_many_arguments)]
pub(crate) fn run_removing_steps(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    is_subtree_root: IsSubtreeRoot,
    parent: NodeRef,
    parent_root: NodeRef,
    was_tracked_by_style_engine: bool,
    is_parent_connected: bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (hooks(main_thread).run_removing_steps)(
            node,
            is_subtree_root,
            parent,
            parent_root,
            was_tracked_by_style_engine,
            is_parent_connected,
        );
    }
}

/// Enqueues the custom element callback reaction `callback` with `node` and « », if `node` is a custom element.
///
/// Allocates. Runs no script.
pub(crate) fn enqueue_custom_element_callback_reaction(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    callback: CustomElementCallback,
) {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe {
        (hooks(main_thread).enqueue_custom_element_callback_reaction)(node, callback);
    }
}
