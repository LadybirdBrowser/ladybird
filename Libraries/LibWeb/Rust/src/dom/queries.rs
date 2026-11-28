/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The read-only algorithms of nodes, over the trees and the cores: tree order and its positions, inclusion, equality,
//! text content and namespace lookups.
//!
//! Attr nodes have no core: C++ steps from an Attr node to its element before it asks, as the algorithms say, and
//! answers what only Attr nodes decide itself.

use super::hooks;
use super::node::NodeCell;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::strings::BorrowedString;
use super::strings::FlyStringField;
use super::strings::units_equal;
use super::strings::units_equal_ascii;
use crate::stage::MainThread;
use ak::Utf16StringUnits;

crate::stage::main_thread_ffi_entries!(without_host);

/// https://dom.spec.whatwg.org/#dom-node-document_position_disconnected
pub const DOCUMENT_POSITION_DISCONNECTED: u16 = 0x01;
/// https://dom.spec.whatwg.org/#dom-node-document_position_preceding
pub const DOCUMENT_POSITION_PRECEDING: u16 = 0x02;
/// https://dom.spec.whatwg.org/#dom-node-document_position_following
pub const DOCUMENT_POSITION_FOLLOWING: u16 = 0x04;
/// https://dom.spec.whatwg.org/#dom-node-document_position_contains
pub const DOCUMENT_POSITION_CONTAINS: u16 = 0x08;
/// https://dom.spec.whatwg.org/#dom-node-document_position_contained_by
pub const DOCUMENT_POSITION_CONTAINED_BY: u16 = 0x10;
/// https://dom.spec.whatwg.org/#dom-node-document_position_implementation_specific
pub const DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: u16 = 0x20;

/// The number of ancestors of `node`, and its root.
#[inline(always)]
fn depth_and_root(node: NodeRef) -> (usize, NodeRef) {
    let mut depth = 0;
    let mut current = node;
    while let Some(parent) = current.parent() {
        depth += 1;
        current = parent;
    }
    (depth, current)
}

/// The ancestor of `node` that is `steps` steps up from it.
#[inline(always)]
fn ancestor_up(node: NodeRef, steps: usize) -> NodeRef {
    let mut current = node;
    for _ in 0..steps {
        current = current.parent().expect("the node is that deep");
    }
    current
}

