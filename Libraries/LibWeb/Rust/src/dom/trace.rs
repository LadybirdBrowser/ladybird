/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Tracing the fields of DOM objects that Rust defines: every field of the prefixes and cores derives libgc_api's
//! Trace, so that a field that can hold a cell has to be traced, or say what keeps its cell alive, to compile. Rust
//! traces the nodes itself, see node_objects.rs and element_objects.rs, and the visit_edges() of every other DOM object
//! with a core hands the core to the functions here. C++ visits what the opaque OpaqueCxx slots of the prefixes hold.
//!
//! Tracing runs on the threads of the collector while the main thread is stopped, so it only reads.

use core::ffi::c_void;
use core::num::NonZeroU32;
use core::ptr::NonNull;

use libgc_api::capi::GCVisitor;
use libgc_api::{Trace, Visitor};

use super::attr::NamedNodeMapCore;
use super::collection::CollectionCore;
use super::event::EventCore;
use super::node::heap_region_base;
use super::node_class::NodeClassId;
use super::prefix::{
    AttrPrefix, CharacterDataPrefix, DocumentFragmentPrefix, DocumentPrefix, ElementPrefix, EventTargetPrefix,
    HTMLSlotElementPrefix, NodePrefix, ShadowRootPrefix, TextPrefix,
};
use super::range::RangeCore;
use super::traversal::{NodeIteratorCore, TreeWalkerCore};

/// The start of the cell a GC::Member names, or none for null.
#[inline(always)]
pub(super) fn member_cell_address(compressed: u32) -> Option<NonNull<c_void>> {
    let compressed = NonZeroU32::new(compressed)?;
    let address = libgc_api::member::decompress(heap_region_base(), compressed.get());
    // SAFETY: A compressed pointer decodes to an address in the heap region, which is never null.
    Some(unsafe { NonNull::new_unchecked(core::ptr::with_exposed_provenance_mut(address)) })
}

/// Makes field types that are laid out as GC::Member, a compressed pointer in a Cell<u32>, traceable through their slot.
macro_rules! member_fields {
    ($($type:ty),* $(,)?) => {
        $(
            // SAFETY: The field is a Cell<u32> that holds 0 or the compressed pointer of a live cell.
            unsafe impl libgc_api::MemberSlot for $type {
                #[inline(always)]
                fn slot(&self) -> *const u32 {
                    self.0.as_ptr()
                }

                #[inline(always)]
                fn cell_address(&self) -> Option<core::ptr::NonNull<core::ffi::c_void>> {
                    crate::dom::trace::member_cell_address(self.0.get())
                }
            }

            // SAFETY: Visits the cell the field names, if any.
            unsafe impl libgc_api::Trace for $type {
                #[inline(always)]
                fn trace<'c>(&'c self, visitor: &mut libgc_api::Visitor<'c>) {
                    visitor.visit_member(self);
                }
            }
        )*
    };
}

/// Makes field types that cannot hold cells traceable, as tracing nothing.
macro_rules! fields_without_cells {
    ($($type:ty),* $(,)?) => {
        $(
            // SAFETY: The field holds no cells.
            unsafe impl libgc_api::Trace for $type {
                #[inline(always)]
                fn trace(&self, _: &mut libgc_api::Visitor<'_>) {}
            }
        )*
    };
}

pub(super) use fields_without_cells;
pub(super) use member_fields;

/// Traces the fields Rust defines of the event target `cell`: those of its EventTargetPrefix, and of every prefix of a
/// node class that it starts with.
///
/// # Safety
///
/// `cell` must be a live event target, and `visitor` a visitor LibGC passed its visit_edges().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_event_target(cell: *const c_void, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    let mut visitor = unsafe { Visitor::from_raw(visitor) };
    // SAFETY: Guaranteed by the caller.
    let class_id = unsafe { NodeClassId::of_cell(cell) };
    // SAFETY: Every event target starts with the prefix of the most derived class of nodes it is of, which the C++
    //         of each class asserts.
    unsafe {
        if class_id == NodeClassId::NotANode {
            trace_as::<EventTargetPrefix>(cell, &mut visitor);
        } else if class_id.is_a(NodeClassId::Element) {
            if class_id.is_a(NodeClassId::HTMLSlotElement) {
                trace_as::<HTMLSlotElementPrefix>(cell, &mut visitor);
            } else {
                trace_as::<ElementPrefix>(cell, &mut visitor);
            }
        } else if class_id.is_a(NodeClassId::Text) {
            trace_as::<TextPrefix>(cell, &mut visitor);
        } else if class_id.is_a(NodeClassId::CharacterData) {
            trace_as::<CharacterDataPrefix>(cell, &mut visitor);
        } else if class_id.is_a(NodeClassId::Document) {
            trace_as::<DocumentPrefix>(cell, &mut visitor);
        } else if class_id.is_a(NodeClassId::ShadowRoot) {
            trace_as::<ShadowRootPrefix>(cell, &mut visitor);
        } else if class_id.is_a(NodeClassId::DocumentFragment) {
            trace_as::<DocumentFragmentPrefix>(cell, &mut visitor);
        } else if class_id.is_a(NodeClassId::Attr) {
            trace_as::<AttrPrefix>(cell, &mut visitor);
        } else {
            trace_as::<NodePrefix>(cell, &mut visitor);
        }
    }
}

