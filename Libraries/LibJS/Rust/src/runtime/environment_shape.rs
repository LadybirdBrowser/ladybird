/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use std::collections::HashMap;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::BINDING_FLAG_MUTABLE;
pub use crate::layout::environment::EnvironmentShape;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;

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
            // NB: The names stay where they are when the storage moves into the cell.
            binding_names: binding_names.as_ptr().cast(),
            has_unique_binding_names: binding_indices.len() == binding_names.len(),
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
            .allocate(|_| Self::new(binding_names, binding_flags, binding_indices))
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

/// The slot where the environments one piece of code creates find their shape, which this keeps alive along with the
/// cell that owns it.
#[derive(Clone, Copy, Trace)]
pub enum EnvironmentShapeCache {
    /// An entry of an executable's environment shape caches.
    Executable(Gc<Executable>, #[gc(untraced)] u32),
    /// The function environment shape of a function's shared data.
    FunctionEnvironment(Gc<SharedFunctionInstanceData>),
    /// The var environment shape of a function's shared data.
    VarEnvironment(Gc<SharedFunctionInstanceData>),
}

impl EnvironmentShapeCache {
    fn slot(&self) -> &Cell<Option<Gc<EnvironmentShape>>> {
        match self {
            Self::Executable(executable, index) => executable.environment_shape_cache_slot(*index),
            Self::FunctionEnvironment(shared_data) => shared_data.function_environment_shape_slot(),
            Self::VarEnvironment(shared_data) => shared_data.var_environment_shape_slot(),
        }
    }

    pub fn shape(&self) -> Option<Gc<EnvironmentShape>> {
        self.slot().get()
    }

    pub fn set_shape(&self, shape: Gc<EnvironmentShape>) {
        self.slot().set(Some(shape));
    }
}
