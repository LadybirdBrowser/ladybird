/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::HashSet;
use std::rc::Rc;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::bytecode::bytecode_cache::{
    BytecodeCacheInstall, DecodedBytecodeCache, ExecutableBacking, create_executable_and_its_functions,
    failed_to_materialize_bytecode_cache, functions_created_by, have_only_bytecode_cache_compile_inputs,
};
use crate::bytecode::executable::Executable;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::hash_table::Utf16FlyStringHashTable;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::parser_error::ParserError;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::ecmascript_function_object::EcmascriptFunctionObject;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::global_environment::GlobalEnvironment;
use crate::runtime::module_request::LoadedModuleRequest;
use crate::runtime::private_environment::PrivateEnvironment;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::source_code::SourceCode;
use libjs_rust::ast::{ProgramType, Utf16String};
use libjs_rust::bytecode_cache::DecodedDeclarationMetadata;
use libjs_rust::compile::{CompiledScript, ParsedProgram, compile_script, parse};

/// Script::FunctionToInitialize.
#[derive(Trace)]
pub struct FunctionToInitialize {
    pub shared_data: Gc<SharedFunctionInstanceData>,
    pub name: Utf16FlyString,
}

/// Script::LexicalBinding.
#[derive(Clone, Trace)]
pub struct LexicalBinding {
    pub name: Utf16FlyString,
    pub is_constant: bool,
}

// 16.1.4 Script Records, https://tc39.es/ecma262/#sec-script-records
#[repr(C)]
#[derive(Trace)]
pub struct Script {
    header: CellHeader,
    realm: Gc<Realm>,                                    // [[Realm]]
    loaded_modules: GcRefCell<Vec<LoadedModuleRequest>>, // [[LoadedModules]]
    host_defined: ForeignCellSlot,                       // [[HostDefined]]
    executable: Cell<Gc<Executable>>,
    #[gc(untraced)]
    executable_backing: Cell<ExecutableBacking>,
    /// What the script's functions compile themselves from when they are first called.
    #[gc(untraced)]
    source_code: Rc<SourceCode>,

    // Pre-computed global declaration instantiation data.
    // These are extracted from the AST at parse time so that GDI can run
    // without needing to walk the AST.
    lexical_names: Vec<Utf16FlyString>,
    var_names: Vec<Utf16FlyString>,
    functions_to_initialize: Vec<FunctionToInitialize>,
    #[gc(untraced)]
    declared_function_names: HashSet<Utf16FlyString>,
    var_scoped_names: Vec<Utf16FlyString>,
    annex_b_candidate_names: Vec<Utf16FlyString>,
    lexical_bindings: Vec<LexicalBinding>,
    is_strict_mode: bool,

    // Needed for potential lookups of modules.
    filename: String,
}

define_cell!(Script, Other);

fn fly_string_of(name: &Utf16String) -> Utf16FlyString {
    Utf16FlyString::from_utf16(&name.0)
}

fn fly_strings_of(names: &[Utf16String]) -> Vec<Utf16FlyString> {
    names.iter().map(fly_string_of).collect()
}

/// What a Script Record is made of, however its code was compiled.
struct ScriptParts {
    executable: Gc<Executable>,
    executable_backing: ExecutableBacking,
    function_names: Vec<Utf16FlyString>,
    lexical_names: Vec<Utf16FlyString>,
    var_names: Vec<Utf16FlyString>,
    var_scoped_names: Vec<Utf16FlyString>,
    annex_b_candidate_names: Vec<Utf16FlyString>,
    lexical_bindings: Vec<LexicalBinding>,
}

impl Script {
    // 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
    pub fn parse(vm: &Vm, source: &[u16], realm: Gc<Realm>) -> Result<Gc<Script>, Vec<ParserError>> {
        Self::parse_with_filename(vm, source, realm, "")
    }

    /// ParseScript of a script from `filename`, which the script's code reports and module loading resolves the
    /// specifiers of its dynamic imports against.
    pub fn parse_with_filename(
        vm: &Vm,
        source: &[u16],
        realm: Gc<Realm>,
        filename: &str,
    ) -> Result<Gc<Script>, Vec<ParserError>> {
        Self::parse_with_host_defined(
            vm,
            source,
            realm,
            filename,
            ak::Utf16String::default(),
            ForeignCellSlot::empty(),
            1,
        )
    }

