/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the runtime's garbage-collected objects and linear memories live: in the embedder's LibGC heap and primitive
//! storage, reached through the functions of Libraries/LibGC/CAPI.h that the embedder hands over in a `GcInterface`,
//! or, for a store without one (as in this crate's tests), in plain allocations that are never freed and mappings of
//! their own.
//!
//! Cells start with a word that belongs to the runtime and a word that holds LibGC's mark, state, and kind. GC objects
//! (see gc.rs) are cells; so are runtime types, exceptions, and the cell that roots a store. The embedder's thread
//! stack, which holds the interpreter's frames, is scanned conservatively, and LibGC recognizes pointers into a cell,
//! so references with tags in their low bits keep their cells alive there too.

use core::ffi::{c_char, c_void};
use std::sync::OnceLock;

/// Mirrors GCCellTypeInfo.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct CellTypeInfo {
    pub cell_size: u32,
    pub alignment: u32,
    pub kind: u8,
    pub visit_edges: unsafe extern "C" fn(cell: *mut c_void, visitor: *mut c_void),
    pub finalize: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    pub destroy: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    pub external_memory_size: Option<unsafe extern "C" fn(cell: *const c_void) -> usize>,
    pub class_name: Option<unsafe extern "C" fn(cell: *const c_void, length: *mut usize) -> *const c_char>,
}

/// Mirrors GCLayout.
#[derive(Default, Clone, Copy)]
#[repr(C)]
pub struct GcLayout {
    pub cell_mark_offset: u32,
    pub cell_state_offset: u32,
    pub cell_kind_offset: u32,
    pub min_cell_size: u32,
    pub max_cell_size: u32,
    pub max_cell_alignment: u32,
    pub cell_type_info_size: u32,
    pub weak_impl_pointer_offset: u32,
    pub free_cell_next_offset: u32,
    pub free_cell_link_mask: u32,
    pub local_cursor_offset: u32,
    pub local_end_offset: u32,
    pub local_cell_size_offset: u32,
    pub heap_allocated_bytes_since_last_gc_offset: u32,
    pub heap_gc_bytes_threshold_offset: u32,
    pub heap_total_allocated_bytes_offset: u32,
    pub heap_region_offset_mask: u64,
    pub primitive_storage_cage_offset_mask: u64,
}

/// Mirrors GCPrimitiveStorageLayout.
#[derive(Default, Clone, Copy)]
#[repr(C)]
pub struct PrimitiveStorageLayout {
    pub offset: usize,
    pub size: usize,
    pub capacity: usize,
}

/// The functions of LibGC's C interface the runtime uses, and the heap to use them with. An embedder fills it in with
/// the functions of Libraries/LibGC/CAPI.h of the same names.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct GcInterface {
    pub heap: *mut c_void,
    pub gc_get_layout: unsafe extern "C" fn(layout: *mut GcLayout),
    pub gc_allocator_create:
        unsafe extern "C" fn(type_info: *const CellTypeInfo, name: *const c_char, name_length: usize) -> *mut c_void,
    pub gc_heap_allocate_cell:
        unsafe extern "C" fn(heap: *mut c_void, allocator: *mut c_void, must_mark: *mut bool) -> *mut c_void,
    pub gc_heap_defer_gc: unsafe extern "C" fn(heap: *mut c_void),
    pub gc_heap_undefer_gc: unsafe extern "C" fn(heap: *mut c_void),
    pub gc_heap_collect_garbage: unsafe extern "C" fn(heap: *mut c_void, collection_type: i32, print_report: bool),
    pub gc_heap_did_allocate_external_memory: unsafe extern "C" fn(heap: *mut c_void, size: usize),
    pub gc_root_create: unsafe extern "C" fn(cell: *mut c_void) -> *mut c_void,
    pub gc_root_destroy: unsafe extern "C" fn(root: *mut c_void),
    pub gc_visitor_visit_cell: unsafe extern "C" fn(visitor: *mut c_void, cell: *mut c_void),
    pub gc_visitor_visit_possible_values: unsafe extern "C" fn(visitor: *mut c_void, data: *const u8, size: usize),
    pub gc_primitive_storage_reserve: unsafe extern "C" fn(
        size: usize,
        capacity: usize,
        zero_fill: bool,
        guard_size: usize,
        out_handle: *mut u64,
        out_layout: *mut PrimitiveStorageLayout,
    ) -> bool,
    pub gc_primitive_storage_resize: unsafe extern "C" fn(
        handle: u64,
        new_size: usize,
        zero_fill: bool,
        out_layout: *mut PrimitiveStorageLayout,
    ) -> bool,
    pub gc_primitive_storage_free: unsafe extern "C" fn(handle: u64),
    pub gc_primitive_storage_data: unsafe extern "C" fn(handle: u64) -> *mut u8,
}

