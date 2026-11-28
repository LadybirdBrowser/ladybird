/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slots of shadow trees and the slottables assigned to them.
//! https://dom.spec.whatwg.org/#shadow-tree-slots
//!
//! A slot keeps its name and its two lists of nodes in a [`SlotCore`], which HTML::HTMLSlotElement embeds, and a shadow
//! root keeps its slot assignment and the slots of its tree in a [`ShadowRootCore`], which DOM::ShadowRoot embeds. The
//! lists are [`NodeListField`]s: buffers Rust allocates, grows and frees, which hold their nodes strongly. The cells that
//! embed them visit every node of them in place, as DOM::Event does the path in its core, so the nodes stay alive for
//! as long as a list names them, with no rooting and no weak references to clear.
//!
//! A slottable's name is the value of its slot attribute, or the empty string where it has none, which is what the
//! attribute change steps that keep it set it to. So it is read from the element's attribute list rather than kept.
//!
//! AD-HOC: Assigned nodes are recomputed lazily. Assigning a slot for a slottable, or unassigning it, sets its assigned
//!         slot at once, signals the slot change and marks the slot as needing assignment, and its assigned nodes are
//!         brought up to date the next time they are read, with SLOT_NEEDS_ASSIGNMENT telling which slots that is for.
//!         Every slottable child of a named shadow root's host is then assigned to the slot that finding a slot finds
//!         for it, so a slot's assigned nodes are the slottable children of the host assigned to it, which is how they
//!         are recomputed.
//!
//! What the slots' assignments mean for the systems whose state C++ keeps, the style engine, layout, and the
//! signal slots of the similar-origin window agent, is done through SlotHooks, whose callbacks fire only where an
//! assignment changes.

use core::alloc::Layout;
use core::cell::Cell;
use core::mem::offset_of;

use ak::Utf16StringUnits;

use super::attributes::fly_string_of;
use super::element_core::ELEMENT_IS_HTML_ELEMENT;
use super::hooks;
use super::hooks::SLOT_CHANGE_INVALIDATE;
use super::hooks::SLOT_CHANGE_RECORD_ASSIGNED_NODES;
use super::hooks::SLOT_CHANGE_SIGNAL;
use super::node::NodeCell;
use super::node_core::NodeMember;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::rooted_nodes::RootedNodeListCore;
use super::rooted_nodes::RootedNodes;
use super::strings::BorrowedString;
use super::strings::OptionalFlyStringField;
use super::strings::raw_strings_equal;
use super::strings::units_are_empty;
use super::strings::units_equal;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// A list of nodes that the cell which embeds it holds strongly, laid out as C++ reads it: the nodes as GC::Member<Node>s
/// in a buffer Rust allocates, as many as the size, with room for the capacity. Only Rust writes it. C++ visits every
/// node of it as it visits the edges of the cell, and has Rust free the buffer as it destroys the cell.
#[repr(C)]
pub struct NodeListField {
    items: Cell<*mut NodeMember>,
    size: Cell<u32>,
    capacity: Cell<u32>,
}

const _: () = assert!(size_of::<NodeListField>() == 16 && align_of::<NodeListField>() == 8);
const _: () = assert!(offset_of!(NodeListField, items) == 0);
const _: () = assert!(offset_of!(NodeListField, size) == 8);
const _: () = assert!(offset_of!(NodeListField, capacity) == 12);

impl NodeListField {
    #[cfg(test)]
    pub(crate) const fn new_for_test() -> Self {
        Self {
            items: Cell::new(core::ptr::null_mut()),
            size: Cell::new(0),
            capacity: Cell::new(0),
        }
    }

