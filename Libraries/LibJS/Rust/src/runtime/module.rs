/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! JS::Module, the base of every Module Record.
//!
//! The abstract methods of Module are a table of function pointers per class of module, ModuleMethods, which
//! Module::methods() finds by matching on the module's class. A class's table starts from the table of the class it
//! extends, as in `ModuleMethods { resolve_export: ..., ..CYCLIC_MODULE_METHODS }`, so that it only names the methods
//! the class overrides.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::embedding::host::host_module::HOST_MODULE_METHODS;
use crate::gc::class::{Class, GcCell, define_cell};
use crate::gc::class_id::ClassId;
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::cyclic_module::{CyclicModule, continue_dynamic_import, continue_module_loading};
use crate::runtime::module_environment::ModuleEnvironment;
use crate::runtime::module_loading::{ImportedModulePayload, ImportedModuleReferrer};
use crate::runtime::module_namespace_object::ModuleNamespaceObject;
use crate::runtime::module_request::{LoadedModuleRequest, ModuleRequest, module_requests_equal};
use crate::runtime::promise::{Promise, PromiseState, RejectionOperation};
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::source_text_module::SOURCE_TEXT_MODULE_METHODS;
use crate::runtime::synthetic_module::SYNTHETIC_MODULE_METHODS;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResolvedBindingType {
    BindingName,
    Namespace,
    Ambiguous,
    Null,
}

/// A ResolvedBinding Record, or one of the null and AMBIGUOUS results of ResolveExport.
#[derive(Clone, Trace)]
pub struct ResolvedBinding {
    #[gc(untraced)]
    pub binding_type: ResolvedBindingType,
    pub module: Option<Gc<Module>>,
    pub export_name: Utf16FlyString,
}

impl ResolvedBinding {
    pub fn null() -> Self {
        Self {
            binding_type: ResolvedBindingType::Null,
            module: None,
            export_name: Utf16FlyString::default(),
        }
    }

    pub fn ambiguous() -> Self {
        Self {
            binding_type: ResolvedBindingType::Ambiguous,
            ..Self::null()
        }
    }

    pub fn is_valid(&self) -> bool {
        matches!(
            self.binding_type,
            ResolvedBindingType::BindingName | ResolvedBindingType::Namespace
        )
    }

    pub fn is_namespace(&self) -> bool {
        self.binding_type == ResolvedBindingType::Namespace
    }

    pub fn is_ambiguous(&self) -> bool {
        self.binding_type == ResolvedBindingType::Ambiguous
    }
}

/// The resolveSet of ResolveExport. It is passed by value, so the records a call appends are only seen by the calls it
/// makes, not by its caller.
pub type ResolveSet<'vm> = MarkedVec<'vm, ResolvedBinding>;

/// A copy of `resolve_set`, for passing it on by value.
pub fn copy_of_resolve_set<'vm>(vm: &'vm Vm, resolve_set: &ResolveSet<'_>) -> ResolveSet<'vm> {
    let copy = MarkedVec::with_capacity(vm, resolve_set.len());
    for index in 0..resolve_set.len() {
        copy.push(resolve_set.get(index).expect("the index is in bounds"));
    }
    copy
}

/// The exportStarSet of GetExportedNames, which every call shares.
pub type ExportStarSet<'vm> = MarkedVec<'vm, Gc<Module>>;

/// The stack of InnerModuleLinking and InnerModuleEvaluation.
pub type ModuleStack<'vm> = MarkedVec<'vm, Gc<Module>>;

pub fn module_stack_contains(stack: &ModuleStack<'_>, module: Gc<Module>) -> bool {
    (0..stack.len()).any(|index| stack.get(index) == Some(module))
}

// https://tc39.es/ecma262/#graphloadingstate-record
#[repr(C)]
#[derive(Trace)]
pub struct GraphLoadingState {
    header: CellHeader,
    promise_capability: Gc<PromiseCapability>, // [[PromiseCapability]]
    is_loading: Cell<bool>,                    // [[IsLoading]]
    pending_module_count: Cell<usize>,         // [[PendingModulesCount]]
    visited: GcRefCell<Vec<Gc<CyclicModule>>>, // [[Visited]]
    host_defined: ForeignCellSlot,             // [[HostDefined]]
}

