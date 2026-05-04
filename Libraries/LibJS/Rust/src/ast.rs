/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AST types for JavaScript.
//!
//! This module defines the Abstract Syntax Tree using idiomatic Rust enums.
//! Every node carries a `SourceRange` for error messages and source maps.
//!
//! ## Design
//!
//! - `ExpressionKind` and `StatementKind` are flat enums — pattern matching
//!   replaces virtual dispatch.
//! - `Node<T>` wraps every AST node with source location info.
//! - `Identifier` carries scope analysis results as plain fields, written
//!   by the scope collector through `&mut arena.identifiers[id]` after
//!   parsing.
//! - Operator enums use `#[repr(u8)]` with ABI-compatible values for
//!   trivial FFI conversion.
//! - `ScopeData` is carried by block-like constructs (Program,
//!   BlockStatement, FunctionBody, etc.) and holds scope analysis results.

use std::ffi::c_void;
use std::fmt;
use std::ops::{Index, IndexMut};
use std::sync::Arc;
use std::sync::atomic::{AtomicPtr, Ordering};

use crate::fast_hash::HashMap;

use crate::u32_from_usize;

// =============================================================================
// AST arena (contiguous storage for identifiers, scopes, and interned strings)
// =============================================================================
//
// Bulk allocation replaces per-node `Rc::new` calls. AST nodes hold opaque
// `Copy` indexes into the arena's `Vec`s; the actual data lives contiguously
// for cache-friendly traversal during scope analysis and codegen. After parse
// the arena is logically frozen and can be shared across threads via
// `Arc<AstArena>` without atomic refcount churn on individual nodes.

/// Opaque handle into `IdentifierArena`. `Copy` so AST nodes can hold one
/// without cloning anything.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub struct IdentifierId(u32);

/// Opaque handle into `ScopeArena`.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub struct ScopeId(u32);

/// Opaque handle into `StringInterner`. Equal `StringId`s mean equal strings,
/// so name comparisons are a single `u32` compare instead of slice equality.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub struct StringId(u32);

/// Contiguous backing store for `Identifier` nodes.
#[derive(Debug, Default)]
pub struct IdentifierArena {
    storage: Vec<Identifier>,
}

impl IdentifierArena {
    pub fn new() -> Self {
        Self { storage: Vec::new() }
    }

    pub fn insert(&mut self, identifier: Identifier) -> IdentifierId {
        let id = IdentifierId(u32_from_usize(self.storage.len()));
        self.storage.push(identifier);
        id
    }

    pub fn len(&self) -> usize {
        self.storage.len()
    }

    pub fn is_empty(&self) -> bool {
        self.storage.is_empty()
    }
}

impl Index<IdentifierId> for IdentifierArena {
    type Output = Identifier;
    fn index(&self, id: IdentifierId) -> &Identifier {
        &self.storage[id.0 as usize]
    }
}

impl IndexMut<IdentifierId> for IdentifierArena {
    fn index_mut(&mut self, id: IdentifierId) -> &mut Identifier {
        &mut self.storage[id.0 as usize]
    }
}

/// Contiguous backing store for `ScopeData` nodes.
#[derive(Debug, Default)]
pub struct ScopeArena {
    storage: Vec<ScopeData>,
}

impl ScopeArena {
    pub fn new() -> Self {
        Self { storage: Vec::new() }
    }

    pub fn insert(&mut self, scope: ScopeData) -> ScopeId {
        let id = ScopeId(u32_from_usize(self.storage.len()));
        self.storage.push(scope);
        id
    }

    pub fn len(&self) -> usize {
        self.storage.len()
    }

    pub fn is_empty(&self) -> bool {
        self.storage.is_empty()
    }
}

impl Index<ScopeId> for ScopeArena {
    type Output = ScopeData;
    fn index(&self, id: ScopeId) -> &ScopeData {
        &self.storage[id.0 as usize]
    }
}

impl IndexMut<ScopeId> for ScopeArena {
    fn index_mut(&mut self, id: ScopeId) -> &mut ScopeData {
        &mut self.storage[id.0 as usize]
    }
}

/// Deduplicating string table. Repeated identifier names from the source
/// (`length`, `i`, `this`, ...) all map to the same `StringId`, so the
/// per-identifier name no longer allocates after the first occurrence.
#[derive(Debug, Default)]
pub struct StringInterner {
    storage: Vec<Utf16String>,
    lookup: HashMap<Utf16String, StringId>,
}

impl StringInterner {
    pub fn new() -> Self {
        Self {
            storage: Vec::new(),
            lookup: HashMap::default(),
        }
    }

    pub fn intern(&mut self, value: &[u16]) -> StringId {
        if let Some(&id) = self.lookup.get(value) {
            return id;
        }
        let id = StringId(u32_from_usize(self.storage.len()));
        let owned = Utf16String::from(value);
        self.storage.push(owned.clone());
        self.lookup.insert(owned, id);
        id
    }

    pub fn intern_owned(&mut self, value: Utf16String) -> StringId {
        if let Some(&id) = self.lookup.get(value.as_slice()) {
            return id;
        }
        let id = StringId(u32_from_usize(self.storage.len()));
        self.storage.push(value.clone());
        self.lookup.insert(value, id);
        id
    }

    pub fn len(&self) -> usize {
        self.storage.len()
    }

    pub fn is_empty(&self) -> bool {
        self.storage.is_empty()
    }
}

impl Index<StringId> for StringInterner {
    type Output = Utf16String;
    fn index(&self, id: StringId) -> &Utf16String {
        &self.storage[id.0 as usize]
    }
}

/// Bundles the three arenas. Owned by the parser during parsing, then handed
/// to `ParsedProgram`/`CompiledProgram`. Since every contained type is plain
/// data (no `Cell`/`RefCell`/`Rc`) once the parser is done, this is naturally
/// `Send + Sync` and can be wrapped in `Arc` for concurrent codegen.
#[derive(Debug, Default)]
pub struct AstArena {
    pub identifiers: IdentifierArena,
    pub scopes: ScopeArena,
    pub strings: StringInterner,
}

impl AstArena {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn name_of(&self, id: IdentifierId) -> &Utf16String {
        &self.strings[self.identifiers[id].name]
    }

    pub fn name_slice(&self, id: IdentifierId) -> &[u16] {
        self.strings[self.identifiers[id].name].as_slice()
    }
}

// =============================================================================
// Function table (side table for FunctionData)
// =============================================================================

