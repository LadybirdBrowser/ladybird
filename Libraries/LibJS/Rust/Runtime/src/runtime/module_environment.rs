/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;
use std::collections::HashMap;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::declarative_environment::{
    BindingAndIndex, DECLARATIVE_ENVIRONMENT_METHODS, DeclarativeEnvironment,
};
use crate::runtime::environment::{Environment, EnvironmentMethods};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::module::Module;

#[derive(Clone, Trace)]
struct IndirectBinding {
    module: Option<Gc<Module>>,
    binding_name: Utf16FlyString,
}

#[derive(Default)]
struct IndirectBindings(HashMap<Utf16FlyString, IndirectBinding>);

// SAFETY: Visits the module of every indirect binding.
unsafe impl Trace for IndirectBindings {
    fn trace(&self, visitor: &mut Visitor) {
        for indirect_binding in self.0.values() {
            indirect_binding.trace(visitor);
        }
    }
}

// 9.1.1.5 Module Environment Records, https://tc39.es/ecma262/#sec-module-environment-records
#[repr(C)]
#[derive(Trace)]
pub struct ModuleEnvironment {
    base: DeclarativeEnvironment,
    indirect_bindings: GcRefCell<IndirectBindings>,
}

define_cell!(ModuleEnvironment, Other, extends: [DeclarativeEnvironment, Environment], finalize: finalize);

impl Finalize for ModuleEnvironment {
    fn finalize(&self) {
        self.base.finalize();
    }
}

impl Deref for ModuleEnvironment {
    type Target = DeclarativeEnvironment;

    fn deref(&self) -> &DeclarativeEnvironment {
        &self.base
    }
}

fn module_environment(environment: &Environment) -> &ModuleEnvironment {
    environment
        .downcast_ref::<ModuleEnvironment>()
        .expect("only module environments have the module environment methods")
}

// Note: Module Environment Records support all of the declarative Environment Record methods listed
//       in Table 18 and share the same specifications for all of those methods except for
//       GetBindingValue, DeleteBinding, HasThisBinding and GetThisBinding.
//       In addition, module Environment Records support the methods listed in Table 24.
pub const MODULE_ENVIRONMENT_METHODS: EnvironmentMethods = EnvironmentMethods {
    get_binding_value: |environment, vm, name, strict| {
        module_environment(environment).get_binding_value(vm, name, strict)
    },
    delete_binding: |environment, vm, name| module_environment(environment).delete_binding(vm, name),
    has_this_binding: |environment| module_environment(environment).has_this_binding(),
    get_this_binding: |environment, vm| module_environment(environment).get_this_binding(vm),
    ..DECLARATIVE_ENVIRONMENT_METHODS
};

/// M.[[Environment]] of the module an indirect binding refers to.
fn environment_of_module(module: Gc<Module>) -> Option<Gc<Environment>> {
    module.environment().map(Gc::upcast)
}

impl ModuleEnvironment {
    // 9.1.2.6 NewModuleEnvironment ( E ), https://tc39.es/ecma262/#sec-newmoduleenvironment
    pub fn create(vm: &Vm, outer_environment: Option<Gc<Environment>>) -> Gc<ModuleEnvironment> {
        vm.heap().allocate(ModuleEnvironment {
            base: DeclarativeEnvironment::new(Self::CLASS, outer_environment),
            indirect_bindings: GcRefCell::default(),
        })
    }

    // 9.1.1.5.1 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-module-environment-records-getbindingvalue-n-s
    pub fn get_binding_value(&self, vm: &Vm, name: &Utf16FlyString, strict: bool) -> ThrowCompletionOr<Value> {
        // 1. Assert: S is true.
        assert!(strict);

        // 2. Assert: envRec has a binding for N.
        let indirect_binding = self.get_indirect_binding(name);
        assert!(indirect_binding.is_some() || self.base.has_binding(name, None).is_ok());

        // 3. If the binding for N is an indirect binding, then
        if let Some(indirect_binding) = indirect_binding {
            // a. Let M and N2 be the indirection values provided when this binding for N was created.

            // b. Let targetEnv be M.[[Environment]].
            let target_env =
                environment_of_module(indirect_binding.module.expect("an indirect binding refers to a module"));

            // c. If targetEnv is empty, throw a ReferenceError exception.
            let Some(target_env) = target_env else {
                return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::ModuleNoEnvironment, &[]);
            };

            // d. Return ? targetEnv.GetBindingValue(N2, true).
            return target_env.get_binding_value(vm, &indirect_binding.binding_name, true);
        }