    #[inline(always)]
    pub fn len(&self) -> usize {
        self.size.get() as usize
    }

    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        self.size.get() == 0
    }

    /// The compressed pointers of the nodes, read for as long as the list is borrowed, during which nothing may call
    /// into C++.
    #[inline(always)]
    fn pointers(&self) -> &[NodeMember] {
        // SAFETY: The buffer holds `size` nodes, which only Rust writes, or is null where the list never had any.
        unsafe { ak::slice_from_raw_parts(self.items.get(), self.len()) }
    }

    /// The node at `index`.
    #[inline(always)]
    pub(super) fn get(&self, index: usize) -> NodeRef {
        // SAFETY: The cell that embeds the list keeps the nodes it names alive.
        unsafe { NodeRef::from_member(&self.pointers()[index]) }.expect("a list of nodes holds no null")
    }

    pub(super) fn position(&self, node: NodeRef) -> Option<usize> {
        let pointer = Some(node.pointer());
        self.pointers().iter().position(|member| member.get() == pointer)
    }

    #[inline(always)]
    pub(super) fn contains(&self, node: NodeRef) -> bool {
        self.position(node).is_some()
    }

    /// Whether the list holds the nodes `pointers` names, in that order.
    pub(super) fn is_identical_to(&self, pointers: &[NodePointer]) -> bool {
        let members = self.pointers();
        members.len() == pointers.len()
            && members
                .iter()
                .zip(pointers)
                .all(|(member, &pointer)| member.get() == Some(pointer))
    }

    pub(super) fn push(&self, node: NodeRef) {
        self.insert(self.len(), node);
    }

    pub(super) fn insert(&self, index: usize, node: NodeRef) {
        let size = self.len();
        assert!(index <= size);
        self.reserve(size + 1);
        // SAFETY: The buffer has room for one more node past the `size` it holds, which move up by one from `index`.
        unsafe {
            let items = self.items.get();
            core::ptr::copy(items.add(index), items.add(index + 1), size - index);
            items.add(index).write(NodeMember::null());
            (*items.add(index)).set(Some(node.pointer()));
        }
        self.size.set(self.size.get() + 1);
    }

    pub(super) fn remove(&self, index: usize) {
        let size = self.len();
        assert!(index < size);
        // SAFETY: The buffer holds `size` nodes, which move down by one onto `index`.
        unsafe {
            let items = self.items.get();
            core::ptr::copy(items.add(index + 1), items.add(index), size - index - 1);
        }
        self.size.set(self.size.get() - 1);
    }

    /// Makes the list hold the nodes `pointers` names, in that order.
    pub(super) fn set_to(&self, pointers: &[NodePointer]) {
        self.size.set(0);
        self.reserve(pointers.len());
        // SAFETY: The buffer has room for every node.
        unsafe {
            let items = self.items.get();
            for (index, &pointer) in pointers.iter().enumerate() {
                items.add(index).write(NodeMember::null());
                (*items.add(index)).set(Some(pointer));
            }
        }
        self.size.set(pointers.len() as u32);
    }

    fn reserve(&self, needed: usize) {
        let capacity = self.capacity.get() as usize;
        if needed <= capacity {
            return;
        }
        let new_capacity = needed.max(capacity * 2).max(4);
        let new_capacity_u32 = u32::try_from(new_capacity).expect("a list of nodes is never that long");
        let new_layout = Layout::array::<NodeMember>(new_capacity).expect("a list of nodes is never that long");
        // SAFETY: The buffer was allocated with the layout of `capacity` nodes, or is null where `capacity` is 0, and
        //         neither layout is zero-sized.
        let items = unsafe {
            match capacity {
                0 => std::alloc::alloc(new_layout),
                _ => std::alloc::realloc(
                    self.items.get().cast(),
                    Layout::array::<NodeMember>(capacity).unwrap(),
                    new_layout.size(),
                ),
            }
        };
        if items.is_null() {
            std::alloc::handle_alloc_error(new_layout);
        }
        self.items.set(items.cast());
        self.capacity.set(new_capacity_u32);
    }

    /// Frees the buffer, which leaves the list empty.
    fn free(&self) {
        let capacity = self.capacity.get() as usize;
        if capacity != 0 {
            // SAFETY: The buffer was allocated with the layout of `capacity` nodes.
            unsafe { std::alloc::dealloc(self.items.get().cast(), Layout::array::<NodeMember>(capacity).unwrap()) };
        }
        self.items.set(core::ptr::null_mut());
        self.size.set(0);
        self.capacity.set(0);
    }
}

/// Where HTML::HTMLSlotElement embeds its core, in bytes from the start of the slot's cell, which HTMLSlotElement.cpp
/// asserts.
pub const SLOT_CORE_OFFSET: usize = 320;

/// Whether the slot's assigned nodes are to be recomputed before they are read, see the module documentation.
pub const SLOT_NEEDS_ASSIGNMENT: u8 = 1 << 0;

/// The state of a slot, which C++ embeds in HTML::HTMLSlotElement as its only storage of it.
/// https://dom.spec.whatwg.org/#concept-slot
#[repr(C)]
pub struct SlotCore {
    /// https://dom.spec.whatwg.org/#slot-name, with none for the empty string.
    name: OptionalFlyStringField,
    /// https://dom.spec.whatwg.org/#slot-assigned-nodes
    assigned_nodes: NodeListField,
    /// https://html.spec.whatwg.org/multipage/scripting.html#manually-assigned-nodes
    manually_assigned_nodes: NodeListField,
    /// The SLOT_* flags.
    flags: Cell<u8>,
}

const _: () = assert!(size_of::<SlotCore>() == 48 && align_of::<SlotCore>() == 8);
const _: () = assert!(offset_of!(SlotCore, name) == 0);
const _: () = assert!(offset_of!(SlotCore, assigned_nodes) == 8);
const _: () = assert!(offset_of!(SlotCore, manually_assigned_nodes) == 24);
const _: () = assert!(offset_of!(SlotCore, flags) == 40);

impl SlotCore {
    #[inline(always)]
    pub fn name(&self) -> Utf16StringUnits<'_> {
        match self.name.get() {
            Some(name) => name.units(),
            None => Utf16StringUnits::Ascii(b""),
        }
    }

    #[inline(always)]
    pub(super) fn assigned_nodes(&self) -> &NodeListField {
        &self.assigned_nodes
    }

    #[inline(always)]
    pub(super) fn manually_assigned_nodes(&self) -> &NodeListField {
        &self.manually_assigned_nodes
    }
}

/// Where DOM::ShadowRoot embeds its core, in bytes from the start of the shadow root's cell, which ShadowRoot.cpp
/// asserts.
pub const SHADOW_ROOT_CORE_OFFSET: usize = 120;

/// Whether a slot of the shadow tree has SLOT_NEEDS_ASSIGNMENT.
pub const SHADOW_ROOT_HAS_SLOTS_NEEDING_ASSIGNMENT: u8 = 1 << 0;

/// The values of a shadow root's slot assignment, as C++'s Bindings::SlotAssignmentMode has them.
/// https://dom.spec.whatwg.org/#shadowroot-slot-assignment
pub const SLOT_ASSIGNMENT_MANUAL: u8 = 0;
pub const SLOT_ASSIGNMENT_NAMED: u8 = 1;

