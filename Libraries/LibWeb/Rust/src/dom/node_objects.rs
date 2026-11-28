/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The classes of nodes whose objects Rust defines whole: Text, CDATASection, Comment, ProcessingInstruction,
//! DocumentType, Attr and DocumentFragment. Rust lays
//! out each object as a struct of its own, allocates it from LibGC's heap with a type info of its own, traces it,
//! finalizes it and destroys it. C++ keeps the class as a shell over the object, through which C++ code and the
//! bindings reach the node as before. The shell has no member of its own beyond the core Rust lays out at its end, and
//! no allocator, so C++ can neither allocate one nor construct one but as the probe below, and its destructor is
//! private and never runs.
//!
//! What C++ still owns of such a node is what the prefixes keep in opaque slots (see prefix.rs). Rust writes them as the
//! constructors of the C++ classes would: the vtable pointer, which C++ reads once from a probe of each class that it
//! constructs as it registers the hooks below, the cell header, and the null wrapper, data and rare data. LibGC
//! dispatches through the type info Rust gives each class, whose functions trace, finalize, measure and destroy the
//! fields Rust defines, and have C++ do the same for the members in the slots through the hooks.
//!
//! The objects are allocated with the cell size and alignment C++ gave the classes, in blocks of their own for each
//! class, and traced on the main thread only, as C++ cells are, since what C++ visits of them is not all their own.

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::mem::MaybeUninit;
use core::ptr::NonNull;

use libgc_api::capi::{self, CellTypeInfo, GCVisitor};
use libgc_api::cell::{self, Allocator, RustCell};
use libgc_api::{Trace, Visitor};

use super::attr::AttrCore;
use super::attr::attr_value;
use super::attributes;
use super::node::NodeCell;
use super::node::compress;
use super::node_class::NODE_OBJECT_HAS_PAYLOAD;
use super::node_class::NODE_OBJECT_PAYLOAD_HAS_EDGES;
use super::node_class::NodeClassId;
use super::node_core::CharacterDataCore;
use super::node_core::DocumentFragmentCore;
use super::node_core::DocumentTypeCore;
use super::node_core::NODE_IS_CLOSED_SHADOW_ROOT;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use super::node_core::ProcessingInstructionCore;
use super::node_ref::NodeRef;
use super::prefix::AttrPrefix;
use super::prefix::CharacterDataPrefix;
use super::prefix::CxxWords;
use super::prefix::DocumentFragmentPrefix;
use super::prefix::NodePrefix;
use super::prefix::ParentNodePrefix;
use super::prefix::ShadowRootPrefix;
use super::prefix::TextPrefix;
use super::qualified_name::QualifiedNameImpl;
use super::slots::ShadowRootCore;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// What C++ registers for the nodes whose objects Rust defines: the vtable pointer it writes into an object of each
/// class as it constructs one, and the callbacks through which it visits, finalizes, measures and destroys what it
/// keeps in the opaque slots of a node's prefixes.
#[repr(C)]
pub struct NodeObjectHooks {
    pub text_vtable: *const c_void,
    /// The vtable pointer of the SlottableMixin base of DOM::Text, which TextPrefix keeps in an opaque slot.
    pub text_slottable_mixin_vtable: *const c_void,
    pub cdata_section_vtable: *const c_void,
    pub cdata_section_slottable_mixin_vtable: *const c_void,
    pub comment_vtable: *const c_void,
    pub processing_instruction_vtable: *const c_void,
    pub document_type_vtable: *const c_void,
    pub attr_vtable: *const c_void,
    pub document_fragment_vtable: *const c_void,
    /// Visits the cells the members in the opaque slots of `node` hold. Runs on the main thread, while the collector
    /// marks or gathers a graph of the heap.
    pub visit_slot_edges: unsafe extern "C" fn(node: &NodeCell, visitor: *mut GCVisitor),
    /// Runs DOM::Node::finalize() on `node`, which the collector found dead, and which finalizes the members in the
    /// opaque slots.
    pub finalize: unsafe extern "C" fn(node: &NodeCell),
    /// The size of the memory outside the heap that `node` holds, as the C++ class of the node measures it.
    pub external_memory_size: unsafe extern "C" fn(node: &NodeCell) -> usize,
    /// Destroys the members in the opaque slots of `node`, which the collector found dead and finalized, and whose
    /// cell it is about to free.
    pub destroy_slots: unsafe extern "C" fn(node: &NodeCell),
    /// Visits the cells that `node`, a node of a class with a payload with cells, holds through the visit_edges() of
    /// its class, which visits those of the opaque slots as well. Runs on the main thread, while the collector marks or
    /// gathers a graph of the heap.
    pub visit_edges: unsafe extern "C" fn(node: &NodeCell, visitor: *mut GCVisitor),
    /// Runs the destructor of the class of `node`, a node of a class with a payload, which the collector found dead and
    /// finalized, once Rust has released what the cores of its prefix hold.
    pub destroy: unsafe extern "C" fn(node: &NodeCell),
}

