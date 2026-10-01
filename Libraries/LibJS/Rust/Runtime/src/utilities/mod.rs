/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The command line tools built on the runtime, which their C++ mains in Utilities/ call into.

pub mod js;
pub mod test262_runner;

use crate::gc::class::GcCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::runtime::global_environment::GlobalEnvironment;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, Object, allocate_object};
use crate::runtime::realm::Realm;

/// The realm the tools run scripts in until realms have intrinsics: the steps of InitializeHostDefinedRealm that need
/// none, with a global object that has no prototype yet. Its execution context stays the bottom of the execution
/// context stack for as long as the VM exists, like the one the C++ tools keep.
pub(crate) fn initialize_realm_without_intrinsics(vm: &Vm) -> Gc<Realm> {
    // 1. Let realm be a new Realm Record
    let realm = Realm::create(vm);

    // 7. Let newContext be a new execution context.
    let new_context = vm
        .interpreter_stack()
        .allocate(0, 0, 0)
        .expect("the interpreter stack has room for the realm's execution context");

    // 8. Set the Function of newContext to null.
    // 9. Set the Realm of newContext to realm.
    // SAFETY: The context was just allocated, and nothing frees the bottom of the interpreter stack.
    unsafe { new_context.as_ref() }.realm.set(Some(realm));

    // 10. Set the ScriptOrModule of newContext to null.

    // 11. Push newContext onto the execution context stack; newContext is now the running execution context.
    vm.push_execution_context(new_context);

    // 13. Else,
    //     a. Let global be OrdinaryObjectCreate(realm.[[Intrinsics]].[[%Object.prototype%]]).
    let global = allocate_object(
        vm,
        Object::new_global_object(vm, Object::CLASS, realm, MayInterfereWithIndexedPropertyAccess::No),
    );

    // 15. Else,
    //     a. Let thisValue be global.
    let this_value = global;

    // 16. Set realm.[[GlobalObject]] to global.
    realm.set_global_object(global);

    // 17. Set realm.[[GlobalEnv]] to NewGlobalEnvironment(global, thisValue).
    realm.set_global_environment(GlobalEnvironment::create(vm, global, this_value));

    realm
}