/// Steps 6 to 10 of comparing the document positions of `node1` and `node2`, the nodes or the elements of the Attr
/// nodes of steps 2 to 5, where `attr1` and `attr2` say whether there were Attr nodes.
/// https://dom.spec.whatwg.org/#dom-node-comparedocumentposition
fn compare_tree_positions(node1: NodeRef, node2: NodeRef, attr1: bool, attr2: bool) -> u16 {
    // 6. If node1 or node2 is null, or node1's root is not node2's root, then return the result of adding
    //    DOCUMENT_POSITION_DISCONNECTED, DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC, and either
    //    DOCUMENT_POSITION_PRECEDING or DOCUMENT_POSITION_FOLLOWING, with the constraint that this is to be consistent,
    //    together.
    // NB: The caller answers where node1 or node2 is null.
    let (depth1, root1) = depth_and_root(node1);
    let (depth2, root2) = depth_and_root(node2);
    if root1 != root2 {
        // NB: Ordering by address is consistent for as long as the nodes live where they are, as C++ also orders them.
        let order = match node1.cell() as *const NodeCell > node2.cell() as *const NodeCell {
            true => DOCUMENT_POSITION_PRECEDING,
            false => DOCUMENT_POSITION_FOLLOWING,
        };
        return DOCUMENT_POSITION_DISCONNECTED | DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC | order;
    }

    if node1 == node2 {
        // 7. If node1 is an ancestor of node2 and attr1 is null, or node1 is node2 and attr2 is non-null, then return
        //    the result of adding DOCUMENT_POSITION_CONTAINS to DOCUMENT_POSITION_PRECEDING.
        if attr2 {
            return DOCUMENT_POSITION_CONTAINS | DOCUMENT_POSITION_PRECEDING;
        }
        // 8. If node1 is a descendant of node2 and attr2 is null, or node1 is node2 and attr1 is non-null, then return
        //    the result of adding DOCUMENT_POSITION_CONTAINED_BY to DOCUMENT_POSITION_FOLLOWING.
        // NB: Step 1 answers where neither is an Attr node, and step 5 where both are, so attr1 is non-null here.
        return DOCUMENT_POSITION_CONTAINED_BY | DOCUMENT_POSITION_FOLLOWING;
    }

    let ancestor1 = ancestor_up(node1, depth1.saturating_sub(depth2));
    let ancestor2 = ancestor_up(node2, depth2.saturating_sub(depth1));

    if ancestor1 == ancestor2 {
        // 7. If node1 is an ancestor of node2 and attr1 is null, or node1 is node2 and attr2 is non-null, then return
        //    the result of adding DOCUMENT_POSITION_CONTAINS to DOCUMENT_POSITION_PRECEDING.
        if depth1 < depth2 && !attr1 {
            return DOCUMENT_POSITION_CONTAINS | DOCUMENT_POSITION_PRECEDING;
        }
        // 8. If node1 is a descendant of node2 and attr2 is null, or node1 is node2 and attr1 is non-null, then return
        //    the result of adding DOCUMENT_POSITION_CONTAINED_BY to DOCUMENT_POSITION_FOLLOWING.
        if depth1 > depth2 && !attr2 {
            return DOCUMENT_POSITION_CONTAINED_BY | DOCUMENT_POSITION_FOLLOWING;
        }
        // 9. If node1 is preceding node2, then return DOCUMENT_POSITION_PRECEDING.
        // NB: An ancestor precedes its descendants.
        if depth1 < depth2 {
            return DOCUMENT_POSITION_PRECEDING;
        }
        // 10. Return DOCUMENT_POSITION_FOLLOWING.
        return DOCUMENT_POSITION_FOLLOWING;
    }

    // NB: Below the deepest common ancestor, the order of its children that hold node1 and node2 decides.
    let (mut child1, mut child2) = (ancestor1, ancestor2);
    while child1.parent() != child2.parent() {
        child1 = child1.parent().expect("the nodes share a root");
        child2 = child2.parent().expect("the nodes share a root");
    }
    let mut sibling = child1.next_sibling();
    while let Some(node) = sibling {
        // 9. If node1 is preceding node2, then return DOCUMENT_POSITION_PRECEDING.
        if node == child2 {
            return DOCUMENT_POSITION_PRECEDING;
        }
        sibling = node.next_sibling();
    }
    // 10. Return DOCUMENT_POSITION_FOLLOWING.
    DOCUMENT_POSITION_FOLLOWING
}

/// Steps 6 to 10 of compareDocumentPosition(), for `node1` and `node2`, the nodes, or the elements of the Attr nodes
/// steps 2 to 5 look at, where `attr1` and `attr2` say whether there were Attr nodes.
/// https://dom.spec.whatwg.org/#dom-node-comparedocumentposition
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_compare_tree_positions(
    node1: &NodeCell,
    node2: &NodeCell,
    attr1: bool,
    attr2: bool,
) -> u16 {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let [node1, node2] = [node1, node2].map(|cell| unsafe { NodeRef::from_cell(cell, &main_thread) });
    compare_tree_positions(node1, node2, attr1, attr2)
}

/// https://dom.spec.whatwg.org/#concept-shadow-including-root
fn shadow_including_root(node: NodeRef) -> NodeRef {
    // The shadow-including root of an object is its root's host's shadow-including root, if the object's root is a
    // shadow root; otherwise its root.
    let mut root = node.root();
    while root.is_shadow_root() {
        let Some(host) = root.host() else {
            break;
        };
        root = host.root();
    }
    root
}

/// The shadow-including root of the node.
/// https://dom.spec.whatwg.org/#concept-shadow-including-root
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_shadow_including_root(node: &NodeCell) -> &NodeCell {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // SAFETY: The root is live as long as the node is.
    unsafe { &*core::ptr::from_ref(shadow_including_root(node).cell()) }
}

/// Whether `node` is a shadow-including inclusive descendant of `other`.
/// https://dom.spec.whatwg.org/#concept-shadow-including-inclusive-descendant
fn is_shadow_including_inclusive_descendant_of(node: NodeRef, other: NodeRef) -> bool {
    // A shadow-including inclusive descendant is an object or one of its shadow-including descendants.
    // https://dom.spec.whatwg.org/#concept-shadow-including-descendant
    // An object A is a shadow-including descendant of an object B, if A is a descendant of B, or A's root is a shadow
    // root and A's root's host is a shadow-including inclusive descendant of B.
    // NB: One walk up from each node reaches both its ancestors and its root.
    let mut current = node;
    loop {
        if current == other {
            return true;
        }
        if let Some(parent) = current.parent() {
            current = parent;
            continue;
        }
        if !current.is_shadow_root() {
            return false;
        }
        let Some(host) = current.host() else {
            return false;
        };
        current = host;
    }
}

