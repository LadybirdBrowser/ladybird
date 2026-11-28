/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Live collections, HTMLCollection and the live NodeList objects: their state, which C++ keeps in cores whose layout
//! is defined here, the walks that find the nodes they represent, and the filters that need no C++ state.
//! https://dom.spec.whatwg.org/#concept-collection
//!
//! A collection represents the nodes under its root that its filter matches, in tree order. It remembers the last node
//! it reached by index, its cursor, and the number of nodes it represents once it has counted them, and walks from the
//! cursor, the start or the end to the node it is asked for. Asking for the nodes in turn so costs one step of the walk
//! each. Once it is asked for a node more than a step from the cursor, the first node and the last node, it also
//! remembers the list of its nodes, so that asking for them in any order costs no walk at all.
//!
//! What a collection remembers holds while the versions it was found at are the current ones, see CollectionValidity.
//! The collection holds the nodes it remembers weakly: while the versions are current, they are all in the tree its
//! root is in, which the root keeps alive, and C++ forgets them once one of them dies.
//! The filters that C++ keeps are called through hooks::collection_filter_matches(), which runs no script and changes
//! no tree. The collections themselves are C++ cells, which never move, and which the C++ caller keeps alive.

use core::cell::Cell;
use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::mem::offset_of;

use libgc_api::Trace;

use super::child_index::element_children_stamp;
use super::element::Element;
use super::element_core::ClassNamesField;
use super::hooks;
use super::node::NodeCell;
use super::node_class::NodeClassId;
use super::node_core::COLLECTION_ATTRIBUTE_TYPE_COUNT;
use super::node_core::NodeMember;
use super::node_ref::NodeRef;
use super::strings::FlyStringField;
use super::strings::OptionalFlyStringField;
use super::strings::equals_ignoring_ascii_case;
use super::tree_version::dom_tree_version;
use super::tree_version::last_stamp;
use super::tree_version::take_next_stamp;
use crate::stage::MainThread;

super::trace::fields_without_cells!(CollectionFilter, CollectionScope, CollectionValidity);

crate::stage::main_thread_ffi_entries!(without_host);

/// Where DOM::HTMLCollection and DOM::LiveNodeList embed their cores, in bytes from the start of their cells, which
/// HTMLCollection.cpp and LiveNodeList.cpp assert.
pub const COLLECTION_CORE_OFFSET: usize = 72;

/// Where DOM::HTMLCollection embeds its names, right after its core, which HTMLCollection.cpp asserts.
pub const COLLECTION_NAMES_OFFSET: usize = 128;

const _: () = assert!(COLLECTION_NAMES_OFFSET == COLLECTION_CORE_OFFSET + size_of::<CollectionCore>());

/// The length of a collection that has not counted its nodes since they last changed.
pub const COLLECTION_LENGTH_UNKNOWN: u32 = u32::MAX;

/// The collection represents nodes of any type that its filter matches, as a live NodeList does, rather than only
/// elements, as an HTMLCollection does.
pub const COLLECTION_OF_NODES: u8 = 1 << 0;
/// The ClassNames filter compares classes without their ASCII case, for a document in quirks mode.
pub const COLLECTION_IN_QUIRKS_MODE: u8 = 1 << 1;

/// Which nodes a collection's filter matches, of those its scope and type allow.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CollectionFilter {
    /// The ones the C++ filter of the collection matches, which hooks::collection_filter_matches() asks.
    Host,
    /// All of them.
    All,
    /// The elements whose qualified name is the name.
    QualifiedName,
    /// The elements in the HTML namespace whose qualified name is the other name, which is the name in ASCII lowercase,
    /// and the other elements whose qualified name is the name.
    QualifiedNameInHtmlDocument,
    /// The elements whose local name is the name.
    LocalName,
    /// The elements whose namespace is the name, or the null namespace where there is no name.
    Namespace,
    /// The elements whose namespace is the name, or the null namespace where there is no name, and whose local name is
    /// the other name.
    NamespaceAndLocalName,
    /// The elements that have every class of the class names, of which there is at least one.
    ClassNames,
}

/// https://dom.spec.whatwg.org/#concept-collection
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CollectionScope {
    /// The children of the root.
    Children,
    /// The descendants of the root.
    Descendants,
}

/// Which changes make a collection find its nodes again.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CollectionValidity {
    /// A change to the children of any node of the root's node document that adds or removes an element, or a change
    /// to an attribute that the collection's attribute type counts.
    ElementTree,
    /// A change to the children of the root that adds or removes an element, as child_index::element_children_stamp()
    /// tells them, or a change to an attribute that the collection's attribute type counts, for a collection of
    /// elements whose scope is the root's children. NB: The stamp is the generation of the root's child list and its
    ///     last element child, and so would be mistaken for the same after its generation wrapped around exactly while
    ///     the last element child stayed, which would take billions of insertions into it with no read in between.
    ElementChildren,
    /// A change to the children of any node of the root's node document.
    NodeTree,
    /// Any change to the tree the root is in, as its DOM tree version counts them, or to the children of any node of the
    /// root's node document.
    DomTree,
}

