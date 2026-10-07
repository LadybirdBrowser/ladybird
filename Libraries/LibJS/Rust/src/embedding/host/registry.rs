/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime class of each host class table.
//!
//! The VM derives a class for a table the first time it sees it. The class extends the class of the table's parent, or
//! the root class of its kind for a table without a parent of its kind, so that Class::is_subclass_of() follows the
//! chain of tables. Its name is the table's, and its internal methods come from the table's hooks.

use std::collections::HashMap;

use crate::embedding::host::host_array::{HostArray, derive_host_array_class};
use crate::embedding::host::host_function::{HostFunction, derive_host_function_class};
use crate::embedding::host::host_module::{HostModule, derive_host_module_class};
use crate::embedding::host::host_object::{HostObject, derive_host_object_class};
use crate::gc::class::{Class, GcCell};
use crate::gc::heap::RuntimeClassAllocator;
use crate::interpreter::vm::Vm;
use crate::layout::host_class::{
    JS_HOST_CLASS_ARRAY, JS_HOST_CLASS_FUNCTION, JS_HOST_CLASS_MODULE, JS_HOST_CLASS_OBJECT, JSHostClass,
};

/// What the VM derived for a host class table.
#[derive(Clone, Copy)]
pub struct RegisteredHostClass {
    class: &'static Class,
    /// The allocator of the table's cells, which the first of them creates. A table that only ever is the parent of
    /// others has none.
    allocator: Option<RuntimeClassAllocator>,
}

/// The classes derived for host class tables, by the address of their JSHostClass.
pub type HostClassRegistry = HashMap<usize, RegisteredHostClass, foldhash::fast::RandomState>;

fn root_class_of_kind(kind: u8) -> &'static Class {
    match kind {
        JS_HOST_CLASS_OBJECT => HostObject::CLASS,
        JS_HOST_CLASS_FUNCTION => HostFunction::CLASS,
        JS_HOST_CLASS_ARRAY => HostArray::CLASS,
        JS_HOST_CLASS_MODULE => HostModule::CLASS,
        kind => panic!("{kind} is not a kind of host class"),
    }
}

fn derive_class_of_kind(table: &'static JSHostClass, parent: &'static Class) -> &'static Class {
    match table.kind {
        JS_HOST_CLASS_OBJECT => derive_host_object_class(table, parent),
        JS_HOST_CLASS_FUNCTION => derive_host_function_class(table, parent),
        JS_HOST_CLASS_ARRAY => derive_host_array_class(table, parent),
        JS_HOST_CLASS_MODULE => derive_host_module_class(table, parent),
        kind => panic!("{kind} is not a kind of host class"),
    }
}

/// The runtime class of `table`.
pub fn runtime_class_of_host_class(vm: &Vm, table: &'static JSHostClass) -> &'static Class {
    let registered = vm.host_classes().borrow().get(&table.identity()).copied();
    if let Some(registered) = registered {
        return registered.class;
    }

    table.validate(table.kind);
    let parent = match table.parent_class() {
        Some(parent) if parent.kind == table.kind => runtime_class_of_host_class(vm, parent),
        _ => root_class_of_kind(table.kind),
    };
    let class = derive_class_of_kind(table, parent);
    vm.host_classes()
        .borrow_mut()
        .insert(table.identity(), RegisteredHostClass { class, allocator: None });
    class
}

/// The runtime class of `table`, which must be of `kind`, and the allocator its objects come from: the class's own,
/// or with JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT, that of its nearest ancestor that has one of its own.
pub fn runtime_class_and_allocator_of_host_class(
    vm: &Vm,
    table: &'static JSHostClass,
    kind: u8,
) -> (&'static Class, RuntimeClassAllocator) {
    assert!(
        table.kind == kind,
        "the host class {} is of kind {}, not {kind}",
        table.class_name(),
        table.kind
    );
    let registered = vm.host_classes().borrow().get(&table.identity()).copied();
    if let Some(RegisteredHostClass {
        class,
        allocator: Some(allocator),
    }) = registered
    {
        return (class, allocator);
    }

    let class = runtime_class_of_host_class(vm, table);
    let allocator = vm
        .heap()
        .runtime_class_allocator(runtime_class_of_host_class(vm, table.allocating_class()));
    vm.host_classes()
        .borrow_mut()
        .get_mut(&table.identity())
        .expect("the class was registered above")
        .allocator = Some(allocator);
    (class, allocator)
}