/// Whether `node` is a shadow-including descendant of `other`, or an inclusive one where `inclusive` says so.
/// https://dom.spec.whatwg.org/#concept-shadow-including-descendant
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_is_shadow_including_descendant_of(
    node: &NodeCell,
    other: &NodeCell,
    inclusive: bool,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let [node, other] = [node, other].map(|cell| unsafe { NodeRef::from_cell(cell, &main_thread) });
    if node == other {
        return inclusive;
    }
    is_shadow_including_inclusive_descendant_of(node, other)
}

/// A string built from appended code units, one byte per code unit until a code unit above U+00FF comes along.
struct ConcatenatedUnits {
    latin1: Vec<u8>,
    is_ascii: bool,
    utf16: Option<Vec<u16>>,
}

impl ConcatenatedUnits {
    fn append(&mut self, units: Utf16StringUnits<'_>) {
        if let Some(utf16) = &mut self.utf16 {
            match units {
                Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => {
                    utf16.extend(units.iter().map(|&unit| u16::from(unit)));
                }
                Utf16StringUnits::Utf16(units) => utf16.extend_from_slice(units),
            }
            return;
        }
        match units {
            Utf16StringUnits::Ascii(units) => self.latin1.extend_from_slice(units),
            Utf16StringUnits::Latin1(units) => {
                self.latin1.extend_from_slice(units);
                self.is_ascii = false;
            }
            Utf16StringUnits::Utf16(units) => {
                let mut utf16 = Vec::with_capacity(self.latin1.len() + units.len());
                utf16.extend(self.latin1.iter().map(|&unit| u16::from(unit)));
                utf16.extend_from_slice(units);
                self.utf16 = Some(utf16);
            }
        }
    }

    fn into_string(self) -> ak::Utf16String {
        match self.utf16 {
            Some(utf16) => ak::Utf16String::from_utf16(&utf16),
            None => ak::Utf16String::from_latin1_concatenation(self.latin1.len(), self.is_ascii, [&self.latin1[..]]),
        }
    }
}

/// The concatenation of the data of the Text nodes `nodes` yields, in the order it yields them.
pub(super) fn concatenated_data(main_thread: &MainThread<'_>, nodes: impl Iterator<Item = NodeRef>) -> ak::Utf16String {
    let mut nodes = nodes.map(|node| node.read(main_thread).data());
    let Some(first) = nodes.next() else {
        return ak::Utf16String::from_ascii(b"");
    };
    let Some(second) = nodes.next() else {
        // OPTIMIZATION: The data of one node is that node's own string.
        return first.to_utf16_string();
    };
    // NB: The data is copied as the walk reaches it, as walking the nodes again would cost more than growing the
    //     buffer.
    let mut units = ConcatenatedUnits {
        latin1: Vec::with_capacity(256),
        is_ascii: true,
        utf16: None,
    };
    units.append(first.units());
    units.append(second.units());
    for data in nodes {
        units.append(data.units());
    }
    units.into_string()
}

/// Whether the node is a Text node, which a CDATASection node is too.
#[inline(always)]
pub(super) fn is_text(node: NodeRef) -> bool {
    matches!(node.node_type(), NodeType::TEXT_NODE | NodeType::CDATA_SECTION_NODE)
}

/// The Text node descendants of `node`, in tree order.
fn text_descendants(node: NodeRef) -> impl Iterator<Item = NodeRef> {
    let mut current = node.first_child();
    core::iter::from_fn(move || {
        loop {
            let candidate = current?;
            let candidate_is_text = is_text(candidate);
            // NB: The walk goes down first, then to the next sibling of the nearest inclusive ancestor that has one,
            //     and stops back at the node. A Text node has no children.
            let first_child = match candidate_is_text {
                true => None,
                false => candidate.first_child(),
            };
            current = first_child.or_else(|| {
                let mut up = candidate;
                loop {
                    if up == node {
                        return None;
                    }
                    if let Some(next) = up.next_sibling() {
                        return Some(next);
                    }
                    up = up.parent()?;
                    if up == node {
                        return None;
                    }
                }
            });
            if candidate_is_text {
                return Some(candidate);
            }
        }
    })
}

