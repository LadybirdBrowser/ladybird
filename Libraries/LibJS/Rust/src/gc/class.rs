/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::{c_char, c_void};

use super::capi::GCVisitor;
use super::class_id::ClassId;
use super::visitor::{Trace, Visitor};
use crate::layout::cell::{CellHeader, CellKind, CellState};
use crate::runtime::object::ObjectMethods;

#[cfg(feature = "address-sanitizer")]
unsafe extern "C" {
    fn __lsan_ignore_object(pointer: *const c_void);
}

/// Mirrors GCCellTypeInfo from Libraries/LibGC/CAPI.h, which LibGC dispatches every per-cell operation through.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct CellTypeInfo {
    pub cell_size: u32,
    pub alignment: u32,
    pub kind: u8,
    pub visit_edges: unsafe extern "C" fn(cell: *mut c_void, visitor: *mut GCVisitor),
    pub finalize: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    pub destroy: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    pub external_memory_size: Option<unsafe extern "C" fn(cell: *const c_void) -> usize>,
    pub class_name: Option<unsafe extern "C" fn(cell: *const c_void, length: *mut usize) -> *const c_char>,
}

/// The class of a cell, which the block the cell lives in holds (see `class_of`). It starts with the type info LibGC
/// dispatches through.
#[repr(C)]
pub struct Class {
    pub type_info: CellTypeInfo,
    pub name: &'static str,
    pub id: ClassId,
    /// The class of the cell type this one extends.
    pub parent: Option<&'static Class>,
    /// The internal methods of the objects of this class. Cells that are not objects have none.
    pub object_methods: Option<&'static ObjectMethods>,
    /// The kind of the cells of this class, which type_info also holds for LibGC.
    kind: CellKind,
}

// The layout module's mirror of the class, through which Layout.h tells an embedder where a class keeps its id and
// the class it extends.
const _: () = {
    use crate::layout::class::{CellTypeInfoLayout, ClassLayout};
    assert!(size_of::<CellTypeInfo>() == size_of::<CellTypeInfoLayout>());
    assert!(core::mem::offset_of!(Class, type_info) == core::mem::offset_of!(ClassLayout, type_info));
    assert!(core::mem::offset_of!(Class, name) == core::mem::offset_of!(ClassLayout, name));
    assert!(core::mem::offset_of!(Class, id) == core::mem::offset_of!(ClassLayout, id));
    assert!(size_of::<ClassId>() == size_of::<u16>());
    assert!(core::mem::offset_of!(Class, parent) == core::mem::offset_of!(ClassLayout, parent));
    assert!(size_of::<Option<&'static Class>>() == size_of::<*const ClassLayout>());
};

/// LibGC hands out cells in the slots of fixed-size blocks, whose free list needs room for its own entry in each.
pub(crate) const MIN_CELL_SIZE: usize = 24;
const MAX_CELL_ALIGNMENT: usize = 16;

impl Class {
    pub const fn new<T: Trace>(
        name: &'static str,
        id: ClassId,
        kind: CellKind,
        parent: Option<&'static Class>,
        object_methods: Option<&'static ObjectMethods>,
        finalize: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    ) -> Self {
        assert!(size_of::<T>() >= MIN_CELL_SIZE);
        assert!(size_of::<T>().is_multiple_of(8));
        assert!(align_of::<T>() <= MAX_CELL_ALIGNMENT);
        assert!(
            matches!(kind, CellKind::Object) == object_methods.is_some(),
            "exactly the object classes have internal methods"
        );
        Self {
            type_info: CellTypeInfo {
                cell_size: size_of::<T>() as u32,
                alignment: align_of::<T>() as u32,
                kind: kind as u8,
                visit_edges: visit_edges::<T>,
                finalize,
                destroy: if core::mem::needs_drop::<T>() {
                    Some(destroy::<T>)
                } else {
                    None
                },
                external_memory_size: None,
                class_name: Some(class_name),
            },
            name,
            id,
            parent,
            object_methods,
            kind,
        }
    }

    pub fn kind(&self) -> CellKind {
        self.kind
    }

    /// What Cell::class_name() returns for the cells of this class, which printing and messages show. It is the name
    /// of the class unless the Rust type is spelled differently.
    pub fn class_name(&self) -> &'static str {
        if self.is_derived_at_run_time() {
            return self.name;
        }
        match self.id {
            ClassId::EcmascriptFunctionObject => "ECMAScriptFunctionObject",
            ClassId::Test262GlobalObject => "GlobalObject",
            ClassId::Dollar262Object => "$262Object",
            _ => self.name,
        }
    }
}

