/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parse and compile pipeline shared by every runtime that embeds the frontend.

#![cfg_attr(
    not(feature = "cpp-runtime"),
    allow(
        dead_code,
        reason = "only the C++ runtime drives this pipeline until the native API covers it"
    )
)]

use crate::ast;
use crate::ast::StatementKind;
use crate::bytecode;
use crate::bytecode::generator::PendingSharedFunctionData;
use crate::parser::ParseError;
use crate::parser::ProgramType;
use std::collections::HashSet;
use std::ffi::c_void;

// Compile-time assertion: `ParsedProgram` travels between the parse worker
// thread and the main thread, so it must be `Send`. After the StringId and
// ScopeId arena migrations the AST itself contains no `Rc`/`Cell`/`RefCell`
// values, so this is naturally satisfied without `unsafe impl Send`.
const _: fn() = || {
    fn assert_send<T: Send>() {}
    assert_send::<ParsedProgram>();
};

// =============================================================================
// ParsedProgram: GC-free parse result for off-thread parsing
// =============================================================================

/// A parsed program (script or module) that can be compiled later.
/// Contains no GC references, so it can safely be transferred between threads.
pub struct ParsedProgram {
    pub(crate) program: ast::Statement,
    pub(crate) function_table: ast::FunctionTable,
    pub(crate) arena: std::sync::Arc<ast::AstArena>,
    pub(crate) scope_ref: ast::ScopeId,
    pub(crate) program_type: ast::ProgramType,
    pub(crate) is_strict_mode: bool,
    pub(crate) has_top_level_await: bool,
    pub(crate) errors: Vec<ParseError>,
    pub(crate) ast_dump: Option<Vec<u8>>,
}

pub struct CompiledProgram {
    pub(crate) parsed: ParsedProgram,
    pub(crate) bytecode: CompiledProgramBytecode,
    pub(crate) declaration_functions: Vec<PendingSharedFunctionData>,
    pub(crate) source_len: usize,
}

pub(crate) enum CompiledProgramBytecode {
    Program(CompiledBytecode),
    AsyncModule(CompiledBytecode),
}

pub(crate) struct CompiledBytecode {
    pub(crate) generator: bytecode::generator::Generator,
    pub(crate) assembled: bytecode::generator::AssembledBytecode,
}

// SAFETY: `CompiledProgram` owns codegen state that uses `Rc`/`RefCell` and
// raw VM pointers; it is created on the parse-worker thread and consumed (or
// freed) on the main thread, never accessed concurrently.
unsafe impl Send for CompiledProgram {}

/// Convert scope local variables to generator LocalVariable format.
fn convert_local_variables(scope: &ast::ScopeData) -> Vec<bytecode::generator::LocalVariable> {
    scope
        .local_variables
        .iter()
        .map(|lv| bytecode::generator::LocalVariable {
            name: ak::Utf16FlyString::from_utf16(&lv.name),
            is_lexically_declared: lv.kind == ast::LocalVarKind::LetOrConst,
            is_initialized_during_declaration_instantiation: false,
            is_mutable: lv.is_mutable,
            scope_range: lv.scope_range,
        })
        .collect()
}

/// Create a Generator configured for program-level compilation.
pub(crate) fn new_program_generator(
    strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
) -> bytecode::generator::Generator {
    let mut generator = bytecode::generator::Generator::new();
    generator.strict = strict;
    generator.must_propagate_completion = true;
    generator.vm_ptr = vm_ptr;
    generator.source_code_ptr = source_code_ptr;
    generator.source_len = source_len;
    generator
}

/// Shared codegen pipeline: local variable setup → bytecode generation → assembly.
///
/// This deliberately stops before `create_executable()`, because executable materialization creates GC-managed objects
/// and resolves VM-specific constants. Keeping that work separate lets WebContent perform the expensive AST-to-bytecode
/// pass on a worker thread while preserving all main-thread ownership rules for VM and heap data.
pub(crate) fn compile_program_body_to_bytecode(
    generator: &mut bytecode::generator::Generator,
    program: &ast::Statement,
    scope_id: ast::ScopeId,
) -> bytecode::generator::AssembledBytecode {
    let arena_clone = generator.arena.clone();
    generator.local_variables = convert_local_variables(&arena_clone.scopes[scope_id]);
    if let StatementKind::Program(data) = &program.inner
        && data.program_type == ProgramType::Module
    {
        generator.enclosing_environment_scope =
            Some(module_environment_scope(&arena_clone.scopes[scope_id], &arena_clone));
    }

    let entry_block = generator.make_block();
    generator.switch_to_basic_block(entry_block);
    generator.emit(bytecode::instruction::Instruction::Enter {});
    generator.capture_saved_lexical_environment();

    let result = bytecode::codegen::generate_statement(program, generator, None);

    if !generator.is_current_block_terminated()
        && let Some(value) = result
    {
        generator.emit(bytecode::instruction::Instruction::End { value: value.operand() });
    }
    // If result is None, the assembler will add End(undefined) as a fallthrough for unterminated blocks, matching C++.

    generator.assemble()
}