/// The raw word of a reference to the descendant text content of the node, which passes to the caller.
/// https://dom.spec.whatwg.org/#concept-descendant-text-content
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_descendant_text_content(node: &NodeCell) -> usize {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // The descendant text content of a node node is the concatenation of the data of all the Text node descendants of
    // node, in tree order.
    concatenated_data(&main_thread, text_descendants(node)).into_raw()
}

/// The raw word of a reference to the child text content of the node, which passes to the caller.
/// https://dom.spec.whatwg.org/#concept-child-text-content
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_child_text_content(node: &NodeCell) -> usize {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // The child text content of a node node is the concatenation of the data of all the Text node children of node, in
    // tree order.
    let mut current = node.first_child();
    let children = core::iter::from_fn(move || {
        let child = current?;
        current = child.next_sibling();
        Some(child)
    });
    concatenated_data(&main_thread, children.filter(|child| is_text(*child))).into_raw()
}

/// What locating a namespace answers, as C++ reads it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum LocatedNamespaceKind {
    Null,
    /// https://infra.spec.whatwg.org/#xml-namespace
    Xml,
    /// https://infra.spec.whatwg.org/#xmlns-namespace
    Xmlns,
    /// The string whose raw word the result holds, which the node keeps.
    String,
}

/// What locating a namespace or a namespace prefix answers: a kind, and for LocatedNamespaceKind::String, the raw word
/// of a string the node keeps, which C++ takes a reference of its own to.
#[repr(C)]
pub struct LocatedNamespace {
    pub kind: LocatedNamespaceKind,
    pub string: usize,
}

impl LocatedNamespace {
    const NULL: Self = Self {
        kind: LocatedNamespaceKind::Null,
        string: 0,
    };

    fn string(raw: usize) -> Self {
        Self {
            kind: LocatedNamespaceKind::String,
            string: raw,
        }
    }
}

/// https://infra.spec.whatwg.org/#xmlns-namespace
const XMLNS_NAMESPACE: &str = "http://www.w3.org/2000/xmlns/";

/// The element whose namespaces a lookup on `node` looks at, as the algorithms switch on the interface `node`
/// implements: the element itself, a document's document element, none for a doctype or a document fragment, and
/// otherwise its parent element.
fn element_to_look_up_on(node: NodeRef) -> Option<NodeRef> {
    match node.node_type() {
        NodeType::ELEMENT_NODE => Some(node),
        NodeType::DOCUMENT_NODE => {
            let mut child = node.first_child();
            while let Some(candidate) = child {
                if candidate.is_element() {
                    return Some(candidate);
                }
                child = candidate.next_sibling();
            }
            None
        }
        NodeType::DOCUMENT_TYPE_NODE | NodeType::DOCUMENT_FRAGMENT_NODE => None,
        _ => node.parent().filter(|parent| parent.is_element()),
    }
}

/// https://dom.spec.whatwg.org/#locate-a-namespace
fn locate_a_namespace(node: NodeRef, prefix: Option<BorrowedString>) -> LocatedNamespace {
    // To locate a namespace for a node using prefix, switch on the interface node implements:
    // NB: The interfaces but Element lead to an element, or to null, see element_to_look_up_on().
    let Some(mut element) = element_to_look_up_on(node) else {
        return LocatedNamespace::NULL;
    };
    // Element
    loop {
        // 1. If prefix is "xml", then return the XML namespace.
        // 2. If prefix is "xmlns", then return the XMLNS namespace.
        if let Some(prefix) = prefix {
            if units_equal_ascii(prefix.units(), "xml") {
                return LocatedNamespace {
                    kind: LocatedNamespaceKind::Xml,
                    string: 0,
                };
            }
            if units_equal_ascii(prefix.units(), "xmlns") {
                return LocatedNamespace {
                    kind: LocatedNamespaceKind::Xmlns,
                    string: 0,
                };
            }
        }
        let core = element.element_core().expect("the node is an element");
        let name = core.qualified_name().parts();
        let (namespace, element_prefix) = (name.namespace(), name.prefix());

        // 3. If its namespace is non-null and its namespace prefix is prefix, then return namespace.
        if let Some(namespace) = namespace {
            let prefix_matches = match (element_prefix, prefix) {
                (None, None) => true,
                (Some(element_prefix), Some(prefix)) => units_equal(element_prefix.units(), prefix.units()),
                _ => false,
            };
            if prefix_matches {
                return LocatedNamespace::string(namespace.raw_identity());
            }
        }

        // 4. If it has an attribute whose namespace is the XMLNS namespace, namespace prefix is "xmlns", and local name
        //    is prefix, or if prefix is null and it has an attribute whose namespace is the XMLNS namespace, namespace
        //    prefix is null, and local name is "xmlns", then return its value if it is not the empty string, and null
        //    otherwise.
        for attribute in core.attributes() {
            let name = attribute.name().parts();
            let Some(attribute_namespace) = name.namespace() else {
                continue;
            };
            if !units_equal_ascii(attribute_namespace.units(), XMLNS_NAMESPACE) {
                continue;
            }
            let matches = match (prefix, name.prefix()) {
                (Some(prefix), Some(attribute_prefix)) => {
                    units_equal_ascii(attribute_prefix.units(), "xmlns")
                        && units_equal(name.local_name().units(), prefix.units())
                }
                (None, None) => units_equal_ascii(name.local_name().units(), "xmlns"),
                _ => false,
            };
            if matches {
                let value = attribute.value();
                if value.is_empty() {
                    return LocatedNamespace::NULL;
                }
                return LocatedNamespace::string(value.raw());
            }
        }

        // 5. If its parent element is null, then return null.
        // 6. Return the result of running locate a namespace on its parent element using prefix.
        match element.parent().filter(|parent| parent.is_element()) {
            Some(parent) => element = parent,
            None => return LocatedNamespace::NULL,
        }
    }
}

