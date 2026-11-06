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
//!
//! Nothing here needs the VM, so a runtime can also parse and compile on
//! another thread with [`compile_parsed_program_off_thread()`] and finish the
//! script or module on its main thread.
//!
//! A syntax tree only lives as long as the compile job that needs it. All that
//! a function that has not run yet holds on to is a [`FunctionPayload`], and it
//! gets parsed and compiled on its own by [`compile_function()`].

use crate::ast;
use crate::ast::FunctionId;
use crate::ast::ProgramType;
use crate::ast::Sequence;
use crate::ast::StatementKind;
use crate::ast::SyntaxTree;
use crate::ast_dump;
use crate::bytecode;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::executable_data::PendingSharedFunctionData;
use crate::parser::EnclosingCode;
use crate::parser::FunctionRow;
use crate::parser::OutOfStack;
use crate::parser::ParseError;
use crate::parser::ParseOptions;
use crate::parser::ScriptData;
use crate::parser::check_syntax;
use crate::parser::parse_to_tree;
use crate::utf16::Utf16View;
use ak::Utf16String;
use std::collections::HashSet;
use std::sync::Arc;
use std::sync::OnceLock;

// Compile-time assertion: what parse and compile produce travels from a worker thread to the main thread.
const _: fn() = || {
    fn assert_send<T: Send>() {}
    fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send::<ParsedProgram>();
    assert_send::<CompiledProgram>();
    assert_send::<CompiledScript>();
    assert_send::<CompiledModule>();
    assert_send_and_sync::<bytecode::executable_data::PrecompiledFunction>();
    assert_send_and_sync::<ExecutableData>();
    assert_send_and_sync::<SyntaxTree>();
    assert_send_and_sync::<ScriptData>();
};

// =============================================================================
// ParsedProgram: GC-free parse result for off-thread parsing
// =============================================================================

/// A parsed program (script or module) that can be compiled later.
/// Contains no GC references, so it can safely be transferred between threads.
pub struct ParsedProgram {
    /// The tree, unless the program has errors.
    pub(crate) tree: Option<Arc<SyntaxTree>>,
    pub(crate) program_type: ast::ProgramType,
    pub(crate) is_strict_mode: bool,
    pub(crate) has_top_level_await: bool,
    pub(crate) errors: Vec<ParseError>,
    pub(crate) ast_dump: Option<String>,
    options: ParseOptions,
}

impl ParsedProgram {
    fn from_tree(tree: SyntaxTree, options: ParseOptions) -> Self {
        Self {
            program_type: tree.program_type(),
            is_strict_mode: tree.is_strict_mode(),
            has_top_level_await: tree.has_top_level_await(),
            tree: Some(Arc::new(tree)),
            errors: Vec::new(),
            ast_dump: None,
            options,
        }
    }

    pub(crate) fn tree(&self) -> &Arc<SyntaxTree> {
        self.tree.as_ref().expect("a program without errors has a tree")
    }

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

    /// The textual AST dump of the whole program, which parses it again to get the tree of everything, generated on
    /// first use.
    pub fn ast_dump(&mut self) -> &str {
        self.ast_dump.get_or_insert_with(|| {
            let source = &self
                .tree
                .as_ref()
                .expect("the program has a tree")
                .script
                .positions
                .source;
            let mut options = self.options;
            options.keep_whole_tree = true;
            let tree = parse_to_tree(source, &options)
                .ok()
                .expect("a program without errors parses the same way again");
            ast_dump::dump_program_to_string(&Arc::new(tree))
        })
    }
}

/// Lex and parse a script or module, then run scope analysis on it.
///
/// Errors are reported through the returned program rather than by failing,
/// so check [`ParsedProgram::has_errors()`] before compiling it. Lines are
/// counted from `initial_line_number`, except that a module always starts at
/// line 1 or later.
pub fn parse(source: &ak::Utf16String, program_type: ProgramType, initial_line_number: usize) -> ParsedProgram {
    let initial_line_number = if program_type == ProgramType::Module && initial_line_number == 0 {
        1
    } else {
        initial_line_number
    };
    let mut options = ParseOptions::new(program_type);
    options.initial_line_number = crate::u32_from_usize(initial_line_number);
    match parse_to_tree(source, &options) {
        Ok(tree) => ParsedProgram::from_tree(tree, options),
        Err(errors) => ParsedProgram {
            tree: None,
            program_type,
            is_strict_mode: false,
            has_top_level_await: false,
            errors,
            ast_dump: None,
            options,
        },
    }
}

/// A script or module compiled to bytecode, with what its record needs from its code. The syntax tree is gone by now, so
/// a compiled program holds no more of its source than the functions that have yet to compile need.
///
/// A clone shares everything with the original, and both can be finished into records of any number of realms.
#[derive(Clone)]
pub struct CompiledProgram {
    program_type: ProgramType,
    has_top_level_await: bool,
    executable: Arc<ExecutableData>,
    declarations: CompiledProgramDeclarations,
    script: Arc<ScriptData>,
    /// The bytes of the top-level code and of the declarations, which do not change.
    top_level_size: usize,
}

#[derive(Clone)]
enum CompiledProgramDeclarations {
    Script(Arc<ScriptDeclarations>),
    Module(Arc<ModuleDeclarations>),
}

/// The scope of the program of `tree`.
fn program_scope(tree: &SyntaxTree) -> ast::Scope<'_> {
    let StatementKind::Program(data) = tree.program().kind() else {
        unreachable!("the root of a tree is a program");
    };
    data.scope
}

/// A compiled script and what GlobalDeclarationInstantiation needs to run it.
pub struct CompiledScript {
    pub executable: Arc<ExecutableData>,
    pub declarations: Arc<ScriptDeclarations>,
}

/// Compile a script that parsed without errors.
///
/// # Panics
/// Panics if `parsed` is a module or has errors.
pub fn compile_script(parsed: ParsedProgram) -> CompiledScript {
    assert!(
        parsed.program_type == ProgramType::Script,
        "compile_script() needs a script, not a module"
    );
    assert!(
        !parsed.has_errors(),
        "compile_script() needs a script without parse errors"
    );

    let tree = parsed.tree().clone();
    let executable = Arc::new(compile_program(
        &tree,
        ProgramOptions {
            strict: parsed.is_strict_mode,
            is_async_module: false,
            eager_compile_direct_iifes: false,
        },
    ));

    let declarations = Arc::new(collect_script_declarations(&tree));

    CompiledScript {
        executable,
        declarations,
    }
}

/// The lowest address that compiling a program on this thread may take the stack to. A program has no VM to ask, as it
/// can compile on any thread, but it may run as deep as what the code of a function may.
fn stack_limit_of_this_thread() -> usize {
    match crate::parser::vm_stack_limit_of_this_thread() {
        Some(limit) => limit.saturating_add(STACK_FOR_SMALL_WALKS),
        None => crate::parser::stack_limit_for_budget(crate::parser::base::DEFAULT_STACK_BUDGET),
    }
}