unsafe extern "C" fn visit_edges<T: Trace>(cell: *mut c_void, visitor: *mut GCVisitor) {
    // SAFETY: LibGC passes a live cell of this class and a visitor that outlives the call.
    let (cell, mut visitor) = unsafe { (&*cell.cast::<T>(), Visitor::from_raw(visitor)) };
    cell.trace(&mut visitor);
}

/// Cells that need to act before they are destroyed. Finalizers run during a collection, while every cell that dies
/// in it is still intact, and must not allocate.
pub trait Finalize {
    fn finalize(&self);
}

/// The finalize entry of a class whose cells implement Finalize.
///
/// # Safety
///
/// Only LibGC calls it, with a dead cell of that class.
pub unsafe extern "C" fn finalize<T: Finalize>(cell: *mut c_void) {
    // SAFETY: LibGC finalizes each dead cell of this class once, before destroying it.
    unsafe { &*cell.cast::<T>() }.finalize();
}

/// Cells that own memory outside the heap, which LibGC counts towards the bytes that make it collect, as C++ cells
/// override Cell::external_memory_size().
pub trait ExternalMemorySize {
    fn external_memory_size(&self) -> usize;
}

/// The external_memory_size entry of a class whose cells implement ExternalMemorySize.
///
/// # Safety
///
/// Only LibGC calls it, with a cell of that class.
pub unsafe extern "C" fn external_memory_size<T: ExternalMemorySize>(cell: *const c_void) -> usize {
    // SAFETY: LibGC passes a cell of this class, which stays intact for the call.
    unsafe { &*cell.cast::<T>() }.external_memory_size()
}

impl Class {
    /// Has LibGC ask the cells of this class how much memory they own outside the heap.
    pub const fn with_external_memory_size(
        mut self,
        external_memory_size: Option<unsafe extern "C" fn(cell: *const c_void) -> usize>,
    ) -> Self {
        self.type_info.external_memory_size = external_memory_size;
        self
    }

    /// Creates a class that extends `parent` while the runtime runs, as an embedder needs one for each class of host
    /// object it defines. Its cells have the parent's layout, so it shares the parent's type info and id, and code that
    /// tells classes apart by id sees the parent. It has a name and internal methods of its own, and its cells come from
    /// an allocator of their own once allocated with Heap::allocate_in(). The class lives as long as the process.
    pub fn derive_runtime(
        parent: &'static Class,
        name: &'static str,
        object_methods: &'static ObjectMethods,
    ) -> &'static Class {
        assert!(
            parent.object_methods.is_some(),
            "only object classes are derived at run time"
        );
        Self::leak_class_derived_at_run_time(parent, name, Some(object_methods))
    }

    /// Creates a class of cells that are not objects, such as the module records of a host class, that extends
    /// `parent` while the runtime runs, as derive_runtime() does for objects.
    pub fn derive_runtime_without_object_methods(parent: &'static Class, name: &'static str) -> &'static Class {
        assert!(
            parent.object_methods.is_none(),
            "the classes of objects have internal methods"
        );
        Self::leak_class_derived_at_run_time(parent, name, None)
    }

    fn leak_class_derived_at_run_time(
        parent: &'static Class,
        name: &'static str,
        object_methods: Option<&'static ObjectMethods>,
    ) -> &'static Class {
        let class = Box::leak(Box::new(Class {
            type_info: parent.type_info,
            name,
            id: parent.id,
            parent: Some(parent),
            object_methods,
            kind: parent.kind,
        }));
        #[cfg(feature = "address-sanitizer")]
        // SAFETY: The class is a live allocation, which the process keeps for as long as it runs.
        unsafe {
            __lsan_ignore_object(core::ptr::from_ref(class).cast());
        }
        class
    }

    /// Whether Class::derive_runtime() created the class. Every class define_cell! defines has an id of its own.
    pub fn is_derived_at_run_time(&self) -> bool {
        self.parent.is_some_and(|parent| parent.id == self.id)
    }

    /// Whether the class is that of the cell type `T` or a subclass of it. Classes derived at run time have the id of
    /// the class they extend, and every cell type has an id of its own, so a class with the id of `T` is the class of
    /// `T` or one derived from it at run time. That covers the long chains of host classes without walking them.
    pub fn is_class_of<T: GcCell>(&self) -> bool {
        self.id == T::CLASS.id || self.is_subclass_of(T::CLASS)
    }

    pub fn is_subclass_of(&self, ancestor: &Class) -> bool {
        let mut class = Some(self);
        while let Some(current) = class {
            if core::ptr::eq(current, ancestor) {
                return true;
            }
            class = current.parent;
        }
        false
    }
}

