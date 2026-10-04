/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{all_import_attributes_supported, call_function_object};
use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::module::{
    GraphLoadingState, MODULE_METHODS, Module, ModuleMethods, ModuleStack, module_stack_contains,
};
use crate::runtime::module_loading::{ImportedModulePayload, ImportedModuleReferrer};
use crate::runtime::module_request::{LoadedModuleRequest, ModuleRequest, module_requests_equal};
use crate::runtime::native_function::NativeFunction;
use crate::runtime::promise::Promise;
use crate::runtime::promise_capability::{PromiseCapability, new_promise_capability};
use crate::runtime::source_text_module::SourceTextModule;
use crate::runtime::value::same_value;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Trace)]
pub enum ModuleStatus {
    New,
    Unlinked,
    Linking,
    Linked,
    Evaluating,
    EvaluatingAsync,
    Evaluated,
}

// 16.2.1.5 Cyclic Module Records, https://tc39.es/ecma262/#cyclic-module-record
#[repr(C)]
#[derive(Trace)]
pub struct CyclicModule {
    base: Module,
    status: Cell<ModuleStatus>,                                // [[Status]]
    evaluation_error: Cell<Option<Value>>,                     // [[EvaluationError]]
    dfs_index: Cell<Option<u32>>,                              // [[DFSIndex]]
    dfs_ancestor_index: Cell<Option<u32>>,                     // [[DFSAncestorIndex]]
    requested_modules: Vec<ModuleRequest>,                     // [[RequestedModules]]
    loaded_modules: GcRefCell<Vec<LoadedModuleRequest>>,       // [[LoadedModules]]
    cycle_root: Cell<Option<Gc<CyclicModule>>>,                // [[CycleRoot]]
    has_top_level_await: bool,                                 // [[HasTLA]]
    async_evaluation_order: Cell<Option<u64>>,                 // [[AsyncEvaluationOrder]]
    top_level_capability: Cell<Option<Gc<PromiseCapability>>>, // [[TopLevelCapability]]
    async_parent_modules: GcRefCell<Vec<Gc<CyclicModule>>>,    // [[AsyncParentModules]]
    pending_async_dependencies: Cell<Option<u32>>,             // [[PendingAsyncDependencies]]
}

define_cell!(CyclicModule, Other, extends: [Module]);

impl Deref for CyclicModule {
    type Target = Module;

    fn deref(&self) -> &Module {
        &self.base
    }
}

// NOTE: Do not call these methods directly unless you are HostResolveImportedModule.
//       Badges cannot be used because other hosts must be able to call this (and it is called recursively)
pub const CYCLIC_MODULE_METHODS: ModuleMethods = ModuleMethods {
    link: |module, vm| as_cyclic_module(module).link(vm),
    evaluate: |module, vm| as_cyclic_module(module).evaluate(vm),
    load_requested_modules: |module, vm, host_defined| {
        as_cyclic_module(module).load_requested_modules(vm, host_defined)
    },
    inner_module_linking: |module, vm, stack, index| as_cyclic_module(module).inner_module_linking(vm, stack, index),
    inner_module_evaluation: |module, vm, stack, index| {
        as_cyclic_module(module).inner_module_evaluation(vm, stack, index)
    },
    ..MODULE_METHODS
};

/// The Cyclic Module Record a method of a cyclic module was called on.
fn as_cyclic_module(module: &Module) -> &CyclicModule {
    module
        .as_cyclic_module()
        .expect("only Cyclic Module Records have the methods of cyclic modules")
}

pub(crate) fn promise_of(capability: Gc<PromiseCapability>) -> Gc<Promise> {
    capability
        .promise()
        .downcast::<Promise>()
        .expect("the promise of a promise capability for %Promise% is a Promise")
}

fn new_intrinsic_promise_capability(vm: &Vm, realm: Gc<Realm>) -> Gc<PromiseCapability> {
    new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must()
}

impl CyclicModule {
    /// CyclicModule(Realm&, StringView filename, bool has_top_level_await, Vector<ModuleRequest> requested_modules,
    /// GC::Ptr<GC::Cell> host_defined), for `class`, which extends CyclicModule.
    pub fn new(
        class: &'static Class,
        realm: Gc<Realm>,
        filename: String,
        has_top_level_await: bool,
        requested_modules: Vec<ModuleRequest>,
        host_defined: ForeignCellSlot,
    ) -> CyclicModule {
        CyclicModule {
            base: Module::new(class, realm, filename, host_defined),
            status: Cell::new(ModuleStatus::New),
            evaluation_error: Cell::new(None),
            dfs_index: Cell::new(None),
            dfs_ancestor_index: Cell::new(None),
            requested_modules,
            loaded_modules: GcRefCell::new(Vec::new()),
            cycle_root: Cell::new(None),
            has_top_level_await,
            async_evaluation_order: Cell::new(None),
            top_level_capability: Cell::new(None),
            async_parent_modules: GcRefCell::new(Vec::new()),
            pending_async_dependencies: Cell::new(None),
        }
    }

    pub fn as_cyclic_module_gc(&self) -> Gc<CyclicModule> {
        // SAFETY: Modules only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    pub fn status(&self) -> ModuleStatus {
        self.status.get()
    }

    pub fn set_status(&self, status: ModuleStatus) {
        self.status.set(status);
    }

    pub fn has_top_level_await(&self) -> bool {
        self.has_top_level_await
    }

    pub fn requested_modules(&self) -> &[ModuleRequest] {
        &self.requested_modules
    }

    pub fn loaded_modules(&self) -> &GcRefCell<Vec<LoadedModuleRequest>> {
        &self.loaded_modules
    }

    /// The module of the [[LoadedModules]] record for `request`, if there is one.
    fn find_record_in_loaded_modules(&self, request: &ModuleRequest) -> Option<Gc<Module>> {
        self.loaded_modules
            .borrow()
            .iter()
            .find(|record| module_requests_equal(*record, request))
            .map(|record| record.module)
    }

