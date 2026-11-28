/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Nodes as the code that changes the DOM holds them.
//!
//! A read handle, `Node<'a>`, lets the DOM not change for `'a`, which mutation code cannot promise: it changes the tree
//! itself, and it calls into C++, which allocates and can run script that changes the tree in any way. So mutation code
//! holds nodes as [`NodeRef`]s, which promise only that the node stays alive while the reference is held:
//!
//! - A `NodeRef` holds the start of the node's cell, which the conservative stack scan recognizes, so one in a local
//!   variable, an argument or a register keeps its node alive across an allocation. A collection that moves cells only
//!   runs when it does not scan the stack, and so never while Rust holds one.
//! - It reads every link from the node's core when asked, and keeps nothing it read: what it returns is the tree as it
//!   is at that moment.
//! - It derives the core's address from the cell's anew on every read, behind an empty assembly barrier, so the
//!   compiler keeps the cell's address live rather than the address of the core, which is in the middle of the cell
//!   and which the scan does not recognize.
//! - A link it follows is decoded as C++ decodes a GC::Member, behind the same barrier, so the compiler keeps the
//!   compressed pointer or the decoded address live, never the unshifted offset in between, which the scan does not
//!   recognize either.
//!
//! The rule for holding nodes: a `NodeRef` lives only on the stack. Mutation code never puts one in memory the
//! collector does not scan, such as a `Vec`, a `Box` or a `SmallVec` that may spill. It holds a list of nodes either as
//! a fixed array on its own stack, or as a span of a C++ GC::RootVector<GC::Ref<DOM::Node>> that lives in a C++ frame
//! below the Rust code that reads it, which roots the nodes for as long as that frame does.
//!
//! A `NodeRef` is laid out as a GC::Ref<DOM::Node>, so a span of those reads as a slice of them. It is neither Send
//! nor Sync, so it stays on the main thread, which owns the DOM.

use core::ptr::NonNull;

use super::element_core::ELEMENT_CORE_OFFSET;
use super::element_core::ElementCore;
use super::node::Node;
use super::node::NodeCell;
use super::node::compress;
use super::node::decompress;
use super::node_core::DOCUMENT_CORE_OFFSET;
use super::node_core::DOCUMENT_FRAGMENT_CORE_OFFSET;
use super::node_core::DocumentCore;
use super::node_core::DocumentFragmentCore;
use super::node_core::NODE_CORE_OFFSET;
use super::node_core::NodeCore;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use crate::stage::MainThread;

/// A node held by code that changes the DOM, valid across tree changes, allocation and script for as long as it is
/// held on the stack. See the module documentation for the rules it keeps.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct NodeRef {
    // NB: NonNull makes the reference neither Send nor Sync.
    cell: NonNull<NodeCell>,
}

/// A node, as a function that crosses to C++ takes or returns it: C++ sees it as a raw pointer, which every ABI
/// passes and returns as C does, where a GC::Ref is a class that MSVC returns through a hidden pointer.
pub type RawNodeRef = NodeRef;

impl<'a> From<Node<'a>> for NodeRef {
    /// The node a read handle names, to hold beyond the read.
    #[inline(always)]
    fn from(node: Node<'a>) -> Self {
        Self {
            cell: NonNull::from(node.cell()),
        }
    }
}

impl NodeRef {
    /// The node `cell` is, as an FFI entry point that holds a main thread token is handed it.
    ///
    /// # Safety
    ///
    /// `cell` must be a live node.
    #[inline(always)]
    pub(crate) unsafe fn from_cell(cell: &NodeCell, _main_thread: &MainThread<'_>) -> Self {
        Self {
            cell: NonNull::from(cell),
        }
    }

    /// # Safety
    ///
    /// `pointer` must name a live node.
    #[inline(always)]
    unsafe fn decode(pointer: NodePointer) -> Self {
        Self {
            cell: decompress(pointer),
        }
    }

    /// The compressed pointer that names the node, as links and GC::Member fields hold it.
    #[inline(always)]
    pub(super) fn pointer(self) -> NodePointer {
        compress(self.cell())
    }

    #[inline(always)]
    fn follow(link: Option<NodePointer>) -> Option<NodeRef> {
        // SAFETY: The links of a live node name nodes it keeps alive.
        link.map(|pointer| unsafe { Self::decode(pointer) })
    }

