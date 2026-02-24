/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Scope analysis for the parser.
//!
//! This is a two-phase system:
//!
//! ## Phase 1: Build scope tree (during parsing)
//!
//! As the parser encounters scopes (functions, blocks, for-loops, etc.),
//! it calls `open_*_scope()` to push a `ScopeRecord` onto the tree, and
//! the scope is closed when parsing of the construct finishes. During
//! parsing, declarations and identifier references are registered:
//!
//! - `add_var_declaration()` — `var` bindings (hoist to function scope)
//! - `add_lexical_declaration()` — `let`/`const` (block-scoped)
//! - `add_function_declaration()` — function declarations (may Annex-B hoist)
//! - `register_identifier()` — any identifier reference
//!
//! ## Phase 2: Analyze (after parsing)
//!
//! `analyze()` walks the scope tree bottom-up and for each scope:
//!
//! 1. **Resolves identifiers**: matches identifier references to their
//!    declarations (var, let/const, function, parameter, catch binding).
//!    Unresolved identifiers are marked as global.
//!
//! 2. **Propagates eval poisoning**: if a scope contains a direct call
//!    to `eval()`, all ancestor scopes must know (they can't optimize
//!    away their environment records).
//!
//! 3. **Hoists functions (Annex B)**: in non-strict sloppy mode,
//!    function declarations inside blocks can create `var` bindings
//!    in the enclosing function scope.
//!
//! 4. **Builds local variable lists**: populates `FunctionScopeData`
//!    on AST ScopeData nodes, enabling the bytecode generator
//!    to use indexed locals.
//!
//! ## Key data structures
//!
//! - `ScopeRecord` — one scope (function, block, etc.) with its
//!   variables, identifiers, and child scopes
//! - `ScopeVariable` — a declared name within a scope (flags track
//!   whether it's var/lexical/function/catch/parameter)
//! - `IdentifierGroup` — a set of identifier references with the same
//!   name within one scope (multiple `foo` refs are grouped together)

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use crate::ast::{
    FunctionScopeData, Identifier, LocalBinding, LocalVarKind, LocalVariable, ScopeData,
    Utf16String, VarToInit,
};
use crate::parser::{DeclarationKind, FunctionKind, ProgramType};
use crate::u32_from_usize;

// === Enums ===

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ScopeType {
    Function,
    Program,
    Block,
    ForLoop,
    With,
    Catch,
    ClassStaticInit,
    ClassField,
    ClassDeclaration,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ScopeLevel {
    NotTopLevel,
    ScriptTopLevel,
    ModuleTopLevel,
    FunctionTopLevel,
    StaticInitTopLevel,
}

impl ScopeLevel {
    fn is_top_level(self) -> bool {
        self != ScopeLevel::NotTopLevel
    }
}

// === Variable flags ===
// Bit flags on ScopeVariable that track how a name was declared.
// A single name can accumulate multiple flags (e.g., a `var` that
// shadows a parameter gets both VAR and FORBIDDEN_LEXICAL).

#[derive(Clone, Copy, Debug, Default)]
struct VarFlags(u16);

impl VarFlags {
    const EMPTY: Self = Self(0);
    const VAR: Self = Self(1 << 0);
    const LEXICAL: Self = Self(1 << 1);
    const FUNCTION: Self = Self(1 << 2);
    const CATCH_PARAMETER: Self = Self(1 << 3);
    const FORBIDDEN_LEXICAL: Self = Self(1 << 4);
    const FORBIDDEN_VAR: Self = Self(1 << 5);
    const BOUND: Self = Self(1 << 6);
    const PARAMETER_CANDIDATE: Self = Self(1 << 7);
    const REFERENCED_IN_FORMAL_PARAMETERS: Self = Self(1 << 8);

    const fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }
}

impl std::ops::BitOr for VarFlags {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl std::ops::BitOrAssign for VarFlags {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

// === Data structures ===

/// A declared name within a scope. Multiple declaration forms can share
/// the same name (e.g., `var x` and `function x`), so flags are ORed together.
#[derive(Debug, Default)]
struct ScopeVariable {
    /// Bit flags describing how this name was declared.
    flags: VarFlags,
    /// The Identifier AST node for the `var` declaration (used to build
    /// FunctionScopeData). None if not a var.
    var_identifier: Option<Rc<Identifier>>,
}

/// Groups all Identifier AST nodes that share the same name within a scope.
/// During analysis, the group is resolved to a local variable, parameter,
/// or propagated to the parent scope if unresolved.
#[derive(Debug)]
struct IdentifierGroup {
    /// True if any identifier in this group is referenced from a nested
    /// function (prevents local variable optimization).
    captured_by_nested_function: bool,
    /// True if any identifier in this group is inside a `with` block
    /// (prevents local variable optimization since `with` can shadow anything).
    used_inside_with_statement: bool,
    /// All Identifier AST nodes with this name in this scope.
    identifiers: Vec<Rc<Identifier>>,
    /// If this name was declared (var/let/const), tracks the declaration kind
    /// so we can annotate each Identifier AST node.
    declaration_kind: Option<DeclarationKind>,
}

/// A function to hoist via Annex B.3.3.
#[derive(Debug)]
struct HoistableFunction {
    name: Utf16String,
    /// Reference to the block ScopeData that contains the function declaration.
    /// Used to set `is_hoisted = true` on the FunctionData when it's hoisted.
    block_scope_data: Option<Rc<RefCell<ScopeData>>>,
}

#[derive(Debug)]
struct ParameterName {
    name: Utf16String,
    is_rest: bool,
}

/// Entry describing a single parameter binding for scope analysis.
pub struct ParameterEntry {
    pub name: Utf16String,
    pub identifier: Option<Rc<Identifier>>,
    pub is_rest: bool,
    pub is_from_pattern: bool,
    pub is_first_from_pattern: bool,
}

#[derive(Debug)]
struct ScopeRecord {
    scope_type: ScopeType,
    scope_level: ScopeLevel,
    scope_data: Option<Rc<RefCell<ScopeData>>>,

    variables: HashMap<Utf16String, ScopeVariable>,
    identifier_groups: HashMap<Utf16String, IdentifierGroup>,
    functions_to_hoist: Vec<HoistableFunction>,