#[derive(Clone, Copy, PartialEq)]
pub(crate) enum FunctionPrecompileMode {
    EagerOnly,
    All,
}

fn precompile_functions(generator: &mut bytecode::generator::Generator, mode: FunctionPrecompileMode) {
    for pending in &mut generator.shared_function_data {
        if pending.precompiled_function.is_some() {
            continue;
        }
        if matches!(mode, FunctionPrecompileMode::EagerOnly) && !pending.should_eager_compile {
            continue;
        }

        let function_data = pending
            .function_data
            .take()
            .expect("pending eager function data was already materialized");
        let subtable = pending
            .subtable
            .take()
            .expect("pending eager function subtable was already materialized");
        let arena = pending.arena.clone().unwrap_or_else(|| generator.arena.clone());
        let payload = ast::FunctionPayload {
            data: *function_data,
            function_table: subtable,
            arena: arena.clone(),
            enclosing_environment_scope: pending.enclosing_environment_scope.clone(),
        };
        let (function_data, precompiled) = compile_function_payload_to_bytecode(
            payload,
            generator.source_len,
            generator.builtin_abstract_operations_enabled,
            arena,
            mode,
        );

        pending.function_data = Some(function_data);
        // The precompiled executable owns any nested lazy function payloads. Keep
        // an empty payload here only until materialization creates the SFD; it is
        // immediately cleared after the precompiled executable is attached.
        pending.subtable = Some(ast::FunctionTable::new());
        pending.precompiled_function = Some(precompiled);
    }
}

fn precompile_declaration_functions(
    program_type: ast::ProgramType,
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> Vec<PendingSharedFunctionData> {
    if mode != FunctionPrecompileMode::All {
        return Vec::new();
    }

    match program_type {
        ast::ProgramType::Script => precompile_script_declaration_functions(scope_id, generator, mode),
        ast::ProgramType::Module => precompile_module_declaration_functions(scope_id, generator, mode),
    }
}

fn precompile_script_declaration_functions(
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> Vec<PendingSharedFunctionData> {
    let arena = generator.arena.clone();
    let scope = &arena.scopes[scope_id];
    let mut last_position: std::collections::HashMap<ast::StringId, usize> = std::collections::HashMap::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let Some(function) = child.inner.function_declaration_for_labelled_item()
            && let Some(name) = function.name
        {
            last_position.insert(arena.identifiers[name].name, index);
        }
    }

    let mut declaration_functions = Vec::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let Some(function) = child.inner.function_declaration_for_labelled_item()
            && let Some(name) = function.name
            && last_position.get(&arena.identifiers[name].name).copied() == Some(index)
        {
            declaration_functions.push(precompile_declaration_function(
                function.function_id,
                None,
                None,
                generator,
                mode,
            ));
        }
    }

    declaration_functions
}