    /// The node as a read handle, for as long as `main_thread` is borrowed, which keeps anything from changing the DOM.
    #[inline(always)]
    pub(super) fn read<'a>(self, main_thread: &'a MainThread<'_>) -> Node<'a> {
        // SAFETY: The node is alive for as long as the reference is held, and the shared borrow of the main thread
        //         token keeps anything from changing the DOM for 'a.
        unsafe { Node::from_cell(&*self.cell.as_ptr(), main_thread) }
    }

    /// The node as C++ sees it, for as long as the reference is borrowed.
    #[inline(always)]
    pub fn cell(&self) -> &NodeCell {
        // SAFETY: The node is alive for as long as the reference is held.
        unsafe { self.cell.as_ref() }
    }

    /// The start of the cell, as an opaque value the compiler has to keep the held address live for.
    #[inline(always)]
    fn pinned_cell(&self) -> *const NodeCell {
        let mut address = self.cell.as_ptr() as usize;
        // SAFETY: The assembly is empty: it only has the compiler take the address in a register here, and give back a
        //         value it knows nothing about, which keeps it from deriving what it reads below from a value it kept
        //         from before.
        unsafe {
            core::arch::asm!(
                "/* {address} */",
                address = inout(reg) address,
                options(nomem, nostack, preserves_flags),
            );
        }
        core::ptr::with_exposed_provenance(address)
    }

