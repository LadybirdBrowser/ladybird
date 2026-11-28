/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Trees of nodes for unit tests: real cores, in cells of an arena that stands in for the GC heap region, which the
//! nodes' compressed pointers are offsets into.

use core::mem::ManuallyDrop;
use core::mem::MaybeUninit;
use core::mem::offset_of;
use std::sync::OnceLock;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use super::element::Element;
use super::element_core::ELEMENT_CORE_OFFSET;
use super::element_core::ElementCore;
use super::links::append_child_for_test;
use super::node::Node;
use super::node::NodeCell;
use super::node::compress;
use super::node_core::CHARACTER_DATA_CORE_OFFSET;
use super::node_core::CharacterDataCore;
use super::node_core::DOCUMENT_CORE_OFFSET;
use super::node_core::DOCUMENT_FRAGMENT_CORE_OFFSET;
use super::node_core::DocumentCore;
use super::node_core::DocumentFragmentCore;
use super::node_core::NODE_CORE_OFFSET;
use super::node_core::NodeCore;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use super::node_core::PARENT_NODE_CORE_OFFSET;
use super::node_core::ParentNodeCore;
use super::node_ref::NodeRef;
use super::qualified_name::QualifiedNameField;
use crate::css::style::tree::StyleNodeID;
use crate::css::style::tree::TreeScopeID;
use crate::stage::MainThread;

/// A node's cell, with its core where DOM::Node has it, then either a character data core where DOM::CharacterData has
/// it, which only a CharacterData node's cell holds, or a parent node core where DOM::ParentNode has it, which the cell
/// of every other node holds, a fragment core where DOM::DocumentFragment has it, and then either a document core where
/// DOM::Document has it, which only a document's cell holds, or an element core where DOM::Element has it, which only an
/// element's cell holds.
#[repr(C, align(8))]
struct TestCell {
    header: [u8; NODE_CORE_OFFSET],
    core: NodeCore,
    fields: [u8; CHARACTER_DATA_CORE_OFFSET - NODE_CORE_OFFSET - size_of::<NodeCore>()],
    middle: TestCellMiddle,
    fragment_core: DocumentFragmentCore,
    fragment_fields: [u8; DOCUMENT_CORE_OFFSET - DOCUMENT_FRAGMENT_CORE_OFFSET - size_of::<DocumentFragmentCore>()],
    tail: TestCellTail,
}

/// What a CharacterData node's cell and a ParentNode's cell hold past the node core, where the two overlap.
#[repr(C)]
union TestCellMiddle {
    character_data_core: ManuallyDrop<MaybeUninit<CharacterDataCore>>,
    parent_node_core: ManuallyDrop<ParentNodeCore>,
}

/// What a document's cell and an element's cell hold past the fragment core, where the two overlap.
#[repr(C)]
union TestCellTail {
    document_core: ManuallyDrop<DocumentCore>,
    element: ManuallyDrop<ElementTail>,
}

#[repr(C)]
struct ElementTail {
    fields: [u8; ELEMENT_CORE_OFFSET - DOCUMENT_CORE_OFFSET],
    core: MaybeUninit<ElementCore>,
}

const _: () = assert!(offset_of!(TestCell, core) == NODE_CORE_OFFSET);
const _: () = assert!(offset_of!(TestCell, middle) == CHARACTER_DATA_CORE_OFFSET);
const _: () = assert!(PARENT_NODE_CORE_OFFSET == CHARACTER_DATA_CORE_OFFSET);
const _: () = assert!(size_of::<TestCellMiddle>() == DOCUMENT_FRAGMENT_CORE_OFFSET - CHARACTER_DATA_CORE_OFFSET);
const _: () = assert!(offset_of!(TestCell, fragment_core) == DOCUMENT_FRAGMENT_CORE_OFFSET);
const _: () = assert!(offset_of!(TestCell, tail) == DOCUMENT_CORE_OFFSET);
const _: () = assert!(DOCUMENT_CORE_OFFSET + offset_of!(ElementTail, core) == ELEMENT_CORE_OFFSET);

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

fn pointer_of(node: Node<'_>) -> NodePointer {
    compress(node.cell())
}

/// A new node of `node_type`, the root of a tree of its own. An element is a `div` in no namespace, and a CharacterData
/// node has no data.
pub(crate) fn node<'a>(main_thread: &'a MainThread<'_>, node_type: NodeType) -> Node<'a> {
    match node_type {
        NodeType::ELEMENT_NODE => element(
            main_thread,
            ElementCore::for_test(QualifiedNameField::for_test("div", None)),
        )
        .node(),
        NodeType::TEXT_NODE
        | NodeType::CDATA_SECTION_NODE
        | NodeType::PROCESSING_INSTRUCTION_NODE
        | NodeType::COMMENT_NODE => character_data(main_thread, node_type, ""),
        _ => new_node(main_thread, node_type, |_| {}, None),
    }
}

/// A new CharacterData node of `node_type` with the data `data`, the root of a tree of its own.
pub(crate) fn character_data<'a>(main_thread: &'a MainThread<'_>, node_type: NodeType, data: &str) -> Node<'a> {
    let node = new_node(main_thread, node_type, |_| {}, None);
    // SAFETY: Every node of a test tree is a TestCell, and no reference to this one's character data core exists yet.
    unsafe {
        (*(node.cell() as *const NodeCell as *mut TestCell)).middle = TestCellMiddle {
            character_data_core: ManuallyDrop::new(MaybeUninit::new(CharacterDataCore::for_test(data))),
        };
    }
    node
}

