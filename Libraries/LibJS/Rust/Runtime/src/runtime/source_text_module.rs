/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;
use std::rc::Rc;

use ak::{ScopeGuard, Utf16FlyString};
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ScriptOrModule;
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::parser_error::ParserError;
use crate::runtime::abstract_operations::{call, call_function_object, dispose_resources};
use crate::runtime::completion::{Completion, CompletionType, Must, ThrowCompletionOr};
use crate::runtime::cyclic_module::{CYCLIC_MODULE_METHODS, CyclicModule, ModuleStatus};
use crate::runtime::declarative_environment::DeclarativeEnvironment;
use crate::runtime::ecmascript_function_object::EcmascriptFunctionObject;
use crate::runtime::environment::{Environment, InitializeBindingHint};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::module::{
    ExportStarSet, Module, ModuleMethods, ResolveSet, ResolvedBinding, ResolvedBindingType, copy_of_resolve_set,
};
use crate::runtime::module_entry::{ExportEntry, ExportEntryKind, ImportEntry};
use crate::runtime::module_environment::ModuleEnvironment;
use crate::runtime::module_request::ModuleRequest;
use crate::runtime::object_environment::name_for_message;
use crate::runtime::private_environment::PrivateEnvironment;
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::shared_function_instance_data::{FunctionKind, SharedFunctionInstanceData};
use crate::source_code::SourceCode;
use libjs_rust::ast::ProgramType;
use libjs_rust::compile::{CompiledModule, ModuleDeclarations, ParsedProgram, compile_module, parse};

/// SourceTextModule::FunctionToInitialize.
#[derive(Trace)]
pub struct FunctionToInitialize {
    pub shared_data: Gc<SharedFunctionInstanceData>,
    pub name: Utf16FlyString,
}

/// SourceTextModule::LexicalBinding: `function_index` refers to the function it is initialized with.
#[derive(Clone, Trace)]
pub struct LexicalBinding {
    pub name: Utf16FlyString,
    pub is_constant: bool,
    pub function_index: Option<usize>,
}

/// The binding an import entry resolves to, which GetImport reads.
#[derive(Clone, Default, Trace)]
struct ImportedBinding {
    namespace: Option<Gc<Object>>,
    module: Option<Gc<Module>>,
    binding_name: Utf16FlyString,
    environment: Option<Gc<ModuleEnvironment>>,
    binding_index: u32,
}

// 16.2.1.6 Source Text Module Records, https://tc39.es/ecma262/#sec-source-text-module-records
#[repr(C)]
#[derive(Trace)]
pub struct SourceTextModule {
    base: CyclicModule,
    execution_context: OwnedExecutionContext, // [[Context]]
    import_meta: Cell<Option<Gc<Object>>>,    // [[ImportMeta]]
    #[gc(untraced)]
    import_entries: Vec<ImportEntry>, // [[ImportEntries]]
    #[gc(untraced)]
    local_export_entries: Vec<ExportEntry>, // [[LocalExportEntries]]
    #[gc(untraced)]
    indirect_export_entries: Vec<ExportEntry>, // [[IndirectExportEntries]]
    #[gc(untraced)]
    star_export_entries: Vec<ExportEntry>, // [[StarExportEntries]]

    // The binding each import entry resolves to, indexed like import_entries.
    imported_bindings: GcRefCell<Vec<ImportedBinding>>,

    // Pre-computed module declaration instantiation data.
    // These are extracted from the AST at construction time so that
    // initialize_environment() can run without walking the AST.
    var_declared_names: Vec<Utf16FlyString>,
    lexical_bindings: Vec<LexicalBinding>,
    functions_to_initialize: Vec<FunctionToInitialize>,
    default_export_binding_name: Option<Utf16FlyString>,

    executable: Option<Gc<Executable>>,
    tla_shared_data: Option<Gc<SharedFunctionInstanceData>>,
    /// What the module's functions compile themselves from when they are first called.
    #[gc(untraced)]
    source_code: Rc<SourceCode>,
}

define_cell!(SourceTextModule, Other, extends: [CyclicModule, Module]);

impl Deref for SourceTextModule {
    type Target = CyclicModule;

    fn deref(&self) -> &CyclicModule {
        &self.base
    }
}

pub const SOURCE_TEXT_MODULE_METHODS: ModuleMethods = ModuleMethods {
    get_exported_names: |module, vm, export_star_set| {
        as_source_text_module(module).get_exported_names(vm, export_star_set)
    },
    resolve_export: |module, vm, export_name, resolve_set| {
        as_source_text_module(module).resolve_export(vm, export_name, resolve_set)
    },
    ..CYCLIC_MODULE_METHODS
};