unsafe extern "C" fn destroy<T>(cell: *mut c_void) {
    // SAFETY: LibGC destroys each dead cell of this class exactly once.
    unsafe { core::ptr::drop_in_place(cell.cast::<T>()) };
}

unsafe extern "C" fn class_name(cell: *const c_void, length: *mut usize) -> *const c_char {
    // SAFETY: LibGC passes a cell of a Rust allocator.
    let class = unsafe { class_at(cell.cast()) };
    // SAFETY: LibGC passes somewhere to store the length.
    unsafe { length.write(class.name.len()) };
    class.name.as_ptr().cast()
}

/// A type whose values live in the garbage-collected heap.
///
/// # Safety
///
/// The type must be #[repr(C)] and start with its cell header, either directly or through the cell it extends.
pub unsafe trait GcCell: Trace {
    const CLASS: &'static Class;
}

/// Marks that a cell type extends `Base`, starting with a `Base` at offset zero.
///
/// # Safety
///
/// The type must start with a `Base`, directly or through other cells that do.
pub unsafe trait Extends<Base> {}

// SAFETY: Every type starts with itself.
unsafe impl<T: GcCell> Extends<T> for T {}

// SAFETY: Every cell starts with its header.
unsafe impl<T: GcCell> Extends<CellHeader> for T {}

// SAFETY: A header starts with itself.
unsafe impl Extends<CellHeader> for CellHeader {}

/// A cell's block starts at the cell's address rounded down to a multiple of BLOCK_SIZE, and holds the class of its
/// cells at BLOCK_CLASS_OFFSET: the foreign type of the allocator that created the block (see Heap::create_allocator).
/// The block of an allocator that classes share holds the class they extend with BLOCK_CLASS_TAIL_TAG added, and each of
/// its cells keeps its own class in the word after the cell_size bytes of that class.
pub const BLOCK_SIZE: usize = 16 * 1024;
pub const BLOCK_CLASS_OFFSET: usize = 2 * size_of::<usize>();
pub use crate::layout::cell::BLOCK_CLASS_TAIL_TAG;

/// The class that the block `address` is in holds, without its tag, or null for a block of C++ cells.
///
/// # Safety
///
/// `address` must be in a live cell.
pub unsafe fn block_class_pointer_at(address: *const u8) -> *const Class {
    debug_assert!(is_in_heap_region(address.addr()), "only cells in the heap have a class");
    let block = address.wrapping_sub(address.addr() & (BLOCK_SIZE - 1));
    // SAFETY: The caller passes an address in a live cell, whose block is live as well.
    let class = unsafe { block.add(BLOCK_CLASS_OFFSET).cast::<*const u8>().read() };
    class.wrapping_sub(class.addr() & BLOCK_CLASS_TAIL_TAG).cast()
}