    /// The modules of [[AsyncParentModules]], kept alive by the returned list.
    fn async_parent_modules<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Gc<CyclicModule>> {
        let async_parent_modules = MarkedVec::new(vm);
        for module in self.async_parent_modules.borrow().iter() {
            async_parent_modules.push(*module);
        }
        async_parent_modules
    }

    // 16.2.1.5.1 LoadRequestedModules ( [ hostDefined ] ), https://tc39.es/ecma262/#sec-LoadRequestedModules
    fn load_requested_modules(&self, vm: &Vm, host_defined: ForeignCellSlot) -> Gc<PromiseCapability> {
        // 1. If hostDefined is not present, let hostDefined be EMPTY.
        // NOTE: The empty state is handled by hostDefined being an empty slot.

        // 2. Let pc be ! NewPromiseCapability(%Promise%).
        let realm = vm.current_realm().expect("LoadRequestedModules runs in a realm");
        let promise_capability = new_intrinsic_promise_capability(vm, realm);

        // 3. Let state be the GraphLoadingState Record { [[IsLoading]]: true, [[PendingModulesCount]]: 1, [[Visited]]: « », [[PromiseCapability]]: pc, [[HostDefined]]: hostDefined }.
        let state = GraphLoadingState::create(vm, promise_capability, true, 1, host_defined);

        // 4. Perform InnerModuleLoading(state, module).
        inner_module_loading(vm, state, self.as_gc());

        // NOTE: This is likely a spec bug, see https://matrixlogs.bakkot.com/WHATWG/2023-02-13#L1
        // FIXME: 5. Return pc.[[Promise]].
        promise_capability
    }

    // 16.2.1.5.2 Link ( ), https://tc39.es/ecma262/#sec-moduledeclarationlinking
    fn link(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        // 1. Assert: module.[[Status]] is one of unlinked, linked, evaluating-async, or evaluated.
        assert!(matches!(
            self.status(),
            ModuleStatus::Unlinked | ModuleStatus::Linked | ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
        ));
        // 2. Let stack be a new empty List.
        let stack = ModuleStack::new(vm);

        // 3. Let result be Completion(InnerModuleLinking(module, stack, 0)).
        let result = self.inner_module_linking(vm, &stack, 0);

        // 4. If result is an abrupt completion, then
        if let Err(error) = result {
            // a. For each Cyclic Module Record m of stack, do
            for index in 0..stack.len() {
                let module = stack.get(index).expect("the index is in bounds");
                if let Some(cyclic_module) = module.as_cyclic_module() {
                    // i. Assert: m.[[Status]] is linking.
                    assert_eq!(cyclic_module.status(), ModuleStatus::Linking);

                    // ii. Set m.[[Status]] to unlinked.
                    cyclic_module.set_status(ModuleStatus::Unlinked);
                }
            }
            // b. Assert: module.[[Status]] is unlinked.
            assert_eq!(self.status(), ModuleStatus::Unlinked);

            // c. Return ? result.
            return Err(error);
        }

        // 5. Assert: module.[[Status]] is one of linked, evaluating-async, or evaluated.
        assert!(matches!(
            self.status(),
            ModuleStatus::Linked | ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
        ));
        // 6. Assert: stack is empty.
        assert!(stack.is_empty());

        // 7. Return unused.
        Ok(())
    }

    // 16.2.1.5.1.1 InnerModuleLinking ( module, stack, index ), https://tc39.es/ecma262/#sec-InnerModuleLinking
    fn inner_module_linking(&self, vm: &Vm, stack: &ModuleStack<'_>, mut index: u32) -> ThrowCompletionOr<u32> {
        // 1. If module is not a Cyclic Module Record, then
        //    a. Perform ? module.Link().
        //    b. Return index.
        // Note: Step 1, 1.a and 1.b are handled in Module.cpp

        // 2. If module.[[Status]] is linking, linked, evaluating-async, or evaluated, then
        if matches!(
            self.status(),
            ModuleStatus::Linking | ModuleStatus::Linked | ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
        ) {
            // a. Return index.
            return Ok(index);
        }

        // 3. Assert: module.[[Status]] is unlinked.
        assert_eq!(self.status(), ModuleStatus::Unlinked);

        // 4. Set module.[[Status]] to linking.
        self.set_status(ModuleStatus::Linking);

        // 5. Set module.[[DFSIndex]] to index.
        self.dfs_index.set(Some(index));

        // 6. Set module.[[DFSAncestorIndex]] to index.
        self.dfs_ancestor_index.set(Some(index));

        // 7. Set index to index + 1.
        index += 1;

        // 8. Append module to stack.
        stack.push(self.as_gc());

        // 9. For each ModuleRequest Record request of module.[[RequestedModules]], do
        for request_index in 0..self.requested_modules.len() {
            let request = self.requested_modules[request_index].clone();

            // a. Let requiredModule be GetImportedModule(module, request).
            let required_module = self.get_imported_module(&request);

            // b. Set index to ? InnerModuleLinking(requiredModule, stack, index).
            index = required_module.inner_module_linking(vm, stack, index)?;

            // c. If requiredModule is a Cyclic Module Record, then
            if let Some(cyclic_module) = required_module.as_cyclic_module() {
                // i. Assert: requiredModule.[[Status]] is either linking, linked, evaluating-async, or evaluated.
                assert!(matches!(
                    cyclic_module.status(),
                    ModuleStatus::Linking
                        | ModuleStatus::Linked
                        | ModuleStatus::EvaluatingAsync
                        | ModuleStatus::Evaluated
                ));

                // ii. Assert: requiredModule.[[Status]] is linking if and only if requiredModule is in stack.
                assert_eq!(
                    cyclic_module.status() == ModuleStatus::Linking,
                    module_stack_contains(stack, required_module)
                );

                // iii. If requiredModule.[[Status]] is linking, then
                if cyclic_module.status() == ModuleStatus::Linking {
                    // 1. Set module.[[DFSAncestorIndex]] to min(module.[[DFSAncestorIndex]], requiredModule.[[DFSAncestorIndex]]).
                    self.dfs_ancestor_index
                        .set(Some(self.dfs_ancestor_index().min(cyclic_module.dfs_ancestor_index())));
                }
            }
        }

        // 10. Perform ? module.InitializeEnvironment().
        self.initialize_environment(vm)?;

        // 11. Assert: module occurs exactly once in stack.
        assert_eq!(self.occurrences_in_stack(stack), 1);

        // 12. Assert: module.[[DFSAncestorIndex]] ≤ module.[[DFSIndex]].
        assert!(self.dfs_ancestor_index() <= self.dfs_index());

        // 13. If module.[[DFSAncestorIndex]] = module.[[DFSIndex]], then
        if self.dfs_ancestor_index == self.dfs_index {
            // a. Let done be false.
            // b. Repeat, while done is false,
            loop {
                // i. Let requiredModule be the last element in stack.
                // ii. Remove the last element of stack.
                let required_module = stack.pop().expect("the module is in the stack");

                // iii. Assert: requiredModule is a Cyclic Module Record.
                let cyclic_module = required_module
                    .as_cyclic_module()
                    .expect("only Cyclic Module Records stay on the stack");

                // iv. Set requiredModule.[[Status]] to linked.
                cyclic_module.set_status(ModuleStatus::Linked);

                // v. If requiredModule and module are the same Module Record, set done to true.
                if required_module == self.as_gc() {
                    break;
                }
            }
        }

        // 14. Return index.
        Ok(index)
    }