/// The hooks C++ registers, and an allocator for each class, which live for the rest of the process.
struct NodeObjects {
    hooks: &'static NodeObjectHooks,
    null_weak_impl: u64,
    text: Allocator,
    cdata_section: Allocator,
    comment: Allocator,
    processing_instruction: Allocator,
    document_type: Allocator,
    attr: Allocator,
    document_fragment: Allocator,
    shadow_root: Allocator,
}

impl NodeObjects {
    #[inline(always)]
    fn words(&self, vtable: *const c_void) -> CxxWords {
        CxxWords {
            vtable: vtable.addr() as u64,
            null_weak_impl: self.null_weak_impl,
        }
    }
}

/// The registered state, which C++ sets once, before any node exists, and never changes after that.
#[repr(transparent)]
struct RegisteredNodeObjects(UnsafeCell<Option<NodeObjects>>);

// SAFETY: The state is set once, before any node exists, and only the main thread, which makes, traces and destroys
//         every node, reads it.
unsafe impl Sync for RegisteredNodeObjects {}

static NODE_OBJECTS: RegisteredNodeObjects = RegisteredNodeObjects(UnsafeCell::new(None));

#[inline(always)]
fn node_objects() -> &'static NodeObjects {
    // SAFETY: Nothing writes the state after the first node exists, see RegisteredNodeObjects.
    unsafe { (*NODE_OBJECTS.0.get()).as_ref() }.expect("the node object hooks are registered before any node exists")
}

/// An allocator of the objects of `T`, which LibGC traces on the main thread only.
///
/// NB: No allocator of nodes is movable, see gc_allocator_set_movable(), so collections never move a node: C++
///     constructs the payload of a node in place and keeps pointers into it, and the live collections and the
///     C++ tables keyed by node hold nodes by address without visiting them.
///
/// # Safety
///
/// Only rust_dom_register_node_objects() may call it, once for each type.
unsafe fn allocator_of<T: NodeObject>() -> Allocator {
    // SAFETY: The allocator lives for the rest of the process, as the heaps it allocates from may, and the address of
    //         the type info, which lives as long, is unique to the type.
    unsafe {
        let allocator = Allocator::new(T::TYPE_INFO, T::NAME, core::ptr::from_ref(T::TYPE_INFO).cast());
        capi::gc_allocator_set_traced_on_owner_thread_only(allocator.as_raw());
        allocator
    }
}

/// Registers the hooks of the nodes whose objects Rust defines, and makes their allocators.
///
/// # Safety
///
/// No node may exist yet, and the call must be made on the main thread, before any other thread starts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_register_node_objects(hooks: &'static NodeObjectHooks) {
    // NB: The null impl counts no references, so every null GC::Weak can hold it without taking one.
    let null_weak_impl = capi::gc_weak_impl_null().addr() as u64;
    // SAFETY: Guaranteed by the caller.
    unsafe {
        *NODE_OBJECTS.0.get() = Some(NodeObjects {
            hooks,
            null_weak_impl,
            text: allocator_of::<TextObject>(),
            cdata_section: allocator_of::<CDATASectionObject>(),
            comment: allocator_of::<CommentObject>(),
            processing_instruction: allocator_of::<ProcessingInstructionObject>(),
            document_type: allocator_of::<DocumentTypeObject>(),
            attr: allocator_of::<AttrObject>(),
            document_fragment: allocator_of::<DocumentFragmentObject>(),
            shadow_root: payload_allocator_of::<ShadowRootObject>(0),
        });
    }
}

/// The classes of nodes whose objects Rust lays out as the prefix of the class followed by a payload, which C++
/// constructs over the prefix Rust made (see DOM::RustMadePrefix), destroys, and visits, finalizes and measures through
/// the functions of the class: shadow roots. Each has a type info of its own, with the cell size C++ gives the class.
const PAYLOAD_CLASSES: [(NodeClassId, &str); 1] = [(NodeClassId::ShadowRoot, "ShadowRoot")];

/// The type info of each class of PAYLOAD_CLASSES, by its index there, which rust_dom_register_node_objects() writes
/// before any node exists, and which never changes after that.
#[repr(transparent)]
struct PayloadTypeInfos(UnsafeCell<[MaybeUninit<CellTypeInfo>; PAYLOAD_CLASSES.len()]>);

// SAFETY: The type infos are written once, before any node exists, and only read after that.
unsafe impl Sync for PayloadTypeInfos {}

static PAYLOAD_TYPE_INFOS: PayloadTypeInfos = PayloadTypeInfos(UnsafeCell::new(
    [const { MaybeUninit::uninit() }; PAYLOAD_CLASSES.len()],
));

