/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Nodes as Rust reaches them: by following the compressed node pointers of the cores, on the main thread, which owns
//! the DOM.

use core::cell::UnsafeCell;
use core::hash::Hash;
use core::hash::Hasher;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::ptr::NonNull;

use super::node_core::CHARACTER_DATA_CORE_OFFSET;
use super::node_core::CharacterDataCore;
use super::node_core::DOCUMENT_FRAGMENT_CORE_OFFSET;
use super::node_core::DocumentFragmentCore;
use super::node_core::NODE_CORE_OFFSET;
use super::node_core::NodeCore;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use super::strings::StringField;
use crate::css::style::tree::TreeScopeID;
use crate::stage::MainThread;

/// This library's copy of LibGC's heap region base, which compressed cell pointers are offsets from. LibGC fills it in
/// through the GC::HeapRegionBaseCopyRegistration that Node.cpp makes of it, before any cell exists, and never changes
/// it after that.
#[repr(transparent)]
struct HeapRegionBaseCopy(UnsafeCell<usize>);

// SAFETY: The copy is written once, before any node exists, and every read of it is for a node that the main thread
//         made since.
unsafe impl Sync for HeapRegionBaseCopy {}

// OPTIMIZATION: A copy of the base in this library, which a decode reads with one PC-relative load instead of a load of
//               js_heap_region_base's address from the GOT first.
#[unsafe(export_name = "rust_dom_heap_region_base")]
static HEAP_REGION_BASE: HeapRegionBaseCopy = HeapRegionBaseCopy(UnsafeCell::new(0));

/// How far a compressed cell pointer is shifted, as LibGC's COMPRESSED_POINTER_SHIFT: cells are 8-byte aligned.
const COMPRESSED_POINTER_SHIFT: u32 = 3;

#[inline(always)]
fn heap_region_base() -> usize {
    // SAFETY: Nothing writes the copy after the first node exists, see HeapRegionBaseCopy.
    unsafe { *HEAP_REGION_BASE.0.get() }
}

/// The start of the cell a node pointer names, decoded as GC::decompress_nonnull() decodes a compressed pointer.
///
/// Makes the compressed pointer live up to this point, and the decoded address from this point on. The conservative
/// stack scan recognizes a cell by its address or by its compressed pointer in either half of a word, but not by its
/// unshifted offset, which the compiler could otherwise keep instead while either is held across an allocation.
#[inline(always)]
pub(super) fn decompress(pointer: NodePointer) -> NonNull<NodeCell> {
    let compressed = pointer.compressed();
    let mut address = heap_region_base() + ((compressed as usize) << COMPRESSED_POINTER_SHIFT);
    // SAFETY: The assembly is empty: it only has the compiler put `compressed` and `address` in registers here.
    unsafe {
        core::arch::asm!(
            "/* {compressed} {address} */",
            compressed = in(reg) compressed as usize,
            address = inout(reg) address,
            options(nomem, nostack, preserves_flags),
        );
    }
    // SAFETY: A compressed pointer decodes to an address in the heap region, which is never null. The base is an exposed
    //         address of the region, whose provenance the cell's address takes.
    unsafe { NonNull::new_unchecked(core::ptr::with_exposed_provenance_mut(address)) }
}

/// The compressed pointer of a node: its offset into the heap region in units of the cell alignment.
#[inline(always)]
pub(super) fn compress(cell: &NodeCell) -> NodePointer {
    let offset = (cell as *const NodeCell as usize).wrapping_sub(heap_region_base());
    debug_assert!(offset < (1usize << (32 + COMPRESSED_POINTER_SHIFT)));
    debug_assert!(offset.is_multiple_of(1 << COMPRESSED_POINTER_SHIFT));
    NodePointer::from_compressed((offset >> COMPRESSED_POINTER_SHIFT) as u32)
}

/// Sets the heap region base to that of a test arena.
///
/// # Safety
///
/// No node may exist yet, and no other thread may read the base until it synchronizes with this one.
#[cfg(test)]
pub(super) unsafe fn set_heap_region_base_for_test(base: usize) {
    // SAFETY: Guaranteed by the caller.
    unsafe { *HEAP_REGION_BASE.0.get() = base };
}