/// The state of a live collection, but for its C++ filter, which C++ keeps.
#[derive(Trace)]
#[repr(C)]
pub struct CollectionCore {
    /// https://dom.spec.whatwg.org/#concept-collection
    root: NodeMember,
    /// The last node the collection reached by index, or null for none. The collection holds it weakly, see the module.
    #[gc(untraced = "the collection holds its cursor weakly")]
    cursor: NodeMember,
    /// The index of the cursor.
    cursor_index: Cell<u32>,
    /// The number of nodes the collection represents, or COLLECTION_LENGTH_UNKNOWN.
    length: Cell<u32>,
    /// The tree version the cursor and the length were found at, or for CollectionValidity::ElementChildren the stamp
    /// of the root's element children, see CollectionValidity.
    tree_version: Cell<u64>,
    /// The version of the attributes of the collection's attribute type, or the node tree version for
    /// CollectionValidity::DomTree, the cursor and the length were found at.
    attribute_version: Cell<u64>,
    /// The last stamp handed out when the versions were last found current, which tells that they still are while no
    /// version has taken a stamp since.
    stamp: Cell<u64>,
    // NB: C++ sets the fields below as it constructs the collection, and never changes them after that.
    filter: CollectionFilter,
    scope: CollectionScope,
    validity: CollectionValidity,
    /// The index of the DocumentCore attribute version that a change to the attributes the filter reads moves, plus
    /// one, or 0 where the filter reads none.
    attribute_type: u8,
    /// The COLLECTION_* flags.
    flags: u8,
    /// A byte that only C++ reads and writes, for the state of its collection classes.
    cpp_state: u8,
    /// The nodes the collection represents, as many as its length, or null where it remembers no list of them. The
    /// collection owns the list, which forget_nodes() frees, and holds its nodes weakly, see the module.
    #[gc(untraced = "the collection holds the nodes of its list weakly")]
    list: Cell<*mut NodeMember>,
}

const _: () = assert!(size_of::<CollectionCore>() == 56 && align_of::<CollectionCore>() == 8);
// NB: HTMLCollection.cpp asserts the same layout of the struct it includes.
const _: () = assert!(offset_of!(CollectionCore, stamp) == 32);
const _: () = assert!(offset_of!(CollectionCore, filter) == 40);
const _: () = assert!(offset_of!(CollectionCore, list) == 48);

/// The names a named filter compares with, which only DOM::HTMLCollection has, right after its core, as only its
/// filters are named. C++ sets them as it constructs the collection, and never changes them after that.
#[repr(C)]
pub struct CollectionNames {
    /// The name the filter compares with, see CollectionFilter.
    name: OptionalFlyStringField,
    /// The other name the filter compares with, see CollectionFilter.
    other_name: OptionalFlyStringField,
    /// The classes the ClassNames filter looks for.
    class_names: ClassNamesField,
}

const _: () = assert!(size_of::<CollectionNames>() == 40 && align_of::<CollectionNames>() == 8);
// NB: HTMLCollection.cpp asserts the same layout of the struct it includes.
const _: () = assert!(offset_of!(CollectionNames, class_names) == 16);

impl CollectionCore {
    #[inline(always)]
    fn root(&self) -> NodeRef {
        // SAFETY: A collection keeps its root alive.
        unsafe { NodeRef::from_member(&self.root) }.expect("a collection has a root")
    }

    #[inline(always)]
    fn cursor(&self) -> Option<(NodeRef, u32)> {
        // SAFETY: The cursor is only read once the versions are found current, so it is alive, see the module.
        let cursor = unsafe { NodeRef::from_member(&self.cursor) }?;
        Some((cursor, self.cursor_index.get()))
    }

    #[inline(always)]
    fn set_cursor(&self, node: NodeRef, index: u32) {
        self.cursor.set(Some(node.pointer()));
        self.cursor_index.set(index);
    }

    /// The nodes the collection represents, where it remembers the list of them.
    #[inline(always)]
    fn list(&self) -> Option<&[NodeMember]> {
        let list = self.list.get();
        // SAFETY: A list that is not null holds as many nodes as the length, see the field.
        (!list.is_null()).then(|| unsafe { ak::slice_from_raw_parts(list, self.length.get() as usize) })
    }

    /// Remembers `nodes` as the list of the nodes the collection represents, and their number as its length.
    fn set_list(&self, nodes: Vec<NodeMember>) {
        debug_assert!(self.list.get().is_null());
        let length = u32::try_from(nodes.len())
            .ok()
            .filter(|&length| length != COLLECTION_LENGTH_UNKNOWN)
            .expect("a collection represents fewer nodes than COLLECTION_LENGTH_UNKNOWN");
        self.length.set(length);
        self.list
            .set(Box::into_raw(nodes.into_boxed_slice()).cast::<NodeMember>());
    }

    /// Forgets the cursor, the length and the list, which no longer hold.
    #[inline(always)]
    fn forget_nodes(&self) {
        let list = self.list.replace(core::ptr::null_mut());
        if !list.is_null() {
            // SAFETY: set_list() made the list from a boxed slice as long as the length.
            drop(unsafe { Box::from_raw(core::ptr::slice_from_raw_parts_mut(list, self.length.get() as usize)) });
        }
        self.cursor.set(None);
        self.cursor_index.set(0);
        self.length.set(COLLECTION_LENGTH_UNKNOWN);
    }