    // Parameter tracking
    has_function_parameters: bool,
    parameter_names: Vec<ParameterName>,

    // Flags
    contains_access_to_arguments_object_in_non_strict_mode: bool,
    contains_direct_call_to_eval: bool,
    contains_await_expression: bool,
    poisoned_by_eval_in_scope_chain: bool,
    eval_in_current_function: bool,
    uses_this_from_environment: bool,
    uses_this: bool,
    is_arrow_function: bool,
    is_function_declaration: bool,
    has_parameter_expressions: bool,

    // Tree (indices into ScopeCollector::records)
    parent: Option<usize>,
    top_level: Option<usize>,
    children: Vec<usize>,
}

impl ScopeRecord {
    fn new(
        scope_type: ScopeType,
        scope_level: ScopeLevel,
        scope_data: Option<Rc<RefCell<ScopeData>>>,
    ) -> Self {
        Self {
            scope_type,
            scope_level,
            scope_data,
            variables: HashMap::new(),
            identifier_groups: HashMap::new(),
            functions_to_hoist: Vec::new(),
            has_function_parameters: false,
            parameter_names: Vec::new(),
            contains_access_to_arguments_object_in_non_strict_mode: false,
            contains_direct_call_to_eval: false,
            contains_await_expression: false,
            poisoned_by_eval_in_scope_chain: false,
            eval_in_current_function: false,
            uses_this_from_environment: false,
            uses_this: false,
            is_arrow_function: false,
            is_function_declaration: false,
            has_parameter_expressions: false,
            parent: None,
            top_level: None,
            children: Vec::new(),
        }
    }

    fn is_top_level(&self) -> bool {
        self.scope_level.is_top_level()
    }

    fn variable(&mut self, name: &[u16]) -> &mut ScopeVariable {
        self.variables.entry(Utf16String::from(name)).or_default()
    }

    fn has_flag(&self, name: &[u16], flags: VarFlags) -> bool {
        self.variables
            .get(name)
            .is_some_and(|v| v.flags.intersects(flags))
    }

    fn get_parameter_index(&self, name: &[u16]) -> Option<u32> {
        // Iterate backwards to return the last parameter with the same name,
        // matching the semantics of duplicate parameter names in non-strict mode.
        for (i, param) in self.parameter_names.iter().enumerate().rev() {
            if param.name == name {
                return Some(u32_from_usize(i));
            }
        }
        None
    }

    fn has_rest_parameter_with_name(&self, name: &[u16]) -> bool {
        self.parameter_names
            .iter()
            .any(|param| param.is_rest && param.name == name)
    }

    fn has_hoistable_function_named(&self, name: &[u16]) -> bool {
        self.functions_to_hoist.iter().any(|f| f.name == name)
    }
}

/// Walk ancestor scopes starting from `start` (inclusive), following parent links.
fn ancestor_scopes(start: usize, records: &[ScopeRecord]) -> impl Iterator<Item = usize> + '_ {
    std::iter::successors(Some(start), move |&i| records[i].parent)
}

fn last_function_scope(index: usize, records: &[ScopeRecord]) -> Option<usize> {
    ancestor_scopes(index, records).find(|&i| {
        records[i].scope_type == ScopeType::Function
            || records[i].scope_type == ScopeType::ClassStaticInit
    })
}

// === ScopeCollector ===

pub struct ScopeError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

/// Saved flags for a scope record, used to restore state after
/// speculative parsing (e.g. failed arrow function attempts).
struct SavedScopeFlags {
    index: usize,
    uses_this: bool,
    uses_this_from_environment: bool,
}

pub struct ScopeCollectorState {
    records_len: usize,
    current: Option<usize>,
    errors_len: usize,
    /// Snapshot of propagatable flags on existing scope records.
    /// During speculative parsing, set_uses_this() and set_uses_new_target()
    /// propagate flags to ancestor function scopes. These changes must be
    /// undone if the speculative parse fails.
    saved_flags: Vec<SavedScopeFlags>,
}

pub struct ScopeCollector {
    records: Vec<ScopeRecord>,
    current: Option<usize>,
    errors: Vec<ScopeError>,
}

impl Default for ScopeCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl ScopeCollector {
    pub fn new() -> Self {
        Self {
            records: Vec::new(),
            current: None,
            errors: Vec::new(),
        }
    }

    pub fn drain_errors(&mut self) -> Vec<ScopeError> {
        std::mem::take(&mut self.errors)
    }

    fn already_declared_error(&mut self, name: &[u16], line: u32, column: u32) {
        self.errors.push(ScopeError {
            message: format!(
                "Identifier '{}' already declared",
                String::from_utf16_lossy(name)
            ),
            line,
            column,
        });
    }

    pub fn has_errors(&self) -> bool {
        !self.errors.is_empty()
    }

    pub fn has_current_scope(&self) -> bool {
        self.current.is_some()
    }

    pub fn save_state(&self) -> ScopeCollectorState {
        // Snapshot flags on ancestor function scopes that set_uses_this()
        // and set_uses_new_target() might modify during speculative parsing.
        let mut saved_flags = Vec::new();
        if let Some(start) = self.current {
            for index in ancestor_scopes(start, &self.records) {
                if self.records[index].scope_type == ScopeType::Function {
                    saved_flags.push(SavedScopeFlags {
                        index,
                        uses_this: self.records[index].uses_this,
                        uses_this_from_environment: self.records[index].uses_this_from_environment,
                    });
                }
            }
        }
        ScopeCollectorState {
            records_len: self.records.len(),
            current: self.current,
            errors_len: self.errors.len(),
            saved_flags,
        }
    }

    pub fn load_state(&mut self, state: ScopeCollectorState) {
        let saved_len = state.records_len;
        self.records.truncate(saved_len);
        self.current = state.current;
        self.errors.truncate(state.errors_len);
        // Remove any child indices that pointed to now-truncated records.
        if let Some(current_index) = self.current {
            self.records[current_index]
                .children
                .retain(|&c| c < saved_len);
        }
        // Restore flags on ancestor function scopes that may have been
        // modified by set_uses_this() or set_uses_new_target() during
        // the speculative parse.
        for saved in &state.saved_flags {
            if saved.index < self.records.len() {
                self.records[saved.index].uses_this = saved.uses_this;
                self.records[saved.index].uses_this_from_environment =
                    saved.uses_this_from_environment;
            }
        }
    }