/// The size of the words every cell starts with: one of the runtime's and one of LibGC's.
pub const CELL_HEADER_SIZE: usize = 16;

const CELL_STATE_LIVE: u8 = 0;
const CELL_KIND_OTHER: u8 = 0;

/// The byte after LibGC's in a cell, which holds the runtime's flags for the cell.
const CELL_FLAGS_OFFSET: usize = 11;
/// The cell keeps an object of the embedder's alive (see set_host_object()).
const CELL_FLAG_HOST_OBJECT: u8 = 1;

thread_local! {
    /// The object of the embedder's each cell of this thread's heap keeps alive, by cell.
    static HOST_OBJECTS: core::cell::RefCell<std::collections::HashMap<usize, *mut c_void>> = Default::default();
}

/// Have `cell` keep the embedder's cell `object` alive for as long as it lives itself, as an edge the collector follows
/// from it. An embedder's object that stands for `cell` and refers to it then stays the same object while `cell` lives,
/// and goes away with it.
///
/// # Safety
///
/// `cell` must be a live cell of the runtime, and `object` a cell of the same LibGC heap.
pub unsafe fn set_host_object(cell: *mut u8, object: *mut c_void) {
    HOST_OBJECTS.with_borrow_mut(|objects| objects.insert(cell.addr(), object));
    unsafe { *cell.add(CELL_FLAGS_OFFSET) |= CELL_FLAG_HOST_OBJECT };
}

/// The object `cell` keeps alive, or null if it keeps none.
///
/// # Safety
///
/// `cell` must be a live cell of the runtime.
pub unsafe fn host_object(cell: *mut c_void) -> *mut c_void {
    if unsafe { *cell.cast::<u8>().add(CELL_FLAGS_OFFSET) } & CELL_FLAG_HOST_OBJECT == 0 {
        return core::ptr::null_mut();
    }
    HOST_OBJECTS.with_borrow(|objects| objects.get(&cell.addr()).copied().unwrap_or(core::ptr::null_mut()))
}

/// Visit the object `cell` keeps alive, if it keeps one.
///
/// # Safety
///
/// `cell` must be a live cell of the runtime.
pub unsafe fn visit_host_object(cell: *mut c_void, visitor: &Visitor) {
    let object = unsafe { host_object(cell) };
    if !object.is_null() {
        visitor.visit_cell(object.cast());
    }
}

/// Forget the object `cell` keeps alive, as `cell` is going away.
///
/// # Safety
///
/// `cell` must be a cell of the runtime.
pub unsafe fn forget_host_object(cell: *mut c_void) {
    let flags = unsafe { &mut *cell.cast::<u8>().add(CELL_FLAGS_OFFSET) };
    if *flags & CELL_FLAG_HOST_OBJECT == 0 {
        return;
    }
    *flags &= !CELL_FLAG_HOST_OBJECT;
    // NB: A thread's cells are destroyed on the thread, unless it is exiting, when its table is gone already.
    let _ = HOST_OBJECTS.try_with(|objects| objects.borrow_mut().remove(&cell.addr()));
}

