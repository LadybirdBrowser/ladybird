/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Trees of nodes for unit tests: real cores, in cells of an arena that stands in for the GC heap region, which the
//! nodes' compressed pointers are offsets into.

use core::mem::MaybeUninit;
use core::mem::offset_of;
use std::sync::OnceLock;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use super::node::Node;
use super::node::NodeCell;
use super::node_core::DOCUMENT_FRAGMENT_HOST_OFFSET;
use super::node_core::NODE_CORE_OFFSET;
use super::node_core::NodeCore;
use super::node_core::NodeMember;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use crate::stage::MainThread;

/// A node's cell, with its core where DOM::Node has it, and a host where DOM::DocumentFragment has it.
#[repr(C, align(8))]
struct TestCell {
    header: [u8; NODE_CORE_OFFSET],
    core: NodeCore,
    fields: [u8; DOCUMENT_FRAGMENT_HOST_OFFSET - NODE_CORE_OFFSET - size_of::<NodeCore>()],
    host: NodeMember,
}

const _: () = assert!(offset_of!(TestCell, core) == NODE_CORE_OFFSET);
const _: () = assert!(offset_of!(TestCell, host) == DOCUMENT_FRAGMENT_HOST_OFFSET);

const ARENA_CELLS: usize = 1 << 14;

/// The cells of every test's nodes, which the heap region base of the process points just below.
struct Arena {
    cells: *mut MaybeUninit<TestCell>,
    next: AtomicUsize,
}

// SAFETY: Each cell is handed out once, to the test that allocates it.
unsafe impl Send for Arena {}
unsafe impl Sync for Arena {}

impl Arena {
    /// The base that the compressed pointers of the cells are offsets from, which keeps the first one from being 0.
    fn base(&self) -> usize {
        self.cells as usize - 8
    }
}

fn arena() -> &'static Arena {
    static ARENA: OnceLock<Arena> = OnceLock::new();
    ARENA.get_or_init(|| {
        let cells = Box::leak(Box::<[TestCell]>::new_uninit_slice(ARENA_CELLS)).as_mut_ptr();
        let arena = Arena {
            cells,
            next: AtomicUsize::new(0),
        };
        // SAFETY: No node exists yet, and every test reaches its nodes through this, which synchronizes with it.
        unsafe { super::node::set_heap_region_base_for_test(arena.base()) };
        arena
    })
}

fn compress(address: usize) -> NodePointer {
    let compressed = u32::try_from((address - arena().base()) >> 3).unwrap();
    NodePointer::from_compressed_for_test(compressed)
}

fn pointer_of(node: Node<'_>) -> NodePointer {
    compress(node.cell() as *const NodeCell as usize)
}

/// A new node of `node_type`, the root of a tree of its own.
pub(crate) fn node<'a>(main_thread: &'a MainThread<'_>, node_type: NodeType) -> Node<'a> {
    new_node(main_thread, node_type, |_| {})
}

/// A new shadow root of `host`.
pub(crate) fn shadow_root<'a>(main_thread: &'a MainThread<'_>, host: Node<'a>) -> Node<'a> {
    let shadow_root = new_node(
        main_thread,
        NodeType::DOCUMENT_FRAGMENT_NODE,
        NodeCore::set_is_shadow_root_for_test,
    );
    set_host(shadow_root, host);
    shadow_root
}

/// Sets the host of a DocumentFragment node.
pub(crate) fn set_host(fragment: Node<'_>, host: Node<'_>) {
    assert_eq!(fragment.node_type(), NodeType::DOCUMENT_FRAGMENT_NODE);
    // SAFETY: Every node of a test tree is a TestCell.
    let cell = unsafe { &*(fragment.cell() as *const NodeCell as *const TestCell) };
    cell.host.set(Some(pointer_of(host)));
}