    /// ParseScript as C++ Script::parse runs it for a host: the script's code reports `display_filename`, or
    /// `filename` if that is empty, its lines count from `line_number_offset`, and it keeps `host_defined` as its
    /// [[HostDefined]].
    pub fn parse_with_host_defined(
        vm: &Vm,
        source: &[u16],
        realm: Gc<Realm>,
        filename: &str,
        display_filename: ak::Utf16String,
        host_defined: ForeignCellSlot,
        line_number_offset: usize,
    ) -> Result<Gc<Script>, Vec<ParserError>> {
        let parsed = parse(source, ProgramType::Script, line_number_offset);
        if parsed.has_errors() {
            return Err(ParserError::all_from_parsed_program(&parsed));
        }
        let display_filename = if display_filename.is_empty() {
            ak::Utf16String::from_utf8(filename)
        } else {
            display_filename
        };
        let source_code = SourceCode::create(display_filename, ak::Utf16String::from_utf16(source));
        let source_length = source_code.length_in_code_units();
        Ok(Self::create(
            vm,
            realm,
            compile_script(parsed, source_length),
            source_code,
            filename,
            host_defined,
            ExecutableBacking::Source,
        ))
    }

    /// Compiles a script the caller parsed without errors from `source`.
    pub fn compile_parsed_program(vm: &Vm, parsed: ParsedProgram, source: &[u16], realm: Gc<Realm>) -> Gc<Script> {
        Self::compile_parsed_program_with_filename(vm, parsed, source, realm, ak::Utf16String::default())
    }

    /// Compiles a script the caller parsed without errors from `source`, which came from `filename`.
    pub fn compile_parsed_program_with_filename(
        vm: &Vm,
        parsed: ParsedProgram,
        source: &[u16],
        realm: Gc<Realm>,
        filename: ak::Utf16String,
    ) -> Gc<Script> {
        let source_code = SourceCode::create(filename, ak::Utf16String::from_utf16(source));
        Self::create_from_parsed(vm, parsed, source_code, realm)
    }

    /// Compiles a script the caller parsed without errors from the code of `source_code`, whose filename the
    /// script's code reports.
    pub fn create_from_parsed(
        vm: &Vm,
        parsed: ParsedProgram,
        source_code: Rc<SourceCode>,
        realm: Gc<Realm>,
    ) -> Gc<Script> {
        Self::create_from_parsed_with_filename(vm, parsed, source_code, realm, "")
    }

    /// Compiles a script the caller parsed without errors from the code of `source_code`, whose filename the
    /// script's code reports, as `filename`, which module loading resolves the specifiers of its dynamic imports
    /// against.
    pub fn create_from_parsed_with_filename(
        vm: &Vm,
        parsed: ParsedProgram,
        source_code: Rc<SourceCode>,
        realm: Gc<Realm>,
        filename: &str,
    ) -> Gc<Script> {
        assert!(parsed.program_type() == ProgramType::Script && !parsed.has_errors());
        let source_length = source_code.length_in_code_units();
        Self::create(
            vm,
            realm,
            compile_script(parsed, source_length),
            source_code,
            filename,
            ForeignCellSlot::empty(),
            ExecutableBacking::Source,
        )
    }