    fn dfs_index(&self) -> u32 {
        self.dfs_index.get().expect("the module has a DFS index")
    }

    fn dfs_ancestor_index(&self) -> u32 {
        self.dfs_ancestor_index
            .get()
            .expect("the module has a DFS ancestor index")
    }

    fn occurrences_in_stack(&self, stack: &ModuleStack<'_>) -> usize {
        (0..stack.len())
            .filter(|index| stack.get(*index) == Some(self.as_gc()))
            .count()
    }

    // 16.2.1.5.3 Evaluate ( ), https://tc39.es/ecma262/#sec-moduleevaluation
    fn evaluate(&self, vm: &Vm) -> ThrowCompletionOr<Gc<PromiseCapability>> {
        // 1. Assert: This call to Evaluate is not happening at the same time as another call to Evaluate within the surrounding agent.
        // FIXME: Verify this somehow

        // 2. Assert: module.[[Status]] is one of linked, evaluating-async, or evaluated.
        assert!(matches!(
            self.status(),
            ModuleStatus::Linked | ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
        ));

        // 3. If module.[[Status]] is either evaluating-async or evaluated, then
        if matches!(self.status(), ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated)
            && self.cycle_root.get() != Some(self.as_cyclic_module_gc())
        {
            // a. If module.[[CycleRoot]] is not empty, then
            if let Some(cycle_root) = self.cycle_root.get() {
                // i. Set module to module.[[CycleRoot]].
                // NOTE: This will continue this function with module.[[CycleRoot]]
                return cycle_root.evaluate(vm);
            }
            // b. Else,
            // i. Assert: module.[[Status]] is evaluated and module.[[EvaluationError]] is a throw completion.
            assert!(self.status() == ModuleStatus::Evaluated && self.evaluation_error.get().is_some());
        }

        // 4. If module.[[TopLevelCapability]] is not empty, then
        if let Some(top_level_capability) = self.top_level_capability.get() {
            // a. Return module.[[TopLevelCapability]].[[Promise]].
            return Ok(top_level_capability);
        }

        // 5. Let stack be a new empty List.
        let stack = ModuleStack::new(vm);

        let realm = vm.current_realm().expect("Evaluate runs in a realm");

        // 6. Let capability be ! NewPromiseCapability(%Promise%).
        // 7. Set module.[[TopLevelCapability]] to capability.
        let capability = new_intrinsic_promise_capability(vm, realm);
        self.top_level_capability.set(Some(capability));

        // 8. Let result be Completion(InnerModuleEvaluation(module, stack, 0)).
        let result = self.inner_module_evaluation(vm, &stack, 0);

        // 9. If result is an abrupt completion, then
        if let Err(error) = result {
            // a. For each Cyclic Module Record m of stack, do
            for index in 0..stack.len() {
                let module = stack.get(index).expect("the index is in bounds");
                let Some(cyclic_module) = module.as_cyclic_module() else {
                    continue;
                };

                // i. Assert: m.[[Status]] is evaluating.
                assert_eq!(cyclic_module.status(), ModuleStatus::Evaluating);

                // ii. Set m.[[Status]] to evaluated.
                cyclic_module.set_status(ModuleStatus::Evaluated);

                // iii. Set m.[[EvaluationError]] to result.
                cyclic_module.evaluation_error.set(Some(error.value()));
            }

            // b. Assert: module.[[Status]] is evaluated.
            assert_eq!(self.status(), ModuleStatus::Evaluated);

            // c. Assert: module.[[EvaluationError]] is result.
            let evaluation_error = self.evaluation_error.get().expect("the module has an evaluation error");
            assert!(same_value(evaluation_error, error.value()));

            // d. Perform ! Call(capability.[[Reject]], undefined, « result.[[Value]] »).
            call_function_object(vm, capability.reject(), Value::UNDEFINED, &[error.value()]).must();
        }
        // 10. Else,
        else {
            // a. Assert: module.[[Status]] is either evaluating-async or evaluated.
            assert!(matches!(
                self.status(),
                ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
            ));
            // b. Assert: module.[[EvaluationError]] is empty.
            assert!(self.evaluation_error.get().is_none());

            // c. If _module_.[[Status]] is ~evaluated~, then
            if self.status() == ModuleStatus::Evaluated {
                // i. Assert: _module_.[[AsyncEvaluationOrder]] is either ~unset~ or ~done~.
                assert!(self.async_evaluation_order.get().is_none());

                // ii. NOTE: _module_.[[AsyncEvaluationOrder]] is ~done~ if and only if _module_ had already been evaluated and
                //     that evaluation was asynchronous.

                // iii. Perform ! Call(_capability_.[[Resolve]], *undefined*, « *undefined* »).
                call_function_object(vm, capability.resolve(), Value::UNDEFINED, &[Value::UNDEFINED]).must();
            }

            // d. Assert: stack is empty.
            assert!(stack.is_empty());
        }

        // 11. Return capability.[[Promise]].
        // AD-HOC: Return the promise capability and let the caller unwrap the promise
        Ok(capability)
    }

