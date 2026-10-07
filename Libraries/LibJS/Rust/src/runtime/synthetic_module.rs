/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::OnceCell;
use core::ops::Deref;

use ak::{ScopeGuard, Utf16FlyString};
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ScriptOrModule;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::json_object::JSONObject;
use crate::runtime::module::{
    ExportStarSet, MODULE_METHODS, Module, ModuleMethods, ResolveSet, ResolvedBinding, ResolvedBindingType,
};
use crate::runtime::module_environment::ModuleEnvironment;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise_capability::{PromiseCapability, new_promise_capability};
use crate::utf16::Utf16View;

/// The [[EvaluationSteps]] of a Synthetic Module Record together with the state they capture.
trait EvaluationSteps: Trace + 'static {
    fn call(&self, vm: &Vm, module: &SyntheticModule) -> ThrowCompletionOr<()>;
}

struct CapturingEvaluationSteps<C, F> {
    captures: C,
    steps: F,
}

// SAFETY: The steps are zero-sized, so the captures are all the cells this reaches.
unsafe impl<C: Trace, F> Trace for CapturingEvaluationSteps<C, F> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

impl<C, F> EvaluationSteps for CapturingEvaluationSteps<C, F>
where
    C: Trace + 'static,
    F: Fn(&Vm, &SyntheticModule, &C) -> ThrowCompletionOr<()> + 'static,
{
    fn call(&self, vm: &Vm, module: &SyntheticModule) -> ThrowCompletionOr<()> {
        (self.steps)(vm, module, &self.captures)
    }
}

// 16.2.1.8 Synthetic Module Records, https://tc39.es/ecma262/#sec-synthetic-module-records
#[repr(C)]
#[derive(Trace)]
pub struct SyntheticModule {
    base: Module,
    export_names: Vec<Utf16FlyString>,                    // [[ExportNames]]
    evaluation_steps: OnceCell<Box<dyn EvaluationSteps>>, // [[EvaluationSteps]]
}

define_cell!(SyntheticModule, Other, extends: [Module]);

impl Deref for SyntheticModule {
    type Target = Module;

    fn deref(&self) -> &Module {
        &self.base
    }
}

pub const SYNTHETIC_MODULE_METHODS: ModuleMethods = ModuleMethods {
    load_requested_modules: |module, vm, _| as_synthetic_module(module).load_requested_modules(vm),
    get_exported_names: |module, _, _: &ExportStarSet<'_>| as_synthetic_module(module).get_exported_names(),
    resolve_export: |module, _, export_name, _: ResolveSet<'_>| as_synthetic_module(module).resolve_export(export_name),
    link: |module, vm| as_synthetic_module(module).link(vm),
    evaluate: |module, vm| as_synthetic_module(module).evaluate(vm),
    ..MODULE_METHODS
};

/// The Synthetic Module Record a method of a synthetic module was called on.
fn as_synthetic_module(module: &Module) -> &SyntheticModule {
    module
        .downcast_ref::<SyntheticModule>()
        .expect("only Synthetic Module Records have the methods of synthetic modules")
}

impl SyntheticModule {
    /// A synthetic module of `realm` that exports `export_names` and evaluates with `evaluation_steps`. The steps must
    /// not capture anything: the state they need is `captures`, which the module keeps alive.
    pub fn create<C, F>(
        vm: &Vm,
        realm: Gc<Realm>,
        export_names: Vec<Utf16FlyString>,
        captures: C,
        evaluation_steps: F,
        filename: String,
    ) -> Gc<SyntheticModule>
    where
        C: Trace + 'static,
        F: Fn(&Vm, &SyntheticModule, &C) -> ThrowCompletionOr<()> + 'static,
    {
        const {
            assert!(
                size_of::<F>() == 0,
                "the evaluation steps capture their state through their captures"
            );
        };
        let module = vm.heap().allocate(SyntheticModule {
            base: Module::new(Self::CLASS, realm, filename, ForeignCellSlot::empty()),
            export_names,
            evaluation_steps: OnceCell::new(),
        });
        // The captures stay on the stack, where the collector finds them, until the module that keeps them alive
        // exists. Nothing allocates from the heap between allocating the module and storing them in it.
        let steps: Box<dyn EvaluationSteps> = Box::new(CapturingEvaluationSteps {
            captures,
            steps: evaluation_steps,
        });
        if module.evaluation_steps.set(steps).is_err() {
            unreachable!("the evaluation steps are stored once");
        }
        module
    }