/// What the kinds of programs differ in, as far as generating their code goes.
#[derive(Clone, Copy)]
struct ProgramOptions {
    strict: bool,
    /// Whether the program is a module with top-level await, which runs like an async function.
    is_async_module: bool,
    eager_compile_direct_iifes: bool,
}

/// Generates the code of the program of `tree`.
fn compile_program(tree: &Arc<SyntaxTree>, options: ProgramOptions) -> ExecutableData {
    let is_module = options.is_async_module || tree.program_type() == ProgramType::Module;
    let input = bytecode::codegen::ProgramInput {
        tree,
        strict: options.strict,
        is_async_module: options.is_async_module,
        eager_compile_direct_iifes: options.eager_compile_direct_iifes,
        enclosing_environment_scope: is_module.then(|| module_environment_scope(&program_scope(tree))),
        stack_limit: stack_limit_of_this_thread(),
    };
    bytecode::codegen::compile_program(&input)
        .unwrap_or_else(|_| bytecode::codegen::compile_program_too_deep_to_generate(tree))
}

/// Compiles a script or module that parsed without errors, which needs no VM and so can run on any thread. The main thread
/// finishes it with [`CompiledProgram::into_script()`] or [`CompiledProgram::into_module()`].
///
/// # Panics
/// Panics if `parsed` has errors.
pub fn compile_parsed_program_off_thread(parsed: ParsedProgram) -> CompiledProgram {
    assert!(
        !parsed.has_errors(),
        "compile_parsed_program_off_thread() needs a program without parse errors"
    );
    let tree = parsed.tree().clone();
    let executable = Arc::new(compile_program(
        &tree,
        ProgramOptions {
            strict: parsed.has_top_level_await || parsed.is_strict_mode,
            is_async_module: parsed.has_top_level_await,
            eager_compile_direct_iifes: true,
        },
    ));
    let declarations = match parsed.program_type {
        ProgramType::Script => CompiledProgramDeclarations::Script(Arc::new(collect_script_declarations(&tree))),
        ProgramType::Module => CompiledProgramDeclarations::Module(Arc::new(collect_module_declarations(&tree))),
    };

    tree.script.index.prepare_for_reparsing();

    let declarations_size = match &declarations {
        CompiledProgramDeclarations::Script(declarations) => declarations.memory_size(),
        CompiledProgramDeclarations::Module(declarations) => declarations.memory_size(),
    };

    CompiledProgram {
        program_type: parsed.program_type,
        has_top_level_await: parsed.has_top_level_await,
        top_level_size: executable.memory_size() + declarations_size,
        executable,
        declarations,
        script: tree.script.clone(),
    }
}

impl CompiledProgram {
    pub fn program_type(&self) -> ProgramType {
        self.program_type
    }

    pub fn has_top_level_await(&self) -> bool {
        self.has_top_level_await
    }

    /// Whether the code that functions of this program compile to stays with the program from now on, so that the
    /// next record of it finds them compiled. It is memory that nothing but such a record has a use for.
    pub fn set_retains_compiled_functions(&self, retains_compiled_functions: bool) {
        self.script.set_retains_compiled_functions(retains_compiled_functions);
    }

    /// The bytes that the program holds on to right now: its source and the tables of its functions, its top-level
    /// code, and the code of its functions that compiled while it retained them.
    pub fn memory_size(&self) -> usize {
        self.top_level_size + self.script.memory_size()
    }

    /// Finishes a script into what [`compile_script()`] returns.
    ///
    /// # Panics
    /// Panics if the program is a module.
    pub fn into_script(self) -> CompiledScript {
        let CompiledProgramDeclarations::Script(declarations) = self.declarations else {
            panic!("into_script() needs a script, not a module");
        };
        CompiledScript {
            executable: self.executable,
            declarations,
        }
    }

    /// Finishes a module into what [`compile_module()`] returns.
    ///
    /// # Panics
    /// Panics if the program is a script.
    pub fn into_module(self) -> CompiledModule {
        let CompiledProgramDeclarations::Module(declarations) = self.declarations else {
            panic!("into_module() needs a module, not a script");
        };
        CompiledModule {
            executable: self.executable,
            declarations,
        }
    }
}

/// Returns the name of the binding that holds the value of `export default <expression>`, if the module has one.
pub(crate) fn module_default_export_binding_name(scope: &ast::Scope) -> Option<Utf16String> {
    for child in &scope.children {
        let StatementKind::Export(export_data) = child.kind() else {
            continue;
        };
        if !export_data.is_default_export {
            continue;
        }
        let entries = export_data.entries();
        if entries.len() != 1 {
            continue;
        }

        // If the default export is not a declaration (function/class/etc.),
        // its binding name is the local_or_import_name.
        let is_declaration = export_data.statement.as_ref().is_some_and(|statement| {
            matches!(
                statement.kind(),
                StatementKind::FunctionDeclaration(_) | StatementKind::ClassDeclaration(_)
            )
        });
        if is_declaration {
            continue;
        }

        let entry = &entries[0];
        let is_specific_import_export = scope.children.iter().any(|child| {
            let StatementKind::Import(import_data) = child.kind() else {
                return false;
            };
            import_data.entries().iter().any(|import_entry| {
                entry.local_or_import_name.as_ref() == Some(&import_entry.local_name)
                    && import_entry.import_name.is_some()
            })
        });
        if !is_specific_import_export {
            return entry.local_or_import_name.clone();
        }
    }
    None
}

/// Returns the names of a module's own bindings, in the order that `SourceTextModule::initialize_environment()`
/// creates them in the module environment.
fn module_environment_binding_names(scope: &ast::Scope) -> Vec<ak::Utf16FlyString> {
    let mut names = Vec::new();
    let mut var_names = HashSet::new();
    for child in &scope.children {
        collect_module_var_names(&child, &mut |name| {
            if var_names.insert(Utf16String::from(name)) {
                names.push(name.to_utf16_fly_string());
            }
        });
    }

    for child in &scope.children {
        let declaration = match child.kind() {
            StatementKind::Export(export_data) => match export_data.statement {
                Some(statement) => statement.kind(),
                None => continue,
            },
            other => other,
        };

        match declaration {
            StatementKind::FunctionDeclaration(function) => {
                if let Some(name) = function.name {
                    names.push(name.text().to_utf16_fly_string());
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    names.push(name.text().to_utf16_fly_string());
                }
            }
            StatementKind::VariableDeclaration(declaration) if declaration.kind != ast::DeclarationKind::Var => {
                for declarator in &declaration.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| {
                        names.push(name.to_utf16_fly_string());
                    });
                }
            }
            StatementKind::UsingDeclaration(declaration) => {
                for declarator in &declaration.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| {
                        names.push(name.to_utf16_fly_string());
                    });
                }
            }
            _ => {}
        }
    }

    if let Some(name) = module_default_export_binding_name(scope) {
        names.push(Utf16View::of_string(&name).to_utf16_fly_string());
    }
    names
}

