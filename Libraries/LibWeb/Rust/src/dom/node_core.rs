/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of a DOM node: the tree state of a node, whose layout is defined here, and which C++ embeds in DOM::Node and
//! reads and writes in place.

use core::cell::Cell;
use core::mem::offset_of;
use core::num::NonZeroU32;

/// A node, named by its compressed cell pointer: its offset into the GC heap region in units of the 8-byte cell
/// alignment, as a GC::Member holds it. Two node pointers are equal exactly when they name the same node.
///
/// NB: The conservative stack scan recognizes a compressed pointer, so one on the stack keeps its node alive. A
///     collection that moves cells only runs when it does not scan the stack, and so while nothing holds one.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct NodePointer(NonZeroU32);

impl NodePointer {
    /// The compressed cell pointer, which is never 0.
    #[inline(always)]
    pub fn compressed(self) -> u32 {
        self.0.get()
    }
}

/// Where DOM::Node embeds its core, in bytes from the start of the node's cell, which Node.h asserts.
pub const NODE_CORE_OFFSET: usize = 32;

/// A field that holds a node or null, laid out as C++'s GC::Member<DOM::Node>, which is what C++ sees it as: a compressed
/// cell pointer in 32 bits, with 0 for null. Rust and C++ both write it, and the collector rewrites it when it moves the
/// node, so it is a Cell.
#[repr(transparent)]
pub struct NodeMember(Cell<u32>);

impl NodeMember {
    #[inline(always)]
    pub fn get(&self) -> Option<NodePointer> {
        NonZeroU32::new(self.0.get()).map(NodePointer)
    }
}

/// A field that holds a document, laid out as C++'s GC::Member<DOM::Document>, which is what C++ sees it as. A document is
/// a node, whose compressed pointer is the document's, so it reads as a node.
#[repr(transparent)]
pub struct DocumentMember(Cell<u32>);

impl DocumentMember {
    #[inline(always)]
    pub fn get(&self) -> Option<NodePointer> {
        NonZeroU32::new(self.0.get()).map(NodePointer)
    }
}

/// The type of a node, laid out as C++'s DOM::NodeType, which is what C++ sees it as.
/// https://dom.spec.whatwg.org/#dom-node-nodetype
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct NodeType(u16);

impl NodeType {
    pub const INVALID: Self = Self(0);
    pub const ELEMENT_NODE: Self = Self(1);
    pub const ATTRIBUTE_NODE: Self = Self(2);
    pub const TEXT_NODE: Self = Self(3);
    pub const CDATA_SECTION_NODE: Self = Self(4);
    pub const ENTITY_REFERENCE_NODE: Self = Self(5);
    pub const ENTITY_NODE: Self = Self(6);
    pub const PROCESSING_INSTRUCTION_NODE: Self = Self(7);
    pub const COMMENT_NODE: Self = Self(8);
    pub const DOCUMENT_NODE: Self = Self(9);
    pub const DOCUMENT_TYPE_NODE: Self = Self(10);
    pub const DOCUMENT_FRAGMENT_NODE: Self = Self(11);
    pub const NOTATION_NODE: Self = Self(12);
}

/// The tree state of a node, which C++ embeds in DOM::Node as its only storage of it: the node's type, its node
/// document, its root, whether it is connected, and its tree links.
/// https://dom.spec.whatwg.org/#concept-tree
#[repr(C)]
pub struct NodeCore {
    parent: NodeMember,
    first_child: NodeMember,
    last_child: NodeMember,
    next_sibling: NodeMember,
    previous_sibling: NodeMember,
    document: DocumentMember,
    root: NodeMember,
    // NB: The type is set as the node is made, and never changes.
    node_type: NodeType,
    is_connected: Cell<bool>,
}

impl NodeCore {
    #[inline(always)]
    pub fn node_type(&self) -> NodeType {
        self.node_type
    }

    /// https://dom.spec.whatwg.org/#concept-node-document
    #[inline(always)]
    pub fn document(&self) -> NodePointer {
        self.document.get().expect("a node always has a node document")
    }

    /// The root of the node's tree, which links.rs keeps up to date as the node is inserted and removed.
    /// https://dom.spec.whatwg.org/#concept-tree-root
    #[inline(always)]
    pub fn root(&self) -> NodePointer {
        self.root.get().expect("a node always has a root")
    }

    /// https://dom.spec.whatwg.org/#connected
    #[inline(always)]
    pub fn is_connected(&self) -> bool {
        self.is_connected.get()
    }

    #[inline(always)]
    pub fn parent(&self) -> Option<NodePointer> {
        self.parent.get()
    }

    #[inline(always)]
    pub fn first_child(&self) -> Option<NodePointer> {
        self.first_child.get()
    }

    #[inline(always)]
    pub fn last_child(&self) -> Option<NodePointer> {
        self.last_child.get()
    }

    #[inline(always)]
    pub fn next_sibling(&self) -> Option<NodePointer> {
        self.next_sibling.get()
    }