/// The Source Text Module Record a method of a source text module was called on.
fn as_source_text_module(module: &Module) -> &SourceTextModule {
    module
        .downcast_ref::<SourceTextModule>()
        .expect("only Source Text Module Records have the methods of source text modules")
}

fn fly_string_of(name: &libjs_rust::ast::Utf16String) -> Utf16FlyString {
    Utf16FlyString::from_utf16(&name.0)
}

fn export_entries_of(entries: Vec<libjs_rust::compile::ModuleExportEntry>) -> Vec<ExportEntry> {
    entries
        .into_iter()
        .map(|entry| ExportEntry {
            kind: entry.kind,
            export_name: entry.export_name.as_ref().map(fly_string_of),
            local_or_import_name: entry.local_or_import_name.as_ref().map(fly_string_of),
            module_request: ModuleRequest::of_entry_from_frontend(entry.module_request.as_ref()),
        })
        .collect()
}

impl SourceTextModule {
    // 16.2.1.7.1 ParseModule ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parsemodule
    pub fn parse(
        vm: &Vm,
        source_code: Rc<SourceCode>,
        realm: Gc<Realm>,
        filename: &str,
    ) -> Result<Gc<SourceTextModule>, Vec<ParserError>> {
        let source = source_code.code().to_utf16();
        let parsed = parse(&source, ProgramType::Module, 0);
        drop(source);
        if parsed.has_errors() {
            return Err(ParserError::all_from_parsed_program(&parsed));
        }
        Ok(Self::create_from_parsed(vm, parsed, source_code, realm, filename))
    }

