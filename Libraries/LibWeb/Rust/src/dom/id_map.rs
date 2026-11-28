/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The elements of a connected tree by their IDs, which a document keeps for its document tree and a shadow root for
//! its shadow tree, and getting an element by ID with them.
//! https://dom.spec.whatwg.org/#concept-id
//!
//! Each map lives in a buffer Rust allocates, which an [`IdMapField`] of the tree scope's core points at. It keeps the
//! connected elements of the tree that have an ID, by ID, and is kept up to date as such elements join and leave the
//! tree and as their IDs change. Getting an element by ID then looks the ID up rather than walk the tree.
//!
//! The map holds its elements weakly, as GC::WeakImpls, which C++ makes and releases through IdMapHooks. The collector
//! points one at the new address of an element it moves, and nulls one whose element dies, so an element the map lost
//! track of gives a miss rather than a freed pointer. Elements leave the map as they leave the tree, so no entry is
//! meant to outlive its element.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16StringUnits;
use hashbrown::HashTable;
use smallvec::SmallVec;

use super::character_data::units_of_view;
use super::hooks;
use super::node::NodeCell;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::rooted_nodes::RootedNodeListCore;
use super::rooted_nodes::RootedNodes;
use super::strings::BorrowedString;
use super::strings::FlyStringField;
use super::strings::string_hash;
use super::strings::units_equal;
use super::strings::units_hash;
use super::weak_cells::WEAK_IMPL_VALUE_OFFSET;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// The elements of a connected tree by their IDs, laid out as C++ embeds it in a tree scope's core: a pointer to the
/// map, which Rust allocates as the first element with an ID joins the tree, or null while it has none.
#[repr(C)]
pub struct IdMapField {
    map: Cell<*mut IdMap>,
}

const _: () = assert!(size_of::<IdMapField>() == 8 && align_of::<IdMapField>() == 8);

struct IdMap {
    entries: HashTable<IdEntry>,
}

/// The elements of the tree whose ID is `id`, in no particular order.
struct IdEntry {
    /// The hash AK gives the ID.
    hash: u32,
    id: ak::Utf16FlyString,
    /// NB: Most IDs name one element, which needs no allocation of its own.
    elements: SmallVec<[WeakElement; 1]>,
    /// The index in `elements` of the first of them in tree order, or NO_FIRST where it is to be found.
    first: Cell<u32>,
}

const NO_FIRST: u32 = u32::MAX;

/// The hash the table keys an entry by, which spreads AK's 32-bit hash over 64 bits: the table tells entries apart in a
/// probe by the top bits of the hash, which AK's leaves zero.
#[inline(always)]
fn table_hash(hash: u32) -> u64 {
    u64::from(hash).wrapping_mul(0x9E37_79B9_7F4A_7C15)
}

/// An element the map holds weakly, as a pointer to a GC::WeakImpl to which it holds a reference.
#[derive(Clone, Copy)]
struct WeakElement(NonNull<c_void>);

impl WeakElement {
    /// The element, or none if it has died.
    #[inline(always)]
    fn get(self) -> Option<NodeRef> {
        // SAFETY: The map holds a reference to the GC::WeakImpl, which keeps the address of its cell, or null once the
        //         cell has died, WEAK_IMPL_VALUE_OFFSET bytes in.
        let cell = unsafe { *self.0.as_ptr().byte_add(WEAK_IMPL_VALUE_OFFSET).cast::<*mut NodeCell>() };
        // SAFETY: The cell of a live GC::WeakImpl is a live element.
        NonNull::new(cell).map(|cell| unsafe { NodeRef::from_cell_unchecked(cell) })
    }
}

impl IdMapField {
    #[cfg(test)]
    pub(crate) const fn empty_for_test() -> Self {
        Self {
            map: Cell::new(core::ptr::null_mut()),
        }
    }

    /// The map, read for as long as the field is borrowed, during which nothing may call into C++, or none if no
    /// element with an ID has joined the tree.
    #[inline(always)]
    fn map(&self) -> Option<&IdMap> {
        // SAFETY: Only this module writes the map, never while it is borrowed.
        unsafe { self.map.get().as_ref() }
    }

