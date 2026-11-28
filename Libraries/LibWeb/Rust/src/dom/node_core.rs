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

use super::strings::StringField;
use super::weak_cells::WeakCellSpan;
use crate::css::style::tree::StyleNodeID;
use crate::css::style::tree::TreeScopeID;

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

    /// The node `compressed` names, which is never 0.
    #[inline(always)]
    pub(super) fn from_compressed(compressed: u32) -> Self {
        Self(NonZeroU32::new(compressed).expect("a node pointer is never null"))
    }
}

/// Where a collector that has to know about stores into cells, like a generational or incremental one, will hook in,
/// as GC::Detail::member_write_barrier() does for the stores C++ makes. It does nothing for now.
#[inline(always)]
fn member_write_barrier(_slot: &Cell<u32>, _compressed: u32) {}

/// Where DOM::Node embeds its core, in bytes from the start of the node's cell, which Node.h asserts.
pub const NODE_CORE_OFFSET: usize = 32;

/// Where DOM::DocumentFragment embeds its core, in bytes from the start of the fragment's cell, which
/// DocumentFragment.cpp asserts.
pub const DOCUMENT_FRAGMENT_CORE_OFFSET: usize = 104;

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

    /// Stores `node`, or null, as GC::Member's assignment does, through the write barrier hook.
    #[inline(always)]
    pub(super) fn set(&self, node: Option<NodePointer>) {
        let compressed = node.map_or(0, NodePointer::compressed);
        member_write_barrier(&self.0, compressed);
        self.0.set(compressed);
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

/// Where DOM::CharacterData embeds its core, in bytes from the start of the node's cell, which CharacterData.cpp asserts.
pub const CHARACTER_DATA_CORE_OFFSET: usize = 88;

/// The state of a CharacterData node beyond that of every node, which C++ embeds in DOM::CharacterData as its only
/// storage of it.
/// https://dom.spec.whatwg.org/#characterdata
#[repr(C)]
pub struct CharacterDataCore {
    data: StringField,
}

impl CharacterDataCore {
    /// https://dom.spec.whatwg.org/#concept-cd-data
    #[inline(always)]
    pub fn data(&self) -> &StringField {
        &self.data
    }
}

/// A field that holds an element or null, laid out as C++'s GC::Member<DOM::Element>, which is what C++ sees it as. An
/// element starts with its node, so it reads as a node.
#[repr(transparent)]
pub struct ElementMember(Cell<u32>);

impl ElementMember {
    #[inline(always)]
    pub fn get(&self) -> Option<NodePointer> {
        NonZeroU32::new(self.0.get()).map(NodePointer)
    }
}

/// A field that holds a shadow root or null, laid out as C++'s GC::Member<DOM::ShadowRoot>, which is what C++ sees it
/// as. A shadow root starts with its node, so it reads as a node.
#[repr(transparent)]
pub struct ShadowRootMember(Cell<u32>);

impl ShadowRootMember {
    #[inline(always)]
    pub fn get(&self) -> Option<NodePointer> {
        NonZeroU32::new(self.0.get()).map(NodePointer)
    }
}

/// The state of a DocumentFragment node beyond that of every node, which C++ embeds in DOM::DocumentFragment as its only
/// storage of it.
/// https://dom.spec.whatwg.org/#documentfragment
#[repr(C)]
pub struct DocumentFragmentCore {
    host: ElementMember,
    /// The style engine's number for the tree scope of a shadow root, or the document's where C++ has numbered none for
    /// it yet. Only a shadow root has a scope of its own.
    style_engine_tree_scope: TreeScopeField,
}

impl DocumentFragmentCore {
    /// https://dom.spec.whatwg.org/#concept-documentfragment-host
    #[inline(always)]
    pub fn host(&self) -> Option<NodePointer> {
        self.host.get()
    }

    #[inline(always)]
    pub fn style_engine_tree_scope(&self) -> TreeScopeID {
        self.style_engine_tree_scope.get()
    }
}

/// Where DOM::Document embeds its core, in bytes from the start of the document's cell, which Document.cpp asserts.
pub const DOCUMENT_CORE_OFFSET: usize = 112;

/// Whether the document may have a node with a blocking wheel event listener, which C++ sets as it adds the first such
/// listener, and as it adopts a node from a document that may have one. It is never cleared.
pub const DOCUMENT_MAY_HAVE_BLOCKING_WHEEL_EVENT_LISTENER: u8 = 1 << 0;

/// The state of a document that the DOM algorithms in Rust read, which C++ embeds in DOM::Document as its only storage
/// of it.
/// https://dom.spec.whatwg.org/#interface-document
#[repr(C)]
pub struct DocumentCore {
    /// The DOCUMENT_* flags.
    flags: Cell<u8>,
    /// The live ranges whose start node's node document is the document, as its DOM::WeakCellRegistry of them mirrors
    /// them, see weak_cells.rs.
    /// https://dom.spec.whatwg.org/#concept-live-range
    live_ranges: WeakCellSpan,
    /// The NodeIterator objects whose root's node document is the document, as its DOM::WeakCellRegistry of them
    /// mirrors them.
    /// https://dom.spec.whatwg.org/#nodeiterator
    node_iterators: WeakCellSpan,
}

impl DocumentCore {
    #[inline(always)]
    pub fn may_have_blocking_wheel_event_listener(&self) -> bool {
        self.flags.get() & DOCUMENT_MAY_HAVE_BLOCKING_WHEEL_EVENT_LISTENER != 0
    }

    #[inline(always)]
    pub fn live_ranges(&self) -> &WeakCellSpan {
        &self.live_ranges
    }

    #[inline(always)]
    pub fn node_iterators(&self) -> &WeakCellSpan {
        &self.node_iterators
    }
}

/// A field that holds a tree scope's number, laid out as C++'s CSS::TreeScopeID, which is what C++ sees it as, with 0 for
/// the document's.
#[repr(transparent)]
pub struct TreeScopeField(Cell<u32>);

impl TreeScopeField {
    #[inline(always)]
    pub fn get(&self) -> TreeScopeID {
        TreeScopeID(self.0.get())
    }
}

/// A field that holds a node's identity in its document's style engine, laid out as C++'s CSS::StyleNodeID, which is what
/// C++ sees it as, with 0 for none.
#[repr(transparent)]
pub struct StyleNodeField(Cell<u32>);

impl StyleNodeField {
    #[inline(always)]
    pub fn get(&self) -> Option<StyleNodeID> {
        StyleNodeID::from_raw(self.0.get())
    }
}

/// A byte of flags that only C++ reads and writes, laid out as C++'s DOM::CppNodeFlags, which is what C++ sees it as.
/// The node keeps it in its core, beside the core's own flags, so that DOM::Node does not grow.
#[repr(transparent)]
pub struct CppNodeFlags(Cell<u8>);

// The flags of a node's core, a bit each in its flags byte, which C++ reads and writes as well.

/// Whether the node is connected.
/// https://dom.spec.whatwg.org/#connected
pub const NODE_IS_CONNECTED: u8 = 1 << 0;
pub const NODE_IS_SHADOW_ROOT: u8 = 1 << 1;
/// Whether the node is in an editable subtree, as Node::recompute_editable_subtree_flag() works it out.
pub const NODE_IN_EDITABLE_SUBTREE: u8 = 1 << 4;

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
/// document, its root, whether it is connected, its tree links, and how it stands in its document's style engine.
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
    /// The node's identity in its document's style engine. Only an element, a text node, a shadow root or a document
    /// takes one.
    style_node_id: StyleNodeField,
    // NB: The type is set as the node is made, and never changes.
    node_type: NodeType,
    /// The NODE_* flags. C++ sets NODE_IS_SHADOW_ROOT as it constructs a shadow root, and never changes it.
    flags: Cell<u8>,
    cpp_flags: CppNodeFlags,
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

    #[inline(always)]
    fn has_flag(&self, flag: u8) -> bool {
        self.flags.get() & flag != 0
    }

    /// https://dom.spec.whatwg.org/#connected
    #[inline(always)]
    pub fn is_connected(&self) -> bool {
        self.has_flag(NODE_IS_CONNECTED)
    }

    /// https://dom.spec.whatwg.org/#concept-shadow-root
    #[inline(always)]
    pub fn is_shadow_root(&self) -> bool {
        self.has_flag(NODE_IS_SHADOW_ROOT)
    }

    #[inline(always)]
    pub fn is_in_editable_subtree(&self) -> bool {
        self.has_flag(NODE_IN_EDITABLE_SUBTREE)
    }

    #[inline(always)]
    pub(super) fn set_in_editable_subtree(&self, value: bool) {
        let flags = self.flags.get() & !NODE_IN_EDITABLE_SUBTREE;
        self.flags
            .set(if value { flags | NODE_IN_EDITABLE_SUBTREE } else { flags });
    }

    #[inline(always)]
    pub fn style_node_id(&self) -> Option<StyleNodeID> {
        self.style_node_id.get()
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

    #[inline(always)]
    pub(super) fn set_parent(&self, node: Option<NodePointer>) {
        self.parent.set(node);
    }

    #[inline(always)]
    pub(super) fn set_first_child(&self, node: Option<NodePointer>) {
        self.first_child.set(node);
    }

    #[inline(always)]
    pub(super) fn set_last_child(&self, node: Option<NodePointer>) {
        self.last_child.set(node);
    }

    #[inline(always)]
    pub(super) fn set_next_sibling(&self, node: Option<NodePointer>) {
        self.next_sibling.set(node);
    }

    #[inline(always)]
    pub(super) fn set_previous_sibling(&self, node: Option<NodePointer>) {
        self.previous_sibling.set(node);
    }

    #[inline(always)]
    pub(super) fn set_root(&self, node: NodePointer) {
        self.root.set(Some(node));
    }
}

#[cfg(test)]
impl NodeMember {
    pub(crate) fn null_for_test() -> Self {
        Self(Cell::new(0))
    }
}

#[cfg(test)]
impl ShadowRootMember {
    pub(crate) fn null_for_test() -> Self {
        Self(Cell::new(0))
    }

    pub(crate) fn set_for_test(&self, shadow_root: Option<NodePointer>) {
        self.0.set(shadow_root.map_or(0, NodePointer::compressed));
    }
}

#[cfg(test)]
impl CharacterDataCore {
    pub(crate) fn for_test(data: &str) -> Self {
        Self {
            data: StringField::for_test(data),
        }
    }
}

#[cfg(test)]
impl DocumentFragmentCore {
    pub(crate) fn null_for_test() -> Self {
        Self {
            host: ElementMember(Cell::new(0)),
            style_engine_tree_scope: TreeScopeField(Cell::new(0)),
        }
    }

    pub(crate) fn set_host_for_test(&self, host: Option<NodePointer>) {
        self.host.0.set(host.map_or(0, NodePointer::compressed));
    }

    pub(crate) fn set_style_engine_tree_scope_for_test(&self, tree_scope: TreeScopeID) {
        self.style_engine_tree_scope.0.set(tree_scope.0);
    }
}

#[cfg(test)]
impl DocumentCore {
    pub(crate) fn for_test() -> Self {
        Self {
            flags: Cell::new(0),
            live_ranges: WeakCellSpan::empty_for_test(),
            node_iterators: WeakCellSpan::empty_for_test(),
        }
    }

    pub(crate) fn set_may_have_blocking_wheel_event_listener_for_test(&self) {
        self.flags
            .set(self.flags.get() | DOCUMENT_MAY_HAVE_BLOCKING_WHEEL_EVENT_LISTENER);
    }
}

#[cfg(test)]
impl NodeCore {
    pub(crate) fn set_style_node_id_for_test(&self, node: Option<StyleNodeID>) {
        self.style_node_id.0.set(node.map_or(0, StyleNodeID::raw));
    }

    pub(crate) fn set_document_for_test(&self, document: NodePointer) {
        self.document.0.set(document.compressed());
    }

    pub(crate) fn set_is_connected_for_test(&self) {
        self.flags.set(self.flags.get() | NODE_IS_CONNECTED);
    }

    pub(crate) fn set_is_shadow_root_for_test(&mut self) {
        self.flags.set(self.flags.get() | NODE_IS_SHADOW_ROOT);
    }

    /// The core of a node of `node_type`, `node`, for a test tree: the root of a tree of its own, whose node document is
    /// `document`.
    pub(crate) fn for_test(node_type: NodeType, node: NodePointer, document: NodePointer) -> Self {
        let member = |node: Option<NodePointer>| NodeMember(Cell::new(node.map_or(0, NodePointer::compressed)));
        Self {
            parent: member(None),
            first_child: member(None),
            last_child: member(None),
            next_sibling: member(None),
            previous_sibling: member(None),
            document: DocumentMember(Cell::new(document.compressed())),
            root: member(Some(node)),
            style_node_id: StyleNodeField(Cell::new(0)),
            node_type,
            flags: Cell::new(0),
            cpp_flags: CppNodeFlags(Cell::new(0)),
        }
    }
}

// NB: Node.cpp asserts the same layout of the C++ types these are seen as.
const _: () = assert!(size_of::<NodeMember>() == 4 && align_of::<NodeMember>() == 4);
const _: () = assert!(size_of::<DocumentMember>() == 4 && align_of::<DocumentMember>() == 4);
const _: () = assert!(size_of::<NodeType>() == 2 && align_of::<NodeType>() == 2);
const _: () = assert!(size_of::<ElementMember>() == 4 && align_of::<ElementMember>() == 4);
const _: () = assert!(size_of::<ShadowRootMember>() == 4 && align_of::<ShadowRootMember>() == 4);
// NB: CharacterData.cpp asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<CharacterDataCore>() == 8 && align_of::<CharacterDataCore>() == 8);
const _: () = assert!(offset_of!(CharacterDataCore, data) == 0);
// NB: DocumentFragment.cpp asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<DocumentFragmentCore>() == 8 && align_of::<DocumentFragmentCore>() == 4);
const _: () = assert!(offset_of!(DocumentFragmentCore, host) == 0);
const _: () = assert!(offset_of!(DocumentFragmentCore, style_engine_tree_scope) == 4);
// NB: Document.cpp asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<DocumentCore>() == 40 && align_of::<DocumentCore>() == 8);
const _: () = assert!(offset_of!(DocumentCore, flags) == 0);
const _: () = assert!(offset_of!(DocumentCore, live_ranges) == 8);
const _: () = assert!(offset_of!(DocumentCore, node_iterators) == 24);
const _: () = assert!(size_of::<TreeScopeField>() == 4 && align_of::<TreeScopeField>() == 4);
const _: () = assert!(size_of::<StyleNodeField>() == 4 && align_of::<StyleNodeField>() == 4);
const _: () = assert!(size_of::<CppNodeFlags>() == 1 && align_of::<CppNodeFlags>() == 1);
const _: () = assert!(size_of::<NodeCore>() == 36 && align_of::<NodeCore>() == 4);
const _: () = assert!(offset_of!(NodeCore, parent) == 0);
const _: () = assert!(offset_of!(NodeCore, first_child) == 4);
const _: () = assert!(offset_of!(NodeCore, last_child) == 8);
const _: () = assert!(offset_of!(NodeCore, next_sibling) == 12);
const _: () = assert!(offset_of!(NodeCore, previous_sibling) == 16);
const _: () = assert!(offset_of!(NodeCore, document) == 20);
const _: () = assert!(offset_of!(NodeCore, root) == 24);
const _: () = assert!(offset_of!(NodeCore, style_node_id) == 28);
const _: () = assert!(offset_of!(NodeCore, node_type) == 32);
const _: () = assert!(offset_of!(NodeCore, flags) == 34);
const _: () = assert!(offset_of!(NodeCore, cpp_flags) == 35);

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
            style_node_id: StyleNodeField(Cell::new(7)),
            node_type: NodeType::TEXT_NODE,
            flags: Cell::new(NODE_IS_CONNECTED | NODE_IS_SHADOW_ROOT),
            cpp_flags: CppNodeFlags(Cell::new(0xff)),
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
        assert!(core.is_shadow_root());
        assert_eq!(core.style_node_id(), StyleNodeID::from_raw(7));
    }
}
