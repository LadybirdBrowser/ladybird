/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The version counters of the trees of nodes, for caches that depend on a tree's structure or contents.
//!
//! The DOM tree version of a tree moves whenever a node is inserted into or removed from it, or an element attribute in
//! it changes; its character data version whenever CharacterData in it is modified. A connected node's tree is its
//! document; a disconnected node's is the tree under its root. Counting per tree keeps a document's churn from
//! invalidating caches keyed on disconnected trees, and vice versa. The root that heads a tree keeps its counters in its
//! ParentNodeCore; a lone node that cannot have children heads a tree nothing can be cached on, so it keeps none.
//!
//! Every move takes a stamp from one counter no tree has had before, so a version remembered from one tree is never
//! mistaken for the version of another.

use super::node::NodeCell;
use super::node_core::ParentNodeCore;
use super::node_ref::NodeRef;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

#[cfg(not(test))]
mod last_stamp {
    use core::cell::UnsafeCell;

    use crate::stage::MainThread;

    /// The last stamp handed out to a tree.
    #[repr(transparent)]
    struct LastStamp(UnsafeCell<u64>);

    // SAFETY: Only the main thread reads or writes the stamp, which every access proves with its token.
    unsafe impl Sync for LastStamp {}

    static LAST_STAMP: LastStamp = LastStamp(UnsafeCell::new(0));

    #[inline(always)]
    pub(super) fn get(_main_thread: &MainThread<'_>) -> u64 {
        // SAFETY: Only the main thread reaches the stamp, see LastStamp.
        unsafe { *LAST_STAMP.0.get() }
    }

    #[inline(always)]
    pub(super) fn take_next(_main_thread: &mut MainThread<'_>) -> u64 {
        // SAFETY: Only the main thread reaches the stamp, see LastStamp, and nothing borrows it across this.
        unsafe {
            let stamp = LAST_STAMP.0.get();
            *stamp += 1;
            *stamp
        }
    }
}

// NB: Unit tests run on many threads, each of which stands in for the main thread of its own DOM.
#[cfg(test)]
mod last_stamp {
    use core::cell::Cell;

    use crate::stage::MainThread;

    std::thread_local! {
        static LAST_STAMP: Cell<u64> = const { Cell::new(0) };
    }

    pub(super) fn get(_main_thread: &MainThread<'_>) -> u64 {
        LAST_STAMP.get()
    }

    pub(super) fn take_next(_main_thread: &mut MainThread<'_>) -> u64 {
        LAST_STAMP.set(LAST_STAMP.get() + 1);
        LAST_STAMP.get()
    }
}

/// The last stamp any tree's version took, which moves whenever a version of any tree does.
#[inline(always)]
pub(crate) fn last_tree_version_stamp(main_thread: &MainThread<'_>) -> u64 {
    last_stamp::get(main_thread)
}

/// Calls `callback` with the counters of the tree `node` is in: its document's while it is connected, otherwise its
/// root's, unless that root cannot have children.
#[inline(always)]
fn with_tree_version_holder(node: NodeRef, callback: impl FnOnce(&ParentNodeCore)) {
    let holder = match node.is_connected() {
        true => node.document(),
        false => node.root(),
    };
    if let Some(core) = holder.parent_node_core() {
        callback(core);
    }
}

/// Moves the DOM tree version of the tree `node` is in.
#[inline(always)]
pub(crate) fn bump_dom_tree_version(main_thread: &mut MainThread<'_>, node: NodeRef) {
    with_tree_version_holder(node, |core| {
        core.set_dom_tree_version(last_stamp::take_next(main_thread));
    });
}

/// Moves the character data version of the tree `node` is in.
#[inline(always)]
fn bump_character_data_version(main_thread: &mut MainThread<'_>, node: NodeRef) {
    with_tree_version_holder(node, |core| {
        core.set_character_data_version(last_stamp::take_next(main_thread));
    });
}

/// Moves the DOM tree version of the tree `node` is in, for DOM::Node.
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_bump_dom_tree_version(node: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    bump_dom_tree_version(&mut main_thread, node);
}

/// Moves the character data version of the tree `node` is in, for DOM::Node.
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_bump_character_data_version(node: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    bump_character_data_version(&mut main_thread, node);
}

/// Starts both version counters of the tree `shadow_root` heads over, with a stamp no tree has had, as it leaves the
/// document whose counters its tree followed while it was connected, for DOM::Node.
///
/// # Safety
///
/// The shadow root must be live and not connected, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_restart_tree_versions(shadow_root: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let shadow_root = unsafe { NodeRef::from_cell(shadow_root, &main_thread) };
    let stamp = last_stamp::take_next(&mut main_thread);
    let core = shadow_root.parent_node_core().expect("a shadow root is a ParentNode");
    core.set_dom_tree_version(stamp);
    core.set_character_data_version(stamp);
}

#[cfg(test)]
mod tests {
    use super::super::links::insert_child;
    use super::super::node_core::NodeType;
    use super::super::test_tree;
    use super::*;

    #[test]
    fn a_change_moves_the_version_of_the_tree_it_is_in_only() {
        let mut main_thread = MainThread::for_test();
        let [root, child, other_root] =
            [NodeType::ELEMENT_NODE; 3].map(|node_type| NodeRef::from(test_tree::node(&main_thread, node_type)));
        insert_child(&mut main_thread, root, child, None);
        let before = (dom_tree_version(root), dom_tree_version(other_root));

        bump_dom_tree_version(&mut main_thread, child);
        assert_ne!(dom_tree_version(root), before.0);
        assert_eq!(dom_tree_version(child), dom_tree_version(root));
        assert_eq!(dom_tree_version(other_root), before.1);
        assert_eq!(dom_tree_version(root), last_tree_version_stamp(&main_thread));
    }

    #[test]
    fn a_tree_whose_root_cannot_have_children_has_no_version() {
        let mut main_thread = MainThread::for_test();
        let text = NodeRef::from(test_tree::node(&main_thread, NodeType::TEXT_NODE));
        let stamp = last_tree_version_stamp(&main_thread);
        bump_dom_tree_version(&mut main_thread, text);
        assert_eq!(dom_tree_version(text), 0);
        assert_eq!(last_tree_version_stamp(&main_thread), stamp);
    }
}
