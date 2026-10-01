/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashSet;
use std::rc::Rc;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{GcCell, define_cell};
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
use crate::runtime::object_environment::name_for_message;
use crate::runtime::private_environment::PrivateEnvironment;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::source_code::SourceCode;
use libjs_rust::ast::{ProgramType, Utf16String};
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
    realm: Gc<Realm>, // [[Realm]]
    executable: Gc<Executable>,
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
}

define_cell!(Script, Other);

fn fly_string_of(name: &Utf16String) -> Utf16FlyString {
    Utf16FlyString::from_utf16(&name.0)
}

fn fly_strings_of(names: &[Utf16String]) -> Vec<Utf16FlyString> {
    names.iter().map(fly_string_of).collect()
}

impl Script {
    // 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
    pub fn parse(vm: &Vm, source: &[u16], realm: Gc<Realm>) -> Result<Gc<Script>, Vec<ParserError>> {
        let parsed = parse(source, ProgramType::Script, 1);
        if parsed.has_errors() {
            return Err(ParserError::all_from_parsed_program(&parsed));
        }
        Ok(Self::compile_parsed_program(vm, parsed, source, realm))
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
        assert!(parsed.program_type() == ProgramType::Script && !parsed.has_errors());
        let source_length = source_code.length_in_code_units();
        Self::create(vm, realm, compile_script(parsed, source_length), source_code)
    }

    fn create(vm: &Vm, realm: Gc<Realm>, compiled: CompiledScript, source_code: Rc<SourceCode>) -> Gc<Script> {
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
        let functions_to_initialize: Vec<FunctionToInitialize> = rooted_shared_data
            .to_vec()
            .into_iter()
            .zip(function_names)
            .map(|(shared_data, name)| FunctionToInitialize { shared_data, name })
            .collect();
        let declared_function_names = functions_to_initialize
            .iter()
            .map(|function| function.name.clone())
            .collect();
        let lexical_bindings = declarations
            .lexical_bindings
            .iter()
            .map(|binding| LexicalBinding {
                name: fly_string_of(&binding.name),
                is_constant: binding.is_constant,
            })
            .collect();

        let script = vm.heap().allocate(Script {
            header: CellHeader::for_class(Self::CLASS),
            realm,
            executable,
            source_code,
            lexical_names: fly_strings_of(&declarations.lexical_names),
            var_names: fly_strings_of(&declarations.var_names),
            functions_to_initialize,
            declared_function_names,
            var_scoped_names: fly_strings_of(&declarations.var_scoped_names),
            annex_b_candidate_names: fly_strings_of(&declarations.annex_b_candidate_names),
            lexical_bindings,
            // NB: The C++ runtime never sets the strictness of a script it compiles, so Annex B function hoisting
            //     always runs; the frontend only collects candidates for sloppy scripts.
            is_strict_mode: false,
        });
        drop(rooted_shared_data);
        script
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub fn cached_executable(&self) -> Gc<Executable> {
        self.executable
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
                    &[&name_for_message(&name)],
                );
            }

            // b. Let hasRestrictedGlobal be ? HasRestrictedGlobalProperty(env, name).
            let has_restricted_global = global_environment.has_restricted_global_property(vm, &name)?;

            // d. If hasRestrictedGlobal is true, throw a SyntaxError exception.
            if has_restricted_global {
                return vm.throw_completion(
                    ErrorKind::SyntaxError,
                    ErrorType::RestrictedGlobalProperty,
                    &[&name_for_message(&name)],
                );
            }
        }

        // 4. For each element name of varNames, do
        for name in self.var_names.clone() {
            // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
            if global_environment.has_lexical_declaration(&name) {
                return vm.throw_completion(
                    ErrorKind::SyntaxError,
                    ErrorType::TopLevelVariableAlreadyDeclared,
                    &[&name_for_message(&name)],
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
                    &[&name_for_message(function_name)],
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
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::CannotDeclareGlobalVariable,
                    &[&name_for_message(&name)],
                );
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
            // NB: C++ binds function->name(), which is the name the declaration binds.
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
