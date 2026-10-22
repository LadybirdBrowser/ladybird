/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entities of the runtime: functions, tables, memories, globals, tags, and instances. Each is a cell of the heap,
//! with the entity right after the cell header, and lives while something refers to it: an embedder's cell, another
//! entity (an instance refers to everything it defines and imports, a function to its instance), or an interpreter
//! frame, whose function pointer the conservative scan of the stack finds. The store refers to none of them.
//!
//! The first word of an entity's cell says what it is, so that a handle an embedder passes in can be checked.

use super::heap::{CELL_HEADER_SIZE, CellType, Heap, Visitor, forget_host_object, visit_host_object};
use super::memory::MemoryInstance;
use super::{FunctionInstance, FunctionKind, GlobalInstance, Instance, TableInstance, TagInstance};
use core::ffi::c_void;
use core::ptr::NonNull;

pub const ENTITY_FUNCTION: usize = 0;
pub const ENTITY_TABLE: usize = 1;
pub const ENTITY_MEMORY: usize = 2;
pub const ENTITY_GLOBAL: usize = 3;
pub const ENTITY_TAG: usize = 4;
pub const ENTITY_INSTANCE: usize = 5;
pub const ENTITY_KIND_COUNT: usize = 6;

pub const ENTITY_NAMES: [&str; ENTITY_KIND_COUNT] = [
    "WebAssembly function",
    "WebAssembly table",
    "WebAssembly memory",
    "WebAssembly global",
    "WebAssembly tag",
    "WebAssembly instance",
];

/// What the first word of an entity's cell holds, besides its kind.
const ENTITY_MAGIC: u64 = 0x5741_534d_0000_0000;

pub trait Entity: Sized {
    const KIND: usize;

    /// Visit the cells the entity refers to.
    fn visit_edges(&self, visitor: &Visitor);
}

/// An entity, as the address of the entity in its cell. Function references and tag identities are the handles of
/// their functions and tags, and the interpreter's views of instances, tables, memories, and globals are the first
/// fields of theirs, so the interpreter's pointers are handles too, and a slice of handles is an array of them.
#[repr(transparent)]
pub struct Handle<T>(NonNull<T>);

impl<T> Clone for Handle<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Handle<T> {}

impl<T> PartialEq for Handle<T> {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}

impl<T> Eq for Handle<T> {}

impl<T> core::hash::Hash for Handle<T> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.0.hash(state);
    }
}

impl<T> core::fmt::Debug for Handle<T> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "{:p}", self.0)
    }
}

impl<T: Entity> Handle<T> {
    /// # Safety
    ///
    /// `pointer` must be the address of a live entity of this kind.
    pub unsafe fn from_ptr(pointer: *const T) -> Self {
        Self(NonNull::new(pointer.cast_mut()).expect("an entity is never null"))
    }

    /// The entity of a cell, if the cell is an entity of this kind.
    ///
    /// # Safety
    ///
    /// `cell` must be a live cell of the runtime.
    pub unsafe fn from_cell(cell: *mut c_void) -> Option<Self> {
        if cell.is_null() || unsafe { cell.cast::<u64>().read() } != ENTITY_MAGIC | T::KIND as u64 {
            return None;
        }
        Some(Self(unsafe {
            NonNull::new_unchecked(cell.cast::<u8>().add(CELL_HEADER_SIZE).cast())
        }))
    }

    pub fn as_ptr(self) -> *mut T {
        self.0.as_ptr()
    }

    pub fn cell(self) -> *mut u8 {
        self.0.as_ptr().cast::<u8>().wrapping_sub(CELL_HEADER_SIZE)
    }

    /// # Safety
    ///
    /// The entity must be alive for `'a`, and not be mutated meanwhile.
    pub unsafe fn get<'a>(self) -> &'a T {
        unsafe { self.0.as_ref() }
    }

    /// # Safety
    ///
    /// The entity must be alive for `'a`, and not be accessed otherwise meanwhile.
    pub unsafe fn get_mut<'a>(mut self) -> &'a mut T {
        unsafe { self.0.as_mut() }
    }
}

/// The size of the cell of an entity kind.
pub fn entity_cell_size(kind: usize) -> usize {
    let size = match kind {
        ENTITY_FUNCTION => size_of::<FunctionInstance>(),
        ENTITY_TABLE => size_of::<TableInstance>(),
        ENTITY_MEMORY => size_of::<MemoryInstance>(),
        ENTITY_GLOBAL => size_of::<GlobalInstance>(),
        ENTITY_TAG => size_of::<TagInstance>(),
        ENTITY_INSTANCE => size_of::<Instance>(),
        _ => unreachable!("there are {ENTITY_KIND_COUNT} entity kinds"),
    };
    (CELL_HEADER_SIZE + size).next_multiple_of(16)
}

/// Make `entity` a cell of `heap`. Garbage is not collected until the entity is in place, but may be right after, so
/// the caller keeps the handle where the collector looks: on the stack, or in a cell it reaches.
pub fn allocate<T: Entity>(heap: &Heap, entity: T) -> Handle<T> {
    const { assert!(align_of::<T>() <= 16, "cells are 16-byte aligned") };
    heap.defer_gc();
    let cell = heap.allocate_cell(CellType::Entity(T::KIND));
    let handle = unsafe {
        cell.cast::<u64>().write(ENTITY_MAGIC | T::KIND as u64);
        let pointer = cell.add(CELL_HEADER_SIZE).cast::<T>();
        pointer.write(entity);
        Handle::from_ptr(pointer)
    };
    heap.undefer_gc();
    handle
}