    // === Open/close scopes ===

    fn open_scope(
        &mut self,
        scope_type: ScopeType,
        scope_data: Option<Rc<RefCell<ScopeData>>>,
        scope_level: ScopeLevel,
    ) {
        let index = self.records.len();
        let mut record = ScopeRecord::new(scope_type, scope_level, scope_data);
        record.parent = self.current;

        if scope_type != ScopeType::Function
            && record.scope_data.is_none()
            && let Some(parent_index) = self.current
        {
            record.scope_data = self.records[parent_index].scope_data.clone();
        }

        if scope_level == ScopeLevel::NotTopLevel {
            if let Some(parent_index) = self.current {
                record.top_level = self.records[parent_index].top_level;
            }
        } else {
            record.top_level = Some(index);
        }

        self.records.push(record);
        if let Some(parent_index) = self.current {
            self.records[parent_index].children.push(index);
        }
        self.current = Some(index);
    }

    pub fn close_scope(&mut self) {
        let index = self.current.expect("close_scope with no current scope");

        if let Some(parent_index) = self.records[index].parent
            && !self.records[index].has_function_parameters
        {
            let c = &self.records[index];
            let arguments = c.contains_access_to_arguments_object_in_non_strict_mode;
            let eval = c.contains_direct_call_to_eval;
            let contains_await = c.contains_await_expression;
            self.records[parent_index].contains_access_to_arguments_object_in_non_strict_mode |=
                arguments;
            self.records[parent_index].contains_direct_call_to_eval |= eval;
            self.records[parent_index].contains_await_expression |= contains_await;
        }

        self.current = self.records[index].parent;
    }

    pub fn open_program_scope(&mut self, program_type: ProgramType) {
        let level = if program_type == ProgramType::Script {
            ScopeLevel::ScriptTopLevel
        } else {
            ScopeLevel::ModuleTopLevel
        };
        self.open_scope(ScopeType::Program, None, level);
    }

    pub fn open_function_scope(&mut self, function_name: Option<&[u16]>) {
        self.open_scope(ScopeType::Function, None, ScopeLevel::FunctionTopLevel);
        if let Some(name) = function_name {
            let index = self.current.expect("no current scope");
            self.records[index].variable(name).flags |= VarFlags::BOUND;
        }
    }

    pub fn open_block_scope(&mut self, scope_data: Option<Rc<RefCell<ScopeData>>>) {
        self.open_scope(ScopeType::Block, scope_data, ScopeLevel::NotTopLevel);
    }

    pub fn open_for_loop_scope(&mut self, scope_data: Option<Rc<RefCell<ScopeData>>>) {
        self.open_scope(ScopeType::ForLoop, scope_data, ScopeLevel::NotTopLevel);
    }

    // https://tc39.es/ecma262/#sec-with-statement
    // The `with` statement creates an object environment record that intercepts
    // identifier lookups, preventing any local variable optimization for
    // identifiers used within its scope.
    pub fn open_with_scope(&mut self, scope_data: Option<Rc<RefCell<ScopeData>>>) {
        self.open_scope(ScopeType::With, scope_data, ScopeLevel::NotTopLevel);
    }

    pub fn open_catch_scope(&mut self) {
        self.open_scope(ScopeType::Catch, None, ScopeLevel::NotTopLevel);
    }

    pub fn open_static_init_scope(&mut self, scope_data: Option<Rc<RefCell<ScopeData>>>) {
        self.open_scope(
            ScopeType::ClassStaticInit,
            scope_data,
            ScopeLevel::StaticInitTopLevel,
        );
    }

    pub fn open_class_field_scope(&mut self, scope_data: Option<Rc<RefCell<ScopeData>>>) {
        self.open_scope(ScopeType::ClassField, scope_data, ScopeLevel::NotTopLevel);
    }

    pub fn open_class_declaration_scope(&mut self, class_name: Option<&[u16]>) {
        self.open_scope(ScopeType::ClassDeclaration, None, ScopeLevel::NotTopLevel);
        if let Some(name) = class_name {
            let index = self.current.expect("no current scope");
            self.records[index].variable(name).flags |= VarFlags::BOUND;
        }
    }

    // === Declaration registration ===

    // https://tc39.es/ecma262/#sec-let-and-const-declarations
    // https://tc39.es/ecma262/#sec-block-level-function-declarations-web-legacy-compatibility-semantics
    // Lexical declarations (let/const) are block-scoped and must not collide
    // with any existing var, function, or lexical binding in the same scope.
    pub fn add_lexical_declaration(
        &mut self,
        bound_names: &[&[u16]],
        declaration_line: u32,
        declaration_column: u32,
    ) {
        let index = self.current.expect("no current scope");

        for name in bound_names {
            let flags = self.records[index].variable(name).flags;
            if flags.intersects(
                VarFlags::VAR
                    | VarFlags::FORBIDDEN_LEXICAL
                    | VarFlags::FUNCTION
                    | VarFlags::LEXICAL,
            ) {
                self.already_declared_error(name, declaration_line, declaration_column);
            }
            self.records[index].variable(name).flags |= VarFlags::LEXICAL;
        }
    }

    // https://tc39.es/ecma262/#sec-variable-statement
    // `var` declarations hoist to the enclosing function or script scope.
    // They walk the scope chain upward, registering in every scope along
    // the way until reaching a top-level scope (function, program, or
    // class static initializer).
    pub fn add_var_declaration(
        &mut self,
        bound_names: &[(&[u16], Option<Rc<Identifier>>)],
        declaration_line: u32,
        declaration_column: u32,
        declaration_kind: Option<DeclarationKind>,
    ) {
        let index = self.current.expect("no current scope");

        for (name, identifier) in bound_names {
            // Register the declaration identifier so it participates in scope analysis.
            if let Some(id) = identifier {
                self.register_identifier(id.clone(), name, declaration_kind);
            }

            let mut scope_index = index;
            loop {
                let existing_flags = self.records[scope_index].variable(name).flags;
                if existing_flags
                    .intersects(VarFlags::LEXICAL | VarFlags::FUNCTION | VarFlags::FORBIDDEN_VAR)
                {
                    self.already_declared_error(name, declaration_line, declaration_column);
                }
                let var = self.records[scope_index].variable(name);
                var.flags |= VarFlags::VAR;
                var.var_identifier = identifier.clone();
                if self.records[scope_index].is_top_level() {
                    break;
                }
                scope_index = self.records[scope_index]
                    .parent
                    .expect("scope has no parent");
            }
        }
    }

