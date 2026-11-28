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
//!
//! A document also counts the changes to the children of the nodes whose node document it is, connected or not, for the
//! live collections, see collection.rs: its node tree version moves whenever a node joins or leaves a node's children,
//! and its element tree version whenever an element does. These move as the child is linked or unlinked, before any
//! step that could run script follows.

use super::node::NodeCell;
use super::node_core::ParentNodeCore;
use super::node_ref::NodeRef;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// The stamps handed out so far: the last one, and the last one a DOM tree or character data version took.
#[derive(Clone, Copy, Default)]
struct Stamps {
    last: u64,
    last_tree_version: u64,
}

#[cfg(not(test))]
mod last_stamp {
    use core::cell::UnsafeCell;

    use super::Stamps;
    use crate::stage::MainThread;

    #[repr(transparent)]
    struct LastStamp(UnsafeCell<Stamps>);

    // SAFETY: Only the main thread reads or writes the stamps, which every access proves with its token.
    unsafe impl Sync for LastStamp {}

    static LAST_STAMP: LastStamp = LastStamp(UnsafeCell::new(Stamps {
        last: 0,
        last_tree_version: 0,
    }));

    #[inline(always)]
    pub(super) fn get(_main_thread: &MainThread<'_>) -> Stamps {
        // SAFETY: Only the main thread reaches the stamps, see LastStamp.
        unsafe { *LAST_STAMP.0.get() }
    }

    #[inline(always)]
    pub(super) fn set(_main_thread: &mut MainThread<'_>, stamps: Stamps) {
        // SAFETY: Only the main thread reaches the stamps, see LastStamp, and nothing borrows them across this.
        unsafe { *LAST_STAMP.0.get() = stamps };
    }
}

// NB: Unit tests run on many threads, each of which stands in for the main thread of its own DOM.
#[cfg(test)]
mod last_stamp {
    use core::cell::Cell;

    use super::Stamps;
    use crate::stage::MainThread;

    std::thread_local! {
        static LAST_STAMP: Cell<Stamps> = const { Cell::new(Stamps { last: 0, last_tree_version: 0 }) };
    }

    pub(super) fn get(_main_thread: &MainThread<'_>) -> Stamps {
        LAST_STAMP.get()
    }

    pub(super) fn set(_main_thread: &mut MainThread<'_>, stamps: Stamps) {
        LAST_STAMP.set(stamps);
    }
}

/// A stamp no tree has had before.
#[inline(always)]
pub(super) fn take_next_stamp(main_thread: &mut MainThread<'_>) -> u64 {
    let mut stamps = last_stamp::get(main_thread);
    stamps.last += 1;
    last_stamp::set(main_thread, stamps);
    stamps.last
}

/// A stamp no tree has had before, for a DOM tree or character data version.
#[inline(always)]
fn take_next_tree_version_stamp(main_thread: &mut MainThread<'_>) -> u64 {
    let mut stamps = last_stamp::get(main_thread);
    stamps.last += 1;
    stamps.last_tree_version = stamps.last;
    last_stamp::set(main_thread, stamps);
    stamps.last
}

/// The last stamp handed out, which moves whenever any version that takes stamps does.
#[inline(always)]
pub(super) fn last_stamp(main_thread: &MainThread<'_>) -> u64 {
    last_stamp::get(main_thread).last
}

/// The last stamp the DOM tree or character data version of any tree took, which moves whenever one of those does.
#[inline(always)]
pub(crate) fn last_tree_version_stamp(main_thread: &MainThread<'_>) -> u64 {
    last_stamp::get(main_thread).last_tree_version
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

/// The DOM tree version of the tree `node` is in, which is 0 for a tree whose root cannot have children.
#[inline(always)]
pub(crate) fn dom_tree_version(node: NodeRef) -> u64 {
    let mut version = 0;
    with_tree_version_holder(node, |core| version = core.dom_tree_version());
    version
}

/// Moves the node tree version of `parent`'s node document, and its element tree version if `elements_changed`, as a
/// node joins or leaves `parent`'s children.
#[inline(always)]
pub(super) fn child_list_changed(main_thread: &mut MainThread<'_>, parent: NodeRef, elements_changed: bool) {
    let stamp = take_next_stamp(main_thread);
    parent.document_core().set_tree_versions(stamp, elements_changed);
}

/// Moves the DOM tree version of the tree `node` is in.
#[inline(always)]
pub(crate) fn bump_dom_tree_version(main_thread: &mut MainThread<'_>, node: NodeRef) {
    with_tree_version_holder(node, |core| {
        core.set_dom_tree_version(take_next_tree_version_stamp(main_thread));
    });
}

/// Moves the character data version of the tree `node` is in.
#[inline(always)]
fn bump_character_data_version(main_thread: &mut MainThread<'_>, node: NodeRef) {
    with_tree_version_holder(node, |core| {
        core.set_character_data_version(take_next_tree_version_stamp(main_thread));
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
    let stamp = take_next_tree_version_stamp(&mut main_thread);
    let core = shadow_root.parent_node_core().expect("a shadow root is a ParentNode");
    core.set_dom_tree_version(stamp);
    core.set_character_data_version(stamp);
}

/// Starts the node and element tree versions of `document` with a stamp no document has had, as C++ constructs it, so
/// that what a collection found under another document's versions is never taken to hold under this one's.
///
/// # Safety
///
/// The document must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_start_tree_versions_of_document(document: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The document is live.
    let document = unsafe { NodeRef::from_cell(document, &main_thread) };
    let stamp = take_next_stamp(&mut main_thread);
    document.document_core().set_tree_versions(stamp, true);
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