    /// The map, which nothing else may borrow until the reference is dropped, allocating it if need be.
    #[allow(clippy::mut_from_ref)]
    fn map_mut(&self) -> &mut IdMap {
        if self.map.get().is_null() {
            self.map.set(Box::into_raw(Box::new(IdMap {
                entries: HashTable::new(),
            })));
        }
        // SAFETY: Only this module writes the map, and its callers borrow it once at a time.
        unsafe { &mut *self.map.get() }
    }
}

impl IdMap {
    #[inline(always)]
    fn find(&self, hash: u32, units: Utf16StringUnits<'_>) -> Option<&IdEntry> {
        self.entries.find(table_hash(hash), |entry| {
            units_equal(entry.id.as_units(), copy_units(&units))
        })
    }
}

#[inline(always)]
fn copy_units<'a>(units: &Utf16StringUnits<'a>) -> Utf16StringUnits<'a> {
    match units {
        Utf16StringUnits::Ascii(units) => Utf16StringUnits::Ascii(units),
        Utf16StringUnits::Latin1(units) => Utf16StringUnits::Latin1(units),
        Utf16StringUnits::Utf16(units) => Utf16StringUnits::Utf16(units),
    }
}

/// The map of the tree whose root is `root`, a document or a shadow root.
#[inline(always)]
fn id_map_of(root: &NodeRef) -> &IdMapField {
    match root.shadow_root_core() {
        Some(core) => core.ids(),
        None => root.document_core().ids(),
    }
}

/// Puts `element`, which is in the connected tree whose root is `root` and whose ID is `id`, in the tree's map. An
/// element the map already has under the ID stays where it is, but may no longer be first in tree order.
fn add(main_thread: &mut MainThread<'_>, root: NodeRef, element: NodeRef, id: &FlyStringField) {
    let field = id_map_of(&root);
    // SAFETY: The element's ID is a live string.
    let hash = unsafe { string_hash(id.raw_identity()) };
    if let Some(entry) = field.map().and_then(|map| map.find(hash, id.units()))
        && entry.elements.iter().any(|weak| weak.get() == Some(element))
    {
        // NB: A move can change which of several elements with the ID is first.
        if entry.elements.len() > 1 {
            entry.first.set(NO_FIRST);
        }
        return;
    }

    let weak = WeakElement(hooks::create_weak(main_thread, element));
    let map = id_map_of(&root).map_mut();
    let raw = id.raw_identity();
    let entry = match map
        .entries
        .find_mut(table_hash(hash), |entry| entry.id.raw_identity() == raw)
    {
        Some(entry) => entry,
        None => map
            .entries
            .insert_unique(
                table_hash(hash),
                IdEntry {
                    hash,
                    // SAFETY: The element's core holds the ID for the call.
                    id: unsafe { ak::Utf16FlyString::from_raw_borrowed(raw) },
                    elements: SmallVec::new(),
                    first: Cell::new(NO_FIRST),
                },
                |entry| table_hash(entry.hash),
            )
            .into_mut(),
    };
    entry.elements.push(weak);
    entry.first.set(if entry.elements.len() == 1 { 0 } else { NO_FIRST });
}

/// Takes `element` out of the map of the tree whose root was `root`, under the ID with code units `id` and AK hash
/// `hash`, along with any element under the ID that has died.
fn remove(main_thread: &mut MainThread<'_>, root: NodeRef, element: NodeRef, hash: u32, id: Utf16StringUnits<'_>) {
    // SAFETY: Only this module writes the map, and nothing else borrows it during the call.
    let Some(map) = (unsafe { id_map_of(&root).map.get().as_mut() }) else {
        return;
    };
    let Ok(mut occupied) = map.entries.find_entry(table_hash(hash), |entry| {
        units_equal(entry.id.as_units(), copy_units(&id))
    }) else {
        return;
    };
    let entry = occupied.get_mut();
    // NB: Releasing a GC::WeakImpl leaves the map as it is.
    entry.elements.retain(|weak| match weak.get() {
        Some(other) if other != element => true,
        _ => {
            hooks::release_weak(main_thread, weak.0);
            false
        }
    });
    if entry.elements.is_empty() {
        occupied.remove();
    } else {
        entry.first.set(if entry.elements.len() == 1 { 0 } else { NO_FIRST });
    }
}