fn precompile_module_declaration_functions(
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> Vec<PendingSharedFunctionData> {
    use ast::StatementKind;

    let arena = generator.arena.clone();
    let scope = &arena.scopes[scope_id];
    let default_name: ast::Utf16String = utf16!("*default*").into();
    let module_environment_scope = module_environment_scope(scope, &arena);
    let mut declaration_functions = Vec::new();
    for child in &scope.children {
        let (declaration, is_exported) = match &child.inner {
            StatementKind::Export(export_data) => {
                if let Some(ref statement) = export_data.statement {
                    (&statement.inner, true)
                } else {
                    continue;
                }
            }
            other => (other, false),
        };

        if let StatementKind::FunctionDeclaration(function) = declaration {
            let is_default = is_exported
                && function
                    .name
                    .is_some_and(|name| arena.name_slice(name) == default_name.as_slice());
            let name_override = if is_default {
                Some(utf16!("default").into())
            } else {
                None
            };
            declaration_functions.push(precompile_declaration_function(
                function.function_id,
                name_override,
                Some(module_environment_scope.clone()),
                generator,
                mode,
            ));
        }
    }

    declaration_functions
}

fn precompile_declaration_function(
    function_id: ast::FunctionId,
    name_override: Option<ast::Utf16String>,
    enclosing_environment_scope: Option<std::sync::Arc<bytecode::generator::EnclosingEnvironmentScope>>,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> PendingSharedFunctionData {
    let function_data = generator.function_table.take(function_id);
    let arena = generator.arena.clone();
    let subtable = generator
        .function_table
        .extract_reachable(&function_data, &arena.scopes);
    let payload = ast::FunctionPayload {
        data: *function_data,
        function_table: subtable,
        arena: arena.clone(),
        enclosing_environment_scope: enclosing_environment_scope.clone(),
    };
    let (function_data, precompiled_function) = compile_function_payload_to_bytecode(
        payload,
        generator.source_len,
        generator.builtin_abstract_operations_enabled,
        arena.clone(),
        mode,
    );

    PendingSharedFunctionData {
        function_data: Some(function_data),
        subtable: Some(ast::FunctionTable::new()),
        arena: Some(arena),
        name_override,
        class_field_initializer_name: None,
        should_eager_compile: false,
        precompiled_function: Some(precompiled_function),
        enclosing_environment_scope,
    }
}

pub(crate) fn compile_parsed_program_off_thread_impl(
    mut parsed: ParsedProgram,
    source_len: usize,
    function_precompile_mode: FunctionPrecompileMode,
) -> CompiledProgram {
    let arena_arc = parsed.arena.clone();
    let (bytecode, declaration_functions) = if parsed.has_top_level_await {
        let mut generator = new_module_async_generator(source_len, std::mem::take(&mut parsed.function_table));
        generator.arena = arena_arc;
        generator.eager_compile_direct_iifes = true;
        let assembled = compile_module_as_async_to_bytecode(&parsed.program, parsed.scope_ref, &mut generator);
        let declaration_functions = precompile_declaration_functions(
            parsed.program_type,
            parsed.scope_ref,
            &mut generator,
            function_precompile_mode,
        );
        precompile_functions(&mut generator, function_precompile_mode);
        (
            CompiledProgramBytecode::AsyncModule(CompiledBytecode { generator, assembled }),
            declaration_functions,
        )
    } else {
        let mut generator = new_program_generator(
            parsed.is_strict_mode,
            std::ptr::null_mut(),
            std::ptr::null(),
            source_len,
        );
        generator.arena = arena_arc;
        generator.eager_compile_direct_iifes = true;
        generator.function_table = std::mem::take(&mut parsed.function_table);
        let assembled = compile_program_body_to_bytecode(&mut generator, &parsed.program, parsed.scope_ref);
        let declaration_functions = precompile_declaration_functions(
            parsed.program_type,
            parsed.scope_ref,
            &mut generator,
            function_precompile_mode,
        );
        precompile_functions(&mut generator, function_precompile_mode);
        (
            CompiledProgramBytecode::Program(CompiledBytecode { generator, assembled }),
            declaration_functions,
        )
    };

    CompiledProgram {
        parsed,
        bytecode,
        declaration_functions,
        source_len,
    }
}

/// Returns the name of the binding that holds the value of `export default <expression>`, if the module has one.
pub(crate) fn module_default_export_binding_name(scope: &ast::ScopeData) -> Option<&ast::Utf16String> {
    use ast::StatementKind;

    for child in &scope.children {
        let StatementKind::Export(ref export_data) = child.inner else {
            continue;
        };
        if !export_data.is_default_export || export_data.entries.len() != 1 {
            continue;
        }

        // If the default export is not a declaration (function/class/etc.),
        // its binding name is the local_or_import_name.
        let is_declaration = export_data.statement.as_ref().is_some_and(|s| {
            matches!(
                s.inner,
                StatementKind::FunctionDeclaration(_) | StatementKind::ClassDeclaration(_)
            )
        });
        if is_declaration {
            continue;
        }

        let entry = &export_data.entries[0];
        let is_specific_import_export = scope.children.iter().any(|child| {
            let StatementKind::Import(ref import_data) = child.inner else {
                return false;
            };
            import_data.entries.iter().any(|import_entry| {
                entry.local_or_import_name.as_ref() == Some(&import_entry.local_name)
                    && import_entry.import_name.is_some()
            })
        });
        if !is_specific_import_export {
            return entry.local_or_import_name.as_ref();
        }
    }
    None
}

/// Returns the names of a module's own bindings, in the order that `SourceTextModule::initialize_environment()`
/// creates them in the module environment.
fn module_environment_binding_names(scope: &ast::ScopeData, arena: &ast::AstArena) -> Vec<ak::Utf16FlyString> {
    use ast::StatementKind;

    let mut names = Vec::new();
    let mut var_names = HashSet::new();
    for child in &scope.children {
        collect_module_var_names(&child.inner, arena, &mut |name| {
            if var_names.insert(name.to_vec()) {
                names.push(ak::Utf16FlyString::from_utf16(name));
            }
        });
    }

    for child in &scope.children {
        let declaration = match &child.inner {
            StatementKind::Export(export_data) => match &export_data.statement {
                Some(statement) => &statement.inner,
                None => continue,
            },
            other => other,
        };

        match declaration {
            StatementKind::FunctionDeclaration(function) => {
                if let Some(name) = function.name {
                    names.push(ak::Utf16FlyString::from_utf16(arena.name_slice(name)));
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    names.push(ak::Utf16FlyString::from_utf16(arena.name_slice(name)));
                }
            }
            StatementKind::VariableDeclaration(vd) if vd.kind != ast::DeclarationKind::Var => {
                for declaration in &vd.declarations {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        names.push(ak::Utf16FlyString::from_utf16(name));
                    });
                }
            }
            StatementKind::UsingDeclaration(declarations) => {
                for declaration in declarations.iter() {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        names.push(ak::Utf16FlyString::from_utf16(name));
                    });
                }
            }
            _ => {}
        }
    }

    if let Some(name) = module_default_export_binding_name(scope) {
        names.push(ak::Utf16FlyString::from_utf16(name.as_slice()));
    }
    names
}