/// The state of a shadow root beyond that of every DocumentFragment, which C++ embeds in DOM::ShadowRoot as its only
/// storage of it.
/// https://dom.spec.whatwg.org/#concept-shadow-root
#[repr(C)]
pub struct ShadowRootCore {
    /// The slots of the shadow tree, in tree order. While it has none, the tree has no slot, so it doubles as the
    /// shadow root's summary of whether it has any.
    slots: NodeListField,
    /// The slots of the shadow tree that need assignment, once each, in the order they came to, which the next flush
    /// brings up to date.
    slots_needing_assignment: NodeListField,
    /// https://dom.spec.whatwg.org/#shadowroot-slot-assignment, as SLOT_ASSIGNMENT_*.
    slot_assignment: Cell<u8>,
    /// The SHADOW_ROOT_* flags.
    flags: Cell<u8>,
}

const _: () = assert!(size_of::<ShadowRootCore>() == 40 && align_of::<ShadowRootCore>() == 8);
const _: () = assert!(offset_of!(ShadowRootCore, slots) == 0);
const _: () = assert!(offset_of!(ShadowRootCore, slots_needing_assignment) == 16);
const _: () = assert!(offset_of!(ShadowRootCore, slot_assignment) == 32);
const _: () = assert!(offset_of!(ShadowRootCore, flags) == 33);

impl ShadowRootCore {
    #[inline(always)]
    pub(super) fn slots(&self) -> &NodeListField {
        &self.slots
    }

    #[inline(always)]
    fn is_manual(&self) -> bool {
        self.slot_assignment.get() == SLOT_ASSIGNMENT_MANUAL
    }
}

/// Whether `node` is a slot: an HTML slot element.
#[inline(always)]
pub(super) fn is_slot(node: NodeRef) -> bool {
    node.element_core().is_some_and(|core| {
        core.flags() & ELEMENT_IS_HTML_ELEMENT != 0
            && matches!(
                core.qualified_name().local_name().units(),
                Utf16StringUnits::Ascii(b"slot")
            )
    })
}

/// https://dom.spec.whatwg.org/#concept-slotable
#[inline(always)]
fn is_slottable(node: NodeRef) -> bool {
    matches!(
        node.node_type(),
        NodeType::ELEMENT_NODE | NodeType::TEXT_NODE | NodeType::CDATA_SECTION_NODE
    )
}

fn slot_core(slot: &NodeRef) -> &SlotCore {
    slot.slot_core().expect("a slot has a slot core")
}

fn shadow_root_core(shadow_root: &NodeRef) -> &ShadowRootCore {
    shadow_root
        .shadow_root_core()
        .expect("a shadow root has a shadow root core")
}

/// https://dom.spec.whatwg.org/#slotable-name, which is the value of an element's slot attribute, or the empty string
/// where it has none, see the module documentation.
fn slottable_name(slottable: &NodeRef) -> Utf16StringUnits<'_> {
    if let Some(core) = slottable.element_core() {
        for attribute in core.attributes() {
            let name = attribute.name();
            if name.namespace().is_none() && matches!(name.local_name().units(), Utf16StringUnits::Ascii(b"slot")) {
                return attribute.value().units();
            }
        }
    }
    Utf16StringUnits::Ascii(b"")
}

/// https://dom.spec.whatwg.org/#find-a-slot
pub(super) fn find_a_slot(slottable: NodeRef, open: bool) -> Option<NodeRef> {
    // To find a slot, given a slottable slottable and an optional boolean open (default false):

    // 1. If slottable's parent is null, then return null.
    let parent = slottable.parent()?;

    // 2. Let shadow be slottable's parent's shadow root.
    // 3. If shadow is null, then return null.
    let shadow = parent.shadow_root()?;

    // 4. If open is true and shadow's mode is not "open", then return null.
    if open && shadow.is_closed_shadow_root() {
        return None;
    }

    // NB: The slots of the shadow tree are the slots in shadow's descendants, in tree order.
    let core = shadow_root_core(&shadow);
    let slots = core.slots();

    // 5. If shadow's slot assignment is "manual", then return the slot in shadow's descendants whose manually assigned
    //    nodes contains slottable, if any; otherwise null.
    if core.is_manual() {
        return (0..slots.len())
            .map(|index| slots.get(index))
            .find(|slot| slot_core(slot).manually_assigned_nodes().contains(slottable));
    }

    // 6. Return the first slot in tree order in shadow's descendants whose name is slottable's name, if any; otherwise
    //    null.
    let name = slottable_name(&slottable);
    (0..slots.len()).map(|index| slots.get(index)).find(|slot| {
        let name = match &name {
            Utf16StringUnits::Ascii(units) => Utf16StringUnits::Ascii(units),
            Utf16StringUnits::Latin1(units) => Utf16StringUnits::Latin1(units),
            Utf16StringUnits::Utf16(units) => Utf16StringUnits::Utf16(units),
        };
        units_equal(slot_core(slot).name(), name)
    })
}

