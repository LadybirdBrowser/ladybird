/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What installed JIT code depends on (see `libjs_jit::code::Dependency`), and its invalidation when that stops
//! holding.
//!
//! Code is only installed if everything it depends on still holds then, which also covers what stopped holding while
//! it compiled. Each kind of dependency stops holding in one place in the runtime, which calls
//! `invalidate_dependents()`: the code is invalidated, so that every frame running it exits where it next relies on
//! its dependencies, and discarded, so that nothing enters it anymore.

use core::cell::RefCell;
use core::ops::Range;
use core::ptr::NonNull;
use std::collections::HashMap;

pub use libjs_jit::code::Dependency;
pub use libjs_jit::snapshot::CellId;

use super::code::CompileState;
use crate::bytecode::executable::Executable;
use crate::gc::sweep::Sweep;
use crate::gc::weak::{GcWeak, SweptAfterCollection};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::DeclarativeEnvironment;
use crate::layout::object::Object;
use crate::layout::shape::WatchpointSet;
use crate::runtime::shape::Shape;

/// Installed code that depends on something, as its executable and its id. Once the executable died or has other
/// code by now, the code is gone.
struct DependentCode {
    executable: GcWeak<Executable>,
    id: u64,
}

/// The installed code that depends on a dependency, with the cell the dependency names, if any.
struct DependentEntry {
    /// The cell of the dependency, whose slot a collection that moves the cell rewrites. The table is keyed by the
    /// address of the cell, so it takes the new address once the collection is over (see `Dependents::rekey()`).
    cell: Option<Gc<CellHeader>>,
    code: Vec<DependentCode>,
}

/// The installed code that depends on each dependency. Entries for code that is gone are stale and skipped.
#[derive(Default)]
pub struct Dependents {
    code: RefCell<HashMap<Dependency, DependentEntry>>,
    /// The slots of the named properties of global objects that may have been assigned, as bit sets by the object,
    /// which only VMs that compile note (see `Dependency::GlobalPropertyUnassigned`). The table is keyed by the
    /// identity of the objects, which moves do not change, and a collection that moves an object rewrites its key.
    ///
    /// A slot is assigned when the runtime changes a value other than undefined in it to another one, or deletes its
    /// property or one before it, which moves the property. Giving a slot its first value that is not undefined, as
    /// the initializer of a `var` does, or a global object computing an intrinsic the first time it is read, is no
    /// assignment, and neither is storing the value it holds, as CreateGlobalFunctionBinding does after defining the
    /// property.
    ///
    /// Nothing but the runtime (`Object::replace_direct()` and `Object::storage_delete()`) writes to the slots of a
    /// global object that are not assigned yet, while its shape is a dictionary: property lookup caches only write to
    /// slots whose writes they cached, and the runtime caches no write to such a slot (see
    /// `global_object_slot_may_have_been_assigned()`). And no other object ever gets one of the shapes of a global
    /// object once it has a dictionary shape, which belongs to it alone, as do the shapes it gets from that one, so
    /// that writes the caches made for other objects never apply to it.
    assigned_global_object_slots: RefCell<HashMap<Gc<Object>, Vec<u64>>>,
}

// SAFETY: The VM's sweep callback calls remove_dead_entries() after each collection, which forgets the global objects
//         that died.
unsafe impl SweptAfterCollection for Dependents {}

impl Dependents {
    /// Records that the code with `id` installed for `executable` depends on `dependencies`.
    pub fn register(&self, vm: &Vm, executable: Gc<Executable>, id: u64, dependencies: &[Dependency]) {
        if dependencies.is_empty() {
            return;
        }
        let mut code = self.code.borrow_mut();
        let weak_executable = GcWeak::new(vm.heap(), executable);
        for dependency in dependencies {
            match dependency {
                Dependency::GlobalDeclarations { environment, .. }
                | Dependency::GlobalBindingUnassigned { environment, .. } => {
                    declarative_environment(*environment).set_has_dependent_code();
                }
                Dependency::StableShape(shape) => shape_of(*shape).set_has_dependent_code(),
                Dependency::WatchpointSetValid(_)
                | Dependency::GlobalPropertyUnassigned { .. }
                | Dependency::NoHtmlDdaObjects => {}
            }
            code.entry(*dependency)
                .or_insert_with(|| DependentEntry {
                    // SAFETY: Code that is being installed depends only on live cells, which its compile job keeps
                    //         alive.
                    cell: cell_of(dependency).map(|cell| unsafe { gc_of(cell) }),
                    code: Vec::new(),
                })
                .code
                .push(DependentCode {
                    executable: weak_executable.clone(),
                    id,
                });
        }
    }

