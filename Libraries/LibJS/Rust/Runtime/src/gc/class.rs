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

/// Mirrors GCCellTypeInfo from Libraries/LibGC/CAPI.h, which LibGC dispatches every per-cell operation through.
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

/// The class of a cell, stored in the first word of every cell. It starts with the type info LibGC dispatches
/// through, so the class of a cell and the type info of the block it lives in are the same address.
#[repr(C)]
pub struct Class {
    pub type_info: CellTypeInfo,
    pub name: &'static str,
    pub id: ClassId,
    /// The class of the cell type this one extends.
    pub parent: Option<&'static Class>,
    /// The internal methods of the objects of this class, the Rust form of the C++ Object vtable. Cells that are not
    /// objects have none.
    pub object_methods: Option<&'static ObjectMethods>,
}

/// LibGC hands out cells in the slots of fixed-size blocks, whose free list needs room for its own entry in each.
const MIN_CELL_SIZE: usize = 24;
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
        }
    }

    pub fn kind(&self) -> CellKind {
        // SAFETY: The kind was stored from a CellKind.
        unsafe { core::mem::transmute::<u8, CellKind>(self.type_info.kind) }
    }

    /// What Cell::class_name() returns for the C++ class this one mirrors, which printing and messages show. It is
    /// the name of the class unless the Rust type is spelled differently.
    pub fn class_name(&self) -> &'static str {
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

impl Class {
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
    // SAFETY: Every Rust cell starts with its class.
    let class = unsafe { cell.cast::<&'static Class>().read() };
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

/// The class a cell was allocated with, which may be a subclass of its static type.
pub fn class_of<T>(cell: crate::layout::cell::Gc<T>) -> &'static Class {
    // SAFETY: Every cell starts with its class.
    unsafe { cell.as_ptr().cast::<&'static Class>().read() }
}

impl<T: ?Sized> core::fmt::Debug for crate::layout::cell::Gc<T> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "Gc({:p})", self.as_non_null())
    }
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
    /// The Gc of a cell the caller only has a reference to, like the receiver of a method.
    ///
    /// # Safety
    ///
    /// `cell` must be a cell in the heap, not a value that has yet to be allocated.
    pub unsafe fn from_ref(cell: &T) -> Self {
        // SAFETY: The caller guarantees that the reference is to a live cell.
        unsafe { Self::from_non_null(core::ptr::NonNull::from(cell)) }
    }
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
            .is_subclass_of(Derived::CLASS)
            // SAFETY: The cell was allocated as a Derived or a subclass of it.
            .then(|| unsafe { crate::layout::cell::Gc::from_non_null(self.as_non_null().cast()) })
    }
}

impl CellHeader {
    pub fn for_class(class: &'static Class) -> Self {
        Self {
            class,
            mark: false.into(),
            state: CellState::Live.into(),
            kind: class.kind(),
        }
    }
}

/// Defines the class of a cell type, named after the type and its ClassId. `extends` names the cell types it
/// extends, nearest first. `methods` gives an object class its internal methods, and `finalize` opts into running its
/// Finalize implementation. A class that names neither inherits them from the class it extends, as C++ subclasses
/// inherit virtual methods, so the Finalize implementation of a subclass has to finalize its base as well.
macro_rules! define_cell {
    ($type:ident, $kind:ident $(, extends: [$parent:ident $(, $ancestor:ident)*])? $(, methods: $methods:path)? $(, finalize: $finalize:ident)?) => {
        const _: () = {
            static CLASS: $crate::gc::class::Class = $crate::gc::class::Class::new::<$type>(
                stringify!($type),
                $crate::gc::class_id::ClassId::$type,
                $crate::layout::cell::CellKind::$kind,
                define_cell!(@parent $($parent)?),
                define_cell!(@methods [$($parent)?] [$($methods)?]),
                define_cell!(@finalize $type [$($parent)?] [$($finalize)?]),
            );

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
}

pub(crate) use define_cell;