/// https://dom.spec.whatwg.org/#find-slotables
///
/// Hands each node of the result to `append`, in order, rather than making a list of them.
fn find_slottables(slot: NodeRef, mut append: impl FnMut(NodeRef)) {
    // To find slottables for a given slot slot:

    // 1. Let result be « ».

    // 2. Let root be slot's root.
    let root = slot.root();

    // 3. If root is not a shadow root, then return result.
    if !root.is_shadow_root() {
        return;
    }

    // 4. Let host be root's host.
    let host = root.host().expect("a shadow root has a host");

    // 5. If root's slot assignment is "manual":
    if shadow_root_core(&root).is_manual() {
        // 1. For each slottable slottable of slot's manually assigned nodes, if slottable's parent is host, append
        //    slottable to result.
        let manually_assigned_nodes = slot_core(&slot).manually_assigned_nodes();
        for index in 0..manually_assigned_nodes.len() {
            let slottable = manually_assigned_nodes.get(index);
            if slottable.parent() == Some(host) {
                append(slottable);
            }
        }
    }
    // 6. Otherwise, for each slottable child slottable of host, in tree order:
    else {
        let mut child = host.first_child();
        while let Some(slottable) = child {
            child = slottable.next_sibling();
            if !is_slottable(slottable) {
                continue;
            }

            // 1. Let foundSlot be the result of finding a slot given slottable.
            let found_slot = find_a_slot(slottable, false);

            // 2. If foundSlot is slot, then append slottable to result.
            if found_slot == Some(slot) {
                append(slottable);
            }
        }
    }

    // 7. Return result.
}

/// https://dom.spec.whatwg.org/#find-flattened-slotables
fn find_flattened_slottables(slot: NodeRef, result: &mut RootedNodes<'_>) {
    // To find flattened slottables for a given slot slot:

    // 1. Let result be « ».

    // 2. If slot's root is not a shadow root, then return result.
    if !slot.root().is_shadow_root() {
        return;
    }

    // 5. For each node of slottables:
    let mut for_each_node = |node: NodeRef| {
        // 1. If node is a slot whose root is a shadow root:
        if is_slot(node) && node.root().is_shadow_root() {
            // 1. Let temporaryResult be the result of finding flattened slottables given node.
            // 2. Append each slottable in temporaryResult, in order, to result.
            find_flattened_slottables(node, result);
        }
        // 2. Otherwise, append node to result.
        else {
            result.push(node);
        }
    };

    // 3. Let slottables be the result of finding slottables given slot.
    let mut slottables_is_empty = true;
    find_slottables(slot, |node| {
        slottables_is_empty = false;
        for_each_node(node);
    });

    // 4. If slottables is the empty list, then append each slottable child of slot, in tree order, to slottables.
    if slottables_is_empty {
        let mut child = slot.first_child();
        while let Some(node) = child {
            child = node.next_sibling();
            if is_slottable(node) {
                for_each_node(node);
            }
        }
    }

    // 6. Return result.
}

/// AD-HOC: Brings the assigned nodes of `slot`, a slot of the shadow tree of `shadow_root`, up to date if it needs
///         assignment, see the module documentation, and publishes them to the style engine.
fn settle_slot(main_thread: &mut MainThread<'_>, slot: NodeRef, shadow_root: NodeRef) {
    let core = slot_core(&slot);
    if core.flags.get() & SLOT_NEEDS_ASSIGNMENT == 0 {
        return;
    }
    core.flags.set(core.flags.get() & !SLOT_NEEDS_ASSIGNMENT);

    // NB: The slottable children of the host assigned to the slot, which the tree keeps alive.
    let host = shadow_root.host().expect("a shadow root has a host");
    let mut assigned_nodes = Vec::new();
    let mut child = host.first_child();
    while let Some(node) = child {
        child = node.next_sibling();
        if is_slottable(node) && node.assigned_slot() == Some(slot) {
            assigned_nodes.push(node.pointer());
        }
    }
    if !core.assigned_nodes().is_identical_to(&assigned_nodes) {
        core.assigned_nodes().set_to(&assigned_nodes);
    }
    if slot.core().style_node_id().is_some() {
        hooks::slot_changed(main_thread, slot, SLOT_CHANGE_RECORD_ASSIGNED_NODES);
    }
}

/// AD-HOC: Brings the assigned nodes of every slot of the shadow tree of `shadow_root` that needs assignment up to
///         date, see the module documentation.
fn assign_slottables_if_needed(main_thread: &mut MainThread<'_>, shadow_root: NodeRef) {
    let core = shadow_root_core(&shadow_root);
    if core.flags.get() & SHADOW_ROOT_HAS_SLOTS_NEEDING_ASSIGNMENT == 0 {
        return;
    }
    core.flags
        .set(core.flags.get() & !SHADOW_ROOT_HAS_SLOTS_NEEDING_ASSIGNMENT);
    // NB: The list holds its slots strongly while they are settled, and is emptied once all are.
    let mut index = 0;
    while index < shadow_root_core(&shadow_root).slots_needing_assignment.len() {
        let slot = shadow_root_core(&shadow_root).slots_needing_assignment.get(index);
        // NB: Removing a slot from the shadow tree settles the slots of the tree, while the slot is still among them.
        settle_slot(main_thread, slot, shadow_root);
        index += 1;
    }
    shadow_root_core(&shadow_root).slots_needing_assignment.set_to(&[]);
}

/// AD-HOC: Has the assigned nodes of `slot`, a slot of the shadow tree of `shadow_root`, recomputed before they are
///         next read, see the module documentation.
fn set_needs_assignment(main_thread: &mut MainThread<'_>, slot: NodeRef, shadow_root: NodeRef) {
    let core = slot_core(&slot);
    core.flags.set(core.flags.get() | SLOT_NEEDS_ASSIGNMENT);
    let core = shadow_root_core(&shadow_root);
    if !core.slots_needing_assignment.contains(slot) {
        core.slots_needing_assignment.push(slot);
    }
    if core.flags.get() & SHADOW_ROOT_HAS_SLOTS_NEEDING_ASSIGNMENT == 0 {
        core.flags
            .set(core.flags.get() | SHADOW_ROOT_HAS_SLOTS_NEEDING_ASSIGNMENT);
        hooks::shadow_root_needs_slot_assignment(main_thread, shadow_root);
    }
}