/// Returns the local names of a module's import entries, in the order of `SourceTextModule`'s import entries.
fn module_import_names(scope: &ast::ScopeData) -> Vec<ak::Utf16FlyString> {
    let mut names = Vec::new();
    for child in &scope.children {
        if let StatementKind::Import(ref import_data) = child.inner {
            for entry in &import_data.entries {
                names.push(ak::Utf16FlyString::from_utf16(entry.local_name.as_slice()));
            }
        }
    }
    names
}

pub(crate) fn module_environment_scope(
    scope: &ast::ScopeData,
    arena: &ast::AstArena,
) -> std::sync::Arc<bytecode::generator::EnclosingEnvironmentScope> {
    bytecode::generator::EnclosingEnvironmentScope::for_module_environment(
        module_environment_binding_names(scope, arena),
        module_import_names(scope),
    )
}

/// Recursively collect var declared names for module scope.
pub(crate) fn collect_module_var_names(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    f: &mut dyn FnMut(&[u16]),
) {
    match statement {
        ast::StatementKind::VariableDeclaration(vd) if vd.kind == ast::DeclarationKind::Var => {
            for declaration in &vd.declarations {
                for_each_bound_name(&declaration.target, arena, f);
            }
        }
        ast::StatementKind::Export(export_data) => {
            if let Some(ref stmt) = export_data.statement {
                collect_module_var_names(&stmt.inner, arena, f);
            }
        }
        _ => {
            for_each_child_statement(statement, arena, &mut |child| {
                collect_module_var_names(child, arena, f);
            });
        }
    }
}

/// Compile a module body as an async function (for TLA modules).
///
/// Emits async-function wrapping (initial Yield, final Yield) around the
/// module body statements.
pub(crate) fn new_module_async_generator(
    source_len: usize,
    function_table: ast::FunctionTable,
) -> bytecode::generator::Generator {
    let mut generator = bytecode::generator::Generator::new();
    generator.strict = true;
    generator.function_table = function_table;
    generator.source_len = source_len;
    generator.enclosing_function_kind = ast::FunctionKind::Async;
    generator
}

pub(crate) fn compile_module_as_async_to_bytecode(
    program: &ast::Statement,
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
) -> bytecode::generator::AssembledBytecode {
    use bytecode::instruction::Instruction;

    let arena_clone = generator.arena.clone();
    let scope = &arena_clone.scopes[scope_id];

    // Extract local variables from the program scope so the executable has the correct registers_and_locals_count.
    // Without this, locals are not saved across await suspension points, causing them to become undefined.
    generator.local_variables = convert_local_variables(scope);
    generator.enclosing_environment_scope = Some(module_environment_scope(scope, &arena_clone));

    let entry_block = generator.make_block();
    generator.switch_to_basic_block(entry_block);
    generator.emit(Instruction::Enter {});

    // Async function start: emit initial Yield before GetLexicalEnvironment.
    let start_block = generator.make_block();
    let undef = generator.add_constant_undefined();
    generator.emit(Instruction::Yield {
        continuation_label: Some(start_block),
        value: undef.operand(),
    });
    generator.switch_to_basic_block(start_block);
    generator.capture_saved_lexical_environment_with_coordinates(true);

    // Generate module body statements.
    let _result = bytecode::codegen::generate_statement(program, generator, None);

    // Async function end: emit final Yield (no continuation = done).
    if !generator.is_current_block_terminated() {
        let undef = generator.add_constant_undefined();
        generator.emit(Instruction::Yield {
            continuation_label: None,
            value: undef.operand(),
        });
    }

    // Terminate all unterminated blocks with Yield.
    generator.terminate_unterminated_blocks_with_yield();

    generator.assemble()
}

