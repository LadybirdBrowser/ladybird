/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The classes of elements whose objects Rust defines whole: those DOM/NodeClasses.json marks "@rust_owned", which are
//! DOM::Element, HTML::HTMLElement and MathML::MathMLElement, the layout classes, and classes that add no member to the
//! class they derive from, which DOM/ElementObjects.cpp checks. Rust lays out an element of such a class as the object
//! of its layout class, an ElementObject, HTMLElementObject or MathMLElementObject, allocates it from LibGC's heap with
//! an allocator of the class's own, traces it, finalizes it and destroys it. C++ keeps the class as a shell over the
//! object, as for the nodes of node_objects.rs.
//!
//! The members C++ keeps in the opaque slots of an element's prefixes, its rare data among them, start out empty, and
//! C++ only reaches into them for an element that has needed them. Rust compares the slots with what they start out as,
//! and only then calls into C++: to visit the cells they hold, to measure the memory they hold, to finalize them, and to
//! destroy them. Rust destroys what the element's core holds itself. An element that C++ has nothing to visit for may
//! be traced on any thread, see traced_on_owner_thread().

use core::ffi::{c_char, c_void};
use core::ptr::NonNull;

use libgc_api::capi::{self, CellTypeInfo, GCVisitor};
use libgc_api::cell::{self, Allocator, RustCell};
use libgc_api::{Trace, Visitor};

use super::attributes;
use super::attributes::HTML_NAMESPACE;
use super::element_core::ELEMENT_USES_DOCUMENT_GLOBAL_CUSTOM_ELEMENT_REGISTRY;
use super::element_core::ElementCore;
use super::node::NodeCell;
use super::node_class::NodeClassId;
use super::node_class::RUST_OWNED_ELEMENT_CLASS_INDICES;
use super::node_class::RUST_OWNED_ELEMENT_CLASSES;
use super::node_core::NodeType;
use super::node_objects::allocate;
use super::node_ref::NodeRef;
use super::prefix::CUSTOM_ELEMENT_STATE_UNCUSTOMIZED;
use super::prefix::CUSTOM_ELEMENT_STATE_UNDEFINED;
use super::prefix::CxxWords;
use super::prefix::ElementPrefix;
use super::prefix::HTMLElementPrefix;
use super::prefix::MathMLElementPrefix;
use super::prefix::NodePrefix;
use super::prefix::ParentNodePrefix;
use super::qualified_name::QualifiedNameImpl;
use super::strings::units_contain_ascii;
use super::strings::units_equal_ascii;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// The number of classes of elements whose objects Rust defines.
const CLASS_COUNT: usize = RUST_OWNED_ELEMENT_CLASSES.len();

/// The vtable pointers C++ writes into an element of a class whose objects Rust defines, as it would construct one:
/// that of the class, and those of the bases of the class's layout class that have virtual functions.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct ElementClassVtables {
    pub vtable: *const c_void,
    pub slottable_mixin: *const c_void,
    pub aria_mixin: *const c_void,
    pub animatable: *const c_void,
    /// That of the HTML::GlobalEventHandlers base of an HTML or MathML element, or null.
    pub global_event_handlers: *const c_void,
    /// That of the HTML::FormAssociatedElement base of an HTML element, or null.
    pub form_associated_element: *const c_void,
    /// An element of the class that C++ constructs, but never as a node, against which Rust checks that it makes an
    /// element of the class as C++ would.
    pub probe: *const c_void,
}

/// What C++ registers for the elements whose objects Rust defines.
#[repr(C)]
pub struct ElementObjectHooks {
    /// The vtable pointers of each class, in the order of RUST_OWNED_ELEMENT_CLASSES.
    pub classes: *const ElementClassVtables,
    pub class_count: usize,
    /// Visits the cells the members in the opaque slots of `element` hold: what DOM::Element::visit_edges() visits
    /// beyond what Rust traces. Runs on the main thread, while the collector marks or gathers a graph of the heap.
    pub visit_slot_edges: unsafe extern "C" fn(element: &NodeCell, visitor: *mut GCVisitor),
    /// Runs DOM::Element::finalize() on `element`, which the collector found dead.
    pub finalize: unsafe extern "C" fn(element: &NodeCell),
    /// DOM::Element::external_memory_size() of `element`, which includes what Rust measures.
    pub external_memory_size: unsafe extern "C" fn(element: &NodeCell) -> usize,
    /// Runs the destructor of `element`'s class on it, once Rust has released what the element's core holds.
    pub destroy: unsafe extern "C" fn(element: &NodeCell),
}