/// Opaque handle into the `FunctionTable`. Copy + Clone so AST nodes can
/// freely duplicate it without cloning the underlying `FunctionData`.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub struct FunctionId(u32);

/// Flat side table that owns all `FunctionData` produced during parsing.
///
/// The parser inserts `FunctionData` via `insert()` and receives a
/// `FunctionId`. Later consumers (ast_dump, scope collector, codegen)
/// either borrow via `get()` or take ownership via `take()`.
///
/// `take()` replaces the slot with `None` so each `FunctionData` is
/// moved out exactly once (during codegen / GDI). This eliminates the
/// deep clone that was previously required in `create_shared_function_data`.
pub struct FunctionTable {
    functions: HashMap<FunctionId, Box<FunctionData>>,
    next_id: u32,
}

impl Default for FunctionTable {
    fn default() -> Self {
        Self::new()
    }
}

impl FunctionTable {
    pub fn new() -> Self {
        Self {
            functions: HashMap::default(),
            next_id: 0,
        }
    }

    /// Insert a `FunctionData`, returning a `FunctionId` handle.
    pub fn insert(&mut self, data: FunctionData) -> FunctionId {
        let id = FunctionId(self.next_id);
        self.next_id += 1;
        self.functions.insert(id, Box::new(data));
        id
    }

    /// Borrow the data (for read-only access like ast_dump).
    ///
    /// # Panics
    /// Panics if the slot was already taken.
    pub fn get(&self, id: FunctionId) -> &FunctionData {
        self.functions.get(&id).expect("FunctionTable::get: slot already taken")
    }

    /// Take ownership of the data (for codegen / GDI).
    ///
    /// # Panics
    /// Panics if the slot was already taken.
    pub fn take(&mut self, id: FunctionId) -> Box<FunctionData> {
        self.functions
            .remove(&id)
            .expect("FunctionTable::take: slot already taken")
    }

    /// Take ownership if the slot is still present; returns None if already taken.
    fn try_take(&mut self, id: FunctionId) -> Option<Box<FunctionData>> {
        self.functions.remove(&id)
    }

    /// Insert a `Box<FunctionData>` at a specific id.
    fn insert_at(&mut self, id: FunctionId, data: Box<FunctionData>) {
        self.functions.insert(id, data);
    }

    /// Extract the nested function subtree needed to compile `data` later.
    ///
    /// Parser-created functions carry an explicit child list, so this is
    /// normally proportional to the number of nested functions instead of the
    /// size of the function body.
    pub fn extract_reachable(&mut self, data: &FunctionData, scopes: &ScopeArena) -> FunctionTable {
        let mut subtable = FunctionTable::new();
        if let Some(nested_function_ids) = &data.nested_function_ids {
            for id in nested_function_ids {
                self.transfer(*id, &mut subtable, scopes);
            }
        } else {
            // Synthetic function wrappers created during codegen (for example
            // class field initializers) do not come from the parser's function
            // context stack, so keep the structural scan for those rare cases.
            for param in &data.parameters {
                if let Some(ref default) = param.default_value {
                    self.collect_from_expression(default, &mut subtable, scopes);
                }
                if let FunctionParameterBinding::BindingPattern(ref pat) = param.binding {
                    self.collect_from_pattern(pat, &mut subtable, scopes);
                }
            }
            self.collect_from_statement(&data.body, &mut subtable, scopes);
        }
        subtable
    }

    fn transfer(&mut self, id: FunctionId, result: &mut FunctionTable, scopes: &ScopeArena) {
        if let Some(data) = self.try_take(id) {
            if let Some(nested_function_ids) = &data.nested_function_ids {
                for id in nested_function_ids {
                    self.transfer(*id, result, scopes);
                }
            } else {
                for param in &data.parameters {
                    if let Some(ref default) = param.default_value {
                        self.collect_from_expression(default, result, scopes);
                    }
                    if let FunctionParameterBinding::BindingPattern(ref pat) = param.binding {
                        self.collect_from_pattern(pat, result, scopes);
                    }
                }
                self.collect_from_statement(&data.body, result, scopes);
            }
            result.insert_at(id, data);
        }
    }