fn new_node<'a>(
    main_thread: &'a MainThread<'_>,
    node_type: NodeType,
    initialize: impl FnOnce(&mut NodeCore),
) -> Node<'a> {
    let arena = arena();
    let index = arena.next.fetch_add(1, Ordering::Relaxed);
    assert!(index < ARENA_CELLS, "the test arena is full");
    // SAFETY: The index is in the arena, and no other node has this cell.
    let cell = unsafe { &mut *arena.cells.add(index) };
    let pointer = compress(cell as *mut MaybeUninit<TestCell> as usize);
    let mut core = NodeCore::for_test(node_type, pointer);
    initialize(&mut core);
    let cell = cell.write(TestCell {
        header: [0; NODE_CORE_OFFSET],
        core,
        fields: [0; DOCUMENT_FRAGMENT_HOST_OFFSET - NODE_CORE_OFFSET - size_of::<NodeCore>()],
        host: NodeMember::null_for_test(),
    });
    // SAFETY: The cell lives for the rest of the process, and only the test that made it changes its tree, which it
    //         does not while it reads it.
    unsafe { Node::from_cell(&*(cell as *const TestCell as *const NodeCell), main_thread) }
}

/// Appends `child`, the root of a tree of its own, to `parent`'s children.
pub(crate) fn append_child(parent: Node<'_>, child: Node<'_>) {
    let child_links = child.core().links_for_test();
    assert!(child.parent().is_none());
    let parent_links = parent.core().links_for_test();
    child_links.parent.set(Some(pointer_of(parent)));
    child_links.previous_sibling.set(parent.core().last_child());
    match parent.last_child() {
        Some(last_child) => last_child
            .core()
            .links_for_test()
            .next_sibling
            .set(Some(pointer_of(child))),
        None => parent_links.first_child.set(Some(pointer_of(child))),
    }
    parent_links.last_child.set(Some(pointer_of(child)));
    set_root(child, pointer_of(parent.root()));
}

fn set_root(node: Node<'_>, root: NodePointer) {
    node.core().links_for_test().root.set(Some(root));
    let mut child = node.first_child();
    while let Some(current) = child {
        set_root(current, root);
        child = current.next_sibling();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn links_lead_to_the_nodes_they_name() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let first = node(&main_thread, NodeType::TEXT_NODE);
        let second = node(&main_thread, NodeType::COMMENT_NODE);
        append_child(parent, first);
        append_child(parent, second);

        assert_eq!(parent.first_child(), Some(first));
        assert_eq!(parent.last_child(), Some(second));
        assert_eq!(first.parent(), Some(parent));
        assert_eq!(first.next_sibling(), Some(second));
        assert_eq!(second.previous_sibling(), Some(first));
        assert_eq!(first.previous_sibling(), None);
        assert_eq!(second.next_sibling(), None);
        assert_eq!(parent.parent(), None);
        assert_eq!(second.node_type(), NodeType::COMMENT_NODE);
    }

    #[test]
    fn a_node_is_the_root_of_its_tree_until_it_is_appended() {
        let main_thread = MainThread::for_test();
        let root = node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
        let element = node(&main_thread, NodeType::ELEMENT_NODE);
        let text = node(&main_thread, NodeType::TEXT_NODE);
        append_child(element, text);
        assert_eq!(text.root(), element);
        assert_eq!(root.root(), root);

        append_child(root, element);
        assert_eq!(element.root(), root);
        assert_eq!(text.root(), root);
    }

    #[test]
    fn a_fragment_leads_to_its_host() {
        let main_thread = MainThread::for_test();
        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        let shadow_root = shadow_root(&main_thread, host);
        assert!(shadow_root.is_shadow_root());
        assert_eq!(shadow_root.host(), Some(host));
        assert!(!host.is_shadow_root());

        let fragment = node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
        assert_eq!(fragment.host(), None);
        assert!(!fragment.is_shadow_root());
    }

    #[test]
    fn handles_are_equal_exactly_when_they_are_the_same_node() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let child = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(parent, child);
        assert_eq!(child.parent().unwrap(), parent);
        assert_ne!(child, parent);
        assert!(core::ptr::eq(child.parent().unwrap().cell(), parent.cell()));
    }
}