/// A class of elements whose objects Rust defines: its vtable pointers and its allocator.
struct ElementClass {
    vtables: ElementClassVtables,
    allocator: Allocator,
}

/// The hooks C++ registers, and the classes, which live for the rest of the process.
struct ElementObjects {
    hooks: &'static ElementObjectHooks,
    null_weak_impl: u64,
    classes: [ElementClass; CLASS_COUNT],
}

/// The registered state, which C++ sets once, before any node exists, and never changes after that.
#[repr(transparent)]
struct RegisteredElementObjects(core::cell::UnsafeCell<Option<ElementObjects>>);

// SAFETY: The state is set once, before any node exists, and only the main thread, which makes, finalizes and destroys
//         every element, and traces those whose slots C++ has edges in, reads it.
unsafe impl Sync for RegisteredElementObjects {}

static ELEMENT_OBJECTS: RegisteredElementObjects = RegisteredElementObjects(core::cell::UnsafeCell::new(None));

#[inline(always)]
fn element_objects() -> &'static ElementObjects {
    // SAFETY: Nothing writes the state after the first node exists, see RegisteredElementObjects.
    unsafe { (*ELEMENT_OBJECTS.0.get()).as_ref() }
        .expect("the element object hooks are registered before any node exists")
}

/// The type of the object of a layout class, which starts with the class's prefix.
///
/// # Safety
///
/// The type must be #[repr(C)], start with an ElementPrefix, and have the size and alignment of the C++ layout class,
/// which asserts them.
unsafe trait ElementLayoutObject: RustCell {
    #[inline(always)]
    fn element(&self) -> &ElementPrefix {
        // SAFETY: The object starts with its ElementPrefix.
        unsafe { &*core::ptr::from_ref(self).cast::<ElementPrefix>() }
    }

    #[inline(always)]
    fn node(&self) -> &NodeCell {
        // SAFETY: The object starts with its NodePrefix, so it is a node.
        unsafe { &*core::ptr::from_ref(self).cast::<NodeCell>() }
    }
}

/// Implements what every object type has alike: its Trace, which traces its fields and then has C++ visit the slots
/// that hold cells, its RustCell and its Drop.
macro_rules! element_object {
    ($object:ident, $name:literal, $field:ident) => {
        // SAFETY: Traces every field, and has C++ visit the members of the opaque slots where they hold cells.
        unsafe impl Trace for $object {
            #[inline(always)]
            fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
                self.$field.trace(visitor);
                visit_slot_edges(self, visitor);
            }
        }

        // SAFETY: The object is #[repr(C)], starts with the header of its cell, which its WrappablePrefix lays out, and
        //         is the size and alignment of the C++ class, which fits a cell.
        unsafe impl RustCell for $object {
            const NAME: &'static str = $name;
        }

        impl Drop for $object {
            fn drop(&mut self) {
                // SAFETY: LibGC destroys the object once, on the main thread, and nothing reads it after this.
                unsafe { destroy(self) }
            }
        }
    };
}

/// A whole element of a class whose layout class is DOM::Element.
/// https://dom.spec.whatwg.org/#interface-element
#[repr(C)]
pub struct ElementObject {
    element: ElementPrefix,
}

element_object!(ElementObject, "Element", element);

// SAFETY: The object starts with its ElementPrefix, and ElementObjects.cpp asserts its size and alignment.
unsafe impl ElementLayoutObject for ElementObject {}

/// A whole element of a class whose layout class is HTML::HTMLElement.
/// https://html.spec.whatwg.org/multipage/dom.html#htmlelement
#[repr(C)]
pub struct HTMLElementObject {
    html_element: HTMLElementPrefix,
}

element_object!(HTMLElementObject, "HTMLElement", html_element);