/// Recursively collect var-declared names from a statement and all nested
/// statements, excluding function/class bodies (which create new var scopes).
pub(crate) fn collect_var_names_recursive(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    push_name: &mut dyn FnMut(&[u16]),
) {
    match statement {
        ast::StatementKind::VariableDeclaration(vd) if vd.kind == ast::DeclarationKind::Var => {
            for declaration in &vd.declarations {
                for_each_bound_name(&declaration.target, arena, push_name);
            }
        }
        _ => {
            for_each_child_statement(statement, arena, &mut |child| {
                collect_var_names_recursive(child, arena, push_name);
            });
        }
    }
}

/// Visit each child statement of a statement, excluding function/class bodies
/// (which create new var scopes). This enables recursive var-declaration walking.
fn for_each_child_statement(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    f: &mut dyn FnMut(&ast::StatementKind),
) {
    use ast::StatementKind;

    match statement {
        StatementKind::Block(scope) => {
            for child in &arena.scopes[*scope].children {
                f(&child.inner);
            }
        }
        StatementKind::If(data) => {
            f(&data.consequent.inner);
            if let Some(alt) = &data.alternate {
                f(&alt.inner);
            }
        }
        StatementKind::While(data) => {
            f(&data.body.inner);
        }
        StatementKind::DoWhile(data) => {
            f(&data.body.inner);
        }
        StatementKind::With(data) => {
            f(&data.body.inner);
        }
        StatementKind::For(data) => {
            if let Some(ast::ForInit::Declaration(decl)) = &data.init {
                f(&decl.inner);
            }
            f(&data.body.inner);
        }
        StatementKind::ForInOf(data) => {
            if let ast::ForInOfLhs::Declaration(declaration) = &data.lhs {
                f(&declaration.inner);
            }
            f(&data.body.inner);
        }
        StatementKind::Switch(data) => {
            for case in &data.cases {
                for child in &arena.scopes[case.scope].children {
                    f(&child.inner);
                }
            }
        }
        StatementKind::Try(data) => {
            f(&data.block.inner);
            if let Some(ref handler) = data.handler {
                f(&handler.body.inner);
            }
            if let Some(ref finalizer) = data.finalizer {
                f(&finalizer.inner);
            }
        }
        StatementKind::Labelled(data) => {
            f(&data.item.inner);
        }
        // Don't recurse into function/class bodies (new var scopes)
        _ => {}
    }
}

pub(crate) fn for_each_bound_name(
    target: &ast::VariableDeclaratorTarget,
    arena: &ast::AstArena,
    f: &mut dyn FnMut(&[u16]),
) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(id) => f(arena.name_slice(*id)),
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            for_each_bound_name_in_pattern(pattern, arena, f);
        }
    }
}