/// An allocator of the nodes of the class at `index` in PAYLOAD_CLASSES, whose objects start with `T`, which LibGC
/// traces on the main thread only, as the payload of every such class holds cells that only C++ visits.
///
/// # Safety
///
/// Only rust_dom_register_node_objects() may call it, once for each class.
unsafe fn payload_allocator_of<T: PayloadObject>(index: usize) -> Allocator {
    let (class_id, name) = PAYLOAD_CLASSES[index];
    let facts = class_id.facts();
    assert!(
        facts.flags & NODE_OBJECT_HAS_PAYLOAD != 0
            && facts.cell_size as usize >= size_of::<T>()
            && facts.cell_size.is_multiple_of(align_of::<T>() as u32),
        "a node of class {name} is its prefix and a payload"
    );
    let type_info = CellTypeInfo {
        cell_size: facts.cell_size,
        external_memory_size: Some(external_memory_size),
        ..cell::type_info_of::<T>(Some(finalize))
    };
    // SAFETY: Guaranteed by the caller; nothing reads the type info before this writes it, and it lives for the rest of
    //         the process, at an address unique to the class.
    unsafe {
        let type_info: &'static CellTypeInfo = (*PAYLOAD_TYPE_INFOS.0.get())[index].write(type_info);
        let allocator = Allocator::new(type_info, name, core::ptr::from_ref(type_info).cast());
        capi::gc_allocator_set_traced_on_owner_thread_only(allocator.as_raw());
        allocator
    }
}

/// The object of a class of PAYLOAD_CLASSES, which is the prefix of the class: the payload that follows it in the cell
/// is C++'s.
///
/// # Safety
///
/// The type must be #[repr(C)], and start with the prefix of the class, which the C++ class asserts it starts with.
unsafe trait PayloadObject: RustCell {
    #[inline(always)]
    fn node(&self) -> &NodeCell {
        // SAFETY: The object starts with its NodePrefix, so it is a node.
        unsafe { &*core::ptr::from_ref(self).cast::<NodeCell>() }
    }
}

/// Has C++ visit the cells the payload of `node`, a node of a class of PAYLOAD_CLASSES, holds, and those in the opaque
/// slots of its prefixes, through the visit_edges() of its class where its payload holds cells, which T's Trace calls
/// after tracing its fields.
#[inline(always)]
fn visit_payload_edges(node: &NodeCell, visitor: &mut Visitor<'_>) {
    let Some(raw) = visitor.as_raw() else {
        return;
    };
    let hooks = node_objects().hooks;
    // SAFETY: LibGC traces live nodes on the main thread, with a visitor that outlives the call.
    unsafe {
        if NodeClassId::of_cell(node).facts().flags & NODE_OBJECT_PAYLOAD_HAS_EDGES != 0 {
            (hooks.visit_edges)(node, raw.as_ptr());
        } else {
            (hooks.visit_slot_edges)(node, raw.as_ptr());
        }
    }
}

/// Runs the destructor of the class of `object`, a node of a class of PAYLOAD_CLASSES, which destroys its payload and
/// the members in its opaque slots, as its Drop does once it has released what its cores hold.
///
/// # Safety
///
/// LibGC must be destroying the object, which nothing reads after this.
#[inline(always)]
unsafe fn destroy_payload<T: PayloadObject>(object: &T) {
    // SAFETY: Guaranteed by the caller.
    unsafe { (node_objects().hooks.destroy)(object.node()) }
}

/// A DOM::ShadowRoot: the prefix of the class, which Rust makes, traces and releases, followed by the payload of the
/// class.
/// https://dom.spec.whatwg.org/#interface-shadowroot
#[repr(C)]
pub struct ShadowRootObject {
    shadow_root: ShadowRootPrefix,
}

// SAFETY: Traces every field, and has C++ visit the members of the payload and of the opaque slots.
unsafe impl Trace for ShadowRootObject {
    #[inline(always)]
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        self.shadow_root.trace(visitor);
        visit_payload_edges(self.node(), visitor);
    }
}

// SAFETY: The object is #[repr(C)], starts with the header of its cell, which its WrappablePrefix lays out, and is the
//         size and alignment of the prefix of DOM::ShadowRoot, which fits a cell.
unsafe impl RustCell for ShadowRootObject {
    const NAME: &'static str = "ShadowRoot";
}

// SAFETY: The object is the prefix of DOM::ShadowRoot, which ShadowRoot.cpp asserts the class starts with.
unsafe impl PayloadObject for ShadowRootObject {}

impl Drop for ShadowRootObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, on the main thread, and nothing reads it after this. The destructor
        //         of DOM::ShadowRoot takes the core as release() leaves it.
        unsafe {
            self.shadow_root.core.release(&mut main_thread());
            destroy_payload(self);
        }
    }
}

/// An object whose class Rust defines, which starts with the NodePrefix of every node.
///
/// # Safety
///
/// The type must be #[repr(C)], start with a NodePrefix, and have the size and alignment of the C++ class it defines,
/// which asserts them.
unsafe trait NodeObject: RustCell {
    /// The type info of the class, which the allocator of the class allocates with.
    const TYPE_INFO: &'static CellTypeInfo;

    #[inline(always)]
    fn node(&self) -> &NodeCell {
        // SAFETY: The object starts with its NodePrefix, so it is a node.
        unsafe { &*core::ptr::from_ref(self).cast::<NodeCell>() }
    }
}