/// https://dom.spec.whatwg.org/#assign-slotables
pub(super) fn assign_slottables(main_thread: &mut MainThread<'_>, slot: NodeRef) {
    // AD-HOC: Assigned nodes are recomputed lazily, so the slot's are brought up to date first.
    let root = slot.root();
    if root.is_shadow_root() {
        settle_slot(main_thread, slot, root);
    }

    // To assign slottables for a slot slot:

    // 1. Let slottables be the result of finding slottables for slot.
    // NB: The slottables are children of the host, which the tree keeps alive, and the callbacks below change no tree.
    //     So the list holds their compressed pointers on the heap, where the collector does not look for them.
    let mut slottables = Vec::new();
    find_slottables(slot, |slottable| slottables.push(slottable.pointer()));

    // 2. If slottables and slot's assigned nodes are not identical, then run signal a slot change for slot.
    let assignment_changed = !slot_core(&slot).assigned_nodes().is_identical_to(&slottables);
    if assignment_changed {
        // AD-HOC: The slottables that are no longer assigned to slot have their assigned slot cleared, before the new
        //         ones have theirs set, to avoid stale references during style computation. Assigning slottables for a
        //         tree can already have moved one of them to another slot, which slot must not overwrite.
        // NB: A slottable is among the slottables exactly when finding a slot given it finds slot and its parent is
        //     the host, so that is asked of each rather than searching the list.
        let mut index = 0;
        while index < slot_core(&slot).assigned_nodes().len() {
            let old_slottable = slot_core(&slot).assigned_nodes().get(index);
            index += 1;
            if old_slottable.assigned_slot() != Some(slot) {
                continue;
            }
            let still_assigned = root.is_shadow_root()
                && old_slottable.parent() == root.host()
                && find_a_slot(old_slottable, false) == Some(slot);
            if !still_assigned {
                hooks::assigned_slot_changed(main_thread, old_slottable, Some(slot), None);
            }
        }

        // 4. For each slottable of slottables: set slottable's assigned slot to slot.
        for &pointer in &slottables {
            // SAFETY: The slottable is a child of the host, which the tree keeps alive.
            let slottable = unsafe { NodeRef::from_pointer(pointer) };
            let old_slot = slottable.assigned_slot();
            if old_slot != Some(slot) {
                hooks::assigned_slot_changed(main_thread, slottable, old_slot, Some(slot));
            }
        }

        // 3. Set slot's assigned nodes to slottables.
        // NB: This comes last, after the old assigned nodes were read above.
        slot_core(&slot).assigned_nodes().set_to(&slottables);
    }

    // AD-HOC: The whole ordered list is published to the style engine, which only the list tells in what order the
    //         slot projects its slottables, and which Text nodes are among them. A changed assignment also invalidates
    //         the slot's layout and style.
    let mut steps = 0;
    if assignment_changed {
        steps |= SLOT_CHANGE_SIGNAL | SLOT_CHANGE_INVALIDATE;
    }
    if slot.core().style_node_id().is_some() {
        steps |= SLOT_CHANGE_RECORD_ASSIGNED_NODES;
    }
    if steps != 0 {
        hooks::slot_changed(main_thread, slot, steps);
    }
}

/// https://dom.spec.whatwg.org/#assign-slotables-for-a-tree
#[inline(always)]
pub(super) fn assign_slottables_for_a_tree(main_thread: &mut MainThread<'_>, root: NodeRef) {
    // OPTIMIZATION: A slot whose root is not a shadow root finds no slottables, and has no assigned nodes either, as
    //               removing a node assigns slottables for each slot it takes out of a shadow tree. So assigning
    //               slottables for a tree that is no shadow tree changes nothing.
    if root.is_shadow_root() {
        assign_slottables_for_a_shadow_tree(main_thread, root);
    }
}

/// https://dom.spec.whatwg.org/#assign-slotables-for-a-tree, for `root`, a shadow root.
#[inline(never)]
fn assign_slottables_for_a_shadow_tree(main_thread: &mut MainThread<'_>, root: NodeRef) {
    // To assign slottables for a tree, given a node root, run assign slottables for each slot of root's inclusive
    // descendants, in tree order.
    // NB: The slots of a shadow tree are its shadow root's slots.
    let mut index = 0;
    while index < shadow_root_core(&root).slots().len() {
        let slot = shadow_root_core(&root).slots().get(index);
        assign_slottables(main_thread, slot);
        index += 1;
    }
}