// SAFETY: The object starts with its ElementPrefix, and ElementObjects.cpp asserts its size and alignment.
unsafe impl ElementLayoutObject for HTMLElementObject {}

/// A whole element of a class whose layout class is MathML::MathMLElement.
/// https://w3c.github.io/mathml-core/#dom-mathmlelement
#[repr(C)]
pub struct MathMLElementObject {
    mathml_element: MathMLElementPrefix,
}

element_object!(MathMLElementObject, "MathMLElement", mathml_element);

// SAFETY: The object starts with its ElementPrefix, and ElementObjects.cpp asserts its size and alignment.
unsafe impl ElementLayoutObject for MathMLElementObject {}

/// Has C++ visit the cells the members in the opaque slots of `object` hold, if they hold any.
#[inline(always)]
fn visit_slot_edges<T: ElementLayoutObject>(object: &T, visitor: &mut Visitor) {
    // SAFETY: LibGC traces live elements, which C++ does not change meanwhile.
    if !unsafe { object.element().has_cxx_edges() } {
        return;
    }
    let Some(raw) = visitor.as_raw() else {
        return;
    };
    // SAFETY: LibGC traces an element whose slots hold cells on the main thread only, see traced_on_owner_thread(), with
    //         a visitor that outlives the call.
    unsafe { (element_objects().hooks.visit_slot_edges)(object.node(), raw.as_ptr()) }
}

/// Destroys `object`: releases what its core holds, and has C++ run its destructor where a member in its opaque slots
/// holds anything to release.
///
/// # Safety
///
/// LibGC must be destroying the object, on the main thread, and nothing may read it after this.
#[inline(always)]
unsafe fn destroy<T: ElementLayoutObject>(object: &T) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    let element = object.element();
    // SAFETY: Guaranteed by the caller. The destructor of DOM::Element takes the core as release() leaves it.
    unsafe {
        element.core.release(&mut main_thread);
        let objects = element_objects();
        if element.needs_cxx_destroy(objects.null_weak_impl) {
            (objects.hooks.destroy)(object.node());
        }
    }
}

/// The prefix of the element whose cell starts at `cell`.
///
/// # Safety
///
/// `cell` must be the start of a live element of a class whose objects Rust defines, which lives for `'a`.
#[inline(always)]
unsafe fn element_at<'a>(cell: *const c_void) -> &'a ElementPrefix {
    // SAFETY: Guaranteed by the caller. Every element starts with an ElementPrefix.
    unsafe { &*cell.cast::<ElementPrefix>() }
}

/// Whether LibGC must trace the element at `cell` on the thread that owns its heap: where C++ has cells to visit or
/// memory to measure in its opaque slots, which only that thread may do. Any thread may trace the rest, as tracing them
/// only reads Rust's fields.
///
/// # Safety
///
/// Only LibGC calls it, with a live element of a class whose objects Rust defines, while no thread changes it.
unsafe extern "C" fn traced_on_owner_thread(cell: *const c_void) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        let element = element_at(cell);
        element.has_cxx_edges() || element.has_cxx_external_memory()
    }
}

/// # Safety
///
/// Only LibGC calls it, with a dead element of a class whose objects Rust defines, on the main thread.
unsafe extern "C" fn finalize(cell: *mut c_void) {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        if element_at(cell).needs_cxx_finalize() {
            (element_objects().hooks.finalize)(&*cell.cast::<NodeCell>());
        }
    }
}

/// The size of the memory outside the heap that the element at `cell` holds: that of its attributes, which Rust
/// measures, unless C++ holds memory in its opaque slots, which C++ measures with the rest.
///
/// # Safety
///
/// Only LibGC calls it, with a live element of a class whose objects Rust defines, on the main thread for an element
/// for which traced_on_owner_thread() holds, and while no thread changes it.
unsafe extern "C" fn external_memory_size(cell: *const c_void) -> usize {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        let element = element_at(cell);
        if element.has_cxx_external_memory() {
            return (element_objects().hooks.external_memory_size)(&*cell.cast::<NodeCell>());
        }
        element.core.attribute_list().external_memory_size()
    }
}