/// A DOM::Node as C++ allocates it: the start of the node's cell, with the node's core NODE_CORE_OFFSET bytes into it.
/// FFI entry points take and return nodes as references to it, which C++ sees as DOM::Node pointers.
#[repr(C)]
pub struct NodeCell {
    // NB: The cell's state changes behind shared references to it, so a reference to it must not let the compiler
    //     assume that the memory it leads to stays the same, which it would for a type without an UnsafeCell.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which Rust never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

/// A node of the DOM, borrowed on the main thread for `'a`, during which the DOM does not change.
///
/// Only code holding a main thread token makes one, and the handle is neither Send nor Sync, so a node is never reached
/// from another thread. The nodes the handle's links lead to are borrowed for as long as the handle.
///
/// NB: The handle holds the start of the node's cell, which the conservative stack scan recognizes, rather than the
///     address of its core, which it does not.
#[derive(Clone, Copy)]
pub struct Node<'a> {
    cell: &'a NodeCell,
}

impl<'a> Node<'a> {
    /// The node `cell` is, as an FFI entry point that holds `main_thread` is handed it.
    ///
    /// # Safety
    ///
    /// `cell` must be a live node, and the DOM must not change for `'a`.
    #[inline(always)]
    pub(crate) unsafe fn from_cell(cell: &'a NodeCell, _main_thread: &'a MainThread<'_>) -> Self {
        Self { cell }
    }

    /// The node `pointer` names, as code holding `main_thread` reads it from a field of a cell that is no node.
    ///
    /// # Safety
    ///
    /// `pointer` must name a live node, and the DOM must not change for `'a`.
    #[inline(always)]
    pub(crate) unsafe fn from_pointer(pointer: NodePointer, _main_thread: &'a MainThread<'_>) -> Self {
        // SAFETY: Guaranteed by the caller.
        unsafe { Self::decode(pointer) }
    }

    /// # Safety
    ///
    /// `pointer` must name a node that lives for `'a`.
    #[inline(always)]
    unsafe fn decode(pointer: NodePointer) -> Self {
        let address = heap_region_base() + ((pointer.compressed() as usize) << COMPRESSED_POINTER_SHIFT);
        // SAFETY: A compressed pointer decodes to the start of the cell it names, which lives for 'a.
        Self {
            cell: unsafe { &*(address as *const NodeCell) },
        }
    }