/// Calls `callback` with each slot of `root`'s inclusive descendants, in tree order, as the callbacks it calls change no
/// tree.
fn for_each_slot_of_inclusive_descendants(
    main_thread: &mut MainThread<'_>,
    root: NodeRef,
    mut callback: impl FnMut(&mut MainThread<'_>, NodeRef),
) {
    let mut current = Some(root);
    while let Some(node) = current {
        if is_slot(node) {
            callback(main_thread, node);
        }
        current = next_in_subtree(node, root);
    }
}

/// The node after `node` in tree order that is a descendant of `root`, an inclusive ancestor of `node`.
fn next_in_subtree(node: NodeRef, root: NodeRef) -> Option<NodeRef> {
    if let Some(first_child) = node.first_child() {
        return Some(first_child);
    }
    let mut current = node;
    while current != root {
        if let Some(next_sibling) = current.next_sibling() {
            return Some(next_sibling);
        }
        current = current.parent()?;
    }
    None
}

/// https://dom.spec.whatwg.org/#assign-a-slot
fn assign_a_slot(main_thread: &mut MainThread<'_>, slottable: NodeRef) {
    // To assign a slot, given a slottable slottable:

    // 1. Let slot be the result of finding a slot with slottable.
    let slot = find_a_slot(slottable, false);

    // 2. If slot is non-null, then run assign slottables for slot.
    let Some(slot) = slot else {
        return;
    };
    // AD-HOC: Assigned nodes are recomputed lazily.
    let old_slot = slottable.assigned_slot();
    change_assigned_slot(main_thread, slottable, slot, old_slot, Some(slot));
}

/// AD-HOC: Runs assign slottables for the assigned slot of `node`, if it is assigned, with assigned nodes recomputed
///         lazily.
#[inline(always)]
fn unassign_a_slot(main_thread: &mut MainThread<'_>, node: NodeRef) {
    let Some(slot) = node.assigned_slot() else {
        return;
    };
    change_assigned_slot(main_thread, node, slot, Some(slot), None);
}

/// AD-HOC: What running assign slottables for `changed_slot` does when the only slottable whose assignment it changes
///         is `slottable`, whose assigned slot goes from `old_slot` to `new_slot`: in a named shadow tree, sets the
///         assigned slot at once, signals the slot change and has the assigned nodes recomputed when next read.
fn change_assigned_slot(
    main_thread: &mut MainThread<'_>,
    slottable: NodeRef,
    changed_slot: NodeRef,
    old_slot: Option<NodeRef>,
    new_slot: Option<NodeRef>,
) {
    // NB: Other children of the host can join a manual slot when slottable joins or leaves it.
    let shadow_root = changed_slot.root();
    if shadow_root_core(&shadow_root).is_manual() {
        assign_slottables(main_thread, changed_slot);
        return;
    }
    if old_slot == new_slot {
        return;
    }
    hooks::assigned_slot_changed(main_thread, slottable, old_slot, new_slot);
    set_needs_assignment(main_thread, changed_slot, shadow_root);
    hooks::slot_changed(main_thread, changed_slot, SLOT_CHANGE_SIGNAL | SLOT_CHANGE_INVALIDATE);
}

/// https://dom.spec.whatwg.org/#signal-a-slot-change
fn signal_a_slot_change(main_thread: &mut MainThread<'_>, slot: NodeRef) {
    // To signal a slot change, for a slot slot:
    // 1. Append slot to slot's relevant agent's signal slots.
    // 2. Queue a mutation observer microtask.
    hooks::slot_changed(main_thread, slot, SLOT_CHANGE_SIGNAL);
}

/// Whether `slot`, a slot of the shadow tree of `shadow_root`, has no assigned nodes, which are brought up to date
/// first.
fn assigned_nodes_are_empty(main_thread: &mut MainThread<'_>, slot: NodeRef, shadow_root: NodeRef) -> bool {
    settle_slot(main_thread, slot, shadow_root);
    slot_core(&slot).assigned_nodes().is_empty()
}

/// Adds the slots of `node`'s inclusive descendants, which have just joined the shadow tree of `shadow_root`, to its
/// slots, in tree order. Answers whether there were any.
fn add_slots_of_subtree(main_thread: &mut MainThread<'_>, shadow_root: NodeRef, node: NodeRef) -> bool {
    let mut added_any = false;
    for_each_slot_of_inclusive_descendants(main_thread, node, |main_thread, slot| {
        if !added_any {
            // NB: The assigned nodes of the slots already there are brought up to date before the new ones can find
            //     slottables they had.
            assign_slottables_if_needed(main_thread, shadow_root);
            added_any = true;
        }
        let slots = shadow_root_core(&shadow_root).slots();
        let main_thread = &*main_thread;
        let slot_node = slot.read(main_thread);
        let follows_all = slots.is_empty() || slots.get(slots.len() - 1).read(main_thread).is_preceding(slot_node);
        if follows_all {
            slots.push(slot);
            return;
        }
        // NB: The first slot that follows the new one is where it goes.
        let (mut low, mut high) = (0, slots.len());
        while low < high {
            let middle = (low + high) / 2;
            if slots.get(middle).read(main_thread).is_preceding(slot_node) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        slots.insert(low, slot);
    });
    added_any
}

/// Steps 7.4 to 7.6 of inserting nodes, and steps 22 to 24 of moving a node, for `node`, which has just joined `parent`'s
/// children.
/// https://dom.spec.whatwg.org/#concept-node-insert
/// https://dom.spec.whatwg.org/#move
#[inline(always)]
pub(super) fn assign_slots_after_linking(main_thread: &mut MainThread<'_>, parent: NodeRef, node: NodeRef) {
    // 4. If parent is a shadow host whose shadow root's slot assignment is "named" and node is a slottable, then
    //    assign a slot for node.
    if let Some(shadow_root) = parent.shadow_root()
        && !shadow_root_core(&shadow_root).is_manual()
        && is_slottable(node)
    {
        assign_a_slot(main_thread, node);
    }

    // 5. If parent's root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run
    //    signal a slot change for parent.
    let parent_root = parent.root();
    if parent_root.is_shadow_root() && is_slot(parent) && assigned_nodes_are_empty(main_thread, parent, parent_root) {
        signal_a_slot_change(main_thread, parent);
    }

    // 6. Run assign slottables for a tree with node's root.
    let root = node.root();
    if root.is_shadow_root() {
        // AD-HOC: The slots of the subtree join the slots of the shadow tree.
        let subtree_has_slot = add_slots_of_subtree(main_thread, root, node);
        // OPTIMIZATION: Inserting into a named shadow tree only changes slot assignments when the subtree holds a slot.
        if !subtree_has_slot && !shadow_root_core(&root).is_manual() {
            return;
        }
    }
    assign_slottables_for_a_tree(main_thread, root);
}

/// Steps 8 to 10 of removing a node, and steps 14 to 16 of moving a node, for `node`, which has just left the children
/// of `parent`, whose root is `parent_root`.
/// https://dom.spec.whatwg.org/#concept-node-remove
/// https://dom.spec.whatwg.org/#move
#[inline(always)]
pub(super) fn assign_slots_after_unlinking(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    parent_root: NodeRef,
) {
    // 8. If node is assigned, then run assign slottables for node's assigned slot.
    // AD-HOC: Assigned nodes are recomputed lazily.
    unassign_a_slot(main_thread, node);

    if !parent_root.is_shadow_root() {
        // NB: Neither step does anything where parent's root is not a shadow root: no slot of a tree that is not a
        //     shadow tree has assigned nodes.
        return;
    }
    assign_slots_after_unlinking_from_shadow_tree(main_thread, node, parent, parent_root);
}

/// Steps 9 and 10 of removing a node, and steps 15 and 16 of moving a node, for `node`, which has just left the
/// children of `parent`, whose root is `parent_root`, a shadow root.
#[inline(never)]
fn assign_slots_after_unlinking_from_shadow_tree(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    parent_root: NodeRef,
) {
    // 9. If parent's root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run
    //    signal a slot change for parent.
    if is_slot(parent) && assigned_nodes_are_empty(main_thread, parent, parent_root) {
        signal_a_slot_change(main_thread, parent);
    }

    // 10. If node has an inclusive descendant that is a slot:
    // NB: The slots that have left the shadow tree are the ones whose root is now node.
    let slots_left = {
        let slots = shadow_root_core(&parent_root).slots();
        (0..slots.len()).any(|index| slots.get(index).root() == node)
    };
    if !slots_left {
        return;
    }
    // AD-HOC: The assigned nodes of every slot are brought up to date while the slots that left are still among the
    //         shadow tree's, which they then leave.
    assign_slottables_if_needed(main_thread, parent_root);
    let slots = shadow_root_core(&parent_root).slots();
    let mut index = 0;
    while index < slots.len() {
        if slots.get(index).root() == node {
            slots.remove(index);
        } else {
            index += 1;
        }
    }

    //     1. Run assign slottables for a tree with parent's root.
    assign_slottables_for_a_tree(main_thread, parent_root);

    //     2. Run assign slottables for a tree with node.
    // NB: Each slot of the subtree, whose root is not a shadow root, can still have assigned nodes.
    for_each_slot_of_inclusive_descendants(main_thread, node, assign_slottables);
}

/// The attribute change steps that keep a slottable's name and a slot's name, for a change of the attribute without a
/// namespace whose local name is `local_name` of `element` from `old_value` to `value`, the raw words of the strings or
/// none for null.
/// https://dom.spec.whatwg.org/#slot-name
/// https://dom.spec.whatwg.org/#slotable-name
#[inline]
pub(super) fn run_attribute_change_steps(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    local_name: Utf16StringUnits<'_>,
    old_value: Option<usize>,
    value: Option<usize>,
) {
    let Utf16StringUnits::Ascii(ascii) = local_name else {
        return;
    };
    let is_slot_name = match ascii {
        b"slot" => false,
        b"name" if is_slot(element) => true,
        _ => return,
    };

    let is_empty = |value: usize| {
        // SAFETY: The caller borrows the values for the call.
        let string = unsafe { BorrowedString::from_raw(value) };
        units_are_empty(string.units())
    };
    // 1. If value is oldValue, then return.
    // 2. If value is null and oldValue is the empty string, then return.
    // 3. If value is the empty string and oldValue is null, then return.
    match (old_value, value) {
        (None, None) => return,
        // SAFETY: The caller borrows the values for the call.
        (Some(old_value), Some(value)) if unsafe { raw_strings_equal(old_value, value) } => return,
        (Some(old_value), None) if is_empty(old_value) => return,
        (None, Some(value)) if is_empty(value) => return,
        _ => {}
    }

    if is_slot_name {
        // 4. If value is null or the empty string, then set element's name to the empty string.
        // 5. Otherwise, set element's name to value.
        let name = value
            .filter(|&value| !is_empty(value))
            // SAFETY: The caller borrows the value for the call.
            .map(|value| fly_string_of(unsafe { BorrowedString::from_raw(value) }));
        // SAFETY: Nothing borrows the slot's name across the change.
        drop(unsafe { slot_core(&element).name.replace(name) });

        // 6. Run assign slottables for a tree with element's root.
        assign_slottables_for_a_tree(main_thread, element.root());
        return;
    }

    // 4. If value is null or the empty string, then set element's name to the empty string.
    // 5. Otherwise, set element's name to value.
    // NB: The name is read from the attribute, see the module documentation.

    // 6. If element is assigned, then run assign slottables for element's assigned slot.
    if let Some(assigned_slot) = element.assigned_slot() {
        assign_slottables(main_thread, assigned_slot);
    }

    // 7. Run assign a slot for element.
    assign_a_slot(main_thread, element);
}

/// Brings the assigned nodes of the slots of the shadow tree of `shadow_root` up to date, for
/// DOM::ShadowRoot::assign_slottables_if_needed().
///
/// # Safety
///
/// The shadow root must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_assign_slottables_if_needed(shadow_root: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The shadow root is live.
    let shadow_root = unsafe { NodeRef::from_cell(shadow_root, &main_thread) };
    assign_slottables_if_needed(&mut main_thread, shadow_root);
}

/// Finds a slot given `slottable` and `open`, for DOM::SlottableMixin::assigned_slot().
///
/// # Safety
///
/// The slottable must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_find_a_slot(slottable: &NodeCell, open: bool) -> *const NodeCell {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The slottable is live.
    let slottable = unsafe { NodeRef::from_cell(slottable, &main_thread) };
    find_a_slot(slottable, open).map_or(core::ptr::null(), |slot| slot.cell() as *const NodeCell)
}

