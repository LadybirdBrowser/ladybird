/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parse and compile pipeline shared by every runtime that embeds the frontend.
//!
//! A runtime parses a program with [`parse()`] and compiles a script with
//! [`compile_script()`], which returns the script's bytecode as
//! [`ExecutableData`] along with the [`ScriptDeclarations`] that
//! GlobalDeclarationInstantiation needs.

#![cfg_attr(
    not(feature = "cpp-runtime"),
    allow(
        dead_code,
        reason = "only the C++ runtime compiles functions, modules and off-thread programs until the native API does"
    )
)]

use crate::ast;
use crate::ast::StatementKind;
use crate::ast_dump;
use crate::bytecode;
use crate::bytecode::executable::ExecutableData;
use crate::bytecode::generator::PendingSharedFunctionData;
use crate::parser::ParseError;
use crate::parser::Parser;
use crate::parser::ProgramType;
use crate::u32_from_usize;
use std::collections::HashSet;

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
    pub(crate) ast_dump: Option<String>,
}

impl ParsedProgram {
    pub fn has_errors(&self) -> bool {
        !self.errors.is_empty()
    }

    pub fn errors(&self) -> &[ParseError] {
        &self.errors
    }

    pub fn program_type(&self) -> ProgramType {
        self.program_type
    }

    pub fn is_strict_mode(&self) -> bool {
        self.is_strict_mode
    }

    pub fn has_top_level_await(&self) -> bool {
        self.has_top_level_await
    }

    /// The textual AST dump, generated on first use.
    pub fn ast_dump(&mut self) -> &str {
        self.ast_dump
            .get_or_insert_with(|| ast_dump::dump_program_to_string(&self.program, &self.function_table, &self.arena))
    }
}

/// Lex and parse a script or module, then run scope analysis on it.
///
/// Errors are reported through the returned program rather than by failing,
/// so check [`ParsedProgram::has_errors()`] before compiling it. Lines are
/// counted from `initial_line_number`, except that a module always starts at
/// line 1 or later.
pub fn parse(source: &[u16], program_type: ProgramType, initial_line_number: usize) -> ParsedProgram {
    let initial_line_number = if program_type == ProgramType::Module && initial_line_number == 0 {
        1
    } else {
        initial_line_number
    };
    let mut parser = Parser::new_with_line_offset(source, program_type, u32_from_usize(initial_line_number));

    let program = parser.parse_program(false);

    // Collect errors from both parser and scope collector.
    let mut errors = parser.take_errors();
    if errors.is_empty() {
        errors = parser.scope_collector.drain_errors();
    }

    if errors.is_empty() {
        parser.scope_collector.analyze(
            false,
            &mut parser.arena.identifiers,
            &parser.arena.strings,
            &mut parser.arena.scopes,
        );
    }

    let (scope_ref, is_strict, has_tla) = if errors.is_empty()
        && let StatementKind::Program(ref data) = program.inner
    {
        (data.scope, data.is_strict_mode, data.has_top_level_await)
    } else {
        (parser.arena.scopes.insert(ast::ScopeData::default()), false, false)
    };

    ParsedProgram {
        program,
        function_table: std::mem::take(&mut parser.function_table),
        arena: std::sync::Arc::new(std::mem::take(&mut parser.arena)),
        scope_ref,
        program_type,
        is_strict_mode: is_strict,
        has_top_level_await: has_tla,
        errors,
        ast_dump: None,
    }
}

pub struct CompiledProgram {
    /// Its function table holds the functions that codegen left for declaration instantiation.
    pub(crate) parsed: ParsedProgram,
    pub(crate) bytecode: CompiledProgramBytecode,
    pub(crate) declaration_functions: Vec<PendingSharedFunctionData>,
    pub(crate) source_len: usize,
}

pub(crate) enum CompiledProgramBytecode {
    Program(ExecutableData),
    AsyncModule(ExecutableData),
}