    fn collect_from_statement(&mut self, stmt: &Statement, result: &mut FunctionTable, scopes: &ScopeArena) {
        match &stmt.inner {
            StatementKind::FunctionDeclaration(data) => {
                self.transfer(data.function_id, result, scopes);
            }
            StatementKind::Expression(expr) => self.collect_from_expression(expr, result, scopes),
            StatementKind::Block(scope) | StatementKind::FunctionBody { scope, .. } => {
                for child in &scopes[*scope].children {
                    self.collect_from_statement(child, result, scopes);
                }
            }
            StatementKind::Program(data) => {
                for child in &scopes[data.scope].children {
                    self.collect_from_statement(child, result, scopes);
                }
            }
            StatementKind::If(data) => {
                self.collect_from_expression(&data.test, result, scopes);
                self.collect_from_statement(&data.consequent, result, scopes);
                if let Some(alt) = &data.alternate {
                    self.collect_from_statement(alt, result, scopes);
                }
            }
            StatementKind::While(data) => {
                self.collect_from_expression(&data.test, result, scopes);
                self.collect_from_statement(&data.body, result, scopes);
            }
            StatementKind::DoWhile(data) => {
                self.collect_from_statement(&data.body, result, scopes);
                self.collect_from_expression(&data.test, result, scopes);
            }
            StatementKind::For(data) => {
                if let Some(init) = &data.init {
                    match init {
                        ForInit::Expression(expr) => self.collect_from_expression(expr, result, scopes),
                        ForInit::Declaration(decl) => self.collect_from_statement(decl, result, scopes),
                    }
                }
                if let Some(test) = &data.test {
                    self.collect_from_expression(test, result, scopes);
                }
                if let Some(update) = &data.update {
                    self.collect_from_expression(update, result, scopes);
                }
                self.collect_from_statement(&data.body, result, scopes);
            }
            StatementKind::ForInOf(data) => {
                match &data.lhs {
                    ForInOfLhs::Declaration(decl) => self.collect_from_statement(decl, result, scopes),
                    ForInOfLhs::Expression(expr) => self.collect_from_expression(expr, result, scopes),
                    ForInOfLhs::Pattern(pattern) => self.collect_from_pattern(pattern, result, scopes),
                }
                self.collect_from_expression(&data.rhs, result, scopes);
                self.collect_from_statement(&data.body, result, scopes);
            }
            StatementKind::Switch(data) => {
                self.collect_from_expression(&data.discriminant, result, scopes);
                for case in &data.cases {
                    if let Some(ref test) = case.test {
                        self.collect_from_expression(test, result, scopes);
                    }
                    for child in &scopes[case.scope].children {
                        self.collect_from_statement(child, result, scopes);
                    }
                }
            }
            StatementKind::With(data) => {
                self.collect_from_expression(&data.object, result, scopes);
                self.collect_from_statement(&data.body, result, scopes);
            }
            StatementKind::Labelled(data) => {
                self.collect_from_statement(&data.item, result, scopes);
            }
            StatementKind::Return(arg) => {
                if let Some(expr) = arg {
                    self.collect_from_expression(expr, result, scopes);
                }
            }
            StatementKind::Throw(expr) => {
                self.collect_from_expression(expr, result, scopes);
            }
            StatementKind::Try(data) => {
                self.collect_from_statement(&data.block, result, scopes);
                if let Some(ref handler) = data.handler {
                    if let Some(CatchBinding::BindingPattern(ref pat)) = handler.parameter {
                        self.collect_from_pattern(pat, result, scopes);
                    }
                    self.collect_from_statement(&handler.body, result, scopes);
                }
                if let Some(ref finalizer) = data.finalizer {
                    self.collect_from_statement(finalizer, result, scopes);
                }
            }
            StatementKind::VariableDeclaration(data) => {
                for decl in &data.declarations {
                    self.collect_from_target(&decl.target, result, scopes);
                    if let Some(ref init) = decl.init {
                        self.collect_from_expression(init, result, scopes);
                    }
                }
            }
            StatementKind::UsingDeclaration(declarations) => {
                for decl in declarations.iter() {
                    self.collect_from_target(&decl.target, result, scopes);
                    if let Some(ref init) = decl.init {
                        self.collect_from_expression(init, result, scopes);
                    }
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                self.collect_from_class(class_data, result, scopes);
            }
            StatementKind::Export(data) => {
                if let Some(ref stmt) = data.statement {
                    self.collect_from_statement(stmt, result, scopes);
                }
            }
            StatementKind::ClassFieldInitializer(data) => {
                self.collect_from_expression(&data.expression, result, scopes);
            }
            StatementKind::Empty
            | StatementKind::Debugger
            | StatementKind::Break { .. }
            | StatementKind::Continue { .. }
            | StatementKind::Import(_)
            | StatementKind::Error
            | StatementKind::ErrorDeclaration => {}
        }
    }

    fn collect_from_expression(&mut self, expr: &Expression, result: &mut FunctionTable, scopes: &ScopeArena) {
        match &expr.inner {
            ExpressionKind::Function(function_id) => {
                self.transfer(*function_id, result, scopes);
            }
            ExpressionKind::Class(class_data) => {
                self.collect_from_class(class_data, result, scopes);
            }
            ExpressionKind::Binary(data) => {
                self.collect_from_expression(&data.lhs, result, scopes);
                self.collect_from_expression(&data.rhs, result, scopes);
            }
            ExpressionKind::Logical(data) => {
                self.collect_from_expression(&data.lhs, result, scopes);
                self.collect_from_expression(&data.rhs, result, scopes);
            }
            ExpressionKind::Unary { operand, .. } => {
                self.collect_from_expression(operand, result, scopes);
            }
            ExpressionKind::Update(data) => {
                self.collect_from_expression(&data.argument, result, scopes);
            }
            ExpressionKind::Assignment(data) => {
                match &data.lhs {
                    AssignmentLhs::Expression(expr) => self.collect_from_expression(expr, result, scopes),
                    AssignmentLhs::Pattern(pat) => self.collect_from_pattern(pat, result, scopes),
                }
                self.collect_from_expression(&data.rhs, result, scopes);
            }
            ExpressionKind::Conditional(data) => {
                self.collect_from_expression(&data.test, result, scopes);
                self.collect_from_expression(&data.consequent, result, scopes);
                self.collect_from_expression(&data.alternate, result, scopes);
            }
            ExpressionKind::Sequence(exprs) => {
                for expr in exprs.iter() {
                    self.collect_from_expression(expr, result, scopes);
                }
            }
            ExpressionKind::Member(data) => {
                self.collect_from_expression(&data.object, result, scopes);
                self.collect_from_expression(&data.property, result, scopes);
            }
            ExpressionKind::OptionalChain(data) => {
                self.collect_from_expression(&data.base, result, scopes);
                for reference in &data.references {
                    match reference {
                        OptionalChainReference::Call { arguments, .. } => {
                            for arg in arguments {
                                self.collect_from_expression(&arg.value, result, scopes);
                            }
                        }
                        OptionalChainReference::ComputedReference { expression, .. } => {
                            self.collect_from_expression(expression, result, scopes);
                        }
                        OptionalChainReference::MemberReference { .. }
                        | OptionalChainReference::PrivateMemberReference { .. } => {}
                    }
                }
            }
            ExpressionKind::Call(data) | ExpressionKind::New(data) => {
                self.collect_from_expression(&data.callee, result, scopes);
                for arg in &data.arguments {
                    self.collect_from_expression(&arg.value, result, scopes);
                }
            }
            ExpressionKind::SuperCall(data) => {
                for arg in &data.arguments {
                    self.collect_from_expression(&arg.value, result, scopes);
                }
            }
            ExpressionKind::Spread(expr) | ExpressionKind::Await(expr) => {
                self.collect_from_expression(expr, result, scopes);
            }
            ExpressionKind::Array(elements) => {
                for expr in elements.iter().flatten() {
                    self.collect_from_expression(expr, result, scopes);
                }
            }
            ExpressionKind::Object(properties) => {
                for prop in properties.iter() {
                    self.collect_from_expression(&prop.key, result, scopes);
                    if let Some(ref val) = prop.value {
                        self.collect_from_expression(val, result, scopes);
                    }
                }
            }
            ExpressionKind::TemplateLiteral(data) => {
                for expr in &data.expressions {
                    self.collect_from_expression(expr, result, scopes);
                }
            }
            ExpressionKind::TaggedTemplateLiteral(data) => {
                self.collect_from_expression(&data.tag, result, scopes);
                self.collect_from_expression(&data.template_literal, result, scopes);
            }
            ExpressionKind::Yield(data) => {
                if let Some(ref expr) = data.argument {
                    self.collect_from_expression(expr, result, scopes);
                }
            }
            ExpressionKind::ImportCall(data) => {
                self.collect_from_expression(&data.specifier, result, scopes);
                if let Some(ref opts) = data.options {
                    self.collect_from_expression(opts, result, scopes);
                }
            }
            ExpressionKind::NumericLiteral(_)
            | ExpressionKind::StringLiteral(_)
            | ExpressionKind::BooleanLiteral(_)
            | ExpressionKind::NullLiteral
            | ExpressionKind::BigIntLiteral(_)
            | ExpressionKind::RegExpLiteral(_)
            | ExpressionKind::Identifier(_)
            | ExpressionKind::PrivateIdentifier(_)
            | ExpressionKind::This
            | ExpressionKind::Super
            | ExpressionKind::MetaProperty(_)
            | ExpressionKind::Error => {}
        }
    }

    fn collect_from_class(&mut self, class_data: &ClassData, result: &mut FunctionTable, scopes: &ScopeArena) {
        if let Some(ref super_class) = class_data.super_class {
            self.collect_from_expression(super_class, result, scopes);
        }
        if let Some(ref constructor) = class_data.constructor {
            self.collect_from_expression(constructor, result, scopes);
        }
        for element in &class_data.elements {
            match &element.inner {
                ClassElement::Method { key, function, .. } => {
                    self.collect_from_expression(key, result, scopes);
                    self.collect_from_expression(function, result, scopes);
                }
                ClassElement::Field { key, initializer, .. } => {
                    self.collect_from_expression(key, result, scopes);
                    if let Some(init) = initializer {
                        self.collect_from_expression(init, result, scopes);
                    }
                }
                ClassElement::StaticInitializer { body } => {
                    self.collect_from_statement(body, result, scopes);
                }
            }
        }
    }

    fn collect_from_pattern(&mut self, pattern: &BindingPattern, result: &mut FunctionTable, scopes: &ScopeArena) {
        for entry in &pattern.entries {
            if let Some(BindingEntryName::Expression(expr)) = entry.name.as_ref() {
                self.collect_from_expression(expr, result, scopes);
            }
            if let Some(ref alias) = entry.alias {
                match alias {
                    BindingEntryAlias::BindingPattern(sub) => {
                        self.collect_from_pattern(sub, result, scopes);
                    }
                    BindingEntryAlias::MemberExpression(expr) => {
                        self.collect_from_expression(expr, result, scopes);
                    }
                    BindingEntryAlias::Identifier(_) => {}
                }
            }
            if let Some(ref init) = entry.initializer {
                self.collect_from_expression(init, result, scopes);
            }
        }
    }

    fn collect_from_target(
        &mut self,
        target: &VariableDeclaratorTarget,
        result: &mut FunctionTable,
        scopes: &ScopeArena,
    ) {
        if let VariableDeclaratorTarget::BindingPattern(pat) = target {
            self.collect_from_pattern(pat, result, scopes);
        }
    }
}

/// Bundles a `FunctionData` with a subtable of all nested functions
/// reachable from its body. Stored as the raw pointer in C++ SFDs.
pub struct FunctionPayload {
    pub data: FunctionData,
    pub function_table: FunctionTable,
    /// Shared access to the program-wide identifier/scope/string tables.
    /// Each lazy-compile SFD carries an Arc clone so it can resolve its
    /// identifier IDs without depending on a parent generator.
    pub arena: Arc<AstArena>,
}

// =============================================================================
// Source location
// =============================================================================

/// A UTF-16 encoded string.
///
/// Wraps `Vec<u16>` to provide type safety and distinguish UTF-16 text
/// from arbitrary `u16` buffers. Access the inner Vec via `.0` when
/// Vec-specific methods like `push` or `extend` are needed.
#[derive(Clone, Debug, Hash, Eq, PartialEq, Ord, PartialOrd, Default)]
pub struct Utf16String(pub Vec<u16>);

impl std::ops::Deref for Utf16String {
    type Target = [u16];
    fn deref(&self) -> &[u16] {
        &self.0
    }
}

impl std::ops::DerefMut for Utf16String {
    fn deref_mut(&mut self) -> &mut [u16] {
        &mut self.0
    }
}

impl From<Vec<u16>> for Utf16String {
    fn from(v: Vec<u16>) -> Self {
        Self(v)
    }
}

impl From<&[u16]> for Utf16String {
    fn from(s: &[u16]) -> Self {
        Self(s.to_vec())
    }
}

impl std::borrow::Borrow<[u16]> for Utf16String {
    fn borrow(&self) -> &[u16] {
        &self.0
    }
}

impl AsRef<[u16]> for Utf16String {
    fn as_ref(&self) -> &[u16] {
        &self.0
    }
}

impl PartialEq<[u16]> for Utf16String {
    fn eq(&self, other: &[u16]) -> bool {
        self.0 == other
    }
}

impl PartialEq<&[u16]> for Utf16String {
    fn eq(&self, other: &&[u16]) -> bool {
        self.0.as_slice() == *other
    }
}

impl PartialEq<Vec<u16>> for Utf16String {
    fn eq(&self, other: &Vec<u16>) -> bool {
        self.0 == *other
    }
}

impl FromIterator<u16> for Utf16String {
    fn from_iter<I: IntoIterator<Item = u16>>(iter: I) -> Self {
        Self(iter.into_iter().collect())
    }
}

impl<'a> IntoIterator for &'a Utf16String {
    type Item = &'a u16;
    type IntoIter = std::slice::Iter<'a, u16>;
    fn into_iter(self) -> Self::IntoIter {
        self.0.iter()
    }
}

impl Utf16String {
    pub fn new() -> Self {
        Self(Vec::new())
    }

