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
//! - `Identifier` uses `Cell` fields for scope analysis results that are
//!   written after parsing (by the scope collector).
//! - Operator enums use `#[repr(u8)]` with ABI-compatible values for
//!   trivial FFI conversion.
//! - `ScopeData` is carried by block-like constructs (Program,
//!   BlockStatement, FunctionBody, etc.) and holds scope analysis results.

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::fmt;
use std::rc::Rc;

use crate::u32_from_usize;

// =============================================================================
// Function table (side table for FunctionData)
// =============================================================================

/// Opaque handle into the `FunctionTable`. Copy + Clone so AST nodes can
/// freely duplicate it without cloning the underlying `FunctionData`.
#[derive(Clone, Copy, Debug)]
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
pub struct FunctionTable(Vec<Option<Box<FunctionData>>>);

impl Default for FunctionTable {
    fn default() -> Self {
        Self::new()
    }
}

impl FunctionTable {
    pub fn new() -> Self {
        Self(Vec::new())
    }

    /// Insert a `FunctionData`, returning a `FunctionId` handle.
    pub fn insert(&mut self, data: FunctionData) -> FunctionId {
        let id = FunctionId(u32_from_usize(self.0.len()));
        self.0.push(Some(Box::new(data)));
        id
    }

    /// Borrow the data (for read-only access like ast_dump).
    ///
    /// # Panics
    /// Panics if the slot was already taken.
    pub fn get(&self, id: FunctionId) -> &FunctionData {
        self.0[id.0 as usize]
            .as_ref()
            .expect("FunctionTable::get: slot already taken")
    }

    /// Take ownership of the data (for codegen / GDI).
    ///
    /// # Panics
    /// Panics if the slot was already taken.
    pub fn take(&mut self, id: FunctionId) -> Box<FunctionData> {
        let idx = id.0 as usize;
        if idx >= self.0.len() {
            panic!(
                "FunctionTable::take: index {} out of bounds (table len {})",
                idx,
                self.0.len()
            );
        }
        self.0[idx]
            .take()
            .expect("FunctionTable::take: slot already taken")
    }

    /// Take ownership if the slot is still present; returns None if already taken.
    fn try_take(&mut self, id: FunctionId) -> Option<Box<FunctionData>> {
        self.0.get_mut(id.0 as usize).and_then(|slot| slot.take())
    }

    /// Insert a `Box<FunctionData>` at a specific id, growing the table if needed.
    fn insert_at(&mut self, id: FunctionId, data: Box<FunctionData>) {
        let idx = id.0 as usize;
        if idx >= self.0.len() {
            self.0.resize_with(idx + 1, || None);
        }
        self.0[idx] = Some(data);
    }

    /// Extract a subtable containing all `FunctionId`s reachable from the
    /// given function body and parameters. This recursively takes nested
    /// functions so that the returned subtable has everything needed to
    /// compile the function.
    pub fn extract_reachable(&mut self, data: &FunctionData) -> FunctionTable {
        let mut subtable = FunctionTable::new();
        // Walk parameters first (default values can contain functions).
        for param in &data.parameters {
            if let Some(ref default) = param.default_value {
                self.collect_from_expression(default, &mut subtable);
            }
            if let FunctionParameterBinding::BindingPattern(ref pat) = param.binding {
                self.collect_from_pattern(pat, &mut subtable);
            }
        }
        self.collect_from_statement(&data.body, &mut subtable);
        subtable
    }

    fn transfer(&mut self, id: FunctionId, result: &mut FunctionTable) {
        if let Some(data) = self.try_take(id) {
            // Walk parameters (default values / binding patterns can contain functions).
            for param in &data.parameters {
                if let Some(ref default) = param.default_value {
                    self.collect_from_expression(default, result);
                }
                if let FunctionParameterBinding::BindingPattern(ref pat) = param.binding {
                    self.collect_from_pattern(pat, result);
                }
            }
            // Walk the function body.
            self.collect_from_statement(&data.body, result);
            result.insert_at(id, data);
        }
    }