/// The type info of the objects of `T`: LibGC traces them with T's Trace, which includes the members in the opaque
/// slots, has C++ finalize and measure them, and destroys them by dropping them in place.
const fn type_info_of<T: NodeObject>() -> CellTypeInfo {
    CellTypeInfo {
        external_memory_size: Some(external_memory_size),
        ..cell::type_info_of::<T>(Some(finalize))
    }
}

/// Visits the cells the members in the opaque slots of `node` hold, which T's Trace calls after tracing its fields.
#[inline(always)]
fn visit_slot_edges(node: &NodeCell, visitor: &mut Visitor<'_>) {
    let Some(raw) = visitor.as_raw() else {
        return;
    };
    // SAFETY: LibGC traces live nodes on the main thread, with a visitor that outlives the call.
    unsafe { (node_objects().hooks.visit_slot_edges)(node, raw.as_ptr()) }
}

/// # Safety
///
/// Only LibGC calls it, with a dead node of a class whose objects Rust defines.
unsafe extern "C" fn finalize(cell: *mut c_void) {
    // SAFETY: Guaranteed by the caller.
    unsafe { (node_objects().hooks.finalize)(&*cell.cast::<NodeCell>()) }
}

/// # Safety
///
/// Only LibGC calls it, with a live node of a class whose objects Rust defines.
unsafe extern "C" fn external_memory_size(cell: *const c_void) -> usize {
    // SAFETY: Guaranteed by the caller.
    unsafe { (node_objects().hooks.external_memory_size)(&*cell.cast::<NodeCell>()) }
}

/// Destroys the members in the opaque slots of `object`, as its Drop does before it drops its own fields.
///
/// # Safety
///
/// LibGC must be destroying the object, which nothing reads after this but its Drop.
#[inline(always)]
unsafe fn destroy_slots<T: NodeObject>(object: &T) {
    // SAFETY: Guaranteed by the caller.
    unsafe { (node_objects().hooks.destroy_slots)(object.node()) }
}

/// Allocates a node of `T` in the heap of `document`, whose value `make` makes from its node pointer, with `allocator`,
/// which must be one of objects of `T`. Can collect garbage, which `make` cannot.
#[inline(always)]
pub(super) fn allocate<T: RustCell>(
    _main_thread: &mut MainThread<'_>,
    document: NodeRef,
    allocator: &Allocator,
    make: impl FnOnce(NodePointer) -> T,
) -> NodeRef {
    // SAFETY: The document is live, and its heap is that of the main thread, whose token the caller holds.
    let heap = unsafe { cell::heap_of(NonNull::from(document.cell())) };
    // SAFETY: The allocator is one of objects of T. The document, which the caller holds on the stack, outlives a
    //         collection the allocation runs, and `make` neither allocates nor reads the cell.
    let object = unsafe {
        cell::allocate_with(heap, allocator, |cell: NonNull<T>| {
            // SAFETY: A NodeCell has no size, so the uninitialized storage is one.
            make(compress(cell.cast::<NodeCell>().as_ref()))
        })
    };
    // SAFETY: The new node is live, and the caller holds it on the stack.
    unsafe { NodeRef::from_cell_unchecked(object.cast()) }
}

/// Implements what every object type has alike: its Trace, which traces its fields and then has C++ visit the slots,
/// and its RustCell.
macro_rules! node_object {
    ($object:ident, $name:literal, $($field:ident),+) => {
        // SAFETY: Traces every field, and has C++ visit the members of the opaque slots.
        unsafe impl Trace for $object {
            #[inline(always)]
            fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
                $(self.$field.trace(visitor);)+
                visit_slot_edges(self.node(), visitor);
            }
        }

        // SAFETY: The object is #[repr(C)], starts with the header of its cell, which its WrappablePrefix lays out, and
        //         is the size and alignment of the C++ class, which fits a cell.
        unsafe impl RustCell for $object {
            const NAME: &'static str = $name;
        }
    };
}

/// A whole DOM::Text.
/// https://dom.spec.whatwg.org/#interface-text
#[repr(C)]
pub struct TextObject {
    text: TextPrefix,
}

node_object!(TextObject, "Text", text);

static TEXT_TYPE_INFO: CellTypeInfo = type_info_of::<TextObject>();

// SAFETY: The object starts with its NodePrefix, and Text.cpp asserts its size and alignment.
unsafe impl NodeObject for TextObject {
    const TYPE_INFO: &'static CellTypeInfo = &TEXT_TYPE_INFO;
}

impl Drop for TextObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, and nothing reads it after this.
        unsafe {
            destroy_slots(self);
            drop(self.text.character_data.core.take_data());
        }
    }
}

/// A whole DOM::CDATASection, which is a Text node with a class of its own.
/// https://dom.spec.whatwg.org/#interface-cdatasection
#[repr(C)]
pub struct CDATASectionObject {
    text: TextPrefix,
}

node_object!(CDATASectionObject, "CDATASection", text);

static CDATA_SECTION_TYPE_INFO: CellTypeInfo = type_info_of::<CDATASectionObject>();