    /// The node's core, read for as long as the reference is borrowed, during which nothing may call into C++.
    #[inline(always)]
    pub(super) fn core(&self) -> &NodeCore {
        // SAFETY: Every node embeds its core NODE_CORE_OFFSET bytes into its cell, which Node.h asserts, and the node
        //         is alive for as long as the reference is held.
        unsafe { &*self.pinned_cell().byte_add(NODE_CORE_OFFSET).cast::<NodeCore>() }
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

    /// https://dom.spec.whatwg.org/#connected
    #[inline(always)]
    pub fn is_connected(self) -> bool {
        self.core().is_connected()
    }

    /// https://dom.spec.whatwg.org/#concept-tree-parent
    #[inline(always)]
    pub fn parent(self) -> Option<NodeRef> {
        Self::follow(self.core().parent())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-first-child
    #[inline(always)]
    pub fn first_child(self) -> Option<NodeRef> {
        Self::follow(self.core().first_child())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-last-child
    #[inline(always)]
    pub fn last_child(self) -> Option<NodeRef> {
        Self::follow(self.core().last_child())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-next-sibling
    #[inline(always)]
    pub fn next_sibling(self) -> Option<NodeRef> {
        Self::follow(self.core().next_sibling())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-previous-sibling
    #[inline(always)]
    pub fn previous_sibling(self) -> Option<NodeRef> {
        Self::follow(self.core().previous_sibling())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-root
    #[inline(always)]
    pub fn root(self) -> NodeRef {
        // SAFETY: A live node keeps the root of its tree alive.
        unsafe { Self::decode(self.core().root()) }
    }

    /// https://dom.spec.whatwg.org/#concept-node-document
    #[inline(always)]
    pub fn document(self) -> NodeRef {
        // SAFETY: A live node keeps its node document alive.
        unsafe { Self::decode(self.core().document()) }
    }

    /// The element's core, read for as long as the reference is borrowed, during which nothing may call into C++, or
    /// none for a node that is no element.
    #[inline(always)]
    pub(super) fn element_core(&self) -> Option<&ElementCore> {
        if !self.is_element() {
            return None;
        }
        // SAFETY: Every element embeds its core ELEMENT_CORE_OFFSET bytes into its cell, which Element.cpp asserts.
        Some(unsafe { &*self.pinned_cell().byte_add(ELEMENT_CORE_OFFSET).cast::<ElementCore>() })
    }

    /// The core of the node's node document, read for as long as the reference is borrowed, during which nothing may
    /// call into C++.
    #[inline(always)]
    pub(super) fn document_core(&self) -> &DocumentCore {
        // SAFETY: A live node keeps its node document alive.
        let document = unsafe { Self::decode(self.core().document()) };
        // SAFETY: Every document embeds its core DOCUMENT_CORE_OFFSET bytes into its cell, which Document.cpp asserts,
        //         and the document lives for as long as the node, whose reference is borrowed.
        unsafe {
            &*document
                .pinned_cell()
                .byte_add(DOCUMENT_CORE_OFFSET)
                .cast::<DocumentCore>()
        }
    }

    /// The shadow root of an element, which DOM::Element keeps in its core, or none for any other node.
    /// https://dom.spec.whatwg.org/#concept-element-shadow-root
    #[inline(always)]
    pub fn shadow_root(self) -> Option<NodeRef> {
        Self::follow(self.element_core()?.shadow_root())
    }

    /// The host of a DocumentFragment node, which DOM::DocumentFragment keeps in its core, or none for any other node.
    /// https://dom.spec.whatwg.org/#concept-documentfragment-host
    #[inline(always)]
    pub fn host(self) -> Option<NodeRef> {
        if self.node_type() != NodeType::DOCUMENT_FRAGMENT_NODE {
            return None;
        }
        // SAFETY: Every DocumentFragment embeds its core DOCUMENT_FRAGMENT_CORE_OFFSET bytes into its cell, which
        //         DocumentFragment.cpp asserts.
        let core = unsafe {
            &*self
                .pinned_cell()
                .byte_add(DOCUMENT_FRAGMENT_CORE_OFFSET)
                .cast::<DocumentFragmentCore>()
        };
        Self::follow(core.host())
    }
}

/// A node or null, as a function that crosses to C++ takes or returns it: C++ sees it as a raw pointer, which every ABI
/// passes and returns as C does, where a GC::Ptr is a class that MSVC returns through a hidden pointer.
pub type RawNullableNodeRef = NullableNodeRef;

/// A node or null, as C++ hands one over: laid out as a GC::Ptr<DOM::Node>, which is what C++ sees it as.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub struct NullableNodeRef(Option<NodeRef>);

impl From<Option<NodeRef>> for NullableNodeRef {
    #[inline(always)]
    fn from(node: Option<NodeRef>) -> Self {
        Self(node)
    }
}

impl From<NullableNodeRef> for Option<NodeRef> {
    #[inline(always)]
    fn from(node: NullableNodeRef) -> Self {
        node.0
    }
}

impl core::fmt::Debug for NodeRef {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "NodeRef({:p})", self.cell)
    }
}

// NB: The reference is laid out as a GC::Ref<DOM::Node>, a pointer to the start of the node's cell.
const _: () = assert!(size_of::<NodeRef>() == size_of::<usize>() && align_of::<NodeRef>() == align_of::<usize>());
const _: () = assert!(size_of::<Option<NodeRef>>() == size_of::<usize>());

#[cfg(test)]
mod tests {
    use super::super::test_tree::append_child;
    use super::super::test_tree::node;
    use super::super::test_tree::shadow_root;
    use super::*;

    #[test]
    fn a_reference_reads_the_links_its_read_handle_reads() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let first = node(&main_thread, NodeType::TEXT_NODE);
        let second = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(parent, first);
        append_child(parent, second);

        let [parent_ref, first_ref, second_ref] = [parent, first, second].map(NodeRef::from);
        assert_eq!(parent_ref.first_child(), Some(first_ref));
        assert_eq!(parent_ref.last_child(), Some(second_ref));
        assert_eq!(first_ref.parent(), Some(parent_ref));
        assert_eq!(first_ref.next_sibling(), Some(second_ref));
        assert_eq!(second_ref.previous_sibling(), Some(first_ref));
        assert_eq!(first_ref.previous_sibling(), None);
        assert_eq!(second_ref.root(), parent_ref);
        assert_eq!(first_ref.node_type(), NodeType::TEXT_NODE);
        assert!(second_ref.is_element());
    }

    #[test]
    fn a_reference_reads_links_as_they_are_when_asked() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let child = node(&main_thread, NodeType::ELEMENT_NODE);
        let parent_ref = NodeRef::from(parent);
        assert_eq!(parent_ref.first_child(), None);
        append_child(parent, child);
        assert_eq!(parent_ref.first_child(), Some(NodeRef::from(child)));
    }

    #[test]
    fn a_reference_reaches_shadow_roots_and_hosts() {
        let main_thread = MainThread::for_test();
        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        let shadow_root = shadow_root(&main_thread, host);
        let [host_ref, shadow_root_ref] = [host, shadow_root].map(NodeRef::from);
        assert_eq!(host_ref.shadow_root(), Some(shadow_root_ref));
        assert_eq!(shadow_root_ref.host(), Some(host_ref));
        assert!(shadow_root_ref.is_shadow_root());
        assert_eq!(shadow_root_ref.shadow_root(), None);
        assert_eq!(host_ref.host(), None);
    }
}