/// A new element with the core `core`, the root of a tree of its own.
pub(crate) fn element<'a>(main_thread: &'a MainThread<'_>, core: ElementCore) -> Element<'a> {
    new_node(main_thread, NodeType::ELEMENT_NODE, |_| {}, Some(core))
        .as_element()
        .unwrap()
}

/// A new shadow root of `host`.
pub(crate) fn shadow_root<'a>(main_thread: &'a MainThread<'_>, host: Node<'a>) -> Node<'a> {
    let shadow_root = new_node(
        main_thread,
        NodeType::DOCUMENT_FRAGMENT_NODE,
        NodeCore::set_is_shadow_root_for_test,
        None,
    );
    set_host(shadow_root, host);
    if let Some(element) = host.as_element() {
        element.core().set_shadow_root_for_test(Some(pointer_of(shadow_root)));
    }
    shadow_root
}

/// Sets the host of a DocumentFragment node.
pub(crate) fn set_host(fragment: Node<'_>, host: Node<'_>) {
    assert_eq!(fragment.node_type(), NodeType::DOCUMENT_FRAGMENT_NODE);
    // SAFETY: Every node of a test tree is a TestCell.
    let cell = unsafe { &*(fragment.cell() as *const NodeCell as *const TestCell) };
    cell.fragment_core.set_host_for_test(Some(pointer_of(host)));
}

fn new_node<'a>(
    main_thread: &'a MainThread<'_>,
    node_type: NodeType,
    initialize: impl FnOnce(&mut NodeCore),
    element_core: Option<ElementCore>,
) -> Node<'a> {
    // NB: Every node but a document gets a document of its own, whose core the DOM algorithms read.
    let document = (node_type != NodeType::DOCUMENT_NODE).then(|| {
        pointer_of(new_node(
            main_thread,
            NodeType::DOCUMENT_NODE,
            (|_| {}) as fn(&mut NodeCore),
            None,
        ))
    });
    let arena = arena();
    let index = arena.next.fetch_add(1, Ordering::Relaxed);
    assert!(index < ARENA_CELLS, "the test arena is full");
    // SAFETY: The index is in the arena, and no other node has this cell.
    let cell = unsafe { &mut *arena.cells.add(index) };
    // SAFETY: A test cell starts with its node's cell.
    let pointer = compress(unsafe { &*(cell as *mut MaybeUninit<TestCell> as *const NodeCell) });
    let mut core = NodeCore::for_test(node_type, pointer, document.unwrap_or(pointer));
    initialize(&mut core);
    let tail = match element_core {
        Some(element_core) => TestCellTail {
            element: ManuallyDrop::new(ElementTail {
                fields: [0; ELEMENT_CORE_OFFSET - DOCUMENT_CORE_OFFSET],
                core: MaybeUninit::new(element_core),
            }),
        },
        None => TestCellTail {
            document_core: ManuallyDrop::new(DocumentCore::for_test()),
        },
    };
    let cell = cell.write(TestCell {
        header: [0; NODE_CORE_OFFSET],
        core,
        fields: [0; CHARACTER_DATA_CORE_OFFSET - NODE_CORE_OFFSET - size_of::<NodeCore>()],
        middle: TestCellMiddle {
            parent_node_core: ManuallyDrop::new(ParentNodeCore::for_test()),
        },
        fragment_core: DocumentFragmentCore::null_for_test(),
        fragment_fields: [0; DOCUMENT_CORE_OFFSET - DOCUMENT_FRAGMENT_CORE_OFFSET - size_of::<DocumentFragmentCore>()],
        tail,
    });
    // SAFETY: The cell lives for the rest of the process, and only the test that made it changes its tree, which it
    //         does not while it reads it.
    unsafe { Node::from_cell(&*(cell as *const TestCell as *const NodeCell), main_thread) }
}

/// The core of a document of a test tree.
pub(crate) fn document_core<'a>(document: Node<'a>) -> &'a DocumentCore {
    assert_eq!(document.node_type(), NodeType::DOCUMENT_NODE);
    // SAFETY: Every node of a test tree is a TestCell, and a document's holds a document core.
    unsafe {
        &(*(document.cell() as *const NodeCell as *const TestCell))
            .tail
            .document_core
    }
}

/// Makes `document` the node document of `node`.
pub(crate) fn set_document(node: Node<'_>, document: Node<'_>) {
    assert_eq!(document.node_type(), NodeType::DOCUMENT_NODE);
    node.core().set_document_for_test(pointer_of(document));
}

/// Gives the node the identity `node` in its document's style engine, or none.
pub(crate) fn set_style_node_id(node: Node<'_>, style_node_id: Option<StyleNodeID>) {
    node.core().set_style_node_id_for_test(style_node_id);
}

/// Numbers the tree scope of a shadow root.
pub(crate) fn set_style_engine_tree_scope(shadow_root: Node<'_>, tree_scope: TreeScopeID) {
    assert!(shadow_root.is_shadow_root());
    // SAFETY: Every node of a test tree is a TestCell.
    let cell = unsafe { &*(shadow_root.cell() as *const NodeCell as *const TestCell) };
    cell.fragment_core.set_style_engine_tree_scope_for_test(tree_scope);
}

/// Appends `child`, the root of a tree of its own, to `parent`'s children.
pub(crate) fn append_child(parent: Node<'_>, child: Node<'_>) {
    append_child_for_test(NodeRef::from(parent), NodeRef::from(child));
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