    // 16.2.1.8.1 CreateDefaultExportSyntheticModule ( defaultExport ), https://tc39.es/ecma262/#sec-create-default-export-synthetic-module
    pub fn create_default_export_synthetic_module(
        vm: &Vm,
        realm: Gc<Realm>,
        default_export: Value,
        filename: String,
    ) -> Gc<SyntheticModule> {
        // 1. Let realm be the current Realm Record.

        // 2. Let setDefaultExport be a new Abstract Closure with parameters (module) that captures defaultExport and
        //    performs the following steps when called:
        // 2. Return the Synthetic Module Record { [[Realm]]: realm, [[Environment]]: empty, [[Namespace]]: empty, [[HostDefined]]: undefined, [[ExportNames]]: « "default" », [[EvaluationSteps]]: setDefaultExport }.
        Self::create(
            vm,
            realm,
            vec![Utf16FlyString::from_utf8("default")],
            default_export,
            |vm, module, &default_export: &Value| {
                // a. Perform SetSyntheticModuleExport(module, "default", defaultExport).
                module.set_synthetic_module_export(vm, &Utf16FlyString::from_utf8("default"), default_export)?;

                // b. Return NormalCompletion(UNUSED).
                Ok(())
            },
            filename,
        )
    }

    fn as_synthetic_module_gc(&self) -> Gc<SyntheticModule> {
        // SAFETY: Modules only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    // 16.2.1.8.3 SetSyntheticModuleExport ( module, exportName, exportValue ), https://tc39.es/ecma262/#sec-setsyntheticmoduleexport
    pub fn set_synthetic_module_export(
        &self,
        vm: &Vm,
        export_name: &Utf16FlyString,
        export_value: Value,
    ) -> ThrowCompletionOr<()> {
        // 1. Assert: module.[[ExportNames]] contains exportName.
        assert!(self.export_names.contains(export_name));

        // 2. Let envRec be module.[[Environment]].
        // 3. Assert: envRec is not EMPTY.
        let environment_record = self
            .environment()
            .expect("a linked synthetic module has an environment");

        // 4. Perform envRec.SetMutableBinding(exportName, exportValue, true).
        environment_record.set_mutable_binding(vm, export_name, export_value, true)?;

        // 5. Return UNUSED.
        Ok(())
    }

    // 16.2.1.8.4.1 LoadRequestedModules ( ), https://tc39.es/ecma262/#sec-smr-LoadRequestedModules
    fn load_requested_modules(&self, vm: &Vm) -> Gc<PromiseCapability> {
        let realm = self.realm();

        // 1. Return ! PromiseResolve(%Promise%, undefined).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();
        call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[Value::UNDEFINED]).must();

        // NOTE: We need to return a PromiseCapability, rather than a Promise, so we flatten PromiseResolve here.
        //       This is likely a spec bug, see https://matrixlogs.bakkot.com/WHATWG/2023-02-13#L1
        promise_capability
    }

    // 16.2.1.8.4.2 GetExportedNames ( ), https://tc39.es/ecma262/#sec-smr-getexportednames
    fn get_exported_names(&self) -> Vec<Utf16FlyString> {
        // 1. Return module.[[ExportNames]].
        self.export_names.clone()
    }

    // 16.2.1.8.4.3 ResolveExport ( exportName ), https://tc39.es/ecma262/#sec-smr-resolveexport
    fn resolve_export(&self, export_name: &Utf16FlyString) -> ResolvedBinding {
        // 1. If module.[[ExportNames]] does not contain exportName, return null.
        if !self.export_names.contains(export_name) {
            return ResolvedBinding::null();
        }

        // 2. Return ResolvedBinding Record { [[Module]]: module, [[BindingName]]: exportName }.
        ResolvedBinding {
            binding_type: ResolvedBindingType::BindingName,
            module: Some(self.as_gc()),
            export_name: export_name.clone(),
        }
    }