    /// The versions a collection's nodes would be found at now, see CollectionValidity.
    #[inline(always)]
    fn current_versions(&self) -> (u64, u64) {
        let root = self.root();
        match self.validity {
            CollectionValidity::ElementTree => (
                root.document_core().element_tree_version(),
                self.attribute_version_now(root),
            ),
            CollectionValidity::ElementChildren => (element_children_stamp(root), self.attribute_version_now(root)),
            CollectionValidity::NodeTree => (root.document_core().node_tree_version(), 0),
            CollectionValidity::DomTree => (dom_tree_version(root), root.document_core().node_tree_version()),
        }
    }

    /// The version of the attributes of the collection's attribute type in the node document of `root`, or 0 where its
    /// filter reads none.
    #[inline(always)]
    fn attribute_version_now(&self, root: NodeRef) -> u64 {
        match self.attribute_type {
            0 => 0,
            attribute_type => root
                .document_core()
                .collection_attribute_version(usize::from(attribute_type - 1)),
        }
    }

    /// Whether the versions the cursor and the length were found at are the current ones, see CollectionValidity.
    #[inline(always)]
    fn versions_are_current(&self, main_thread: &MainThread<'_>) -> bool {
        let last_stamp = last_stamp(main_thread);
        if self.stamp.get() == last_stamp {
            return true;
        }
        if !self.versions_were_found_current() {
            return false;
        }
        self.stamp.set(last_stamp);
        true
    }

    #[inline(never)]
    fn versions_were_found_current(&self) -> bool {
        let root = self.root();
        let document = root.document_core();
        match self.validity {
            CollectionValidity::ElementTree => {
                self.tree_version.get() == document.element_tree_version()
                    && self.attribute_version.get() == self.attribute_version_now(root)
            }
            CollectionValidity::ElementChildren => {
                self.tree_version.get() == element_children_stamp(root)
                    && self.attribute_version.get() == self.attribute_version_now(root)
            }
            CollectionValidity::NodeTree => self.tree_version.get() == document.node_tree_version(),
            CollectionValidity::DomTree => {
                self.tree_version.get() == dom_tree_version(root)
                    && self.attribute_version.get() == document.node_tree_version()
            }
        }
    }

    /// Brings the versions up to date, and forgets the cursor and the length if they changed. Answers whether the
    /// nodes the collection found still hold.
    #[inline(always)]
    fn validate(&self, main_thread: &MainThread<'_>) -> bool {
        if self.versions_are_current(main_thread) {
            return true;
        }
        let (tree_version, attribute_version) = self.current_versions();
        self.tree_version.set(tree_version);
        self.attribute_version.set(attribute_version);
        self.stamp.set(last_stamp(main_thread));
        self.forget_nodes();
        false
    }
}

/// A DOM::HTMLCollection or DOM::LiveNodeList as C++ allocates it: the start of its cell, with its core
/// COLLECTION_CORE_OFFSET bytes into it.
#[repr(C)]
pub struct CollectionCell {
    // NB: The cell's state changes behind shared references to it.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

impl CollectionCell {
    #[inline(always)]
    fn core(&self) -> &CollectionCore {
        // SAFETY: Every HTMLCollection and LiveNodeList embeds its core COLLECTION_CORE_OFFSET bytes into its cell, which
        //         HTMLCollection.cpp and LiveNodeList.cpp assert.
        unsafe {
            &*(self as *const Self)
                .byte_add(COLLECTION_CORE_OFFSET)
                .cast::<CollectionCore>()
        }
    }