/// https://dom.spec.whatwg.org/#locate-a-namespace-prefix
fn locate_a_namespace_prefix(mut element: NodeRef, namespace: BorrowedString) -> LocatedNamespace {
    loop {
        let core = element.element_core().expect("the node is an element");
        let name = core.qualified_name().parts();
        let (element_namespace, element_prefix) = (name.namespace(), name.prefix());

        // 1. If element's namespace is namespace and its namespace prefix is non-null, then return its namespace prefix.
        if let (Some(element_namespace), Some(element_prefix)) = (element_namespace, element_prefix)
            && units_equal(element_namespace.units(), namespace.units())
        {
            return LocatedNamespace::string(element_prefix.raw_identity());
        }

        // 2. If element has an attribute whose namespace prefix is "xmlns" and value is namespace, then return
        //    element's first such attribute's local name.
        for attribute in core.attributes() {
            let name = attribute.name().parts();
            if name
                .prefix()
                .is_some_and(|prefix| units_equal_ascii(prefix.units(), "xmlns"))
                && units_equal(attribute.value().units(), namespace.units())
            {
                return LocatedNamespace::string(name.local_name().raw_identity());
            }
        }

        // 3. If element's parent element is not null, then return the result of running locate a namespace prefix on
        //    that element using namespace.
        // 4. Return null.
        match element.parent().filter(|parent| parent.is_element()) {
            Some(parent) => element = parent,
            None => return LocatedNamespace::NULL,
        }
    }
}

/// Locates a namespace for the node using the prefix `prefix`, which is null where `prefix_is_null` says so.
/// https://dom.spec.whatwg.org/#locate-a-namespace
///
/// # Safety
///
/// The node must be live and not an Attr node, `prefix` the raw word of a live string unless `prefix_is_null`, and the
/// call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_locate_a_namespace(
    node: &NodeCell,
    prefix: usize,
    prefix_is_null: bool,
) -> LocatedNamespace {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // SAFETY: The prefix is live for the call.
    let prefix = (!prefix_is_null).then(|| unsafe { BorrowedString::from_raw(prefix) });
    locate_a_namespace(node, prefix)
}

/// The result of lookupPrefix(namespace) on the node, for a namespace that is neither null nor the empty string.
/// https://dom.spec.whatwg.org/#dom-node-lookupprefix
///
/// # Safety
///
/// The node must be live and not an Attr node, `namespace_uri` the raw word of a live string, and the call must be made
/// on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_lookup_prefix(node: &NodeCell, namespace_uri: usize) -> LocatedNamespace {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // SAFETY: The namespace is live for the call.
    let namespace = unsafe { BorrowedString::from_raw(namespace_uri) };
    // 2. Switch on the interface this implements:
    // NB: Each interface but Attr locates a namespace prefix for an element, or answers null, see
    //     element_to_look_up_on().
    match element_to_look_up_on(node) {
        Some(element) => locate_a_namespace_prefix(element, namespace),
        None => LocatedNamespace::NULL,
    }
}