    // https://tc39.es/ecma262/#sec-function-definitions
    // https://tc39.es/ecma262/#sec-block-level-function-declarations-web-legacy-compatibility-semantics
    // At top level (function/script scope), function declarations act like `var`.
    // At block level in sloppy mode, Annex B.3.3 allows hoisting the name as
    // a `var` binding to the enclosing function scope.
    // In strict mode (or for async/generator functions), block-scoped function
    // declarations are treated as lexical bindings and are NOT Annex-B hoisted.
    pub fn add_function_declaration(
        &mut self,
        name: &[u16],
        name_identifier: Option<Rc<Identifier>>,
        function_kind: FunctionKind,
        strict_mode: bool,
        declaration_line: u32,
        declaration_column: u32,
    ) {
        let index = self.current.expect("no current scope");
        let scope_level = self.records[index].scope_level;

        // Register the name identifier so it participates in scope analysis.
        if let Some(ref id) = name_identifier {
            self.register_identifier(id.clone(), name, None);
        }

        if scope_level != ScopeLevel::NotTopLevel && scope_level != ScopeLevel::ModuleTopLevel {
            let var = self.records[index].variable(name);
            var.flags |= VarFlags::VAR;
            var.var_identifier = name_identifier.clone();
        } else {
            // Check flags first, then modify. This avoids borrow checker issues
            // since we need to access both variables and functions_to_hoist.
            let existing_flags = self.records[index]
                .variables
                .get(name)
                .map_or(VarFlags::EMPTY, |v| v.flags);

            if existing_flags.intersects(VarFlags::VAR | VarFlags::LEXICAL) {
                self.already_declared_error(name, declaration_line, declaration_column);
            }

            if function_kind != FunctionKind::Normal || strict_mode {
                if existing_flags.intersects(VarFlags::FUNCTION) {
                    self.already_declared_error(name, declaration_line, declaration_column);
                }
                self.records[index].variable(name).flags |= VarFlags::LEXICAL;
                return;
            }

            if !existing_flags.intersects(VarFlags::LEXICAL) {
                let block_scope = self.records[index].scope_data.clone();
                self.records[index]
                    .functions_to_hoist
                    .push(HoistableFunction {
                        name: Utf16String::from(name),
                        block_scope_data: block_scope,
                    });
            }

            let var = self.records[index].variable(name);
            var.flags |= VarFlags::FUNCTION;
        }
    }

    // https://tc39.es/ecma262/#sec-try-statement
    // Catch clause parameters create a new scope. The bound names in a
    // catch pattern forbid `var` declarations with the same name in the
    // catch block (unlike regular block-scoped variables).
    pub fn add_catch_parameter_pattern(&mut self, bound_names: &[&[u16]]) {
        let index = self.current.expect("no current scope");
        for name in bound_names {
            let var = self.records[index].variable(name);
            var.flags |= VarFlags::FORBIDDEN_VAR | VarFlags::BOUND | VarFlags::CATCH_PARAMETER;
        }
    }

    pub fn add_catch_parameter_identifier(&mut self, name: &[u16], identifier: Rc<Identifier>) {
        let index = self.current.expect("no current scope");
        let var = self.records[index].variable(name);
        var.flags |= VarFlags::VAR | VarFlags::BOUND | VarFlags::CATCH_PARAMETER;
        var.var_identifier = Some(identifier);
    }

    // === Identifier registration ===

    pub fn register_identifier(
        &mut self,
        id: Rc<Identifier>,
        name: &[u16],
        declaration_kind: Option<DeclarationKind>,
    ) {
        let index = self.current.expect("no current scope");
        self.records[index]
            .identifier_groups
            .entry(Utf16String::from(name))
            .and_modify(|group| {
                group.identifiers.push(id.clone());
                if declaration_kind.is_some() && group.declaration_kind.is_none() {
                    group.declaration_kind = declaration_kind;
                }
            })
            .or_insert_with(|| IdentifierGroup {
                captured_by_nested_function: false,
                used_inside_with_statement: false,
                identifiers: vec![id],
                declaration_kind,
            });
    }

    // === Function parameters ===

    pub fn set_function_parameters(
        &mut self,
        entries: &[ParameterEntry],
        has_parameter_expressions: bool,
    ) {
        let index = self.current.expect("no current scope");
        self.records[index].has_function_parameters = true;
        self.records[index].has_parameter_expressions = has_parameter_expressions;

        for entry in entries {
            if entry.is_from_pattern {
                if entry.is_first_from_pattern {
                    // Placeholder for a pattern parameter — push an empty
                    // entry so subsequent parameters get the correct
                    // positional index. Don't register anything else.
                    self.records[index].parameter_names.push(ParameterName {
                        name: Utf16String::default(),
                        is_rest: false,
                    });
                    continue;
                }
            } else {
                self.records[index].parameter_names.push(ParameterName {
                    name: entry.name.clone(),
                    is_rest: entry.is_rest,
                });
            }
            if let Some(ref id) = entry.identifier {
                self.register_identifier(id.clone(), &entry.name, None);
            }
            let var = self.records[index]
                .variables
                .entry(entry.name.clone())
                .or_default();
            var.flags |= VarFlags::PARAMETER_CANDIDATE | VarFlags::FORBIDDEN_LEXICAL;
        }

        // Mark non-parameter names that were referenced during formal parameter
        // parsing (i.e. in default value expressions). If a body var later
        // declares the same name, it must not be optimized to a local, since the
        // default expression needs to resolve it from the outer scope.
        if has_parameter_expressions {
            let names_to_mark: Vec<Utf16String> = self.records[index]
                .identifier_groups
                .keys()
                .filter(|name| !self.records[index].has_flag(name, VarFlags::FORBIDDEN_LEXICAL))
                .cloned()
                .collect();
            for name in names_to_mark {
                self.records[index].variable(&name).flags |=
                    VarFlags::REFERENCED_IN_FORMAL_PARAMETERS;
            }
        }
    }