/// Returns the local names of a module's import entries, in the order of `SourceTextModule`'s import entries.
fn module_import_names(scope: &ast::Scope) -> Vec<ak::Utf16FlyString> {
    let mut names = Vec::new();
    for child in &scope.children {
        if let StatementKind::Import(import_data) = child.kind() {
            for entry in import_data.entries() {
                names.push(Utf16View::of_string(&entry.local_name).to_utf16_fly_string());
            }
        }
    }
    names
}

pub(crate) fn module_environment_scope(scope: &ast::Scope) -> Arc<bytecode::codegen::EnclosingEnvironmentScope> {
    bytecode::codegen::EnclosingEnvironmentScope::for_module_environment(
        module_environment_binding_names(scope),
        module_import_names(scope),
    )
}

/// Recursively collect var declared names for module scope.
pub(crate) fn collect_module_var_names(statement: &ast::Statement, f: &mut dyn FnMut(Utf16View<'_>)) {
    match statement.kind() {
        StatementKind::VariableDeclaration(declaration) if declaration.kind == ast::DeclarationKind::Var => {
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, f);
            }
        }
        StatementKind::Export(export_data) => {
            if let Some(statement) = export_data.statement {
                collect_module_var_names(&statement, f);
            }
        }
        _ => {
            for_each_child_statement(statement, &mut |child| {
                collect_module_var_names(child, f);
            });
        }
    }
}

/// Recursively collect var-declared names from a statement and all nested
/// statements, excluding function/class bodies (which create new var scopes).
pub(crate) fn collect_var_names_recursive(statement: &ast::Statement, push_name: &mut dyn FnMut(Utf16View<'_>)) {
    match statement.kind() {
        StatementKind::VariableDeclaration(declaration) if declaration.kind == ast::DeclarationKind::Var => {
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, push_name);
            }
        }
        _ => {
            for_each_child_statement(statement, &mut |child| {
                collect_var_names_recursive(child, push_name);
            });
        }
    }
}

/// Visit each child statement of a statement, excluding function/class bodies
/// (which create new var scopes). This enables recursive var-declaration walking.
fn for_each_child_statement(statement: &ast::Statement, f: &mut dyn FnMut(&ast::Statement)) {
    match statement.kind() {
        StatementKind::Block(scope) => {
            for child in &scope.children {
                f(&child);
            }
        }
        StatementKind::If(data) => {
            f(&data.consequent);
            if let Some(alternate) = &data.alternate {
                f(alternate);
            }
        }
        StatementKind::While(data) => {
            f(&data.body);
        }
        StatementKind::DoWhile(data) => {
            f(&data.body);
        }
        StatementKind::With(data) => {
            f(&data.body);
        }
        StatementKind::For(data) => {
            if let Some(ast::ForInit::Declaration(declaration)) = &data.init {
                f(declaration);
            }
            f(&data.body);
        }
        StatementKind::ForInOf(data) => {
            if let ast::ForInOfLhs::Declaration(declaration) = &data.lhs {
                f(declaration);
            }
            f(&data.body);
        }
        StatementKind::Switch(data) => {
            for case in &data.cases {
                for child in &case.scope.children {
                    f(&child);
                }
            }
        }
        StatementKind::Try(data) => {
            f(&data.block);
            if let Some(handler) = &data.handler {
                f(&handler.body);
            }
            if let Some(finalizer) = &data.finalizer {
                f(finalizer);
            }
        }
        StatementKind::Labelled(data) => {
            f(&data.item);
        }
        // Don't recurse into function/class bodies (new var scopes)
        _ => {}
    }
}

pub(crate) fn for_each_bound_name(target: &ast::VariableDeclaratorTarget, f: &mut dyn FnMut(Utf16View<'_>)) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(identifier) => f(identifier.text()),
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            for_each_bound_name_in_pattern(pattern, f);
        }
    }
}

