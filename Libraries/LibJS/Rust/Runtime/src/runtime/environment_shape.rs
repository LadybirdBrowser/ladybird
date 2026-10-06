/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ptr::NonNull;
use std::collections::HashMap;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::BINDING_FLAG_MUTABLE;
pub use crate::layout::environment::EnvironmentShape;

/// The names of an environment shape's bindings, whose flags the interpreter reads from the shape itself.
pub struct EnvironmentShapeStorage {
    binding_names: Vec<Utf16FlyString>,
    binding_indices: HashMap<Utf16FlyString, usize, foldhash::fast::RandomState>,
}

define_cell!(EnvironmentShape, Other, finalize: finalize);

// SAFETY: A shape holds binding names and flags, and no cells.
unsafe impl Trace for EnvironmentShape {
    fn trace(&self, _: &mut Visitor) {}
}

impl Finalize for EnvironmentShape {
    fn finalize(&self) {
        self.binding_flags.clear();
    }
}

impl EnvironmentShape {
    pub const BINDING_FLAG_STRICT: u8 = 1 << 0;
    pub const BINDING_FLAG_MUTABLE: u8 = BINDING_FLAG_MUTABLE;
    pub const BINDING_FLAG_CAN_BE_DELETED: u8 = 1 << 2;

    fn new(
        binding_names: Vec<Utf16FlyString>,
        binding_flags: InterpreterBuffer<u8>,
        binding_indices: HashMap<Utf16FlyString, usize, foldhash::fast::RandomState>,
    ) -> Self {
        Self {
            header: CellHeader::for_class(Self::CLASS),
            binding_flags,
            storage: EnvironmentShapeStorage {
                binding_names,
                binding_indices,
            },
        }
    }

    pub fn create(vm: &Vm, names: &[Utf16FlyString], flags: &[u8]) -> Gc<EnvironmentShape> {
        assert!(names.len() == flags.len());

        let mut binding_names = Vec::with_capacity(names.len());

        let binding_flags = InterpreterBuffer::new();
        binding_flags.ensure_capacity(flags.len());

        let mut binding_indices = HashMap::with_capacity_and_hasher(names.len(), Default::default());

        for (index, (name, flags)) in names.iter().zip(flags).enumerate() {
            binding_names.push(name.clone());
            binding_flags.append(*flags);

            if !name.is_empty() {
                binding_indices.insert(name.clone(), index);
            }
        }

        vm.heap()
            .allocate(Self::new(binding_names, binding_flags, binding_indices))
    }

    pub fn size(&self) -> usize {
        self.storage.binding_names.len()
    }

    pub fn binding_name(&self, index: usize) -> &Utf16FlyString {
        &self.storage.binding_names[index]
    }

    pub fn binding_flags(&self, index: usize) -> u8 {
        self.binding_flags.get(index)
    }

    pub fn find_binding(&self, name: &Utf16FlyString) -> Option<usize> {
        self.storage.binding_indices.get(name).copied()
    }
}

/// The slot where the environments one piece of code creates find their shape: an entry of an executable's
/// environment shape caches, or the function or var environment shape of a function's shared data. Along with a
/// pointer to the slot, this keeps the cell that owns the slot alive, so that the pointer cannot dangle.
#[derive(Clone, Copy, Trace)]
pub struct EnvironmentShapeCache {
    owner: Gc<CellHeader>,
    #[gc(untraced)]
    slot: NonNull<Cell<Option<Gc<EnvironmentShape>>>>,
}

impl EnvironmentShapeCache {
    /// # Safety
    ///
    /// `slot` must be part of the cell `owner`, or of memory that `owner` owns and only frees when it is destroyed.
    pub unsafe fn new<Owner>(owner: Gc<Owner>, slot: &Cell<Option<Gc<EnvironmentShape>>>) -> Self {
        Self {
            // SAFETY: Every cell starts with its header.
            owner: unsafe { Gc::from_non_null(owner.as_non_null().cast()) },
            slot: NonNull::from(slot),
        }
    }

    pub fn shape(&self) -> Option<Gc<EnvironmentShape>> {
        // SAFETY: The owner keeps the slot allocated, and this keeps the owner alive.
        unsafe { self.slot.as_ref() }.get()
    }

    pub fn set_shape(&self, shape: Gc<EnvironmentShape>) {
        // SAFETY: As above.
        unsafe { self.slot.as_ref() }.set(Some(shape));
    }
}