    // === Scope node ===

    pub fn set_scope_node(&mut self, scope_data: Rc<RefCell<ScopeData>>) {
        let index = self.current.expect("no current scope");
        self.records[index].scope_data = Some(scope_data.clone());
        // Update block_scope_data for any pending functions_to_hoist that
        // were registered before the ScopeData was created.
        for function in &mut self.records[index].functions_to_hoist {
            if function.block_scope_data.is_none() {
                function.block_scope_data = Some(scope_data.clone());
            }
        }
    }

    // === Flag setters ===

    // https://tc39.es/ecma262/#sec-function-calls-runtime-semantics-evaluation
    // A direct call to eval (identifier `eval` as callee) can introduce new
    // bindings at runtime, so the entire scope chain must retain its environment
    // records (no local variable optimization through eval-poisoned scopes).
    pub fn set_contains_direct_call_to_eval(&mut self) {
        let index = self.current.expect("no current scope");
        self.records[index].contains_direct_call_to_eval = true;
        self.records[index].poisoned_by_eval_in_scope_chain = true;
        self.records[index].eval_in_current_function = true;
    }

    pub fn set_contains_access_to_arguments_object_in_non_strict_mode(&mut self) {
        let index = self.current.expect("no current scope");
        self.records[index].contains_access_to_arguments_object_in_non_strict_mode = true;
    }

    pub fn set_contains_await_expression(&mut self) {
        let index = self.current.expect("no current scope");
        self.records[index].contains_await_expression = true;
    }

    // https://tc39.es/ecma262/#sec-arrow-function-definitions-runtime-semantics-evaluation
    // Arrow functions don't have their own `this`; they inherit it from the
    // enclosing lexical environment (uses_this_from_environment).
    pub fn set_uses_this(&mut self) {
        let index = self.current.expect("no current scope");
        let closest_fn = last_function_scope(index, &self.records);
        let this_from_env = closest_fn.is_some_and(|fi| self.records[fi].is_arrow_function);

        // NB: collect() is needed because ancestor_scopes borrows self.records
        //     immutably, but the loop body needs mutable access.
        for si in ancestor_scopes(index, &self.records).collect::<Vec<_>>() {
            if self.records[si].scope_type == ScopeType::Function {
                self.records[si].uses_this = true;
                if this_from_env {
                    self.records[si].uses_this_from_environment = true;
                }
            }
        }
    }

    pub fn set_uses_new_target(&mut self) {
        let index = self.current.expect("no current scope");
        // NB: collect() for the same reason as set_uses_this above.
        for si in ancestor_scopes(index, &self.records).collect::<Vec<_>>() {
            if self.records[si].scope_type == ScopeType::Function {
                self.records[si].uses_this = true;
                self.records[si].uses_this_from_environment = true;
            }
        }
    }

    pub fn set_is_arrow_function(&mut self) {
        let index = self.current.expect("no current scope");
        self.records[index].is_arrow_function = true;
    }

    pub fn set_is_function_declaration(&mut self) {
        let index = self.current.expect("no current scope");
        self.records[index].is_function_declaration = true;
    }

    // === Getters ===

    pub fn contains_direct_call_to_eval(&self) -> bool {
        self.current
            .is_some_and(|index| self.records[index].contains_direct_call_to_eval)
    }

    pub fn uses_this_from_environment(&self) -> bool {
        self.current
            .is_some_and(|index| self.records[index].uses_this_from_environment)
    }

    pub fn uses_this(&self) -> bool {
        self.current
            .is_some_and(|index| self.records[index].uses_this)
    }

    pub fn contains_await_expression(&self) -> bool {
        self.current
            .is_some_and(|index| self.records[index].contains_await_expression)
    }

    pub fn scope_type(&self) -> Option<ScopeType> {
        self.current.map(|index| self.records[index].scope_type)
    }

    pub fn can_have_using_declaration(&self) -> bool {
        self.current
            .is_some_and(|index| self.records[index].scope_level != ScopeLevel::ScriptTopLevel)
    }

    pub fn has_declaration(&self, name: &[u16]) -> bool {
        if let Some(index) = self.current {
            if self.records[index].has_flag(name, VarFlags::LEXICAL | VarFlags::VAR) {
                return true;
            }
            return self.records[index].has_hoistable_function_named(name);
        }
        false
    }

    pub fn has_declaration_in_current_function(&self, name: &[u16]) -> bool {
        let Some(index) = self.current else {
            return false;
        };
        let fn_scope = last_function_scope(index, &self.records);
        let stop = fn_scope.and_then(|fi| self.records[fi].parent);
        for si in ancestor_scopes(index, &self.records) {
            if Some(si) == stop {
                break;
            }
            if self.records[si].has_flag(
                name,
                VarFlags::LEXICAL | VarFlags::VAR | VarFlags::PARAMETER_CANDIDATE,
            ) {
                return true;
            }
            if self.records[si].has_hoistable_function_named(name) {
                return true;
            }
        }
        false
    }

    // === Post-parse analysis ===

    pub fn analyze(&mut self, initiated_by_eval: bool) {
        self.analyze_inner(initiated_by_eval, false);
    }

    /// Like analyze(), but suppresses marking identifiers as global.
    /// Used for dynamic functions (new Function(...)) where the source is
    /// parsed as a Script but identifiers must not use GetGlobal/SetGlobal,
    /// matching the C++ path which parses as a FunctionExpression.
    pub fn analyze_as_dynamic_function(&mut self) {
        self.analyze_inner(false, true);
    }

    fn analyze_inner(&mut self, initiated_by_eval: bool, suppress_globals: bool) {
        if !self.records.is_empty() {
            self.analyze_recursive(0, initiated_by_eval, suppress_globals);
        }
    }