    /// Forgets the entries of code that is gone, the dependencies on cells that died, and the slots of global objects
    /// that died.
    ///
    /// NB: Code may lose a cell it depends on while its executable lives on, as it holds shapes and objects only while
    ///     their realms are alive (see `JitCode::embedded_cells`). Such code is dropped earlier in this callback, so
    ///     its entries go with it, before another cell can take the cell's address.
    pub fn remove_dead_entries(&self, vm: &Vm, sweep: &Sweep<'_>) {
        let mut code = self.code.borrow_mut();
        code.retain(|_, entry| {
            entry.code.retain(|dependent| {
                dependent
                    .executable
                    .get()
                    .and_then(|executable| executable.jit_code(vm))
                    .is_some_and(|code| code.id() == dependent.id)
            });
            !entry.code.is_empty() && !entry.cell.is_some_and(|cell| sweep.is_dead(cell))
        });
        if sweep.moves_cells() && !code.is_empty() {
            for entry in code.values() {
                if let Some(cell) = &entry.cell {
                    // SAFETY: Only registering code and invalidating it change the table, which nothing does until
                    //         the collection is over, when rekey() takes the new addresses.
                    unsafe { sweep.record_slot(cell) };
                }
            }
            vm.jit.relocate_code_once_collection_is_over(vm);
        }
        drop(code);
        let mut assigned_global_object_slots = self.assigned_global_object_slots.borrow_mut();
        assigned_global_object_slots.retain(|object, _| !sweep.is_dead(*object));
        if sweep.moves_cells() {
            for object in assigned_global_object_slots.keys() {
                // SAFETY: Only note_global_object_slot_assignments() changes the table, which nothing calls until the
                //         collection is over.
                unsafe { sweep.record_slot(object) };
            }
        }
    }
}

impl Dependents {
    /// Keys the entries by the addresses their cells have now, once a collection that moved cells is over.
    pub fn rekey(&self) {
        let mut code = self.code.borrow_mut();
        let moved = |dependency: &Dependency, entry: &DependentEntry| {
            entry.cell.map(|cell| CellId::from_address(cell.as_ptr() as u64)) != cell_of(dependency)
        };
        if !code.iter().any(|(dependency, entry)| moved(dependency, entry)) {
            return;
        }
        *code = code
            .drain()
            .map(|(dependency, entry)| {
                let dependency = match entry.cell {
                    Some(cell) => with_cell(dependency, CellId::from_address(cell.as_ptr() as u64)),
                    None => dependency,
                };
                (dependency, entry)
            })
            .collect();
    }
}

/// `dependency`, naming `cell` in place of the cell it names.
fn with_cell(dependency: Dependency, cell: CellId) -> Dependency {
    match dependency {
        Dependency::WatchpointSetValid(_) => Dependency::WatchpointSetValid(cell),
        Dependency::StableShape(_) => Dependency::StableShape(cell),
        Dependency::GlobalDeclarations { serial, .. } => Dependency::GlobalDeclarations {
            environment: cell,
            serial,
        },
        Dependency::GlobalBindingUnassigned { index, .. } => Dependency::GlobalBindingUnassigned {
            environment: cell,
            index,
        },
        Dependency::GlobalPropertyUnassigned { offset, .. } => {
            Dependency::GlobalPropertyUnassigned { object: cell, offset }
        }
        Dependency::NoHtmlDdaObjects => Dependency::NoHtmlDdaObjects,
    }
}

/// The cell `cell` names.
///
/// # Safety
///
/// `cell` must be the address of a live cell.
unsafe fn gc_of(cell: CellId) -> Gc<CellHeader> {
    // SAFETY: The caller vouches for the cell.
    unsafe { Gc::from_non_null(NonNull::new_unchecked(cell.address() as *mut CellHeader)) }
}

/// Whether slot `offset` of the global object `object` may have been assigned (see
/// `Dependents::assigned_global_object_slots`). In VMs that do not compile, every slot may have been.
pub fn global_object_slot_may_have_been_assigned(vm: &Vm, object: &Object, offset: u32) -> bool {
    if !vm.jit.options.enabled {
        return true;
    }
    let slots = vm.jit.dependents.assigned_global_object_slots.borrow();
    slots
        .get(&Gc::from_ref(object))
        .and_then(|bits| bits.get(offset as usize / 64))
        .is_some_and(|word| word & (1 << (offset % 64)) != 0)
}

/// Notes that the runtime assigns the slots `offsets` of the global object `object`, which invalidates the code that
/// depends on them staying unassigned.
pub fn note_global_object_slot_assignments(vm: &Vm, object: &Object, offsets: Range<u32>) {
    if !vm.jit.options.enabled {
        return;
    }
    for offset in offsets {
        {
            let mut slots = vm.jit.dependents.assigned_global_object_slots.borrow_mut();
            let bits = slots.entry(Gc::from_ref(object)).or_default();
            let word = offset as usize / 64;
            if bits.len() <= word {
                bits.resize(word + 1, 0);
            }
            if bits[word] & (1 << (offset % 64)) != 0 {
                continue;
            }
            bits[word] |= 1 << (offset % 64);
        }
        invalidate_dependents(
            vm,
            Dependency::GlobalPropertyUnassigned {
                object: CellId::from_address(core::ptr::from_ref(object) as u64),
                offset,
            },
        );
    }
}

