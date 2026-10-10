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
use crate::gc::heap::cell_is_dead;
use crate::gc::weak::GcWeak;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::environment::DeclarativeEnvironment;
use crate::layout::object::Object;
use crate::layout::shape::PrototypeChainValidity;
use crate::runtime::shape::Shape;

/// Installed code that depends on something, as its executable and its id. Once the executable died or has other
/// code by now, the code is gone.
struct DependentCode {
    executable: GcWeak<Executable>,
    id: u64,
}

/// The installed code that depends on each dependency. Entries for code that is gone are stale and skipped.
#[derive(Default)]
pub struct Dependents {
    code: RefCell<HashMap<Dependency, Vec<DependentCode>>>,
    /// The slots of the named properties of global objects that may have been assigned, as bit sets by the address of
    /// the object, which only VMs that compile note (see `Dependency::GlobalPropertyUnassigned`).
    ///
    /// A slot is assigned when the runtime changes a value other than undefined in it to another one, or deletes its
    /// property or one before it, which moves the property. Giving a slot its first value that is not undefined, as
    /// the initializer of a `var` does, or a global object computing an intrinsic the first time it is read, is no
    /// assignment, and neither is storing the value it holds, as CreateGlobalFunctionBinding does after defining the
    /// property.
    ///
    /// Nothing but the runtime (`Object::storage_set()` and `Object::storage_delete()`) writes to the slots of a global
    /// object that are not assigned yet, while its shape is a dictionary: property lookup caches only write to slots
    /// whose writes they cached, and the runtime caches no write to such a slot (see
    /// `global_object_slot_may_have_been_assigned()`). And no other object ever gets one of the shapes of a global
    /// object once it has a dictionary shape, which belongs to it alone, as do the shapes it gets from that one, so
    /// that writes the caches made for other objects never apply to it.
    assigned_global_object_slots: RefCell<HashMap<usize, Vec<u64>>>,
}

impl Dependents {
    /// Records that the code with `id` installed for `executable` depends on `dependencies`.
    pub fn register(&self, vm: &Vm, executable: Gc<Executable>, id: u64, dependencies: &[Dependency]) {
        let mut code = self.code.borrow_mut();
        for dependency in dependencies {
            match dependency {
                Dependency::GlobalDeclarations { environment, .. }
                | Dependency::GlobalBindingUnassigned { environment, .. } => {
                    declarative_environment(*environment).set_has_dependent_code();
                }
                Dependency::StableShape(shape) => shape_of(*shape).set_has_dependent_code(),
                Dependency::PrototypeChainValid(_)
                | Dependency::GlobalPropertyUnassigned { .. }
                | Dependency::NoHtmlDdaObjects => {}
            }
            code.entry(*dependency).or_default().push(DependentCode {
                executable: GcWeak::new(vm.heap(), executable),
                id,
            });
        }
    }

    /// Forgets the entries of code whose executable died, and the slots of global objects that died.
    pub fn remove_dead_entries(&self) {
        self.code.borrow_mut().retain(|_, dependents| {
            dependents.retain(|dependent| dependent.executable.get().is_some());
            !dependents.is_empty()
        });
        self.assigned_global_object_slots.borrow_mut().retain(|&object, _| {
            // SAFETY: The map only holds objects that were live when their entries were added, and a dead object is
            //         intact until the sweep that follows this callback.
            let object = unsafe { Gc::from_non_null(NonNull::new_unchecked(object as *mut Object)) };
            !cell_is_dead(object)
        });
    }
}

/// Whether slot `offset` of the global object `object` may have been assigned (see
/// `Dependents::assigned_global_object_slots`). In VMs that do not compile, every slot may have been.
pub fn global_object_slot_may_have_been_assigned(vm: &Vm, object: &Object, offset: u32) -> bool {
    if !vm.jit.options.enabled {
        return true;
    }
    let slots = vm.jit.dependents.assigned_global_object_slots.borrow();
    slots
        .get(&(core::ptr::from_ref(object) as usize))
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
            let bits = slots.entry(core::ptr::from_ref(object) as usize).or_default();
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
                object: CellId(core::ptr::from_ref(object) as u64),
                offset,
            },
        );
    }
}

/// The global declarative environment a dependency names.
fn declarative_environment(environment: CellId) -> &'static DeclarativeEnvironment {
    // SAFETY: Code that depends on the environment embeds it, which keeps it alive.
    unsafe { &*(environment.0 as *const DeclarativeEnvironment) }
}

/// Whether `dependency` still holds.
pub fn holds(vm: &Vm, dependency: &Dependency) -> bool {
    match dependency {
        Dependency::PrototypeChainValid(validity) => {
            // SAFETY: Code that depends on the cell embeds it, which keeps it alive.
            unsafe { &*(validity.0 as *const PrototypeChainValidity) }.is_valid()
        }
        Dependency::GlobalDeclarations { environment, serial } => {
            declarative_environment(*environment).environment_serial_number() == *serial
        }
        Dependency::GlobalBindingUnassigned { environment, index } => {
            !declarative_environment(*environment).binding_may_have_been_assigned(*index as usize)
        }
        Dependency::GlobalPropertyUnassigned { object, offset } => {
            // SAFETY: Code that depends on the object embeds it, which keeps it alive.
            !global_object_slot_may_have_been_assigned(vm, unsafe { &*(object.0 as *const Object) }, *offset)
        }
        Dependency::NoHtmlDdaObjects => !vm.jit.htmldda_objects_exist(),
        Dependency::StableShape(shape) => shape_of(*shape).is_stable(),
    }
}

/// The shape a dependency names.
fn shape_of(shape: CellId) -> &'static Shape {
    // SAFETY: Code that depends on the shape embeds it, which keeps it alive.
    unsafe { &*(shape.0 as *const Shape) }
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
    for DependentCode { executable, id } in dependents {
        let Some(executable) = executable.get() else {
            continue;
        };
        // NB: The executable may have dropped the code since, or have other code by now.
        let Some(code) = executable.jit_code().filter(|code| code.id() == id) else {
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