    /// The names of a collection whose filter is a named one, see CollectionNames.
    #[inline(always)]
    fn names(&self) -> &CollectionNames {
        debug_assert!(!matches!(
            self.core().filter,
            CollectionFilter::Host | CollectionFilter::All
        ));
        // SAFETY: Only a DOM::HTMLCollection has a named filter, and it embeds its names COLLECTION_NAMES_OFFSET bytes
        //         into its cell, which HTMLCollection.cpp asserts.
        unsafe {
            &*(self as *const Self)
                .byte_add(COLLECTION_NAMES_OFFSET)
                .cast::<CollectionNames>()
        }
    }
}

/// Whether `element` has the class `class_name`, compared without its ASCII case where `ignoring_ascii_case` says so.
#[inline(always)]
fn has_class(element: Element<'_>, class_name: &FlyStringField, ignoring_ascii_case: bool) -> bool {
    if ignoring_ascii_case {
        return has_class_ignoring_ascii_case(element, class_name);
    }
    let class_name = class_name.raw_identity();
    for class in element.classes() {
        if class.raw_identity() == class_name {
            return true;
        }
    }
    false
}

#[inline(never)]
fn has_class_ignoring_ascii_case(element: Element<'_>, class_name: &FlyStringField) -> bool {
    element
        .classes()
        .iter()
        .any(|class| equals_ignoring_ascii_case(class.units(), class_name.units()))
}

#[inline(always)]
fn identity(name: Option<&FlyStringField>) -> Option<usize> {
    name.map(FlyStringField::raw_identity)
}

/// Whether the filter of `collection`, which is a named filter, matches `element`.
#[inline(always)]
fn element_matches(collection: &CollectionCell, element: Element<'_>) -> bool {
    let core = collection.core();
    let names = collection.names();
    let name = || names.name.get().expect("the filter has a name");
    let other_name = || names.other_name.get().expect("the filter has another name");
    let qualified_name = || element.core().qualified_name().as_string().raw_identity();
    match core.filter {
        CollectionFilter::Host | CollectionFilter::All => unreachable!("the filter is a named one"),
        CollectionFilter::QualifiedName => qualified_name() == name().raw_identity(),
        CollectionFilter::QualifiedNameInHtmlDocument => {
            // NB: An element is in the HTML namespace exactly when it is an HTMLElement.
            let is_html = element.node().class_id().is_a(NodeClassId::HTMLElement);
            qualified_name() == if is_html { other_name() } else { name() }.raw_identity()
        }
        CollectionFilter::LocalName => element.local_name().raw_identity() == name().raw_identity(),
        CollectionFilter::Namespace => identity(element.namespace()) == identity(names.name.get()),
        CollectionFilter::NamespaceAndLocalName => {
            identity(element.namespace()) == identity(names.name.get())
                && element.local_name().raw_identity() == other_name().raw_identity()
        }
        CollectionFilter::ClassNames => {
            let class_names = names.class_names.as_slice();
            let ignoring_ascii_case = core.flags & COLLECTION_IN_QUIRKS_MODE != 0;
            if class_names.is_empty() {
                return false;
            }
            for class_name in class_names {
                if !has_class(element, class_name, ignoring_ascii_case) {
                    return false;
                }
            }
            true
        }
    }
}

/// Whether `collection` represents `node`, which is in its scope.
#[inline(always)]
fn matches(main_thread: &mut MainThread<'_>, collection: &CollectionCell, node: NodeRef) -> bool {
    let core = collection.core();
    match core.filter {
        CollectionFilter::Host => {
            (core.flags & COLLECTION_OF_NODES != 0 || node.is_element())
                && hooks::collection_filter_matches(main_thread, collection, node)
        }
        _ => builtin_filter_matches(main_thread, collection, node),
    }
}

/// Whether `collection`, whose filter is no Host filter, represents `node`, which is in its scope.
#[inline(always)]
fn builtin_filter_matches(main_thread: &MainThread<'_>, collection: &CollectionCell, node: NodeRef) -> bool {
    let core = collection.core();
    if core.flags & COLLECTION_OF_NODES == 0 && !node.is_element() {
        return false;
    }
    match core.filter {
        CollectionFilter::All => true,
        _ => element_matches(
            collection,
            node.read(main_thread)
                .as_element()
                .expect("a named filter matches elements only"),
        ),
    }
}

/// The node after `node` in tree order that is in the scope of `core`, or none after the last.
#[inline(always)]
fn next_in_scope(core: &CollectionCore, node: NodeRef) -> Option<NodeRef> {
    if core.scope == CollectionScope::Children {
        return node.next_sibling();
    }
    if let Some(first_child) = node.first_child() {
        return Some(first_child);
    }
    let root = core.root();
    let mut current = node;
    while current != root {
        if let Some(next_sibling) = current.next_sibling() {
            return Some(next_sibling);
        }
        current = current.parent().expect("a descendant of the root has a parent");
    }
    None
}

/// The node before `node` in tree order that is in the scope of `core`, or none before the first.
#[inline(always)]
fn previous_in_scope(core: &CollectionCore, node: NodeRef) -> Option<NodeRef> {
    if core.scope == CollectionScope::Children {
        return node.previous_sibling();
    }
    let Some(mut previous) = node.previous_sibling() else {
        let parent = node.parent().expect("a descendant of the root has a parent");
        return (parent != core.root()).then_some(parent);
    };
    while let Some(last_child) = previous.last_child() {
        previous = last_child;
    }
    Some(previous)
}

/// The first node in the scope of `core`, or none if there is none.
#[inline(always)]
fn first_in_scope(core: &CollectionCore) -> Option<NodeRef> {
    core.root().first_child()
}

/// The first node `collection` represents from `start` on in tree order, or none.
#[inline(always)]
fn find_forward(
    main_thread: &mut MainThread<'_>,
    collection: &CollectionCell,
    start: Option<NodeRef>,
) -> Option<NodeRef> {
    let core = collection.core();
    let mut candidate = start;
    while let Some(node) = candidate {
        if matches(main_thread, collection, node) {
            return Some(node);
        }
        candidate = next_in_scope(core, node);
    }
    None
}

/// The last node `collection` represents from `start` back in tree order, or none.
#[inline(always)]
fn find_backward(
    main_thread: &mut MainThread<'_>,
    collection: &CollectionCell,
    start: Option<NodeRef>,
) -> Option<NodeRef> {
    let core = collection.core();
    let mut candidate = start;
    while let Some(node) = candidate {
        if matches(main_thread, collection, node) {
            return Some(node);
        }
        candidate = previous_in_scope(core, node);
    }
    None
}

/// The number of nodes `collection` represents.
/// https://dom.spec.whatwg.org/#dom-htmlcollection-length
/// https://dom.spec.whatwg.org/#dom-nodelist-length
#[inline(always)]
fn length(main_thread: &mut MainThread<'_>, collection: &CollectionCell) -> u32 {
    let core = collection.core();
    if core.versions_are_current(main_thread) && core.length.get() != COLLECTION_LENGTH_UNKNOWN {
        return core.length.get();
    }
    count(main_thread, collection)
}

/// Counts the nodes `collection` represents, whose number it does not know.
#[inline(never)]
fn count(main_thread: &mut MainThread<'_>, collection: &CollectionCell) -> u32 {
    let core = collection.core();
    if core.validate(main_thread) && core.length.get() != COLLECTION_LENGTH_UNKNOWN {
        return core.length.get();
    }
    // NB: The nodes up to the cursor are counted already.
    let (mut length, mut node) = match core.cursor() {
        Some((cursor, index)) => (
            index + 1,
            find_forward(main_thread, collection, next_in_scope(core, cursor)),
        ),
        None => {
            let first = find_forward(main_thread, collection, first_in_scope(core));
            // NB: Counting the nodes is mostly followed by asking for them in turn.
            if let Some(first) = first {
                core.set_cursor(first, 0);
            }
            (0, first)
        }
    };
    while let Some(current) = node {
        length += 1;
        node = find_forward(main_thread, collection, next_in_scope(core, current));
    }
    core.length.set(length);
    length
}

/// The node at `index` among the nodes `collection` represents, or none if there are no more nodes than that.
/// https://dom.spec.whatwg.org/#dom-htmlcollection-item
/// https://dom.spec.whatwg.org/#dom-nodelist-item
#[inline(always)]
fn item(main_thread: &mut MainThread<'_>, collection: &CollectionCell, index: u32) -> Option<NodeRef> {
    let core = collection.core();
    // NB: Asking for the nodes in turn asks for the cursor's node or the one after it.
    if core.versions_are_current(main_thread)
        && let Some((cursor, cursor_index)) = core.cursor()
    {
        if cursor_index == index {
            return Some(cursor);
        }
        if cursor_index.wrapping_add(1) == index {
            match core.filter {
                CollectionFilter::All => {
                    if let Some(next) = next_of_type(core, cursor) {
                        core.set_cursor(next, index);
                        return Some(next);
                    }
                }
                CollectionFilter::Host => {}
                _ => return next_matching_a_named_filter(main_thread, collection, cursor, index),
            }
        }
    }
    walk_to_item(main_thread, collection, index)
}

/// The first node after `node` in the scope of `core` of the type of node the collection represents, or none.
#[inline(always)]
fn next_of_type(core: &CollectionCore, node: NodeRef) -> Option<NodeRef> {
    let mut next = next_in_scope(core, node)?;
    if core.flags & COLLECTION_OF_NODES == 0 {
        while !next.is_element() {
            next = next_in_scope(core, next)?;
        }
    }
    Some(next)
}

/// The node at `index` among the nodes `collection` represents, which is the one after its cursor at `cursor`, where
/// its filter is a named one.
#[inline(never)]
fn next_matching_a_named_filter(
    main_thread: &mut MainThread<'_>,
    collection: &CollectionCell,
    cursor: NodeRef,
    index: u32,
) -> Option<NodeRef> {
    let core = collection.core();
    let mut candidate = next_in_scope(core, cursor);
    while let Some(next) = candidate {
        if builtin_filter_matches(main_thread, collection, next) {
            core.set_cursor(next, index);
            return Some(next);
        }
        candidate = next_in_scope(core, next);
    }
    // NB: The walk went past the last node, so it counted them all.
    core.length.set(index);
    None
}

/// The node at `index` among the nodes `collection` represents, or none, by walking from the nearest node it knows.
#[inline(never)]
fn walk_to_item(main_thread: &mut MainThread<'_>, collection: &CollectionCell, index: u32) -> Option<NodeRef> {
    let core = collection.core();
    core.validate(main_thread);
    let length = core.length.get();
    if length != COLLECTION_LENGTH_UNKNOWN && index >= length {
        return None;
    }

    if let Some(list) = core.list() {
        // SAFETY: The versions are current, so the nodes of the list are alive, see the module.
        let node = unsafe { NodeRef::from_member(&list[index as usize]) }.expect("a list holds no null");
        // NB: Asking for the nodes in turn from here on takes the cursor's fast path.
        core.set_cursor(node, index);
        return Some(node);
    }

    // NB: A node that is more than a step from the cursor, the first node and the last node is asked for out of turn,
    //     which a walk does not answer well, so the collection remembers the list of its nodes.
    if let Some((_, cursor_index)) = core.cursor() {
        let steps_from_last = match length {
            COLLECTION_LENGTH_UNKNOWN => u32::MAX,
            length => length - 1 - index,
        };
        if cursor_index.abs_diff(index).min(index).min(steps_from_last) > 1 {
            return item_from_new_list(main_thread, collection, index);
        }
    }

    // Walk from the nearest of the cursor, the first node and, once the length is known, the last node.
    let (mut node, mut node_index) = match core.cursor() {
        Some((cursor, cursor_index)) if cursor_index == index => return Some(cursor),
        Some((cursor, cursor_index)) if cursor_index < index || cursor_index - index < index => (cursor, cursor_index),
        _ => {
            let first = find_forward(main_thread, collection, first_in_scope(core));
            let Some(first) = first else {
                core.length.set(0);
                return None;
            };
            (first, 0)
        }
    };
    if length != COLLECTION_LENGTH_UNKNOWN && index > node_index && length - 1 - index < index - node_index {
        let last = core.root().last_child();
        let last = match core.scope {
            CollectionScope::Children => last,
            CollectionScope::Descendants => last.map(|mut last| {
                while let Some(last_child) = last.last_child() {
                    last = last_child;
                }
                last
            }),
        };
        node = find_backward(main_thread, collection, last).expect("a collection with a length has a last node");
        node_index = length - 1;
    }

    while node_index < index {
        let Some(next) = find_forward(main_thread, collection, next_in_scope(core, node)) else {
            // NB: The walk went past the last node, so it counted them all.
            core.length.set(node_index + 1);
            core.set_cursor(node, node_index);
            return None;
        };
        node = next;
        node_index += 1;
    }
    while node_index > index {
        node = find_backward(main_thread, collection, previous_in_scope(core, node))
            .expect("a node before the cursor is represented");
        node_index -= 1;
    }
    core.set_cursor(node, node_index);
    Some(node)
}

/// The node at `index` among the nodes `collection` represents, or none, from the list of them that it makes and
/// remembers, which it does not have.
#[inline(never)]
fn item_from_new_list(main_thread: &mut MainThread<'_>, collection: &CollectionCell, index: u32) -> Option<NodeRef> {
    let core = collection.core();
    let mut nodes = Vec::new();
    let mut node = find_forward(main_thread, collection, first_in_scope(core));
    while let Some(current) = node {
        let member = NodeMember::null();
        member.set(Some(current.pointer()));
        nodes.push(member);
        node = find_forward(main_thread, collection, next_in_scope(core, current));
    }
    core.set_list(nodes);
    let member = core.list()?.get(index as usize)?;
    // SAFETY: The collection just found the nodes of the list in the tree below its root, which it keeps alive.
    let node = unsafe { NodeRef::from_member(member) }.expect("a list holds no null");
    core.set_cursor(node, index);
    Some(node)
}

/// The number of nodes `collection` represents, for DOM::HTMLCollection and DOM::LiveNodeList.
///
/// # Safety
///
/// The collection must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_collection_length(collection: &CollectionCell) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    length(&mut main_thread, collection)
}