/// Whether finding slottables for `slot` finds any.
///
/// # Safety
///
/// The slot must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_slot_finds_slottables(slot: &NodeCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The slot is live.
    let slot = unsafe { NodeRef::from_cell(slot, &main_thread) };
    let mut found = false;
    find_slottables(slot, |_| found = true);
    found
}

/// Finds flattened slottables for `slot` into `result`, a list C++ roots.
///
/// # Safety
///
/// The slot must be live, `result` the core of a DOM::RootedNodeList that lives for the call, and the call must be
/// made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_find_flattened_slottables(slot: &NodeCell, result: &RootedNodeListCore) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The slot is live.
    let slot = unsafe { NodeRef::from_cell(slot, &main_thread) };
    // SAFETY: The list lives for the call, and nothing else uses it.
    let mut result = unsafe { RootedNodes::from_core(result) };
    find_flattened_slottables(slot, &mut result);
}

/// Runs assign slottables for a tree with `root`, for HTML::HTMLSlotElement::assign().
///
/// # Safety
///
/// The node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_assign_slottables_for_a_tree(root: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let root = unsafe { NodeRef::from_cell(root, &main_thread) };
    assign_slottables_for_a_tree(&mut main_thread, root);
}

/// Removes `node` from the manually assigned nodes of `slot`, for HTML::HTMLSlotElement::assign().
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_slot_remove_manually_assigned_node(slot: &NodeCell, node: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let (slot, node) = unsafe {
        (
            NodeRef::from_cell(slot, &main_thread),
            NodeRef::from_cell(node, &main_thread),
        )
    };
    let list = slot_core(&slot).manually_assigned_nodes();
    if let Some(index) = list.position(node) {
        list.remove(index);
    }
}