unsafe fn kind_of(cell: *mut c_void) -> usize {
    let word = unsafe { cell.cast::<u64>().read() };
    debug_assert_eq!(word & !0xffff, ENTITY_MAGIC);
    (word & 0xffff) as usize
}

/// # Safety
///
/// Only LibGC calls this, with a live entity's cell.
pub unsafe extern "C" fn entity_visit_edges(cell: *mut c_void, visitor: *mut c_void) {
    unsafe fn visit<T: Entity>(cell: *mut c_void, visitor: *mut c_void) {
        let handle = unsafe { Handle::<T>::from_cell(cell) }.expect("the cell is an entity of its kind");
        unsafe { handle.get() }.visit_edges(&Visitor::new(visitor));
    }
    unsafe {
        visit_host_object(cell, &Visitor::new(visitor));
        match kind_of(cell) {
            ENTITY_FUNCTION => visit::<FunctionInstance>(cell, visitor),
            ENTITY_TABLE => visit::<TableInstance>(cell, visitor),
            ENTITY_MEMORY => visit::<MemoryInstance>(cell, visitor),
            ENTITY_GLOBAL => visit::<GlobalInstance>(cell, visitor),
            ENTITY_TAG => visit::<TagInstance>(cell, visitor),
            ENTITY_INSTANCE => visit::<Instance>(cell, visitor),
            _ => unreachable!("there are {ENTITY_KIND_COUNT} entity kinds"),
        }
    }
}

/// Drop the entity of a cell, which is going away.
///
/// # Safety
///
/// `cell` must be an entity's cell, which nothing uses afterwards.
pub unsafe extern "C" fn destroy_entity(cell: *mut u8) {
    unsafe fn destroy<T: Entity>(cell: *mut u8) {
        let handle = unsafe { Handle::<T>::from_cell(cell.cast()) }.expect("the cell is an entity of its kind");
        unsafe {
            core::ptr::drop_in_place(handle.as_ptr());
            // A dead cell is no entity, so a stale handle to it is rejected.
            cell.cast::<u64>().write(0);
        }
    }
    unsafe {
        forget_host_object(cell.cast());
        match kind_of(cell.cast()) {
            ENTITY_FUNCTION => destroy::<FunctionInstance>(cell),
            ENTITY_TABLE => destroy::<TableInstance>(cell),
            ENTITY_MEMORY => destroy::<MemoryInstance>(cell),
            ENTITY_GLOBAL => destroy::<GlobalInstance>(cell),
            ENTITY_TAG => destroy::<TagInstance>(cell),
            ENTITY_INSTANCE => destroy::<Instance>(cell),
            _ => unreachable!("there are {ENTITY_KIND_COUNT} entity kinds"),
        }
    }
}

/// # Safety
///
/// Only LibGC calls this, with an entity's cell that is going away.
pub unsafe extern "C" fn entity_destroy(cell: *mut c_void) {
    unsafe { destroy_entity(cell.cast()) };
}

impl Entity for FunctionInstance {
    const KIND: usize = ENTITY_FUNCTION;

    fn visit_edges(&self, visitor: &Visitor) {
        match &self.kind {
            FunctionKind::Wasm { instance, .. } => visitor.visit_cell(instance.cell()),
            FunctionKind::Host(host) => host.visit_edges(visitor),
        }
    }
}

impl Entity for TableInstance {
    const KIND: usize = ENTITY_TABLE;

    fn visit_edges(&self, visitor: &Visitor) {
        for &element in &self.elements {
            visitor.visit_value(element, self.trace);
        }
    }
}

impl Entity for MemoryInstance {
    const KIND: usize = ENTITY_MEMORY;

    fn visit_edges(&self, _: &Visitor) {}
}

impl Entity for GlobalInstance {
    const KIND: usize = ENTITY_GLOBAL;

    fn visit_edges(&self, visitor: &Visitor) {
        visitor.visit_value(self.value[0], self.trace);
    }
}

impl Entity for TagInstance {
    const KIND: usize = ENTITY_TAG;

    fn visit_edges(&self, _: &Visitor) {}
}

impl Entity for Instance {
    const KIND: usize = ENTITY_INSTANCE;

    fn visit_edges(&self, visitor: &Visitor) {
        for function in &self.functions {
            visitor.visit_cell(function.cell());
        }
        for table in &self.tables {
            visitor.visit_cell(table.cell());
        }
        for memory in &self.memories {
            visitor.visit_cell(memory.cell());
        }
        for global in &self.globals {
            visitor.visit_cell(global.cell());
        }
        for tag in &self.tags {
            visitor.visit_cell(tag.cell());
        }
        for (segment, &trace) in self.element_storage.iter().zip(&self.element_traces) {
            for &value in segment {
                visitor.visit_value(value, trace);
            }
        }
    }
}