// SAFETY: The object starts with its NodePrefix, and CDATASection.cpp asserts its size and alignment.
unsafe impl NodeObject for CDATASectionObject {
    const TYPE_INFO: &'static CellTypeInfo = &CDATA_SECTION_TYPE_INFO;
}

impl Drop for CDATASectionObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, and nothing reads it after this.
        unsafe {
            destroy_slots(self);
            drop(self.text.character_data.core.take_data());
        }
    }
}

/// A whole DOM::Comment.
/// https://dom.spec.whatwg.org/#interface-comment
#[repr(C)]
pub struct CommentObject {
    character_data: CharacterDataPrefix,
}

node_object!(CommentObject, "Comment", character_data);

static COMMENT_TYPE_INFO: CellTypeInfo = type_info_of::<CommentObject>();

// SAFETY: The object starts with its NodePrefix, and Comment.cpp asserts its size and alignment.
unsafe impl NodeObject for CommentObject {
    const TYPE_INFO: &'static CellTypeInfo = &COMMENT_TYPE_INFO;
}

impl Drop for CommentObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, and nothing reads it after this.
        unsafe {
            destroy_slots(self);
            drop(self.character_data.core.take_data());
        }
    }
}

/// A whole DOM::ProcessingInstruction.
/// https://dom.spec.whatwg.org/#interface-processinginstruction
#[repr(C)]
pub struct ProcessingInstructionObject {
    character_data: CharacterDataPrefix,
    pub(super) core: ProcessingInstructionCore,
}

node_object!(
    ProcessingInstructionObject,
    "ProcessingInstruction",
    character_data,
    core
);

static PROCESSING_INSTRUCTION_TYPE_INFO: CellTypeInfo = type_info_of::<ProcessingInstructionObject>();

// SAFETY: The object starts with its NodePrefix, and ProcessingInstruction.cpp asserts its size and alignment.
unsafe impl NodeObject for ProcessingInstructionObject {
    const TYPE_INFO: &'static CellTypeInfo = &PROCESSING_INSTRUCTION_TYPE_INFO;
}

impl Drop for ProcessingInstructionObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, and nothing reads it after this.
        unsafe {
            destroy_slots(self);
            drop(self.character_data.core.take_data());
            self.core.release();
        }
    }
}

/// A whole DOM::DocumentType.
/// https://dom.spec.whatwg.org/#interface-documenttype
#[repr(C)]
pub struct DocumentTypeObject {
    node: NodePrefix,
    pub(super) core: DocumentTypeCore,
}

node_object!(DocumentTypeObject, "DocumentType", node, core);

static DOCUMENT_TYPE_TYPE_INFO: CellTypeInfo = type_info_of::<DocumentTypeObject>();

// SAFETY: The object starts with its NodePrefix, and DocumentType.cpp asserts its size and alignment.
unsafe impl NodeObject for DocumentTypeObject {
    const TYPE_INFO: &'static CellTypeInfo = &DOCUMENT_TYPE_TYPE_INFO;
}

impl Drop for DocumentTypeObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, and nothing reads it after this.
        unsafe {
            destroy_slots(self);
            self.core.release();
        }
    }
}

/// A whole DOM::Attr.
/// https://dom.spec.whatwg.org/#interface-attr
#[repr(C)]
pub struct AttrObject {
    attr: AttrPrefix,
}

node_object!(AttrObject, "Attr", attr);

static ATTR_TYPE_INFO: CellTypeInfo = type_info_of::<AttrObject>();

// SAFETY: The object starts with its NodePrefix, and Attr.cpp asserts its size and alignment.
unsafe impl NodeObject for AttrObject {
    const TYPE_INFO: &'static CellTypeInfo = &ATTR_TYPE_INFO;
}

impl Drop for AttrObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, on the main thread, and nothing reads it after this.
        unsafe {
            destroy_slots(self);
            self.attr.core.release(&mut main_thread());
        }
    }
}

/// A whole DOM::DocumentFragment.
/// https://dom.spec.whatwg.org/#interface-documentfragment
#[repr(C)]
pub struct DocumentFragmentObject {
    document_fragment: DocumentFragmentPrefix,
}

node_object!(DocumentFragmentObject, "DocumentFragment", document_fragment);

static DOCUMENT_FRAGMENT_TYPE_INFO: CellTypeInfo = type_info_of::<DocumentFragmentObject>();

// SAFETY: The object starts with its NodePrefix, and DocumentFragment.cpp asserts its size and alignment.
unsafe impl NodeObject for DocumentFragmentObject {
    const TYPE_INFO: &'static CellTypeInfo = &DOCUMENT_FRAGMENT_TYPE_INFO;
}

impl Drop for DocumentFragmentObject {
    fn drop(&mut self) {
        // SAFETY: LibGC destroys the object once, and nothing reads it after this. The fields Rust defines of a fragment
        //         hold nothing to release.
        unsafe { destroy_slots(self) }
    }
}