/// # Safety
///
/// `cell` must start with a live `T`.
#[inline(always)]
unsafe fn trace_as<T: Trace + 'static>(cell: *const c_void, visitor: &mut Visitor<'_>) {
    // SAFETY: Guaranteed by the caller.
    unsafe { &*cell.cast::<T>() }.trace(visitor);
}

/// Traces the core `core` of a cell that is no event target, which C++ embeds in the cell.
///
/// # Safety
///
/// `core` must be the core of a live cell, and `visitor` a visitor LibGC passed the cell's visit_edges().
#[inline(always)]
unsafe fn trace_core<T: Trace + 'static>(core: &T, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    core.trace(&mut unsafe { Visitor::from_raw(visitor) });
}

/// Traces the fields of an EventCore, see the module.
///
/// # Safety
///
/// See trace_core().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_event_core(core: &EventCore, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace_core(core, visitor) };
}

/// Traces the fields of a RangeCore, see the module.
///
/// # Safety
///
/// See trace_core().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_range_core(core: &RangeCore, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace_core(core, visitor) };
}

/// Traces the fields of a NodeIteratorCore, see the module.
///
/// # Safety
///
/// See trace_core().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_node_iterator_core(core: &NodeIteratorCore, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace_core(core, visitor) };
}

/// Traces the fields of a TreeWalkerCore, see the module.
///
/// # Safety
///
/// See trace_core().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_tree_walker_core(core: &TreeWalkerCore, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace_core(core, visitor) };
}

/// Traces the fields of a CollectionCore, see the module.
///
/// # Safety
///
/// See trace_core().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_collection_core(core: &CollectionCore, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace_core(core, visitor) };
}

/// Traces the fields of a NamedNodeMapCore, see the module.
///
/// # Safety
///
/// See trace_core().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_trace_named_node_map_core(core: &NamedNodeMapCore, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace_core(core, visitor) };
}

#[cfg(test)]
mod tests {
    use crate::gc_test_stubs::RecordingVisitor;

    use super::*;
    use crate::dom::node::Node;
    use crate::dom::node_core::NodeType;
    use crate::dom::test_tree::{append_child, node, set_assigned_slot, shadow_root};
    use crate::stage::MainThread;

    /// The cells that tracing `node` visits.
    fn traced(node: Node<'_>) -> Vec<usize> {
        let mut recording = RecordingVisitor::new();
        let visitor = recording.raw();
        // SAFETY: The node is live, and the visitor outlives the call.
        unsafe { rust_dom_trace_event_target(core::ptr::from_ref(node.cell()).cast(), visitor) };
        recording.take_visited()
    }

    fn address(node: Node<'_>) -> usize {
        core::ptr::from_ref(node.cell()).addr()
    }

    #[test]
    fn an_element_traces_its_tree_its_shadow_root_and_its_slot() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let element = node(&main_thread, NodeType::ELEMENT_NODE);
        let child = node(&main_thread, NodeType::TEXT_NODE);
        let slot = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(parent, element);
        append_child(element, child);
        let shadow_root = shadow_root(&main_thread, element);
        set_assigned_slot(element, slot);

        let visited = traced(element);
        for expected in [parent, child, shadow_root, slot] {
            assert!(visited.contains(&address(expected)));
        }
        // NB: The first and last child are the same node, which each of them visits.
        assert_eq!(visited.iter().filter(|&&cell| cell == address(child)).count(), 2);
    }

    #[test]
    fn a_text_node_traces_its_parent_and_its_slot_and_a_shadow_root_its_host() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let text = node(&main_thread, NodeType::TEXT_NODE);
        let slot = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(parent, text);
        set_assigned_slot(text, slot);
        let visited = traced(text);
        assert!(visited.contains(&address(parent)) && visited.contains(&address(slot)));

        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        let shadow_root = shadow_root(&main_thread, host);
        assert!(traced(shadow_root).contains(&address(host)));
    }
}