/// Sets the manually assigned nodes of `slot` to `nodes`, for HTML::HTMLSlotElement::assign().
///
/// # Safety
///
/// The nodes must be live and rooted for the call, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_slot_set_manually_assigned_nodes(slot: &NodeCell, nodes: ak::Span<'_, NodeRef>) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The slot is live.
    let slot = unsafe { NodeRef::from_cell(slot, &main_thread) };
    let pointers: Vec<NodePointer> = nodes.as_slice().iter().map(|node| node.pointer()).collect();
    slot_core(&slot).manually_assigned_nodes().set_to(&pointers);
}

/// Frees the buffers of a slot's core, as C++ destroys the slot.
///
/// # Safety
///
/// The core must be that of a slot that is being destroyed, and nothing may read it after.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_slot_core_destroy(core: &SlotCore) {
    core.assigned_nodes.free();
    core.manually_assigned_nodes.free();
}

/// Frees the buffers of a shadow root's core, as C++ destroys the shadow root.
///
/// # Safety
///
/// The core must be that of a shadow root that is being destroyed, and nothing may read it after.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_shadow_root_core_destroy(core: &ShadowRootCore) {
    core.slots.free();
    core.slots_needing_assignment.free();
}

#[cfg(test)]
mod tests {
    use super::super::node_core::NodeType;
    use super::super::test_tree::node;
    use super::*;

    #[test]
    fn a_list_of_nodes_inserts_removes_and_compares_in_order() {
        let main_thread = MainThread::for_test();
        let [a, b, c] = [0; 3].map(|_| NodeRef::from(node(&main_thread, NodeType::ELEMENT_NODE)));
        let list = NodeListField::new_for_test();
        list.push(b);
        list.insert(0, a);
        list.push(c);
        assert_eq!([list.get(0), list.get(1), list.get(2)], [a, b, c]);
        assert!(list.is_identical_to(&[a.pointer(), b.pointer(), c.pointer()]));
        assert!(!list.is_identical_to(&[a.pointer(), c.pointer(), b.pointer()]));
        assert_eq!(list.position(c), Some(2));

        list.remove(1);
        assert!(list.is_identical_to(&[a.pointer(), c.pointer()]));
        assert!(!list.contains(b));

        list.set_to(&[c.pointer()]);
        assert!(list.is_identical_to(&[c.pointer()]));
        list.set_to(&[]);
        assert!(list.is_empty());
        list.free();
    }
}