    pub fn as_slice(&self) -> &[u16] {
        &self.0
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Position {
    pub line: u32,
    pub column: u32,
    pub offset: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct SourceRange {
    pub start: Position,
    pub end: Position,
}

// =============================================================================
// Node wrapper
// =============================================================================

/// Every AST node wraps its payload with source location.
#[derive(Clone, Debug)]
pub struct Node<T> {
    pub range: SourceRange,
    pub inner: T,
}

pub type Expression = Node<ExpressionKind>;

pub type Statement = Node<StatementKind>;

impl<T> Node<T> {
    pub fn new(range: SourceRange, inner: T) -> Self {
        Self { range, inner }
    }
}

// =============================================================================
// Operator enums — values are ABI-compatible for FFI
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum BinaryOp {
    Addition = 0,
    Subtraction = 1,
    Multiplication = 2,
    Division = 3,
    Modulo = 4,
    Exponentiation = 5,
    StrictlyEquals = 6,
    StrictlyInequals = 7,
    LooselyEquals = 8,
    LooselyInequals = 9,
    GreaterThan = 10,
    GreaterThanEquals = 11,
    LessThan = 12,
    LessThanEquals = 13,
    BitwiseAnd = 14,
    BitwiseOr = 15,
    BitwiseXor = 16,
    LeftShift = 17,
    RightShift = 18,
    UnsignedRightShift = 19,
    In = 20,
    InstanceOf = 21,
}

#[derive(Clone, Copy, Debug)]
#[repr(u8)]
pub enum LogicalOp {
    And = 0,
    Or = 1,
    NullishCoalescing = 2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum UnaryOp {
    BitwiseNot = 0,
    Not = 1,
    Plus = 2,
    Minus = 3,
    Typeof = 4,
    Void = 5,
    Delete = 6,
}

#[derive(Clone, Copy, Debug)]
#[repr(u8)]
pub enum UpdateOp {
    Increment = 0,
    Decrement = 1,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AssignmentOp {
    Assignment = 0,
    AdditionAssignment = 1,
    SubtractionAssignment = 2,
    MultiplicationAssignment = 3,
    DivisionAssignment = 4,
    ModuloAssignment = 5,
    ExponentiationAssignment = 6,
    BitwiseAndAssignment = 7,
    BitwiseOrAssignment = 8,
    BitwiseXorAssignment = 9,
    LeftShiftAssignment = 10,
    RightShiftAssignment = 11,
    UnsignedRightShiftAssignment = 12,
    AndAssignment = 13,
    OrAssignment = 14,
    NullishAssignment = 15,
}

// =============================================================================
// Kind enums
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DeclarationKind {
    Var = 1,
    Let = 2,
    Const = 3,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FunctionKind {
    Normal = 0,
    Generator = 1,
    Async = 2,
    AsyncGenerator = 3,
}

impl FunctionKind {
    pub fn from_async_generator(is_async: bool, is_generator: bool) -> Self {
        match (is_async, is_generator) {
            (true, true) => Self::AsyncGenerator,
            (true, false) => Self::Async,
            (false, true) => Self::Generator,
            (false, false) => Self::Normal,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ProgramType {
    Script = 0,
    Module = 1,
}

#[derive(Clone, Copy, Debug)]
pub enum MetaPropertyType {
    NewTarget,
    ImportMeta,
}

// =============================================================================
// Identifier
// =============================================================================

/// Scope analysis result: how this identifier is resolved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LocalType {
    Argument,
    Variable,
}

/// An identifier reference or binding name.
///
/// Scope analysis writes the resolution fields via `&mut arena.identifiers[id]`
/// during analyze(); after that the arena is logically frozen and these
/// fields are read-only.
#[derive(Clone, Debug)]
pub struct Identifier {
    pub range: SourceRange,
    pub name: StringId,
    // Scope analysis results — set by scope collector after parsing.
    pub local_type: Option<LocalType>,
    pub local_index: u32,
    pub is_global: bool,
    pub is_inside_scope_with_eval: bool,
    pub declaration_kind: Option<DeclarationKind>,
}

impl Identifier {
    pub fn new(range: SourceRange, name: StringId) -> Self {
        Self {
            range,
            name,
            local_type: None,
            local_index: 0,
            is_global: false,
            is_inside_scope_with_eval: false,
            declaration_kind: None,
        }
    }

    pub fn is_local(&self) -> bool {
        self.local_type.is_some()
    }
}

#[derive(Clone, Debug)]
pub struct PrivateIdentifier {
    pub range: SourceRange,
    pub name: Utf16String,
}

// =============================================================================
// Function support types
// =============================================================================

/// Parsing insights collected during function body parsing.
///
/// The scope collector populates `uses_this`, `uses_this_from_environment`,
/// and `contains_direct_call_to_eval` during scope analysis.
/// `might_need_arguments_object` is set by the parser during body parsing.
#[derive(Clone, Copy, Debug, Default)]
pub struct FunctionParsingInsights {
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub contains_direct_call_to_eval: bool,
    pub might_need_arguments_object: bool,
}

#[derive(Clone, Debug)]
pub struct FunctionParameter {
    pub binding: FunctionParameterBinding,
    pub default_value: Option<Expression>,
    pub is_rest: bool,
}

#[derive(Clone, Debug)]
pub enum FunctionParameterBinding {
    Identifier(IdentifierId),
    BindingPattern(BindingPattern),
}

/// Shared data for FunctionDeclaration and FunctionExpression.
#[derive(Debug)]
pub struct FunctionData {
    pub name: Option<IdentifierId>,
    pub source_text_start: u32,
    pub source_text_end: u32,
    pub body: Box<Statement>,
    pub parameters: Vec<FunctionParameter>,
    pub function_length: i32,
    pub kind: FunctionKind,
    pub is_strict_mode: bool,
    pub is_arrow_function: bool,
    pub parsing_insights: FunctionParsingInsights,
    /// Parser-created functions know their nested function ids up front, so
    /// lazy-compile payload extraction can move only that subtree instead of
    /// re-walking the full body. `None` is reserved for synthetic function
    /// wrappers built during codegen, where the old structural scan is still
    /// needed to discover nested functions inside the wrapped AST.
    pub nested_function_ids: Option<Vec<FunctionId>>,
}

// =============================================================================
// Class support types
// =============================================================================

/// Shared data for ClassDeclaration and ClassExpression.
#[derive(Clone, Debug)]
pub struct ClassData {
    pub name: Option<IdentifierId>,
    pub source_text_start: u32,
    pub source_text_end: u32,
    pub constructor: Option<Box<Expression>>,
    pub super_class: Option<Box<Expression>>,
    pub elements: Vec<Node<ClassElement>>,
}

#[derive(Clone, Debug)]
pub enum ClassElement {
    Method {
        key: Box<Expression>,
        function: Box<Expression>,
        kind: ClassMethodKind,
        is_static: bool,
    },
    Field {
        key: Box<Expression>,
        initializer: Option<Box<Expression>>,
        is_static: bool,
    },
    StaticInitializer {
        body: Box<Statement>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ClassMethodKind {
    Method = 0,
    Getter = 1,
    Setter = 2,
}

// =============================================================================
// Binding pattern types
// =============================================================================

#[derive(Clone, Debug)]
pub struct BindingPattern {
    pub kind: BindingPatternKind,
    pub entries: Vec<BindingEntry>,
}

impl BindingPattern {
    pub fn contains_expression(&self) -> bool {
        for entry in &self.entries {
            if matches!(entry.name, Some(BindingEntryName::Expression(_))) {
                return true;
            }
            if entry.initializer.is_some() {
                return true;
            }
            if let Some(BindingEntryAlias::BindingPattern(ref nested)) = entry.alias
                && nested.contains_expression()
            {
                return true;
            }
        }
        false
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BindingPatternKind {
    Array,
    Object,
}

#[derive(Clone, Debug)]
pub struct BindingEntry {
    pub name: Option<BindingEntryName>,
    pub alias: Option<BindingEntryAlias>,
    pub initializer: Option<Expression>,
    pub is_rest: bool,
}

/// The "name" part of a binding entry.
/// - `None`: elision in array patterns (`[, , x]`)
/// - `Identifier`: object property shorthand (`{ x }`)
/// - `Expression`: computed property key (`{ [expression]: x }`)
#[derive(Clone, Debug)]
pub enum BindingEntryName {
    Identifier(IdentifierId),
    Expression(Box<Expression>),
}

/// The "alias" (target) of a binding entry.
/// - `None`: name is the binding target (`{ x }` — x is both name and alias)
/// - `Identifier`: simple binding (`{ x: y }`)
/// - `BindingPattern`: nested destructuring (`{ x: { a, b } }`)
/// - `MemberExpression`: assignment target (`{ x: obj.property }`)
#[derive(Clone, Debug)]
pub enum BindingEntryAlias {
    Identifier(IdentifierId),
    BindingPattern(Box<BindingPattern>),
    MemberExpression(Box<Expression>),
}

// =============================================================================
// Variable declaration types
// =============================================================================

#[derive(Clone, Debug)]
pub struct VariableDeclarator {
    pub range: SourceRange,
    pub target: VariableDeclaratorTarget,
    pub init: Option<Expression>,
}

#[derive(Clone, Debug)]
pub enum VariableDeclaratorTarget {
    Identifier(IdentifierId),
    BindingPattern(BindingPattern),
}

// =============================================================================
// Object literal types
// =============================================================================

#[derive(Clone, Debug)]
pub struct ObjectProperty {
    pub range: SourceRange,
    pub property_type: ObjectPropertyType,
    pub key: Box<Expression>,
    pub value: Option<Box<Expression>>,
    pub is_method: bool,
    pub is_computed: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ObjectPropertyType {
    KeyValue = 0,
    Getter = 1,
    Setter = 2,
    Spread = 3,
    ProtoSetter = 4,
}

// =============================================================================
// Call expression types
// =============================================================================

#[derive(Clone, Debug)]
pub struct CallArgument {
    pub value: Expression,
    pub is_spread: bool,
}

#[derive(Clone, Debug)]
pub struct CallExpressionData {
    pub callee: Box<Expression>,
    pub arguments: Vec<CallArgument>,
    pub is_parenthesized: bool,
    pub is_inside_parens: bool,
}

#[derive(Clone, Debug)]
pub struct SuperCallData {
    pub arguments: Vec<CallArgument>,
    pub is_synthetic: bool,
}

// =============================================================================
// Optional chain types
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OptionalChainMode {
    Optional,
    NotOptional,
}

#[derive(Clone, Debug)]
pub enum OptionalChainReference {
    Call {
        arguments: Vec<CallArgument>,
        mode: OptionalChainMode,
    },
    ComputedReference {
        expression: Box<Expression>,
        mode: OptionalChainMode,
    },
    MemberReference {
        identifier: IdentifierId,
        mode: OptionalChainMode,
    },
    PrivateMemberReference {
        private_identifier: PrivateIdentifier,
        mode: OptionalChainMode,
    },
}

// =============================================================================
// Template literal types
// =============================================================================

#[derive(Clone, Debug)]
pub struct TemplateLiteralData {
    pub expressions: Vec<Expression>,
    pub raw_strings: Vec<Utf16String>,
}

// =============================================================================
// RegExp literal
// =============================================================================

unsafe extern "C" {
    fn rust_free_compiled_regex(ptr: *mut c_void);
}

/// Handle to a compiled regex from C++.
///
/// Wrapped in `Arc` in `RegExpLiteralData` so that AST clones (e.g. for
/// class field initializers) share the handle cheaply. The first codegen
/// path to call `take()` gets the handle; `Drop` frees it if untaken.
/// Uses `AtomicPtr` so the regex can be safely shared across threads
/// during background compile of sibling functions.
pub struct CompiledRegex(AtomicPtr<c_void>);

impl CompiledRegex {
    pub fn new(ptr: *mut c_void) -> Self {
        Self(AtomicPtr::new(ptr))
    }

    /// Take ownership of the compiled regex handle, leaving null behind
    /// so the destructor won't free it.
    pub fn take(&self) -> *mut c_void {
        self.0.swap(std::ptr::null_mut(), Ordering::AcqRel)
    }

    /// Set the compiled regex handle (used by deferred compilation).
    pub fn set(&self, ptr: *mut c_void) {
        self.0.store(ptr, Ordering::Release);
    }
}

impl Drop for CompiledRegex {
    fn drop(&mut self) {
        let ptr = *self.0.get_mut();
        if !ptr.is_null() {
            unsafe { rust_free_compiled_regex(ptr) };
        }
    }
}

impl fmt::Debug for CompiledRegex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CompiledRegex({:p})", self.0.load(Ordering::Acquire))
    }
}

#[derive(Clone, Debug)]
pub struct RegExpLiteralData {
    pub pattern: Utf16String,
    pub flags: Utf16String,
    pub compiled_regex: Arc<CompiledRegex>,
}

// =============================================================================
// Try/Catch types
// =============================================================================

#[derive(Clone, Debug)]
pub struct TryStatementData {
    pub block: Box<Statement>,
    pub handler: Option<CatchClause>,
    pub finalizer: Option<Box<Statement>>,
}

#[derive(Clone, Debug)]
pub struct CatchClause {
    pub range: SourceRange,
    pub parameter: Option<CatchBinding>,
    pub body: Box<Statement>,
}

#[derive(Clone, Debug)]
pub enum CatchBinding {
    Identifier(IdentifierId),
    BindingPattern(BindingPattern),
}

// =============================================================================
// Switch types
// =============================================================================

#[derive(Clone, Debug)]
pub struct SwitchStatementData {
    pub scope: ScopeId,
    pub discriminant: Box<Expression>,
    pub cases: Vec<SwitchCase>,
}

#[derive(Clone, Debug)]
pub struct SwitchCase {
    pub range: SourceRange,
    pub scope: ScopeId,
    pub test: Option<Expression>,
}

// =============================================================================
// Module types (import/export)
// =============================================================================

#[derive(Clone, Debug)]
pub struct ModuleRequest {
    pub module_specifier: Utf16String,
    pub attributes: Vec<ImportAttribute>,
}

#[derive(Clone, Debug)]
pub struct ImportAttribute {
    pub key: Utf16String,
    pub value: Utf16String,
}

#[derive(Clone, Debug)]
pub struct ImportEntry {
    /// `None` means namespace import (`import * as x`).
    pub import_name: Option<Utf16String>,
    pub local_name: Utf16String,
}

#[derive(Clone, Debug)]
pub struct ImportStatementData {
    pub module_request: ModuleRequest,
    pub entries: Vec<ImportEntry>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ExportEntryKind {
    NamedExport = 0,
    ModuleRequestAll = 1,
    ModuleRequestAllButDefault = 2,
    EmptyNamedExport = 3,
}

#[derive(Clone, Debug)]
pub struct ExportEntry {
    pub kind: ExportEntryKind,
    pub export_name: Option<Utf16String>,
    pub local_or_import_name: Option<Utf16String>,
}

#[derive(Clone, Debug)]
pub struct ExportStatementData {
    pub statement: Option<Box<Statement>>,
    pub entries: Vec<ExportEntry>,
    pub is_default_export: bool,
    pub module_request: Option<ModuleRequest>,
}

// =============================================================================
// For-in/of LHS
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ForInOfKind {
    ForIn,
    ForOf,
    ForAwaitOf,
}

/// Init clause of a for loop: either a declaration or an expression.
/// C++ stores this as a polymorphic `RefPtr<ASTNode>` that can be either
/// an Expression or a VariableDeclaration. We use an explicit enum so that
/// expression inits are NOT wrapped in an ExpressionStatement node.
#[derive(Clone, Debug)]
pub enum ForInit {
    Declaration(Box<Statement>),
    Expression(Box<Expression>),
}

/// Left-hand side of for-in, for-of, for-await-of.
#[derive(Clone, Debug)]
pub enum ForInOfLhs {
    /// A variable declaration (`for (let x of ...)`)
    Declaration(Box<Statement>),
    /// An expression (`for (x in obj)`)
    Expression(Box<Expression>),
    /// A binding pattern (`for ({a, b} of ...)`)
    Pattern(BindingPattern),
}

// =============================================================================
// Assignment LHS
// =============================================================================

#[derive(Clone, Debug)]
pub enum AssignmentLhs {
    Expression(Box<Expression>),
    Pattern(BindingPattern),
}

// =============================================================================
// Scope data
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum LocalVarKind {
    Var = 0,
    LetOrConst = 1,
    Function = 2,
    ArgumentsObject = 3,
    CatchClauseParameter = 4,
}

#[derive(Clone, Debug)]
pub struct LocalVariable {
    pub name: Utf16String,
    pub kind: LocalVarKind,
}

/// Data shared by all scope-bearing nodes (Program, BlockStatement,
/// FunctionBody, SwitchStatement, SwitchCase).
///
/// AST nodes refer to scopes via `ScopeId` indices into `ScopeArena`,
/// so the AST itself is plain data — no `Rc`/`RefCell` and naturally
/// `Send + Sync` once parsing is done. The scope collector's two-phase
/// design (build tree during parse, analyze bottom-up afterwards)
/// ensures only one mutable borrow of a given `ScopeData` is ever
/// live at a time.
#[derive(Clone, Debug, Default)]
pub struct ScopeData {
    pub children: Vec<Statement>,
    pub local_variables: Vec<LocalVariable>,
    pub function_scope_data: Option<Box<FunctionScopeData>>,
    pub hoisted_functions: Vec<usize>,
    /// Function names hoisted from inner blocks via Annex B.3.3.
    /// The FDI should create `var` bindings initialized to `undefined`
    /// for each name.
    pub annexb_function_names: Vec<Utf16String>,
    // Scope analysis insights, written by the scope collector after analyze().
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub contains_direct_call_to_eval: bool,
    pub contains_access_to_arguments_object: bool,
}

/// Scope analysis data for function bodies, populated by the scope collector.
#[derive(Clone, Debug)]
pub struct FunctionScopeData {
    pub functions_to_initialize: Vec<FunctionToInit>,
    pub vars_to_initialize: Vec<VarToInit>,
    pub var_names: Vec<Utf16String>,
    pub has_function_named_arguments: bool,
    pub has_argument_parameter: bool,
    pub has_lexically_declared_arguments: bool,
    pub non_local_var_count: usize,
    pub non_local_var_count_for_parameter_expressions: usize,
}

/// Reference to a function declaration that needs hoisting/initialization.
/// Stores the index within the parent ScopeData.children.
#[derive(Clone, Debug)]
pub struct FunctionToInit {
    pub child_index: usize,
}

/// A resolved local binding: the operand type and index assigned by scope analysis.
#[derive(Clone, Copy, Debug)]
pub struct LocalBinding {
    pub local_type: LocalType,
    pub index: u32,
}

/// A `var` binding that needs initialization during function entry.
#[derive(Clone, Debug)]
pub struct VarToInit {
    pub name: Utf16String,
    pub is_parameter: bool,
    pub is_function_name: bool,
    /// If the scope analysis optimized this var to a local, stores the binding info.
    pub local: Option<LocalBinding>,
}

// =============================================================================
// Expression data structs (boxed variants)
// =============================================================================

#[derive(Clone, Debug)]
pub struct BinaryExprData {
    pub op: BinaryOp,
    pub lhs: Box<Expression>,
    pub rhs: Box<Expression>,
}

#[derive(Clone, Debug)]
pub struct LogicalExprData {
    pub op: LogicalOp,
    pub lhs: Box<Expression>,
    pub rhs: Box<Expression>,
}

#[derive(Clone, Debug)]
pub struct UpdateExprData {
    pub op: UpdateOp,
    pub argument: Box<Expression>,
    pub prefixed: bool,
}

#[derive(Clone, Debug)]
pub struct AssignmentExprData {
    pub op: AssignmentOp,
    pub lhs: AssignmentLhs,
    pub rhs: Box<Expression>,
}

#[derive(Clone, Debug)]
pub struct ConditionalExprData {
    pub test: Box<Expression>,
    pub consequent: Box<Expression>,
    pub alternate: Box<Expression>,
}

#[derive(Clone, Debug)]
pub struct MemberExprData {
    pub object: Box<Expression>,
    pub property: Box<Expression>,
    pub computed: bool,
}

#[derive(Clone, Debug)]
pub struct OptionalChainData {
    pub base: Box<Expression>,
    pub references: Vec<OptionalChainReference>,
}

#[derive(Clone, Debug)]
pub struct TaggedTemplateData {
    pub tag: Box<Expression>,
    pub template_literal: Box<Expression>,
}

#[derive(Clone, Debug)]
pub struct ImportCallData {
    pub specifier: Box<Expression>,
    pub options: Option<Box<Expression>>,
}

#[derive(Clone, Debug)]
pub struct YieldExprData {
    pub argument: Option<Box<Expression>>,
    pub is_yield_from: bool,
}

// =============================================================================
// Expression enum
// =============================================================================

#[derive(Clone, Debug)]
pub enum ExpressionKind {
    // Literals
    NumericLiteral(f64),
    StringLiteral(Box<Utf16String>),
    BooleanLiteral(bool),
    NullLiteral,
    BigIntLiteral(Box<String>),
    RegExpLiteral(Box<RegExpLiteralData>),

    // Identifiers
    Identifier(IdentifierId),
    PrivateIdentifier(Box<PrivateIdentifier>),

    // Operators
    Binary(Box<BinaryExprData>),
    Logical(Box<LogicalExprData>),
    Unary { op: UnaryOp, operand: Box<Expression> },
    Update(Box<UpdateExprData>),
    Assignment(Box<AssignmentExprData>),
    Conditional(Box<ConditionalExprData>),
    Sequence(Box<Vec<Expression>>),

    // Member access
    Member(Box<MemberExprData>),
    OptionalChain(Box<OptionalChainData>),

    // Calls
    Call(Box<CallExpressionData>),
    New(Box<CallExpressionData>),
    SuperCall(Box<SuperCallData>),

    // Spread
    Spread(Box<Expression>),

    // This / Super
    This,
    Super,

    // Functions
    Function(FunctionId),

    // Classes
    Class(Box<ClassData>),

    // Collections
    Array(Box<Vec<Option<Expression>>>),
    Object(Box<Vec<ObjectProperty>>),

    // Templates
    TemplateLiteral(Box<TemplateLiteralData>),
    TaggedTemplateLiteral(Box<TaggedTemplateData>),

    // Meta
    MetaProperty(MetaPropertyType),
    ImportCall(Box<ImportCallData>),

    // Async / Generator
    Yield(Box<YieldExprData>),
    Await(Box<Expression>),

    // Error recovery
    Error,
}

// =============================================================================
// Statement data structs
// =============================================================================

#[derive(Clone, Debug)]
pub struct IfStatementData {
    pub test: Box<Expression>,
    pub consequent: Box<Statement>,
    pub alternate: Option<Box<Statement>>,
}

#[derive(Clone, Debug)]
pub struct WhileStatementData {
    pub test: Box<Expression>,
    pub body: Box<Statement>,
}

#[derive(Clone, Debug)]
pub struct ForStatementData {
    pub init: Option<ForInit>,
    pub test: Option<Box<Expression>>,
    pub update: Option<Box<Expression>>,
    pub body: Box<Statement>,
}

#[derive(Clone, Debug)]
pub struct ForInOfStatementData {
    pub kind: ForInOfKind,
    pub lhs: ForInOfLhs,
    pub rhs: Box<Expression>,
    pub body: Box<Statement>,
}

#[derive(Clone, Debug)]
pub struct WithStatementData {
    pub object: Box<Expression>,
    pub body: Box<Statement>,
}

#[derive(Clone, Debug)]
pub struct LabelledStatementData {
    pub label: Utf16String,
    pub item: Box<Statement>,
}

#[derive(Clone, Debug)]
pub struct VariableDeclarationData {
    pub kind: DeclarationKind,
    pub declarations: Vec<VariableDeclarator>,
}

#[derive(Clone, Debug)]
pub struct FunctionDeclarationData {
    pub function_id: FunctionId,
    pub name: Option<IdentifierId>,
    pub kind: FunctionKind,
    pub is_hoisted: bool,
}

#[derive(Clone, Debug)]
pub struct ClassFieldInitializerData {
    pub expression: Box<Expression>,
    pub field_name: Utf16String,
}

// =============================================================================
// Statement enum
// =============================================================================

#[derive(Clone, Debug)]
pub enum StatementKind {
    // Basic
    Empty,
    Error,
    Expression(Box<Expression>),
    Debugger,

    // Blocks (carry ScopeData for scope analysis)
    Block(ScopeId),
    FunctionBody { scope: ScopeId, in_strict_mode: bool },
    Program(Box<ProgramData>),

    // Control flow
    If(Box<IfStatementData>),
    While(Box<WhileStatementData>),
    DoWhile(Box<WhileStatementData>),
    For(Box<ForStatementData>),
    ForInOf(Box<ForInOfStatementData>),
    Switch(Box<SwitchStatementData>),
    With(Box<WithStatementData>),
    Labelled(Box<LabelledStatementData>),

    // Jumps
    Break { target_label: Option<Utf16String> },
    Continue { target_label: Option<Utf16String> },
    Return(Option<Box<Expression>>),
    Throw(Box<Expression>),
    Try(Box<TryStatementData>),

    // Declarations
    VariableDeclaration(Box<VariableDeclarationData>),
    UsingDeclaration(Box<Vec<VariableDeclarator>>),
    FunctionDeclaration(Box<FunctionDeclarationData>),
    ClassDeclaration(Box<ClassData>),
    ErrorDeclaration,

    // Module
    Import(Box<ImportStatementData>),
    Export(Box<ExportStatementData>),

    // Special
    ClassFieldInitializer(Box<ClassFieldInitializerData>),
}

// =============================================================================
// Program data
// =============================================================================

#[derive(Clone, Debug)]
pub struct ProgramData {
    pub scope: ScopeId,
    pub program_type: ProgramType,
    pub is_strict_mode: bool,
    pub has_top_level_await: bool,
}