/// The name of the class of the element at `cell`.
///
/// # Safety
///
/// Only LibGC calls it, with a live element of a class whose objects Rust defines, and somewhere to store the length.
unsafe extern "C" fn class_name(cell: *const c_void, length: *mut usize) -> *const c_char {
    // SAFETY: Guaranteed by the caller.
    let class_id = unsafe { NodeClassId::of_cell(cell) };
    let (_, _, name) = RUST_OWNED_ELEMENT_CLASSES[usize::from(RUST_OWNED_ELEMENT_CLASS_INDICES[class_id.index()])];
    // SAFETY: Guaranteed by the caller.
    unsafe { length.write(name.len()) };
    name.as_ptr().cast()
}

/// The type info of the objects of `T`: LibGC traces them with T's Trace, which has C++ visit the opaque slots where
/// they hold cells, finalizes and measures them, and destroys them by dropping them in place.
const fn type_info_of<T: ElementLayoutObject>() -> CellTypeInfo {
    CellTypeInfo {
        finalize: Some(finalize),
        external_memory_size: Some(external_memory_size),
        class_name: Some(class_name),
        traced_on_owner_thread: Some(traced_on_owner_thread),
        ..cell::type_info_of::<T>(None)
    }
}

/// The type info of each class, by its index in RUST_OWNED_ELEMENT_CLASSES: that of the objects of its layout class.
/// Each class has a type info of its own, whose address keeps the blocks of the class apart from those of the others.
const fn type_infos() -> [CellTypeInfo; CLASS_COUNT] {
    let mut type_infos = [type_info_of::<ElementObject>(); CLASS_COUNT];
    let mut index = 0;
    while index < CLASS_COUNT {
        let layout = RUST_OWNED_ELEMENT_CLASSES[index].1.index();
        type_infos[index] = if layout == NodeClassId::HTMLElement.index() {
            type_info_of::<HTMLElementObject>()
        } else if layout == NodeClassId::MathMLElement.index() {
            type_info_of::<MathMLElementObject>()
        } else {
            assert!(layout == NodeClassId::Element.index());
            type_info_of::<ElementObject>()
        };
        index += 1;
    }
    type_infos
}

static ELEMENT_TYPE_INFOS: [CellTypeInfo; CLASS_COUNT] = type_infos();

/// Registers the hooks of the elements whose objects Rust defines, checks the probe of each class against what Rust
/// makes, and makes the allocators of the classes.
///
/// # Safety
///
/// No node may exist yet, and the call must be made on the main thread, before any other thread starts. The probes
/// must be live elements of their classes, which C++ constructed and never changes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_register_element_objects(hooks: &'static ElementObjectHooks) {
    assert_eq!(
        hooks.class_count, CLASS_COUNT,
        "C++ registers every class of RUST_OWNED_ELEMENT_CLASSES"
    );
    // NB: The null impl counts no references, so every null GC::Weak can hold it without taking one.
    let null_weak_impl = capi::gc_weak_impl_null().addr() as u64;
    // SAFETY: Guaranteed by the caller.
    let vtables = unsafe { ak::slice_from_raw_parts(hooks.classes, CLASS_COUNT) };
    let classes = core::array::from_fn(|index| {
        let vtables = vtables[index];
        let (class_id, layout, name) = RUST_OWNED_ELEMENT_CLASSES[index];
        // SAFETY: Guaranteed by the caller.
        assert!(
            unsafe { probe_holds_new_slots(&vtables, layout) },
            "Rust makes an element of class {name} as C++ constructs one"
        );
        // SAFETY: The probe is a live element of the class.
        assert!(unsafe { NodeClassId::of_cell(vtables.probe) } == class_id);
        let type_info = &ELEMENT_TYPE_INFOS[index];
        // SAFETY: The allocator lives for the rest of the process, as the heaps it allocates from may, and the address of
        //         the type info, which lives as long, is unique to the class. Any thread may trace its cells, see
        //         traced_on_owner_thread().
        let allocator = unsafe { Allocator::new(type_info, name, core::ptr::from_ref(type_info).cast()) };
        ElementClass { vtables, allocator }
    });
    // SAFETY: Guaranteed by the caller.
    unsafe {
        *ELEMENT_OBJECTS.0.get() = Some(ElementObjects {
            hooks,
            null_weak_impl,
            classes,
        });
    }
}