fn for_each_bound_name_in_pattern(pattern: &ast::BindingPattern, arena: &ast::AstArena, f: &mut dyn FnMut(&[u16])) {
    for entry in &pattern.entries {
        match &entry.alias {
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name {
                    f(arena.name_slice(*id));
                }
            }
            Some(ast::BindingEntryAlias::Identifier(id)) => f(arena.name_slice(*id)),
            Some(ast::BindingEntryAlias::BindingPattern(inner)) => {
                for_each_bound_name_in_pattern(inner, arena, f);
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

pub(crate) fn compile_function_payload_to_bytecode(
    payload: ast::FunctionPayload,
    source_len: usize,
    builtin_abstract_operations_enabled: bool,
    arena: std::sync::Arc<ast::AstArena>,
    precompile_mode: FunctionPrecompileMode,
) -> (Box<ast::FunctionData>, Box<bytecode::generator::PrecompiledFunction>) {
    let function_data = Box::new(payload.data);
    let enclosing_environment_scope = payload.enclosing_environment_scope;

    let body_scope: Option<ast::ScopeId> = match &function_data.body.inner {
        StatementKind::FunctionBody { scope, .. } => Some(*scope),
        StatementKind::Block(scope) => Some(*scope),
        _ => None,
    };

    // Compute SFD metadata before codegen so the generator can optimize
    // direct `this` access when it does not need environment resolution.
    let sfd_metadata = compute_sfd_metadata(&function_data, &arena);

    let mut generator = bytecode::generator::Generator::new();
    generator.arena = arena;
    generator.strict = function_data.is_strict_mode;
    generator.contains_direct_call_to_eval_in_non_strict_mode =
        function_data.parsing_insights.contains_direct_call_to_eval && !function_data.is_strict_mode;
    generator.enclosing_environment_scope = enclosing_environment_scope;
    generator.this_value_needs_environment_resolution = sfd_metadata.this_value_needs_environment_resolution;
    generator.builtin_abstract_operations_enabled = builtin_abstract_operations_enabled;
    generator.function_table = payload.function_table;
    generator.source_len = source_len;
    generator.enclosing_function_kind = function_data.kind;
    generator.argument_variable_names = function_data
        .parameters
        .iter()
        .map(|parameter| match parameter.binding {
            ast::FunctionParameterBinding::Identifier(identifier)
                if generator.arena.identifiers[identifier].local_type == Some(ast::LocalType::Argument) =>
            {
                ak::Utf16FlyString::from_utf16(generator.arena.name_slice(identifier))
            }
            ast::FunctionParameterBinding::BindingPattern(_) => ak::Utf16FlyString::default(),
            _ => ak::Utf16FlyString::default(),
        })
        .collect();

    if let Some(scope_id) = body_scope {
        let arena_clone = generator.arena.clone();
        generator.local_variables = convert_local_variables(&arena_clone.scopes[scope_id]);
    }

    let entry_block = generator.make_block();
    generator.switch_to_basic_block(entry_block);
    generator.emit(bytecode::instruction::Instruction::Enter {});

    // https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
    // For async (non-generator) functions, emit the initial Yield BEFORE
    // GetLexicalEnvironment so that parameter evaluation errors are caught
    // by the async promise wrapper. This matches C++ ordering.
    if generator.is_in_async_function() && !generator.is_in_generator_function() {
        let start_block = generator.make_block();
        let undef = generator.add_constant_undefined();
        generator.emit(bytecode::instruction::Instruction::Yield {
            continuation_label: Some(start_block),
            value: undef.operand(),
        });
        generator.switch_to_basic_block(start_block);
    }

    generator.capture_saved_lexical_environment_with_coordinates(sfd_metadata.function_environment_needed);

    if let Some(scope_id) = body_scope {
        let arena_clone = generator.arena.clone();
        bytecode::codegen::emit_function_declaration_instantiation(
            &mut generator,
            &function_data,
            &arena_clone.scopes[scope_id],
            sfd_metadata.var_environment_bindings_count,
        );
    }

    // https://tc39.es/ecma262/#sec-generatorstart
    // For generator functions (including async generators), emit the initial Yield
    // AFTER FDI. Parameter evaluation happens synchronously before the generator starts.
    if generator.is_in_generator_function() {
        let start_block = generator.make_block();
        let undef = generator.add_constant_undefined();
        generator.emit(bytecode::instruction::Instruction::Yield {
            continuation_label: Some(start_block),
            value: undef.operand(),
        });
        generator.switch_to_basic_block(start_block);
    }

    let result = bytecode::codegen::generate_statement(&function_data.body, &mut generator, None);

    if !generator.is_current_block_terminated() {
        if generator.is_in_generator_or_async_function() {
            // Generator/async functions end with Yield (no continuation = done).
            let undef = generator.add_constant_undefined();
            generator.emit(bytecode::instruction::Instruction::Yield {
                continuation_label: None,
                value: undef.operand(),
            });
        } else if let Some(value) = result {
            generator.emit(bytecode::instruction::Instruction::End { value: value.operand() });
        }
        // If result is None, the assembler will add End(undefined) as a
        // fallthrough for unterminated blocks, matching C++ compile().
    }

    // For generator/async functions, terminate all unterminated blocks with Yield.
    if generator.is_in_generator_or_async_function() {
        generator.terminate_unterminated_blocks_with_yield();
    }

    precompile_functions(&mut generator, precompile_mode);

    let assembled = generator.assemble();

    (
        function_data,
        Box::new(bytecode::generator::PrecompiledFunction {
            generator: Box::new(generator),
            assembled,
            metadata: sfd_metadata,
        }),
    )
}

// =============================================================================
// SFD metadata computation (ECMA-262 section 10.2.11)
// =============================================================================

/// Intermediate scope analysis data extracted from the function body scope.
struct BodyScopeInfo {
    uses_this: bool,
    contains_eval: bool,
    uses_this_from_env: bool,
    might_need_arguments: bool,
    has_function_named_arguments: bool,
    has_lexically_declared_arguments: bool,
    non_local_var_count: usize,
    non_local_var_count_for_parameter_expressions: usize,
    var_names: Vec<ast::Utf16String>,
    annexb_function_names: Vec<ast::Utf16String>,
    has_arguments_object_local: bool,
}

/// Compute FDI runtime metadata matching the C++ SharedFunctionInstanceData
/// constructor (ECMA-262 §10.2.11).
fn compute_sfd_metadata(
    function_data: &ast::FunctionData,
    arena: &ast::AstArena,
) -> bytecode::generator::FunctionSfdMetadata {
    let body_scope: Option<ast::ScopeId> = match &function_data.body.inner {
        ast::StatementKind::FunctionBody { scope, .. } => Some(*scope),
        _ => None,
    };

    let strict = function_data.is_strict_mode;
    let is_arrow = function_data.is_arrow_function;

    // Extract all scope analysis data in one borrow.
    let bsi = if let Some(scope_id) = body_scope {
        let sd = &arena.scopes[scope_id];
        let fsd = sd.function_scope_data.as_ref();
        BodyScopeInfo {
            uses_this: sd.uses_this || function_data.parsing_insights.uses_this,
            contains_eval: sd.contains_direct_call_to_eval,
            uses_this_from_env: sd.uses_this_from_environment
                || function_data.parsing_insights.uses_this_from_environment,
            might_need_arguments: function_data.parsing_insights.might_need_arguments_object,
            has_function_named_arguments: fsd.is_some_and(|f| f.has_function_named_arguments),
            has_lexically_declared_arguments: fsd.is_some_and(|f| f.has_lexically_declared_arguments),
            non_local_var_count: fsd.map_or(0, |f| f.non_local_var_count),
            non_local_var_count_for_parameter_expressions: fsd
                .map_or(0, |f| f.non_local_var_count_for_parameter_expressions),
            var_names: fsd.map(|f| &f.var_names).cloned().unwrap_or_default(),
            annexb_function_names: sd.annexb_function_names.clone(),
            has_arguments_object_local: sd
                .local_variables
                .iter()
                .any(|lv| lv.kind == ast::LocalVarKind::ArgumentsObject),
        }
    } else {
        BodyScopeInfo {
            uses_this: function_data.parsing_insights.uses_this,
            contains_eval: function_data.parsing_insights.contains_direct_call_to_eval,
            uses_this_from_env: function_data.parsing_insights.uses_this_from_environment,
            might_need_arguments: function_data.parsing_insights.might_need_arguments_object,
            has_function_named_arguments: false,
            has_lexically_declared_arguments: false,
            non_local_var_count: 0,
            non_local_var_count_for_parameter_expressions: 0,
            var_names: Vec::new(),
            annexb_function_names: Vec::new(),
            has_arguments_object_local: false,
        }
    };

    // §10.2.11 step 4: check for parameter expressions.
    let has_parameter_expressions = function_data.parameters.iter().any(|p| {
        p.default_value.is_some()
            || matches!(
                p.binding,
                ast::FunctionParameterBinding::BindingPattern(ref pat) if pat.contains_expression()
            )
    });

    // §10.2.11 steps 5-8: count non-local unique parameter names.
    let mut parameter_names: HashSet<ast::Utf16String> = HashSet::new();
    let mut parameters_in_environment: usize = 0;
    for parameter in &function_data.parameters {
        match &parameter.binding {
            ast::FunctionParameterBinding::Identifier(id) => {
                let ident = &arena.identifiers[*id];
                if parameter_names.insert(arena.strings[ident.name].clone()) && !ident.is_local() {
                    parameters_in_environment += 1;
                }
            }
            ast::FunctionParameterBinding::BindingPattern(pattern) => {
                for_each_binding_pattern_identifier(pattern, arena, &mut |ident| {
                    if parameter_names.insert(arena.strings[ident.name].clone()) && !ident.is_local() {
                        parameters_in_environment += 1;
                    }
                });
            }
        }
    }

    // §10.2.11 steps 15-18: determine if arguments object is needed.
    // We trust either the parser's conservative "might need arguments" flag
    // (set when we consume an `arguments` or `eval` Identifier as a free
    // reference) OR scope analysis having allocated an ArgumentsObject local
    // for `arguments`. The latter catches references created without going
    // through consume(), e.g. shorthand `{ arguments }` in an object literal.
    // Skip the local-driven path when a function named `arguments` shadows
    // it; in that case the local belongs to the function declaration, not
    // a real arguments-object reference.
    let arguments_object_referenced =
        bsi.might_need_arguments || (bsi.has_arguments_object_local && !bsi.has_function_named_arguments);
    let arguments_object_needed = arguments_object_referenced
        && !is_arrow
        && !parameter_names.contains(utf16!("arguments"))
        && body_scope.is_some()
        && (has_parameter_expressions || !bsi.has_function_named_arguments)
        && (has_parameter_expressions || !bsi.has_lexically_declared_arguments);

    // Arguments object needs an environment binding if it's not a local variable.
    let arguments_object_needs_binding = arguments_object_needed && !bsi.has_arguments_object_local;

    let mut function_environment_bindings_count: usize = 0;
    let mut var_environment_bindings_count: usize = 0;
    let mut lex_environment_bindings_count: usize = 0;

    // §10.2.11 step 19: route parameter bindings.
    let env_is_function_env = strict || !has_parameter_expressions;
    if env_is_function_env {
        function_environment_bindings_count += parameters_in_environment;
    }

    // §10.2.11 step 22: arguments binding.
    if arguments_object_needs_binding && env_is_function_env {
        function_environment_bindings_count += 1;
    }

    if let Some(body_scope) = body_scope {
        if !has_parameter_expressions {
            // §10.2.11 step 27: var env shares function env.
            function_environment_bindings_count += bsi.non_local_var_count;

            // Annex B: function names hoisted from blocks that aren't already vars.
            if !strict {
                for name in &bsi.annexb_function_names {
                    if !bsi.var_names.contains(name) {
                        function_environment_bindings_count += 1;
                    }
                }
            }

            // §10.2.11 step 30: lexical environment.
            let non_local_lex_count = count_non_local_lex_declarations(body_scope, arena);
            if strict {
                // Lex env == var env == function env.
                function_environment_bindings_count += non_local_lex_count;
            } else {
                let can_elide = !bsi.contains_eval && non_local_lex_count == 0;
                if !can_elide {
                    lex_environment_bindings_count += non_local_lex_count;
                }
            }
        } else {
            // §10.2.11 step 28: separate var environment.
            var_environment_bindings_count += bsi.non_local_var_count_for_parameter_expressions;

            if !strict {
                for name in &bsi.annexb_function_names {
                    if !bsi.var_names.contains(name) {
                        var_environment_bindings_count += 1;
                    }
                }
            }

            let non_local_lex_count = count_non_local_lex_declarations(body_scope, arena);
            if strict {
                // Lex env == var env.
                var_environment_bindings_count += non_local_lex_count;
            } else {
                let can_elide = !bsi.contains_eval && non_local_lex_count == 0;
                if !can_elide {
                    lex_environment_bindings_count += non_local_lex_count;
                }
            }
        }
    }

    let this_value_needs_environment_resolution = bsi.uses_this_from_env;
    let function_environment_needed = arguments_object_needs_binding
        || function_environment_bindings_count > 0
        || var_environment_bindings_count > 0
        || lex_environment_bindings_count > 0
        || (!is_arrow && bsi.uses_this_from_env)
        || bsi.contains_eval;

    bytecode::generator::FunctionSfdMetadata {
        uses_this: bsi.uses_this,
        this_value_needs_environment_resolution,
        function_environment_needed,
        function_environment_bindings_count,
        var_environment_bindings_count,
        might_need_arguments: bsi.might_need_arguments,
        contains_eval: bsi.contains_eval,
    }
}

/// Count non-local lexically-declared identifiers in a function body scope.
/// Returns the count (used for environment sizing in the function_environment_needed
/// computation).
fn count_non_local_lex_declarations(scope_id: ast::ScopeId, arena: &ast::AstArena) -> usize {
    let sd = &arena.scopes[scope_id];
    let mut count = 0;
    for child in &sd.children {
        match &child.inner {
            ast::StatementKind::VariableDeclaration(vd) => {
                use crate::parser::DeclarationKind;
                if vd.kind == DeclarationKind::Let || vd.kind == DeclarationKind::Const {
                    for declaration in &vd.declarations {
                        count_non_local_names_in_target(&declaration.target, &mut count, arena);
                    }
                }
            }
            ast::StatementKind::UsingDeclaration(declarations) => {
                for declaration in declarations.iter() {
                    count_non_local_names_in_target(&declaration.target, &mut count, arena);
                }
            }
            ast::StatementKind::ClassDeclaration(class_data) => {
                if let Some(name_ident) = class_data.name
                    && !arena.identifiers[name_ident].is_local()
                {
                    count += 1;
                }
            }
            _ => {}
        }
    }
    count
}

fn count_non_local_names_in_target(target: &ast::VariableDeclaratorTarget, count: &mut usize, arena: &ast::AstArena) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(id) => {
            if !arena.identifiers[*id].is_local() {
                *count += 1;
            }
        }
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            count_non_local_names_in_binding_pattern(pattern, count, arena);
        }
    }
}

fn count_non_local_names_in_binding_pattern(pattern: &ast::BindingPattern, count: &mut usize, arena: &ast::AstArena) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(id)) => {
                if !arena.identifiers[*id].is_local() {
                    *count += 1;
                }
            }
            Some(ast::BindingEntryAlias::BindingPattern(sub)) => {
                count_non_local_names_in_binding_pattern(sub, count, arena);
            }
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name
                    && !arena.identifiers[*id].is_local()
                {
                    *count += 1;
                }
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

fn for_each_binding_pattern_identifier(
    pattern: &ast::BindingPattern,
    arena: &ast::AstArena,
    callback: &mut dyn FnMut(&ast::Identifier),
) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(id)) => callback(&arena.identifiers[*id]),
            Some(ast::BindingEntryAlias::BindingPattern(sub)) => {
                for_each_binding_pattern_identifier(sub, arena, callback);
            }
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name {
                    callback(&arena.identifiers[*id]);
                }
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}