/// The node at `index` among the nodes `collection` represents, or null, for DOM::HTMLCollection and
/// DOM::LiveNodeList.
///
/// # Safety
///
/// The collection must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_collection_item(collection: &CollectionCell, index: u32) -> *const NodeCell {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    item(&mut main_thread, collection, index).map_or(core::ptr::null(), |node| node.cell() as *const NodeCell)
}

/// Brings the versions of `collection` up to date, and answers whether the nodes it found at the ones before still
/// hold, for a DOM::HTMLCollection that keeps the nodes it represents in an order of its own.
///
/// # Safety
///
/// The collection must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_collection_validate(collection: &CollectionCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    collection.core().validate(&main_thread)
}

/// Remembers `nodes` as the list of the nodes `collection` represents, in place of what it remembered, for a
/// DOM::HTMLCollection that keeps the nodes it represents in an order of its own, which C++ reads from the list.
///
/// # Safety
///
/// The collection must be live, its versions current, `nodes` must be what it represents now, and the call must be
/// made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_collection_remember_nodes_in_own_order(
    collection: &CollectionCell,
    nodes: ak::Span<'_, NodeRef>,
) {
    let core = collection.core();
    core.forget_nodes();
    core.set_list(
        nodes
            .as_slice()
            .iter()
            .map(|node| {
                let member = NodeMember::null();
                member.set(Some(node.pointer()));
                member
            })
            .collect(),
    );
}