define_cell!(GraphLoadingState, Other);

impl GraphLoadingState {
    pub fn create(
        vm: &Vm,
        promise_capability: Gc<PromiseCapability>,
        is_loading: bool,
        pending_module_count: usize,
        host_defined: ForeignCellSlot,
    ) -> Gc<GraphLoadingState> {
        vm.heap().allocate(GraphLoadingState {
            header: CellHeader::for_class(Self::CLASS),
            promise_capability,
            is_loading: Cell::new(is_loading),
            pending_module_count: Cell::new(pending_module_count),
            visited: GcRefCell::new(Vec::new()),
            host_defined,
        })
    }

    pub fn promise_capability(&self) -> Gc<PromiseCapability> {
        self.promise_capability
    }

    pub fn host_defined(&self) -> Option<NonNull<c_void>> {
        self.host_defined.get()
    }

    pub fn is_loading(&self) -> bool {
        self.is_loading.get()
    }

    pub fn set_is_loading(&self, is_loading: bool) {
        self.is_loading.set(is_loading);
    }

    pub fn pending_module_count(&self) -> usize {
        self.pending_module_count.get()
    }

    pub fn set_pending_module_count(&self, pending_module_count: usize) {
        self.pending_module_count.set(pending_module_count);
    }

    pub fn visited_contains(&self, module: Gc<CyclicModule>) -> bool {
        self.visited.borrow().contains(&module)
    }

    pub fn append_to_visited(&self, module: Gc<CyclicModule>) {
        self.visited.borrow_mut().push(module);
    }

    /// The modules of [[Visited]], kept alive by the returned list.
    pub fn visited<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Gc<CyclicModule>> {
        let visited = MarkedVec::new(vm);
        for module in self.visited.borrow().iter() {
            visited.push(*module);
        }
        visited
    }
}