/// The cell a dependency names, if it names one.
fn cell_of(dependency: &Dependency) -> Option<CellId> {
    match *dependency {
        Dependency::WatchpointSetValid(cell) | Dependency::StableShape(cell) => Some(cell),
        Dependency::GlobalDeclarations { environment, .. }
        | Dependency::GlobalBindingUnassigned { environment, .. } => Some(environment),
        Dependency::GlobalPropertyUnassigned { object, .. } => Some(object),
        Dependency::NoHtmlDdaObjects => None,
    }
}

/// The global declarative environment a dependency names.
fn declarative_environment(environment: CellId) -> &'static DeclarativeEnvironment {
    // SAFETY: Only code that is being installed asks, and its compile job keeps the cells it depends on alive.
    unsafe { &*(environment.address() as *const DeclarativeEnvironment) }
}

/// Whether `dependency` still holds.
pub fn holds(vm: &Vm, dependency: &Dependency) -> bool {
    match dependency {
        Dependency::WatchpointSetValid(validity) => {
            // SAFETY: As in declarative_environment().
            unsafe { &*(validity.address() as *const WatchpointSet) }.is_valid()
        }
        Dependency::GlobalDeclarations { environment, serial } => {
            declarative_environment(*environment).environment_serial_number() == *serial
        }
        Dependency::GlobalBindingUnassigned { environment, index } => {
            !declarative_environment(*environment).binding_may_have_been_assigned(*index as usize)
        }
        Dependency::GlobalPropertyUnassigned { object, offset } => {
            // SAFETY: As in declarative_environment().
            !global_object_slot_may_have_been_assigned(vm, unsafe { &*(object.address() as *const Object) }, *offset)
        }
        Dependency::NoHtmlDdaObjects => !vm.jit.htmldda_objects_exist(),
        Dependency::StableShape(shape) => shape_of(*shape).is_stable(),
    }
}

/// The shape a dependency names.
fn shape_of(shape: CellId) -> &'static Shape {
    // SAFETY: As in declarative_environment().
    unsafe { &*(shape.address() as *const Shape) }
}

/// Notes that an object with the `[[IsHTMLDDA]]` internal slot exists, which invalidates the code that depends on
/// none existing.
pub fn note_htmldda_object(vm: &Vm) {
    if vm.jit.htmldda_objects_exist.replace(true) {
        return;
    }
    invalidate_dependents(vm, Dependency::NoHtmlDdaObjects);
}

/// Invalidates the code that depends on a dependency chosen at random, as if it stopped holding, for the
/// "stress-invalidate" option. Invalidating code is always correct: frames running it exit to the interpreter, and the
/// recompiles depend on the same things again.
pub fn invalidate_random_dependency(vm: &Vm) {
    let mut dependencies: Vec<Dependency> = vm.jit.dependents.code.borrow().keys().copied().collect();
    if dependencies.is_empty() {
        return;
    }
    // NB: Sorted, so that the choice only depends on the seed and the addresses of the cells.
    dependencies.sort_unstable();
    let index = vm
        .jit
        .stress_random
        .between(0, u32::try_from(dependencies.len() - 1).unwrap_or(u32::MAX));
    let dependency = dependencies[index as usize];
    vm.jit.count_coverage("stress-invalidate");
    invalidate_dependents(vm, dependency);
}

/// Invalidates and discards the code that depends on `dependency`, which no longer holds.
pub fn invalidate_dependents(vm: &Vm, dependency: Dependency) {
    let dependents = {
        let mut code = vm.jit.dependents.code.borrow_mut();
        if code.is_empty() {
            return;
        }
        code.remove(&dependency)
    };
    let Some(dependents) = dependents else {
        return;
    };
    for DependentCode { executable, id } in dependents.code {
        let Some(executable) = executable.get() else {
            continue;
        };
        // NB: The executable may have dropped the code since, or have other code by now.
        let Some(code) = executable.jit_code(vm).filter(|code| code.id() == id) else {
            continue;
        };
        if vm.jit.options.log_exits {
            eprintln!(
                "JIT invalidate: {}, {dependency:?} no longer holds",
                super::describe_executable(&executable)
            );
        }
        code.invalidate();
        if executable.jit_compile_state() == CompileState::Installed {
            executable.discard_jit_code(vm);
        }
    }
}