    /// Analyze a scope and all its descendants, bottom-up.
    /// Children are analyzed first so that unresolved identifiers bubble up
    /// to their parent, and eval poisoning propagates outward.
    fn analyze_recursive(&mut self, index: usize, initiated_by_eval: bool, suppress_globals: bool) {
        // Process children first (bottom-up traversal).
        let children = std::mem::take(&mut self.records[index].children);
        for child_index in children {
            self.analyze_recursive(child_index, initiated_by_eval, suppress_globals);
        }

        // Steps 1-3 must run even for scopes without scope_data (e.g. catch
        // scopes), so that identifier groups propagate through the scope chain.
        // Without this, captured variables inside catch blocks are invisible
        // to enclosing scopes and get incorrectly optimized as locals.

        // 1. Propagate eval() flags from children to parent.
        Self::propagate_eval_poisoning(&mut self.records, index);
        // 2. Match identifier references to declarations; optimize as locals.
        Self::resolve_identifiers(
            &mut self.records,
            index,
            initiated_by_eval,
            suppress_globals,
        );
        // 3. Annex B: hoist block-scoped functions to enclosing function scope.
        Self::hoist_functions(&mut self.records, index);

        // 4. For function-like scopes, build the var declaration list that
        //    the bytecode generator uses to initialize function-scoped variables.
        if self.records[index].scope_data.is_some() {
            let st = self.records[index].scope_type;
            let needs_fsd = (st == ScopeType::Function
                && self.records[index].has_function_parameters)
                || st == ScopeType::ClassStaticInit
                || st == ScopeType::ClassField;
            if needs_fsd {
                Self::build_function_scope_data(&self.records, index);
            }
        }
    }

    /// Propagate eval-related flags from a child scope to its parent.
    ///
    /// Three separate flags track eval impact:
    /// - `contains_direct_call_to_eval`: this scope itself has `eval()`
    /// - `poisoned_by_eval_in_scope_chain`: some descendant has eval, so
    ///   this scope can't optimize away its environment record
    /// - `eval_in_current_function`: eval exists somewhere in the current
    ///   function (propagates through blocks but stops at function boundaries)
    fn propagate_eval_poisoning(records: &mut [ScopeRecord], index: usize) {
        if let Some(parent_index) = records[index].parent {
            if records[index].contains_direct_call_to_eval
                || records[index].poisoned_by_eval_in_scope_chain
            {
                records[parent_index].poisoned_by_eval_in_scope_chain = true;
            }
            // eval_in_current_function propagates upward through blocks but
            // stops at function boundaries (each function is independent).
            if records[index].eval_in_current_function
                && records[index].scope_type != ScopeType::Function
            {
                records[parent_index].eval_in_current_function = true;
            }
        }
    }