    fn collect_from_statement(&mut self, stmt: &Statement, result: &mut FunctionTable) {
        match &stmt.inner {
            StatementKind::FunctionDeclaration { function_id, .. } => {
                self.transfer(*function_id, result);
            }
            StatementKind::Expression(expr) => self.collect_from_expression(expr, result),
            StatementKind::Block(scope) | StatementKind::FunctionBody { scope, .. } => {
                for child in &scope.borrow().children {
                    self.collect_from_statement(child, result);
                }
            }
            StatementKind::Program(data) => {
                for child in &data.scope.borrow().children {
                    self.collect_from_statement(child, result);
                }
            }
            StatementKind::If {
                test,
                consequent,
                alternate,
            } => {
                self.collect_from_expression(test, result);
                self.collect_from_statement(consequent, result);
                if let Some(alt) = alternate {
                    self.collect_from_statement(alt, result);
                }
            }
            StatementKind::While { test, body } => {
                self.collect_from_expression(test, result);
                self.collect_from_statement(body, result);
            }
            StatementKind::DoWhile { test, body } => {
                self.collect_from_statement(body, result);
                self.collect_from_expression(test, result);
            }
            StatementKind::For {
                init,
                test,
                update,
                body,
            } => {
                if let Some(init) = init {
                    match init {
                        ForInit::Expression(expr) => self.collect_from_expression(expr, result),
                        ForInit::Declaration(decl) => self.collect_from_statement(decl, result),
                    }
                }
                if let Some(test) = test {
                    self.collect_from_expression(test, result);
                }
                if let Some(update) = update {
                    self.collect_from_expression(update, result);
                }
                self.collect_from_statement(body, result);
            }
            StatementKind::ForInOf { lhs, rhs, body, .. } => {
                match lhs {
                    ForInOfLhs::Declaration(decl) => self.collect_from_statement(decl, result),
                    ForInOfLhs::Expression(expr) => self.collect_from_expression(expr, result),
                    ForInOfLhs::Pattern(pattern) => self.collect_from_pattern(pattern, result),
                }
                self.collect_from_expression(rhs, result);
                self.collect_from_statement(body, result);
            }
            StatementKind::Switch(data) => {
                self.collect_from_expression(&data.discriminant, result);
                for case in &data.cases {
                    if let Some(ref test) = case.test {
                        self.collect_from_expression(test, result);
                    }
                    for child in &case.scope.borrow().children {
                        self.collect_from_statement(child, result);
                    }
                }
            }
            StatementKind::With { object, body } => {
                self.collect_from_expression(object, result);
                self.collect_from_statement(body, result);
            }
            StatementKind::Labelled { item, .. } => {
                self.collect_from_statement(item, result);
            }
            StatementKind::Return(arg) => {
                if let Some(expr) = arg {
                    self.collect_from_expression(expr, result);
                }
            }
            StatementKind::Throw(expr) => {
                self.collect_from_expression(expr, result);
            }
            StatementKind::Try(data) => {
                self.collect_from_statement(&data.block, result);
                if let Some(ref handler) = data.handler {
                    if let Some(CatchBinding::BindingPattern(ref pat)) = handler.parameter {
                        self.collect_from_pattern(pat, result);
                    }
                    self.collect_from_statement(&handler.body, result);
                }
                if let Some(ref finalizer) = data.finalizer {
                    self.collect_from_statement(finalizer, result);
                }
            }
            StatementKind::VariableDeclaration { declarations, .. } => {
                for decl in declarations {
                    self.collect_from_target(&decl.target, result);
                    if let Some(ref init) = decl.init {
                        self.collect_from_expression(init, result);
                    }
                }
            }
            StatementKind::UsingDeclaration { declarations } => {
                for decl in declarations {
                    self.collect_from_target(&decl.target, result);
                    if let Some(ref init) = decl.init {
                        self.collect_from_expression(init, result);
                    }
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                self.collect_from_class(class_data, result);
            }
            StatementKind::Export(data) => {
                if let Some(ref stmt) = data.statement {
                    self.collect_from_statement(stmt, result);
                }
            }
            StatementKind::ClassFieldInitializer { expression, .. } => {
                self.collect_from_expression(expression, result);
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

    fn collect_from_expression(&mut self, expr: &Expression, result: &mut FunctionTable) {
        match &expr.inner {
            ExpressionKind::Function(function_id) => {
                self.transfer(*function_id, result);
            }
            ExpressionKind::Class(class_data) => {
                self.collect_from_class(class_data, result);
            }
            ExpressionKind::Binary { lhs, rhs, .. } | ExpressionKind::Logical { lhs, rhs, .. } => {
                self.collect_from_expression(lhs, result);
                self.collect_from_expression(rhs, result);
            }
            ExpressionKind::Unary { operand, .. } => {
                self.collect_from_expression(operand, result);
            }
            ExpressionKind::Update { argument, .. } => {
                self.collect_from_expression(argument, result);
            }
            ExpressionKind::Assignment { lhs, rhs, .. } => {
                match lhs {
                    AssignmentLhs::Expression(expr) => self.collect_from_expression(expr, result),
                    AssignmentLhs::Pattern(pat) => self.collect_from_pattern(pat, result),
                }
                self.collect_from_expression(rhs, result);
            }
            ExpressionKind::Conditional {
                test,
                consequent,
                alternate,
            } => {
                self.collect_from_expression(test, result);
                self.collect_from_expression(consequent, result);
                self.collect_from_expression(alternate, result);
            }
            ExpressionKind::Sequence(exprs) => {
                for expr in exprs {
                    self.collect_from_expression(expr, result);
                }
            }
            ExpressionKind::Member {
                object, property, ..
            } => {
                self.collect_from_expression(object, result);
                self.collect_from_expression(property, result);
            }
            ExpressionKind::OptionalChain { base, references } => {
                self.collect_from_expression(base, result);
                for reference in references {
                    match reference {
                        OptionalChainReference::Call { arguments, .. } => {
                            for arg in arguments {
                                self.collect_from_expression(&arg.value, result);
                            }
                        }
                        OptionalChainReference::ComputedReference { expression, .. } => {
                            self.collect_from_expression(expression, result);
                        }
                        OptionalChainReference::MemberReference { .. }
                        | OptionalChainReference::PrivateMemberReference { .. } => {}
                    }
                }
            }
            ExpressionKind::Call(data) | ExpressionKind::New(data) => {
                self.collect_from_expression(&data.callee, result);
                for arg in &data.arguments {
                    self.collect_from_expression(&arg.value, result);
                }
            }
            ExpressionKind::SuperCall(data) => {
                for arg in &data.arguments {
                    self.collect_from_expression(&arg.value, result);
                }
            }
            ExpressionKind::Spread(expr) | ExpressionKind::Await(expr) => {
                self.collect_from_expression(expr, result);
            }
            ExpressionKind::Array(elements) => {
                for expr in elements.iter().flatten() {
                    self.collect_from_expression(expr, result);
                }
            }
            ExpressionKind::Object(properties) => {
                for prop in properties {
                    self.collect_from_expression(&prop.key, result);
                    if let Some(ref val) = prop.value {
                        self.collect_from_expression(val, result);
                    }
                }
            }
            ExpressionKind::TemplateLiteral(data) => {
                for expr in &data.expressions {
                    self.collect_from_expression(expr, result);
                }
            }
            ExpressionKind::TaggedTemplateLiteral {
                tag,
                template_literal,
            } => {
                self.collect_from_expression(tag, result);
                self.collect_from_expression(template_literal, result);
            }
            ExpressionKind::Yield { argument, .. } => {
                if let Some(expr) = argument {
                    self.collect_from_expression(expr, result);
                }
            }
            ExpressionKind::ImportCall { specifier, options } => {
                self.collect_from_expression(specifier, result);
                if let Some(opts) = options {
                    self.collect_from_expression(opts, result);
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

    fn collect_from_class(&mut self, class_data: &ClassData, result: &mut FunctionTable) {
        if let Some(ref super_class) = class_data.super_class {
            self.collect_from_expression(super_class, result);
        }
        if let Some(ref constructor) = class_data.constructor {
            self.collect_from_expression(constructor, result);
        }
        for element in &class_data.elements {
            match &element.inner {
                ClassElement::Method { key, function, .. } => {
                    self.collect_from_expression(key, result);
                    self.collect_from_expression(function, result);
                }
                ClassElement::Field {
                    key, initializer, ..
                } => {
                    self.collect_from_expression(key, result);
                    if let Some(init) = initializer {
                        self.collect_from_expression(init, result);
                    }
                }
                ClassElement::StaticInitializer { body } => {
                    self.collect_from_statement(body, result);
                }
            }
        }
    }

    fn collect_from_pattern(&mut self, pattern: &BindingPattern, result: &mut FunctionTable) {
        for entry in &pattern.entries {
            if let Some(BindingEntryName::Expression(expr)) = entry.name.as_ref() {
                self.collect_from_expression(expr, result);
            }
            if let Some(ref alias) = entry.alias {
                match alias {
                    BindingEntryAlias::BindingPattern(sub) => {
                        self.collect_from_pattern(sub, result)
                    }
                    BindingEntryAlias::MemberExpression(expr) => {
                        self.collect_from_expression(expr, result)
                    }
                    BindingEntryAlias::Identifier(_) => {}
                }
            }
            if let Some(ref init) = entry.initializer {
                self.collect_from_expression(init, result);
            }
        }
    }

    fn collect_from_target(
        &mut self,
        target: &VariableDeclaratorTarget,
        result: &mut FunctionTable,
    ) {
        if let VariableDeclaratorTarget::BindingPattern(pat) = target {
            self.collect_from_pattern(pat, result);
        }
    }
}

/// Bundles a `FunctionData` with a subtable of all nested functions
/// reachable from its body. Stored as the raw pointer in C++ SFDs.
pub struct FunctionPayload {
    pub data: FunctionData,
    pub function_table: FunctionTable,
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
/// Scope analysis results are stored in `Cell` fields because the scope
/// collector writes them after parsing through shared references.
#[derive(Clone, Debug)]
pub struct Identifier {
    pub range: SourceRange,
    pub name: Utf16String,
    // Scope analysis results — set by scope collector after parsing.
    pub local_type: Cell<Option<LocalType>>,
    pub local_index: Cell<u32>,
    pub is_global: Cell<bool>,
    pub is_inside_scope_with_eval: Cell<bool>,
    pub declaration_kind: Cell<Option<DeclarationKind>>,
}

impl Identifier {
    pub fn new(range: SourceRange, name: Utf16String) -> Self {
        Self {
            range,
            name,
            local_type: Cell::new(None),
            local_index: Cell::new(0),
            is_global: Cell::new(false),
            is_inside_scope_with_eval: Cell::new(false),
            declaration_kind: Cell::new(None),
        }
    }

    pub fn is_local(&self) -> bool {
        self.local_type.get().is_some()
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
    Identifier(Rc<Identifier>),
    BindingPattern(BindingPattern),
}

/// Shared data for FunctionDeclaration and FunctionExpression.
#[derive(Debug)]
pub struct FunctionData {
    pub name: Option<Rc<Identifier>>,
    pub source_text_start: u32,
    pub source_text_end: u32,
    pub body: Box<Statement>,
    pub parameters: Vec<FunctionParameter>,
    pub function_length: i32,
    pub kind: FunctionKind,
    pub is_strict_mode: bool,
    pub is_arrow_function: bool,
    pub parsing_insights: FunctionParsingInsights,
}

// =============================================================================
// Class support types
// =============================================================================

/// Shared data for ClassDeclaration and ClassExpression.
#[derive(Clone, Debug)]
pub struct ClassData {
    pub name: Option<Rc<Identifier>>,
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
    Identifier(Rc<Identifier>),
    Expression(Box<Expression>),
}

/// The "alias" (target) of a binding entry.
/// - `None`: name is the binding target (`{ x }` — x is both name and alias)
/// - `Identifier`: simple binding (`{ x: y }`)
/// - `BindingPattern`: nested destructuring (`{ x: { a, b } }`)
/// - `MemberExpression`: assignment target (`{ x: obj.property }`)
#[derive(Clone, Debug)]
pub enum BindingEntryAlias {
    Identifier(Rc<Identifier>),
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
    Identifier(Rc<Identifier>),
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
        identifier: Rc<Identifier>,
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

/// Unique handle to a compiled regex from C++.
///
/// The FFI handle has unique ownership semantics: it must be consumed
/// exactly once via `take()`. `Clone` panics to prevent accidental
/// sharing (the derive on `ExpressionKind` requires the trait to exist).
/// `Drop` frees the handle if it was never taken (e.g. on parse error).
pub struct CompiledRegex(Cell<*mut c_void>);

impl CompiledRegex {
    pub fn new(ptr: *mut c_void) -> Self {
        Self(Cell::new(ptr))
    }

    /// Take ownership of the compiled regex handle, leaving null behind
    /// so the destructor won't free it.
    pub fn take(&self) -> *mut c_void {
        self.0.replace(std::ptr::null_mut())
    }
}

impl Clone for CompiledRegex {
    fn clone(&self) -> Self {
        panic!("CompiledRegex cannot be cloned: FFI handle has unique ownership");
    }
}

impl Drop for CompiledRegex {
    fn drop(&mut self) {
        let ptr = self.0.get();
        if !ptr.is_null() {
            unsafe { rust_free_compiled_regex(ptr) };
        }
    }
}

impl fmt::Debug for CompiledRegex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CompiledRegex({:p})", self.0.get())
    }
}

#[derive(Clone, Debug)]
pub struct RegExpLiteralData {
    pub pattern: Utf16String,
    pub flags: Utf16String,
    pub compiled_regex: CompiledRegex,
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
    Identifier(Rc<Identifier>),
    BindingPattern(BindingPattern),
}

// =============================================================================
// Switch types
// =============================================================================

#[derive(Clone, Debug)]
pub struct SwitchStatementData {
    pub scope: Rc<RefCell<ScopeData>>,
    pub discriminant: Box<Expression>,
    pub cases: Vec<SwitchCase>,
}

#[derive(Clone, Debug)]
pub struct SwitchCase {
    pub range: SourceRange,
    pub scope: Rc<RefCell<ScopeData>>,
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
/// Wrapped in `Rc<RefCell<...>>` for interior mutability during the
/// scope collector's analysis phase. Borrow safety: the scope
/// collector's two-phase design (build tree during parsing, then
/// analyze bottom-up) ensures borrows never overlap — the analysis
/// phase only borrows one scope at a time in a bottom-up traversal.
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

impl ScopeData {
    pub fn new_shared() -> Rc<RefCell<Self>> {
        Rc::new(RefCell::new(Self::default()))
    }

    pub fn shared_with_children(children: Vec<Statement>) -> Rc<RefCell<Self>> {
        Rc::new(RefCell::new(Self {
            children,
            ..Default::default()
        }))
    }
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
// Expression enum
// =============================================================================

#[derive(Clone, Debug)]
pub enum ExpressionKind {
    // Literals
    NumericLiteral(f64),
    StringLiteral(Utf16String),
    BooleanLiteral(bool),
    NullLiteral,
    BigIntLiteral(String),
    RegExpLiteral(RegExpLiteralData),

    // Identifiers
    Identifier(Rc<Identifier>),
    PrivateIdentifier(PrivateIdentifier),

    // Operators
    Binary {
        op: BinaryOp,
        lhs: Box<Expression>,
        rhs: Box<Expression>,
    },
    Logical {
        op: LogicalOp,
        lhs: Box<Expression>,
        rhs: Box<Expression>,
    },
    Unary {
        op: UnaryOp,
        operand: Box<Expression>,
    },
    Update {
        op: UpdateOp,
        argument: Box<Expression>,
        prefixed: bool,
    },
    Assignment {
        op: AssignmentOp,
        lhs: AssignmentLhs,
        rhs: Box<Expression>,
    },
    Conditional {
        test: Box<Expression>,
        consequent: Box<Expression>,
        alternate: Box<Expression>,
    },
    Sequence(Vec<Expression>),

    // Member access
    Member {
        object: Box<Expression>,
        property: Box<Expression>,
        computed: bool,
    },
    OptionalChain {
        base: Box<Expression>,
        references: Vec<OptionalChainReference>,
    },

    // Calls
    Call(CallExpressionData),
    New(CallExpressionData),
    SuperCall(SuperCallData),

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
    Array(Vec<Option<Expression>>),
    Object(Vec<ObjectProperty>),

    // Templates
    TemplateLiteral(TemplateLiteralData),
    TaggedTemplateLiteral {
        tag: Box<Expression>,
        template_literal: Box<Expression>,
    },

    // Meta
    MetaProperty(MetaPropertyType),
    ImportCall {
        specifier: Box<Expression>,
        options: Option<Box<Expression>>,
    },

    // Async / Generator
    Yield {
        argument: Option<Box<Expression>>,
        is_yield_from: bool,
    },
    Await(Box<Expression>),

    // Error recovery
    Error,
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
    Block(Rc<RefCell<ScopeData>>),
    FunctionBody {
        scope: Rc<RefCell<ScopeData>>,
        in_strict_mode: bool,
    },
    Program(ProgramData),

    // Control flow
    If {
        test: Box<Expression>,
        consequent: Box<Statement>,
        alternate: Option<Box<Statement>>,
    },
    While {
        test: Box<Expression>,
        body: Box<Statement>,
    },
    DoWhile {
        test: Box<Expression>,
        body: Box<Statement>,
    },
    For {
        init: Option<ForInit>,
        test: Option<Box<Expression>>,
        update: Option<Box<Expression>>,
        body: Box<Statement>,
    },
    ForInOf {
        kind: ForInOfKind,
        lhs: ForInOfLhs,
        rhs: Box<Expression>,
        body: Box<Statement>,
    },
    Switch(SwitchStatementData),
    With {
        object: Box<Expression>,
        body: Box<Statement>,
    },
    Labelled {
        label: Utf16String,
        item: Box<Statement>,
    },

    // Jumps
    Break {
        target_label: Option<Utf16String>,
    },
    Continue {
        target_label: Option<Utf16String>,
    },
    Return(Option<Box<Expression>>),
    Throw(Box<Expression>),
    Try(TryStatementData),

    // Declarations
    VariableDeclaration {
        kind: DeclarationKind,
        declarations: Vec<VariableDeclarator>,
    },
    UsingDeclaration {
        declarations: Vec<VariableDeclarator>,
    },
    FunctionDeclaration {
        function_id: FunctionId,
        name: Option<Rc<Identifier>>,
        kind: FunctionKind,
        is_hoisted: Cell<bool>,
    },
    ClassDeclaration(Box<ClassData>),
    ErrorDeclaration,

    // Module
    Import(ImportStatementData),
    Export(ExportStatementData),

    // Special
    ClassFieldInitializer {
        expression: Box<Expression>,
        field_name: Utf16String,
    },
}

// =============================================================================
// Program data
// =============================================================================

#[derive(Clone, Debug)]
pub struct ProgramData {
    pub scope: Rc<RefCell<ScopeData>>,
    pub program_type: ProgramType,
    pub is_strict_mode: bool,
    pub has_top_level_await: bool,
}