/// The virtual methods of Module, and those of CyclicModule, which only the classes that extend it implement.
pub struct ModuleMethods {
    pub link: fn(&Module, &Vm) -> ThrowCompletionOr<()>,
    pub evaluate: fn(&Module, &Vm) -> ThrowCompletionOr<Gc<PromiseCapability>>,
    pub get_exported_names: fn(&Module, &Vm, &ExportStarSet<'_>) -> Vec<Utf16FlyString>,
    pub resolve_export: fn(&Module, &Vm, &Utf16FlyString, ResolveSet<'_>) -> ResolvedBinding,
    pub inner_module_linking: fn(&Module, &Vm, &ModuleStack<'_>, u32) -> ThrowCompletionOr<u32>,
    pub inner_module_evaluation: fn(&Module, &Vm, &ModuleStack<'_>, u32) -> ThrowCompletionOr<u32>,
    pub load_requested_modules: fn(&Module, &Vm, ForeignCellSlot) -> Gc<PromiseCapability>,
    pub initialize_environment: fn(&CyclicModule, &Vm) -> ThrowCompletionOr<()>,
    pub execute_module: fn(&CyclicModule, &Vm, Option<Gc<PromiseCapability>>) -> ThrowCompletionOr<()>,
}

pub const MODULE_METHODS: ModuleMethods = ModuleMethods {
    link: |_, _| unreachable!("Module::link is pure virtual"),
    evaluate: |_, _| unreachable!("Module::evaluate is pure virtual"),
    get_exported_names: |_, _, _| unreachable!("Module::get_exported_names is pure virtual"),
    resolve_export: |_, _, _, _| unreachable!("Module::resolve_export is pure virtual"),
    inner_module_linking: Module::inner_module_linking_of_module,
    inner_module_evaluation: Module::inner_module_evaluation_of_module,
    load_requested_modules: |_, _, _| unreachable!("Module::load_requested_modules is pure virtual"),
    initialize_environment: |_, _| unreachable!("only Cyclic Module Records have InitializeEnvironment"),
    execute_module: |_, _, _| unreachable!("only Cyclic Module Records have ExecuteModule"),
};

// 16.2.1.4 Abstract Module Records, https://tc39.es/ecma262/#sec-abstract-module-records
#[repr(C)]
#[derive(Trace)]
pub struct Module {
    header: CellHeader,
    realm: Gc<Realm>,                                 // [[Realm]]
    environment: Cell<Option<Gc<ModuleEnvironment>>>, // [[Environment]]
    namespace: Cell<Option<Gc<Object>>>,              // [[Namespace]]
    host_defined: ForeignCellSlot,                    // [[HostDefined]]

    // Needed for potential lookups of modules.
    filename: String,
}

define_cell!(Module, Other);

impl Module {
    /// A module of `realm`, for `class`, which extends Module.
    pub fn new(class: &'static Class, realm: Gc<Realm>, filename: String, host_defined: ForeignCellSlot) -> Module {
        Module {
            header: CellHeader::for_class(class),
            realm,
            environment: Cell::new(None),
            namespace: Cell::new(None),
            host_defined,
            filename,
        }
    }

    pub fn class(&self) -> &'static Class {
        self.header.class
    }

    pub(crate) fn methods(&self) -> &'static ModuleMethods {
        match self.class().id {
            ClassId::SourceTextModule => &SOURCE_TEXT_MODULE_METHODS,
            ClassId::SyntheticModule => &SYNTHETIC_MODULE_METHODS,
            ClassId::HostModule => &HOST_MODULE_METHODS,
            class_id => unreachable!("{class_id:?} is not a class of module"),
        }
    }

    pub fn as_gc(&self) -> Gc<Module> {
        // SAFETY: Modules only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    /// The module as a `T`, if it was allocated as one.
    pub fn downcast_ref<T: GcCell + crate::gc::class::Extends<Module>>(&self) -> Option<&T> {
        self.class()
            .is_subclass_of(T::CLASS)
            // SAFETY: The module was allocated as a T or a subclass of it, which starts with a Module.
            .then(|| unsafe { &*core::ptr::from_ref(self).cast::<T>() })
    }

    pub fn as_cyclic_module(&self) -> Option<&CyclicModule> {
        self.downcast_ref::<CyclicModule>()
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub fn filename(&self) -> &str {
        &self.filename
    }

    pub fn host_defined(&self) -> Option<NonNull<c_void>> {
        self.host_defined.get()
    }

    pub fn environment(&self) -> Option<Gc<ModuleEnvironment>> {
        self.environment.get()
    }

    pub fn set_environment(&self, environment: Gc<ModuleEnvironment>) {
        self.environment.set(Some(environment));
    }

    pub fn link(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        (self.methods().link)(self, vm)
    }

    pub fn evaluate(&self, vm: &Vm) -> ThrowCompletionOr<Gc<PromiseCapability>> {
        (self.methods().evaluate)(self, vm)
    }

    pub fn get_exported_names(&self, vm: &Vm) -> Vec<Utf16FlyString> {
        let export_star_set = ExportStarSet::new(vm);
        self.get_exported_names_with_export_star_set(vm, &export_star_set)
    }

    pub fn get_exported_names_with_export_star_set(
        &self,
        vm: &Vm,
        export_star_set: &ExportStarSet<'_>,
    ) -> Vec<Utf16FlyString> {
        (self.methods().get_exported_names)(self, vm, export_star_set)
    }

    pub fn resolve_export(&self, vm: &Vm, export_name: &Utf16FlyString) -> ResolvedBinding {
        self.resolve_export_with_resolve_set(vm, export_name, ResolveSet::new(vm))
    }

    pub fn resolve_export_with_resolve_set(
        &self,
        vm: &Vm,
        export_name: &Utf16FlyString,
        resolve_set: ResolveSet<'_>,
    ) -> ResolvedBinding {
        (self.methods().resolve_export)(self, vm, export_name, resolve_set)
    }

    pub fn inner_module_linking(&self, vm: &Vm, stack: &ModuleStack<'_>, index: u32) -> ThrowCompletionOr<u32> {
        (self.methods().inner_module_linking)(self, vm, stack, index)
    }

    pub fn inner_module_evaluation(&self, vm: &Vm, stack: &ModuleStack<'_>, index: u32) -> ThrowCompletionOr<u32> {
        (self.methods().inner_module_evaluation)(self, vm, stack, index)
    }

    /// LoadRequestedModules ( [ hostDefined ] ), where an empty slot stands for a hostDefined that is not present.
    pub fn load_requested_modules(&self, vm: &Vm, host_defined: ForeignCellSlot) -> Gc<PromiseCapability> {
        (self.methods().load_requested_modules)(self, vm, host_defined)
    }

    // 16.2.1.5.1 EvaluateModuleSync ( module ), https://tc39.es/ecma262/#sec-EvaluateModuleSync
    fn evaluate_module_sync(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        // 1. Assert: module is not a Cyclic Module Record.
        // 2. Let promise be module.Evaluate().
        let promise = self
            .evaluate(vm)?
            .promise()
            .downcast::<Promise>()
            .expect("the promise of a promise capability for %Promise% is a Promise");

        // 3. Assert: promise.[[PromiseState]] is either FULFILLED or REJECTED.
        assert!(matches!(
            promise.state(),
            PromiseState::Fulfilled | PromiseState::Rejected
        ));

        // 4. If promise.[[PromiseState]] is REJECTED, then
        if promise.state() == PromiseState::Rejected {
            // a. If promise.[[PromiseIsHandled]] is false, perform HostPromiseRejectionTracker(promise, "handle").
            if !promise.is_handled() {
                vm.host_promise_rejection_tracker()(vm, promise, RejectionOperation::Handle);
            }

            // b. Set promise.[[PromiseIsHandled]] to true.
            promise.set_is_handled();

            // c. Return ThrowCompletion(promise.[[PromiseResult]]).
            crate::embedding::completion::log_exception_if_enabled(vm, promise.result());
            return Err(Throw::new(promise.result()));
        }

        // 5. Return UNUSED.
        Ok(())
    }

    // 16.2.1.5.1.1 InnerModuleLinking ( module, stack, index ), https://tc39.es/ecma262/#sec-InnerModuleLinking
    fn inner_module_linking_of_module(
        module: &Module,
        vm: &Vm,
        _stack: &ModuleStack<'_>,
        index: u32,
    ) -> ThrowCompletionOr<u32> {
        // 1. If module is not a Cyclic Module Record, then
        // a. Perform ? module.Link().
        module.link(vm)?;
        // b. Return index.
        Ok(index)
    }

    // 16.2.1.5.2.1 InnerModuleEvaluation ( module, stack, index ), https://tc39.es/ecma262/#sec-innermoduleevaluation
    fn inner_module_evaluation_of_module(
        module: &Module,
        vm: &Vm,
        _stack: &ModuleStack<'_>,
        index: u32,
    ) -> ThrowCompletionOr<u32> {
        // 1. If module is not a Cyclic Module Record, then
        // a. Perform ? EvaluateModuleSync(module).
        module.evaluate_module_sync(vm)?;

        // b. Return index.
        Ok(index)
    }

    // 16.2.1.10 GetModuleNamespace ( module ), https://tc39.es/ecma262/#sec-getmodulenamespace
    pub fn get_module_namespace(&self, vm: &Vm) -> Gc<Object> {
        // 1. Assert: If module is a Cyclic Module Record, then module.[[Status]] is not NEW or UNLINKED.
        // FIXME: Spec bug: https://github.com/tc39/ecma262/issues/3114

        // 2. Let namespace be module.[[Namespace]].
        // 3. If namespace is EMPTY, then
        if let Some(namespace) = self.namespace.get() {
            // 4. Return namespace.
            return namespace;
        }

        // a. Let exportedNames be module.GetExportedNames().
        let exported_names = self.get_exported_names(vm);

        // b. Let unambiguousNames be a new empty List.
        let mut unambiguous_names = Vec::new();

        // c. For each element name of exportedNames, do
        for name in exported_names {
            // i. Let resolution be module.ResolveExport(name).
            let resolution = self.resolve_export(vm, &name);

            // ii. If resolution is a ResolvedBinding Record, append name to unambiguousNames.
            if resolution.is_valid() {
                unambiguous_names.push(name);
            }
        }

        // d. Set namespace to ModuleNamespaceCreate(module, unambiguousNames).
        // 4. Return namespace.
        self.module_namespace_create(vm, unambiguous_names)
    }

    // 10.4.6.12 ModuleNamespaceCreate ( module, exports ), https://tc39.es/ecma262/#sec-modulenamespacecreate
    fn module_namespace_create(&self, vm: &Vm, unambiguous_names: Vec<Utf16FlyString>) -> Gc<Object> {
        let realm = self.realm();

        // 1. Assert: module.[[Namespace]] is empty.
        assert!(self.namespace.get().is_none());

        // 2. Let internalSlotsList be the internal slots listed in Table 34.
        // 3. Let M be MakeBasicObject(internalSlotsList).
        // 4. Set M's essential internal methods to the definitions specified in 10.4.6.
        // 5. Set M.[[Module]] to module.
        // 6. Let sortedExports be a List whose elements are the elements of exports ordered as if an Array of the same values had been sorted using %Array.prototype.sort% using undefined as comparefn.
        // 7. Set M.[[Exports]] to sortedExports.
        // 8. Create own properties of M corresponding to the definitions in 28.3.
        let module_namespace = ModuleNamespaceObject::create(vm, realm, self.as_gc(), unambiguous_names).upcast();

        // 9. Set module.[[Namespace]] to M.
        self.namespace.set(Some(module_namespace));

        // 10. Return M.
        module_namespace
    }
}

/// The [[LoadedModules]] of a referrer that has them: only Script and CyclicModule referrers do.
fn loaded_modules_of(referrer: &ImportedModuleReferrer) -> Option<&GcRefCell<Vec<LoadedModuleRequest>>> {
    match referrer {
        ImportedModuleReferrer::Script(script) => Some(script.loaded_modules()),
        ImportedModuleReferrer::CyclicModule(module) => Some(module.loaded_modules()),
        ImportedModuleReferrer::Realm(_) => None,
    }
}

// 16.2.1.9 FinishLoadingImportedModule ( referrer, specifier, payload, result ), https://tc39.es/ecma262/#sec-FinishLoadingImportedModule
pub fn finish_loading_imported_module(
    vm: &Vm,
    referrer: ImportedModuleReferrer,
    module_request: &ModuleRequest,
    payload: ImportedModulePayload,
    result: ThrowCompletionOr<Gc<Module>>,
) {
    // 1. If result is a normal completion, then
    if let Ok(module) = result
        // NOTE: Only Script and CyclicModule referrers have the [[LoadedModules]] internal slot.
        && let Some(loaded_modules) = loaded_modules_of(&referrer)
    {
        let mut found_record = false;

        // a. If referrer.[[LoadedModules]] contains a LoadedModuleRequest Record record such that ModuleRequestsEqual(record, moduleRequest) is true, then
        for record in loaded_modules.borrow().iter() {
            if module_requests_equal(record, module_request) {
                // i. Assert: record.[[Module]] and result.[[Value]] are the same Module Record.
                assert!(record.module == module);
                found_record = true;
            }
        }

        // b. Else,
        if !found_record {
            // i. Append the LoadedModuleRequest Record { [[Specifier]]: moduleRequest.[[Specifier]], [[Attributes]]: moduleRequest.[[Attributes]], [[Module]]: result.[[Value]] } to referrer.[[LoadedModules]].
            loaded_modules.borrow_mut().push(LoadedModuleRequest {
                specifier: Utf16String::from(&module_request.module_specifier),
                attributes: module_request.attributes.clone(),
                module,
            });
        }
    }

    match payload {
        // 2. If payload is a GraphLoadingState Record, then
        ImportedModulePayload::GraphLoadingState(state) => {
            // a. Perform ContinueModuleLoading(payload, result)
            continue_module_loading(vm, state, result);
        }
        // 3. Else,
        ImportedModulePayload::PromiseCapability(promise_capability) => {
            // a. Perform ContinueDynamicImport(payload, result).
            continue_dynamic_import(vm, promise_capability, result);
        }
    }

    // 4. Return unused.
}