    // 16.2.1.5.2.1 InnerModuleEvaluation ( module, stack, index ), https://tc39.es/ecma262/#sec-innermoduleevaluation
    fn inner_module_evaluation(&self, vm: &Vm, stack: &ModuleStack<'_>, mut index: u32) -> ThrowCompletionOr<u32> {
        // Note: Step 1 is performed in Module.cpp

        // 2. If module.[[Status]] is evaluating-async or evaluated, then
        if matches!(self.status(), ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated) {
            // a. If module.[[EvaluationError]] is empty, return index.
            // b. Otherwise, return ? module.[[EvaluationError]].
            return match self.evaluation_error.get() {
                None => Ok(index),
                Some(error) => Err(Throw::new(error)),
            };
        }

        // 3. If module.[[Status]] is evaluating, return index.
        if self.status() == ModuleStatus::Evaluating {
            return Ok(index);
        }

        // 4. Assert: module.[[Status]] is linked.
        assert_eq!(self.status(), ModuleStatus::Linked);

        // 5. Set module.[[Status]] to evaluating.
        self.set_status(ModuleStatus::Evaluating);

        // 6. Set module.[[DFSIndex]] to index.
        self.dfs_index.set(Some(index));

        // 7. Set module.[[DFSAncestorIndex]] to index.
        self.dfs_ancestor_index.set(Some(index));

        // 8. Set module.[[PendingAsyncDependencies]] to 0.
        self.pending_async_dependencies.set(Some(0));

        // 9. Set index to index + 1.
        index += 1;

        // 10. Append module to stack.
        stack.push(self.as_gc());

        // 11. For each ModuleRequest Record request of module.[[RequestedModules]], do
        for request_index in 0..self.requested_modules.len() {
            let request = self.requested_modules[request_index].clone();

            // a. Let requiredModule be GetImportedModule(module, request).
            let required_module = self.get_imported_module(&request);

            // b. Set index to ? InnerModuleEvaluation(requiredModule, stack, index).
            index = required_module.inner_module_evaluation(vm, stack, index)?;

            // c. If requiredModule is a Cyclic Module Record, then
            let Some(required_cyclic_module) = required_module.as_cyclic_module() else {
                continue;
            };

            let mut cyclic_module = required_cyclic_module.as_cyclic_module_gc();
            // i. Assert: requiredModule.[[Status]] is either evaluating, evaluating-async, or evaluated.
            assert!(matches!(
                cyclic_module.status(),
                ModuleStatus::Evaluating | ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
            ));

            // ii. Assert: requiredModule.[[Status]] is evaluating if and only if requiredModule is in stack.
            assert!(
                cyclic_module.status() != ModuleStatus::Evaluating || module_stack_contains(stack, required_module)
            );

            // iii. If requiredModule.[[Status]] is evaluating, then
            if cyclic_module.status() == ModuleStatus::Evaluating {
                // 1. Set module.[[DFSAncestorIndex]] to min(module.[[DFSAncestorIndex]], requiredModule.[[DFSAncestorIndex]]).
                self.dfs_ancestor_index
                    .set(Some(self.dfs_ancestor_index().min(cyclic_module.dfs_ancestor_index())));
            }
            // iv. Else,
            else {
                // 1. Set requiredModule to requiredModule.[[CycleRoot]].
                cyclic_module = cyclic_module
                    .cycle_root
                    .get()
                    .expect("an evaluated module has a cycle root");

                // 2. Assert: requiredModule.[[Status]] is evaluating-async or evaluated.
                assert!(matches!(
                    cyclic_module.status(),
                    ModuleStatus::EvaluatingAsync | ModuleStatus::Evaluated
                ));

                // 3. If requiredModule.[[EvaluationError]] is not empty, return ? requiredModule.[[EvaluationError]].
                if let Some(error) = cyclic_module.evaluation_error.get() {
                    return Err(Throw::new(error));
                }
            }

            // v. If _requiredModule_.[[AsyncEvaluationOrder]] is an integer, then
            if cyclic_module.async_evaluation_order.get().is_some() {
                // 1. Set module.[[PendingAsyncDependencies]] to module.[[PendingAsyncDependencies]] + 1.
                self.pending_async_dependencies
                    .set(Some(self.pending_async_dependencies() + 1));

                // 2. Append module to requiredModule.[[AsyncParentModules]].
                cyclic_module
                    .async_parent_modules
                    .borrow_mut()
                    .push(self.as_cyclic_module_gc());
            }
        }

        // 12. If module.[[PendingAsyncDependencies]] > 0 or module.[[HasTLA]] is true, then
        if self.pending_async_dependencies() > 0 || self.has_top_level_await {
            // a. Assert: _module_.[[AsyncEvaluationOrder]] is ~unset~.
            assert!(self.async_evaluation_order.get().is_none());

            // b. Set _module_.[[AsyncEvaluationOrder]] to IncrementModuleAsyncEvaluationCount().
            self.async_evaluation_order
                .set(Some(vm.increment_module_async_evaluation_count()));

            // c. If _module_.[[PendingAsyncDependencies]] = 0, perform ExecuteAsyncModule(_module_).
            if self.pending_async_dependencies() == 0 {
                self.execute_async_module(vm);
            }
        }
        // 13. Otherwise, perform ? module.ExecuteModule().
        else {
            self.execute_module(vm, None)?;
        }

        // 14. Assert: module occurs exactly once in stack.
        assert_eq!(self.occurrences_in_stack(stack), 1);

        // 15. Assert: module.[[DFSAncestorIndex]] ≤ module.[[DFSIndex]].
        assert!(self.dfs_ancestor_index() <= self.dfs_index());

        // 16. If module.[[DFSAncestorIndex]] = module.[[DFSIndex]], then
        if self.dfs_ancestor_index == self.dfs_index {
            // a. Let done be false.
            let mut done = false;
            // b. Repeat, while done is false,
            while !done {
                // i. Let requiredModule be the last element in stack.
                // ii. Remove the last element of stack.
                let required_module = stack.pop().expect("the module is in the stack");

                // iii. Assert: requiredModule is a Cyclic Module Record.
                let cyclic_module = required_module
                    .as_cyclic_module()
                    .expect("only Cyclic Module Records stay on the stack");

                // iv. Assert: _requiredModule_.[[AsyncEvaluationOrder]] is either an integer or ~unset~.

                // v. If _requiredModule_.[[AsyncEvaluationOrder]] is ~unset~, set _requiredModule_.[[Status]] to ~evaluated~.
                if cyclic_module.async_evaluation_order.get().is_none() {
                    cyclic_module.set_status(ModuleStatus::Evaluated);
                }
                // vi. Else, set _requiredModule_.[[Status]] to ~evaluating-async~.
                else {
                    cyclic_module.set_status(ModuleStatus::EvaluatingAsync);
                }

                // vii. If _requiredModule_ and _module_ are the same Module Record, set _done_ to *true*.
                if required_module == self.as_gc() {
                    done = true;
                }

                // viii. Set _requiredModule_.[[CycleRoot]] to _module_.
                cyclic_module.cycle_root.set(Some(self.as_cyclic_module_gc()));
            }
        }

        // 17. Return index.
        Ok(index)
    }