    /// Compiles a module the caller parsed without errors from the code of `source_code`, whose filename the
    /// module's code reports, as `filename`, which module loading resolves the module's imports against.
    pub fn create_from_parsed(
        vm: &Vm,
        parsed: ParsedProgram,
        source_code: Rc<SourceCode>,
        realm: Gc<Realm>,
        filename: &str,
    ) -> Gc<SourceTextModule> {
        assert!(parsed.program_type() == ProgramType::Module && !parsed.has_errors());
        let source_length = source_code.length_in_code_units();
        Self::create(vm, realm, filename, compile_module(parsed, source_length), source_code)
    }

    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        filename: &str,
        compiled: CompiledModule,
        source_code: Rc<SourceCode>,
    ) -> Gc<SourceTextModule> {
        let CompiledModule {
            executable,
            declarations,
        } = compiled;
        let ModuleDeclarations {
            has_top_level_await,
            import_entries,
            default_export_binding_name,
            local_export_entries,
            indirect_export_entries,
            star_export_entries,
            var_names,
            functions_to_initialize,
            lexical_bindings,
            requested_modules,
        } = declarations;

        let import_entries = import_entries
            .into_iter()
            .map(|entry| ImportEntry {
                import_name: entry.import_name.as_ref().map(fly_string_of),
                local_name: fly_string_of(&entry.local_name),
                module_request: ModuleRequest::of_entry_from_frontend(Some(&entry.module_request)),
            })
            .collect();

        // The functions stay rooted until the module that holds them is allocated.
        let rooted_shared_data = MarkedVec::with_capacity(vm, functions_to_initialize.len());
        let mut function_names = Vec::with_capacity(functions_to_initialize.len());
        for function in functions_to_initialize {
            let mut description = function.description;
            // NB: C++ renames the shared data of an anonymous default export to the name the module binds it under
            //     once it is created, from which the function's name is all that follows.
            if function.is_anonymous_default_export {
                description.name.clone_from(&function.name.0);
            }
            rooted_shared_data.push(SharedFunctionInstanceData::create(vm, description, Some(&source_code)));
            function_names.push(fly_string_of(&function.name));
        }

        let lexical_bindings = lexical_bindings
            .iter()
            .map(|binding| LexicalBinding {
                name: fly_string_of(&binding.name),
                is_constant: binding.is_constant,
                function_index: binding.function_index,
            })
            .collect();

        let requested_modules = requested_modules.iter().map(ModuleRequest::from_frontend).collect();

        let (executable, tla_shared_data) = if has_top_level_await {
            let top_level_await_executable = Executable::create_with_source_code(vm, executable, Some(&source_code));
            let tla_shared_data = SharedFunctionInstanceData::create_without_function_ast(
                vm,
                FunctionKind::Async,
                Utf16FlyString::from_utf8("module code with top-level await"),
                0,
                0,
                true,
                false,
                true,
                Vec::new(),
            );
            tla_shared_data.set_is_module_wrapper(true);
            tla_shared_data.uses_this.set(true);
            tla_shared_data.function_environment_needed.set(true);
            tla_shared_data.update_asm_call_metadata();
            tla_shared_data.set_executable(Some(top_level_await_executable));
            (None, Some(tla_shared_data))
        } else {
            (
                Some(Executable::create_with_source_code(vm, executable, Some(&source_code))),
                None,
            )
        };

        let functions_to_initialize = rooted_shared_data
            .to_vec()
            .into_iter()
            .zip(function_names)
            .map(|(shared_data, name)| FunctionToInitialize { shared_data, name })
            .collect();

        let module = vm.heap().allocate(SourceTextModule {
            base: CyclicModule::new(
                Self::CLASS,
                realm,
                filename.to_string(),
                has_top_level_await,
                requested_modules,
            ),
            execution_context: OwnedExecutionContext::create(0, 0, 0),
            import_meta: Cell::new(None),
            import_entries,
            local_export_entries: export_entries_of(local_export_entries),
            indirect_export_entries: export_entries_of(indirect_export_entries),
            star_export_entries: export_entries_of(star_export_entries),
            imported_bindings: GcRefCell::new(Vec::new()),
            var_declared_names: var_names.iter().map(fly_string_of).collect(),
            lexical_bindings,
            functions_to_initialize,
            default_export_binding_name: default_export_binding_name.as_ref().map(fly_string_of),
            executable,
            tla_shared_data,
            source_code,
        });
        drop(rooted_shared_data);
        assert!(module.executable.is_some() || module.tla_shared_data.is_some_and(|data| data.executable().is_some()));
        module
    }

    pub fn import_meta(&self) -> Option<Gc<Object>> {
        self.import_meta.get()
    }

    pub fn set_import_meta(&self, import_meta: Gc<Object>) {
        self.import_meta.set(Some(import_meta));
    }

    // 16.2.1.7.2.1 GetExportedNames ( [ exportStarSet ] ), https://tc39.es/ecma262/#sec-getexportednames
    fn get_exported_names(&self, vm: &Vm, export_star_set: &ExportStarSet<'_>) -> Vec<Utf16FlyString> {
        // 1. Assert: module.[[Status]] is not NEW.
        assert_ne!(self.status(), ModuleStatus::New);

        // 2. If exportStarSet is not present, set exportStarSet to a new empty List.
        // NOTE: This is done by Module.

        // 3. If exportStarSet contains module, then
        if (0..export_star_set.len()).any(|index| export_star_set.get(index) == Some(self.as_gc())) {
            // a. Assert: We've reached the starting point of an export * circularity.
            // FIXME: How do we check that?

            // b. Return a new empty List.
            return Vec::new();
        }

        // 4. Append module to exportStarSet.
        export_star_set.push(self.as_gc());

        // 5. Let exportedNames be a new empty List.
        let mut exported_names = Vec::new();

        // 6. For each ExportEntry Record e of module.[[LocalExportEntries]], do
        for entry in &self.local_export_entries {
            // a. Assert: module provides the direct binding for this export.
            // FIXME: How do we check that?

            // b. Assert: e.[[ExportName]] is not null.
            // c. Append e.[[ExportName]] to exportedNames.
            exported_names.push(entry.export_name.clone().expect("a local export has an export name"));
        }

        // 7. For each ExportEntry Record e of module.[[IndirectExportEntries]], do
        for entry in &self.indirect_export_entries {
            // a. a. Assert: module imports a specific binding for this export.
            // FIXME: How do we check that?

            // b. Assert: e.[[ExportName]] is not null.
            // c. Append e.[[ExportName]] to exportedNames.
            exported_names.push(
                entry
                    .export_name
                    .clone()
                    .expect("an indirect export has an export name"),
            );
        }

        // 8. For each ExportEntry Record e of module.[[StarExportEntries]], do
        for entry_index in 0..self.star_export_entries.len() {
            let module_request = self.star_export_entries[entry_index].module_request().clone();

            // a. Assert: e.[[ModuleRequest]] is not null.
            // b. Let requestedModule be GetImportedModule(module, e.[[ModuleRequest]]).
            let requested_module = self.get_imported_module(&module_request);

            // c. Let starNames be requestedModule.GetExportedNames(exportStarSet).
            let star_names = requested_module.get_exported_names_with_export_star_set(vm, export_star_set);

            // d. For each element n of starNames, do
            for name in star_names {
                // i. If n is not "default", then
                // 1. If exportedNames does not contain n, then
                if Utf16FlyString::from_utf8("default") != name && !exported_names.contains(&name) {
                    // a. Append n to exportedNames.
                    exported_names.push(name);
                }
            }
        }

        // 9. Return exportedNames.
        exported_names
    }

    // 16.2.1.7.3.1 InitializeEnvironment ( ), https://tc39.es/ecma262/#sec-source-text-module-record-initialize-environment
    pub(crate) fn initialize_environment(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        // 1. For each ExportEntry Record e of module.[[IndirectExportEntries]], do
        for entry_index in 0..self.indirect_export_entries.len() {
            // a. Assert: e.[[ExportName]] is not null.
            let export_name = self.indirect_export_entries[entry_index]
                .export_name
                .clone()
                .expect("an indirect export has an export name");

            // a. Let resolution be module.ResolveExport(e.[[ExportName]]).
            let resolution = self.resolve_export(vm, &export_name, ResolveSet::new(vm));

            // b. If resolution is either null or AMBIGUOUS, throw a SyntaxError exception.
            if !resolution.is_valid() {
                return vm.throw_completion(
                    ErrorKind::SyntaxError,
                    ErrorType::InvalidOrAmbiguousExportEntry,
                    &[&name_for_message(&export_name)],
                );
            }

            // c. Assert: resolution is a ResolvedBinding Record.
        }

        // 2. Assert: All named exports from module are resolvable.
        // NOTE: We check all the indirect export entries above in step 1 and all the local named exports are resolvable by construction.

        // 3. Let realm be module.[[Realm]].
        // 4. Assert: realm is not undefined.
        let realm = self.realm();

        // 5. Let env be NewModuleEnvironment(realm.[[GlobalEnv]]).
        let environment = ModuleEnvironment::create(vm, Some(realm.global_environment().upcast()));

        // 6. Set module.[[Environment]] to env.
        self.set_environment(environment);

        // NB: A link that fails resets the module to unlinked, so this can run more than once.
        {
            let mut imported_bindings = self.imported_bindings.borrow_mut();
            imported_bindings.clear();
            imported_bindings.reserve(self.import_entries.len());
        }

        // 7. For each ImportEntry Record in of module.[[ImportEntries]], do
        for entry_index in 0..self.import_entries.len() {
            let import_entry = self.import_entries[entry_index].clone();

            // a. Let importedModule be GetImportedModule(module, in.[[ModuleRequest]]).
            let imported_module = self.get_imported_module(import_entry.module_request());

            // b. If in.[[ImportName]] is NAMESPACE-OBJECT, then
            let Some(import_name) = &import_entry.import_name else {
                // i. Let namespace be GetModuleNamespace(importedModule).
                let namespace = imported_module.get_module_namespace(vm);

                // ii. Perform ! env.CreateImmutableBinding(in.[[LocalName]], true).
                // iii. Perform ! env.InitializeBinding(in.[[LocalName]], namespace, normal).
                // AD-HOC: Both steps are performed after step 24.
                self.imported_bindings.borrow_mut().push(ImportedBinding {
                    namespace: Some(namespace),
                    ..ImportedBinding::default()
                });
                continue;
            };

            // c. Else,
            // i. Let resolution be importedModule.ResolveExport(in.[[ImportName]]).
            let resolution = imported_module.resolve_export(vm, import_name);

            // ii. If resolution is either null or AMBIGUOUS, throw a SyntaxError exception.
            if !resolution.is_valid() {
                return vm.throw_completion(
                    ErrorKind::SyntaxError,
                    ErrorType::InvalidOrAmbiguousExportEntry,
                    &[&name_for_message(import_name)],
                );
            }

            let resolved_module = resolution.module.expect("a resolved binding has a module");

            // iii. If resolution.[[BindingName]] is NAMESPACE, then
            if resolution.is_namespace() {
                // 1. Let namespace be GetModuleNamespace(resolution.[[Module]]).
                let namespace = resolved_module.get_module_namespace(vm);

                // 2. Perform ! env.CreateImmutableBinding(in.[[LocalName]], true).
                // 3. Perform ! env.InitializeBinding(in.[[LocalName]], namespace, normal).
                // AD-HOC: Both steps are performed after step 24.
                self.imported_bindings.borrow_mut().push(ImportedBinding {
                    namespace: Some(namespace),
                    ..ImportedBinding::default()
                });
            }
            // iv. Else,
            else {
                // 1. Perform env.CreateImportBinding(in.[[LocalName]], resolution.[[Module]], resolution.[[BindingName]]).
                environment
                    .create_import_binding(
                        import_entry.local_name.clone(),
                        Some(resolved_module),
                        resolution.export_name.clone(),
                    )
                    .must();
                self.imported_bindings.borrow_mut().push(ImportedBinding {
                    module: Some(resolved_module),
                    binding_name: resolution.export_name,
                    ..ImportedBinding::default()
                });
            }
        }

        // 8. Let moduleContext be a new ECMAScript code execution context.
        // NOTE: this has already been created during the construction of this object.

        // 9. Set the Function of moduleContext to null.

        // 10. Assert: module.[[Realm]] is not undefined.
        // NOTE: This must be true because we use a reference.

        // 11. Set the Realm of moduleContext to module.[[Realm]].
        self.execution_context.realm.set(Some(realm));

        // 12. Set the ScriptOrModule of moduleContext to module.
        self.execution_context
            .script_or_module
            .set(ScriptOrModule::Module(self.as_gc()));

        // 13. Set the VariableEnvironment of moduleContext to module.[[Environment]].
        self.execution_context
            .variable_environment
            .set(Some(environment.upcast()));

        // 14. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
        self.execution_context
            .lexical_environment
            .set(Some(environment.upcast()));

        // 15. Set the PrivateEnvironment of moduleContext to null.

        // 16. Set module.[[Context]] to moduleContext.
        // NOTE: We're already working on that one.

        // 17. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
        vm.push_execution_context_checking_stack_space(self.execution_context.as_non_null())?;

        // 18. Let code be module.[[ECMAScriptCode]].

        // 19. Let varDeclarations be the VarScopedDeclarations of code.
        // 20. Let declaredVarNames be a new empty List.
        let mut declared_var_names: Vec<Utf16FlyString> = Vec::new();

        // 21. For each element d of varDeclarations, do
        // a. For each element dn of the BoundNames of d, do
        for name in &self.var_declared_names {
            // i. If dn is not an element of declaredVarNames, then
            if !declared_var_names.contains(name) {
                // 1. Perform ! env.CreateMutableBinding(dn, false).
                environment.create_mutable_binding(vm, name, false).must();

                // 2. Perform ! env.InitializeBinding(dn, undefined, normal).
                environment
                    .initialize_binding(vm, name, Value::UNDEFINED, InitializeBindingHint::Normal)
                    .must();

                // 3. Append dn to declaredVarNames.
                declared_var_names.push(name.clone());
            }
        }

        // 22. Let lexDeclarations be the LexicallyScopedDeclarations of code.
        // 23. Let privateEnv be null.
        let private_environment: Option<Gc<PrivateEnvironment>> = None;

        // 24. For each element d of lexDeclarations, do
        for binding_index in 0..self.lexical_bindings.len() {
            let binding = self.lexical_bindings[binding_index].clone();

            // a. For each element dn of the BoundNames of d, do
            // i. If IsConstantDeclaration of d is true, then
            if binding.is_constant {
                // 1. Perform ! env.CreateImmutableBinding(dn, true).
                environment.create_immutable_binding(vm, &binding.name, true).must();
            }
            // ii. Else,
            else {
                // 1. Perform ! env.CreateMutableBinding(dn, false).
                environment.create_mutable_binding(vm, &binding.name, false).must();
            }

            // iii. If d is a FunctionDeclaration, a GeneratorDeclaration, an AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration, then
            if let Some(function_index) = binding.function_index {
                let shared_data = self.functions_to_initialize[function_index].shared_data;

                // 1. Let fo be InstantiateFunctionObject of d with arguments env and privateEnv.
                let function = EcmascriptFunctionObject::create_from_function_data(
                    vm,
                    realm,
                    shared_data,
                    Some(environment.upcast()),
                    private_environment,
                );

                // 2. Perform ! env.InitializeBinding(dn, fo, normal).
                environment
                    .initialize_binding(
                        vm,
                        &binding.name,
                        Value::from_object(function),
                        InitializeBindingHint::Normal,
                    )
                    .must();
            }
        }

        // NOTE: The default export name is also part of the local lexical declarations but instead of making that a special
        //       case in the parser we just check it here. This is only needed for things which are not declarations. For more
        //       info check Parser::parse_export_statement. Furthermore, that declaration is not constant. so we take 24.a.ii.
        if let Some(default_export_binding_name) = &self.default_export_binding_name {
            environment
                .create_mutable_binding(vm, default_export_binding_name, false)
                .must();
        }

        // AD-HOC: Steps 7.b.ii-iii and 7.c.iii.2-3 are performed here. The bytecode generator places the module's own
        //         declarations first in the module environment, since it cannot know which imports are namespaces.
        for entry_index in 0..self.import_entries.len() {
            let Some(namespace) = self.imported_bindings.borrow()[entry_index].namespace else {
                continue;
            };
            let local_name = &self.import_entries[entry_index].local_name;

            // Perform ! env.CreateImmutableBinding(in.[[LocalName]], true).
            environment.create_immutable_binding(vm, local_name, true).must();

            // Perform ! env.InitializeBinding(in.[[LocalName]], namespace, normal).
            environment
                .initialize_binding(
                    vm,
                    local_name,
                    Value::from_object(namespace),
                    InitializeBindingHint::Normal,
                )
                .must();
        }

        // 25. Remove moduleContext from the execution context stack.
        vm.pop_execution_context();

        // 26. Return unused.
        Ok(())
    }

    pub fn get_imported_binding_value(
        &self,
        vm: &Vm,
        import_index: u32,
        local_name: &Utf16FlyString,
    ) -> ThrowCompletionOr<Value> {
        let import_index = import_index as usize;
        assert!(self.import_entries[import_index].local_name == *local_name);
        let imported_binding = self.imported_bindings.borrow()[import_index].clone();
        if let Some(namespace) = imported_binding.namespace {
            return Ok(Value::from_object(namespace));
        }

        let (environment, binding_index) = match imported_binding.environment {
            Some(environment) => (environment, imported_binding.binding_index),
            None => {
                let Some(target_environment) = imported_binding
                    .module
                    .expect("a direct import binding has a module")
                    .environment()
                else {
                    return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::ModuleNoEnvironment, &[]);
                };

                let mut binding_index = None;
                target_environment
                    .has_binding(&imported_binding.binding_name, Some(&mut binding_index))
                    .must();
                let binding_index = u32::try_from(binding_index.expect("the target module binds the name directly"))
                    .expect("the binding index fits in u32");
                {
                    let mut imported_bindings = self.imported_bindings.borrow_mut();
                    imported_bindings[import_index].environment = Some(target_environment);
                    imported_bindings[import_index].binding_index = binding_index;
                }
                (target_environment, binding_index)
            }
        };

        environment.get_binding_value_direct(vm, binding_index as usize)
    }

    // 16.2.1.7.2.2 ResolveExport ( exportName [ , resolveSet ] ), https://tc39.es/ecma262/#sec-resolveexport
    fn resolve_export(&self, vm: &Vm, export_name: &Utf16FlyString, resolve_set: ResolveSet<'_>) -> ResolvedBinding {
        // 1. Assert: module.[[Status]] is not NEW.
        assert_ne!(self.status(), ModuleStatus::New);

        // 2. If resolveSet is not present, set resolveSet to a new empty List.
        // NOTE: This is done by the default argument.

        // 3. For each Record { [[Module]], [[ExportName]] } r of resolveSet, do
        for index in 0..resolve_set.len() {
            let record = resolve_set.get(index).expect("the index is in bounds");
            // a. If module and r.[[Module]] are the same Module Record and exportName is r.[[ExportName]], then
            if record.module == Some(self.as_gc()) && record.export_name == *export_name {
                // i. Assert: This is a circular import request.

                // ii. Return null.
                return ResolvedBinding::null();
            }
        }

        // 4. Append the Record { [[Module]]: module, [[ExportName]]: exportName } to resolveSet.
        resolve_set.push(ResolvedBinding {
            binding_type: ResolvedBindingType::BindingName,
            module: Some(self.as_gc()),
            export_name: export_name.clone(),
        });

        // 5. For each ExportEntry Record e of module.[[LocalExportEntries]], do
        for entry in &self.local_export_entries {
            // a. If e.[[ExportName]] is exportName, then
            if entry.export_name.as_ref() != Some(export_name) {
                continue;
            }

            // i. Assert: module provides the direct binding for this export.
            // FIXME: What does this mean?

            // ii. Return ResolvedBinding Record { [[Module]]: module, [[BindingName]]: e.[[LocalName]] }.
            return ResolvedBinding {
                binding_type: ResolvedBindingType::BindingName,
                module: Some(self.as_gc()),
                export_name: entry
                    .local_or_import_name
                    .clone()
                    .expect("a local export has a local name"),
            };
        }

        // 5. For each ExportEntry Record e of module.[[IndirectExportEntries]], do
        for entry_index in 0..self.indirect_export_entries.len() {
            let entry = self.indirect_export_entries[entry_index].clone();

            // a. If e.[[ExportName]] is exportName, then
            if entry.export_name.as_ref() != Some(export_name) {
                continue;
            }

            // i. Assert: e.[[ModuleRequest]] is not null.
            // ii. Let importedModule be GetImportedModule(module, e.[[ModuleRequest]]).
            let imported_module = self.get_imported_module(entry.module_request());

            // iii. If e.[[ImportName]] is all, then
            if entry.kind == ExportEntryKind::ModuleRequestAll {
                // 1. Assert: module does not provide the direct binding for this export.
                // FIXME: What does this mean? / How do we check this

                // 2. Return ResolvedBinding Record { [[Module]]: importedModule, [[BindingName]]: NAMESPACE }.
                return ResolvedBinding {
                    binding_type: ResolvedBindingType::Namespace,
                    module: Some(imported_module),
                    export_name: Utf16FlyString::default(),
                };
            }
            // iv. Else,
            // 1. Assert: module imports a specific binding for this export.
            // FIXME: What does this mean? / How do we check this

            // 2. Return importedModule.ResolveExport(e.[[ImportName]], resolveSet).
            return imported_module.resolve_export_with_resolve_set(
                vm,
                &entry
                    .local_or_import_name
                    .expect("an indirect export of a binding has an import name"),
                copy_of_resolve_set(vm, &resolve_set),
            );
        }

        // 7. If exportName is "default", then
        if *export_name == Utf16FlyString::from_utf8("default") {
            // a. Assert: A default export was not explicitly defined by this module.
            // FIXME: What does this mean? / How do we check this

            // b. Return null.
            return ResolvedBinding::null();

            // c. NOTE: A default export cannot be provided by an export * from "mod" declaration.
        }

        // 8. Let starResolution be null.
        let mut star_resolution = ResolvedBinding::null();

        // 9. For each ExportEntry Record e of module.[[StarExportEntries]], do
        for entry_index in 0..self.star_export_entries.len() {
            let module_request = self.star_export_entries[entry_index].module_request().clone();

            // a. Assert: e.[[ModuleRequest]] is not null.
            // b. Let importedModule be GetImportedModule(module, e.[[ModuleRequest]]).
            let imported_module = self.get_imported_module(&module_request);

            // c. Let resolution be importedModule.ResolveExport(exportName, resolveSet).
            let resolution =
                imported_module.resolve_export_with_resolve_set(vm, export_name, copy_of_resolve_set(vm, &resolve_set));

            // d. If resolution is AMBIGUOUS, return AMBIGUOUS.
            if resolution.is_ambiguous() {
                return ResolvedBinding::ambiguous();
            }

            // e. If resolution is not null, then
            if resolution.binding_type == ResolvedBindingType::Null {
                continue;
            }

            // i. Assert: resolution is a ResolvedBinding Record.
            assert!(resolution.is_valid());

            // ii. If starResolution is null, set starResolution to resolution.
            if star_resolution.binding_type == ResolvedBindingType::Null {
                star_resolution = resolution;
            }
            // iii. Else,
            else {
                // 1. Assert: There is more than one * export that includes the requested name.
                // FIXME: Assert this

                // 2. If resolution.[[Module]] and starResolution.[[Module]] are not the same Module Record, return AMBIGUOUS.
                if resolution.module != star_resolution.module {
                    return ResolvedBinding::ambiguous();
                }

                // 3. If resolution.[[BindingName]] is not starResolution.[[BindingName]] and either resolution.[[BindingName]]
                //    or starResolution.[[BindingName]] is NAMESPACE, return AMBIGUOUS.
                if resolution.is_namespace() != star_resolution.is_namespace() {
                    return ResolvedBinding::ambiguous();
                }

                // 4. If resolution.[[BindingName]] is a String, starResolution.[[BindingName]] is a String, and
                //    resolution.[[BindingName]] is not starResolution.[[BindingName]], return ambiguous.
                // NOTE: We know from the previous step that either both are namespaces or both are string, so we can check just one.
                if !resolution.is_namespace() && resolution.export_name != star_resolution.export_name {
                    return ResolvedBinding::ambiguous();
                }
            }
        }

        // 10. Return starResolution.
        star_resolution
    }

    // 16.2.1.6.5 ExecuteModule ( [ capability ] ), https://tc39.es/ecma262/#sec-source-text-module-record-execute-module
    // 9.1.1.1.2 ExecuteModule ( [ capability ] ), https://tc39.es/proposal-explicit-resource-management/#sec-source-text-module-record-execute-module
    pub(crate) fn execute_module(&self, vm: &Vm, capability: Option<Gc<PromiseCapability>>) -> ThrowCompletionOr<()> {
        assert!(self.has_top_level_await() || self.executable.is_some());

        let (registers_and_locals_count, constant_count) = match self.executable {
            Some(executable) => (
                executable.registers_and_locals_count(),
                u32::try_from(executable.constants().len()).expect("the constant count fits in u32"),
            ),
            None => (0, 0),
        };

        // 1. Let moduleContext be a new ECMAScript code execution context.
        let stack = vm.interpreter_stack();
        let stack_mark = stack.top.get();
        let Some(module_context) = stack.allocate(registers_and_locals_count, constant_count, 0) else {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        };
        let _deallocate_guard = ScopeGuard::new(|| stack.deallocate(stack_mark));
        // SAFETY: The context was just allocated and stays allocated until the guard frees it.
        let module_context_ref = unsafe { module_context.as_ref() };

        // 2. Set the Function of moduleContext to null.

        // 3. Set the Realm of moduleContext to module.[[Realm]].
        module_context_ref.realm.set(Some(self.realm()));

        // 4. Set the ScriptOrModule of moduleContext to module.
        module_context_ref
            .script_or_module
            .set(ScriptOrModule::Module(self.as_gc()));

        // 5. Assert: module has been linked and declarations in its module environment have been instantiated.
        assert!(!matches!(
            self.status(),
            ModuleStatus::New | ModuleStatus::Unlinked | ModuleStatus::Linking
        ));
        let environment = self.environment().expect("a linked module has an environment");

        // 6. Set the VariableEnvironment of moduleContext to module.[[Environment]].
        module_context_ref.variable_environment.set(Some(environment.upcast()));

        // 7. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
        module_context_ref.lexical_environment.set(Some(environment.upcast()));

        // 8. Suspend the currently running execution context.
        // NOTE: Done by the push of execution context in steps below.

        vm.enter_module_execution();
        let _leave_module_execution = ScopeGuard::new(|| vm.leave_module_execution());

        // 9. If module.[[HasTLA]] is false, then
        if !self.has_top_level_await() {
            // a. Assert: capability is not present.
            assert!(capability.is_none());

            // b. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
            vm.push_execution_context_checking_stack_space(module_context)?;

            // c. Let result be the result of evaluating module.[[ECMAScriptCode]].
            let executable = self
                .executable
                .expect("a module without top-level await has an executable");
            let mut result = match vm.run_executable(module_context, executable, 0) {
                Err(exception) => Completion::new(CompletionType::Throw, exception),
                Ok(value) if value.is_empty() => Completion::normal(Value::UNDEFINED),
                Ok(value) => Completion::normal(value),
            };

            // d. Let env be moduleContext's LexicalEnvironment.
            let env = module_context_ref
                .lexical_environment
                .get()
                .and_then(|environment| environment.downcast::<DeclarativeEnvironment>())
                .expect("the lexical environment of a module is declarative");

            // e. Set result to Completion(DisposeResources(env.[[DisposeCapability]], result)).
            // NB: No instruction gives a module environment a dispose capability, so this never runs JavaScript while
            //     it refers into the environment.
            if let Some(dispose_capability) = env.dispose_capability_if_exists() {
                result = dispose_resources(vm, dispose_capability, result);
            }

            // f. Suspend moduleContext and remove it from the execution context stack.
            vm.pop_execution_context();

            // g. Resume the context that is now on the top of the execution context stack as the running execution context.
            // FIXME: We don't have resume yet.

            // h. If result is an abrupt completion, then
            if result.is_error() {
                // i. Return ? result.
                return Err(result.release_error());
            }
        }
        // 10. Else,
        else {
            // a. Assert: capability is a PromiseCapability Record.
            let capability = capability.expect("a module with top-level await is executed with a capability");

            // b. Perform AsyncBlockStart(capability, module.[[ECMAScriptCode]], moduleContext).

            // AD-HOC: We implement asynchronous execution via synthetic generator functions,
            //         so we fake "AsyncBlockStart" here by creating an async function to wrap
            //         the top-level module code.
            // FIXME: Improve this situation, so we can match the spec better.

            // NOTE: Like AsyncBlockStart, we need to push/pop the moduleContext around the function construction to ensure that
            //       the async execution context captures the module execution context.
            vm.push_execution_context(module_context);

            let module_wrapper_function = EcmascriptFunctionObject::create_from_function_data(
                vm,
                self.realm(),
                self.tla_shared_data
                    .expect("a module with top-level await has the shared data of its wrapper"),
                Some(environment.upcast::<Environment>()),
                None,
            );

            vm.pop_execution_context();

            let result = call(vm, Value::from_object(module_wrapper_function), Value::UNDEFINED, &[]);

            // AD-HOC: This is basically analogous to what AsyncBlockStart would do.
            match result {
                Err(error) => {
                    call_function_object(vm, capability.reject(), Value::UNDEFINED, &[error.value()]).must();
                }
                Ok(value) => {
                    call_function_object(vm, capability.resolve(), Value::UNDEFINED, &[value]).must();
                }
            }
        }

        // 11. Return unused.
        Ok(())
    }
}