/// The copy of `node` that "clone a single node" makes given `document`, where `node` is of a class whose objects Rust
/// defines, or none for a node of any other class. The node is no element, and its class runs no cloning steps, which
/// NodeObjects.cpp asserts, so the copy is also what steps 2 and 3 of "clone a node" make of it.
/// https://dom.spec.whatwg.org/#clone-a-single-node
pub(super) fn clone_single_node(main_thread: &mut MainThread<'_>, document: NodeRef, node: NodeRef) -> Option<NodeRef> {
    // 3. Otherwise, set copy to a node that implements the same interfaces as node, and fulfills these additional
    //    requirements, switching on the interface node implements:
    // 6. Set copy's node document to document.
    // NB: Each copy is made with document as its node document.
    let copy = match node.class_id() {
        // -> DocumentType
        NodeClassId::DocumentType => {
            // Set copy's name, public ID, and system ID to those of node.
            let core = node.document_type_core().expect("the node is a doctype");
            let (name, public_id, system_id) = (
                core.name().to_fly_string(),
                core.public_id().to_utf16_string(),
                core.system_id().to_utf16_string(),
            );
            create_document_type(main_thread, document, name, public_id, system_id)
        }
        // -> Text
        NodeClassId::Text => {
            // Set copy's data to that of node.
            let data = node.read(main_thread).data().to_utf16_string();
            create_text(main_thread, document, data)
        }
        // NB: A CDATASection node is a Text node, whose copy is one too.
        NodeClassId::CDATASection => {
            let data = node.read(main_thread).data().to_utf16_string();
            create_cdata_section(main_thread, document, data)
        }
        // -> Comment
        NodeClassId::Comment => {
            // Set copy's data to that of node.
            let data = node.read(main_thread).data().to_utf16_string();
            create_comment(main_thread, document, data)
        }
        // -> ProcessingInstruction
        NodeClassId::ProcessingInstruction => {
            // Set copy's target and data to those of node.
            let target = node
                .processing_instruction_core()
                .expect("the node is a processing instruction")
                .target()
                .to_fly_string();
            let data = node.read(main_thread).data().to_utf16_string();
            create_processing_instruction(main_thread, document, data, target)
        }
        // -> Attr
        NodeClassId::Attr => {
            // Set copy's namespace, namespace prefix, local name, and value to those of node.
            let core = node.attr_core().expect("the node is an Attr node");
            let name = attributes::hold_qualified_name(core.qualified_name().parts());
            let value = attr_value(main_thread, core);
            create_attr(main_thread, document, name, value, None)
        }
        _ => return None,
    };
    // 7. Return copy.
    Some(copy)
}

/// The prefix of a new Text node of the class `class_id`, whose C++ class has the vtable pointer of `words` and the
/// SlottableMixin vtable pointer `slottable_mixin_vtable`.
#[inline(always)]
fn text_prefix(
    words: CxxWords,
    slottable_mixin_vtable: *const c_void,
    class_id: NodeClassId,
    node_type: NodeType,
    node: NodePointer,
    document: NodePointer,
    data: ak::Utf16String,
) -> TextPrefix {
    TextPrefix::new(
        CharacterDataPrefix {
            node: NodePrefix::new(words, class_id, node_type, node, document),
            core: CharacterDataCore::new(data),
        },
        slottable_mixin_vtable.addr() as u64,
    )
}

/// A new Text node with the node document `document` and the data `data`.
pub(super) fn create_text(main_thread: &mut MainThread<'_>, document: NodeRef, data: ak::Utf16String) -> NodeRef {
    let objects = node_objects();
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.text, |node| TextObject {
        text: text_prefix(
            objects.words(objects.hooks.text_vtable),
            objects.hooks.text_slottable_mixin_vtable,
            NodeClassId::Text,
            NodeType::TEXT_NODE,
            node,
            document_pointer,
            data,
        ),
    })
}

/// A new CDATASection node with the node document `document` and the data `data`.
pub(super) fn create_cdata_section(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    data: ak::Utf16String,
) -> NodeRef {
    let objects = node_objects();
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.cdata_section, |node| {
        CDATASectionObject {
            text: text_prefix(
                objects.words(objects.hooks.cdata_section_vtable),
                objects.hooks.cdata_section_slottable_mixin_vtable,
                NodeClassId::CDATASection,
                NodeType::CDATA_SECTION_NODE,
                node,
                document_pointer,
                data,
            ),
        }
    })
}

/// A new Comment node with the node document `document` and the data `data`.
pub(super) fn create_comment(main_thread: &mut MainThread<'_>, document: NodeRef, data: ak::Utf16String) -> NodeRef {
    let objects = node_objects();
    let words = objects.words(objects.hooks.comment_vtable);
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.comment, |node| CommentObject {
        character_data: CharacterDataPrefix {
            node: NodePrefix::new(
                words,
                NodeClassId::Comment,
                NodeType::COMMENT_NODE,
                node,
                document_pointer,
            ),
            core: CharacterDataCore::new(data),
        },
    })
}