    fn pending_async_dependencies(&self) -> u32 {
        self.pending_async_dependencies
            .get()
            .expect("the module has a count of pending async dependencies")
    }

    /// The SourceTextModule a method that only Source Text Module Records override runs on. C++ verifies that the
    /// methods of CyclicModule itself are never reached.
    fn as_source_text_module(&self) -> &SourceTextModule {
        self.downcast_ref::<SourceTextModule>()
            .expect("In ecma262 this is never called on a cyclic module only on SourceTextModules.")
    }

    fn initialize_environment(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        self.as_source_text_module().initialize_environment(vm)
    }

    fn execute_module(&self, vm: &Vm, capability: Option<Gc<PromiseCapability>>) -> ThrowCompletionOr<()> {
        self.as_source_text_module().execute_module(vm, capability)
    }

    // 16.2.1.5.2.2 ExecuteAsyncModule ( module ), https://tc39.es/ecma262/#sec-execute-async-module
    fn execute_async_module(&self, vm: &Vm) {
        let realm = vm.current_realm().expect("ExecuteAsyncModule runs in a realm");

        // 1. Assert: module.[[Status]] is evaluating or evaluating-async.
        assert!(matches!(
            self.status(),
            ModuleStatus::Evaluating | ModuleStatus::EvaluatingAsync
        ));
        // 2. Assert: module.[[HasTLA]] is true.
        assert!(self.has_top_level_await);

        // 3. Let capability be ! NewPromiseCapability(%Promise%).
        let capability = new_intrinsic_promise_capability(vm, realm);

        // 4. Let fulfilledClosure be a new Abstract Closure with no parameters that captures module and performs the following steps when called:
        // 5. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 0, "", « »).
        let on_fulfilled = NativeFunction::create_anonymous(
            vm,
            self.as_cyclic_module_gc(),
            |vm, &module: &Gc<CyclicModule>| {
                // a. Perform AsyncModuleExecutionFulfilled(module).
                module.async_module_execution_fulfilled(vm);

                // b. Return undefined.
                Ok(Value::UNDEFINED)
            },
            0,
        );

        // 6. Let rejectedClosure be a new Abstract Closure with parameters (error) that captures module and performs the following steps when called:
        // 7. Let onRejected be CreateBuiltinFunction(rejectedClosure, 0, "", « »).
        let on_rejected = NativeFunction::create_anonymous(
            vm,
            self.as_cyclic_module_gc(),
            |vm, &module: &Gc<CyclicModule>| {
                let error = vm.argument(0);

                // a. Perform AsyncModuleExecutionRejected(module, error).
                module.async_module_execution_rejected(vm, error);

                // b. Return undefined.
                Ok(Value::UNDEFINED)
            },
            0,
        );

        // 8. Perform PerformPromiseThen(capability.[[Promise]], onFulfilled, onRejected).
        promise_of(capability).perform_then(
            vm,
            Value::from_object(on_fulfilled),
            Value::from_object(on_rejected),
            None,
        );

        // 9. Perform ! module.ExecuteModule(capability).
        self.execute_module(vm, Some(capability)).must();

        // 10. Return unused.
    }

    // 16.2.1.5.2.3 GatherAvailableAncestors ( module, execList ), https://tc39.es/ecma262/#sec-gather-available-ancestors
    fn gather_available_ancestors(&self, vm: &Vm, exec_list: &MarkedVec<'_, Gc<CyclicModule>>) {
        // 1. For each Cyclic Module Record m of module.[[AsyncParentModules]], do
        let async_parent_modules = self.async_parent_modules(vm);
        for index in 0..async_parent_modules.len() {
            let module = async_parent_modules.get(index).expect("the index is in bounds");
            let cycle_root = module.cycle_root.get().expect("an evaluating module has a cycle root");

            // a. If execList does not contain m and m.[[CycleRoot]].[[EvaluationError]] is empty, then
            if !(0..exec_list.len()).any(|index| exec_list.get(index) == Some(module))
                && cycle_root.evaluation_error.get().is_none()
            {
                // i. Assert: m.[[Status]] is evaluating-async.
                assert_eq!(module.status(), ModuleStatus::EvaluatingAsync);

                // ii. Assert: m.[[EvaluationError]] is empty.
                assert!(module.evaluation_error.get().is_none());

                // iii. Assert: _m_.[[AsyncEvaluationOrder]] is an integer.
                assert!(module.async_evaluation_order.get().is_some());

                // iv. Assert: m.[[PendingAsyncDependencies]] > 0.
                assert!(module.pending_async_dependencies() > 0);

                // v. Set m.[[PendingAsyncDependencies]] to m.[[PendingAsyncDependencies]] - 1.
                module
                    .pending_async_dependencies
                    .set(Some(module.pending_async_dependencies() - 1));

                // vi. If m.[[PendingAsyncDependencies]] = 0, then
                if module.pending_async_dependencies() == 0 {
                    // 1. Append m to execList.
                    exec_list.push(module);

                    // 2. If m.[[HasTLA]] is false, perform GatherAvailableAncestors(m, execList).
                    if !module.has_top_level_await {
                        module.gather_available_ancestors(vm, exec_list);
                    }
                }
            }
        }

        // 2. Return unused.
    }