/// Forgets the nodes `collection` found, which C++ does when the state of the document its C++ filter reads changes, and
/// as it destroys the collection, for DOM::HTMLCollection and DOM::LiveNodeList.
///
/// # Safety
///
/// The collection must be live or being destroyed, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_collection_forget_nodes(collection: &CollectionCell) {
    collection.core().forget_nodes();
}

/// Whether the versions the nodes of `collection` were found at are the current ones, for DOM::HTMLCollection.
///
/// # Safety
///
/// The collection must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_collection_versions_are_current(collection: &CollectionCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    collection.core().versions_are_current(&main_thread)
}

/// Records that the attributes of an element whose node document is `document` changed, which the filters of the
/// collections whose attribute types are in `attribute_types` read, as a mask of the bits of their indices into
/// DocumentCore's attribute versions.
pub(super) fn collection_attributes_changed(main_thread: &mut MainThread<'_>, document: NodeRef, attribute_types: u8) {
    let stamp = take_next_stamp(main_thread);
    let document = document.document_core();
    for attribute_type in 0..COLLECTION_ATTRIBUTE_TYPE_COUNT {
        if attribute_types & (1 << attribute_type) != 0 {
            document.set_collection_attribute_version(attribute_type, stamp);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::element_core::ElementCore;
    use super::super::links::insert_child;
    use super::super::links::remove_child;
    use super::super::node_core::NodeType;
    use super::super::qualified_name::QualifiedNameField;
    use super::super::test_tree;
    use super::*;

    /// A collection of `filter` under `root`, laid out in a buffer the test leaks as C++ lays out a collection's cell.
    fn collection(
        root: NodeRef,
        scope: CollectionScope,
        filter: CollectionFilter,
        flags: u8,
        name: Option<&str>,
        class_names: &[&str],
    ) -> &'static CollectionCell {
        collection_with_validity(
            root,
            scope,
            filter,
            flags,
            name,
            class_names,
            CollectionValidity::NodeTree,
        )
    }

    fn collection_with_validity(
        root: NodeRef,
        scope: CollectionScope,
        filter: CollectionFilter,
        flags: u8,
        name: Option<&str>,
        class_names: &[&str],
        validity: CollectionValidity,
    ) -> &'static CollectionCell {
        let core = CollectionCore {
            root: NodeMember::null_for_test(),
            cursor: NodeMember::null_for_test(),
            cursor_index: Cell::new(0),
            length: Cell::new(COLLECTION_LENGTH_UNKNOWN),
            tree_version: Cell::new(0),
            attribute_version: Cell::new(0),
            stamp: Cell::new(0),
            filter,
            scope,
            validity,
            attribute_type: 0,
            flags,
            cpp_state: 0,
            list: Cell::new(core::ptr::null_mut()),
        };
        core.root.set(Some(root.pointer()));
        let names = CollectionNames {
            name: OptionalFlyStringField::for_test(name),
            other_name: OptionalFlyStringField::for_test(None),
            class_names: ClassNamesField::for_test(class_names),
        };
        #[repr(C)]
        struct TestCell {
            _base: [u64; COLLECTION_CORE_OFFSET / 8],
            core: CollectionCore,
            names: CollectionNames,
        }
        let cell = Box::leak(Box::new(TestCell {
            _base: [0; COLLECTION_CORE_OFFSET / 8],
            core,
            names,
        }));
        // SAFETY: The buffer starts with the cell, and has its core COLLECTION_CORE_OFFSET bytes into it, followed by its
        //         names.
        unsafe { &*(cell as *const TestCell).cast::<CollectionCell>() }
    }

    fn element(main_thread: &MainThread<'_>, local_name: &str, classes: &[&str]) -> NodeRef {
        let mut core = ElementCore::for_test(QualifiedNameField::for_test(local_name, None));
        core.set_classes_for_test(classes);
        NodeRef::from(test_tree::element(main_thread, core).node())
    }

    fn items(main_thread: &mut MainThread<'_>, collection: &CollectionCell, indices: &[u32]) -> Vec<Option<NodeRef>> {
        indices
            .iter()
            .map(|&index| item(main_thread, collection, index))
            .collect()
    }

    #[test]
    fn a_collection_walks_to_the_nodes_it_represents() {
        let mut main_thread = MainThread::for_test();
        let document = NodeRef::from(test_tree::node(&main_thread, NodeType::DOCUMENT_NODE));
        let root = element(&main_thread, "div", &[]);
        let [a, b, c] = [
            element(&main_thread, "p", &["x"]),
            element(&main_thread, "span", &["x", "y"]),
            element(&main_thread, "p", &["y"]),
        ];
        let text = NodeRef::from(test_tree::node(&main_thread, NodeType::TEXT_NODE));
        for node in [root, a, b, c, text] {
            test_tree::set_document(node.read(&main_thread), document.read(&main_thread));
        }
        insert_child(&mut main_thread, root, a, None);
        insert_child(&mut main_thread, a, b, None);
        insert_child(&mut main_thread, root, text, None);
        insert_child(&mut main_thread, root, c, None);

        let all = collection(root, CollectionScope::Descendants, CollectionFilter::All, 0, None, &[]);
        assert_eq!(length(&mut main_thread, all), 3);
        assert_eq!(
            items(&mut main_thread, all, &[0, 1, 2, 3]),
            [Some(a), Some(b), Some(c), None]
        );
        assert_eq!(items(&mut main_thread, all, &[2, 1, 0]), [Some(c), Some(b), Some(a)]);

        let children = collection(
            root,
            CollectionScope::Children,
            CollectionFilter::All,
            COLLECTION_OF_NODES,
            None,
            &[],
        );
        assert_eq!(
            items(&mut main_thread, children, &[0, 1, 2, 3]),
            [Some(a), Some(text), Some(c), None]
        );
        assert_eq!(length(&mut main_thread, children), 3);

        let by_name = collection(
            root,
            CollectionScope::Descendants,
            CollectionFilter::LocalName,
            0,
            Some("p"),
            &[],
        );
        assert_eq!(items(&mut main_thread, by_name, &[1, 0, 2]), [Some(c), Some(a), None]);

        let by_classes = collection(
            root,
            CollectionScope::Descendants,
            CollectionFilter::ClassNames,
            0,
            None,
            &["y", "x"],
        );
        assert_eq!(length(&mut main_thread, by_classes), 1);
        assert_eq!(item(&mut main_thread, by_classes, 0), Some(b));
        let quirky = collection(
            root,
            CollectionScope::Descendants,
            CollectionFilter::ClassNames,
            COLLECTION_IN_QUIRKS_MODE,
            None,
            &["Y"],
        );
        assert_eq!(items(&mut main_thread, quirky, &[0, 1, 2]), [Some(b), Some(c), None]);
        let none = collection(
            root,
            CollectionScope::Descendants,
            CollectionFilter::ClassNames,
            0,
            None,
            &[],
        );
        assert_eq!(length(&mut main_thread, none), 0);
    }

    #[test]
    fn a_collection_finds_its_nodes_again_after_the_tree_changes() {
        let mut main_thread = MainThread::for_test();
        let document = NodeRef::from(test_tree::node(&main_thread, NodeType::DOCUMENT_NODE));
        let root = element(&main_thread, "ul", &[]);
        let [a, b, c] = core::array::from_fn(|_| element(&main_thread, "li", &[]));
        for node in [root, a, b, c] {
            test_tree::set_document(node.read(&main_thread), document.read(&main_thread));
        }
        insert_child(&mut main_thread, root, a, None);
        insert_child(&mut main_thread, root, c, None);

        let children = collection(root, CollectionScope::Children, CollectionFilter::All, 0, None, &[]);
        assert_eq!(items(&mut main_thread, children, &[0, 1]), [Some(a), Some(c)]);
        assert_eq!(length(&mut main_thread, children), 2);

        insert_child(&mut main_thread, root, b, Some(c));
        assert_eq!(length(&mut main_thread, children), 3);
        assert_eq!(
            items(&mut main_thread, children, &[2, 1, 0]),
            [Some(c), Some(b), Some(a)]
        );

        remove_child(&mut main_thread, root, a);
        assert_eq!(items(&mut main_thread, children, &[0, 1, 2]), [Some(b), Some(c), None]);
        assert_eq!(length(&mut main_thread, children), 2);
    }

    #[test]
    fn a_collection_of_children_keeps_what_it_found_until_its_element_children_change() {
        let mut main_thread = MainThread::for_test();
        let document = NodeRef::from(test_tree::node(&main_thread, NodeType::DOCUMENT_NODE));
        let root = element(&main_thread, "ul", &[]);
        let [a, b, c, nested] = core::array::from_fn(|_| element(&main_thread, "li", &[]));
        let text = NodeRef::from(test_tree::node(&main_thread, NodeType::TEXT_NODE));
        for node in [root, a, b, c, nested, text] {
            test_tree::set_document(node.read(&main_thread), document.read(&main_thread));
        }
        insert_child(&mut main_thread, root, a, None);
        insert_child(&mut main_thread, root, b, None);

        let children = collection_with_validity(
            root,
            CollectionScope::Children,
            CollectionFilter::All,
            0,
            None,
            &[],
            CollectionValidity::ElementChildren,
        );
        // NB: Whether the collection still holds the nodes it found, which it forgets otherwise.
        let still_holds = |main_thread: &MainThread<'_>| children.core().validate(main_thread);
        assert_eq!(length(&mut main_thread, children), 2);

        insert_child(&mut main_thread, a, nested, None);
        insert_child(&mut main_thread, root, text, None);
        assert!(still_holds(&main_thread));

        insert_child(&mut main_thread, root, c, None);
        assert!(!still_holds(&main_thread));
        assert_eq!(
            items(&mut main_thread, children, &[0, 1, 2]),
            [Some(a), Some(b), Some(c)]
        );
        assert_eq!(length(&mut main_thread, children), 3);

        remove_child(&mut main_thread, a, nested);
        assert!(still_holds(&main_thread));
        // NB: Removing and appending elements at the end ends up with the last element child the collection saw.
        remove_child(&mut main_thread, root, c);
        remove_child(&mut main_thread, root, b);
        insert_child(&mut main_thread, root, nested, None);
        insert_child(&mut main_thread, root, c, None);
        assert!(!still_holds(&main_thread));
        assert_eq!(
            items(&mut main_thread, children, &[0, 1, 2]),
            [Some(a), Some(nested), Some(c)]
        );
    }

    #[test]
    fn a_collection_asked_for_its_nodes_out_of_turn_remembers_the_list_of_them() {
        let mut main_thread = MainThread::for_test();
        let document = NodeRef::from(test_tree::node(&main_thread, NodeType::DOCUMENT_NODE));
        let root = element(&main_thread, "ul", &[]);
        let nodes: [NodeRef; 6] = core::array::from_fn(|_| element(&main_thread, "li", &[]));
        test_tree::set_document(root.read(&main_thread), document.read(&main_thread));
        for node in nodes {
            test_tree::set_document(node.read(&main_thread), document.read(&main_thread));
            insert_child(&mut main_thread, root, node, None);
        }

        let children = collection(root, CollectionScope::Children, CollectionFilter::All, 0, None, &[]);
        assert_eq!(
            items(&mut main_thread, children, &[0, 1]),
            [Some(nodes[0]), Some(nodes[1])]
        );
        assert!(children.core().list().is_none());
        assert_eq!(
            items(&mut main_thread, children, &[4, 2, 5, 6, 0]),
            [Some(nodes[4]), Some(nodes[2]), Some(nodes[5]), None, Some(nodes[0])]
        );
        assert_eq!(children.core().list().map(<[NodeMember]>::len), Some(6));

        remove_child(&mut main_thread, root, nodes[3]);
        assert_eq!(items(&mut main_thread, children, &[3]), [Some(nodes[4])]);
        assert!(children.core().list().is_none());
        assert_eq!(length(&mut main_thread, children), 5);
    }
}