/// A new ProcessingInstruction node with the node document `document`, the data `data` and the target `target`.
pub(super) fn create_processing_instruction(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    data: ak::Utf16String,
    target: ak::Utf16FlyString,
) -> NodeRef {
    let objects = node_objects();
    let words = objects.words(objects.hooks.processing_instruction_vtable);
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.processing_instruction, |node| {
        ProcessingInstructionObject {
            character_data: CharacterDataPrefix {
                node: NodePrefix::new(
                    words,
                    NodeClassId::ProcessingInstruction,
                    NodeType::PROCESSING_INSTRUCTION_NODE,
                    node,
                    document_pointer,
                ),
                core: CharacterDataCore::new(data),
            },
            core: ProcessingInstructionCore::new(target),
        }
    })
}

/// A new DocumentType node with the node document `document`, and the name, public ID and system ID given.
pub(super) fn create_document_type(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    name: ak::Utf16FlyString,
    public_id: ak::Utf16String,
    system_id: ak::Utf16String,
) -> NodeRef {
    let objects = node_objects();
    let words = objects.words(objects.hooks.document_type_vtable);
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.document_type, |node| {
        DocumentTypeObject {
            node: NodePrefix::new(
                words,
                NodeClassId::DocumentType,
                NodeType::DOCUMENT_TYPE_NODE,
                node,
                document_pointer,
            ),
            core: DocumentTypeCore::new(name, public_id, system_id),
        }
    })
}

/// A new Attr node with the node document `document`, named `name`, with the value `value` and the element
/// `owner_element`, whose references pass to it.
pub(super) fn create_attr(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    name: NonNull<QualifiedNameImpl>,
    value: ak::Utf16String,
    owner_element: Option<NodePointer>,
) -> NodeRef {
    let objects = node_objects();
    let words = objects.words(objects.hooks.attr_vtable);
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.attr, |node| AttrObject {
        attr: AttrPrefix {
            node: NodePrefix::new(
                words,
                NodeClassId::Attr,
                NodeType::ATTRIBUTE_NODE,
                node,
                document_pointer,
            ),
            core: AttrCore::new(name, value, owner_element),
        },
    })
}

/// A new DocumentFragment node with the node document `document`, which has no host.
pub(super) fn create_document_fragment(main_thread: &mut MainThread<'_>, document: NodeRef) -> NodeRef {
    let objects = node_objects();
    let words = objects.words(objects.hooks.document_fragment_vtable);
    let document_pointer = document.pointer();
    allocate(main_thread, document, &objects.document_fragment, |node| {
        DocumentFragmentObject {
            document_fragment: DocumentFragmentPrefix {
                parent_node: ParentNodePrefix::new(NodePrefix::new(
                    words,
                    NodeClassId::DocumentFragment,
                    NodeType::DOCUMENT_FRAGMENT_NODE,
                    node,
                    document_pointer,
                )),
                core: DocumentFragmentCore::new(),
            },
        }
    })
}

/// The prefix of a new shadow root with the node document `document` and the host `host`, which is closed if `closed`,
/// over which C++ then constructs the payload of DOM::ShadowRoot, as DOM::ShadowRoot::create() does.
pub(super) fn create_shadow_root(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    host: NodeRef,
    closed: bool,
) -> NodeRef {
    let objects = node_objects();
    // NB: The constructor of DOM::ShadowRoot writes the vtable pointer.
    let words = objects.words(core::ptr::null());
    let document_pointer = document.pointer();
    let host_pointer = host.pointer();
    allocate(main_thread, document, &objects.shadow_root, |node| {
        let node_prefix = NodePrefix::new(
            words,
            NodeClassId::ShadowRoot,
            NodeType::DOCUMENT_FRAGMENT_NODE,
            node,
            document_pointer,
        );
        if closed {
            node_prefix.core.set_initial_flags(NODE_IS_CLOSED_SHADOW_ROOT);
        }
        ShadowRootObject {
            shadow_root: ShadowRootPrefix {
                document_fragment: DocumentFragmentPrefix {
                    parent_node: ParentNodePrefix::new(node_prefix),
                    core: DocumentFragmentCore::with_host(host_pointer),
                },
                core: ShadowRootCore::new(),
            },
        }
    })
}

/// A new Text node with the node document `document` and the data `data`, whose reference passes to it, for
/// DOM::Text::create().
///
/// # Safety
///
/// `document` must be a live document, `data` the raw word of an AK::Utf16String whose reference the caller hands
/// over, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_text(document: &NodeCell, data: usize) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let (document, data) = unsafe {
        (
            NodeRef::from_cell(document, &main_thread),
            ak::Utf16String::from_raw_owned(data),
        )
    };
    let node = create_text(&mut main_thread, document, data);
    NonNull::from(node.cell())
}

/// A new CDATASection node with the node document `document` and the data `data`, whose reference passes to it, for
/// DOM::CDATASection::create().
///
/// # Safety
///
/// As for rust_dom_create_text().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_cdata_section(document: &NodeCell, data: usize) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let (document, data) = unsafe {
        (
            NodeRef::from_cell(document, &main_thread),
            ak::Utf16String::from_raw_owned(data),
        )
    };
    let node = create_cdata_section(&mut main_thread, document, data);
    NonNull::from(node.cell())
}