    /// Try to resolve each identifier group in this scope to a local variable.
    ///
    /// For each named group, this function:
    /// 1. Annotates identifiers with their declaration kind (var/let/const)
    /// 2. Determines if the name can be optimized to a local variable index
    /// 3. If not resolvable here, propagates the group to the parent scope
    ///
    /// An identifier is optimized to a local when:
    /// - It's declared in this scope (var, let/const, function, parameter, catch)
    /// - It's NOT captured by a nested function
    /// - It's NOT used inside a `with` statement
    /// - The scope chain is NOT poisoned by `eval()`
    fn resolve_identifiers(
        records: &mut [ScopeRecord],
        index: usize,
        initiated_by_eval: bool,
        suppress_globals: bool,
    ) {
        let groups = std::mem::take(&mut records[index].identifier_groups);
        // Sort groups by name for deterministic local variable indices
        // (HashMap iteration order is arbitrary).
        let mut sorted_groups: Vec<_> = groups.into_iter().collect();
        sorted_groups.sort_by(|a, b| a.0.cmp(&b.0));
        let mut propagate_to_parent: Vec<(Utf16String, IdentifierGroup)> = Vec::new();
        for (name, mut group) in sorted_groups {
            // Annotate each Identifier AST node with its declaration kind,
            // so the bytecode generator knows how to handle TDZ checks, etc.
            if let Some(dk) = group.declaration_kind {
                for id in &group.identifiers {
                    id.declaration_kind.set(Some(dk));
                }
            }

            let var_flags = records[index]
                .variables
                .get(&name)
                .map_or(VarFlags::EMPTY, |v| v.flags);

            // Determine what kind of local variable this is (if any).
            // Priority: var (at top-level) > let/const > function declaration.
            let mut local_var_kind: Option<LocalVarKind> = None;
            if records[index].is_top_level() && var_flags.intersects(VarFlags::VAR) {
                local_var_kind = Some(LocalVarKind::Var);
            } else if var_flags.intersects(VarFlags::LEXICAL) {
                local_var_kind = Some(LocalVarKind::LetOrConst);
            } else if var_flags.intersects(VarFlags::FUNCTION) {
                local_var_kind = Some(LocalVarKind::Function);
            }

            // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
            // Non-arrow functions implicitly declare `arguments` as a local
            // (an Arguments exotic object or a mapped arguments object).
            // https://tc39.es/ecma262/#sec-arrow-function-definitions-runtime-semantics-evaluation
            // Arrow functions do NOT have their own `arguments` binding;
            // references to `arguments` resolve to the enclosing function's.
            if records[index].scope_type == ScopeType::Function
                && !records[index].is_arrow_function
                && name == utf16!("arguments")
            {
                local_var_kind = Some(LocalVarKind::ArgumentsObject);
            }

            if records[index].scope_type == ScopeType::Catch
                && var_flags.intersects(VarFlags::CATCH_PARAMETER)
            {
                local_var_kind = Some(LocalVarKind::CatchClauseParameter);
            }

            // When a function has parameter expressions (default values, etc.), body
            // var declarations live in a separate Variable Environment from the
            // parameter scope. If the same name is also referenced in a default
            // parameter expression, it must not be a local: the default expression
            // needs to resolve it from the outer scope via the environment chain,
            // not read the (uninitialized) local.
            // We also mark the name as captured in the parent scope, so that the
            // outer binding is not optimized to a local register either.
            if var_flags.intersects(VarFlags::REFERENCED_IN_FORMAL_PARAMETERS)
                && var_flags.intersects(VarFlags::VAR)
                && !var_flags.intersects(VarFlags::FORBIDDEN_LEXICAL)
            {
                if let Some(parent_index) = records[index].parent {
                    records[parent_index]
                        .identifier_groups
                        .entry(name.clone())
                        .or_insert_with(|| IdentifierGroup {
                            captured_by_nested_function: false,
                            used_inside_with_statement: false,
                            identifiers: Vec::new(),
                            declaration_kind: None,
                        })
                        .captured_by_nested_function = true;
                }
                continue;
            }

            let hoistable = records[index].has_hoistable_function_named(&name);

            // ClassDeclaration with IsBound: skip entirely.
            if records[index].scope_type == ScopeType::ClassDeclaration
                && var_flags.intersects(VarFlags::BOUND)
            {
                continue;
            }

            // Function expression name binding.
            if records[index].scope_type == ScopeType::Function
                && !records[index].is_function_declaration
                && var_flags.intersects(VarFlags::BOUND)
            {
                for id in &group.identifiers {
                    id.is_inside_scope_with_eval.set(true);
                }
            }

            if records[index].scope_type == ScopeType::ClassDeclaration {
                local_var_kind = None;
            }

            let mut is_function_parameter = false;
            if records[index].scope_type == ScopeType::Function {
                if var_flags.intersects(VarFlags::PARAMETER_CANDIDATE)
                    && (!records[index].contains_access_to_arguments_object_in_non_strict_mode
                        || records[index].has_rest_parameter_with_name(&name))
                {
                    is_function_parameter = true;
                } else if var_flags.intersects(VarFlags::FORBIDDEN_LEXICAL) {
                    continue;
                }
            }

            if records[index].scope_type == ScopeType::Function && hoistable {
                continue;
            }

            if records[index].scope_type == ScopeType::Program {
                let can_use_global =
                    !(suppress_globals || group.used_inside_with_statement || initiated_by_eval);
                if can_use_global {
                    for id in &group.identifiers {
                        if !id.is_inside_scope_with_eval.get() {
                            id.is_global.set(true);
                        }
                    }
                }
            } else if local_var_kind.is_some() || is_function_parameter {
                if hoistable {
                    continue;
                }

                // When a function has parameter expressions and a nested function in a
                // default expression captures a name that is also a body var, propagate
                // the capture to the parent scope so the outer binding stays in the
                // environment (not optimized to a local register).
                if records[index].has_parameter_expressions
                    && group.captured_by_nested_function
                    && var_flags.intersects(VarFlags::VAR)
                    && !var_flags.intersects(VarFlags::FORBIDDEN_LEXICAL)
                    && let Some(parent_index) = records[index].parent
                {
                    records[parent_index]
                        .identifier_groups
                        .entry(name.clone())
                        .or_insert_with(|| IdentifierGroup {
                            captured_by_nested_function: false,
                            used_inside_with_statement: false,
                            identifiers: Vec::new(),
                            declaration_kind: None,
                        })
                        .captured_by_nested_function = true;
                }

                if !group.captured_by_nested_function && !group.used_inside_with_statement {
                    if records[index].poisoned_by_eval_in_scope_chain {
                        continue;
                    }

                    let mut local_scope = last_function_scope(index, records);
                    if local_scope.is_none() {
                        if group.declaration_kind == Some(DeclarationKind::Var) {
                            continue;
                        }
                        local_scope = records[index].top_level;
                    }

                    if let Some(ls) = local_scope
                        && let Some(ref scope_data) = records[ls].scope_data
                    {
                        let mut sd = scope_data.borrow_mut();

                        if is_function_parameter {
                            let argument_index = records[ls].get_parameter_index(&name);
                            if let Some(ai) = argument_index {
                                for id in &group.identifiers {
                                    id.local_index.set(ai);
                                    id.local_type.set(Some(crate::ast::LocalType::Argument));
                                }
                            } else {
                                let lvi = u32_from_usize(sd.local_variables.len());
                                sd.local_variables.push(LocalVariable {
                                    name: name.clone(),
                                    kind: LocalVarKind::Var,
                                });
                                for id in &group.identifiers {
                                    id.local_index.set(lvi);
                                    id.local_type.set(Some(crate::ast::LocalType::Variable));
                                }
                            }
                        } else {
                            let kind = local_var_kind
                                .expect("local_var_kind must be set for local variables");
                            let lvi = u32_from_usize(sd.local_variables.len());
                            sd.local_variables.push(LocalVariable {
                                name: name.clone(),
                                kind,
                            });
                            for id in &group.identifiers {
                                id.local_index.set(lvi);
                                id.local_type.set(Some(crate::ast::LocalType::Variable));
                            }
                        }
                    }
                }
            } else {
                if records[index].has_function_parameters
                    || records[index].scope_type == ScopeType::ClassField
                    || records[index].scope_type == ScopeType::ClassStaticInit
                {
                    group.captured_by_nested_function = true;
                }

                if records[index].scope_type == ScopeType::With {
                    group.used_inside_with_statement = true;
                }

                if records[index].eval_in_current_function {
                    for id in &group.identifiers {
                        id.is_inside_scope_with_eval.set(true);
                    }
                }

                propagate_to_parent.push((name, group));
            }
        }

        if let Some(parent_index) = records[index].parent {
            for (name, group) in propagate_to_parent {
                if let Some(parent_group) = records[parent_index].identifier_groups.get_mut(&name) {
                    parent_group.identifiers.extend(group.identifiers);
                    if group.captured_by_nested_function {
                        parent_group.captured_by_nested_function = true;
                    }
                    if group.used_inside_with_statement {
                        parent_group.used_inside_with_statement = true;
                    }
                } else {
                    records[parent_index].identifier_groups.insert(name, group);
                }
            }
        }
    }

    // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
    // Builds the data needed by FunctionDeclarationInstantiation at runtime:
    // - vars_to_initialize: var-declared names and their local variable indices
    // - functions_to_initialize: function declarations to instantiate (in reverse order)
    // - arguments object metadata (has_argument_parameter, has_function_named_arguments, etc.)
    fn build_function_scope_data(records: &[ScopeRecord], index: usize) {
        let record = &records[index];
        let scope_data = match record.scope_data {
            Some(ref sd) => sd,
            None => return,
        };

        let has_argument_parameter = record
            .variables
            .get(utf16!("arguments") as &[u16])
            .is_some_and(|v| v.flags.intersects(VarFlags::FORBIDDEN_LEXICAL));

        let mut vars_to_initialize = Vec::new();
        let mut var_names = Vec::new();
        let mut has_function_named_arguments = false;
        let mut has_lexically_declared_arguments = false;
        let mut non_local_var_count: usize = 0;
        let mut non_local_var_count_for_parameter_expressions: usize = 0;

        // Build functions_to_initialize by scanning children for FunctionDeclarations.
        // Walk in reverse order, deduplicating by name.
        let mut functions_to_initialize: Vec<crate::ast::FunctionToInit> = Vec::new();
        let mut seen_function_names: HashSet<Utf16String> = HashSet::new();
        {
            let sd = scope_data.borrow();
            for i in (0..sd.children.len()).rev() {
                if let crate::ast::StatementKind::FunctionDeclaration {
                    name: Some(ref name_ident),
                    ..
                } = sd.children[i].inner
                    && seen_function_names.insert(name_ident.name.clone())
                {
                    functions_to_initialize.push(crate::ast::FunctionToInit { child_index: i });
                }
            }
        }

        for (name, var) in &record.variables {
            if !var.flags.intersects(VarFlags::VAR) {
                continue;
            }

            var_names.push(name.clone());

            let is_parameter = var.flags.intersects(VarFlags::FORBIDDEN_LEXICAL);
            let is_function_name = seen_function_names.contains(name);

            let local_info = if let Some(ref ident) = var.var_identifier {
                if ident.is_local() {
                    Some(LocalBinding {
                        local_type: ident
                            .local_type
                            .get()
                            .expect("is_local() implies local_type is Some"),
                        index: ident.local_index.get(),
                    })
                } else {
                    None
                }
            } else {
                None
            };

            if local_info.is_none() {
                non_local_var_count_for_parameter_expressions += 1;
                if !is_parameter {
                    non_local_var_count += 1;
                }
            }

            vars_to_initialize.push(VarToInit {
                name: name.clone(),
                is_parameter,
                is_function_name,
                local: local_info,
            });
        }

        // Sort by name for deterministic output (HashMap iteration order is arbitrary).
        vars_to_initialize.sort_by(|a, b| a.name.cmp(&b.name));
        var_names.sort();

        if seen_function_names.iter().any(|n| n == utf16!("arguments")) {
            has_function_named_arguments = true;
        }

        if record
            .variables
            .get(utf16!("arguments") as &[u16])
            .is_some_and(|v| v.flags.intersects(VarFlags::LEXICAL))
        {
            has_lexically_declared_arguments = true;
        }

        let fsd = FunctionScopeData {
            functions_to_initialize,
            vars_to_initialize,
            var_names,
            has_function_named_arguments,
            has_argument_parameter,
            has_lexically_declared_arguments,
            non_local_var_count,
            non_local_var_count_for_parameter_expressions,
        };

        {
            let mut sd = scope_data.borrow_mut();
            sd.function_scope_data = Some(Box::new(fsd));

            // Write scope analysis insights to ScopeData so they can be read
            // during lazy compilation (write_sfd_metadata, FDI emission).
            sd.uses_this = record.uses_this;
            sd.uses_this_from_environment = record.uses_this_from_environment;
            sd.contains_direct_call_to_eval =
                record.contains_direct_call_to_eval || record.poisoned_by_eval_in_scope_chain;
            sd.contains_access_to_arguments_object =
                record.contains_access_to_arguments_object_in_non_strict_mode;
        }
    }

    /// https://tc39.es/ecma262/#sec-block-level-function-declarations-web-legacy-compatibility-semantics
    ///
    /// Annex B.3.3 function hoisting: in sloppy mode, function declarations
    /// inside blocks can create `var` bindings in the enclosing function scope.
    ///
    /// For example:
    /// ```js
    /// function f() {
    ///     if (true) { function g() {} }  // g is hoisted to f's scope
    ///     g(); // works in sloppy mode!
    /// }
    /// ```
    ///
    /// The function propagates upward through block scopes until it reaches
    /// a function/program scope (top level) or is blocked by an existing
    /// lexical or function declaration with the same name.
    fn hoist_functions(records: &mut [ScopeRecord], index: usize) {
        let functions = std::mem::take(&mut records[index].functions_to_hoist);

        for function in functions {
            // A let/const or forbidden var with the same name blocks hoisting.
            if records[index].has_flag(&function.name, VarFlags::LEXICAL | VarFlags::FORBIDDEN_VAR)
            {
                continue;
            }

            if records[index].is_top_level() {
                // AnnexB.3.3.1: Skip hoisting if the function name is a parameter name.
                if records[index].has_flag(&function.name, VarFlags::FORBIDDEN_LEXICAL) {
                    continue;
                }
                // AnnexB.3.3.1: Skip hoisting if the function name is "arguments"
                // and the function needs an arguments object.
                if function.name == utf16!("arguments")
                    && records[index].contains_access_to_arguments_object_in_non_strict_mode
                    && !records[index].has_flag(utf16!("arguments"), VarFlags::FORBIDDEN_LEXICAL)
                {
                    continue;
                }
                // Reached function/program scope — register the hoisted function name.
                if let Some(ref scope_data) = records[index].scope_data {
                    let mut sd = scope_data.borrow_mut();
                    if !sd.annexb_function_names.contains(&function.name) {
                        sd.annexb_function_names.push(function.name.clone());
                    }
                }
                // Mark all function declarations with this name in the block
                // as hoisted, so they emit GetBinding + SetVariableBinding.
                if let Some(ref block_scope) = function.block_scope_data {
                    let bs = block_scope.borrow();
                    for child in &bs.children {
                        if let crate::ast::StatementKind::FunctionDeclaration {
                            ref name,
                            ref is_hoisted,
                            ..
                        } = child.inner
                            && name.as_ref().is_some_and(|n| n.name == function.name)
                        {
                            is_hoisted.set(true);
                        }
                    }
                }
            } else if let Some(parent_index) = records[index].parent {
                // Not yet at top level — keep propagating upward unless blocked.
                if !records[parent_index]
                    .has_flag(&function.name, VarFlags::LEXICAL | VarFlags::FUNCTION)
                {
                    records[parent_index].functions_to_hoist.push(function);
                }
            }
        }
    }
}