fn for_each_bound_name_in_pattern(pattern: &ast::BindingPattern, f: &mut dyn FnMut(Utf16View<'_>)) {
    for entry in &pattern.entries {
        match &entry.alias {
            None => {
                if let Some(ast::BindingEntryName::Identifier(identifier)) = &entry.name {
                    f(identifier.text());
                }
            }
            Some(ast::BindingEntryAlias::Identifier(identifier)) => f(identifier.text()),
            Some(ast::BindingEntryAlias::BindingPattern(inner)) => {
                for_each_bound_name_in_pattern(inner, f);
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

// =============================================================================
// Eval
// =============================================================================

/// The bytes of the storage of `string`.
pub(crate) fn utf16_string_memory_size(string: &Utf16String) -> usize {
    match string.as_units() {
        ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => units.len(),
        ak::Utf16StringUnits::Utf16(units) => size_of_val(units),
    }
}

fn utf16_strings_memory_size(strings: &[Utf16String]) -> usize {
    size_of_val(strings) + strings.iter().map(utf16_string_memory_size).sum::<usize>()
}

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
    pub(crate) tree: Arc<SyntaxTree>,
}

/// Parses the code of an eval, or returns its syntax errors and then its early errors.
pub fn parse_eval(source: &ak::Utf16String, context: EvalContext) -> Result<ParsedEval, Vec<ParseError>> {
    let mut options = ParseOptions::new(ProgramType::Script);
    options.starts_in_strict_mode = context.starts_in_strict_mode;
    options.initiated_by_eval = true;
    options.in_eval_function_context = context.in_eval_function_context;
    options.allow_super_property_lookup = context.allow_super_property_lookup;
    options.allow_super_constructor_call = context.allow_super_constructor_call;
    options.in_class_field_initializer = context.in_class_field_initializer;
    Ok(ParsedEval {
        tree: Arc::new(parse_to_tree(source, &options)?),
    })
}

/// An eval compiled to bytecode, with what EvalDeclarationInstantiation needs from it.
pub struct CompiledEval {
    pub executable: Arc<ExecutableData>,
    pub declarations: EvalDeclarations,
}

/// Compiles the code of an eval.
pub fn compile_eval(parsed: ParsedEval) -> CompiledEval {
    let tree = parsed.tree;
    let executable = Arc::new(compile_program(
        &tree,
        ProgramOptions {
            strict: tree.is_strict_mode(),
            is_async_module: false,
            eager_compile_direct_iifes: false,
        },
    ));
    let declarations = collect_eval_declarations(&tree);
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
    pub import_name: Option<Utf16String>,
    pub local_name: Utf16String,
    pub module_request: ast::ModuleRequest,
}

/// An ExportEntry Record of a module.
pub struct ModuleExportEntry {
    pub kind: ast::ExportEntryKind,
    pub export_name: Option<Utf16String>,
    pub local_or_import_name: Option<Utf16String>,
    pub module_request: Option<ast::ModuleRequest>,
}

/// A function declaration of a module, which InitializeEnvironment instantiates.
pub struct ModuleFunctionToInitialize {
    pub description: SharedFunctionDescription,
    /// The name the module binds the function under.
    pub name: Utf16String,
}

/// A lexical declaration of a module; `function_index` refers to the function it is initialized with.
pub struct ModuleLexicalBinding {
    pub name: Utf16String,
    pub is_constant: bool,
    pub function_index: Option<usize>,
}

/// What a Source Text Module Record needs from a module's code.
/// https://tc39.es/ecma262/#sec-parsemodule
pub struct ModuleDeclarations {
    pub has_top_level_await: bool,
    pub import_entries: Vec<ModuleImportEntry>,
    pub default_export_binding_name: Option<Utf16String>,
    pub local_export_entries: Vec<ModuleExportEntry>,
    pub indirect_export_entries: Vec<ModuleExportEntry>,
    pub star_export_entries: Vec<ModuleExportEntry>,
    pub var_names: Vec<Utf16String>,
    pub functions_to_initialize: Vec<ModuleFunctionToInitialize>,
    pub lexical_bindings: Vec<ModuleLexicalBinding>,
    /// In source order.
    pub requested_modules: Vec<ast::ModuleRequest>,
}

impl ModuleDeclarations {
    fn memory_size(&self) -> usize {
        let optional_string_size = |string: &Option<Utf16String>| string.as_ref().map_or(0, utf16_string_memory_size);
        let entries = [
            &self.local_export_entries,
            &self.indirect_export_entries,
            &self.star_export_entries,
        ];
        let export_entries_size = entries.iter().flat_map(|entries| entries.iter()).map(|entry| {
            size_of::<ModuleExportEntry>()
                + optional_string_size(&entry.export_name)
                + optional_string_size(&entry.local_or_import_name)
        });
        let import_entries_size = self.import_entries.iter().map(|entry| {
            size_of::<ModuleImportEntry>()
                + optional_string_size(&entry.import_name)
                + utf16_string_memory_size(&entry.local_name)
        });
        let functions_size = self.functions_to_initialize.iter().map(|function| {
            size_of::<ModuleFunctionToInitialize>()
                + function.description.memory_size()
                + utf16_string_memory_size(&function.name)
        });
        let lexical_bindings_size = self
            .lexical_bindings
            .iter()
            .map(|binding| size_of::<ModuleLexicalBinding>() + utf16_string_memory_size(&binding.name));
        size_of::<Self>()
            + utf16_strings_memory_size(&self.var_names)
            + size_of_val(&*self.requested_modules)
            + export_entries_size.sum::<usize>()
            + import_entries_size.sum::<usize>()
            + functions_size.sum::<usize>()
            + lexical_bindings_size.sum::<usize>()
    }
}

/// Collects what ParseModule records from a module's top-level scope.
pub fn collect_module_declarations(tree: &Arc<SyntaxTree>) -> ModuleDeclarations {
    use ast::ExportEntryKind;

    let scope = &program_scope(tree);
    let mut declarations = ModuleDeclarations {
        has_top_level_await: tree.has_top_level_await(),
        import_entries: Vec::new(),
        default_export_binding_name: module_default_export_binding_name(scope),
        local_export_entries: Vec::new(),
        indirect_export_entries: Vec::new(),
        star_export_entries: Vec::new(),
        var_names: Vec::new(),
        functions_to_initialize: Vec::new(),
        lexical_bindings: Vec::new(),
        requested_modules: Vec::new(),
    };

    for child in &scope.children {
        if let StatementKind::Import(import_data) = child.kind() {
            let module_request = import_data.module_request();
            for entry in import_data.entries() {
                declarations.import_entries.push(ModuleImportEntry {
                    import_name: entry.import_name,
                    local_name: entry.local_name,
                    module_request: module_request.clone(),
                });
            }
        }
    }

    // ParseModule steps 9-10.
    for child in &scope.children {
        let StatementKind::Export(export_data) = child.kind() else {
            continue;
        };
        let export_module_request = export_data.module_request();
        for entry in &export_data.entries() {
            if entry.kind == ExportEntryKind::EmptyNamedExport {
                break;
            }
            let Some(ref module_request) = export_module_request else {
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
        collect_module_var_names(&child, &mut |name| {
            declarations.var_names.push(name.into());
        });
    }

    let module_environment_scope = module_environment_scope(scope);
    let enclosing_code = EnclosingCode::of_program(tree);
    for child in &scope.children {
        let (declaration, is_exported) = match child.kind() {
            StatementKind::Export(export_data) => match export_data.statement {
                Some(statement) => (statement.kind(), true),
                None => continue,
            },
            other => (other, false),
        };
        match declaration {
            StatementKind::FunctionDeclaration(function_declaration) => {
                let is_anonymous_default_export =
                    is_exported && function_declaration.name.is_some_and(|name| name.text() == "*default*");
                let mut description = describe_shared_function(
                    tree,
                    function_declaration.function_id,
                    true,
                    None,
                    Some(module_environment_scope.clone()),
                    &enclosing_code,
                );
                // The binding name from the AST, e.g. "*default*" for an anonymous default export.
                let Some(binding_name) = function_declaration.name.map(|name| Utf16String::from(name.text())) else {
                    continue;
                };
                let name = if is_anonymous_default_export {
                    Utf16String::from_utf16(utf16!("default"))
                } else {
                    binding_name.clone()
                };
                // NB: The shared data of an anonymous default export takes the name the module binds it under, from
                //     which the function's name is all that follows.
                if is_anonymous_default_export {
                    description.name = Utf16View::of_string(&name).to_utf16_fly_string();
                }
                declarations.lexical_bindings.push(ModuleLexicalBinding {
                    name: binding_name,
                    is_constant: false,
                    function_index: Some(declarations.functions_to_initialize.len()),
                });
                declarations
                    .functions_to_initialize
                    .push(ModuleFunctionToInitialize { description, name });
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    declarations.lexical_bindings.push(ModuleLexicalBinding {
                        name: name.text().into(),
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
                    for_each_bound_name(&declarator.target, &mut |name| {
                        declarations.lexical_bindings.push(ModuleLexicalBinding {
                            name: name.into(),
                            is_constant,
                            function_index: None,
                        });
                    });
                }
            }
            StatementKind::UsingDeclaration(using_declarations) => {
                for declarator in &using_declarations.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| {
                        declarations.lexical_bindings.push(ModuleLexicalBinding {
                            name: name.into(),
                            is_constant: true,
                            function_index: None,
                        });
                    });
                }
            }
            _ => {}
        }
    }

    let mut requested_modules: Vec<(u32, ast::ModuleRequest)> = Vec::new();
    for child in &scope.children {
        match child.kind() {
            StatementKind::Import(import_data) => {
                requested_modules.push((child.range().start.offset, import_data.module_request()));
            }
            StatementKind::Export(export_data) => {
                if let Some(module_request) = export_data.module_request() {
                    requested_modules.push((child.range().start.offset, module_request));
                }
            }
            _ => {}
        }
    }
    requested_modules.sort_by_key(|(source_offset, _)| *source_offset);
    declarations.requested_modules = requested_modules
        .into_iter()
        .map(|(_, module_request)| module_request)
        .collect();

    declarations
}

/// A module compiled to bytecode, with what its Source Text Module Record needs.
pub struct CompiledModule {
    /// The module body; for a module with top-level await, the body of an async function.
    pub executable: Arc<ExecutableData>,
    pub declarations: Arc<ModuleDeclarations>,
}

/// Compiles a module the caller parsed without errors.
pub fn compile_module(parsed: ParsedProgram) -> CompiledModule {
    assert!(
        parsed.program_type == ProgramType::Module && !parsed.has_errors(),
        "compile_module() needs a module without parse errors"
    );
    let tree = parsed.tree().clone();
    let declarations = Arc::new(collect_module_declarations(&tree));
    let executable = Arc::new(compile_program(
        &tree,
        ProgramOptions {
            strict: true,
            is_async_module: parsed.has_top_level_await,
            eager_compile_direct_iifes: false,
        },
    ));
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
    pub(crate) tree: Arc<SyntaxTree>,
}

// 20.2.1.1.1 CreateDynamicFunction ( constructor, newTarget, kind, parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-createdynamicfunction
/// Checks the parameters and the body of a dynamic function on their own and then parses `full_source`, the function
/// expression made of them, or returns the first errors one of these steps reports. `body_source` is the body as the
/// caller wraps it in newlines.
pub fn parse_dynamic_function(
    full_source: &ak::Utf16String,
    parameters_source: &[u16],
    body_source: &[u16],
    kind: ast::FunctionKind,
) -> Result<ParsedDynamicFunction, Vec<ParseError>> {
    // Lex the parameters on their own first, so that lexer errors such as an unterminated comment have positions in
    // the parameter string.
    let mut lexer = crate::lexer::Lexer::new(Utf16View::Utf16(parameters_source), 1, 0);
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
    check_syntax(
        &ak::Utf16String::from_utf16(&parameters_check),
        &ParseOptions::new(ProgramType::Script),
    )?;

    // Check the body on its own as a script with the flags of a function body of the kind.
    let mut options = ParseOptions::new(ProgramType::Script);
    options.function_body = Some(kind);
    check_syntax(&ak::Utf16String::from_utf16(body_source), &options)?;

    // The function is parsed as a function expression, so it has no Program scope whose globals its identifiers
    // could bind to.
    let mut options = ParseOptions::new(ProgramType::Script);
    options.suppress_globals = true;
    Ok(ParsedDynamicFunction {
        tree: Arc::new(parse_to_tree(full_source, &options)?),
    })
}

impl ParsedDynamicFunction {
    /// Describes the function the source defines.
    pub fn into_description(self) -> Result<SharedFunctionDescription, Vec<ParseError>> {
        // The program is a single ExpressionStatement wrapping a FunctionExpression.
        let function_id = program_scope(&self.tree)
            .children
            .iter()
            .find_map(|child| match child.kind() {
                StatementKind::FunctionDeclaration(function_declaration) => Some(function_declaration.function_id),
                StatementKind::Expression(expression) => match expression.kind() {
                    ast::ExpressionKind::Function(function_id) => Some(function_id),
                    _ => None,
                },
                _ => None,
            });
        let Some(function_id) = function_id else {
            return Err(vec![ParseError {
                message: "Failed to parse dynamic function".to_string(),
                line: 0,
                column: 0,
            }]);
        };

        let is_strict = self.tree.function(function_id).is_strict_mode;
        Ok(describe_shared_function(
            &self.tree,
            function_id,
            is_strict,
            None,
            None,
            &EnclosingCode::of_program(&self.tree),
        ))
    }
}

// =============================================================================
// Builtin files
// =============================================================================

/// Parses a file of builtin functions written in JavaScript, which is strict code, and runs scope analysis on it.
/// Builtin files ship with the engine, so a syntax error in one stops the process.
pub fn parse_builtin_file(source: &ak::Utf16String) -> ParsedProgram {
    let mut options = ParseOptions::new(ProgramType::Script);
    options.starts_in_strict_mode = true;
    match parse_to_tree(source, &options) {
        Ok(tree) => ParsedProgram::from_tree(tree, options),
        Err(errors) => {
            let errors: Vec<String> = errors
                .iter()
                .map(|error| format!("{}:{}: {}", error.line, error.column, error.message))
                .collect();
            panic!("Parse errors in builtin file: {}", errors.join("; "));
        }
    }
}

/// Describes the named functions that a parsed builtin file declares at its top level, in source order.
pub fn describe_builtin_file_functions(parsed: &ParsedProgram) -> Vec<SharedFunctionDescription> {
    let tree = parsed.tree();
    let enclosing_code = EnclosingCode::of_program(tree);
    let mut descriptions = Vec::new();
    for child in &program_scope(tree).children {
        if let StatementKind::FunctionDeclaration(function_declaration) = child.kind()
            && function_declaration.name.is_some()
        {
            descriptions.push(describe_shared_function(
                tree,
                function_declaration.function_id,
                true,
                None,
                None,
                &enclosing_code,
            ));
        }
    }
    descriptions
}

// =============================================================================
// Shared function data
// =============================================================================

/// What a function that has not run yet compiles from: the data of its source, its row there, and what it can not find
/// out on its own, which is what the code around it made of the environment and of the names it closes over.
pub struct FunctionPayload {
    pub script: Arc<ScriptData>,
    pub row: FunctionRow,
    /// The environment the function closes over, as laid out by the generators of its enclosing functions.
    pub enclosing_environment_scope: Option<Arc<bytecode::codegen::EnclosingEnvironmentScope>>,
    pub enclosing_code: Arc<EnclosingCode>,
    /// The function's code, once it compiled, if its script retains compiled functions.
    pub compiled: OnceLock<Arc<bytecode::executable_data::PrecompiledFunction>>,
}

/// What a runtime needs to create a SharedFunctionInstanceData for one function, none of which takes the function's own
/// tree to know. `payload` is what compile_function() turns into bytecode when the function is first called.
#[derive(Clone)]
pub struct SharedFunctionDescription {
    /// Empty for an anonymous function.
    pub name: ak::Utf16FlyString,
    pub function_kind: ast::FunctionKind,
    pub function_length: i32,
    pub formal_parameter_count: u32,
    pub strict: bool,
    pub is_arrow: bool,
    pub has_simple_parameter_list: bool,
    pub source_text_offset: usize,
    pub source_text_length: usize,
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub payload: Arc<FunctionPayload>,
}

impl SharedFunctionDescription {
    /// The bytes of this description and of its payload. What the payload shares with the functions around it counts
    /// for those.
    pub(crate) fn memory_size(&self) -> usize {
        size_of::<Self>() + size_of::<FunctionPayload>()
    }
}

/// Describes a function of `tree`, whose code does not have to be in the tree.
pub fn describe_shared_function(
    tree: &SyntaxTree,
    function: FunctionId,
    is_strict: bool,
    name_override: Option<ak::Utf16FlyString>,
    enclosing_environment_scope: Option<Arc<bytecode::codegen::EnclosingEnvironmentScope>>,
    enclosing_code: &Arc<EnclosingCode>,
) -> SharedFunctionDescription {
    let function_data = tree.function(function);
    let source_start = function_data.source_text_start as usize;
    let source_end = function_data.source_text_end as usize;

    let name = if let Some(name) = name_override {
        name
    } else if let Some(name) = function_data.name {
        name.text().to_utf16_fly_string()
    } else {
        ak::Utf16FlyString::default()
    };

    SharedFunctionDescription {
        name,
        function_kind: function_data.kind,
        function_length: function_data.function_length,
        formal_parameter_count: function_data.parameter_count,
        strict: function_data.is_strict_mode || is_strict,
        is_arrow: function_data.is_arrow_function,
        has_simple_parameter_list: function_data.has_simple_parameter_list,
        source_text_offset: source_start,
        source_text_length: source_end - source_start,
        uses_this: function_data.parsing_insights.uses_this,
        uses_this_from_environment: function_data.parsing_insights.uses_this_from_environment,
        payload: Arc::new(FunctionPayload {
            script: tree.script.clone(),
            row: tree.function_row(function),
            enclosing_environment_scope,
            enclosing_code: enclosing_code.clone(),
            compiled: OnceLock::new(),
        }),
    }
}

/// Describes the function at `row`, whose name the caller knows, from what the first parse left.
pub(crate) fn pending_function_of_row(
    script: &Arc<crate::parser::ScriptData>,
    row: u32,
    name: Option<ak::Utf16FlyString>,
    enclosing_environment_scope: Option<Arc<bytecode::codegen::EnclosingEnvironmentScope>>,
    enclosing_code: &Arc<EnclosingCode>,
) -> PendingSharedFunctionData {
    use crate::parser::columns::function_flag;
    let index = &script.index;
    let at = row as usize;
    let flags = index.flags[at];
    let (start, end) = (index.start[at] as usize, index.end[at] as usize);
    PendingSharedFunctionData {
        function: FunctionId::function(0),
        description: SharedFunctionDescription {
            name: name.unwrap_or_default(),
            function_kind: ast::FunctionKind::from_async_generator(
                flags & function_flag::ASYNC != 0,
                flags & function_flag::GENERATOR != 0,
            ),
            function_length: i32::try_from(index.length[at]).expect("a function has a sane number of parameters"),
            formal_parameter_count: index.parameter_count[at],
            strict: flags & function_flag::STRICT != 0,
            is_arrow: flags & function_flag::ARROW != 0,
            has_simple_parameter_list: flags & function_flag::SIMPLE_PARAMETERS != 0,
            source_text_offset: start,
            source_text_length: end - start,
            uses_this: flags & function_flag::INSIGHT_USES_THIS != 0,
            uses_this_from_environment: flags & function_flag::INSIGHT_THIS_FROM_ENVIRONMENT != 0,
            payload: Arc::new(FunctionPayload {
                script: script.clone(),
                row: crate::parser::FunctionRow(row),
                enclosing_environment_scope,
                enclosing_code: enclosing_code.clone(),
                compiled: OnceLock::new(),
            }),
        },
        class_field_initializer_name: None,
        should_eager_compile: false,
        precompiled_function: None,
    }
}

impl PendingSharedFunctionData {
    /// Describes a function of `tree` that an executable creates.
    pub fn new(
        tree: &SyntaxTree,
        function: FunctionId,
        name_override: Option<ak::Utf16FlyString>,
        enclosing_environment_scope: Option<Arc<bytecode::codegen::EnclosingEnvironmentScope>>,
        enclosing_code: &Arc<EnclosingCode>,
    ) -> Self {
        let description = describe_shared_function(
            tree,
            function,
            false,
            name_override,
            enclosing_environment_scope,
            enclosing_code,
        );
        Self {
            function,
            description,
            class_field_initializer_name: None,
            should_eager_compile: false,
            precompiled_function: None,
        }
    }

    /// The bytes of what is kept for this function, including the code it was precompiled to.
    pub fn memory_size(&self) -> usize {
        size_of::<Self>() - size_of::<SharedFunctionDescription>()
            + self.description.memory_size()
            + self
                .class_field_initializer_name
                .as_ref()
                .map_or(0, |(name, _)| utf16_string_memory_size(name))
            + self
                .precompiled_function
                .as_ref()
                .map_or(0, |precompiled| precompiled.memory_size())
    }

    /// The environment that the function closes over.
    pub fn enclosing_environment_scope(&self) -> Option<Arc<bytecode::codegen::EnclosingEnvironmentScope>> {
        self.description.payload.enclosing_environment_scope.clone()
    }

    /// Whether this function is strict as part of an executable whose code is strict if `is_strict`.
    pub fn is_strict_in(&self, is_strict: bool) -> bool {
        self.description.strict || is_strict
    }
}

/// What parsing, resolving and generating code leave of the stack for what does not watch it: the small helpers that
/// walk a binding pattern or the like, which nest no deeper than the parser allows.
const STACK_FOR_SMALL_WALKS: usize = 1 << 20;

/// Parses the function of `payload` on its own and compiles its body, unless that happened before and the function's
/// script keeps the result. Like [`compile_parsed_program_off_thread()`], it needs no VM and so can run on any thread.
/// `stack_limit` is the lowest address that the stack may reach on the way.
pub fn compile_function(
    payload: &FunctionPayload,
    builtin_abstract_operations_enabled: bool,
    stack_limit: usize,
) -> Result<bytecode::executable_data::PrecompiledFunction, OutOfStack> {
    if let Some(compiled) = payload.compiled.get() {
        return Ok(compiled.shared_copy());
    }

    let stack_limit = stack_limit.saturating_add(STACK_FOR_SMALL_WALKS);
    let compiled = bytecode::codegen::compile_function(
        &payload.script,
        payload.row,
        payload.enclosing_environment_scope.clone(),
        &payload.enclosing_code,
        builtin_abstract_operations_enabled,
        stack_limit,
    )?;

    // NB: Code with builtin abstract operations is only for the builtins of the engine, whatever asked for it.
    if payload.script.retains_compiled_functions()
        && !builtin_abstract_operations_enabled
        && payload.compiled.set(Arc::new(compiled.shared_copy())).is_ok()
    {
        payload.script.add_retained_code_size(compiled.memory_size());
    }
    Ok(compiled)
}

// =============================================================================
// Declaration instantiation data
// =============================================================================

/// A function declaration that declaration instantiation creates a function object for.
pub struct FunctionToInitialize {
    pub name: Utf16String,
    pub shared_function_data: PendingSharedFunctionData,
}

/// A `let`, `const`, `using` or `class` binding that declaration instantiation creates.
pub struct LexicalBinding {
    pub name: Utf16String,
    pub is_constant: bool,
}

/// The names and functions that GlobalDeclarationInstantiation needs from a script.
/// https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
pub struct ScriptDeclarations {
    pub lexical_names: Vec<Utf16String>,
    pub var_names: Vec<Utf16String>,
    /// The last declaration of each function name, in source order.
    pub functions_to_initialize: Vec<FunctionToInitialize>,
    pub var_scoped_names: Vec<Utf16String>,
    pub annex_b_candidate_names: Vec<Utf16String>,
    pub lexical_bindings: Vec<LexicalBinding>,
}

/// The names and functions that EvalDeclarationInstantiation needs from an eval script.
/// https://tc39.es/ecma262/#sec-evaldeclarationinstantiation
pub struct EvalDeclarations {
    pub is_strict: bool,
    pub var_names: Vec<Utf16String>,
    /// The last declaration of each function name, in source order.
    pub functions_to_initialize: Vec<FunctionToInitialize>,
    pub var_scoped_names: Vec<Utf16String>,
    pub annex_b_candidate_names: Vec<Utf16String>,
    pub lexical_bindings: Vec<LexicalBinding>,
    pub private_names: Vec<Utf16String>,
}

impl ScriptDeclarations {
    fn memory_size(&self) -> usize {
        let functions_size = self.functions_to_initialize.iter().map(|function| {
            size_of::<FunctionToInitialize>() - size_of::<PendingSharedFunctionData>()
                + function.shared_function_data.memory_size()
                + utf16_string_memory_size(&function.name)
        });
        let lexical_bindings_size = self
            .lexical_bindings
            .iter()
            .map(|binding| size_of::<LexicalBinding>() + utf16_string_memory_size(&binding.name));
        size_of::<Self>()
            + utf16_strings_memory_size(&self.lexical_names)
            + utf16_strings_memory_size(&self.var_names)
            + utf16_strings_memory_size(&self.var_scoped_names)
            + utf16_strings_memory_size(&self.annex_b_candidate_names)
            + functions_size.sum::<usize>()
            + lexical_bindings_size.sum::<usize>()
    }
}

/// Collect what GlobalDeclarationInstantiation needs from a script's top-level scope.
pub fn collect_script_declarations(tree: &Arc<SyntaxTree>) -> ScriptDeclarations {
    use ast::DeclarationKind;

    let scope = program_scope(tree);

    // Lexical names (let/const/using/class at top level) — script-only step.
    let mut lexical_names = Vec::new();
    for child in &scope.children {
        match child.kind() {
            StatementKind::VariableDeclaration(declaration) if declaration.kind != DeclarationKind::Var => {
                for declarator in &declaration.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| lexical_names.push(name.into()));
                }
            }
            StatementKind::UsingDeclaration(declaration) => {
                for declarator in &declaration.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| lexical_names.push(name.into()));
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    lexical_names.push(name.text().into());
                }
            }
            _ => {}
        }
    }

    let declarations = collect_var_scoped_declarations(tree);
    ScriptDeclarations {
        lexical_names,
        var_names: declarations.var_names,
        functions_to_initialize: declarations.functions_to_initialize,
        var_scoped_names: declarations.var_scoped_names,
        annex_b_candidate_names: declarations.annex_b_candidate_names,
        lexical_bindings: declarations.lexical_bindings,
    }
}

/// Collect what EvalDeclarationInstantiation needs from an eval script's top-level scope.
pub fn collect_eval_declarations(tree: &Arc<SyntaxTree>) -> EvalDeclarations {
    let declarations = collect_var_scoped_declarations(tree);
    EvalDeclarations {
        is_strict: tree.is_strict_mode(),
        var_names: declarations.var_names,
        functions_to_initialize: declarations.functions_to_initialize,
        var_scoped_names: declarations.var_scoped_names,
        annex_b_candidate_names: declarations.annex_b_candidate_names,
        lexical_bindings: declarations.lexical_bindings,
        private_names: tree.eval_referenced_private_names(),
    }
}

struct VarScopedDeclarations {
    var_names: Vec<Utf16String>,
    functions_to_initialize: Vec<FunctionToInitialize>,
    var_scoped_names: Vec<Utf16String>,
    annex_b_candidate_names: Vec<Utf16String>,
    lexical_bindings: Vec<LexicalBinding>,
}

/// Collect var names + function declaration names, deduplicated function
/// initializations, var-scoped names, annex B names, and lexical bindings.
///
/// Shared by both script and eval declaration instantiation.
fn collect_var_scoped_declarations(tree: &Arc<SyntaxTree>) -> VarScopedDeclarations {
    use ast::DeclarationKind;

    let scope = program_scope(tree);

    // Var names (var declarations at any nesting level + top-level function declarations)
    let mut var_names = Vec::new();
    for child in &scope.children {
        collect_var_names_recursive(&child, &mut |name| var_names.push(name.into()));
        if let Some(function) = child.function_declaration_for_labelled_item()
            && let Some(name) = function.name
        {
            var_names.push(name.text().into());
        }
    }

    // Functions to initialize: keep the last declaration with each name
    // (ECMAScript hoisting semantics), but emit them in source order. Two
    // forward passes; name keys keep the inserts to a u32 compare.
    let mut last_position: std::collections::HashMap<ast::Name, usize> = std::collections::HashMap::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let Some(function) = child.function_declaration_for_labelled_item()
            && let Some(name) = function.name
        {
            last_position.insert(name.name, index);
        }
    }
    let enclosing_code = EnclosingCode::of_program(tree);
    let mut functions_to_initialize = Vec::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let Some(function) = child.function_declaration_for_labelled_item()
            && let Some(name) = function.name
            && last_position.get(&name.name).copied() == Some(index)
        {
            functions_to_initialize.push(FunctionToInitialize {
                name: name.text().into(),
                shared_function_data: PendingSharedFunctionData::new(
                    tree,
                    function.function_id,
                    None,
                    None,
                    &enclosing_code,
                ),
            });
        }
    }

    // Var-scoped names (var VariableDeclaration names, excluding function declarations)
    let mut var_scoped_names = Vec::new();
    for child in &scope.children {
        collect_var_names_recursive(&child, &mut |name| var_scoped_names.push(name.into()));
    }

    let mut lexical_bindings = Vec::new();
    for child in &scope.children {
        match child.kind() {
            StatementKind::VariableDeclaration(declaration) if declaration.kind != DeclarationKind::Var => {
                let is_constant = declaration.kind == DeclarationKind::Const;
                for declarator in &declaration.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| {
                        lexical_bindings.push(LexicalBinding {
                            name: name.into(),
                            is_constant,
                        });
                    });
                }
            }
            StatementKind::UsingDeclaration(declaration) => {
                for declarator in &declaration.declarations {
                    for_each_bound_name(&declarator.target, &mut |name| {
                        lexical_bindings.push(LexicalBinding {
                            name: name.into(),
                            is_constant: true,
                        });
                    });
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    lexical_bindings.push(LexicalBinding {
                        name: name.text().into(),
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
        annex_b_candidate_names: scope
            .annexb_function_names()
            .iter()
            .map(|&name| tree.name_text(name).into())
            .collect(),
        lexical_bindings,
    }
}

// =============================================================================
// SFD metadata computation (ECMA-262 section 10.2.11)
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::stack_limit_for_budget;

    const SOURCE: &str = "function outer() { return function inner() { return 1; }; }";

    fn compile(source: &str) -> CompiledProgram {
        compile_parsed_program_off_thread(parse(&Utf16String::from_utf8(source), ProgramType::Script, 1))
    }

    fn first_payload(program: &CompiledProgram) -> Arc<FunctionPayload> {
        let script = program.clone().into_script();
        script.declarations.functions_to_initialize[0]
            .shared_function_data
            .description
            .payload
            .clone()
    }

    fn compile_with_enough_stack(
        payload: &FunctionPayload,
        builtin_abstract_operations_enabled: bool,
    ) -> bytecode::executable_data::PrecompiledFunction {
        compile_function(
            payload,
            builtin_abstract_operations_enabled,
            stack_limit_for_budget(1 << 23),
        )
        .ok()
        .expect("the stack holds the function")
    }

    fn precompiled_functions(source: &str) -> Vec<bool> {
        compile(source)
            .executable
            .shared_function_data
            .iter()
            .map(|function| function.precompiled_function.is_some())
            .collect()
    }

    #[test]
    fn functions_that_run_right_away_compile_with_the_program() {
        // NB: The stack of a test is too small for the frames of the parser of a build without optimizations.
        std::thread::Builder::new()
            .stack_size(1 << 25)
            .spawn(functions_that_run_right_away_compile_with_enough_stack)
            .expect("a thread starts")
            .join()
            .expect("the checks pass");
    }

    fn functions_that_run_right_away_compile_with_enough_stack() {
        for source in [
            "(function () {})();",
            "(function () {}());",
            "!function () {}();",
            "((function () {}))();",
            "(function () {}).call(this);",
            "(function () {}.call(this));",
            "((function () {})).call(this);",
            "!function () {}.call(this);",
            "(function () {}).apply(this, []);",
            "(function () {}.apply(this, []));",
            "(() => {})();",
            "(x => x)(1);",
            "((a, b) => {})(1, 2);",
            "((a = 1) => {})();",
            "((() => {}))();",
            "!(() => {})();",
            "var result = (() => {})();",
            "(() => {}).call(this);",
            "(async () => {})();",
            "(async x => x)();",
            "(async (a, b) => {})();",
            "(async function () {})();",
        ] {
            assert_eq!(precompiled_functions(source), [true], "{source}");
        }
    }

    #[test]
    fn other_functions_wait_for_their_first_call() {
        for source in [
            "(function () {});",
            "(function () {}).bind(this);",
            "(function () {}).call;",
            "(function () {})[call](this);",
            "(function () {}).call.call(this);",
            "new (function () {})();",
            "f(function () {});",
            "var f = function () {};",
            "f.call(function () {});",
            "(() => {});",
            "f(() => {});",
            "f((a) => {});",
            "f(x => x);",
            "f(() => {})();",
            "var f = () => {};",
            "[() => {}];",
        ] {
            assert_eq!(precompiled_functions(source), [false], "{source}");
        }
    }

    #[test]
    fn clones_share_the_code_of_a_program() {
        let program = compile(SOURCE);
        let first = program.clone().into_script();
        let second = program.into_script();
        assert!(Arc::ptr_eq(&first.executable, &second.executable));
        assert!(Arc::ptr_eq(&first.declarations, &second.declarations));
    }

    #[test]
    fn a_retaining_script_compiles_a_function_once() {
        let program = compile(SOURCE);
        program.set_retains_compiled_functions(true);
        let payload = first_payload(&program);
        let first = compile_with_enough_stack(&payload, false);
        let second = compile_with_enough_stack(&payload, false);
        assert!(Arc::ptr_eq(&first.executable, &second.executable));
    }

    #[test]
    fn other_scripts_keep_nothing() {
        let program = compile(SOURCE);
        let size = program.memory_size();
        let payload = first_payload(&program);
        let first = compile_with_enough_stack(&payload, false);
        let second = compile_with_enough_stack(&payload, false);
        assert!(!Arc::ptr_eq(&first.executable, &second.executable));
        assert!(payload.compiled.get().is_none());
        assert_eq!(program.memory_size(), size);
    }

    #[test]
    fn builtin_code_is_not_kept() {
        let program = compile(SOURCE);
        program.set_retains_compiled_functions(true);
        let payload = first_payload(&program);
        compile_with_enough_stack(&payload, true);
        assert!(payload.compiled.get().is_none());
    }

    #[test]
    fn running_out_of_stack_is_not_kept() {
        let program = compile(SOURCE);
        program.set_retains_compiled_functions(true);
        let payload = first_payload(&program);
        assert!(compile_function(&payload, false, usize::MAX).is_err());
        assert!(payload.compiled.get().is_none());
        compile_with_enough_stack(&payload, false);
        assert!(payload.compiled.get().is_some());
    }

    #[test]
    fn code_of_an_eval_does_not_keep_its_script_alive() {
        let parsed = parse_eval(&Utf16String::from_utf8("1 + 1"), EvalContext::default())
            .ok()
            .expect("the source parses");
        let script = Arc::downgrade(&parsed.tree.script);
        let _compiled = compile_eval(parsed);
        assert!(script.upgrade().is_none());
    }

    #[test]
    fn memory_size_grows_with_the_code_kept() {
        let program = compile(SOURCE);
        program.set_retains_compiled_functions(true);
        let size = program.memory_size();
        let payload = first_payload(&program);
        let compiled = compile_with_enough_stack(&payload, false);
        assert_eq!(program.memory_size(), size + compiled.memory_size());
    }
}