/// The cell types of the runtime, in the order of their allocators.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CellType {
    /// A GC object whose inline data has the size of the given size class.
    Object(usize),
    Rtt,
    Exception,
    StoreRoot,
    /// A function, table, memory, global, tag, or instance (see entity.rs), by its ENTITY_* kind.
    Entity(usize),
}

pub use crate::abi::OBJECT_DATA_SIZE_CLASSES;

/// The size class of objects with `size` bytes of data, or None if the data does not fit inline.
pub fn object_size_class(size: usize) -> Option<usize> {
    OBJECT_DATA_SIZE_CLASSES.iter().position(|&class| class >= size)
}

/// The allocators of the runtime's cell types, which LibGC requires to outlive every heap, so there is one set of
/// them for the process, made with the first interface.
struct Allocators {
    objects: Vec<*mut c_void>,
    rtt: *mut c_void,
    exception: *mut c_void,
    store_root: *mut c_void,
    entities: Vec<*mut c_void>,
}

// The allocators are only handles, which LibGC only uses from the thread of the heap it allocates from.
unsafe impl Send for Allocators {}
unsafe impl Sync for Allocators {}

static ALLOCATORS: OnceLock<Allocators> = OnceLock::new();

/// The functions of the first interface, which every heap's cells are visited with: they are the same functions of
/// LibGC whichever heap a store uses.
struct Functions(GcInterface);

// Only the function pointers are used, never the heap.
unsafe impl Send for Functions {}
unsafe impl Sync for Functions {}

static FUNCTIONS: OnceLock<Functions> = OnceLock::new();

unsafe extern "C" fn class_name(_: *const c_void, length: *mut usize) -> *const c_char {
    const NAME: &str = "WebAssembly cell";
    unsafe { length.write(NAME.len()) };
    NAME.as_ptr().cast()
}

/// What LibGC knows each cell type by: its size and name, and the functions it visits and destroys cells with.
fn type_info(cell_type: CellType) -> (&'static CellTypeInfo, &'static str) {
    use super::{entity, gc};
    type VisitEdges = unsafe extern "C" fn(*mut c_void, *mut c_void);
    type Destroy = unsafe extern "C" fn(*mut c_void);
    type ExternalMemorySize = unsafe extern "C" fn(*const c_void) -> usize;
    let (visit_edges, destroy, external_memory_size, name): (VisitEdges, Destroy, Option<ExternalMemorySize>, _) =
        match cell_type {
            CellType::Object(_) => (
                gc::object_visit_edges,
                gc::object_destroy,
                Some(gc::object_external_memory_size),
                "WebAssembly object",
            ),
            CellType::Rtt => (gc::rtt_visit_edges, gc::rtt_destroy, None, "WebAssembly runtime type"),
            CellType::Exception => (
                gc::exception_visit_edges,
                gc::exception_destroy,
                None,
                "WebAssembly exception",
            ),
            CellType::StoreRoot => (
                gc::store_root_visit_edges,
                gc::store_root_destroy,
                None,
                "WebAssembly store",
            ),
            CellType::Entity(kind) => (
                entity::entity_visit_edges,
                entity::entity_destroy,
                None,
                entity::ENTITY_NAMES[kind],
            ),
        };
    let info = Box::leak(Box::new(CellTypeInfo {
        cell_size: cell_size(cell_type) as u32,
        alignment: 16,
        kind: CELL_KIND_OTHER,
        visit_edges,
        finalize: None,
        destroy: Some(destroy),
        external_memory_size,
        class_name: Some(class_name),
    }));
    (info, name)
}

impl Allocators {
    fn new(interface: &GcInterface) -> Self {
        let create = |cell_type: CellType| {
            let (info, name) = type_info(cell_type);
            unsafe { (interface.gc_allocator_create)(info, name.as_ptr().cast(), name.len()) }
        };
        Self {
            objects: (0..OBJECT_DATA_SIZE_CLASSES.len())
                .map(|class| create(CellType::Object(class)))
                .collect(),
            rtt: create(CellType::Rtt),
            exception: create(CellType::Exception),
            store_root: create(CellType::StoreRoot),
            entities: (0..super::entity::ENTITY_KIND_COUNT)
                .map(|kind| create(CellType::Entity(kind)))
                .collect(),
        }
    }