    // 16.2.1.5.2.4 AsyncModuleExecutionFulfilled ( module ), https://tc39.es/ecma262/#sec-async-module-execution-fulfilled
    fn async_module_execution_fulfilled(&self, vm: &Vm) {
        // 1. If module.[[Status]] is evaluated, then
        if self.status() == ModuleStatus::Evaluated {
            // a. Assert: module.[[EvaluationError]] is not empty.
            assert!(self.evaluation_error.get().is_some());

            // b. Return unused.
            return;
        }

        // 2. Assert: module.[[Status]] is evaluating-async.
        assert_eq!(self.status(), ModuleStatus::EvaluatingAsync);

        // 3. Assert: _module_.[[AsyncEvaluationOrder]] is an integer.
        assert!(self.async_evaluation_order.get().is_some());

        // 4. Assert: module.[[EvaluationError]] is empty.
        assert!(self.evaluation_error.get().is_none());

        // 5. Set _module_.[[AsyncEvaluationOrder]] to ~done~.
        self.async_evaluation_order.set(None);

        // 6. Set module.[[Status]] to evaluated.
        self.set_status(ModuleStatus::Evaluated);

        // 7. If module.[[TopLevelCapability]] is not empty, then
        if let Some(top_level_capability) = self.top_level_capability.get() {
            // a. Assert: module.[[CycleRoot]] is module.
            assert!(self.cycle_root.get() == Some(self.as_cyclic_module_gc()));

            // b. Perform ! Call(module.[[TopLevelCapability]].[[Resolve]], undefined, « undefined »).
            call_function_object(
                vm,
                top_level_capability.resolve(),
                Value::UNDEFINED,
                &[Value::UNDEFINED],
            )
            .must();
        }

        // 8. Let execList be a new empty List.
        let exec_list = MarkedVec::new(vm);

        // 9. Perform GatherAvailableAncestors(module, execList).
        self.gather_available_ancestors(vm, &exec_list);

        // 10. Assert: All elements of _execList_ have their [[AsyncEvaluationOrder]] field set to an integer,
        //     [[PendingAsyncDependencies]] field set to 0, and [[EvaluationError]] field set to ~empty~.
        assert!(exec_list.to_vec().iter().all(|module| {
            module.async_evaluation_order.get().is_some()
                && module.pending_async_dependencies() == 0
                && module.evaluation_error.get().is_none()
        }));

        // 11. Let _sortedExecList_ be a List whose elements are the elements of _execList_, sorted by their
        //     [[AsyncEvaluationOrder]] field in ascending order.
        // NB: The sorted copy holds the same modules as the list, which keeps them alive, and nothing collects while
        //     it is sorted.
        let mut sorted_exec_list = exec_list.to_vec();
        sorted_exec_list.sort_by_key(|module| {
            module
                .async_evaluation_order
                .get()
                .expect("the module has an async evaluation order")
        });
        for (index, module) in sorted_exec_list.into_iter().enumerate() {
            exec_list.set(index, module);
        }

        // 12. For each Cyclic Module Record m of sortedExecList, do
        for index in 0..exec_list.len() {
            let module = exec_list.get(index).expect("the index is in bounds");

            // a. If m.[[Status]] is evaluated, then
            if module.status() == ModuleStatus::Evaluated {
                // i. Assert: m.[[EvaluationError]] is not empty.
                assert!(module.evaluation_error.get().is_some());
            }
            // b. Else if m.[[HasTLA]] is true, then
            else if module.has_top_level_await {
                // i. Perform ExecuteAsyncModule(m).
                module.execute_async_module(vm);
            }
            // c. Else,
            else {
                // i. Let result be m.ExecuteModule().
                let result = module.execute_module(vm, None);

                // ii. If result is an abrupt completion, then
                if let Err(error) = result {
                    // 1. Perform AsyncModuleExecutionRejected(m, result.[[Value]]).
                    module.async_module_execution_rejected(vm, error.value());
                }
                // iii. Else,
                else {
                    // 1. Set _m_.[[AsyncEvaluationOrder]] to ~done~.
                    module.async_evaluation_order.set(None);

                    // 2. Set _m_.[[Status]] to ~evaluated~.
                    module.set_status(ModuleStatus::Evaluated);

                    // 3. If _m_.[[TopLevelCapability]] is not ~empty~, then
                    if let Some(top_level_capability) = module.top_level_capability.get() {
                        // a. Assert: _m_.[[CycleRoot]] and _m_ are the same Module Record.
                        assert!(module.cycle_root.get() == Some(module));

                        // b. Perform ! Call(m.[[TopLevelCapability]].[[Resolve]], undefined, « undefined »).
                        call_function_object(
                            vm,
                            top_level_capability.resolve(),
                            Value::UNDEFINED,
                            &[Value::UNDEFINED],
                        )
                        .must();
                    }
                }
            }
        }

        // 13. Return unused.
    }

