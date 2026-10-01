/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The command line tools built on the runtime, which their C++ mains in Utilities/ call into.

pub mod js;
pub mod test262_runner;

use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
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

/// The realm the tools run scripts in, with a GlobalObject as its global object.
pub(crate) fn initialize_realm(vm: &Vm) -> RootExecutionContext<'_> {
    let context = Realm::initialize_host_defined_realm(vm, None, None).must();
    RootExecutionContext { vm, context }
}