/// The ID of `node`, if it is a connected element with an ID, under which it is to be in its tree's map.
#[inline(always)]
fn connected_id(node: &NodeRef) -> Option<&FlyStringField> {
    node.element_core()?.id().filter(|_| node.is_connected())
}

/// Puts `node`, which has just joined a connected tree, in the tree's map if it is an element with an ID.
///
/// NB: The node's connected flag is set by its insertion steps, which come after this.
#[inline(always)]
pub(super) fn element_connected(main_thread: &mut MainThread<'_>, node: NodeRef) {
    if let Some(id) = node.element_core().and_then(|core| core.id()) {
        add(main_thread, node.root(), node, id);
    }
}

/// Takes `node` out of the map it was in if it is an element with an ID, which has just left the connected tree whose
/// root is `old_root` with the subtree it is in. An element in a shadow tree of the subtree was in its shadow root's
/// map.
#[inline(always)]
pub(super) fn element_disconnected(main_thread: &mut MainThread<'_>, node: NodeRef, old_root: NodeRef) {
    let Some(id) = node.element_core().and_then(|core| core.id()) else {
        return;
    };
    let root = node.root();
    let scope_root = if root.is_shadow_root() { root } else { old_root };
    // SAFETY: The element's ID is a live string.
    let hash = unsafe { string_hash(id.raw_identity()) };
    remove(main_thread, scope_root, node, hash, id.units());
}

/// Moves `node` from the map of the tree whose root was `old_root` to the map of its tree, if it is an element with an
/// ID in a subtree that has just moved within a connected document. An element in a shadow tree of the subtree stays in
/// its shadow root's map, where it may no longer be first in tree order.
#[inline(always)]
pub(super) fn element_moved(main_thread: &mut MainThread<'_>, node: NodeRef, old_root: NodeRef, new_root: NodeRef) {
    let Some(id) = connected_id(&node) else {
        return;
    };
    let root = node.root();
    if root == new_root && old_root != new_root {
        // SAFETY: The element's ID is a live string.
        let hash = unsafe { string_hash(id.raw_identity()) };
        remove(main_thread, old_root, node, hash, id.units());
    }
    add(main_thread, root, node, id);
}

/// Moves `element`, which is connected, between the entries of its tree's map, as its ID goes from the string whose
/// raw word is `old_value` to the one whose raw word is `new_value`, either of which may be none for null.
pub(super) fn element_id_changing(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    old_value: Option<usize>,
    new_value: Option<usize>,
) {
    let root = element.root();
    let non_empty = |value: Option<usize>| {
        value.filter(|&value| {
            // SAFETY: The caller borrows the values for the call.
            let string = unsafe { BorrowedString::from_raw(value) };
            !super::strings::units_are_empty(string.units())
        })
    };
    if let Some(old_value) = non_empty(old_value) {
        // SAFETY: The caller borrows the values for the call.
        let string = unsafe { BorrowedString::from_raw(old_value) };
        // SAFETY: The caller borrows the values for the call.
        let hash = unsafe { string_hash(old_value) };
        remove(main_thread, root, element, hash, string.units());
    }
    if let Some(new_value) = non_empty(new_value) {
        // SAFETY: The caller borrows the values for the call.
        let id = super::attributes::fly_string_of(unsafe { BorrowedString::from_raw(new_value) });
        // SAFETY: An AK::Utf16FlyString has the layout of a FlyStringField, and `id` lives for the call.
        let field = unsafe { &*core::ptr::from_ref(&id).cast::<FlyStringField>() };
        add(main_thread, root, element, field);
    }
}

/// Whether `node` is an element whose ID has the code units `id`.
#[inline(always)]
fn has_id(node: NodeRef, id: &Utf16StringUnits<'_>) -> bool {
    node.element_core()
        .and_then(|core| core.id())
        .is_some_and(|element_id| units_equal(element_id.units(), copy_units(id)))
}