    // 16.2.1.5.2.5 AsyncModuleExecutionRejected ( module, error ), https://tc39.es/ecma262/#sec-async-module-execution-rejected
    fn async_module_execution_rejected(&self, vm: &Vm, error: Value) {
        // 1. If module.[[Status]] is evaluated, then
        if self.status() == ModuleStatus::Evaluated {
            // a. Assert: module.[[EvaluationError]] is not empty.
            assert!(self.evaluation_error.get().is_some());

            // b. Return unused.
            return;
        }

        // 2. Assert: module.[[Status]] is evaluating-async.
        assert_eq!(self.status(), ModuleStatus::EvaluatingAsync);

        // 3. Assert: _module_.[[AsyncEvaluationOrder]] is an integer.
        assert!(self.async_evaluation_order.get().is_some());

        // 4. Assert: module.[[EvaluationError]] is empty.
        assert!(self.evaluation_error.get().is_none());

        // 5. Set module.[[EvaluationError]] to ThrowCompletion(error).
        self.evaluation_error.set(Some(error));

        // 6. Set module.[[Status]] to evaluated.
        self.set_status(ModuleStatus::Evaluated);

        // 7. Set _module_.[[AsyncEvaluationOrder]] to ~done~.
        self.async_evaluation_order.set(None);

        // 8. NOTE: _module_.[[AsyncEvaluationOrder]] is set to ~done~ for symmetry with AsyncModuleExecutionFulfilled. In
        //    InnerModuleEvaluation, the value of a module's [[AsyncEvaluationOrder]] internal slot is unused when its
        //    [[EvaluationError]] internal slot is not ~empty~.

        // 9. If module.[[TopLevelCapability]] is not empty, then
        if let Some(top_level_capability) = self.top_level_capability.get() {
            // a. Assert: module.[[CycleRoot]] and module are the same Module Record.
            assert!(self.cycle_root.get() == Some(self.as_cyclic_module_gc()));

            // b. Perform ! Call(module.[[TopLevelCapability]].[[Reject]], undefined, « error »).
            call_function_object(vm, top_level_capability.reject(), Value::UNDEFINED, &[error]).must();
        }

        // 10. For each Cyclic Module Record m of module.[[AsyncParentModules]], do
        let async_parent_modules = self.async_parent_modules(vm);
        for index in 0..async_parent_modules.len() {
            let module = async_parent_modules.get(index).expect("the index is in bounds");

            // a. Perform AsyncModuleExecutionRejected(m, error).
            module.async_module_execution_rejected(vm, error);
        }

        // 11. Return unused.
    }

    // 16.2.1.9 GetImportedModule ( referrer, request ), https://tc39.es/ecma262/#sec-GetImportedModule
    pub fn get_imported_module(&self, request: &ModuleRequest) -> Gc<Module> {
        // 1. Let records be a List consisting of each LoadedModuleRequest Record r of referrer.[[LoadedModules]]
        //    such that ModuleRequestsEqual(r, request) is true.
        let loaded_modules = self.loaded_modules.borrow();
        let mut records = loaded_modules
            .iter()
            .filter(|record| module_requests_equal(*record, request));
        let record = records.next();

        // 2. Assert: records has exactly one element, since LoadRequestedModules has completed successfully
        //    on referrer prior to invoking this abstract operation.
        assert!(record.is_some() && records.next().is_none());

        // 3. Let record be the sole element of records.
        // 4. Return record.[[Module]].
        record.expect("the module was loaded").module
    }
}

// 16.2.1.5.1.1 InnerModuleLoading ( state, module ), https://tc39.es/ecma262/#sec-InnerModuleLoading
pub fn inner_module_loading(vm: &Vm, state: Gc<GraphLoadingState>, module: Gc<Module>) {
    // 1. Assert: state.[[IsLoading]] is true.
    assert!(state.is_loading());

    // 2. If module is a Cyclic Module Record, module.[[Status]] is NEW, and state.[[Visited]] does not contain module, then
    if let Some(cyclic_module) = module.as_cyclic_module()
        && cyclic_module.status() == ModuleStatus::New
        && !state.visited_contains(cyclic_module.as_cyclic_module_gc())
    {
        let cyclic_module = cyclic_module.as_cyclic_module_gc();

        // a. Append module to state.[[Visited]].
        state.append_to_visited(cyclic_module);

        // b. Let requestedModulesCount be the number of elements in module.[[RequestedModules]].
        let requested_modules_count = cyclic_module.requested_modules().len();

        // c. Set state.[[PendingModulesCount]] to state.[[PendingModulesCount]] + requestedModulesCount.
        state.set_pending_module_count(state.pending_module_count() + requested_modules_count);

        // d. For each ModuleRequest Record request of module.[[RequestedModules]], do
        for request_index in 0..requested_modules_count {
            let request = cyclic_module.requested_modules()[request_index].clone();

            // i. If AllImportAttributesSupported(request.[[Attributes]]) is false, then
            if !all_import_attributes_supported(vm, &request.attributes) {
                // 1. Let error be ThrowCompletion(a newly created SyntaxError object).
                let error = vm.throw_completion(ErrorKind::SyntaxError, ErrorType::ImportAttributeUnsupported, &[]);

                // 2. Perform ContinueModuleLoading(state, error).
                continue_module_loading(vm, state, error);
            }
            // ii. Else if module.[[LoadedModules]] contains a LoadedModuleRequest Record record
            //     such that ModuleRequestsEqual(record, request) is true, then
            else if let Some(record_module) = cyclic_module.find_record_in_loaded_modules(&request) {
                // 1. Perform InnerModuleLoading(state, record.[[Module]]).
                inner_module_loading(vm, state, record_module);
            }
            // iii. Else,
            else {
                // 1. Perform HostLoadImportedModule(module, request, state.[[HostDefined]], state).
                vm.host_load_imported_module()(
                    vm,
                    ImportedModuleReferrer::CyclicModule(cyclic_module),
                    &request,
                    state.host_defined(),
                    ImportedModulePayload::GraphLoadingState(state),
                );

                // 2. NOTE: HostLoadImportedModule will call FinishLoadingImportedModule, which re-enters the graph loading process through ContinueModuleLoading.
            }

            // iv. If state.[[IsLoading]] is false, return UNUSED.
            if !state.is_loading() {
                return;
            }
        }
    }

    // 3. Assert: state.[[PendingModulesCount]] ≥ 1.
    assert!(state.pending_module_count() >= 1);

    // 4. Set state.[[PendingModulesCount]] to state.[[PendingModulesCount]] - 1.
    state.set_pending_module_count(state.pending_module_count() - 1);

    // 5. If state.[[PendingModulesCount]] = 0, then
    if state.pending_module_count() == 0 {
        // a. Set state.[[IsLoading]] to false.
        state.set_is_loading(false);

        // b. For each Cyclic Module Record loaded of state.[[Visited]], do
        let visited = state.visited(vm);
        for index in 0..visited.len() {
            let loaded = visited.get(index).expect("the index is in bounds");
            // i. If loaded.[[Status]] is NEW, set loaded.[[Status]] to UNLINKED.
            if loaded.status() == ModuleStatus::New {
                loaded.set_status(ModuleStatus::Unlinked);
            }
        }

        // c. Perform ! Call(state.[[PromiseCapability]].[[Resolve]], undefined, « undefined »).
        call_function_object(
            vm,
            state.promise_capability().resolve(),
            Value::UNDEFINED,
            &[Value::UNDEFINED],
        )
        .must();
    }

    // 6. Return unused.
}