        // 4. If the binding for N in envRec is an uninitialized binding, throw a ReferenceError exception.
        // 5. Return the value currently bound to N in envRec.
        // Note: Step 4 & 5 are the steps performed by declarative environment GetBindingValue
        self.base.get_binding_value(vm, name, strict)
    }

    // 9.1.1.5.2 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-module-environment-records-deletebinding-n
    pub fn delete_binding(&self, _vm: &Vm, _name: &Utf16FlyString) -> ThrowCompletionOr<bool> {
        unreachable!(
            "The DeleteBinding concrete method of a module Environment Record is never used within this specification."
        )
    }

    pub fn has_this_binding(&self) -> bool {
        true
    }

    // 9.1.1.5.4 GetThisBinding ( ), https://tc39.es/ecma262/#sec-module-environment-records-getthisbinding
    pub fn get_this_binding(&self, _vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 9.1.1.5.5 CreateImportBinding ( N, M, N2 ), https://tc39.es/ecma262/#sec-createimportbinding
    pub fn create_import_binding(
        &self,
        name: Utf16FlyString,
        module: Option<Gc<Module>>,
        binding_name: Utf16FlyString,
    ) -> ThrowCompletionOr<()> {
        // 1. Assert: envRec does not already have a binding for N.
        assert!(self.get_indirect_binding(&name).is_none());
        // 2. Assert: When M.[[Environment]] is instantiated it will have a direct binding for N2.
        // FIXME: I don't know what this means or how to check it.

        // 3. Create an immutable indirect binding in envRec for N that references M and N2 as its target binding and record that the binding is initialized.
        // Note: We use the fact that the binding is in this map as it being initialized.
        self.indirect_bindings
            .borrow_mut()
            .0
            .insert(name, IndirectBinding { module, binding_name });

        // 4. Return unused.
        Ok(())
    }

    fn get_indirect_binding(&self, name: &Utf16FlyString) -> Option<IndirectBinding> {
        self.indirect_bindings.borrow().0.get(name).cloned()
    }

    pub(crate) fn find_binding_and_index(&self, name: &Utf16FlyString) -> Option<BindingAndIndex> {
        if let Some(indirect_binding) = self.get_indirect_binding(name) {
            let target_env =
                environment_of_module(indirect_binding.module.expect("an indirect binding refers to a module"))?;

            let target_module_environment = target_env
                .downcast::<ModuleEnvironment>()
                .expect("a module's environment is a module environment");
            let result = target_module_environment.find_binding_and_index(&indirect_binding.binding_name)?;

            // NOTE: We must pretend this binding is actually from this environment
            //       so as specified by
            //       9.1.1.5.5 CreateImportBinding ( N, M, N2 ), https://tc39.es/ecma262/#sec-createimportbinding
            //       It creates a new initialized immutable indirect binding for the
            //       name N. A binding must not already exist in this Environment
            //       Record for N. N2 is the name of a binding that exists in M's
            //       Module Environment Record. Accesses to the value of the new
            //       binding will indirectly access the bound value of the target
            //       binding.
            //       We don't alter the name of the binding as the name is only used
            //       for lookup.
            let mut copy_binding = result.binding(&target_module_environment.base);
            copy_binding.mutable = false;
            copy_binding.can_be_deleted = false;
            copy_binding.initialized = true;
            return Some(BindingAndIndex::Temporary(copy_binding));
        }

        self.base.declarative_find_binding_and_index(name)
    }
}