    fn of(&self, cell_type: CellType) -> *mut c_void {
        match cell_type {
            CellType::Object(class) => self.objects[class],
            CellType::Rtt => self.rtt,
            CellType::Exception => self.exception,
            CellType::StoreRoot => self.store_root,
            CellType::Entity(kind) => self.entities[kind],
        }
    }
}

/// The size of a cell of a type.
pub fn cell_size(cell_type: CellType) -> usize {
    match cell_type {
        CellType::Object(class) => crate::abi::OBJECT_INLINE_DATA_OFFSET + OBJECT_DATA_SIZE_CLASSES[class],
        CellType::Rtt => CELL_HEADER_SIZE + size_of::<crate::abi::Rtt>().next_multiple_of(16),
        CellType::Exception => super::gc::EXCEPTION_CELL_SIZE,
        CellType::StoreRoot => 32,
        CellType::Entity(kind) => super::entity::entity_cell_size(kind),
    }
}

/// A LibGC heap.
pub struct GcHeap {
    pub interface: GcInterface,
    layout: GcLayout,
    allocators: &'static Allocators,
}

pub enum Heap {
    /// Cells are plain allocations, which the store's entities are freed with, and memories are mappings of their own.
    Standalone(core::cell::RefCell<Vec<*mut u8>>),
    Gc(Box<GcHeap>),
}

impl Heap {
    pub fn new(interface: Option<GcInterface>) -> Self {
        let Some(interface) = interface else {
            return Self::Standalone(core::cell::RefCell::new(Vec::new()));
        };
        let mut layout = GcLayout::default();
        unsafe { (interface.gc_get_layout)(&raw mut layout) };
        assert!(
            layout.cell_mark_offset as usize >= 8 && (layout.cell_kind_offset as usize) < CELL_HEADER_SIZE,
            "LibGC keeps its cell header in the second word"
        );
        assert!(
            [
                layout.cell_mark_offset,
                layout.cell_state_offset,
                layout.cell_kind_offset
            ]
            .iter()
            .all(|&offset| (offset as usize) < CELL_FLAGS_OFFSET),
            "the runtime's flags follow LibGC's cell header"
        );
        let allocators = ALLOCATORS.get_or_init(|| Allocators::new(&interface));
        FUNCTIONS.get_or_init(|| Functions(interface));
        Self::Gc(Box::new(GcHeap {
            interface,
            layout,
            allocators,
        }))
    }

    pub fn gc(&self) -> Option<&GcHeap> {
        match self {
            Self::Standalone(_) => None,
            Self::Gc(heap) => Some(heap),
        }
    }

    /// Allocate a zero-filled cell. Garbage may be collected first, so every reference the caller holds outside of
    /// the stack and the cells the store reaches must be in a deferred section.
    pub fn allocate_cell(&self, cell_type: CellType) -> *mut u8 {
        let size = cell_size(cell_type);
        match self {
            Self::Standalone(entities) => {
                // NB: Standalone stores never free their cells, but drop their entities when they go away.
                let words = vec![0u128; size.div_ceil(16)].into_boxed_slice();
                let cell: *mut u8 = Box::leak(words).as_mut_ptr().cast();
                if matches!(cell_type, CellType::Entity(_)) {
                    entities.borrow_mut().push(cell);
                }
                cell
            }
            Self::Gc(heap) => unsafe {
                let mut must_mark = false;
                let cell: *mut u8 = (heap.interface.gc_heap_allocate_cell)(
                    heap.interface.heap,
                    heap.allocators.of(cell_type),
                    &raw mut must_mark,
                )
                .cast();
                assert!(!cell.is_null(), "LibGC allocates the cell");
                core::ptr::write_bytes(cell, 0, size);
                *cell.add(heap.layout.cell_mark_offset as usize) = u8::from(must_mark);
                *cell.add(heap.layout.cell_state_offset as usize) = CELL_STATE_LIVE;
                *cell.add(heap.layout.cell_kind_offset as usize) = CELL_KIND_OTHER;
                cell
            },
        }
    }