/// The class of the cell at `cell`.
///
/// # Safety
///
/// `cell` must be the address of a live cell of a Rust allocator.
pub unsafe fn class_at(cell: *const u8) -> &'static Class {
    debug_assert!(is_in_heap_region(cell.addr()), "only cells in the heap have a class");
    let block = cell.wrapping_sub(cell.addr() & (BLOCK_SIZE - 1));
    // SAFETY: The caller passes a live cell of a Rust allocator, whose block holds a class.
    let class = unsafe { block.add(BLOCK_CLASS_OFFSET).cast::<*const u8>().read() };
    if class.addr() & BLOCK_CLASS_TAIL_TAG == 0 {
        // SAFETY: The allocator created the block with the class of its cells, which lives forever.
        return unsafe { &*class.cast::<Class>() };
    }
    // SAFETY: The class that classes share an allocator of lives forever, and each cell of the allocator keeps its own
    //         class right after the bytes of that class.
    unsafe {
        let shared = &*class.wrapping_sub(BLOCK_CLASS_TAIL_TAG).cast::<Class>();
        cell.add(shared.type_info.cell_size as usize)
            .cast::<&'static Class>()
            .read()
    }
}

/// The class a cell was allocated with, which may be a subclass of its static type.
pub fn class_of<T>(cell: crate::layout::cell::Gc<T>) -> &'static Class {
    // SAFETY: The cell is live.
    unsafe { class_at(cell.as_ptr().cast()) }
}

/// The cell as a `Derived`, if it was allocated as one or as a subclass of it.
///
/// NB: A cell is only ever allocated by the allocator of its class, so the class its block holds tells what the memory
///     around it is.
pub fn downcast_ref<Base: GcCell, Derived: GcCell + Extends<Base>>(cell: &Base) -> Option<&Derived> {
    // SAFETY: The cell is live.
    let class = unsafe { class_at(core::ptr::from_ref(cell).cast()) };
    class
        .is_class_of::<Derived>()
        // SAFETY: The cell is a Derived or a subclass of it, which starts with a Derived.
        .then(|| unsafe { &*core::ptr::from_ref(cell).cast::<Derived>() })
}

/// The cell as a `Derived`, which it has to have been allocated as, or as a subclass of.
pub fn cast_ref<Base: GcCell, Derived: GcCell + Extends<Base>>(cell: &Base) -> &Derived {
    downcast_ref(cell).unwrap_or_else(|| panic!("cell is not a {}", Derived::CLASS.name))
}

/// Cells are only ever mutated through interior mutability, so a shared reference is all a Gc hands out.
impl<T: GcCell> core::ops::Deref for crate::layout::cell::Gc<T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: A Gc points to a live cell, which never moves.
        unsafe { self.as_non_null().as_ref() }
    }
}

impl<T: GcCell> crate::layout::cell::Gc<T> {
    /// The Gc of a cell the caller only has a reference to, like the receiver of a method. Panics if the reference is
    /// to a value that has yet to be allocated.
    pub fn from_ref(cell: &T) -> Self {
        assert!(
            is_in_heap_region(core::ptr::from_ref(cell).addr()),
            "a {} that has yet to be allocated is not a cell",
            T::CLASS.name
        );
        // SAFETY: The reference is into the heap region, where every value of a cell type is a cell that was allocated
        //         as one, since cells only ever embed the cell they extend, at their start.
        let gc = unsafe { Self::from_non_null(core::ptr::NonNull::from(cell)) };
        debug_assert!(class_of(gc).is_subclass_of(T::CLASS));
        gc
    }
}

/// Whether `address` is in the region LibGC allocates every cell in, which has no cells before it is set up.
pub(crate) fn is_in_heap_region(address: usize) -> bool {
    let base = crate::layout_forward::heap_region_base();
    base != 0 && address.wrapping_sub(base) as u64 <= crate::build_configuration::HEAP_REGION_OFFSET_MASK
}

impl<T> crate::layout::cell::Gc<T> {
    pub fn upcast<Base>(self) -> crate::layout::cell::Gc<Base>
    where
        T: Extends<Base>,
    {
        // SAFETY: The cell starts with a Base.
        unsafe { crate::layout::cell::Gc::from_non_null(self.as_non_null().cast()) }
    }