/// Whether the probe of `vtables` holds in the slots of its classes, and in its core but for the name, what Rust writes
/// into a new element of the class, whose layout class is `layout`.
///
/// # Safety
///
/// The probe must be a live element of the class, which C++ constructed and does not change.
unsafe fn probe_holds_new_slots(vtables: &ElementClassVtables, layout: NodeClassId) -> bool {
    let word = |pointer: *const c_void| pointer.addr() as u64;
    // SAFETY: Guaranteed by the caller. The probe starts with the prefix of its layout class.
    unsafe {
        let element = element_at(vtables.probe.cast());
        let element_holds_new_slots = element.core.holds_new_fields_but_name()
            && element.holds_new_slots(
                word(vtables.slottable_mixin),
                word(vtables.aria_mixin),
                word(vtables.animatable),
            );
        element_holds_new_slots
            && match layout {
                NodeClassId::HTMLElement => (*vtables.probe.cast::<HTMLElementPrefix>()).holds_new_slots(
                    word(vtables.global_event_handlers),
                    word(vtables.form_associated_element),
                ),
                NodeClassId::MathMLElement => {
                    vtables.form_associated_element.is_null()
                        && (*vtables.probe.cast::<MathMLElementPrefix>())
                            .holds_new_slots(word(vtables.global_event_handlers))
                }
                _ => vtables.global_event_handlers.is_null() && vtables.form_associated_element.is_null(),
            }
    }
}

/// A new element of the class at `index` in RUST_OWNED_ELEMENT_CLASSES, with the node document `document`, named
/// `name`, whose reference passes to it, with the ELEMENT_* flags `flags` and the custom element state
/// `custom_element_state`, and otherwise as the constructors of the class make it.
pub(super) fn create_element(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    index: usize,
    name: NonNull<QualifiedNameImpl>,
    flags: u8,
    custom_element_state: u8,
) -> NodeRef {
    let objects = element_objects();
    let class = &objects.classes[index];
    let vtables = &class.vtables;
    let (class_id, layout, _) = RUST_OWNED_ELEMENT_CLASSES[index];
    let words = CxxWords {
        vtable: vtables.vtable.addr() as u64,
        null_weak_impl: objects.null_weak_impl,
    };
    let document_pointer = document.pointer();
    let element = move |node| {
        let element = ElementPrefix::new(
            ParentNodePrefix::new(NodePrefix::new(
                words,
                class_id,
                NodeType::ELEMENT_NODE,
                node,
                document_pointer,
            )),
            vtables.slottable_mixin.addr() as u64,
            vtables.aria_mixin.addr() as u64,
            vtables.animatable.addr() as u64,
            ElementCore::new(name, flags),
        );
        element.custom_element_state.set(custom_element_state);
        element
    };
    match layout {
        NodeClassId::HTMLElement => allocate(main_thread, document, &class.allocator, |node| HTMLElementObject {
            html_element: HTMLElementPrefix::new(
                element(node),
                vtables.global_event_handlers.addr() as u64,
                vtables.form_associated_element.addr() as u64,
            ),
        }),
        NodeClassId::MathMLElement => allocate(main_thread, document, &class.allocator, |node| MathMLElementObject {
            mathml_element: MathMLElementPrefix::new(element(node), vtables.global_event_handlers.addr() as u64),
        }),
        _ => allocate(main_thread, document, &class.allocator, |node| ElementObject {
            element: element(node),
        }),
    }
}