/// The first element in tree order within the inclusive descendants of `node` whose ID has the code units `id`, found
/// by walking them.
fn first_inclusive_descendant_with_id(node: NodeRef, id: &Utf16StringUnits<'_>) -> Option<NodeRef> {
    let mut current = Some(node);
    while let Some(inclusive_descendant) = current {
        if has_id(inclusive_descendant, id) {
            return Some(inclusive_descendant);
        }
        current = inclusive_descendant.next_in_subtree(node);
    }
    None
}

/// https://dom.spec.whatwg.org/#concept-getelementbyid
///
/// NB: This looks among node's inclusive descendants, which only differs from its descendants for an element, which C++
///     also asks about.
fn get_an_element_by_id(node: NodeRef, element_id: Utf16StringUnits<'_>) -> Option<NodeRef> {
    // To get an element by ID, given a node node and a string elementId, return the first element, in tree order,
    // within node's descendants, whose ID is elementId; otherwise, if there is no such element, null.
    // OPTIMIZATION: A connected document or shadow root looks the ID up in its map, and so does the document element,
    //               whose inclusive descendants are the document's elements.
    let is_scope_root = node.is_connected()
        && match node.node_type() {
            NodeType::DOCUMENT_NODE => true,
            NodeType::DOCUMENT_FRAGMENT_NODE => node.is_shadow_root(),
            NodeType::ELEMENT_NODE => node
                .parent()
                .is_some_and(|parent| parent.node_type() == NodeType::DOCUMENT_NODE),
            _ => false,
        };
    if !is_scope_root {
        return first_inclusive_descendant_with_id(node, &element_id);
    }
    let root = node.root();
    let entry = id_map_of(&root)
        .map()?
        .find(units_hash(&element_id), copy_units(&element_id))?;
    let first = entry.first.get();
    if first != NO_FIRST {
        return entry.elements[first as usize].get();
    }
    let mut live = entry
        .elements
        .iter()
        .enumerate()
        .filter_map(|(index, weak)| weak.get().map(|element| (index, element)));
    let only = live.next()?;
    let (index, element) = if live.next().is_none() {
        only
    } else {
        // NB: The first in tree order of several elements with the ID is found by walking the tree up to it, which
        //     comparing their tree positions pairwise can take quadratic time for, as for siblings.
        let element = first_inclusive_descendant_with_id(root, &element_id).expect("the map holds connected elements");
        let index = entry
            .elements
            .iter()
            .position(|weak| weak.get() == Some(element))
            .expect("the map holds every connected element with the ID");
        (index, element)
    };
    entry.first.set(index as u32);
    Some(element)
}

/// Gets an element by ID given `node` and the ID whose code units C++ passes as a view of `length` Latin-1 code units
/// at `latin1`, where it is not null, or UTF-16 code units at `utf16`, for DOM::ParentNode::get_element_by_id().
///
/// # Safety
///
/// The node must be live, the code units live for the call, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_get_element_by_id(
    node: &NodeCell,
    latin1: *const u8,
    utf16: *const u16,
    length: usize,
) -> *const NodeCell {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // SAFETY: The code units live for the call.
    let element_id = unsafe { units_of_view(latin1, utf16, length) };
    get_an_element_by_id(node, element_id).map_or(core::ptr::null(), |element| element.cell() as *const NodeCell)
}

