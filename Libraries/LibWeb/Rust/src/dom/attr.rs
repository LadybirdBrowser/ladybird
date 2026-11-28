/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Attr nodes, and the NamedNodeMap that holds those of an element's attributes.
//! https://dom.spec.whatwg.org/#interface-attr
//! https://dom.spec.whatwg.org/#interface-namednodemap
//!
//! An element keeps its attributes as names and values in its attribute list, see element_core.rs, and makes an Attr
//! node for one only once script asks for it, through the element's NamedNodeMap. The map holds the Attr nodes it made
//! in a [`NamedNodeMapCore`], which DOM::NamedNodeMap embeds, and the element keeps the map in a field before the header
//! of its attribute list, which only the lists of elements that have a map have. So an element whose attribute is
//! removed or replaced finds the Attr node to detach from it in Rust, where it has one.
//!
//! An Attr node keeps its name, its value and its element in an [`AttrCore`], which DOM::Attr embeds. The value it keeps
//! is the attribute's while it has no element: an Attr node with an element reads its value from the element's
//! attribute list, which is where the value changes.

use core::mem::offset_of;
use core::ptr::NonNull;

use libgc_api::Trace;

use super::attributes;
use super::node::NodeCell;
use super::node_core::ElementMember;
use super::node_ref::NodeRef;
use super::qualified_name::QualifiedNameField;
use super::qualified_name::QualifiedNameImpl;
use super::slots::NodeListField;
use super::strings::FlyStringField;
use super::strings::StringField;
use super::strings::units_equal_ascii;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// The state of an Attr node beyond that of every node, which C++ embeds in DOM::Attr as its only storage of it.
/// https://dom.spec.whatwg.org/#concept-attribute
#[derive(Trace)]
#[repr(C)]
pub struct AttrCore {
    /// The attribute's namespace, namespace prefix and local name.
    qualified_name: QualifiedNameField,
    /// https://dom.spec.whatwg.org/#concept-attribute-value, while the Attr node has no element.
    value: StringField,
    /// https://dom.spec.whatwg.org/#concept-attribute-element
    owner_element: ElementMember,
}

impl AttrCore {
    #[inline(always)]
    pub fn qualified_name(&self) -> &QualifiedNameField {
        &self.qualified_name
    }
}

/// Where DOM::NamedNodeMap embeds its core, in bytes from the start of the map's cell, which NamedNodeMap.cpp asserts.
pub const NAMED_NODE_MAP_CORE_OFFSET: usize = 32;

/// The state of a NamedNodeMap beyond its element, which C++ embeds in DOM::NamedNodeMap as its only storage of it.
#[derive(Trace)]
#[repr(C)]
pub struct NamedNodeMapCore {
    /// The Attr nodes the map made for the attributes of its element, which they are the attributes of. C++ has Rust
    /// free the list as it destroys the map.
    attribute_nodes: NodeListField,
}

const _: () = assert!(size_of::<NamedNodeMapCore>() == 16 && align_of::<NamedNodeMapCore>() == 8);
const _: () = assert!(offset_of!(NamedNodeMapCore, attribute_nodes) == 0);

/// Whether the names `a` and `b` have the same namespace and local name, which their interned strings tell.
#[inline(always)]
fn same_namespace_and_local_name(a: &QualifiedNameImpl, b: &QualifiedNameImpl) -> bool {
    a.local_name().raw_identity() == b.local_name().raw_identity()
        && a.namespace().map(FlyStringField::raw_identity) == b.namespace().map(FlyStringField::raw_identity)
}

/// The value of the Attr node whose core is `core`.
/// https://dom.spec.whatwg.org/#concept-attribute-value
fn attr_value(main_thread: &mut MainThread<'_>, core: &AttrCore) -> ak::Utf16String {
    let Some(element) = core.owner_element.get() else {
        return core.value.to_utf16_string();
    };
    // SAFETY: An Attr node keeps its element alive.
    let element = unsafe { NodeRef::from_pointer(element) };
    let name = core.qualified_name.parts();
    attributes::synchronize_attribute(main_thread, element, || {
        name.namespace().is_none() && units_equal_ascii(name.local_name().units(), "style")
    });
    let attribute = element
        .element_core()
        .expect("an Attr node's element is an element")
        .attributes()
        .iter()
        .find(|attribute| same_namespace_and_local_name(attribute.name().parts(), name))
        .expect("the element of an Attr node has its attribute");
    attribute.value().to_utf16_string()
}