    // 16.2.1.8.4.4 Link ( ), https://tc39.es/ecma262/#sec-smr-Link
    #[allow(clippy::unnecessary_wraps, reason = "the method's type lets other modules throw")]
    fn link(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        // 1. Let realm be module.[[Realm]].
        let realm = self.realm();

        // 2. Let env be NewModuleEnvironment(realm.[[GlobalEnv]]).
        let environment = ModuleEnvironment::create(vm, Some(realm.global_environment().upcast()));

        // 3. Set module.[[Environment]] to env.
        self.set_environment(environment);

        // 4. For each String exportName of module.[[ExportNames]], do
        for export_name in &self.export_names {
            // a. Perform ! env.CreateMutableBinding(exportName, false).
            environment.create_mutable_binding(vm, export_name, false).must();

            // b. Perform ! env.InitializeBinding(exportName, undefined).
            environment
                .initialize_binding(vm, export_name, Value::UNDEFINED, InitializeBindingHint::Normal)
                .must();
        }

        // 5. Return NormalCompletion(unused).
        Ok(())
    }

    // 16.2.1.8.4.5 Evaluate ( ), https://tc39.es/ecma262/#sec-smr-Evaluate
    fn evaluate(&self, vm: &Vm) -> ThrowCompletionOr<Gc<PromiseCapability>> {
        let realm = self.realm();

        // 1. Let moduleContext be a new ECMAScript code execution context.
        // 2. Set the Function of moduleContext to null.
        let stack = vm.interpreter_stack();
        let stack_mark = stack.top.get();
        let Some(module_context) = stack.allocate(0, 0, 0) else {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        };
        let _deallocate_guard = ScopeGuard::new(|| stack.deallocate(stack_mark));
        // SAFETY: The context was just allocated and stays allocated until the guard frees it.
        let module_context_ref = unsafe { module_context.as_ref() };

        // 3. Set the Realm of moduleContext to module.[[Realm]].
        module_context_ref.realm.set(Some(realm));

        // 4. Set the ScriptOrModule of moduleContext to module.
        module_context_ref
            .script_or_module
            .set(ScriptOrModule::Module(self.as_gc()));

        // 5. Set the VariableEnvironment of moduleContext to module.[[Environment]].
        let environment = self.environment().map(Gc::upcast);
        module_context_ref.variable_environment.set(environment);

        // 6. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
        module_context_ref.lexical_environment.set(environment);

        // 7. Suspend the running execution context.
        // 8. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
        vm.push_execution_context_checking_stack_space(module_context)?;

        // 9. Let steps be module.[[EvaluationSteps]].
        // 10. Let result be Completion(steps(module)).
        let module = self.as_synthetic_module_gc();
        let result = module
            .evaluation_steps
            .get()
            .expect("a synthetic module has its evaluation steps")
            .call(vm, &module);

        // 11. Suspend moduleContext and remove it from the execution context stack.
        // 12. Resume the context that is now on the top of the execution context stack as the running execution context.
        vm.pop_execution_context();

        // 13. Let pc be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        match result {
            // 14. IfAbruptRejectPromise(result, pc).
            Err(error) => {
                call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error.value()]).must();
            }
            // 15. Perform ! Call(pc.[[Resolve]], undefined, « undefined »).
            Ok(()) => {
                call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[Value::UNDEFINED]).must();
            }
        }

        // 16. Return pc.[[Promise]].
        // AD-HOC: Return the promise capability and let the caller unwrap the promise
        Ok(promise_capability)
    }
}

// 16.2.1.8.2 ParseJSONModule ( source ), https://tc39.es/ecma262/#sec-create-default-export-synthetic-module
pub fn parse_json_module(
    vm: &Vm,
    realm: Gc<Realm>,
    source_text: Utf16View<'_>,
    filename: String,
) -> ThrowCompletionOr<Gc<SyntheticModule>> {
    // 1. Let json be ? ParseJSON(source).
    let json = JSONObject::parse_json(vm, source_text, None)?;

    // 3. Return CreateDefaultExportSyntheticModule(json).
    Ok(SyntheticModule::create_default_export_synthetic_module(
        vm, realm, json, filename,
    ))
}

// 16.2.1.8.5 CreateTextModule ( source ), https://tc39.es/proposal-import-text/#sec-create-text-module
pub fn create_text_module(vm: &Vm, realm: Gc<Realm>, source: Utf16View<'_>, filename: String) -> Gc<SyntheticModule> {
    // 1. Return CreateDefaultExportSyntheticModule(source).
    let source = Value::from_string(PrimitiveString::create_from_utf16_view(vm, source));
    SyntheticModule::create_default_export_synthetic_module(vm, realm, source, filename)
}
