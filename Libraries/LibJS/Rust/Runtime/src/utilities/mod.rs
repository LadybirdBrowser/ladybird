/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The command line tools built on the runtime, which their C++ mains in Utilities/ call into.

pub mod js;
pub mod line_editor;
pub mod test262_runner;
pub mod test_js;

use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::completion::Must;
use crate::runtime::realm::Realm;

/// The execution context InitializeHostDefinedRealm pushes, which stays the bottom of the execution context stack for
/// as long as a tool runs scripts in its realm, like the one the C++ tools keep.
pub(crate) struct RootExecutionContext<'vm> {
    vm: &'vm Vm,
    context: OwnedExecutionContext,
}

impl RootExecutionContext<'_> {
    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.context
            .realm
            .get()
            .expect("the root execution context has a realm")
    }
}

impl Drop for RootExecutionContext<'_> {
    fn drop(&mut self) {
        // The contexts above the root one are only left behind when a test unwinds out of a call.
        while self.vm.pop_execution_context() != self.context.as_non_null() {}
    }
}

/// create_simple_execution_context<GlobalObjectType>(): the realm a tool runs scripts in, whose global object
/// `create_global_object` allocates.
pub(crate) fn initialize_realm_with_global_object<'vm>(
    vm: &'vm Vm,
    create_global_object: &dyn Fn(Gc<Realm>) -> Gc<Object>,
) -> RootExecutionContext<'vm> {
    let context = Realm::initialize_host_defined_realm(vm, Some(create_global_object), None).must();
    RootExecutionContext { vm, context }
}