// SAFETY: `CompiledProgram` owns raw handles of compiled regular expressions,
// which Rust never dereferences; it is created on the parse-worker thread and
// consumed (or freed) on the main thread, never accessed concurrently.
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
pub(crate) fn new_program_generator(strict: bool, source_len: usize) -> bytecode::generator::Generator {
    let mut generator = bytecode::generator::Generator::new();
    generator.strict = strict;
    generator.must_propagate_completion = true;
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

/// A compiled script and what GlobalDeclarationInstantiation needs to run it.
pub struct CompiledScript {
    pub executable: ExecutableData,
    pub declarations: ScriptDeclarations,
}

/// Compile a script that parsed without errors.
///
/// `source_len` is the length of the source code in UTF-16 code units, which
/// the source ranges of the script's functions must fit in.
///
/// # Panics
/// Panics if `parsed` is a module or has errors.
pub fn compile_script(mut parsed: ParsedProgram, source_len: usize) -> CompiledScript {
    assert!(
        parsed.program_type == ProgramType::Script,
        "compile_script() needs a script, not a module"
    );
    assert!(
        !parsed.has_errors(),
        "compile_script() needs a script without parse errors"
    );

    let mut generator = new_program_generator(parsed.is_strict_mode, source_len);
    generator.function_table = std::mem::take(&mut parsed.function_table);
    generator.arena = parsed.arena.clone();
    let assembled = compile_program_body_to_bytecode(&mut generator, &parsed.program, parsed.scope_ref);
    let mut function_table = std::mem::take(&mut generator.function_table);
    let executable = ExecutableData::new(generator, assembled);

    let declarations = collect_script_declarations(
        &parsed.arena.scopes[parsed.scope_ref],
        &mut function_table,
        &parsed.arena,
    );

    CompiledScript {
        executable,
        declarations,
    }
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
        parsed.function_table = std::mem::take(&mut generator.function_table);
        (
            CompiledProgramBytecode::AsyncModule(ExecutableData::new(generator, assembled)),
            declaration_functions,
        )
    } else {
        let mut generator = new_program_generator(parsed.is_strict_mode, source_len);
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
        parsed.function_table = std::mem::take(&mut generator.function_table);
        (
            CompiledProgramBytecode::Program(ExecutableData::new(generator, assembled)),
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

// =============================================================================
// Eval
// =============================================================================

/// How the code an eval runs relates to the code that calls it, as PerformEval works it out.
#[derive(Clone, Copy, Default)]
pub struct EvalContext {
    pub starts_in_strict_mode: bool,
    pub in_eval_function_context: bool,
    pub allow_super_property_lookup: bool,
    pub allow_super_constructor_call: bool,
    pub in_class_field_initializer: bool,
}

/// The code of an eval, parsed and analyzed without errors.
pub struct ParsedEval {
    pub(crate) program: ast::Statement,
    pub(crate) function_table: ast::FunctionTable,
    pub(crate) arena: std::sync::Arc<ast::AstArena>,
    pub(crate) scope_id: ast::ScopeId,
    pub(crate) is_strict: bool,
    pub(crate) eval_referenced_private_names: Vec<ast::Utf16String>,
}

/// Parses the code of an eval, or returns its syntax errors and then its early errors.
pub fn parse_eval(source: &[u16], context: EvalContext) -> Result<ParsedEval, Vec<ParseError>> {
    let mut parser = Parser::new(source, ProgramType::Script);
    parser.initiated_by_eval = true;
    parser.in_eval_function_context = context.in_eval_function_context;
    parser.flags.allow_super_property_lookup = context.allow_super_property_lookup;
    parser.flags.allow_super_constructor_call = context.allow_super_constructor_call;
    parser.flags.in_class_field_initializer = context.in_class_field_initializer;

    let program = parser.parse_program(context.starts_in_strict_mode);

    if parser.has_errors() {
        return Err(parser.take_errors());
    }
    if parser.scope_collector.has_errors() {
        return Err(parser.scope_collector.drain_errors());
    }

    let eval_referenced_private_names = parser.eval_referenced_private_names().to_vec();

    parser.scope_collector.analyze(
        true,
        &mut parser.arena.identifiers,
        &parser.arena.strings,
        &mut parser.arena.scopes,
    );

    let StatementKind::Program(ref data) = program.inner else {
        unreachable!("the parser produces a program");
    };
    let (scope_id, is_strict) = (data.scope, data.is_strict_mode);

    Ok(ParsedEval {
        program,
        function_table: std::mem::take(&mut parser.function_table),
        arena: std::sync::Arc::new(std::mem::take(&mut parser.arena)),
        scope_id,
        is_strict,
        eval_referenced_private_names,
    })
}

/// An eval compiled to bytecode, with what EvalDeclarationInstantiation needs from it.
pub struct CompiledEval {
    pub executable: ExecutableData,
    pub declarations: EvalDeclarations,
}

/// Compiles the code of an eval parsed from `source_len` code units.
pub fn compile_eval(mut parsed: ParsedEval, source_len: usize) -> CompiledEval {
    let mut generator = new_program_generator(parsed.is_strict, source_len);
    generator.function_table = std::mem::take(&mut parsed.function_table);
    generator.arena = parsed.arena.clone();
    let assembled = compile_program_body_to_bytecode(&mut generator, &parsed.program, parsed.scope_id);
    let mut function_table = std::mem::take(&mut generator.function_table);
    let executable = ExecutableData::new(generator, assembled);
    let declarations = collect_eval_declarations(
        &parsed.arena.scopes[parsed.scope_id],
        parsed.is_strict,
        &mut function_table,
        &parsed.arena,
        parsed.eval_referenced_private_names,
    );
    CompiledEval {
        executable,
        declarations,
    }
}

// =============================================================================
// Modules
// =============================================================================

/// An ImportEntry Record of a module: `import_name` is None for a namespace import.
pub struct ModuleImportEntry {
    pub import_name: Option<ast::Utf16String>,
    pub local_name: ast::Utf16String,
    pub module_request: ast::ModuleRequest,
}

/// An ExportEntry Record of a module.
pub struct ModuleExportEntry {
    pub kind: ast::ExportEntryKind,
    pub export_name: Option<ast::Utf16String>,
    pub local_or_import_name: Option<ast::Utf16String>,
    pub module_request: Option<ast::ModuleRequest>,
}

/// A function declaration of a module, which InitializeEnvironment instantiates.
pub struct ModuleFunctionToInitialize {
    pub description: SharedFunctionDescription,
    /// The name the module binds the function under.
    pub name: ast::Utf16String,
    /// Whether this is an anonymous default export, whose function is named "default".
    pub is_anonymous_default_export: bool,
}

/// A lexical declaration of a module; `function_index` refers to the function it is initialized with.
pub struct ModuleLexicalBinding {
    pub name: ast::Utf16String,
    pub is_constant: bool,
    pub function_index: Option<usize>,
}

/// What a Source Text Module Record needs from a module's code.
/// https://tc39.es/ecma262/#sec-parsemodule
pub struct ModuleDeclarations {
    pub has_top_level_await: bool,
    pub import_entries: Vec<ModuleImportEntry>,
    pub default_export_binding_name: Option<ast::Utf16String>,
    pub local_export_entries: Vec<ModuleExportEntry>,
    pub indirect_export_entries: Vec<ModuleExportEntry>,
    pub star_export_entries: Vec<ModuleExportEntry>,
    pub var_names: Vec<ast::Utf16String>,
    pub functions_to_initialize: Vec<ModuleFunctionToInitialize>,
    pub lexical_bindings: Vec<ModuleLexicalBinding>,
    /// In source order.
    pub requested_modules: Vec<ast::ModuleRequest>,
}

/// Collects what ParseModule records from a module's top-level scope, taking the functions to initialize out of
/// `function_table`.
pub fn collect_module_declarations(
    scope: &ast::ScopeData,
    has_top_level_await: bool,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
) -> ModuleDeclarations {
    use ast::ExportEntryKind;

    let mut declarations = ModuleDeclarations {
        has_top_level_await,
        import_entries: Vec::new(),
        default_export_binding_name: module_default_export_binding_name(scope).cloned(),
        local_export_entries: Vec::new(),
        indirect_export_entries: Vec::new(),
        star_export_entries: Vec::new(),
        var_names: Vec::new(),
        functions_to_initialize: Vec::new(),
        lexical_bindings: Vec::new(),
        requested_modules: Vec::new(),
    };

    for child in &scope.children {
        if let StatementKind::Import(ref import_data) = child.inner {
            for entry in &import_data.entries {
                declarations.import_entries.push(ModuleImportEntry {
                    import_name: entry.import_name.clone(),
                    local_name: entry.local_name.clone(),
                    module_request: import_data.module_request.clone(),
                });
            }
        }
    }

    // ParseModule steps 9-10.
    for child in &scope.children {
        let StatementKind::Export(ref export_data) = child.inner else {
            continue;
        };
        for entry in &export_data.entries {
            if entry.kind == ExportEntryKind::EmptyNamedExport {
                break;
            }
            let Some(ref module_request) = export_data.module_request else {
                let matching_import = declarations
                    .import_entries
                    .iter()
                    .find(|import_entry| entry.local_or_import_name.as_ref() == Some(&import_entry.local_name));
                let export_entry = match matching_import {
                    // Re-exporting an imported module namespace object is an indirect namespace export.
                    Some(import_entry) if import_entry.import_name.is_none() => ModuleExportEntry {
                        kind: ExportEntryKind::ModuleRequestAll,
                        export_name: entry.export_name.clone(),
                        local_or_import_name: None,
                        module_request: Some(import_entry.module_request.clone()),
                    },
                    // Re-exporting an imported binding is an indirect export of it.
                    Some(import_entry) => ModuleExportEntry {
                        kind: ExportEntryKind::NamedExport,
                        export_name: entry.export_name.clone(),
                        local_or_import_name: import_entry.import_name.clone(),
                        module_request: Some(import_entry.module_request.clone()),
                    },
                    None => {
                        declarations.local_export_entries.push(ModuleExportEntry {
                            kind: entry.kind,
                            export_name: entry.export_name.clone(),
                            local_or_import_name: entry.local_or_import_name.clone(),
                            module_request: None,
                        });
                        continue;
                    }
                };
                declarations.indirect_export_entries.push(export_entry);
                continue;
            };
            let export_entry = ModuleExportEntry {
                kind: entry.kind,
                export_name: entry.export_name.clone(),
                local_or_import_name: entry.local_or_import_name.clone(),
                module_request: Some(module_request.clone()),
            };
            if entry.kind == ExportEntryKind::ModuleRequestAllButDefault {
                declarations.star_export_entries.push(export_entry);
            } else {
                declarations.indirect_export_entries.push(export_entry);
            }
        }
    }

    for child in &scope.children {
        collect_module_var_names(&child.inner, arena, &mut |name| {
            declarations.var_names.push(name.to_vec().into());
        });
    }

    let default_name: ast::Utf16String = utf16!("*default*").into();
    let module_environment_scope = module_environment_scope(scope, arena);
    for child in &scope.children {
        let (declaration, is_exported) = match &child.inner {
            StatementKind::Export(export_data) => match export_data.statement {
                Some(ref statement) => (&statement.inner, true),
                None => continue,
            },
            other => (other, false),
        };
        match declaration {
            StatementKind::FunctionDeclaration(function_declaration) => {
                let is_anonymous_default_export = is_exported
                    && function_declaration
                        .name
                        .is_some_and(|name| arena.name_of(name).as_slice() == default_name.as_slice());
                let function_data = function_table.take(function_declaration.function_id);
                let subtable = function_table.extract_reachable(&function_data, &arena.scopes);
                let description = describe_shared_function(
                    function_data,
                    subtable,
                    true,
                    None,
                    arena.clone(),
                    Some(module_environment_scope.clone()),
                );
                // The binding name from the AST, e.g. "*default*" for an anonymous default export.
                let Some(binding_name) = function_declaration.name.map(|name| arena.name_of(name).clone()) else {
                    continue;
                };
                let name = if is_anonymous_default_export {
                    utf16!("default").into()
                } else {
                    binding_name.clone()
                };
                declarations.lexical_bindings.push(ModuleLexicalBinding {
                    name: binding_name,
                    is_constant: false,
                    function_index: Some(declarations.functions_to_initialize.len()),
                });
                declarations.functions_to_initialize.push(ModuleFunctionToInitialize {
                    description,
                    name,
                    is_anonymous_default_export,
                });
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    declarations.lexical_bindings.push(ModuleLexicalBinding {
                        name: arena.name_of(name).clone(),
                        is_constant: false,
                        function_index: None,
                    });
                }
            }
            StatementKind::VariableDeclaration(variable_declaration)
                if variable_declaration.kind != ast::DeclarationKind::Var =>
            {
                let is_constant = variable_declaration.kind == ast::DeclarationKind::Const;
                for declarator in &variable_declaration.declarations {
                    for_each_bound_name(&declarator.target, arena, &mut |name| {
                        declarations.lexical_bindings.push(ModuleLexicalBinding {
                            name: name.to_vec().into(),
                            is_constant,
                            function_index: None,
                        });
                    });
                }
            }
            StatementKind::UsingDeclaration(using_declarations) => {
                for declarator in using_declarations.iter() {
                    for_each_bound_name(&declarator.target, arena, &mut |name| {
                        declarations.lexical_bindings.push(ModuleLexicalBinding {
                            name: name.to_vec().into(),
                            is_constant: false,
                            function_index: None,
                        });
                    });
                }
            }
            _ => {}
        }
    }

    let mut requested_modules: Vec<(u32, &ast::ModuleRequest)> = Vec::new();
    for child in &scope.children {
        match &child.inner {
            StatementKind::Import(import_data) => {
                requested_modules.push((child.range.start.offset, &import_data.module_request));
            }
            StatementKind::Export(export_data) => {
                if let Some(ref module_request) = export_data.module_request {
                    requested_modules.push((child.range.start.offset, module_request));
                }
            }
            _ => {}
        }
    }
    requested_modules.sort_by_key(|(source_offset, _)| *source_offset);
    declarations.requested_modules = requested_modules
        .into_iter()
        .map(|(_, module_request)| module_request.clone())
        .collect();

    declarations
}

/// A module compiled to bytecode, with what its Source Text Module Record needs.
pub struct CompiledModule {
    /// The module body; for a module with top-level await, the body of an async function.
    pub executable: ExecutableData,
    pub declarations: ModuleDeclarations,
}

/// Compiles a module the caller parsed without errors from `source_len` code units.
pub fn compile_module(mut parsed: ParsedProgram, source_len: usize) -> CompiledModule {
    assert!(
        parsed.program_type == ProgramType::Module && !parsed.has_errors(),
        "compile_module() needs a module without parse errors"
    );
    let declarations = collect_module_declarations(
        &parsed.arena.scopes[parsed.scope_ref],
        parsed.has_top_level_await,
        &mut parsed.function_table,
        &parsed.arena,
    );
    let function_table = std::mem::take(&mut parsed.function_table);
    let executable = if parsed.has_top_level_await {
        let mut generator = new_module_async_generator(source_len, function_table);
        generator.arena = parsed.arena.clone();
        let assembled = compile_module_as_async_to_bytecode(&parsed.program, parsed.scope_ref, &mut generator);
        ExecutableData::new(generator, assembled)
    } else {
        let mut generator = new_program_generator(true, source_len);
        generator.function_table = function_table;
        generator.arena = parsed.arena.clone();
        let assembled = compile_program_body_to_bytecode(&mut generator, &parsed.program, parsed.scope_ref);
        ExecutableData::new(generator, assembled)
    };
    CompiledModule {
        executable,
        declarations,
    }
}

// =============================================================================
// Dynamic functions
// =============================================================================

/// The source of a function that CreateDynamicFunction (new Function() and its relatives) builds, parsed and analyzed
/// without errors.
pub struct ParsedDynamicFunction {
    pub(crate) program: ast::Statement,
    pub(crate) function_table: ast::FunctionTable,
    pub(crate) arena: ast::AstArena,
}

// 20.2.1.1.1 CreateDynamicFunction ( constructor, newTarget, kind, parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-createdynamicfunction
/// Checks the parameters and the body of a dynamic function on their own and then parses `full_source`, the function
/// expression made of them, or returns the first errors one of these steps reports. `body_source` is the body as the
/// caller wraps it in newlines.
pub fn parse_dynamic_function(
    full_source: &[u16],
    parameters_source: &[u16],
    body_source: &[u16],
    kind: ast::FunctionKind,
) -> Result<ParsedDynamicFunction, Vec<ParseError>> {
    // Lex the parameters on their own first, so that lexer errors such as an unterminated comment have positions in
    // the parameter string.
    let mut lexer = crate::lexer::Lexer::new(parameters_source, 1, 0);
    loop {
        let token = lexer.next();
        if token.token_type == crate::token::TokenType::Eof {
            break;
        }
        if token.token_type == crate::token::TokenType::Invalid {
            let message = token
                .message
                .unwrap_or_else(|| format!("Unexpected token {}", token.token_type.name()));
            return Err(vec![ParseError {
                message,
                line: token.line_number,
                column: token.line_column,
            }]);
        }
    }

    // Then check them as the parameters of a function of the same kind.
    let mut parameters_check: Vec<u16> = Vec::new();
    parameters_check.extend_from_slice(match kind {
        ast::FunctionKind::Generator => utf16!("function* test("),
        ast::FunctionKind::Async => utf16!("async function test("),
        ast::FunctionKind::AsyncGenerator => utf16!("async function* test("),
        ast::FunctionKind::Normal => utf16!("function test("),
    });
    parameters_check.extend_from_slice(parameters_source);
    parameters_check.extend_from_slice(utf16!("\n) {}"));
    let mut parser = Parser::new(&parameters_check, ProgramType::Script);
    parser.parse_program(false);
    take_parser_errors(&mut parser)?;

    // Check the body on its own as a script with the flags of a function body of the kind, as the C++
    // parse_function_body_from_string did.
    let mut parser = Parser::new(body_source, ProgramType::Script);
    parser.flags.in_function_context = true;
    parser.flags.new_target_is_valid = true;
    if matches!(kind, ast::FunctionKind::Async | ast::FunctionKind::AsyncGenerator) {
        parser.flags.await_expression_is_valid = true;
    }
    if matches!(kind, ast::FunctionKind::Generator | ast::FunctionKind::AsyncGenerator) {
        parser.flags.in_generator_function_context = true;
    }
    parser.parse_program(false);
    take_parser_errors(&mut parser)?;

    let mut parser = Parser::new(full_source, ProgramType::Script);
    let program = parser.parse_program(false);
    take_parser_errors(&mut parser)?;

    // The function is parsed as a function expression, so it has no Program scope whose globals its identifiers
    // could bind to.
    parser.scope_collector.analyze_as_dynamic_function(
        &mut parser.arena.identifiers,
        &parser.arena.strings,
        &mut parser.arena.scopes,
    );
    if parser.scope_collector.has_errors() {
        return Err(parser.scope_collector.drain_errors());
    }

    Ok(ParsedDynamicFunction {
        program,
        function_table: std::mem::take(&mut parser.function_table),
        arena: std::mem::take(&mut parser.arena),
    })
}

/// The parser's syntax errors, or else the early errors its scope collector found while parsing.
fn take_parser_errors(parser: &mut Parser) -> Result<(), Vec<ParseError>> {
    if parser.has_errors() {
        return Err(parser.take_errors());
    }
    if parser.scope_collector.has_errors() {
        return Err(parser.scope_collector.drain_errors());
    }
    Ok(())
}

impl ParsedDynamicFunction {
    /// Describes the function the source defines, which always gets an arguments object, as C++
    /// FunctionConstructor::create_dynamic_function asks for.
    pub fn into_description(mut self) -> Result<SharedFunctionDescription, Vec<ParseError>> {
        // The program is a single ExpressionStatement wrapping a FunctionExpression.
        let function_id = if let StatementKind::Program(ref data) = self.program.inner {
            self.arena.scopes[data.scope]
                .children
                .iter()
                .find_map(|child| match &child.inner {
                    StatementKind::FunctionDeclaration(function_declaration) => Some(function_declaration.function_id),
                    StatementKind::Expression(expression) => match &expression.inner {
                        ast::ExpressionKind::Function(function_id) => Some(*function_id),
                        _ => None,
                    },
                    _ => None,
                })
        } else {
            None
        };
        let Some(function_id) = function_id else {
            return Err(vec![ParseError {
                message: "Failed to parse dynamic function".to_string(),
                line: 0,
                column: 0,
            }]);
        };

        let mut function_data = self.function_table.take(function_id);
        function_data.parsing_insights.might_need_arguments_object = true;

        let is_strict = function_data.is_strict_mode;
        let subtable = self
            .function_table
            .extract_reachable(&function_data, &self.arena.scopes);
        let arena = std::sync::Arc::new(self.arena);
        Ok(describe_shared_function(
            function_data,
            subtable,
            is_strict,
            None,
            arena,
            None,
        ))
    }
}

// =============================================================================
// Builtin files
// =============================================================================

/// Parses a file of builtin functions written in JavaScript, which is strict code, and runs scope analysis on it.
/// Builtin files ship with the engine, so a syntax error in one stops the process.
pub fn parse_builtin_file(source: &[u16]) -> ParsedProgram {
    let mut parser = Parser::new(source, ProgramType::Script);
    let program = parser.parse_program(true);

    if parser.has_errors() {
        let errors: Vec<String> = parser
            .errors()
            .iter()
            .map(|e| format!("{}:{}: {}", e.line, e.column, e.message))
            .collect();
        panic!("Parse errors in builtin file: {}", errors.join("; "));
    }

    parser.scope_collector.analyze(
        false,
        &mut parser.arena.identifiers,
        &parser.arena.strings,
        &mut parser.arena.scopes,
    );

    let StatementKind::Program(ref data) = program.inner else {
        unreachable!("parse_program() returns a Program");
    };
    let scope_ref = data.scope;

    ParsedProgram {
        program,
        function_table: std::mem::take(&mut parser.function_table),
        arena: std::sync::Arc::new(std::mem::take(&mut parser.arena)),
        scope_ref,
        program_type: ProgramType::Script,
        is_strict_mode: true,
        has_top_level_await: false,
        errors: Vec::new(),
        ast_dump: None,
    }
}

/// Describes the named functions that a parsed builtin file declares at its top level, in source order, taking their
/// ASTs out of the program.
pub fn describe_builtin_file_functions(parsed: &mut ParsedProgram) -> Vec<SharedFunctionDescription> {
    let arena = parsed.arena.clone();
    let scope = &arena.scopes[parsed.scope_ref];
    let mut descriptions = Vec::new();
    for child in &scope.children {
        if let StatementKind::FunctionDeclaration(ref function_declaration) = child.inner
            && function_declaration.name.is_some()
        {
            let function_data = parsed.function_table.take(function_declaration.function_id);
            let subtable = parsed.function_table.extract_reachable(&function_data, &arena.scopes);
            descriptions.push(describe_shared_function(
                function_data,
                subtable,
                true,
                None,
                arena.clone(),
                None,
            ));
        }
    }
    descriptions
}

// =============================================================================
// Shared function data
// =============================================================================

/// What a runtime needs to create a SharedFunctionInstanceData for one function, taken from the frontend's AST of it.
/// The AST travels along in `payload`, which compile_function() turns into bytecode when the function is first called.
pub struct SharedFunctionDescription {
    /// Empty for an anonymous function.
    pub name: Vec<u16>,
    pub function_kind: ast::FunctionKind,
    pub function_length: i32,
    pub formal_parameter_count: u32,
    pub strict: bool,
    pub is_arrow: bool,
    pub has_simple_parameter_list: bool,
    /// The parameter names if the parameter list is simple, and empty otherwise.
    pub parameter_names: Vec<ak::Utf16FlyString>,
    pub source_text_offset: usize,
    pub source_text_length: usize,
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub payload: Box<ast::FunctionPayload>,
}

/// Describes a function whose AST the caller took out of a function table, together with the functions nested in it.
#[allow(clippy::boxed_local)] // Callers produce Box<FunctionData>; unboxing would copy a large struct.
pub fn describe_shared_function(
    function_data: Box<ast::FunctionData>,
    subtable: ast::FunctionTable,
    is_strict: bool,
    name_override: Option<&[u16]>,
    arena: std::sync::Arc<ast::AstArena>,
    enclosing_environment_scope: Option<std::sync::Arc<bytecode::generator::EnclosingEnvironmentScope>>,
) -> SharedFunctionDescription {
    use ast::FunctionParameterBinding;

    let source_start = function_data.source_text_start as usize;
    let source_end = function_data.source_text_end as usize;

    let name = if let Some(name) = name_override {
        name.to_vec()
    } else if let Some(name_ident) = function_data.name {
        arena.name_slice(name_ident).to_vec()
    } else {
        Vec::new()
    };

    let has_simple_parameter_list = function_data.parameters.iter().all(|p| {
        !p.is_rest && p.default_value.is_none() && matches!(p.binding, FunctionParameterBinding::Identifier(_))
    });

    let parameter_names: Vec<ak::Utf16FlyString> = if has_simple_parameter_list {
        function_data
            .parameters
            .iter()
            .map(|p| {
                if let FunctionParameterBinding::Identifier(id) = p.binding {
                    ak::Utf16FlyString::from_utf16(arena.name_slice(id))
                } else {
                    unreachable!("has_simple_parameter_list guarantees all bindings are identifiers")
                }
            })
            .collect()
    } else {
        Vec::new()
    };

    SharedFunctionDescription {
        name,
        function_kind: function_data.kind,
        function_length: function_data.function_length,
        formal_parameter_count: u32_from_usize(function_data.parameters.len()),
        strict: function_data.is_strict_mode || is_strict,
        is_arrow: function_data.is_arrow_function,
        has_simple_parameter_list,
        parameter_names,
        source_text_offset: source_start,
        source_text_length: source_end - source_start,
        uses_this: function_data.parsing_insights.uses_this,
        uses_this_from_environment: function_data.parsing_insights.uses_this_from_environment,
        payload: Box::new(ast::FunctionPayload {
            data: *function_data,
            function_table: subtable,
            arena,
            enclosing_environment_scope,
        }),
    }
}

impl PendingSharedFunctionData {
    /// Describes this function of an executable whose code is strict if `is_strict`, taking its AST. The precompiled
    /// body and the class field initializer name stay here for the caller to take as well.
    pub fn take_description(&mut self, is_strict: bool) -> SharedFunctionDescription {
        let function_data = self
            .function_data
            .take()
            .expect("pending shared function data was already materialized");
        let subtable = self
            .subtable
            .take()
            .expect("pending shared function data subtable was already materialized");
        let arena = self
            .arena
            .clone()
            .expect("executable data records the AST arena of every pending function");
        describe_shared_function(
            function_data,
            subtable,
            is_strict,
            self.name_override.as_ref().map(|name| name.as_slice()),
            arena,
            self.enclosing_environment_scope.clone(),
        )
    }
}

/// Compiles the body of a described function, along with the functions nested in it that must be compiled eagerly.
#[allow(clippy::boxed_local)] // Runtimes keep the payload boxed until the first call; unboxing would copy it.
pub fn compile_function(
    payload: Box<ast::FunctionPayload>,
    source_len: usize,
    builtin_abstract_operations_enabled: bool,
) -> Box<bytecode::generator::PrecompiledFunction> {
    let arena = payload.arena.clone();
    let (_function_data, precompiled) = compile_function_payload_to_bytecode(
        *payload,
        source_len,
        builtin_abstract_operations_enabled,
        arena,
        FunctionPrecompileMode::EagerOnly,
    );
    precompiled
}

// =============================================================================
// Declaration instantiation data
// =============================================================================

/// A function declaration that declaration instantiation creates a function object for.
pub struct FunctionToInitialize {
    pub name: ast::Utf16String,
    pub shared_function_data: PendingSharedFunctionData,
}

/// A `let`, `const`, `using` or `class` binding that declaration instantiation creates.
pub struct LexicalBinding {
    pub name: ast::Utf16String,
    pub is_constant: bool,
}

/// The names and functions that GlobalDeclarationInstantiation needs from a script.
/// https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
pub struct ScriptDeclarations {
    pub lexical_names: Vec<ast::Utf16String>,
    pub var_names: Vec<ast::Utf16String>,
    /// The last declaration of each function name, in source order.
    pub functions_to_initialize: Vec<FunctionToInitialize>,
    pub var_scoped_names: Vec<ast::Utf16String>,
    pub annex_b_candidate_names: Vec<ast::Utf16String>,
    pub lexical_bindings: Vec<LexicalBinding>,
}

/// The names and functions that EvalDeclarationInstantiation needs from an eval script.
/// https://tc39.es/ecma262/#sec-evaldeclarationinstantiation
pub struct EvalDeclarations {
    pub is_strict: bool,
    pub var_names: Vec<ast::Utf16String>,
    /// The last declaration of each function name, in source order.
    pub functions_to_initialize: Vec<FunctionToInitialize>,
    pub var_scoped_names: Vec<ast::Utf16String>,
    pub annex_b_candidate_names: Vec<ast::Utf16String>,
    pub lexical_bindings: Vec<LexicalBinding>,
    pub private_names: Vec<ast::Utf16String>,
}

/// Collect what GlobalDeclarationInstantiation needs from a script's top-level scope, taking the functions to
/// initialize out of `function_table`.
pub fn collect_script_declarations(
    scope: &ast::ScopeData,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
) -> ScriptDeclarations {
    use ast::DeclarationKind;

    // Lexical names (let/const/using/class at top level) — script-only step.
    let mut lexical_names = Vec::new();
    for child in &scope.children {
        match &child.inner {
            StatementKind::VariableDeclaration(vd) if vd.kind != DeclarationKind::Var => {
                for declaration in &vd.declarations {
                    for_each_bound_name(&declaration.target, arena, &mut |name| lexical_names.push(name.into()));
                }
            }
            StatementKind::UsingDeclaration(declarations) => {
                for declaration in declarations.iter() {
                    for_each_bound_name(&declaration.target, arena, &mut |name| lexical_names.push(name.into()));
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    lexical_names.push(arena.name_of(name).clone());
                }
            }
            _ => {}
        }
    }

    let declarations = collect_var_scoped_declarations(scope, function_table, arena);
    ScriptDeclarations {
        lexical_names,
        var_names: declarations.var_names,
        functions_to_initialize: declarations.functions_to_initialize,
        var_scoped_names: declarations.var_scoped_names,
        annex_b_candidate_names: declarations.annex_b_candidate_names,
        lexical_bindings: declarations.lexical_bindings,
    }
}

/// Collect what EvalDeclarationInstantiation needs from an eval script's top-level scope, taking the functions to
/// initialize out of `function_table`.
pub fn collect_eval_declarations(
    scope: &ast::ScopeData,
    is_strict: bool,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
    referenced_private_names: Vec<ast::Utf16String>,
) -> EvalDeclarations {
    let declarations = collect_var_scoped_declarations(scope, function_table, arena);
    EvalDeclarations {
        is_strict,
        var_names: declarations.var_names,
        functions_to_initialize: declarations.functions_to_initialize,
        var_scoped_names: declarations.var_scoped_names,
        annex_b_candidate_names: declarations.annex_b_candidate_names,
        lexical_bindings: declarations.lexical_bindings,
        private_names: referenced_private_names,
    }
}

struct VarScopedDeclarations {
    var_names: Vec<ast::Utf16String>,
    functions_to_initialize: Vec<FunctionToInitialize>,
    var_scoped_names: Vec<ast::Utf16String>,
    annex_b_candidate_names: Vec<ast::Utf16String>,
    lexical_bindings: Vec<LexicalBinding>,
}

/// Collect var names + function declaration names, deduplicated function
/// initializations, var-scoped names, annex B names, and lexical bindings.
///
/// Shared by both script and eval declaration instantiation.
fn collect_var_scoped_declarations(
    scope: &ast::ScopeData,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
) -> VarScopedDeclarations {
    use ast::DeclarationKind;

    // Var names (var declarations at any nesting level + top-level function declarations)
    let mut var_names = Vec::new();
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, arena, &mut |name| var_names.push(name.into()));
        if let Some(fd) = child.inner.function_declaration_for_labelled_item()
            && let Some(name_ident) = fd.name
        {
            var_names.push(arena.name_of(name_ident).clone());
        }
    }

    // Functions to initialize: keep the last declaration with each name
    // (ECMAScript hoisting semantics), but emit them in source order. Two
    // forward passes; StringId keys keep the inserts to a u32 compare.
    let mut last_position: std::collections::HashMap<ast::StringId, usize> = std::collections::HashMap::new();
    for (i, child) in scope.children.iter().enumerate() {
        if let Some(fd) = child.inner.function_declaration_for_labelled_item()
            && let Some(name_ident) = fd.name
        {
            last_position.insert(arena.identifiers[name_ident].name, i);
        }
    }
    let mut functions_to_initialize = Vec::new();
    for (i, child) in scope.children.iter().enumerate() {
        if let Some(fd) = child.inner.function_declaration_for_labelled_item()
            && let Some(name_ident) = fd.name
            && last_position.get(&arena.identifiers[name_ident].name).copied() == Some(i)
        {
            let function_data = function_table.take(fd.function_id);
            let subtable = function_table.extract_reachable(&function_data, &arena.scopes);
            functions_to_initialize.push(FunctionToInitialize {
                name: arena.name_of(name_ident).clone(),
                shared_function_data: PendingSharedFunctionData {
                    function_data: Some(function_data),
                    subtable: Some(subtable),
                    arena: Some(arena.clone()),
                    name_override: None,
                    class_field_initializer_name: None,
                    should_eager_compile: false,
                    precompiled_function: None,
                    enclosing_environment_scope: None,
                },
            });
        }
    }

    // Var-scoped names (var VariableDeclaration names, excluding function declarations)
    let mut var_scoped_names = Vec::new();
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, arena, &mut |name| var_scoped_names.push(name.into()));
    }

    let mut lexical_bindings = Vec::new();
    for child in &scope.children {
        match &child.inner {
            StatementKind::VariableDeclaration(vd) if vd.kind != DeclarationKind::Var => {
                let is_constant = vd.kind == DeclarationKind::Const;
                for declaration in &vd.declarations {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        lexical_bindings.push(LexicalBinding {
                            name: name.into(),
                            is_constant,
                        });
                    });
                }
            }
            StatementKind::UsingDeclaration(declarations) => {
                for declaration in declarations.iter() {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        lexical_bindings.push(LexicalBinding {
                            name: name.into(),
                            is_constant: false,
                        });
                    });
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    lexical_bindings.push(LexicalBinding {
                        name: arena.name_of(name).clone(),
                        is_constant: false,
                    });
                }
            }
            _ => {}
        }
    }

    VarScopedDeclarations {
        var_names,
        functions_to_initialize,
        var_scoped_names,
        annex_b_candidate_names: scope.annexb_function_names.clone(),
        lexical_bindings,
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
            executable: ExecutableData::new(generator, assembled),
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