// 16.2.1.5.1.2 ContinueModuleLoading ( state, moduleCompletion ), https://tc39.es/ecma262/#sec-ContinueModuleLoading
pub fn continue_module_loading(
    vm: &Vm,
    state: Gc<GraphLoadingState>,
    module_completion: ThrowCompletionOr<Gc<Module>>,
) {
    // 1. If state.[[IsLoading]] is false, return UNUSED.
    if !state.is_loading() {
        return;
    }

    match module_completion {
        // 2. If moduleCompletion is a normal completion, then
        Ok(module) => {
            // a. Perform InnerModuleLoading(state, moduleCompletion.[[Value]]).
            inner_module_loading(vm, state, module);
        }
        // 3. Else,
        Err(error) => {
            // a. Set state.[[IsLoading]] to false.
            state.set_is_loading(false);

            // b. Perform ! Call(state.[[PromiseCapability]].[[Reject]], undefined, « moduleCompletion.[[Value]] »).
            call_function_object(
                vm,
                state.promise_capability().reject(),
                Value::UNDEFINED,
                &[error.value()],
            )
            .must();
        }
    }

    // 4. Return UNUSED.
}

// 13.3.10.1.1 ContinueDynamicImport ( promiseCapability, moduleCompletion ), https://tc39.es/ecma262/#sec-ContinueDynamicImport
pub fn continue_dynamic_import(
    vm: &Vm,
    promise_capability: Gc<PromiseCapability>,
    module_completion: ThrowCompletionOr<Gc<Module>>,
) {
    // 1. If moduleCompletion is an abrupt completion, then
    let module = match module_completion {
        Err(error) => {
            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « moduleCompletion.[[Value]] »).
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error.value()]).must();

            // b. Return unused.
            return;
        }
        // 2. Let module be moduleCompletion.[[Value]].
        Ok(module) => module,
    };

    // 3. Let loadPromise be module.LoadRequestedModules().
    let load_promise = module.load_requested_modules(vm, ForeignCellSlot::empty());

    // 4. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures promiseCapability and performs the
    //    following steps when called:
    // 5. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    let on_rejected = NativeFunction::create_anonymous(
        vm,
        promise_capability,
        |vm, &promise_capability: &Gc<PromiseCapability>| {
            let reason = vm.argument(0);

            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « reason »).
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[reason]).must();

            // b. Return unused.
            Ok(Value::UNDEFINED)
        },
        1,
    );

    // 6. Let linkAndEvaluateClosure be a new Abstract Closure with no parameters that captures module, promiseCapability,
    //    and onRejected and performs the following steps when called:
    // 7. Let linkAndEvaluate be CreateBuiltinFunction(linkAndEvaluateClosure, 0, "", « »).
    let link_and_evaluate = NativeFunction::create_anonymous(
        vm,
        (module, promise_capability, on_rejected),
        |vm, &(module, promise_capability, on_rejected)| {
            // a. Let link be Completion(module.Link()).
            let link = module.link(vm);

            // b. If link is an abrupt completion, then
            if let Err(error) = link {
                // i. Perform ! Call(promiseCapability.[[Reject]], undefined, « link.[[Value]] »).
                call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error.value()]).must();

                // ii. Return unused.
                return Ok(Value::UNDEFINED);
            }

            // c. Let evaluatePromise be module.Evaluate().
            let evaluate_promise = module.evaluate(vm);

            // d. Let fulfilledClosure be a new Abstract Closure with no parameters that captures module and
            //    promiseCapability and performs the following steps when called:
            // e. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 0, "", « »).
            let on_fulfilled = NativeFunction::create_anonymous(
                vm,
                (module, promise_capability),
                |vm, &(module, promise_capability)| {
                    // i. Let namespace be GetModuleNamespace(module).
                    let namespace = module.get_module_namespace(vm);

                    // ii. Perform ! Call(promiseCapability.[[Resolve]], undefined, « namespace »).
                    call_function_object(
                        vm,
                        promise_capability.resolve(),
                        Value::UNDEFINED,
                        &[Value::from_object(namespace)],
                    )
                    .must();

                    // iii. Return unused.
                    Ok(Value::UNDEFINED)
                },
                0,
            );

            // f. Perform PerformPromiseThen(evaluatePromise, onFulfilled, onRejected).
            let evaluate_promise = evaluate_promise.expect("the C++ runtime never expects Evaluate() to throw here");
            promise_of(evaluate_promise).perform_then(
                vm,
                Value::from_object(on_fulfilled),
                Value::from_object(on_rejected),
                None,
            );

            // g. Return unused.
            Ok(Value::UNDEFINED)
        },
        0,
    );

    // 8. Perform PerformPromiseThen(loadPromise, linkAndEvaluate, onRejected).
    // FIXME: This is likely a spec bug, see load_requested_modules.
    promise_of(load_promise).perform_then(
        vm,
        Value::from_object(link_and_evaluate),
        Value::from_object(on_rejected),
        None,
    );

    // 9. Return unused.
}