    /// The Script Record of a script compiled, on any thread, from the code of `source_code`, whose filename the
    /// script's code reports. Module loading resolves the specifiers of its dynamic imports against `filename`.
    /// `executable_backing` says where it was compiled.
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        compiled: CompiledScript,
        source_code: Rc<SourceCode>,
        filename: &str,
        host_defined: ForeignCellSlot,
        executable_backing: ExecutableBacking,
    ) -> Gc<Script> {
        let CompiledScript {
            executable,
            declarations,
        } = compiled;
        let is_strict = executable.is_strict;
        let executable = Executable::create_with_source_code(vm, executable, Some(&source_code));

        // The functions stay rooted until the script that holds them is allocated.
        let rooted_shared_data = MarkedVec::with_capacity(vm, declarations.functions_to_initialize.len());
        let mut function_names = Vec::with_capacity(declarations.functions_to_initialize.len());
        for mut function in declarations.functions_to_initialize {
            rooted_shared_data.push(SharedFunctionInstanceData::create_from_pending_shared_function_data(
                vm,
                &mut function.shared_function_data,
                is_strict,
                Some(&source_code),
            ));
            function_names.push(fly_string_of(&function.name));
        }
        let lexical_bindings = declarations
            .lexical_bindings
            .iter()
            .map(|binding| LexicalBinding {
                name: fly_string_of(&binding.name),
                is_constant: binding.is_constant,
            })
            .collect();

        Self::allocate(
            vm,
            realm,
            ScriptParts {
                executable,
                executable_backing,
                function_names,
                lexical_names: fly_strings_of(&declarations.lexical_names),
                var_names: fly_strings_of(&declarations.var_names),
                var_scoped_names: fly_strings_of(&declarations.var_scoped_names),
                annex_b_candidate_names: fly_strings_of(&declarations.annex_b_candidate_names),
                lexical_bindings,
            },
            &rooted_shared_data,
            source_code,
            filename,
            host_defined,
        )
    }

    /// Script::create_from_bytecode_cache(): the Script Record of a classic script from a bytecode cache blob of its
    /// code, which the blob must match. Its executables run in place in the blob, and its functions compile from the
    /// blob when they are first called. Fails with a single error if the blob does not match the source code or turns
    /// out to be malformed.
    pub fn create_from_bytecode_cache(
        vm: &Vm,
        realm: Gc<Realm>,
        bytecode_cache: &DecodedBytecodeCache,
        source_code: Rc<SourceCode>,
        filename: &str,
        host_defined: ForeignCellSlot,
    ) -> Result<Gc<Script>, Vec<ParserError>> {
        let blob = bytecode_cache
            .validated_blob(source_code.length_in_code_units())
            .ok_or_else(failed_to_materialize_bytecode_cache)?;
        let DecodedDeclarationMetadata::Script {
            metadata,
            declaration_functions,
        } = blob.declaration_metadata()
        else {
            return Err(failed_to_materialize_bytecode_cache());
        };
        let program = blob.program();
        if program.is_async_module() || declaration_functions.len() != metadata.function_names.len() {
            return Err(failed_to_materialize_bytecode_cache());
        }

        // The functions stay rooted until the script that holds them is allocated.
        let rooted_shared_data = MarkedVec::with_capacity(vm, declaration_functions.len());
        for function in declaration_functions {
            rooted_shared_data.push(SharedFunctionInstanceData::create_from_bytecode_cache(
                vm,
                function,
                blob.is_strict_mode(),
                &source_code,
            ));
        }
        let executable = create_executable_and_its_functions(vm, program.executable(), &source_code)
            .ok_or_else(failed_to_materialize_bytecode_cache)?;
        let lexical_bindings = metadata
            .lexical_bindings
            .iter()
            .map(|binding| LexicalBinding {
                name: fly_string_of(&binding.name),
                is_constant: binding.is_constant,
            })
            .collect();

        Ok(Self::allocate(
            vm,
            realm,
            ScriptParts {
                executable,
                executable_backing: ExecutableBacking::MappedBytecodeCache,
                function_names: fly_strings_of(&metadata.function_names),
                lexical_names: fly_strings_of(&metadata.lexical_names),
                var_names: fly_strings_of(&metadata.var_names),
                var_scoped_names: fly_strings_of(&metadata.var_scoped_names),
                annex_b_candidate_names: fly_strings_of(&metadata.annex_b_candidate_names),
                lexical_bindings,
            },
            &rooted_shared_data,
            source_code,
            filename,
            host_defined,
        ))
    }

    fn allocate(
        vm: &Vm,
        realm: Gc<Realm>,
        parts: ScriptParts,
        rooted_shared_data: &MarkedVec<'_, Gc<SharedFunctionInstanceData>>,
        source_code: Rc<SourceCode>,
        filename: &str,
        host_defined: ForeignCellSlot,
    ) -> Gc<Script> {
        let functions_to_initialize: Vec<FunctionToInitialize> = rooted_shared_data
            .to_vec()
            .into_iter()
            .zip(parts.function_names)
            .map(|(shared_data, name)| FunctionToInitialize { shared_data, name })
            .collect();
        let declared_function_names = functions_to_initialize
            .iter()
            .map(|function| function.name.clone())
            .collect();

        let script = vm.heap().allocate(Script {
            header: CellHeader::for_class(Self::CLASS),
            realm,
            loaded_modules: GcRefCell::new(Vec::new()),
            host_defined,
            executable: Cell::new(parts.executable),
            executable_backing: Cell::new(parts.executable_backing),
            source_code,
            lexical_names: parts.lexical_names,
            var_names: parts.var_names,
            functions_to_initialize,
            declared_function_names,
            var_scoped_names: parts.var_scoped_names,
            annex_b_candidate_names: parts.annex_b_candidate_names,
            lexical_bindings: parts.lexical_bindings,
            // NB: A script is never marked strict here, so Annex B function hoisting always runs; the frontend only
            //     collects candidates for sloppy scripts.
            is_strict_mode: false,
            filename: filename.to_string(),
        });
        script.verify_executable_backing_invariants(vm);
        script
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub fn loaded_modules(&self) -> &GcRefCell<Vec<LoadedModuleRequest>> {
        &self.loaded_modules
    }

    pub fn host_defined(&self) -> Option<NonNull<c_void>> {
        self.host_defined.get()
    }

    pub fn filename(&self) -> &str {
        &self.filename
    }

    pub fn cached_executable(&self) -> Gc<Executable> {
        self.executable.get()
    }

    pub fn executable_backing(&self) -> ExecutableBacking {
        self.executable_backing.get()
    }

    pub fn can_generate_bytecode_cache(&self) -> bool {
        self.executable_backing.get().can_generate_bytecode_cache()
    }

    pub fn can_install_generated_bytecode_cache(&self) -> bool {
        self.executable_backing.get().can_install_generated_bytecode_cache()
    }

    /// Marks the script as one whose bytecode cache is being generated, until the cache is installed or the generation
    /// finishes without installing it.
    ///
    /// # Panics
    /// Panics unless the script can generate a bytecode cache.
    pub fn begin_bytecode_cache_generation(&self, vm: &Vm) {
        self.executable_backing
            .set(self.executable_backing.get().with_bytecode_cache_generation_begun());
        self.verify_executable_backing_invariants(vm);
    }

    /// # Panics
    /// Panics unless a bytecode cache is being generated for the script.
    pub fn finish_bytecode_cache_generation_without_install(&self, vm: &Vm) {
        self.executable_backing.set(
            self.executable_backing
                .get()
                .with_bytecode_cache_generation_finished_without_install(),
        );
        self.verify_executable_backing_invariants(vm);
    }

    /// Script::try_install_bytecode_cache(): from now on, the script and its functions run from the bytecode cache
    /// blob of its code, if the blob matches the source code and every function the script created so far. Functions
    /// that already ran get executables from the blob, which take over their inline caches; the others compile from
    /// the blob on their first call. Returns false and changes nothing otherwise, and for a script that already runs
    /// from a blob.
    pub fn try_install_bytecode_cache(
        &self,
        vm: &Vm,
        bytecode_cache: &DecodedBytecodeCache,
        source_code: &Rc<SourceCode>,
    ) -> bool {
        if self.executable_backing.get().is_mapped_bytecode_cache() {
            return false;
        }
        let Some(blob) = bytecode_cache.validated_blob(source_code.length_in_code_units()) else {
            return false;
        };
        let DecodedDeclarationMetadata::Script {
            metadata,
            declaration_functions,
        } = blob.declaration_metadata()
        else {
            return false;
        };
        let program = blob.program();
        if program.is_async_module() || declaration_functions.len() != metadata.function_names.len() {
            return false;
        }

        let existing_functions = self.functions_created_so_far(vm);
        let mut install = BytecodeCacheInstall::new(vm, source_code, &existing_functions);
        for function in declaration_functions {
            if install.prepare_function(function, blob.is_strict_mode()).is_none() {
                return false;
            }
        }
        let Some(executable) = install.prepare_executable(program.executable(), Some(self.executable.get())) else {
            return false;
        };
        if !install.commit() {
            return false;
        }

        self.executable.set(executable);
        for function in existing_functions.to_vec() {
            function.clear_non_bytecode_cache_compile_inputs();
        }
        self.executable_backing.set(ExecutableBacking::MappedBytecodeCache);
        self.verify_executable_backing_invariants(vm);
        true
    }

    /// Script::install_generated_bytecode_cache(): installs the bytecode cache that was generated for the script.
    ///
    /// # Panics
    /// Panics unless a bytecode cache is being generated for the script, and if the blob does not match it.
    pub fn install_generated_bytecode_cache(
        &self,
        vm: &Vm,
        bytecode_cache: &DecodedBytecodeCache,
        source_code: &Rc<SourceCode>,
    ) {
        assert!(
            self.can_install_generated_bytecode_cache(),
            "a bytecode cache is being generated for the script"
        );
        assert!(
            self.try_install_bytecode_cache(vm, bytecode_cache, source_code),
            "the bytecode cache generated for a script matches it"
        );
    }

    fn functions_created_so_far<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Gc<SharedFunctionInstanceData>> {
        functions_created_by(
            vm,
            self.functions_to_initialize.iter().map(|function| function.shared_data),
            Some(self.executable.get()),
        )
    }

    fn verify_executable_backing_invariants(&self, vm: &Vm) {
        if self.executable_backing.get().is_mapped_bytecode_cache() {
            assert!(
                have_only_bytecode_cache_compile_inputs(&self.functions_created_so_far(vm)),
                "the functions of a script with a bytecode cache compile from the cache"
            );
        }
    }

    pub fn functions_to_initialize(&self) -> &[FunctionToInitialize] {
        &self.functions_to_initialize
    }

    // 16.1.7 GlobalDeclarationInstantiation ( script, env ), https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
    pub fn global_declaration_instantiation(
        &self,
        vm: &Vm,
        global_environment: Gc<GlobalEnvironment>,
    ) -> ThrowCompletionOr<()> {
        let realm = vm.current_realm();

        // 1. Let lexNames be the LexicallyDeclaredNames of script.
        // 2. Let varNames be the VarDeclaredNames of script.
        // 3. For each element name of lexNames, do
        for name in self.lexical_names.clone() {
            // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
            if global_environment.has_lexical_declaration(&name) {
                return vm.throw_completion(
                    ErrorKind::SyntaxError,
                    ErrorType::TopLevelVariableAlreadyDeclared,
                    &[&name],
                );
            }

            // b. Let hasRestrictedGlobal be ? HasRestrictedGlobalProperty(env, name).
            let has_restricted_global = global_environment.has_restricted_global_property(vm, &name)?;

            // d. If hasRestrictedGlobal is true, throw a SyntaxError exception.
            if has_restricted_global {
                return vm.throw_completion(ErrorKind::SyntaxError, ErrorType::RestrictedGlobalProperty, &[&name]);
            }
        }

        // 4. For each element name of varNames, do
        for name in self.var_names.clone() {
            // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
            if global_environment.has_lexical_declaration(&name) {
                return vm.throw_completion(
                    ErrorKind::SyntaxError,
                    ErrorType::TopLevelVariableAlreadyDeclared,
                    &[&name],
                );
            }
        }

        let function_names: Vec<Utf16FlyString> = self
            .functions_to_initialize
            .iter()
            .map(|function| function.name.clone())
            .collect();

        // 5. Let varDeclarations be the VarScopedDeclarations of script.
        // 6. Let functionsToInitialize be a new empty List.
        // 7. Let declaredFunctionNames be a new empty List.
        // 8. For each element d of varDeclarations, in reverse List order, do
        for function_name in &function_names {
            // 1. Let fnDefinable be ? env.CanDeclareGlobalFunction(fn).
            let function_definable = global_environment.can_declare_global_function(vm, function_name)?;

            // 2. If fnDefinable is false, throw a TypeError exception.
            if !function_definable {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::CannotDeclareGlobalFunction,
                    &[function_name],
                );
            }
        }

        // 9. Let declaredVarNames be a new empty List.
        let mut declared_var_names = Utf16FlyStringHashTable::default();

        // 10. For each element d of varDeclarations, do
        for name in self.var_scoped_names.clone() {
            // 1. If vn is not an element of declaredFunctionNames, then
            if self.declared_function_names.contains(&name) {
                continue;
            }

            // a. Let vnDefinable be ? env.CanDeclareGlobalVar(vn).
            let var_definable = global_environment.can_declare_global_var(vm, &name)?;

            // b. If vnDefinable is false, throw a TypeError exception.
            if !var_definable {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotDeclareGlobalVariable, &[&name]);
            }

            // c. If vn is not an element of declaredVarNames, then
            // i. Append vn to declaredVarNames.
            declared_var_names.set(name);
        }

        // 12. NOTE: Annex B.3.2.2 adds additional steps at this point.
        // 12. Let strict be IsStrict of script.
        // 13. If strict is false, then
        if !self.is_strict_mode {
            // a. Let declaredFunctionOrVarNames be the list-concatenation of declaredFunctionNames and declaredVarNames.
            // b. For each FunctionDeclaration f that is directly contained in the StatementList of a Block, CaseClause, or DefaultClause Contained within script, do
            for function_name in self.annex_b_candidate_names.clone() {
                // i. Let F be StringValue of the BindingIdentifier of f.

                // 1. If env.HasLexicalDeclaration(F) is false, then
                if global_environment.has_lexical_declaration(&function_name) {
                    continue;
                }

                // a. Let fnDefinable be ? env.CanDeclareGlobalVar(F).
                let function_definable = global_environment.can_declare_global_function(vm, &function_name)?;
                // b. If fnDefinable is true, then
                if !function_definable {
                    continue;
                }

                // ii. If declaredFunctionOrVarNames does not contain F, then
                if !self.declared_function_names.contains(&function_name)
                    && !declared_var_names.contains(&function_name)
                {
                    // i. Perform ? env.CreateGlobalVarBinding(F, false).
                    global_environment.create_global_var_binding(vm, &function_name, false)?;
                }
            }
        }

        // 14. Let privateEnv be null.
        let private_environment: Option<Gc<PrivateEnvironment>> = None;

        // 15. For each element d of lexDeclarations, do
        for binding in self.lexical_bindings.clone() {
            // i. If IsConstantDeclaration of d is true, then
            if binding.is_constant {
                // 1. Perform ? env.CreateImmutableBinding(dn, true).
                global_environment.create_immutable_binding(vm, &binding.name, true)?;
            }
            // ii. Else,
            else {
                // 1. Perform ? env.CreateMutableBinding(dn, false).
                global_environment.create_mutable_binding(vm, &binding.name, false)?;
            }
        }

        // 16. For each Parse Node f of functionsToInitialize, do
        for (function_index, function_name) in function_names.iter().enumerate() {
            // a. Let fn be the sole element of the BoundNames of f.
            // b. Let fo be InstantiateFunctionObject of f with arguments env and privateEnv.
            let function = EcmascriptFunctionObject::create_from_function_data(
                vm,
                realm.expect("GlobalDeclarationInstantiation runs in an execution context with a realm"),
                self.functions_to_initialize[function_index].shared_data,
                Some(global_environment.upcast()),
                private_environment,
            );

            // c. Perform ? env.CreateGlobalFunctionBinding(fn, fo, false).
            // NB: The function's name is the sole element of the BoundNames of f.
            global_environment.create_global_function_binding(
                vm,
                function_name,
                Value::from_object(function),
                false,
            )?;
        }

        // 17. For each String vn of declaredVarNames, do
        for var_name in declared_var_names.iter() {
            // a. Perform ? env.CreateGlobalVarBinding(vn, false).
            global_environment.create_global_var_binding(vm, var_name, false)?;
        }

        // 18. Return unused.
        Ok(())
    }
}