    /// Drop the entities of a standalone heap, whose store is going away.
    pub(crate) fn drop_standalone_entities(&self) {
        if let Self::Standalone(entities) = self {
            for cell in entities.take() {
                unsafe { super::entity::destroy_entity(cell) };
            }
        }
    }

    /// Count memory a cell owns outside of the heap towards the next collection.
    pub fn did_allocate_external_memory(&self, size: usize) {
        if let Self::Gc(heap) = self {
            unsafe { (heap.interface.gc_heap_did_allocate_external_memory)(heap.interface.heap, size) };
        }
    }

    /// Stop collecting garbage until the matching undefer_gc(), for code that keeps references where the collector
    /// does not look.
    pub fn defer_gc(&self) {
        if let Self::Gc(heap) = self {
            unsafe { (heap.interface.gc_heap_defer_gc)(heap.interface.heap) };
        }
    }

    pub fn undefer_gc(&self) {
        if let Self::Gc(heap) = self {
            unsafe { (heap.interface.gc_heap_undefer_gc)(heap.interface.heap) };
        }
    }
}

impl GcHeap {
    /// Keep the cell alive until the root is destroyed.
    pub(crate) fn root(&self, cell: *mut u8) -> *mut c_void {
        unsafe { (self.interface.gc_root_create)(cell.cast()) }
    }

    pub(crate) fn unroot(&self, root: *mut c_void) {
        unsafe { (self.interface.gc_root_destroy)(root) };
    }
}

/// How the collector follows a value: not at all, as a reference in the any, extern, or exn hierarchy, or as a
/// function reference, which points into its function's cell.
pub const TRACE_NONE: u8 = 0;
pub const TRACE_REFERENCE: u8 = 1;
pub const TRACE_FUNCTION: u8 = 2;

/// A visitor LibGC passes to the cells' visit_edges, with the functions to visit cells with.
pub struct Visitor {
    raw: *mut c_void,
    interface: *const GcInterface,
}

impl Visitor {
    /// The visitor LibGC passed to a cell's visit_edges. Only cells of a LibGC heap are visited, so there is an
    /// interface by then.
    pub fn new(raw: *mut c_void) -> Self {
        let functions = FUNCTIONS.get().expect("cells are only visited once a heap exists");
        Self {
            raw,
            interface: &raw const functions.0,
        }
    }

    /// The LibGC visitor itself, for an embedder to visit its cells with.
    pub fn raw(&self) -> *mut c_void {
        self.raw
    }

    pub(crate) fn visit_cell(&self, cell: *mut u8) {
        unsafe { ((*self.interface).gc_visitor_visit_cell)(self.raw, cell.cast()) };
    }

    /// Visit a value of a type whose TRACE_* kind is `trace`.
    pub fn visit_value(&self, value: u64, trace: u8) {
        match trace {
            TRACE_REFERENCE => self.visit_reference(value),
            TRACE_FUNCTION if value != 0 => self.visit_cell((value as *mut u8).wrapping_sub(CELL_HEADER_SIZE)),
            _ => {}
        }
    }

    /// Visit a reference in the any, extern, or exn hierarchy, whose kind is in its low bits (see gc.rs).
    pub fn visit_reference(&self, reference: u64) {
        if reference == 0 || reference & 1 != 0 {
            return;
        }
        if reference & 3 == 2 {
            // An embedder's host value, which LibGC only follows if it points into a cell.
            unsafe {
                ((*self.interface).gc_visitor_visit_possible_values)(
                    self.raw,
                    (&raw const reference).cast(),
                    size_of::<u64>(),
                );
            }
            return;
        }
        self.visit_cell(reference as *mut u8);
    }
}