/// Whether `a` equals `b`, but for the state of doctypes, processing instructions and Attr nodes, which C++ compares.
/// https://dom.spec.whatwg.org/#concept-node-equals
fn equals(main_thread: &mut MainThread<'_>, a: NodeRef, b: NodeRef) -> bool {
    if a == b {
        return true;
    }
    // A node A equals a node B if all of the following conditions are true:
    // - A and B implement the same interfaces.
    // NB: The node type tells the interfaces apart, but for Text and CDATASection, which have their own types.
    let node_type = a.node_type();
    if node_type != b.node_type() {
        return false;
    }
    // - The following are equal, switching on the interface A implements:
    match node_type {
        // Element: Its namespace, namespace prefix, local name, and its attribute list's size.
        NodeType::ELEMENT_NODE => {
            let a_core = a.element_core().expect("the node is an element");
            let b_core = b.element_core().expect("the node is an element");
            // NB: Qualified names equal in every part share their parts.
            if a_core.qualified_name().identity() != b_core.qualified_name().identity() {
                return false;
            }
            let same_identity = |x: Option<&FlyStringField>, y: Option<&FlyStringField>| {
                x.map(FlyStringField::raw_identity) == y.map(FlyStringField::raw_identity)
            };
            let (a_attributes, b_attributes) = (a_core.attributes(), b_core.attributes());
            if a_attributes.len() != b_attributes.len() {
                return false;
            }
            // - If A is an element, each attribute in its attribute list has an attribute that equals an attribute in
            //   B's attribute list.
            // NB: Attributes equal when their namespaces, local names and values do.
            for a_attribute in a_attributes {
                let a_name = a_attribute.name();
                let equal = b_attributes.iter().any(|b_attribute| {
                    let b_name = b_attribute.name();
                    a_name.local_name().raw_identity() == b_name.local_name().raw_identity()
                        && same_identity(a_name.namespace(), b_name.namespace())
                        && units_equal(a_attribute.value().units(), b_attribute.value().units())
                });
                if !equal {
                    return false;
                }
            }
        }
        // Comment, Text: Its data.
        NodeType::COMMENT_NODE | NodeType::TEXT_NODE | NodeType::CDATA_SECTION_NODE
            if !units_equal(a.read(main_thread).data().units(), b.read(main_thread).data().units()) =>
        {
            return false;
        }
        // DocumentType: Its name, public ID, and system ID.
        // ProcessingInstruction: Its target and data.
        // Attr: Its namespace, local name, and value.
        NodeType::DOCUMENT_TYPE_NODE | NodeType::PROCESSING_INSTRUCTION_NODE | NodeType::ATTRIBUTE_NODE
            if !hooks::node_state_equals(main_thread, a, b) =>
        {
            return false;
        }
        _ => {}
    }

    // - A and B have the same number of children.
    // - Each child of A equals the child of B at the identical index.
    let (mut a_child, mut b_child) = (a.first_child(), b.first_child());
    loop {
        match (a_child, b_child) {
            (None, None) => return true,
            (Some(a_node), Some(b_node)) => {
                if !equals(main_thread, a_node, b_node) {
                    return false;
                }
                a_child = a_node.next_sibling();
                b_child = b_node.next_sibling();
            }
            _ => return false,
        }
    }
}

/// Whether the node equals `other`, which isEqualNode() answers for a non-null otherNode.
/// https://dom.spec.whatwg.org/#dom-node-isequalnode
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_is_equal_node(node: &NodeCell, other: &NodeCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let [node, other] = [node, other].map(|cell| unsafe { NodeRef::from_cell(cell, &main_thread) });
    equals(&mut main_thread, node, other)
}

/// Whether the node is before `other` in tree order: in the same tree, and preceding it.
/// https://dom.spec.whatwg.org/#concept-tree-preceding
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_is_before(node: &NodeCell, other: &NodeCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let [node, other] = [node, other].map(|cell| unsafe { NodeRef::from_cell(cell, &main_thread) });
    // An object A is preceding an object B if A and B are in the same tree and A comes before B in tree order.
    node != other
        && compare_tree_positions(node, other, false, false)
            & (DOCUMENT_POSITION_DISCONNECTED | DOCUMENT_POSITION_PRECEDING)
            == DOCUMENT_POSITION_PRECEDING
}