/// Sets the element of the Attr node that `element`'s attribute named `name` has, if it has one, to null, and gives
/// it the value `value`, which was the attribute's, as removing or replacing the attribute does.
pub(super) fn detach_attribute_node(element: NodeRef, name: &QualifiedNameImpl, value: &ak::Utf16String) {
    let Some(map) = element.attribute_map_core() else {
        return;
    };
    let nodes = &map.attribute_nodes;
    for index in 0..nodes.len() {
        let attr = nodes.get(index);
        let core = attr.attr_core().expect("a NamedNodeMap holds Attr nodes");
        if !same_namespace_and_local_name(core.qualified_name.parts(), name) {
            continue;
        }
        // SAFETY: Nothing borrows the Attr node's value.
        drop(unsafe { core.value.replace(value.clone()) });
        core.owner_element.set(None);
        nodes.remove(index);
        return;
    }
}

/// "Append an attribute", for the Attr node `attr`, which has no element, and `element`, after C++ set the Attr node's
/// node document to that of `element`, which has a NamedNodeMap.
/// https://dom.spec.whatwg.org/#concept-element-attributes-append
fn append_attribute_node(main_thread: &mut MainThread<'_>, element: NodeRef, attr: NodeRef) {
    let core = attr.attr_core().expect("an Attr node has an Attr core");
    let name = attributes::hold_qualified_name(core.qualified_name.parts());
    let value = core.value.to_utf16_string();
    // NB: The value is held for the steps, which can take the attribute out of the list.
    let new_value = value.clone();

    // 1. Append attribute to element's attribute list.
    // NB: The list takes a reference to the name of its own.
    attributes::append_to_attribute_list(
        element,
        attributes::hold_qualified_name(core.qualified_name.parts()),
        value,
    );

    // 2. Set attribute's element to element.
    core.owner_element.set(Some(element.pointer()));

    // 3. Set attribute's node document to element's node document.
    // NB: C++ did, before.
    element
        .attribute_map_core()
        .expect("an element that is appended an Attr node has a NamedNodeMap")
        .attribute_nodes
        .push(attr);

    // 4. Handle attribute changes for attribute with element, null, and attribute's value.
    attributes::handle_attribute_changes(main_thread, element, name, None, Some(new_value.raw_identity()));
    attributes::release_qualified_name(main_thread, name);
}

/// "Replace an attribute", for the Attr node `old_attr`, the attribute at `index` of `element`'s attribute list, and
/// `new_attr`, which has no element, after C++ set the node document of `new_attr` to that of `element`.
/// https://dom.spec.whatwg.org/#concept-element-attributes-replace
fn replace_attribute_node(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    old_attr: NodeRef,
    new_attr: NodeRef,
    index: usize,
) {
    let old_core = old_attr.attr_core().expect("an Attr node has an Attr core");
    assert!(old_core.owner_element.get() == Some(element.pointer()));
    let new_core = new_attr.attr_core().expect("an Attr node has an Attr core");

    // 1. Let element be oldAttribute's element.
    // 2. Replace oldAttribute by newAttribute in element's attribute list.
    let new_name = attributes::hold_qualified_name(new_core.qualified_name.parts());
    let new_value = new_core.value.to_utf16_string();
    let core = element.element_core().expect("only an element has attributes");
    // SAFETY: Nothing borrows an attribute of the list. The list takes references to the name and value of its own.
    let (old_name, old_value) = unsafe {
        core.attribute_list().replace(
            index,
            attributes::hold_qualified_name(new_name.as_ref()),
            new_value.clone(),
        )
    };

    // 3. Set newAttribute's element to element.
    new_core.owner_element.set(Some(element.pointer()));

    // 4. Set newAttribute's node document to element's node document.
    // NB: C++ did, before.

    // 5. Set oldAttribute's element to null.
    // SAFETY: The list held a reference to the name, which passed to this function.
    detach_attribute_node(element, unsafe { old_name.as_ref() }, &old_value);
    element
        .attribute_map_core()
        .expect("an element whose attribute is replaced by an Attr node has a NamedNodeMap")
        .attribute_nodes
        .push(new_attr);

    // 6. Handle attribute changes for oldAttribute with element, oldAttribute's value, and newAttribute's value.
    attributes::handle_attribute_changes(
        main_thread,
        element,
        new_name,
        Some(old_value.raw_identity()),
        Some(new_value.raw_identity()),
    );
    attributes::release_qualified_name(main_thread, new_name);
    attributes::release_qualified_name(main_thread, old_name);
}