/// Appends the elements of the connected tree whose root is `root` whose ID has the code units C++ passes as a view of
/// `length` Latin-1 code units at `latin1`, where it is not null, or UTF-16 code units at `utf16`, to `result`: in tree
/// order if `in_tree_order` is true, and otherwise in no particular order.
///
/// # Safety
///
/// The root must be a live document or shadow root, the code units live for the call, `result` the core of a
/// DOM::RootedNodeList that lives for the call, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_elements_with_id(
    root: &NodeCell,
    latin1: *const u8,
    utf16: *const u16,
    length: usize,
    in_tree_order: bool,
    result: &RootedNodeListCore,
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The root is live.
    let root = unsafe { NodeRef::from_cell(root, &main_thread) };
    // SAFETY: The list lives for the call, and nothing else uses it.
    let mut result = unsafe { RootedNodes::from_core(result) };
    // SAFETY: The code units live for the call.
    let units = unsafe { units_of_view(latin1, utf16, length) };
    let Some(entry) = id_map_of(&root)
        .map()
        .and_then(|map| map.find(units_hash(&units), copy_units(&units)))
    else {
        return;
    };
    let count = entry.elements.iter().filter(|weak| weak.get().is_some()).count();
    if in_tree_order && count > 1 {
        // NB: Several elements with the ID are found in tree order by walking the tree, up to the last of them, which
        //     sorting them by comparing their tree positions can take quadratic time for, as for siblings.
        let mut remaining = count;
        let mut current = Some(root);
        while let Some(node) = current
            && remaining > 0
        {
            if has_id(node, &units) {
                result.push(node);
                remaining -= 1;
            }
            current = node.next_in_subtree(root);
        }
        return;
    }
    for weak in &entry.elements {
        if let Some(element) = weak.get() {
            result.push(element);
        }
    }
}

/// An element of the connected tree whose root is `root` whose ID is the string whose raw word is `id`, or null if there
/// is none, and in `count`, how many such elements there are.
///
/// # Safety
///
/// The root must be a live document or shadow root, the string live for the call, and the call must be made on the
/// main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_element_with_id(root: &NodeCell, id: usize, count: &mut usize) -> *const NodeCell {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The root is live.
    let root = unsafe { NodeRef::from_cell(root, &main_thread) };
    // SAFETY: The string lives for the call.
    let units = unsafe { ak::utf16_string_units(&id) };
    *count = 0;
    // SAFETY: The string lives for the call.
    let hash = unsafe { string_hash(id) };
    let Some(entry) = id_map_of(&root).map().and_then(|map| map.find(hash, units)) else {
        return core::ptr::null();
    };
    let mut element = None;
    for weak in &entry.elements {
        if let Some(live) = weak.get() {
            element.get_or_insert(live);
            *count += 1;
        }
    }
    element.map_or(core::ptr::null(), |element| element.cell() as *const NodeCell)
}

/// Calls `callback` with `context` and the raw word of each ID of an element of the connected tree whose root is
/// `root`, in no particular order.
///
/// # Safety
///
/// The root must be a live document or shadow root, `callback` must not change the tree or the IDs of its elements, and
/// the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_for_each_id(
    root: &NodeCell,
    context: *mut c_void,
    callback: unsafe extern "C" fn(context: *mut c_void, id: usize),
) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The root is live.
    let root = unsafe { NodeRef::from_cell(root, &main_thread) };
    let Some(map) = id_map_of(&root).map() else {
        return;
    };
    for entry in &map.entries {
        // SAFETY: Guaranteed by the caller.
        unsafe { callback(context, entry.id.raw_identity()) };
    }
}

/// Whether an element of the connected tree whose root is `root` has the ID whose raw word is `id`.
///
/// # Safety
///
/// The root must be a live document or shadow root, the string live for the call, and the call must be made on the
/// main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_has_element_with_id(root: &NodeCell, id: usize) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The root is live.
    let root = unsafe { NodeRef::from_cell(root, &main_thread) };
    // SAFETY: The string lives for the call.
    let units = unsafe { ak::utf16_string_units(&id) };
    // SAFETY: The string lives for the call.
    let hash = unsafe { string_hash(id) };
    id_map_of(&root)
        .map()
        .is_some_and(|map| map.find(hash, units).is_some())
}

/// Frees the map of a document or shadow root, and releases the elements in it, as C++ destroys the tree scope.
///
/// # Safety
///
/// The field must be that of a tree scope that is being destroyed, nothing may read it after, and the call must be made
/// on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_id_map_destroy(field: &IdMapField) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    let map = field.map.replace(core::ptr::null_mut());
    if map.is_null() {
        return;
    }
    // SAFETY: The map was allocated as a Box, and nothing else holds it.
    let map = unsafe { Box::from_raw(map) };
    for entry in map.entries {
        for weak in entry.elements {
            hooks::release_weak(&mut main_thread, weak.0);
        }
    }
}