    /// The node a link of this node names. The links of a node the DOM keeps alive name nodes it keeps alive.
    #[inline(always)]
    pub(super) fn follow(self, link: Option<NodePointer>) -> Option<Node<'a>> {
        // SAFETY: The DOM keeps this node, and so the nodes its links name, alive and unchanged for 'a.
        link.map(|pointer| unsafe { Self::decode(pointer) })
    }

    /// The node as C++ sees it.
    #[inline(always)]
    pub fn cell(self) -> &'a NodeCell {
        self.cell
    }

    #[inline(always)]
    pub fn core(self) -> &'a NodeCore {
        // SAFETY: Every node embeds its core NODE_CORE_OFFSET bytes into its cell, which Node.h asserts.
        unsafe {
            &*(self.cell as *const NodeCell)
                .byte_add(NODE_CORE_OFFSET)
                .cast::<NodeCore>()
        }
    }

    #[inline(always)]
    pub fn node_type(self) -> NodeType {
        self.core().node_type()
    }

    /// https://dom.spec.whatwg.org/#concept-element
    #[inline(always)]
    pub fn is_element(self) -> bool {
        self.node_type() == NodeType::ELEMENT_NODE
    }

    /// https://dom.spec.whatwg.org/#concept-shadow-root
    #[inline(always)]
    pub fn is_shadow_root(self) -> bool {
        self.core().is_shadow_root()
    }

    /// The data of a CharacterData node, which DOM::CharacterData keeps in its core.
    /// https://dom.spec.whatwg.org/#concept-cd-data
    #[inline(always)]
    pub fn data(self) -> &'a StringField {
        assert!(matches!(
            self.node_type(),
            NodeType::TEXT_NODE
                | NodeType::CDATA_SECTION_NODE
                | NodeType::PROCESSING_INSTRUCTION_NODE
                | NodeType::COMMENT_NODE
        ));
        // SAFETY: Every CharacterData node embeds its core CHARACTER_DATA_CORE_OFFSET bytes into its cell, which
        //         CharacterData.cpp asserts.
        let core = unsafe {
            &*(self.cell as *const NodeCell)
                .byte_add(CHARACTER_DATA_CORE_OFFSET)
                .cast::<CharacterDataCore>()
        };
        core.data()
    }

    /// The compressed pointer that names the node.
    #[inline(always)]
    pub fn pointer(self) -> NodePointer {
        compress(self.cell)
    }

    /// https://dom.spec.whatwg.org/#concept-node-length
    pub fn length(self) -> usize {
        // To determine the length of a node node, run these steps:
        match self.node_type() {
            // 1. If node is a DocumentType or Attr node, then return 0.
            NodeType::DOCUMENT_TYPE_NODE | NodeType::ATTRIBUTE_NODE => 0,
            // 2. If node is a CharacterData node, then return node’s data’s length.
            NodeType::TEXT_NODE
            | NodeType::CDATA_SECTION_NODE
            | NodeType::PROCESSING_INSTRUCTION_NODE
            | NodeType::COMMENT_NODE => match self.data().units() {
                ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => units.len(),
                ak::Utf16StringUnits::Utf16(units) => units.len(),
            },
            // 3. Return the number of node’s children.
            _ => core::iter::successors(self.first_child(), |child| child.next_sibling()).count(),
        }
    }

    /// The host of a DocumentFragment node, which DOM::DocumentFragment keeps in its core.
    /// https://dom.spec.whatwg.org/#concept-documentfragment-host
    #[inline(always)]
    pub fn host(self) -> Option<Node<'a>> {
        // NB: An element starts with its node, which DocumentFragment::set_host() verifies, so the core names the host
        //     as a node.
        self.follow(self.fragment_core().host())
    }

    /// The style engine's number for the tree scope of a shadow root, which DOM::DocumentFragment keeps in its core.
    #[inline(always)]
    pub fn style_engine_tree_scope(self) -> TreeScopeID {
        assert!(self.is_shadow_root());
        self.fragment_core().style_engine_tree_scope()
    }

    #[inline(always)]
    fn fragment_core(self) -> &'a DocumentFragmentCore {
        assert!(self.node_type() == NodeType::DOCUMENT_FRAGMENT_NODE);
        // SAFETY: Every DocumentFragment embeds its core DOCUMENT_FRAGMENT_CORE_OFFSET bytes into its cell, which
        //         DocumentFragment.cpp asserts.
        unsafe {
            &*(self.cell as *const NodeCell)
                .byte_add(DOCUMENT_FRAGMENT_CORE_OFFSET)
                .cast::<DocumentFragmentCore>()
        }
    }

    /// https://dom.spec.whatwg.org/#concept-tree-parent
    #[inline(always)]
    pub fn parent(self) -> Option<Node<'a>> {
        self.follow(self.core().parent())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-first-child
    #[inline(always)]
    pub fn first_child(self) -> Option<Node<'a>> {
        self.follow(self.core().first_child())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-last-child
    #[inline(always)]
    pub fn last_child(self) -> Option<Node<'a>> {
        self.follow(self.core().last_child())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-next-sibling
    #[inline(always)]
    pub fn next_sibling(self) -> Option<Node<'a>> {
        self.follow(self.core().next_sibling())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-previous-sibling
    #[inline(always)]
    pub fn previous_sibling(self) -> Option<Node<'a>> {
        self.follow(self.core().previous_sibling())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-root
    #[inline(always)]
    pub fn root(self) -> Node<'a> {
        // SAFETY: The DOM keeps this node, and so the root of its tree, alive and unchanged for 'a.
        unsafe { Self::decode(self.core().root()) }
    }
}

/// Two handles are equal exactly when they are the same node.
impl PartialEq for Node<'_> {
    #[inline(always)]
    fn eq(&self, other: &Self) -> bool {
        core::ptr::eq(self.cell, other.cell)
    }
}

impl Eq for Node<'_> {}

impl Hash for Node<'_> {
    #[inline(always)]
    fn hash<H: Hasher>(&self, state: &mut H) {
        core::ptr::hash(self.cell, state);
    }
}

impl core::fmt::Debug for Node<'_> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "Node({:p}, {:?})", self.cell, self.node_type())
    }
}