    #[inline(always)]
    pub fn previous_sibling(&self) -> Option<NodePointer> {
        self.previous_sibling.get()
    }
}

#[cfg(test)]
impl NodePointer {
    pub(crate) fn from_compressed_for_test(compressed: u32) -> Self {
        Self(NonZeroU32::new(compressed).unwrap())
    }
}

#[cfg(test)]
impl NodeMember {
    pub(crate) fn set(&self, node: Option<NodePointer>) {
        self.0.set(node.map_or(0, NodePointer::compressed));
    }
}

/// The fields of a core that a test tree writes as it builds its tree.
#[cfg(test)]
pub(crate) struct TestLinks<'a> {
    pub(crate) parent: &'a NodeMember,
    pub(crate) first_child: &'a NodeMember,
    pub(crate) last_child: &'a NodeMember,
    pub(crate) next_sibling: &'a NodeMember,
    pub(crate) previous_sibling: &'a NodeMember,
    pub(crate) root: &'a NodeMember,
}

#[cfg(test)]
impl NodeCore {
    /// The core of a node of `node_type`, `node`, for a test tree: the root of a tree of its own and its own node
    /// document.
    pub(crate) fn for_test(node_type: NodeType, node: NodePointer) -> Self {
        let member = |node: Option<NodePointer>| NodeMember(Cell::new(node.map_or(0, NodePointer::compressed)));
        Self {
            parent: member(None),
            first_child: member(None),
            last_child: member(None),
            next_sibling: member(None),
            previous_sibling: member(None),
            document: DocumentMember(Cell::new(node.compressed())),
            root: member(Some(node)),
            node_type,
            is_connected: Cell::new(false),
        }
    }

    pub(crate) fn links_for_test(&self) -> TestLinks<'_> {
        TestLinks {
            parent: &self.parent,
            first_child: &self.first_child,
            last_child: &self.last_child,
            next_sibling: &self.next_sibling,
            previous_sibling: &self.previous_sibling,
            root: &self.root,
        }
    }
}

// NB: Node.cpp asserts the same layout of the C++ types these are seen as.
const _: () = assert!(size_of::<NodeMember>() == 4 && align_of::<NodeMember>() == 4);
const _: () = assert!(size_of::<DocumentMember>() == 4 && align_of::<DocumentMember>() == 4);
const _: () = assert!(size_of::<NodeType>() == 2 && align_of::<NodeType>() == 2);
const _: () = assert!(size_of::<NodeCore>() == 32 && align_of::<NodeCore>() == 4);
const _: () = assert!(offset_of!(NodeCore, parent) == 0);
const _: () = assert!(offset_of!(NodeCore, first_child) == 4);
const _: () = assert!(offset_of!(NodeCore, last_child) == 8);
const _: () = assert!(offset_of!(NodeCore, next_sibling) == 12);
const _: () = assert!(offset_of!(NodeCore, previous_sibling) == 16);
const _: () = assert!(offset_of!(NodeCore, document) == 20);
const _: () = assert!(offset_of!(NodeCore, root) == 24);
const _: () = assert!(offset_of!(NodeCore, node_type) == 28);
const _: () = assert!(offset_of!(NodeCore, is_connected) == 30);

#[cfg(test)]
mod tests {
    use super::*;

    fn member(compressed: u32) -> NodeMember {
        NodeMember(Cell::new(compressed))
    }

    #[test]
    fn null_member_names_no_node() {
        assert_eq!(member(0).get(), None);
    }

    #[test]
    fn members_name_the_same_node_exactly_when_their_compressed_pointers_are_equal() {
        assert!(member(0x2000_0000).get().is_some());
        assert_eq!(member(0x2000_0000).get(), member(0x2000_0000).get());
        assert_ne!(member(0x2000_0000).get(), member(0x2000_0001).get());
    }

    fn pointer(compressed: u32) -> NodePointer {
        member(compressed).get().unwrap()
    }

    #[test]
    fn each_accessor_reads_its_own_field() {
        let core = NodeCore {
            parent: member(0x2000_0001),
            first_child: member(0x2000_0002),
            last_child: member(0x2000_0003),
            next_sibling: member(0x2000_0004),
            previous_sibling: member(0),
            document: DocumentMember(Cell::new(0x2000_0005)),
            root: member(0x2000_0006),
            node_type: NodeType::TEXT_NODE,
            is_connected: Cell::new(true),
        };
        assert_eq!(core.parent(), Some(pointer(0x2000_0001)));
        assert_eq!(core.first_child(), Some(pointer(0x2000_0002)));
        assert_eq!(core.last_child(), Some(pointer(0x2000_0003)));
        assert_eq!(core.next_sibling(), Some(pointer(0x2000_0004)));
        assert_eq!(core.previous_sibling(), None);
        assert_eq!(core.document(), pointer(0x2000_0005));
        assert_eq!(core.root(), pointer(0x2000_0006));
        assert_eq!(core.node_type(), NodeType::TEXT_NODE);
        assert!(core.is_connected());
    }
}
