/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cells that hold values of an embedder's own types, which the embedder allocates from the heap of the VM and keeps
//! alive through the host data slots of its host objects, or through any other traced place.
//!
//! The runtime does not know the embedder's types, so their cells have no class id of their own: every one has the
//! id ClassId::EmbedderCell, and the class of a type, which the embedder defines once as a static, is told apart from
//! every other class by its address. The heap keeps the allocator of such a class by that address.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::mem::{ManuallyDrop, MaybeUninit};
use core::ops::Deref;
use core::ptr::NonNull;

use super::class::{Class, GcCell, MIN_CELL_SIZE, class_of};
use super::class_id::ClassId;
use super::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, CellKind, Gc};

/// A value of an embedder's type in a cell of its own, which derefs to the value.
#[repr(C, align(8))]
pub struct EmbedderCell<T> {
    header: CellHeader,
    value: ValueWithRoomForFreeListLink<T>,
}

/// LibGC keeps the link to the next free cell in the bytes after the header of a free cell, so a cell has at least
/// MIN_CELL_SIZE bytes however small its value is.
#[repr(C)]
union ValueWithRoomForFreeListLink<T> {
    value: ManuallyDrop<T>,
    _room_for_free_list_link: [MaybeUninit<u8>; MIN_CELL_SIZE - size_of::<CellHeader>()],
}

const _: () = assert!(size_of::<EmbedderCell<()>>() == MIN_CELL_SIZE);
const _: () = assert!(size_of::<EmbedderCell<u64>>() == 32);

impl<T> Drop for EmbedderCell<T> {
    fn drop(&mut self) {
        // SAFETY: The union holds a value, which nothing has dropped: the cell is dropped once, by LibGC's destroy.
        unsafe { ManuallyDrop::drop(&mut self.value.value) };
    }
}

impl<T> Deref for EmbedderCell<T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: The union holds a value for as long as the cell lives.
        unsafe { &self.value.value }
    }
}

// SAFETY: The value is all the cell holds.
unsafe impl<T: Trace> Trace for EmbedderCell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        (**self).trace(visitor);
    }
}

/// The class of the cells that hold values of one of the embedder's types, which the embedder defines once per type
/// as a static and names in the type's EmbedderValue implementation:
///
/// ```ignore
/// static THING_CLASS: EmbedderClass<Thing> = EmbedderClass::new("Thing");
///
/// impl EmbedderValue for Thing {
///     const CLASS: &'static EmbedderClass<Self> = &THING_CLASS;
/// }
/// ```
///
/// LibGC traces the cells of the class, and destroys those whose value has a destructor. It does not finalize them
/// or count memory they own outside the heap: an embedder type that needs either has to opt in through its class,
/// as the runtime's own cells do through theirs, which no embedder type does yet.
pub struct EmbedderClass<T: 'static> {
    class: Class,
    _value: PhantomData<fn() -> T>,
}

impl<T: Trace + 'static> EmbedderClass<T> {
    pub const fn new(name: &'static str) -> Self {
        let mut class = Class::new::<EmbedderCell<T>>(name, ClassId::EmbedderCell, CellKind::Other, None, None, None);
        // The cell drops its value, so Class::new sees a type that needs dropping whatever the value is. A value that
        // has no destructor leaves the collector nothing to run when the cell dies.
        if !core::mem::needs_drop::<T>() {
            class.type_info.destroy = None;
        }
        Self {
            class,
            _value: PhantomData,
        }
    }

    pub const fn class(&'static self) -> &'static Class {
        &self.class
    }
}

/// A type of the embedder whose values live in cells of their own.
pub trait EmbedderValue: Trace + Sized + 'static {
    const CLASS: &'static EmbedderClass<Self>;
}

// SAFETY: The cell is #[repr(C)] and starts with its header.
unsafe impl<T: EmbedderValue> GcCell for EmbedderCell<T> {
    const CLASS: &'static Class = T::CLASS.class();
}

impl<T: EmbedderValue> EmbedderClass<T> {
    /// Moves `value` into a new cell of the class, from the allocator of the class, which the first allocation for
    /// the class creates.
    pub fn allocate(&'static self, vm: &Vm, value: T) -> Gc<EmbedderCell<T>> {
        let allocator = vm.heap().embedder_class_allocator(&self.class);
        vm.heap().allocate_in(
            allocator,
            EmbedderCell {
                header: CellHeader::for_class(&self.class),
                value: ValueWithRoomForFreeListLink {
                    value: ManuallyDrop::new(value),
                },
            },
        )
    }

    /// The cell behind the pointer, if it is a cell of this class.
    ///
    /// # Safety
    ///
    /// `cell` must be a live cell that an embedder class allocated, such as the host data of a host object the
    /// embedder created with such a cell, whose first word is therefore its class.
    pub unsafe fn downcast(&'static self, cell: NonNull<c_void>) -> Option<Gc<EmbedderCell<T>>> {
        // SAFETY: The caller passes a live cell of an embedder class.
        let cell = unsafe { Gc::<EmbedderCell<T>>::from_non_null(cell.cast()) };
        core::ptr::eq(class_of(cell), &raw const self.class).then_some(cell)
    }
}