/// The copy that "clone a single node" makes of the element `node` into `document`, but for its attributes, where Rust
/// can make it without C++: where the class of `node` is one whose objects Rust defines, and the element holds nothing
/// in the slots C++ keeps that cloning reads or that the cloning steps of other specifications copy. Otherwise none, and
/// C++ makes the copy. `uses_fallback_registry` says whether the clone has a fallback registry, which is non-null.
/// https://dom.spec.whatwg.org/#clone-a-single-node
pub(super) fn clone_element(
    main_thread: &mut MainThread<'_>,
    document: NodeRef,
    node: NodeRef,
    uses_fallback_registry: bool,
) -> Option<NodeRef> {
    let index = RUST_OWNED_ELEMENT_CLASS_INDICES[node.class_id().index()];
    if index == u8::MAX {
        return None;
    }
    // SAFETY: The node is a live element of a class whose objects Rust defines, which C++ does not change meanwhile.
    let element = unsafe { element_at(core::ptr::from_ref(node.cell()).cast()) };
    // NB: The is value of an element, its custom element registry where that is no global one, and its nonce, which
    //     the cloning steps of HTML copy, live in its rare data. Without declarations in the style attribute of the
    //     element, the copy takes none from it as it runs its cloning steps.
    // SAFETY: As above.
    if element.custom_element_state.get() != CUSTOM_ELEMENT_STATE_UNCUSTOMIZED
        || unsafe { element.has_rare_data() || element.has_inline_style() }
    {
        return None;
    }
    let name = element.core.qualified_name();
    // NB: Creating an element in the HTML namespace whose local name is a valid custom element name looks a definition
    //     up, and gives the element another state. Such a name contains a hyphen.
    let is_html = name
        .namespace()
        .is_some_and(|namespace| units_equal_ascii(namespace.units(), HTML_NAMESPACE));
    if is_html && units_contain_ascii(name.local_name().units(), b'-') {
        return None;
    }

    // 1. Let registry be node's custom element registry.
    // 2. If registry is null, then set registry to fallbackRegistry.
    // 3. If registry is a global custom element registry, then set registry to document's effective global custom
    //    element registry.
    // NB: The registry of an element without rare data is null, or its node document's registry, which is then global.
    //     Its copy's registry is then the document's registry where that is global, and null otherwise, unless the
    //     fallback registry stands in for a null one.
    let uses_document_global_registry =
        element.core.flags() & ELEMENT_USES_DOCUMENT_GLOBAL_CUSTOM_ELEMENT_REGISTRY != 0;
    if !uses_document_global_registry && uses_fallback_registry {
        return None;
    }
    let flags = if uses_document_global_registry && document.document_core().custom_element_registry_is_global() {
        ELEMENT_USES_DOCUMENT_GLOBAL_CUSTOM_ELEMENT_REGISTRY
    } else {
        0
    };

    // 4. Set copy to the result of creating an element, given document, node's local name, node's namespace, node's
    //    namespace prefix, node's is value, false, and registry.
    // NB: Without a definition, which only a name with a hyphen or an is value looks up, creating the element gives it
    //     the interface that the class of node is, and the state "uncustomized".
    let name = attributes::hold_qualified_name(name.parts());
    Some(create_element(
        main_thread,
        document,
        usize::from(index),
        name,
        flags,
        CUSTOM_ELEMENT_STATE_UNCUSTOMIZED,
    ))
}

/// A new element of the class `class_id`, whose objects Rust defines, with the node document `document`, named `name`,
/// as the constructors of the class make it, for the C++ element factory.
///
/// # Safety
///
/// `document` must be a live document, `name` live parts of a qualified name, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_create_element(
    document: &NodeCell,
    class_id: NodeClassId,
    name: &QualifiedNameImpl,
) -> NonNull<NodeCell> {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Guaranteed by the caller.
    let document = unsafe { NodeRef::from_cell(document, &main_thread) };
    let index = RUST_OWNED_ELEMENT_CLASS_INDICES[class_id.index()];
    debug_assert!(index != u8::MAX, "Rust defines the objects of the class");
    let name = attributes::hold_qualified_name(name);
    let element = create_element(
        &mut main_thread,
        document,
        usize::from(index),
        name,
        0,
        CUSTOM_ELEMENT_STATE_UNDEFINED,
    );
    NonNull::from(element.cell())
}

// NB: The C++ layout classes assert that they have the sizes and alignments of these.
const _: () = assert!(size_of::<ElementObject>() == 296 && align_of::<ElementObject>() == 8);
const _: () = assert!(size_of::<HTMLElementObject>() == 320 && align_of::<HTMLElementObject>() == 8);
const _: () = assert!(size_of::<MathMLElementObject>() == 304 && align_of::<MathMLElementObject>() == 8);