    pub fn downcast<Derived: GcCell + Extends<T>>(self) -> Option<crate::layout::cell::Gc<Derived>> {
        class_of(self)
            .is_class_of::<Derived>()
            // SAFETY: The cell was allocated as a Derived or a subclass of it.
            .then(|| unsafe { crate::layout::cell::Gc::from_non_null(self.as_non_null().cast()) })
    }
}

impl CellHeader {
    /// The header of a cell of `class`, which must also be the class of the allocator the cell comes from.
    pub fn for_class(class: &'static Class) -> Self {
        Self {
            unused: [0; 8],
            mark: false.into(),
            state: CellState::Live.into(),
            kind: class.kind(),
        }
    }

    /// The class of the cell, which its block holds.
    pub fn class(&self) -> &'static Class {
        // SAFETY: A header starts a live cell.
        unsafe { class_at(core::ptr::from_ref(self).cast()) }
    }
}

/// Defines the class of a cell type, named after the type and its ClassId. `extends` names the cell types it
/// extends, nearest first. `methods` gives an object class its internal methods, `finalize` opts into running its
/// Finalize implementation, and `external_memory_size` into reporting its ExternalMemorySize. A class that names none
/// of them inherits them from the class it extends, as C++ subclasses inherit virtual methods, so the Finalize
/// implementation of a subclass has to finalize its base as well.
macro_rules! define_cell {
    ($type:ident, $kind:ident $(, extends: [$parent:ident $(, $ancestor:ident)*])? $(, methods: $methods:path)? $(, finalize: $finalize:ident)? $(, external_memory_size: $external_memory_size:ident)?) => {
        const _: () = {
            static CLASS: $crate::gc::class::Class = $crate::gc::class::Class::new::<$type>(
                stringify!($type),
                $crate::gc::class_id::ClassId::$type,
                $crate::layout::cell::CellKind::$kind,
                define_cell!(@parent $($parent)?),
                define_cell!(@methods [$($parent)?] [$($methods)?]),
                define_cell!(@finalize $type [$($parent)?] [$($finalize)?]),
            )
            .with_external_memory_size(define_cell!(@external_memory_size $type [$($parent)?] [$($external_memory_size)?]));

            // SAFETY: Checked by the asserts in Class::new and the cell's #[repr(C)] layout.
            unsafe impl $crate::gc::class::GcCell for $type {
                const CLASS: &'static $crate::gc::class::Class = &CLASS;
            }

            $(
                // SAFETY: The cell's first field is the cell it extends, which starts with each of its ancestors.
                unsafe impl $crate::gc::class::Extends<$parent> for $type {}
                $(unsafe impl $crate::gc::class::Extends<$ancestor> for $type {})*
            )?
        };
    };
    (@parent) => { None };
    (@parent $parent:ident) => { Some(<$parent as $crate::gc::class::GcCell>::CLASS) };
    (@methods [] []) => { None };
    (@methods [$parent:ident] []) => { <$parent as $crate::gc::class::GcCell>::CLASS.object_methods };
    (@methods [$($parent:ident)?] [$methods:path]) => { Some(&$methods) };
    (@finalize $type:ident [] []) => { None };
    (@finalize $type:ident [$parent:ident] []) => { <$parent as $crate::gc::class::GcCell>::CLASS.type_info.finalize };
    (@finalize $type:ident [$($parent:ident)?] [finalize]) => { Some($crate::gc::class::finalize::<$type>) };
    (@external_memory_size $type:ident [] []) => { None };
    (@external_memory_size $type:ident [$parent:ident] []) => {
        <$parent as $crate::gc::class::GcCell>::CLASS.type_info.external_memory_size
    };
    (@external_memory_size $type:ident [$($parent:ident)?] [external_memory_size]) => {
        Some($crate::gc::class::external_memory_size::<$type>)
    };
}

pub(crate) use define_cell;