/// The value of the Attr node `attr`, whose reference passes to the caller, for DOM::Attr.
/// https://dom.spec.whatwg.org/#concept-attribute-value
///
/// # Safety
///
/// `attr` must be a live Attr node, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_attr_value(attr: &NodeCell) -> usize {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let attr = unsafe { NodeRef::from_cell(attr, &main_thread) };
    attr_value(
        &mut main_thread,
        attr.attr_core().expect("an Attr node has an Attr core"),
    )
    .into_raw()
}

/// "Append an attribute" for the Attr node `attr` and `element`, see [`append_attribute_node`].
///
/// # Safety
///
/// The nodes must be live, `attr` an Attr node with no element, `element` an element with a NamedNodeMap, and the call
/// must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_append_attribute_node(element: &NodeCell, attr: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let (element, attr) = unsafe {
        (
            NodeRef::from_cell(element, &main_thread),
            NodeRef::from_cell(attr, &main_thread),
        )
    };
    append_attribute_node(&mut main_thread, element, attr);
}

/// "Replace an attribute" for the Attr nodes `old_attr` and `new_attr`, see [`replace_attribute_node`].
///
/// # Safety
///
/// The nodes must be live, `old_attr` the Attr node of the attribute at `index` of `element`'s attribute list,
/// `new_attr` an Attr node with no element, `element` an element with a NamedNodeMap, and the call must be made on the
/// main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_replace_attribute_node(
    element: &NodeCell,
    old_attr: &NodeCell,
    new_attr: &NodeCell,
    index: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let (element, old_attr, new_attr) = unsafe {
        (
            NodeRef::from_cell(element, &main_thread),
            NodeRef::from_cell(old_attr, &main_thread),
            NodeRef::from_cell(new_attr, &main_thread),
        )
    };
    replace_attribute_node(&mut main_thread, element, old_attr, new_attr, index);
}

/// Has the NamedNodeMap whose core is `map` hold the Attr node `attr`, which it made for an attribute of its element.
///
/// # Safety
///
/// `map` must be the core of a live NamedNodeMap, `attr` a live Attr node whose element is the map's, and the call must
/// be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_named_node_map_append_attribute_node(map: &NamedNodeMapCore, attr: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let attr = unsafe { NodeRef::from_cell(attr, &main_thread) };
    map.attribute_nodes.push(attr);
}

/// Gives the attribute list of `element` a field for the element's NamedNodeMap, and answers it, laid out as a
/// GC::Member<DOM::NamedNodeMap>, for C++ to store the map it makes in.
///
/// # Safety
///
/// `element` must be a live element, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_element_ensure_attribute_map_slot(element: &NodeCell) -> NonNull<u32> {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    let slot = element
        .element_core()
        .expect("only an element has a NamedNodeMap")
        .attribute_list()
        .ensure_attribute_map();
    NonNull::new(slot.as_ptr()).expect("a field is never null")
}

/// Frees the storage of a NamedNodeMap's core, for DOM::NamedNodeMap as it is destroyed.
///
/// # Safety
///
/// `map` must be the core of a NamedNodeMap that is being destroyed, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_named_node_map_core_destroy(map: &NamedNodeMapCore) {
    map.attribute_nodes.free();
}