/// A new Comment node with the node document `document` and the data `data`, whose reference passes to it, for
/// DOM::Comment::create().
///
/// # Safety
///
/// `document` must be a live document, `data` the raw word of an AK::Utf16String whose reference the caller hands
/// over, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_comment(document: &NodeCell, data: usize) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let (document, data) = unsafe {
        (
            NodeRef::from_cell(document, &main_thread),
            ak::Utf16String::from_raw_owned(data),
        )
    };
    let node = create_comment(&mut main_thread, document, data);
    NonNull::from(node.cell())
}

/// A new ProcessingInstruction node with the node document `document`, the data `data` and the target `target`, whose
/// references pass to it, for DOM::ProcessingInstruction::create().
///
/// # Safety
///
/// `document` must be a live document, `data` the raw word of an AK::Utf16String and `target` that of an
/// AK::Utf16FlyString whose references the caller hands over, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_processing_instruction(
    document: &NodeCell,
    data: usize,
    target: usize,
) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let (document, data, target) = unsafe {
        (
            NodeRef::from_cell(document, &main_thread),
            ak::Utf16String::from_raw_owned(data),
            ak::Utf16FlyString::from_raw_owned(target),
        )
    };
    let node = create_processing_instruction(&mut main_thread, document, data, target);
    NonNull::from(node.cell())
}

/// A new DocumentType node with the node document `document`, whose name, public ID and system ID are empty, for
/// DOM::DocumentType::create().
///
/// # Safety
///
/// `document` must be a live document, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_document_type(document: &NodeCell) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The document is live.
    let document = unsafe { NodeRef::from_cell(document, &main_thread) };
    let node = create_document_type(
        &mut main_thread,
        document,
        ak::Utf16FlyString::default(),
        ak::Utf16String::default(),
        ak::Utf16String::default(),
    );
    NonNull::from(node.cell())
}

/// A new Attr node with the node document `document`, named `name`, with the value `value`, whose reference passes to
/// it, and the element `owner_element`, or none, for DOM::Attr::create().
///
/// # Safety
///
/// `document` must be a live document, `name` live parts of a qualified name, `value` the raw word of an
/// AK::Utf16String whose reference the caller hands over, `owner_element` a live element or null, and the call must be
/// made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_attr(
    document: &NodeCell,
    name: &QualifiedNameImpl,
    value: usize,
    owner_element: Option<&NodeCell>,
) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let (document, value) = unsafe {
        (
            NodeRef::from_cell(document, &main_thread),
            ak::Utf16String::from_raw_owned(value),
        )
    };
    let owner_element = owner_element.map(compress);
    let name = attributes::hold_qualified_name(name);
    let node = create_attr(&mut main_thread, document, name, value, owner_element);
    NonNull::from(node.cell())
}

/// A new DocumentFragment node with the node document `document`, which has no host, for DOM::DocumentFragment::create().
///
/// # Safety
///
/// `document` must be a live document, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_document_fragment(document: &NodeCell) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The document is live.
    let document = unsafe { NodeRef::from_cell(document, &main_thread) };
    let node = create_document_fragment(&mut main_thread, document);
    NonNull::from(node.cell())
}

/// The prefix of a new shadow root with the node document `document` and the host `host`, which is closed if `closed`,
/// for DOM::ShadowRoot::create(), which then constructs the payload of DOM::ShadowRoot over it.
///
/// # Safety
///
/// `document` must be a live document, `host` a live element, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_shadow_root(
    document: &NodeCell,
    host: &NodeCell,
    closed: bool,
) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let (document, host) = unsafe {
        (
            NodeRef::from_cell(document, &main_thread),
            NodeRef::from_cell(host, &main_thread),
        )
    };
    let node = create_shadow_root(&mut main_thread, document, host, closed);
    NonNull::from(node.cell())
}

// NB: The C++ classes assert that they have the sizes and alignments of these.
const _: () = assert!(size_of::<TextObject>() == 128 && align_of::<TextObject>() == 8);
const _: () = assert!(size_of::<CDATASectionObject>() == 128 && align_of::<CDATASectionObject>() == 8);
const _: () = assert!(size_of::<CommentObject>() == 104 && align_of::<CommentObject>() == 8);
const _: () =
    assert!(size_of::<ProcessingInstructionObject>() == 112 && align_of::<ProcessingInstructionObject>() == 8);
const _: () = assert!(size_of::<DocumentTypeObject>() == 120 && align_of::<DocumentTypeObject>() == 8);
const _: () = assert!(size_of::<AttrObject>() == 120 && align_of::<AttrObject>() == 8);
const _: () = assert!(size_of::<DocumentFragmentObject>() == 120 && align_of::<DocumentFragmentObject>() == 8);
const _: () = assert!(size_of::<ShadowRootObject>() == 168 && align_of::<ShadowRootObject>() == 8);
