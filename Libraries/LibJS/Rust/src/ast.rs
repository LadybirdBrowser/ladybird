/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The syntax tree of a program, which the parser writes as flat columns and everything after it reads through typed
//! views. A view is a small `Copy` handle of a tree and a node: `expression.kind()` and `statement.kind()` return a
//! borrowing enum to match on, whose data structs hold further views. Nothing is copied out of the tree, and a literal
//! is only read from the source once something asks for its value.

use std::marker::PhantomData;
use std::sync::Arc;

use ak::Utf16String;

use crate::parser::FunctionRow;
use crate::parser::OuterResolutions;
use crate::parser::ScriptData;
use crate::parser::columns::Ast;
use crate::parser::columns::NONE;
use crate::parser::columns::function_flag;
use crate::parser::columns::tag;
use crate::parser::literals;
use crate::parser::literals::EscapeMode;
use crate::parser::token::N_PROTO;
use crate::parser::token::T;
use crate::u32_from_usize;
use crate::utf16::Utf16StringBuilder;
use crate::utf16::Utf16View;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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

/// Scope analysis result: how this identifier is resolved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LocalType {
    Argument,
    Variable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ClassMethodKind {
    Method = 0,
    Getter = 1,
    Setter = 2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BindingPatternKind {
    Array,
    Object,
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OptionalChainMode {
    Optional,
    NotOptional,
}

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ForInOfKind {
    ForIn,
    ForOf,
    ForAwaitOf,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum LocalVarKind {
    Var = 0,
    LetOrConst = 1,
    Function = 2,
    ArgumentsObject = 3,
    CatchClauseParameter = 4,
    /// A parameter that a binding pattern binds.
    PatternParameter = 5,
}

// =============================================================================
// Names
// =============================================================================

/// An interned name of a [`SyntaxTree`]. Equal names of a tree have equal ids.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub struct Name(pub(crate) u32);

#[derive(Clone, Copy)]
struct NameSpan {
    start: u32,
    length: u32,
    is_ascii: bool,
}

/// The text of every name of a source: one byte per code unit for ASCII names, which is nearly all of them, and UTF-16
/// for the others, along with the UTF-8 that a parser knows them by.
#[derive(Default)]
pub(crate) struct NameTable {
    spans: Vec<NameSpan>,
    ascii_text: Vec<u8>,
    utf16_text: Vec<u16>,
    utf8_text: Vec<u8>,
    /// Where the UTF-8 of each non-ASCII name starts and ends, in the order of the names.
    utf8_spans: Vec<(u32, u32)>,
}

impl NameTable {
    pub(crate) fn memory_size(&self) -> usize {
        self.spans.capacity() * size_of::<NameSpan>()
            + self.ascii_text.capacity()
            + self.utf16_text.capacity() * size_of::<u16>()
            + self.utf8_text.capacity()
            + self.utf8_spans.capacity() * size_of::<(u32, u32)>()
    }

    pub(crate) fn push(&mut self, text: &[u8]) {
        let is_ascii = text.is_ascii();
        let start = if is_ascii {
            self.ascii_text.len()
        } else {
            self.utf16_text.len()
        };
        if is_ascii {
            self.ascii_text.extend_from_slice(text);
        } else {
            self.utf16_text.extend(String::from_utf8_lossy(text).encode_utf16());
            let utf8_start = u32_from_usize(self.utf8_text.len());
            self.utf8_text.extend_from_slice(text);
            self.utf8_spans.push((utf8_start, u32_from_usize(self.utf8_text.len())));
        }
        let end = if is_ascii {
            self.ascii_text.len()
        } else {
            self.utf16_text.len()
        };
        self.spans.push(NameSpan {
            start: u32_from_usize(start),
            length: u32_from_usize(end - start),
            is_ascii,
        });
    }

    /// The names as a parser knows them, in the order of their ids.
    pub(crate) fn parser_names(&self) -> impl ExactSizeIterator<Item = &[u8]> {
        let mut utf8_spans = self.utf8_spans.iter();
        self.spans.iter().map(move |span| {
            if span.is_ascii {
                &self.ascii_text[span.start as usize..(span.start + span.length) as usize]
            } else {
                let &(start, end) = utf8_spans.next().expect("every non-ASCII name has its UTF-8");
                &self.utf8_text[start as usize..end as usize]
            }
        })
    }

    fn text(&self, name: Name) -> Utf16View<'_> {
        let span = self.spans[name.0 as usize];
        let (start, end) = (span.start as usize, (span.start + span.length) as usize);
        if span.is_ascii {
            Utf16View::Ascii(&self.ascii_text[start..end])
        } else {
            Utf16View::Utf16(&self.utf16_text[start..end])
        }
    }
}

// =============================================================================
// Resolution
// =============================================================================

/// What scope analysis found out about one identifier, packed into a `u32` of the tree's resolution column. An
/// identifier that it did not look at has the value zero.
#[derive(Clone, Copy, Default)]
pub(crate) struct Resolution(pub(crate) u32);

impl Resolution {
    const LOCAL_TYPE_MASK: u32 = 0b11;
    const LOCAL_ARGUMENT: u32 = 1;
    const LOCAL_VARIABLE: u32 = 2;
    const DECLARATION_KIND_SHIFT: u32 = 2;
    const DECLARATION_KIND_MASK: u32 = 0b11 << Self::DECLARATION_KIND_SHIFT;
    const IS_GLOBAL: u32 = 1 << 4;
    const IS_INSIDE_SCOPE_WITH_EVAL: u32 = 1 << 5;
    const IS_HOISTED: u32 = 1 << 6;
    const IS_DECLARED_FUNCTION: u32 = 1 << 7;
    const IS_INSIDE_TREE_SCOPE_WITH_EVAL: u32 = 1 << 8;
    const LOCAL_INDEX_SHIFT: u32 = 9;
    /// A binding with a higher index stays in its environment.
    pub(crate) const MAX_LOCAL_INDEX: u32 = u32::MAX >> Self::LOCAL_INDEX_SHIFT;

    pub(crate) fn set_local(&mut self, binding: LocalBinding) {
        let local_type = match binding.local_type {
            LocalType::Argument => Self::LOCAL_ARGUMENT,
            LocalType::Variable => Self::LOCAL_VARIABLE,
        };
        self.0 = (self.0 & !Self::LOCAL_TYPE_MASK & ((1 << Self::LOCAL_INDEX_SHIFT) - 1))
            | local_type
            | (binding.index << Self::LOCAL_INDEX_SHIFT);
    }

    pub(crate) fn set_declaration_kind(&mut self, kind: DeclarationKind) {
        self.0 = (self.0 & !Self::DECLARATION_KIND_MASK) | ((kind as u32) << Self::DECLARATION_KIND_SHIFT);
    }

    pub(crate) fn set_is_global(&mut self) {
        self.0 |= Self::IS_GLOBAL;
    }

    pub(crate) fn set_is_inside_scope_with_eval(&mut self) {
        self.0 |= Self::IS_INSIDE_SCOPE_WITH_EVAL;
    }

    pub(crate) fn is_inside_scope_with_eval(self) -> bool {
        self.0 & Self::IS_INSIDE_SCOPE_WITH_EVAL != 0
    }

    /// Whether a scope with an eval around the identifier is one of the tree being resolved.
    pub(crate) fn set_is_inside_tree_scope_with_eval(&mut self) {
        self.0 |= Self::IS_INSIDE_TREE_SCOPE_WITH_EVAL;
    }

    pub(crate) fn is_inside_tree_scope_with_eval(self) -> bool {
        self.0 & Self::IS_INSIDE_TREE_SCOPE_WITH_EVAL != 0
    }

    pub(crate) fn clear_is_global(&mut self) {
        self.0 &= !Self::IS_GLOBAL;
    }

    pub(crate) fn set_is_hoisted(&mut self) {
        self.0 |= Self::IS_HOISTED;
    }

    pub(crate) fn is_hoisted(self) -> bool {
        self.0 & Self::IS_HOISTED != 0
    }

    /// Marks the resolution of a function node as being that of the name of its declaration.
    pub(crate) fn set_is_declared_function(&mut self) {
        self.0 |= Self::IS_DECLARED_FUNCTION;
    }

    pub(crate) fn is_declared_function(self) -> bool {
        self.0 & Self::IS_DECLARED_FUNCTION != 0
    }

    pub(crate) fn local_type(self) -> Option<LocalType> {
        match self.0 & Self::LOCAL_TYPE_MASK {
            Self::LOCAL_ARGUMENT => Some(LocalType::Argument),
            Self::LOCAL_VARIABLE => Some(LocalType::Variable),
            _ => None,
        }
    }

    pub(crate) fn local_index(self) -> u32 {
        self.0 >> Self::LOCAL_INDEX_SHIFT
    }

    pub(crate) fn declaration_kind(self) -> Option<DeclarationKind> {
        match (self.0 & Self::DECLARATION_KIND_MASK) >> Self::DECLARATION_KIND_SHIFT {
            1 => Some(DeclarationKind::Var),
            2 => Some(DeclarationKind::Let),
            3 => Some(DeclarationKind::Const),
            _ => None,
        }
    }

    pub(crate) fn is_global(self) -> bool {
        self.0 & Self::IS_GLOBAL != 0
    }
}

/// A local that scope analysis gave a function, a static block or a program.
#[derive(Clone, Copy)]
pub struct LocalVariable {
    pub name: Name,
    pub kind: LocalVarKind,
    pub is_mutable: bool,
    /// Where the block that declares it starts and ends, if a block does.
    pub(crate) scope_offsets: Option<(u32, u32)>,
    /// How many loops of its function the scope that declares it is in the body of, which is how often that scope can
    /// start again.
    pub(crate) enclosing_loops: u32,
}

/// A resolved local binding: the operand type and index assigned by scope analysis.
#[derive(Clone, Copy, Debug)]
pub struct LocalBinding {
    pub local_type: LocalType,
    pub index: u32,
}

/// A `var` binding that needs initialization during function entry.
#[derive(Clone, Copy)]
pub struct VarToInit {
    pub name: Name,
    pub is_parameter: bool,
    pub is_function_name: bool,
    /// If the scope analysis optimized this var to a local, stores the binding info.
    pub local: Option<LocalBinding>,
}

/// A function declaration that needs hoisting: its index among the children of the scope.
#[derive(Clone, Copy)]
pub struct FunctionToInit {
    pub child_index: u32,
}

/// A range of one of the columns of scope analysis results.
#[derive(Clone, Copy, Default)]
pub(crate) struct ColumnRange {
    pub(crate) start: u32,
    pub(crate) end: u32,
}

impl ColumnRange {
    pub(crate) fn of<T>(self, column: &[T]) -> &[T] {
        &column[self.start as usize..self.end as usize]
    }
}

/// What scope analysis found out about a function, a static block or a program.
#[derive(Clone, Copy, Default)]
pub(crate) struct ScopeRow {
    pub(crate) local_variables: ColumnRange,
    pub(crate) vars_to_initialize: ColumnRange,
    pub(crate) functions_to_initialize: ColumnRange,
    pub(crate) annexb_function_names: ColumnRange,
    pub(crate) non_local_var_count: u32,
    pub(crate) non_local_var_count_for_parameter_expressions: u32,
    pub(crate) has_function_scope_data: bool,
    pub(crate) has_function_named_arguments: bool,
    pub(crate) has_argument_parameter: bool,
    pub(crate) has_lexically_declared_arguments: bool,
    pub(crate) uses_this: bool,
    pub(crate) uses_this_from_environment: bool,
    pub(crate) contains_direct_call_to_eval: bool,
    pub(crate) contains_access_to_arguments_object: bool,
    /// Whether the code of the scope, which is everything but what its declaration instantiation does, can change the
    /// running environment. The code of nested functions has its own.
    pub(crate) creates_environment: bool,
}

/// The results of scope analysis, which are the same for the whole life of a tree.
#[derive(Default)]
pub(crate) struct ScopeTable {
    /// One per function of the function index, then one per static block, then the program's.
    pub(crate) rows: Vec<ScopeRow>,
    pub(crate) local_variables: Vec<LocalVariable>,
    pub(crate) vars_to_initialize: Vec<VarToInit>,
    pub(crate) functions_to_initialize: Vec<FunctionToInit>,
    pub(crate) annexb_function_names: Vec<Name>,
}

impl ScopeTable {
    pub(crate) fn clear(&mut self) {
        self.rows.clear();
        self.local_variables.clear();
        self.vars_to_initialize.clear();
        self.functions_to_initialize.clear();
        self.annexb_function_names.clear();
    }
}

// =============================================================================
// The tree
// =============================================================================

/// What a compile job works from: the tree of the code it compiles, and the results of scope analysis on that code. The
/// tree of a program holds its top-level code and the functions it is likely to call right away, and the tree of a
/// function holds that function, in both cases with nothing but a function node for every other function nested in it.
pub struct SyntaxTree {
    /// What stays of the source once the tree is gone.
    pub(crate) script: Arc<ScriptData>,
    pub(crate) ast: Ast,
    pub(crate) root: u32,
    pub(crate) first_token_start: u32,
    pub(crate) is_strict_mode: bool,
    pub(crate) has_top_level_await: bool,
    pub(crate) eval_referenced_private_names: Vec<Name>,
    /// The resolution of the identifier at each node, if there is one.
    pub(crate) resolutions: Vec<Resolution>,
    pub(crate) scopes: ScopeTable,
    /// The row of the program in `scopes`.
    pub(crate) program_scope_row: u32,
    /// The row in `scopes` of each function node of the tree whose code the tree holds, sorted by node.
    pub(crate) function_scope_rows: Vec<(u32, u32)>,
    /// What scope analysis found out for the functions that the code of the tree goes on to compile.
    pub(crate) outer_resolutions: Option<OuterResolutions>,
}

impl SyntaxTree {
    fn tag(&self, node: u32) -> u8 {
        self.ast.tag[node as usize]
    }

    pub(crate) fn a(&self, node: u32) -> u32 {
        self.ast.a[node as usize]
    }

    pub(crate) fn b(&self, node: u32) -> u32 {
        self.ast.b[node as usize]
    }

    fn pos(&self, node: u32) -> u32 {
        self.ast.pos[node as usize]
    }

    fn extra(&self, index: u32) -> u32 {
        self.ast.extra[index as usize]
    }

    fn list(&self, start: u32) -> &[u32] {
        let start = start as usize;
        let count = self.ast.extra[start] as usize;
        &self.ast.extra[start + 1..start + 1 + count]
    }

    /// Where the code after a block starts, given the list of the block's statements.
    /// Where the block statement at `node` ends.
    pub(crate) fn end_of_block(&self, node: u32) -> u32 {
        self.block_end(self.a(node))
    }

    fn block_end(&self, list: u32) -> u32 {
        self.extra(list + 1 + self.extra(list))
    }

    fn range(&self, offset: u32) -> SourceRange {
        let position = self.script.positions.lines.position(offset);
        SourceRange {
            start: position,
            end: position,
        }
    }

    pub(crate) fn range_between(&self, start: u32, end: u32) -> SourceRange {
        SourceRange {
            start: self.script.positions.lines.position(start),
            end: self.script.positions.lines.position(end),
        }
    }

    fn source_text(&self) -> Utf16View<'_> {
        Utf16View::of_string(&self.script.positions.source)
    }

    pub(crate) fn source_units(&self, start: u32, end: u32) -> Utf16View<'_> {
        self.source_text()
            .substring_view(start as usize, (end - start) as usize)
    }

    pub fn name_text(&self, name: Name) -> Utf16View<'_> {
        self.script.names.text(name)
    }

    fn resolution(&self, node: u32) -> Resolution {
        self.resolutions[node as usize]
    }

    fn identifier(&self, name: Name, offset: u32, node: u32) -> Identifier<'_> {
        Identifier::new(self, name, offset, self.resolution(node))
    }

    /// An identifier that scope analysis does not look at, such as a property name.
    fn plain_identifier(&self, name: Name, offset: u32) -> Identifier<'_> {
        Identifier::new(self, name, offset, Resolution::default())
    }

    pub fn program_type(&self) -> ProgramType {
        self.script.program_type
    }

    /// The range of the block that declares a local, if a block does.
    pub fn local_variable_scope_range(&self, variable: &LocalVariable) -> Option<SourceRange> {
        variable
            .scope_offsets
            .map(|(start, end)| self.range_between(start, end))
    }

    pub fn is_strict_mode(&self) -> bool {
        self.is_strict_mode
    }

    pub fn has_top_level_await(&self) -> bool {
        self.has_top_level_await
    }

    pub fn eval_referenced_private_names(&self) -> Vec<Utf16String> {
        self.eval_referenced_private_names
            .iter()
            .map(|&name| {
                let mut text = Utf16StringBuilder::new();
                text.append_ascii("#");
                text.append(self.name_text(name));
                text.to_utf16_string()
            })
            .collect()
    }

    /// The statement of the whole program.
    pub fn program(&self) -> Statement<'_> {
        Statement::new(self, self.root)
    }

    fn scope_row(&self, row: u32) -> &ScopeRow {
        &self.scopes.rows[row as usize]
    }

    pub(crate) fn function_scope(&self, function: u32) -> &ScopeRow {
        let index = self
            .function_scope_rows
            .binary_search_by_key(&function, |&(node, _)| node)
            .expect("the tree holds the code of the function");
        self.scope_row(self.function_scope_rows[index].1)
    }

    fn string_units(&self, node: u32) -> Utf16View<'_> {
        self.source_units(self.pos(node) + 1, self.a(node) - 1)
    }

    fn string_value(&self, node: u32) -> Utf16String {
        literals::process_escape_sequences_impl(self.string_units(node), EscapeMode::StringLiteral).value
    }

    fn is_proto_key(&self, key: u32) -> bool {
        match self.tag(key) {
            tag::IDENT => self.a(key) == N_PROTO,
            tag::STR => {
                self.string_units(key).length_in_code_units() >= 9
                    && self.string_value(key).to_utf16().as_ref() == utf16!("__proto__")
            }
            _ => false,
        }
    }

    /// The offset of the property name after the `.` or `?.` at `offset`.
    fn name_after(&self, offset: u32) -> u32 {
        let source = self.source_text();
        let at = |index: usize| source.code_unit_at(index);
        let mut position = offset as usize + if at(offset as usize) == 0x3F { 2 } else { 1 };
        loop {
            let code_unit = at(position);
            if matches!(
                code_unit,
                0x20 | 0x09 | 0x0A | 0x0D | 0x0B | 0x0C | 0xA0 | 0xFEFF | 0x2028 | 0x2029
            ) || matches!(code_unit, 0x1680 | 0x2000..=0x200A | 0x202F | 0x205F | 0x3000)
            {
                position += 1;
            } else if code_unit == 0x2F && at(position + 1) == 0x2F {
                while !matches!(at(position), 0x0A | 0x0D | 0x2028 | 0x2029) {
                    position += 1;
                }
            } else if code_unit == 0x2F && at(position + 1) == 0x2A {
                position += 2;
                while !(at(position) == 0x2A && at(position + 1) == 0x2F) {
                    position += 1;
                }
                position += 2;
            } else {
                return u32_from_usize(position);
            }
        }
    }

    fn private_identifier(&self, name: u32, offset: u32) -> PrivateIdentifier<'_> {
        PrivateIdentifier {
            tree: self,
            name: Name(name),
            offset,
        }
    }

    fn class(&self, node: u32) -> ClassData<'_> {
        let name = self.a(node);
        let extra = self.b(node);
        let super_class = self.extra(extra);
        let constructor = self.extra(extra + 3);
        let start = self.pos(node);
        let elements = self.list(self.extra(extra + 1));
        let constructor_index = elements.iter().position(|&element| {
            constructor != 0 && self.tag(element) == tag::METHOD && self.b(element) == constructor
        });
        ClassData {
            name: (name != NONE).then(|| self.identifier(Name(name), start, node)),
            source_text_start: start,
            source_text_end: self.extra(extra + 4),
            constructor: if constructor_index.is_some() {
                Expression::new(self, constructor)
            } else {
                Expression::with_form(self, node, ExpressionForm::DefaultConstructor)
            },
            super_class: (super_class != 0).then(|| Expression::new(self, super_class)),
            elements: ClassElements {
                tree: self,
                elements,
                constructor_index,
                class_start: start,
            },
        }
    }

    fn module_request(&self, source: u32) -> ModuleRequest {
        let mut attributes = Vec::new();
        if self.b(source) != 0 {
            for pair in self.list(self.b(source)).chunks(2) {
                attributes.push(ImportAttribute {
                    key: self.name_text(Name(pair[0])).into(),
                    value: self.name_text(Name(pair[1])).into(),
                });
            }
        }
        ModuleRequest {
            module_specifier: self.string_value(source),
            attributes,
        }
    }

    /// The value of a string literal that the tree holds as an interned name.
    fn specifier_name(&self, node: u32) -> Utf16String {
        self.name_text(Name(self.a(node))).into()
    }

    fn pattern_names(&self, node: u32, names: &mut Vec<Utf16String>) {
        match self.tag(node) {
            tag::IDENT => names.push(self.name_text(Name(self.a(node))).into()),
            tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => {
                for &item in self.list(self.a(node)) {
                    self.pattern_names(item, names);
                }
            }
            tag::PROPERTY | tag::COMPUTED_PROPERTY | tag::SHORTHAND => self.pattern_names(self.b(node), names),
            tag::REST | tag::ASSIGN_PATTERN | tag::ASSIGN_BASE => self.pattern_names(self.a(node), names),
            _ => {}
        }
    }

    /// Whether the pattern at `node` has a computed key or an initializer, nested patterns included.
    pub(crate) fn pattern_contains_expression(&self, node: u32) -> bool {
        self.list(self.a(node)).iter().any(|&item| {
            let item_tag = self.tag(item);
            if item_tag == tag::COMPUTED_PROPERTY {
                return true;
            }
            let mut value = match item_tag {
                tag::HOLE => return false,
                tag::REST => self.a(item),
                tag::PROPERTY | tag::SHORTHAND => self.b(item),
                _ => item,
            };
            if matches!(self.tag(value), tag::ASSIGN_PATTERN | tag::ASSIGN_BASE) {
                if self.b(value) != 0 {
                    return true;
                }
                value = self.a(value);
            }
            matches!(self.tag(value), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN)
                && self.pattern_contains_expression(value)
        })
    }

    fn function_kind(&self, function: u32) -> FunctionKind {
        let flags = self.script.index.flags[self.b(function) as usize];
        FunctionKind::from_async_generator(flags & function_flag::ASYNC != 0, flags & function_flag::GENERATOR != 0)
    }

    fn function_declaration(&self, function: u32, name: u32) -> FunctionDeclarationData<'_> {
        let name = if name == NONE {
            self.script.default_export_name
        } else {
            Name(name)
        };
        FunctionDeclarationData {
            function_id: FunctionId::function(function),
            name: Some(self.identifier(name, self.pos(function), function)),
            kind: self.function_kind(function),
            is_hoisted: self.resolution(function).is_hoisted(),
        }
    }

    /// What scope analysis found out about the program.
    pub(crate) fn program_scope(&self) -> &ScopeRow {
        self.scope_row(self.program_scope_row)
    }

    /// The row of the function behind `id`.
    pub fn function_row(&self, id: FunctionId) -> FunctionRow {
        let (node, extra_of_row) = match id.source {
            FunctionSource::Function => return FunctionRow(self.b(id.node)),
            FunctionSource::DefaultConstructor => return FunctionRow(self.extra(self.b(id.node) + 5)),
            FunctionSource::ClassFieldInitializer => (self.b(id.node), 1),
            FunctionSource::StaticInitializer { .. } => (id.node, 1),
        };
        FunctionRow(self.extra(self.b(node) + extra_of_row))
    }

    /// The function behind `id`. Only what is known without its tree is known of a function that the tree does not hold
    /// the code of.
    pub fn function(&self, id: FunctionId) -> FunctionData<'_> {
        let node = id.node;
        match id.source {
            FunctionSource::Function => {
                let tree = self.a(node);
                let row = self.b(node) as usize;
                let flags = self.script.index.flags[row];
                let name = self.script.index.name[row];
                let start = self.pos(node);
                // NB: The parameters of a function that is not in the tree are not known to its surroundings.
                let parameters = if tree == 0 { &[] } else { self.list(self.extra(tree)) };
                let first_position = parameters.first().map_or(0, |&parameter| match self.tag(parameter) {
                    tag::ASSIGN_PATTERN | tag::ASSIGN_BASE => self.pos(self.a(parameter)),
                    _ => self.pos(parameter),
                });
                let is_method = flags & function_flag::METHOD != 0;
                let name = if self.resolution(node).is_declared_function() {
                    let declared_name = self.script.index.name[row];
                    let name = if declared_name == NONE {
                        self.script.default_export_name
                    } else {
                        Name(declared_name)
                    };
                    Some(self.identifier(name, start, node))
                } else if name != NONE && !is_method {
                    Some(self.plain_identifier(Name(name), start))
                } else {
                    None
                };
                FunctionData {
                    name,
                    source_text_start: self.script.index.start[row],
                    source_text_end: self.script.index.end[row],
                    body: Statement::with_form(self, node, StatementForm::FunctionBody),
                    parameters: FunctionParameters {
                        tree: self,
                        parameters,
                        first_position,
                        rest_of_default_constructor: None,
                    },
                    parameter_count: self.script.index.parameter_count[row],
                    has_simple_parameter_list: flags & function_flag::SIMPLE_PARAMETERS != 0,
                    function_length: i32::try_from(self.script.index.length[row])
                        .expect("a function has a sane number of parameters"),
                    kind: FunctionKind::from_async_generator(
                        flags & function_flag::ASYNC != 0,
                        flags & function_flag::GENERATOR != 0,
                    ),
                    is_strict_mode: flags & function_flag::STRICT != 0,
                    is_arrow_function: flags & function_flag::ARROW != 0,
                    parsing_insights: FunctionParsingInsights {
                        uses_this: flags & function_flag::INSIGHT_USES_THIS != 0,
                        uses_this_from_environment: flags & function_flag::INSIGHT_THIS_FROM_ENVIRONMENT != 0,
                        contains_direct_call_to_eval: flags & function_flag::DIRECT_EVAL != 0,
                        might_need_arguments_object: flags & function_flag::MIGHT_NEED_ARGUMENTS != 0,
                    },
                }
            }
            FunctionSource::DefaultConstructor => {
                let name = self.a(node);
                let extra = self.b(node);
                let start = self.pos(node);
                let has_super_class = self.extra(extra) != 0;
                self.synthetic_function(
                    (name != NONE).then(|| self.plain_identifier(Name(name), start)),
                    start,
                    self.extra(extra + 4),
                    Statement::with_form(self, node, StatementForm::DefaultConstructorBody),
                    FunctionParameters {
                        tree: self,
                        parameters: &[],
                        first_position: 0,
                        rest_of_default_constructor: has_super_class.then_some(start),
                    },
                )
            }
            FunctionSource::ClassFieldInitializer => {
                let initializer = self.b(node);
                let expression = Expression::new(self, self.a(initializer));
                self.synthetic_function(
                    None,
                    expression.range().start.offset,
                    self.pos(initializer),
                    Statement::with_form(self, node, StatementForm::ClassFieldInitializer),
                    FunctionParameters::empty(self),
                )
            }
            FunctionSource::StaticInitializer { class_start } => self.synthetic_function(
                None,
                class_start,
                self.block_end(self.a(node)),
                Statement::with_form(self, node, StatementForm::StaticBlockBody { class_start }),
                FunctionParameters::empty(self),
            ),
        }
    }

    /// A function of a class that the code does not spell out, which is strict like everything in a class.
    fn synthetic_function<'a>(
        &'a self,
        name: Option<Identifier<'a>>,
        source_text_start: u32,
        source_text_end: u32,
        body: Statement<'a>,
        parameters: FunctionParameters<'a>,
    ) -> FunctionData<'a> {
        FunctionData {
            name,
            source_text_start,
            source_text_end,
            body,
            parameters,
            parameter_count: u32_from_usize(parameters.len()),
            has_simple_parameter_list: parameters.rest_of_default_constructor.is_none(),
            function_length: 0,
            kind: FunctionKind::Normal,
            is_strict_mode: true,
            is_arrow_function: false,
            parsing_insights: FunctionParsingInsights {
                uses_this: true,
                uses_this_from_environment: true,
                ..FunctionParsingInsights::default()
            },
        }
    }
}

// =============================================================================
// Lists
// =============================================================================

/// Something that reads as a sequence of views.
pub trait Sequence: Copy {
    type Item;
    fn len(&self) -> usize;
    fn at(&self, index: usize) -> Self::Item;

    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn iter(&self) -> SequenceIterator<Self> {
        SequenceIterator {
            sequence: *self,
            index: 0,
            end: self.len(),
        }
    }

    fn first(&self) -> Option<Self::Item> {
        (!self.is_empty()).then(|| self.at(0))
    }

    fn last(&self) -> Option<Self::Item> {
        (!self.is_empty()).then(|| self.at(self.len() - 1))
    }
}

#[derive(Clone)]
pub struct SequenceIterator<S> {
    sequence: S,
    index: usize,
    end: usize,
}

impl<S: Sequence> Iterator for SequenceIterator<S> {
    type Item = S::Item;

    fn next(&mut self) -> Option<S::Item> {
        if self.index == self.end {
            return None;
        }
        self.index += 1;
        Some(self.sequence.at(self.index - 1))
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.end - self.index, Some(self.end - self.index))
    }
}

impl<S: Sequence> ExactSizeIterator for SequenceIterator<S> {}

impl<S: Sequence> DoubleEndedIterator for SequenceIterator<S> {
    fn next_back(&mut self) -> Option<S::Item> {
        if self.index == self.end {
            return None;
        }
        self.end -= 1;
        Some(self.sequence.at(self.end))
    }
}

/// Lets `for item in &sequence` and `for item in sequence` work.
macro_rules! impl_sequence_into_iterator {
    ($($type:ident),+) => {
        $(
            impl<'a> IntoIterator for $type<'a> {
                type Item = <Self as Sequence>::Item;
                type IntoIter = SequenceIterator<Self>;
                fn into_iter(self) -> Self::IntoIter {
                    self.iter()
                }
            }

            impl<'a> IntoIterator for &$type<'a> {
                type Item = <$type<'a> as Sequence>::Item;
                type IntoIter = SequenceIterator<$type<'a>>;
                fn into_iter(self) -> Self::IntoIter {
                    self.iter()
                }
            }
        )+
    };
}

/// A view that one list item of the tree turns into.
pub trait ListItem<'a>: Sized {
    type Context: Copy;
    fn read(tree: &'a SyntaxTree, node: u32, context: Self::Context) -> Self;
}

/// A list of the tree, read as views of type `T`.
pub struct List<'a, T: ListItem<'a>> {
    tree: &'a SyntaxTree,
    items: &'a [u32],
    context: T::Context,
    item_type: PhantomData<T>,
}

impl<'a, T: ListItem<'a>> Clone for List<'a, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<'a, T: ListItem<'a>> Copy for List<'a, T> {}

impl<'a, T: ListItem<'a>> List<'a, T> {
    fn new(tree: &'a SyntaxTree, start: u32, context: T::Context) -> Self {
        Self::of_items(tree, tree.list(start), context)
    }

    fn of_items(tree: &'a SyntaxTree, items: &'a [u32], context: T::Context) -> Self {
        Self {
            tree,
            items,
            context,
            item_type: PhantomData,
        }
    }
}

impl<'a, T: ListItem<'a>> Sequence for List<'a, T> {
    type Item = T;

    fn len(&self) -> usize {
        self.items.len()
    }

    fn at(&self, index: usize) -> T {
        T::read(self.tree, self.items[index], self.context)
    }
}

impl<'a, T: ListItem<'a>> IntoIterator for List<'a, T> {
    type Item = T;
    type IntoIter = SequenceIterator<Self>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a, T: ListItem<'a>> IntoIterator for &List<'a, T> {
    type Item = T;
    type IntoIter = SequenceIterator<List<'a, T>>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> ListItem<'a> for Expression<'a> {
    type Context = ();
    fn read(tree: &'a SyntaxTree, node: u32, _: ()) -> Self {
        Expression::new(tree, node)
    }
}

impl<'a> ListItem<'a> for Option<Expression<'a>> {
    type Context = ();
    fn read(tree: &'a SyntaxTree, node: u32, _: ()) -> Self {
        (tree.tag(node) != tag::HOLE).then(|| Expression::new(tree, node))
    }
}

impl<'a> ListItem<'a> for Statement<'a> {
    type Context = ();
    fn read(tree: &'a SyntaxTree, node: u32, _: ()) -> Self {
        Statement::new(tree, node)
    }
}

// =============================================================================
// Identifiers
// =============================================================================

/// An identifier reference or binding name, with what scope analysis found out about it.
#[derive(Clone, Copy)]
pub struct Identifier<'a> {
    tree: &'a SyntaxTree,
    pub name: Name,
    offset: u32,
    pub local_type: Option<LocalType>,
    pub local_index: u32,
    pub is_global: bool,
    pub is_inside_scope_with_eval: bool,
    pub declaration_kind: Option<DeclarationKind>,
}

impl<'a> Identifier<'a> {
    fn new(tree: &'a SyntaxTree, name: Name, offset: u32, resolution: Resolution) -> Self {
        Self {
            tree,
            name,
            offset,
            local_type: resolution.local_type(),
            local_index: resolution.local_index(),
            is_global: resolution.is_global(),
            is_inside_scope_with_eval: resolution.is_inside_scope_with_eval(),
            declaration_kind: resolution.declaration_kind(),
        }
    }

    pub fn is_local(&self) -> bool {
        self.local_type.is_some()
    }

    pub fn text(&self) -> Utf16View<'a> {
        self.tree.name_text(self.name)
    }

    pub fn range(&self) -> SourceRange {
        self.tree.range(self.offset)
    }
}

#[derive(Clone, Copy)]
pub struct PrivateIdentifier<'a> {
    tree: &'a SyntaxTree,
    name: Name,
    offset: u32,
}

impl PrivateIdentifier<'_> {
    /// The name with its `#`.
    pub fn name(&self) -> Utf16String {
        let mut text = Utf16StringBuilder::new();
        text.append_ascii("#");
        text.append(self.tree.name_text(self.name));
        text.to_utf16_string()
    }

    pub fn range(&self) -> SourceRange {
        self.tree.range(self.offset)
    }
}

// =============================================================================
// Functions
// =============================================================================

/// Parsing insights collected during function body parsing.
///
/// All of these come out of scope analysis while parsing.
#[derive(Clone, Copy, Debug, Default)]
pub struct FunctionParsingInsights {
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub contains_direct_call_to_eval: bool,
    pub might_need_arguments_object: bool,
}

#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq)]
enum FunctionSource {
    /// A function that the code spells out, at a function node.
    Function,
    /// The constructor of the class node, which does not spell one out.
    DefaultConstructor,
    /// The initializer of the field node.
    ClassFieldInitializer,
    /// The static block node.
    StaticInitializer { class_start: u32 },
}

/// A function of a tree, which stays valid for as long as the tree.
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq)]
pub struct FunctionId {
    node: u32,
    source: FunctionSource,
}

impl FunctionId {
    pub(crate) fn function(node: u32) -> Self {
        Self {
            node,
            source: FunctionSource::Function,
        }
    }
}

/// Everything about a function, in FunctionDeclaration and FunctionExpression alike.
#[derive(Clone, Copy)]
pub struct FunctionData<'a> {
    pub name: Option<Identifier<'a>>,
    pub source_text_start: u32,
    pub source_text_end: u32,
    pub body: Statement<'a>,
    pub parameters: FunctionParameters<'a>,
    pub parameter_count: u32,
    pub has_simple_parameter_list: bool,
    pub function_length: i32,
    pub kind: FunctionKind,
    pub is_strict_mode: bool,
    pub is_arrow_function: bool,
    pub parsing_insights: FunctionParsingInsights,
}

#[derive(Clone, Copy)]
pub struct FunctionParameters<'a> {
    tree: &'a SyntaxTree,
    parameters: &'a [u32],
    /// Where the names of parameters count as being.
    first_position: u32,
    /// The class's start, if these are the parameters of the default constructor of a derived class.
    rest_of_default_constructor: Option<u32>,
}

impl<'a> FunctionParameters<'a> {
    fn empty(tree: &'a SyntaxTree) -> Self {
        Self {
            tree,
            parameters: &[],
            first_position: 0,
            rest_of_default_constructor: None,
        }
    }
}

impl<'a> Sequence for FunctionParameters<'a> {
    type Item = FunctionParameter<'a>;

    fn len(&self) -> usize {
        self.parameters.len() + usize::from(self.rest_of_default_constructor.is_some())
    }

    fn at(&self, index: usize) -> FunctionParameter<'a> {
        let tree = self.tree;
        if let Some(class_start) = self.rest_of_default_constructor {
            return FunctionParameter {
                binding: FunctionParameterBinding::Identifier(
                    tree.plain_identifier(tree.script.default_constructor_parameter_name, class_start),
                ),
                default_value: None,
                is_rest: true,
            };
        }
        let parameter = self.parameters[index];
        let parameter_tag = tree.tag(parameter);
        let (target, default) = match parameter_tag {
            tag::REST => (tree.a(parameter), 0),
            tag::ASSIGN_PATTERN | tag::ASSIGN_BASE => (tree.a(parameter), tree.b(parameter)),
            _ => (parameter, 0),
        };
        let binding = if tree.tag(target) == tag::IDENT {
            FunctionParameterBinding::Identifier(tree.identifier(Name(tree.a(target)), self.first_position, target))
        } else {
            FunctionParameterBinding::BindingPattern(BindingPattern::new(tree, target, PatternMode::Binding))
        };
        FunctionParameter {
            binding,
            default_value: (default != 0).then(|| Expression::new(tree, default)),
            is_rest: parameter_tag == tag::REST,
        }
    }
}

#[derive(Clone, Copy)]
pub struct FunctionParameter<'a> {
    pub binding: FunctionParameterBinding<'a>,
    pub default_value: Option<Expression<'a>>,
    pub is_rest: bool,
}

#[derive(Clone, Copy)]
pub enum FunctionParameterBinding<'a> {
    Identifier(Identifier<'a>),
    BindingPattern(BindingPattern<'a>),
}

// =============================================================================
// Classes
// =============================================================================

/// Shared data for ClassDeclaration and ClassExpression.
#[derive(Clone, Copy)]
pub struct ClassData<'a> {
    pub name: Option<Identifier<'a>>,
    pub source_text_start: u32,
    pub source_text_end: u32,
    pub constructor: Expression<'a>,
    pub super_class: Option<Expression<'a>>,
    pub elements: ClassElements<'a>,
}

/// The elements of a class, other than its constructor.
#[derive(Clone, Copy)]
pub struct ClassElements<'a> {
    tree: &'a SyntaxTree,
    elements: &'a [u32],
    constructor_index: Option<usize>,
    class_start: u32,
}

impl<'a> Sequence for ClassElements<'a> {
    type Item = ClassElementNode<'a>;

    fn len(&self) -> usize {
        self.elements.len() - usize::from(self.constructor_index.is_some())
    }

    fn at(&self, index: usize) -> ClassElementNode<'a> {
        let index = match self.constructor_index {
            Some(constructor_index) if index >= constructor_index => index + 1,
            _ => index,
        };
        ClassElementNode {
            tree: self.tree,
            node: self.elements[index],
            class_start: self.class_start,
        }
    }
}

#[derive(Clone, Copy)]
pub struct ClassElementNode<'a> {
    tree: &'a SyntaxTree,
    node: u32,
    class_start: u32,
}

impl<'a> ClassElementNode<'a> {
    pub fn range(&self) -> SourceRange {
        let tree = self.tree;
        let inner = self.inner_node().0;
        if tree.tag(inner) == tag::CLASS_STATIC_BLOCK {
            tree.range(tree.pos(inner))
        } else {
            tree.range(self.class_start)
        }
    }

    /// The element without its `static`, and whether it had one.
    fn inner_node(&self) -> (u32, bool) {
        let tree = self.tree;
        if tree.tag(self.node) == tag::STATIC_ELEMENT {
            (tree.a(self.node), true)
        } else {
            (self.node, false)
        }
    }

    fn property_key(&self, key: u32, computed: bool) -> Expression<'a> {
        if !computed && self.tree.tag(key) == tag::IDENT {
            Expression::with_form(self.tree, key, ExpressionForm::PropertyKeyName)
        } else {
            Expression::new(self.tree, key)
        }
    }

    pub fn kind(&self) -> ClassElement<'a> {
        let tree = self.tree;
        let (inner, is_static) = self.inner_node();
        let (a, b) = (tree.a(inner), tree.b(inner));
        let inner_tag = tree.tag(inner);
        match inner_tag {
            tag::CLASS_FIELD | tag::COMPUTED_CLASS_FIELD => ClassElement::Field {
                key: self.property_key(a, inner_tag == tag::COMPUTED_CLASS_FIELD),
                initializer: (b != 0).then(|| Expression::new(tree, tree.a(b))),
                initializer_function: FunctionId {
                    node: inner,
                    source: FunctionSource::ClassFieldInitializer,
                },
                is_static,
            },
            tag::CLASS_STATIC_BLOCK => {
                let class_start = self.class_start;
                ClassElement::StaticInitializer {
                    body: Statement::with_form(tree, inner, StatementForm::StaticBlockBody { class_start }),
                    function: FunctionId {
                        node: inner,
                        source: FunctionSource::StaticInitializer { class_start },
                    },
                }
            }
            other => {
                let kind = match other {
                    tag::GETTER | tag::COMPUTED_GETTER => ClassMethodKind::Getter,
                    tag::SETTER | tag::COMPUTED_SETTER => ClassMethodKind::Setter,
                    _ => ClassMethodKind::Method,
                };
                let computed = matches!(
                    other,
                    tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER
                );
                ClassElement::Method {
                    key: self.property_key(a, computed),
                    function: Expression::new(tree, b),
                    kind,
                    is_static,
                }
            }
        }
    }
}

#[derive(Clone, Copy)]
pub enum ClassElement<'a> {
    Method {
        key: Expression<'a>,
        function: Expression<'a>,
        kind: ClassMethodKind,
        is_static: bool,
    },
    Field {
        key: Expression<'a>,
        initializer: Option<Expression<'a>>,
        /// The function that evaluates `initializer`, if there is one.
        initializer_function: FunctionId,
        is_static: bool,
    },
    StaticInitializer {
        body: Statement<'a>,
        function: FunctionId,
    },
}

// =============================================================================
// Binding patterns
// =============================================================================

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PatternMode {
    /// The pattern of a declaration, a parameter or a catch clause.
    Binding,
    /// The target of a destructuring assignment.
    Assignment,
}

#[derive(Clone, Copy)]
pub struct BindingPattern<'a> {
    pub kind: BindingPatternKind,
    pub entries: List<'a, BindingEntry<'a>>,
}

impl<'a> BindingPattern<'a> {
    pub(crate) fn new(tree: &'a SyntaxTree, node: u32, mode: PatternMode) -> Self {
        let is_array = tree.tag(node) == tag::ARRAY_PATTERN;
        Self {
            kind: if is_array {
                BindingPatternKind::Array
            } else {
                BindingPatternKind::Object
            },
            entries: List::new(tree, tree.a(node), (is_array, mode)),
        }
    }
}

#[derive(Clone, Copy)]
pub struct BindingEntry<'a> {
    pub name: Option<BindingEntryName<'a>>,
    pub alias: Option<BindingEntryAlias<'a>>,
    pub initializer: Option<Expression<'a>>,
    pub is_rest: bool,
}

/// The "name" part of a binding entry.
/// - `None`: elision in array patterns (`[, , x]`)
/// - `Identifier`: object property shorthand (`{ x }`)
/// - `Key`: property key (`{ x: y }`, `{ "x": y }`)
/// - `Expression`: computed property key (`{ [expression]: x }`)
#[derive(Clone, Copy)]
pub enum BindingEntryName<'a> {
    Identifier(Identifier<'a>),
    Key(PropertyKey<'a>),
    Expression(Expression<'a>),
}

/// The key of a property in a pattern, which refers to no binding.
#[derive(Clone, Copy)]
pub struct PropertyKey<'a> {
    tree: &'a SyntaxTree,
    node: u32,
}

impl PropertyKey<'_> {
    pub fn text(&self) -> Utf16String {
        let tree = self.tree;
        let key = self.node;
        match tree.tag(key) {
            tag::IDENT => tree.name_text(Name(tree.a(key))).into(),
            tag::STR => tree.string_value(key),
            tag::NUM => crate::host::js_number_to_utf16(literals::parse_numeric_value(
                tree.source_units(tree.pos(key), tree.a(key)),
            )),
            _ => {
                let literal = tree.source_units(tree.pos(key), tree.a(key)).to_utf8();
                let value = crate::bytecode::constant_fold::parse_bigint(literal.trim_end_matches('n'))
                    .expect("the lexer only accepts valid BigInts");
                Utf16String::from_utf8(&value.to_string())
            }
        }
    }

    pub fn range(&self) -> SourceRange {
        self.tree.range(self.tree.pos(self.node))
    }
}

/// The "alias" (target) of a binding entry.
/// - `None`: name is the binding target (`{ x }` — x is both name and alias)
/// - `Identifier`: simple binding (`{ x: y }`)
/// - `BindingPattern`: nested destructuring (`{ x: { a, b } }`)
/// - `MemberExpression`: assignment target (`{ x: obj.property }`)
#[derive(Clone, Copy)]
pub enum BindingEntryAlias<'a> {
    Identifier(Identifier<'a>),
    BindingPattern(BindingPattern<'a>),
    MemberExpression(Expression<'a>),
}

fn pattern_alias(tree: &SyntaxTree, value: u32, mode: PatternMode) -> BindingEntryAlias<'_> {
    match tree.tag(value) {
        tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => {
            BindingEntryAlias::BindingPattern(BindingPattern::new(tree, value, mode))
        }
        tag::IDENT if mode == PatternMode::Binding => {
            BindingEntryAlias::Identifier(tree.identifier(Name(tree.a(value)), tree.pos(value), value))
        }
        _ => {
            let expression = Expression::new(tree, value);
            match expression.kind() {
                ExpressionKind::Identifier(identifier) => BindingEntryAlias::Identifier(identifier),
                _ => BindingEntryAlias::MemberExpression(expression),
            }
        }
    }
}

impl<'a> ListItem<'a> for BindingEntry<'a> {
    type Context = (bool, PatternMode);

    fn read(tree: &'a SyntaxTree, item: u32, (is_array, mode): Self::Context) -> Self {
        let item_tag = tree.tag(item);
        if item_tag == tag::HOLE {
            return BindingEntry {
                name: None,
                alias: None,
                initializer: None,
                is_rest: false,
            };
        }
        let is_rest = item_tag == tag::REST;
        let mut value = match item_tag {
            tag::REST => tree.a(item),
            tag::PROPERTY | tag::COMPUTED_PROPERTY | tag::SHORTHAND => tree.b(item),
            _ => item,
        };
        let mut initializer = 0;
        if matches!(tree.tag(value), tag::ASSIGN_PATTERN | tag::ASSIGN_BASE) {
            initializer = tree.b(value);
            value = tree.a(value);
        }

        let mut name = None;
        let mut alias = None;
        if is_array {
            alias = Some(pattern_alias(tree, value, mode));
        } else if item_tag == tag::SHORTHAND {
            name = Some(BindingEntryName::Identifier(tree.identifier(
                Name(tree.a(item)),
                tree.pos(value),
                item,
            )));
        } else if is_rest {
            if mode == PatternMode::Binding {
                name = Some(BindingEntryName::Identifier(tree.identifier(
                    Name(tree.a(value)),
                    tree.pos(value),
                    value,
                )));
            } else {
                let expression = Expression::new(tree, value);
                match expression.kind() {
                    ExpressionKind::Identifier(identifier) => name = Some(BindingEntryName::Identifier(identifier)),
                    _ => alias = Some(BindingEntryAlias::MemberExpression(expression)),
                }
            }
        } else {
            let key = tree.a(item);
            name = Some(if item_tag == tag::COMPUTED_PROPERTY {
                BindingEntryName::Expression(Expression::new(tree, key))
            } else {
                BindingEntryName::Key(PropertyKey { tree, node: key })
            });
            alias = Some(pattern_alias(tree, value, mode));
        }
        BindingEntry {
            name,
            alias,
            initializer: (initializer != 0).then(|| Expression::new(tree, initializer)),
            is_rest,
        }
    }
}

// =============================================================================
// Object literals
// =============================================================================

#[derive(Clone, Copy)]
pub struct ObjectProperty<'a> {
    tree: &'a SyntaxTree,
    offset: u32,
    pub property_type: ObjectPropertyType,
    pub key: Expression<'a>,
    pub value: Option<Expression<'a>>,
    pub is_method: bool,
    pub is_computed: bool,
}

impl ObjectProperty<'_> {
    pub fn range(&self) -> SourceRange {
        self.tree.range(self.offset)
    }
}

impl<'a> ListItem<'a> for ObjectProperty<'a> {
    /// Where the object starts.
    type Context = u32;

    fn read(tree: &'a SyntaxTree, property: u32, object_start: u32) -> Self {
        let property_tag = tree.tag(property);
        let (a, b) = (tree.a(property), tree.b(property));
        match property_tag {
            tag::SPREAD => ObjectProperty {
                tree,
                offset: tree.pos(property),
                property_type: ObjectPropertyType::Spread,
                key: Expression::new(tree, a),
                value: None,
                is_method: false,
                is_computed: false,
            },
            tag::SHORTHAND => ObjectProperty {
                tree,
                offset: object_start,
                property_type: ObjectPropertyType::KeyValue,
                key: Expression::with_form(tree, property, ExpressionForm::ShorthandKey),
                value: Some(Expression::with_form(
                    tree,
                    property,
                    ExpressionForm::ShorthandValue { object_start },
                )),
                is_method: false,
                is_computed: false,
            },
            _ => {
                let is_computed = matches!(
                    property_tag,
                    tag::COMPUTED_PROPERTY | tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER
                );
                let is_method = !matches!(property_tag, tag::PROPERTY | tag::COMPUTED_PROPERTY);
                let property_type = match property_tag {
                    tag::GETTER | tag::COMPUTED_GETTER => ObjectPropertyType::Getter,
                    tag::SETTER | tag::COMPUTED_SETTER => ObjectPropertyType::Setter,
                    tag::PROPERTY if tree.is_proto_key(a) => ObjectPropertyType::ProtoSetter,
                    _ => ObjectPropertyType::KeyValue,
                };
                let key = if !is_computed && tree.tag(a) == tag::IDENT {
                    Expression::with_form(tree, a, ExpressionForm::PropertyKeyName)
                } else {
                    Expression::new(tree, a)
                };
                ObjectProperty {
                    tree,
                    offset: object_start,
                    property_type,
                    key,
                    value: Some(Expression::new(tree, b)),
                    is_method,
                    is_computed,
                }
            }
        }
    }
}

// =============================================================================
// Calls
// =============================================================================

#[derive(Clone, Copy)]
pub struct CallArgument<'a> {
    pub value: Expression<'a>,
    pub is_spread: bool,
}

impl<'a> ListItem<'a> for CallArgument<'a> {
    type Context = ();

    fn read(tree: &'a SyntaxTree, argument: u32, _: ()) -> Self {
        let is_spread = tree.tag(argument) == tag::SPREAD;
        CallArgument {
            value: Expression::new(tree, if is_spread { tree.a(argument) } else { argument }),
            is_spread,
        }
    }
}

/// The arguments of a call, which are a list of the tree, or a spread of the parameter of a default constructor.
#[derive(Clone, Copy)]
pub enum CallArguments<'a> {
    List(List<'a, CallArgument<'a>>),
    DefaultConstructorSpread(CallArgument<'a>),
}

impl<'a> Sequence for CallArguments<'a> {
    type Item = CallArgument<'a>;

    fn len(&self) -> usize {
        match self {
            CallArguments::List(list) => list.len(),
            CallArguments::DefaultConstructorSpread(_) => 1,
        }
    }

    fn at(&self, index: usize) -> CallArgument<'a> {
        match self {
            CallArguments::List(list) => list.at(index),
            CallArguments::DefaultConstructorSpread(argument) => *argument,
        }
    }
}

#[derive(Clone, Copy)]
pub struct CallExpressionData<'a> {
    pub callee: Expression<'a>,
    pub arguments: CallArguments<'a>,
}

#[derive(Clone, Copy)]
pub struct SuperCallData<'a> {
    pub arguments: CallArguments<'a>,
    pub is_synthetic: bool,
}

// =============================================================================
// Optional chains
// =============================================================================

#[derive(Clone, Copy)]
pub enum OptionalChainReference<'a> {
    Call {
        arguments: CallArguments<'a>,
        mode: OptionalChainMode,
    },
    ComputedReference {
        expression: Expression<'a>,
        mode: OptionalChainMode,
    },
    MemberReference {
        identifier: Identifier<'a>,
        mode: OptionalChainMode,
    },
    PrivateMemberReference {
        private_identifier: PrivateIdentifier<'a>,
        mode: OptionalChainMode,
    },
}

#[derive(Clone, Copy)]
pub struct OptionalChainData<'a> {
    tree: &'a SyntaxTree,
    node: u32,
}

impl<'a> OptionalChainData<'a> {
    fn is_link(tag: u8) -> bool {
        matches!(
            tag,
            tag::MEMBER
                | tag::INDEX
                | tag::PRIVATE_MEMBER
                | tag::CALL
                | tag::OPT_MEMBER
                | tag::OPT_INDEX
                | tag::OPT_PRIVATE_MEMBER
                | tag::OPT_CALL
        )
    }

    fn is_optional(tag: u8) -> bool {
        matches!(
            tag,
            tag::OPT_MEMBER | tag::OPT_INDEX | tag::OPT_PRIVATE_MEMBER | tag::OPT_CALL
        )
    }

    /// The chain reaches down to its innermost optional link, and what is below that is its base. Returns the base and
    /// the links, outermost first.
    pub(crate) fn links(tree: &SyntaxTree, node: u32) -> (u32, Vec<u32>) {
        let mut links = Vec::new();
        let mut base = node;
        let mut remaining_optional_links = 0;
        let mut probe = node;
        while Self::is_link(tree.tag(probe)) {
            remaining_optional_links += u32::from(Self::is_optional(tree.tag(probe)));
            probe = tree.a(probe);
        }
        while remaining_optional_links > 0 {
            remaining_optional_links -= u32::from(Self::is_optional(tree.tag(base)));
            links.push(base);
            base = tree.a(base);
        }
        (base, links)
    }

    pub fn base(&self) -> Expression<'a> {
        Expression::new(self.tree, Self::links(self.tree, self.node).0)
    }

    pub fn references(&self) -> Vec<OptionalChainReference<'a>> {
        let tree = self.tree;
        let (_, mut links) = Self::links(tree, self.node);
        links.reverse();
        links
            .into_iter()
            .map(|link| {
                let link_tag = tree.tag(link);
                let mode = if Self::is_optional(link_tag) {
                    OptionalChainMode::Optional
                } else {
                    OptionalChainMode::NotOptional
                };
                let b = tree.b(link);
                match link_tag {
                    tag::CALL | tag::OPT_CALL => OptionalChainReference::Call {
                        arguments: CallArguments::List(List::new(tree, b, ())),
                        mode,
                    },
                    tag::INDEX | tag::OPT_INDEX => OptionalChainReference::ComputedReference {
                        expression: Expression::new(tree, b),
                        mode,
                    },
                    tag::MEMBER | tag::OPT_MEMBER => OptionalChainReference::MemberReference {
                        identifier: tree.plain_identifier(Name(b), tree.name_after(tree.pos(link))),
                        mode,
                    },
                    _ => OptionalChainReference::PrivateMemberReference {
                        private_identifier: tree.private_identifier(b, tree.name_after(tree.pos(link))),
                        mode,
                    },
                }
            })
            .collect()
    }
}

// =============================================================================
// Literals
// =============================================================================

/// A numeric literal, whose value comes from its source text.
#[derive(Clone, Copy)]
pub struct NumericLiteral<'a> {
    text: Utf16View<'a>,
}

impl NumericLiteral<'_> {
    pub fn value(&self) -> f64 {
        literals::parse_numeric_value(self.text)
    }
}

/// A string, whose value comes from the source or a name.
#[derive(Clone, Copy)]
pub enum StringLiteral<'a> {
    /// The value as it is.
    Name(Utf16View<'a>),
    /// Source text that may contain escape sequences.
    Source(Utf16View<'a>, EscapeMode),
}

impl StringLiteral<'_> {
    pub fn value(&self) -> Utf16String {
        match *self {
            StringLiteral::Name(text) => text.into(),
            StringLiteral::Source(text, mode) => literals::process_escape_sequences_impl(text, mode).value,
        }
    }
}

#[derive(Clone, Copy)]
pub struct RegExpLiteralData<'a> {
    pub pattern: Utf16View<'a>,
    pub flags: Utf16View<'a>,
}

/// An element of a template: one of its expressions, or one of the strings between them.
#[derive(Clone, Copy)]
pub struct TemplateElementContext {
    is_tagged: bool,
    template_start: u32,
}

impl<'a> ListItem<'a> for TemplateElement<'a> {
    type Context = TemplateElementContext;

    fn read(tree: &'a SyntaxTree, node: u32, context: TemplateElementContext) -> Self {
        TemplateElement(if tree.tag(node) == tag::TEMPLATE_CHUNK {
            Expression::with_form(
                tree,
                node,
                ExpressionForm::TemplateChunk {
                    is_tagged: context.is_tagged,
                    template_start: context.template_start,
                },
            )
        } else {
            Expression::new(tree, node)
        })
    }
}

#[derive(Clone, Copy)]
pub struct TemplateElement<'a>(pub Expression<'a>);

#[derive(Clone, Copy)]
pub struct TemplateLiteralData<'a> {
    tree: &'a SyntaxTree,
    items: &'a [u32],
    pub expressions: List<'a, TemplateElement<'a>>,
}

impl TemplateLiteralData<'_> {
    /// The raw strings of a tagged template.
    pub fn raw_strings(&self) -> Vec<Utf16String> {
        let tree = self.tree;
        self.items
            .iter()
            .filter(|&&item| tree.tag(item) == tag::TEMPLATE_CHUNK)
            .map(|&item| literals::raw_template_value(template_chunk_units(tree, item)))
            .collect()
    }
}

fn template_chunk_units(tree: &SyntaxTree, chunk: u32) -> Utf16View<'_> {
    let start = tree.a(chunk);
    tree.source_units(start, start + (tree.b(chunk) >> 1))
}

// =============================================================================
// Statement data
// =============================================================================

#[derive(Clone, Copy)]
pub struct VariableDeclarator<'a> {
    tree: &'a SyntaxTree,
    offset: u32,
    pub target: VariableDeclaratorTarget<'a>,
    pub init: Option<Expression<'a>>,
}

#[derive(Clone, Copy)]
pub enum VariableDeclaratorTarget<'a> {
    Identifier(Identifier<'a>),
    BindingPattern(BindingPattern<'a>),
}

impl VariableDeclarator<'_> {
    pub fn range(&self) -> SourceRange {
        self.tree.range(self.offset)
    }
}

impl<'a> ListItem<'a> for VariableDeclarator<'a> {
    /// Where the declaration starts.
    type Context = u32;

    fn read(tree: &'a SyntaxTree, declarator: u32, offset: u32) -> Self {
        let (target, init) = (tree.a(declarator), tree.b(declarator));
        VariableDeclarator {
            tree,
            offset,
            target: if tree.tag(target) == tag::IDENT {
                VariableDeclaratorTarget::Identifier(tree.identifier(Name(tree.a(target)), tree.pos(target), target))
            } else {
                VariableDeclaratorTarget::BindingPattern(BindingPattern::new(tree, target, PatternMode::Binding))
            },
            init: (init != 0).then(|| Expression::new(tree, init)),
        }
    }
}

#[derive(Clone, Copy)]
pub struct VariableDeclarationData<'a> {
    pub kind: DeclarationKind,
    pub declarations: List<'a, VariableDeclarator<'a>>,
}

/// A `using` or an `await using` declaration.
#[derive(Clone, Copy)]
pub struct UsingDeclarationData<'a> {
    pub is_await: bool,
    pub declarations: List<'a, VariableDeclarator<'a>>,
}

#[derive(Clone, Copy)]
pub struct FunctionDeclarationData<'a> {
    pub function_id: FunctionId,
    pub name: Option<Identifier<'a>>,
    pub kind: FunctionKind,
    pub is_hoisted: bool,
}

/// The body of the function that evaluates the initializer of a class field.
#[derive(Clone, Copy)]
pub struct ClassFieldInitializerData<'a> {
    pub expression: Expression<'a>,
    /// The key of the field.
    pub key: Expression<'a>,
}

impl ClassFieldInitializerData<'_> {
    /// The name that an anonymous function in the field gets.
    pub fn field_name(&self) -> Utf16String {
        match self.key.kind() {
            ExpressionKind::Identifier(identifier) => identifier.text().into(),
            ExpressionKind::StringLiteral(literal) => literal.value(),
            ExpressionKind::PrivateIdentifier(identifier) => identifier.name(),
            ExpressionKind::NumericLiteral(literal) => crate::host::js_number_to_utf16(literal.value()),
            ExpressionKind::BigIntLiteral(literal) => bigint_literal_digits(literal).into(),
            _ => Utf16String::default(),
        }
    }
}

/// The digits of the source text of a BigInt literal, which ends in `n`.
pub fn bigint_literal_digits(literal: Utf16View<'_>) -> Utf16View<'_> {
    literal.substring_view(0, literal.length_in_code_units() - 1)
}

#[derive(Clone, Copy)]
pub struct IfStatementData<'a> {
    pub test: Expression<'a>,
    pub consequent: Statement<'a>,
    pub alternate: Option<Statement<'a>>,
}

#[derive(Clone, Copy)]
pub struct WhileStatementData<'a> {
    pub test: Expression<'a>,
    pub body: Statement<'a>,
}

/// Init clause of a for loop: either a declaration or an expression.
#[derive(Clone, Copy)]
pub enum ForInit<'a> {
    Declaration(Statement<'a>),
    Expression(Expression<'a>),
}

#[derive(Clone, Copy)]
pub struct ForStatementData<'a> {
    pub init: Option<ForInit<'a>>,
    pub test: Option<Expression<'a>>,
    pub update: Option<Expression<'a>>,
    pub body: Statement<'a>,
}

/// Left-hand side of for-in, for-of, for-await-of.
#[derive(Clone, Copy)]
pub enum ForInOfLhs<'a> {
    /// A variable declaration (`for (let x of ...)`)
    Declaration(Statement<'a>),
    /// An expression (`for (x in obj)`)
    Expression(Expression<'a>),
    /// A binding pattern (`for ({a, b} of ...)`)
    Pattern(BindingPattern<'a>),
}

#[derive(Clone, Copy)]
pub struct ForInOfStatementData<'a> {
    pub kind: ForInOfKind,
    pub lhs: ForInOfLhs<'a>,
    pub rhs: Expression<'a>,
    pub body: Statement<'a>,
}

#[derive(Clone, Copy)]
pub struct WithStatementData<'a> {
    pub object: Expression<'a>,
    pub body: Statement<'a>,
}

#[derive(Clone, Copy)]
pub struct LabelledStatementData<'a> {
    pub label: Utf16View<'a>,
    pub item: Statement<'a>,
}

#[derive(Clone, Copy)]
pub struct TryStatementData<'a> {
    pub block: Statement<'a>,
    pub handler: Option<CatchClause<'a>>,
    pub finalizer: Option<Statement<'a>>,
}

#[derive(Clone, Copy)]
pub struct CatchClause<'a> {
    tree: &'a SyntaxTree,
    offset: u32,
    pub parameter: Option<CatchBinding<'a>>,
    pub body: Statement<'a>,
}

impl CatchClause<'_> {
    pub fn range(&self) -> SourceRange {
        self.tree.range(self.offset)
    }
}

#[derive(Clone, Copy)]
pub enum CatchBinding<'a> {
    Identifier(Identifier<'a>),
    BindingPattern(BindingPattern<'a>),
}

#[derive(Clone, Copy)]
pub struct SwitchStatementData<'a> {
    pub discriminant: Expression<'a>,
    pub cases: List<'a, SwitchCase<'a>>,
}

#[derive(Clone, Copy)]
pub struct SwitchCase<'a> {
    tree: &'a SyntaxTree,
    offset: u32,
    pub scope: Scope<'a>,
    pub test: Option<Expression<'a>>,
}

impl SwitchCase<'_> {
    pub fn range(&self) -> SourceRange {
        self.tree.range(self.offset)
    }
}

impl<'a> ListItem<'a> for SwitchCase<'a> {
    type Context = ();

    fn read(tree: &'a SyntaxTree, case: u32, _: ()) -> Self {
        let test = tree.a(case);
        SwitchCase {
            tree,
            offset: tree.pos(case),
            scope: Scope::of_list(tree, tree.b(case)),
            test: (test != 0).then(|| Expression::new(tree, test)),
        }
    }
}

#[derive(Clone, Copy)]
pub struct ImportStatementData<'a> {
    tree: &'a SyntaxTree,
    node: u32,
}

impl ImportStatementData<'_> {
    pub fn module_request(&self) -> ModuleRequest {
        self.tree.module_request(self.tree.b(self.node))
    }

    pub fn entries(&self) -> Vec<ImportEntry> {
        let tree = self.tree;
        tree.list(tree.a(self.node))
            .iter()
            .map(|&specifier| {
                let imported = tree.a(specifier);
                ImportEntry {
                    import_name: match imported {
                        0 => Some(Utf16String::from_utf16(utf16!("default"))),
                        NONE => None,
                        _ => Some(tree.specifier_name(imported)),
                    },
                    local_name: tree.name_text(Name(tree.b(specifier))).into(),
                }
            })
            .collect()
    }
}

#[derive(Clone, Copy)]
pub struct ExportStatementData<'a> {
    tree: &'a SyntaxTree,
    node: u32,
    pub statement: Option<Statement<'a>>,
    pub is_default_export: bool,
}

impl ExportStatementData<'_> {
    pub fn module_request(&self) -> Option<ModuleRequest> {
        let tree = self.tree;
        let (a, b) = (tree.a(self.node), tree.b(self.node));
        match tree.tag(self.node) {
            tag::EXPORT_ALL => Some(tree.module_request(b)),
            tag::EXPORT_NAMED if a != 0 && b != 0 => Some(tree.module_request(b)),
            _ => None,
        }
    }

    pub fn entries(&self) -> Vec<ExportEntry> {
        let tree = self.tree;
        let (a, b) = (tree.a(self.node), tree.b(self.node));
        let mut entries = Vec::new();
        match tree.tag(self.node) {
            tag::EXPORT_ALL => {
                if a == 0 {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::ModuleRequestAllButDefault,
                        export_name: None,
                        local_or_import_name: None,
                    });
                } else {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::ModuleRequestAll,
                        export_name: Some(tree.specifier_name(a)),
                        local_or_import_name: None,
                    });
                }
            }
            tag::EXPORT_DEFAULT => {
                let inner_tag = tree.tag(a);
                let mut local_name = Utf16String::from_utf16(utf16!("*default*"));
                if inner_tag == tag::FUNCTION && b != 0 {
                    let name = tree.script.index.name[tree.b(a) as usize];
                    if name != NONE {
                        local_name = tree.name_text(Name(name)).into();
                    }
                } else if inner_tag == tag::CLASS_DECL {
                    local_name = tree.name_text(Name(tree.a(tree.a(a)))).into();
                }
                entries.push(ExportEntry {
                    kind: ExportEntryKind::NamedExport,
                    export_name: Some(Utf16String::from_utf16(utf16!("default"))),
                    local_or_import_name: Some(local_name),
                });
            }
            _ if a != 0 => {
                for &specifier in tree.list(a) {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::NamedExport,
                        export_name: Some(tree.specifier_name(tree.b(specifier))),
                        local_or_import_name: Some(tree.specifier_name(tree.a(specifier))),
                    });
                }
                if entries.is_empty() {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::EmptyNamedExport,
                        export_name: None,
                        local_or_import_name: None,
                    });
                }
            }
            _ => {
                let mut names = Vec::new();
                match tree.tag(b) {
                    tag::FUNCTION_DECL => names.push(tree.name_text(Name(tree.b(b))).into()),
                    tag::CLASS_DECL => names.push(tree.name_text(Name(tree.a(tree.a(b)))).into()),
                    _ => {
                        for &declarator in tree.list(tree.a(b)) {
                            tree.pattern_names(tree.a(declarator), &mut names);
                        }
                    }
                }
                for name in names {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::NamedExport,
                        export_name: Some(name.clone()),
                        local_or_import_name: Some(name),
                    });
                }
            }
        }
        entries
    }
}

#[derive(Clone, Copy)]
pub struct ProgramData<'a> {
    pub scope: Scope<'a>,
    pub program_type: ProgramType,
    pub is_strict_mode: bool,
    pub has_top_level_await: bool,
}

// =============================================================================
// Scopes
// =============================================================================

/// What statements a scope has, which the tree does not always hold as a list.
#[derive(Clone, Copy)]
pub enum ScopeChildren<'a> {
    List(List<'a, Statement<'a>>),
    One(Statement<'a>),
    None,
}

impl<'a> Sequence for ScopeChildren<'a> {
    type Item = Statement<'a>;

    fn len(&self) -> usize {
        match self {
            ScopeChildren::List(list) => list.len(),
            ScopeChildren::One(_) => 1,
            ScopeChildren::None => 0,
        }
    }

    fn at(&self, index: usize) -> Statement<'a> {
        match self {
            ScopeChildren::List(list) => list.at(index),
            ScopeChildren::One(statement) => *statement,
            ScopeChildren::None => unreachable!("a scope without children has no child {index}"),
        }
    }
}

/// A scope with statements of its own: a program, a function body, a block or a case of a switch.
#[derive(Clone, Copy)]
pub struct Scope<'a> {
    tree: &'a SyntaxTree,
    pub children: ScopeChildren<'a>,
    /// What scope analysis found out if this is the scope of a function, a static block or a program.
    row: Option<&'a ScopeRow>,
}

impl<'a> Scope<'a> {
    fn new(tree: &'a SyntaxTree, children: ScopeChildren<'a>) -> Self {
        Self {
            tree,
            children,
            row: None,
        }
    }

    fn of_list(tree: &'a SyntaxTree, list: u32) -> Self {
        Self::new(tree, ScopeChildren::List(List::new(tree, list, ())))
    }

    fn of_function(tree: &'a SyntaxTree, children: ScopeChildren<'a>, row: &'a ScopeRow) -> Self {
        Self {
            tree,
            children,
            row: Some(row),
        }
    }

    pub fn local_variables(&self) -> &'a [LocalVariable] {
        self.row
            .map_or(&[], |row| row.local_variables.of(&self.tree.scopes.local_variables))
    }

    /// Function names hoisted from inner blocks via Annex B.3.3. The FDI should create `var` bindings initialized to
    /// `undefined` for each name.
    pub fn annexb_function_names(&self) -> &'a [Name] {
        self.row.map_or(&[], |row| {
            row.annexb_function_names.of(&self.tree.scopes.annexb_function_names)
        })
    }

    pub fn uses_this(&self) -> bool {
        self.row.is_some_and(|row| row.uses_this)
    }

    pub fn uses_this_from_environment(&self) -> bool {
        self.row.is_some_and(|row| row.uses_this_from_environment)
    }

    pub fn contains_direct_call_to_eval(&self) -> bool {
        self.row.is_some_and(|row| row.contains_direct_call_to_eval)
    }

    pub fn contains_access_to_arguments_object(&self) -> bool {
        self.row.is_some_and(|row| row.contains_access_to_arguments_object)
    }
}

// =============================================================================
// Expressions
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ExpressionForm {
    /// The expression that the node's tag says.
    Node,
    /// The name of an identifier node as a string.
    PropertyKeyName,
    /// The property name after a member node's dot, which has no node of its own.
    MemberName,
    /// The key of a shorthand property node.
    ShorthandKey,
    /// The value of a shorthand property node.
    ShorthandValue { object_start: u32 },
    /// The string of a template chunk node.
    TemplateChunk { is_tagged: bool, template_start: u32 },
    /// The template node of a tagged template.
    TaggedTemplate,
    /// The default constructor of a class node.
    DefaultConstructor,
    /// The `super(...args)` of the default constructor of a class node.
    DefaultConstructorSuperCall,
    /// The `args` of the default constructor of a class node.
    DefaultConstructorArgument,
}

#[derive(Clone, Copy)]
pub struct Expression<'a> {
    tree: &'a SyntaxTree,
    node: u32,
    form: ExpressionForm,
}

#[derive(Clone, Copy)]
pub struct BinaryExpressionData<'a> {
    pub op: BinaryOp,
    pub lhs: Expression<'a>,
    pub rhs: Expression<'a>,
}

#[derive(Clone, Copy)]
pub struct LogicalExpressionData<'a> {
    pub op: LogicalOp,
    pub lhs: Expression<'a>,
    pub rhs: Expression<'a>,
}

#[derive(Clone, Copy)]
pub struct UpdateExpressionData<'a> {
    pub op: UpdateOp,
    pub argument: Expression<'a>,
    pub prefixed: bool,
}

#[derive(Clone, Copy)]
pub enum AssignmentLhs<'a> {
    Expression(Expression<'a>),
    Pattern(BindingPattern<'a>),
}

#[derive(Clone, Copy)]
pub struct AssignmentExpressionData<'a> {
    pub op: AssignmentOp,
    pub lhs: AssignmentLhs<'a>,
    pub rhs: Expression<'a>,
    pub lhs_is_parenthesized: bool,
}

#[derive(Clone, Copy)]
pub struct ConditionalExpressionData<'a> {
    pub test: Expression<'a>,
    pub consequent: Expression<'a>,
    pub alternate: Expression<'a>,
}

#[derive(Clone, Copy)]
pub struct MemberExpressionData<'a> {
    pub object: Expression<'a>,
    pub property: Expression<'a>,
    pub computed: bool,
}

#[derive(Clone, Copy)]
pub struct TaggedTemplateData<'a> {
    pub tag: Expression<'a>,
    pub template_literal: Expression<'a>,
}

#[derive(Clone, Copy)]
pub struct ImportCallData<'a> {
    pub specifier: Expression<'a>,
    pub options: Option<Expression<'a>>,
}

#[derive(Clone, Copy)]
pub struct YieldExpressionData<'a> {
    pub argument: Option<Expression<'a>>,
    pub is_yield_from: bool,
}

#[derive(Clone, Copy)]
pub enum ExpressionKind<'a> {
    // Literals
    NumericLiteral(NumericLiteral<'a>),
    StringLiteral(StringLiteral<'a>),
    BooleanLiteral(bool),
    NullLiteral,
    BigIntLiteral(Utf16View<'a>),
    RegExpLiteral(RegExpLiteralData<'a>),

    // Identifiers
    Identifier(Identifier<'a>),
    PrivateIdentifier(PrivateIdentifier<'a>),

    // Operators
    Binary(BinaryExpressionData<'a>),
    Logical(LogicalExpressionData<'a>),
    Unary { op: UnaryOp, operand: Expression<'a> },
    Update(UpdateExpressionData<'a>),
    Assignment(AssignmentExpressionData<'a>),
    Conditional(ConditionalExpressionData<'a>),
    Sequence(List<'a, Expression<'a>>),

    // Member access
    Member(MemberExpressionData<'a>),
    OptionalChain(OptionalChainData<'a>),

    // Calls
    Call(CallExpressionData<'a>),
    New(CallExpressionData<'a>),
    SuperCall(SuperCallData<'a>),

    // Spread
    Spread(Expression<'a>),

    // This / Super
    This,
    Super,

    // Functions
    Function(FunctionId),

    // Classes
    Class(ClassData<'a>),

    // Collections
    Array(List<'a, Option<Expression<'a>>>),
    Object(List<'a, ObjectProperty<'a>>),

    // Templates
    TemplateLiteral(TemplateLiteralData<'a>),
    TaggedTemplateLiteral(TaggedTemplateData<'a>),

    // Meta
    MetaProperty(MetaPropertyType),
    ImportCall(ImportCallData<'a>),

    // Async / Generator
    Yield(YieldExpressionData<'a>),
    Await(Expression<'a>),
}

pub(crate) const BINARY_OPERATORS: [Option<BinaryOp>; 25] = [
    None,
    None,
    None,
    Some(BinaryOp::BitwiseOr),
    Some(BinaryOp::BitwiseXor),
    Some(BinaryOp::BitwiseAnd),
    Some(BinaryOp::LooselyEquals),
    Some(BinaryOp::LooselyInequals),
    Some(BinaryOp::StrictlyEquals),
    Some(BinaryOp::StrictlyInequals),
    Some(BinaryOp::LessThan),
    Some(BinaryOp::GreaterThan),
    Some(BinaryOp::LessThanEquals),
    Some(BinaryOp::GreaterThanEquals),
    Some(BinaryOp::LeftShift),
    Some(BinaryOp::RightShift),
    Some(BinaryOp::UnsignedRightShift),
    Some(BinaryOp::Addition),
    Some(BinaryOp::Subtraction),
    Some(BinaryOp::Multiplication),
    Some(BinaryOp::Division),
    Some(BinaryOp::Modulo),
    Some(BinaryOp::Exponentiation),
    Some(BinaryOp::In),
    Some(BinaryOp::InstanceOf),
];

pub(crate) const ASSIGNMENT_OPERATORS: [AssignmentOp; 16] = [
    AssignmentOp::Assignment,
    AssignmentOp::AdditionAssignment,
    AssignmentOp::SubtractionAssignment,
    AssignmentOp::MultiplicationAssignment,
    AssignmentOp::DivisionAssignment,
    AssignmentOp::ModuloAssignment,
    AssignmentOp::ExponentiationAssignment,
    AssignmentOp::LeftShiftAssignment,
    AssignmentOp::RightShiftAssignment,
    AssignmentOp::UnsignedRightShiftAssignment,
    AssignmentOp::BitwiseAndAssignment,
    AssignmentOp::BitwiseOrAssignment,
    AssignmentOp::BitwiseXorAssignment,
    AssignmentOp::AndAssignment,
    AssignmentOp::OrAssignment,
    AssignmentOp::NullishAssignment,
];

impl<'a> Expression<'a> {
    /// The expression at `node`, which a pair of parentheses leaves as it is.
    pub(crate) fn new(tree: &'a SyntaxTree, mut node: u32) -> Self {
        while tree.tag(node) == tag::PAREN {
            node = tree.a(node);
        }
        Self::with_form(tree, node, ExpressionForm::Node)
    }

    fn with_form(tree: &'a SyntaxTree, node: u32, form: ExpressionForm) -> Self {
        Self { tree, node, form }
    }

    /// Where the expression starts in the source.
    pub fn source_offset(&self) -> u32 {
        let tree = self.tree;
        match self.form {
            ExpressionForm::Node if tree.tag(self.node) == tag::STR => tree.b(self.node),
            ExpressionForm::MemberName => tree.name_after(tree.pos(self.node)),
            ExpressionForm::ShorthandKey => tree.pos(tree.b(self.node)),
            ExpressionForm::ShorthandValue { object_start } => object_start,
            ExpressionForm::TemplateChunk { template_start, .. } if self.is_failed_template_chunk() => template_start,
            _ => tree.pos(self.node),
        }
    }

    pub fn range(&self) -> SourceRange {
        self.tree.range(self.source_offset())
    }

    fn is_failed_template_chunk(&self) -> bool {
        let ExpressionForm::TemplateChunk { is_tagged: true, .. } = self.form else {
            return false;
        };
        literals::process_escape_sequences_impl(template_chunk_units(self.tree, self.node), EscapeMode::TaggedTemplate)
            .failed
    }

    fn template(&self, is_tagged: bool) -> TemplateLiteralData<'a> {
        let tree = self.tree;
        let items = tree.list(tree.a(self.node));
        TemplateLiteralData {
            tree,
            items,
            expressions: List::of_items(
                tree,
                items,
                TemplateElementContext {
                    is_tagged,
                    template_start: tree.pos(self.node),
                },
            ),
        }
    }

    pub fn kind(&self) -> ExpressionKind<'a> {
        let tree = self.tree;
        let node = self.node;
        let (a, b) = (tree.a(node), tree.b(node));
        match self.form {
            ExpressionForm::Node => {}
            ExpressionForm::PropertyKeyName => {
                return ExpressionKind::StringLiteral(StringLiteral::Name(tree.name_text(Name(a))));
            }
            ExpressionForm::MemberName => {
                let offset = tree.name_after(tree.pos(node));
                return if tree.tag(node) == tag::MEMBER {
                    ExpressionKind::Identifier(tree.plain_identifier(Name(b), offset))
                } else {
                    ExpressionKind::PrivateIdentifier(tree.private_identifier(b, offset))
                };
            }
            ExpressionForm::ShorthandKey => {
                return ExpressionKind::StringLiteral(StringLiteral::Name(tree.name_text(Name(a))));
            }
            ExpressionForm::ShorthandValue { object_start } => {
                return ExpressionKind::Identifier(tree.identifier(Name(a), object_start, node));
            }
            ExpressionForm::TemplateChunk { is_tagged, .. } => {
                return if self.is_failed_template_chunk() {
                    ExpressionKind::NullLiteral
                } else {
                    let mode = if is_tagged {
                        EscapeMode::TaggedTemplate
                    } else {
                        EscapeMode::StringLiteral
                    };
                    ExpressionKind::StringLiteral(StringLiteral::Source(template_chunk_units(tree, node), mode))
                };
            }
            ExpressionForm::TaggedTemplate => return ExpressionKind::TemplateLiteral(self.template(true)),
            ExpressionForm::DefaultConstructor => {
                return ExpressionKind::Function(FunctionId {
                    node,
                    source: FunctionSource::DefaultConstructor,
                });
            }
            ExpressionForm::DefaultConstructorSuperCall => {
                return ExpressionKind::SuperCall(SuperCallData {
                    arguments: CallArguments::DefaultConstructorSpread(CallArgument {
                        value: Expression::with_form(tree, node, ExpressionForm::DefaultConstructorArgument),
                        is_spread: true,
                    }),
                    is_synthetic: true,
                });
            }
            ExpressionForm::DefaultConstructorArgument => {
                return ExpressionKind::Identifier(
                    tree.plain_identifier(tree.script.default_constructor_parameter_name, tree.pos(node)),
                );
            }
        }
        let node_tag = tree.tag(node);
        let start = tree.pos(node);
        match node_tag {
            tag::IDENT => ExpressionKind::Identifier(tree.identifier(Name(a), start, node)),
            tag::NUM => ExpressionKind::NumericLiteral(NumericLiteral {
                text: tree.source_units(start, a),
            }),
            tag::BIGINT => ExpressionKind::BigIntLiteral(tree.source_units(start, a)),
            tag::STR => ExpressionKind::StringLiteral(StringLiteral::Source(
                tree.string_units(node),
                EscapeMode::StringLiteral,
            )),
            tag::REGEX => ExpressionKind::RegExpLiteral(RegExpLiteralData {
                pattern: tree.source_units(start + 1, a),
                flags: tree.source_units(a + 1, b),
            }),
            tag::TRUE => ExpressionKind::BooleanLiteral(true),
            tag::FALSE => ExpressionKind::BooleanLiteral(false),
            tag::NULL => ExpressionKind::NullLiteral,
            tag::THIS => ExpressionKind::This,
            tag::SUPER => ExpressionKind::Super,
            tag::NEW_TARGET => ExpressionKind::MetaProperty(MetaPropertyType::NewTarget),
            tag::IMPORT_META => ExpressionKind::MetaProperty(MetaPropertyType::ImportMeta),
            tag::PRIVATE_NAME => ExpressionKind::PrivateIdentifier(tree.private_identifier(a, start)),
            tag::FUNCTION => ExpressionKind::Function(FunctionId::function(node)),
            tag::MEMBER | tag::PRIVATE_MEMBER => ExpressionKind::Member(MemberExpressionData {
                object: Expression::new(tree, a),
                property: Expression::with_form(tree, node, ExpressionForm::MemberName),
                computed: false,
            }),
            tag::INDEX => ExpressionKind::Member(MemberExpressionData {
                object: Expression::new(tree, a),
                property: Expression::new(tree, b),
                computed: true,
            }),
            tag::CHAIN => ExpressionKind::OptionalChain(OptionalChainData { tree, node: a }),
            tag::SPREAD => ExpressionKind::Spread(Expression::new(tree, a)),
            tag::YIELD | tag::YIELD_STAR => ExpressionKind::Yield(YieldExpressionData {
                argument: (a != 0).then(|| Expression::new(tree, a)),
                is_yield_from: node_tag == tag::YIELD_STAR,
            }),
            tag::AWAIT => ExpressionKind::Await(Expression::new(tree, a)),
            tag::CONDITIONAL => ExpressionKind::Conditional(ConditionalExpressionData {
                test: Expression::new(tree, a),
                consequent: Expression::new(tree, tree.extra(b)),
                alternate: Expression::new(tree, tree.extra(b + 1)),
            }),
            tag::UNARY => {
                let op = match b {
                    token if token == T::Not as u32 => UnaryOp::Not,
                    token if token == T::Tilde as u32 => UnaryOp::BitwiseNot,
                    token if token == T::Plus as u32 => UnaryOp::Plus,
                    token if token == T::Minus as u32 => UnaryOp::Minus,
                    token if token == T::KTypeof as u32 => UnaryOp::Typeof,
                    token if token == T::KVoid as u32 => UnaryOp::Void,
                    _ => UnaryOp::Delete,
                };
                ExpressionKind::Unary {
                    op,
                    operand: Expression::new(tree, a),
                }
            }
            tag::PREFIX_UPDATE | tag::POSTFIX_UPDATE => ExpressionKind::Update(UpdateExpressionData {
                op: if b == T::Inc as u32 {
                    UpdateOp::Increment
                } else {
                    UpdateOp::Decrement
                },
                argument: Expression::new(tree, a),
                prefixed: node_tag == tag::PREFIX_UPDATE,
            }),
            tag::TAGGED_TEMPLATE => ExpressionKind::TaggedTemplateLiteral(TaggedTemplateData {
                tag: Expression::new(tree, a),
                template_literal: Expression::with_form(tree, b, ExpressionForm::TaggedTemplate),
            }),
            tag::IMPORT_CALL => ExpressionKind::ImportCall(ImportCallData {
                specifier: Expression::new(tree, a),
                options: (b != 0).then(|| Expression::new(tree, b)),
            }),
            tag::CALL if tree.tag(a) == tag::SUPER => ExpressionKind::SuperCall(SuperCallData {
                arguments: CallArguments::List(List::new(tree, b, ())),
                is_synthetic: false,
            }),
            tag::CALL | tag::NEW => {
                let data = CallExpressionData {
                    callee: Expression::new(tree, a),
                    arguments: CallArguments::List(List::new(tree, b, ())),
                };
                if node_tag == tag::CALL {
                    ExpressionKind::Call(data)
                } else {
                    ExpressionKind::New(data)
                }
            }
            tag::ARRAY => ExpressionKind::Array(List::new(tree, a, ())),
            tag::OBJECT => ExpressionKind::Object(List::new(tree, a, start)),
            tag::SEQUENCE => ExpressionKind::Sequence(List::new(tree, a, ())),
            tag::TEMPLATE => ExpressionKind::TemplateLiteral(self.template(false)),
            tag::CLASS => ExpressionKind::Class(tree.class(node)),
            tag::ASSIGN_PATTERN => self.assignment(0, a, b),
            _ if node_tag >= tag::BINARY_BASE => self.binary(node_tag - tag::BINARY_BASE, a, b),
            _ if node_tag >= tag::ASSIGN_BASE => self.assignment(node_tag - tag::ASSIGN_BASE, a, b),
            other => unreachable!("tag {other} is no expression"),
        }
    }

    fn binary(&self, operator: u8, a: u32, b: u32) -> ExpressionKind<'a> {
        let lhs = Expression::new(self.tree, a);
        let rhs = Expression::new(self.tree, b);
        match BINARY_OPERATORS[operator as usize] {
            Some(op) => ExpressionKind::Binary(BinaryExpressionData { op, lhs, rhs }),
            None => {
                let op = match operator {
                    0 => LogicalOp::NullishCoalescing,
                    1 => LogicalOp::Or,
                    _ => LogicalOp::And,
                };
                ExpressionKind::Logical(LogicalExpressionData { op, lhs, rhs })
            }
        }
    }

    fn assignment(&self, operator: u8, a: u32, b: u32) -> ExpressionKind<'a> {
        let tree = self.tree;
        let lhs = if matches!(tree.tag(a), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) {
            AssignmentLhs::Pattern(BindingPattern::new(tree, a, PatternMode::Assignment))
        } else {
            AssignmentLhs::Expression(Expression::new(tree, a))
        };
        ExpressionKind::Assignment(AssignmentExpressionData {
            op: ASSIGNMENT_OPERATORS[operator as usize],
            lhs,
            rhs: Expression::new(tree, b),
            lhs_is_parenthesized: tree.tag(a) == tag::PAREN,
        })
    }
}

// =============================================================================
// Statements
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StatementForm {
    /// The statement that the node's tag says.
    Node,
    /// The body of a function node.
    FunctionBody,
    /// The `return` that the expression of an arrow function node means.
    ExpressionBodyReturn,
    /// The body of a static block node.
    StaticBlockBody { class_start: u32 },
    /// The expression of `export default` at the node.
    ExportedExpression,
    /// The declaration of `export default` of a function node.
    ExportedFunction,
    /// The evaluation of the initializer of a class field node.
    ClassFieldInitializer,
    /// The body of the default constructor of a class node.
    DefaultConstructorBody,
    /// The `return super(...args)` of the default constructor of a class node.
    DefaultConstructorReturn,
}

#[derive(Clone, Copy)]
pub struct Statement<'a> {
    tree: &'a SyntaxTree,
    node: u32,
    form: StatementForm,
}

#[derive(Clone, Copy)]
pub enum StatementKind<'a> {
    // Basic
    Empty,
    Expression(Expression<'a>),
    Debugger,

    // Blocks (carry ScopeData for scope analysis)
    Block(Scope<'a>),
    FunctionBody { scope: Scope<'a>, in_strict_mode: bool },
    Program(ProgramData<'a>),

    // Control flow
    If(IfStatementData<'a>),
    While(WhileStatementData<'a>),
    DoWhile(WhileStatementData<'a>),
    For(ForStatementData<'a>),
    ForInOf(ForInOfStatementData<'a>),
    Switch(SwitchStatementData<'a>),
    With(WithStatementData<'a>),
    Labelled(LabelledStatementData<'a>),

    // Jumps
    Break { target_label: Option<Utf16View<'a>> },
    Continue { target_label: Option<Utf16View<'a>> },
    Return(Option<Expression<'a>>),
    Throw(Expression<'a>),
    Try(TryStatementData<'a>),

    // Declarations
    VariableDeclaration(VariableDeclarationData<'a>),
    UsingDeclaration(UsingDeclarationData<'a>),
    FunctionDeclaration(FunctionDeclarationData<'a>),
    ClassDeclaration(ClassData<'a>),

    // Module
    Import(ImportStatementData<'a>),
    Export(ExportStatementData<'a>),

    // Special
    ClassFieldInitializer(ClassFieldInitializerData<'a>),
}

impl<'a> Statement<'a> {
    pub(crate) fn new(tree: &'a SyntaxTree, node: u32) -> Self {
        Self::with_form(tree, node, StatementForm::Node)
    }

    fn with_form(tree: &'a SyntaxTree, node: u32, form: StatementForm) -> Self {
        Self { tree, node, form }
    }

    /// Where the statement starts in the source.
    pub fn source_offset(&self) -> u32 {
        let tree = self.tree;
        let node = self.node;
        match self.form {
            StatementForm::Node => match tree.tag(node) {
                tag::PROGRAM => tree.first_token_start,
                tag::FUNCTION_DECL => tree.pos(tree.a(node)),
                _ => tree.pos(node),
            },
            StatementForm::FunctionBody => {
                let function_tree = tree.a(node);
                if tree.extra(function_tree + 6) != 0 {
                    tree.pos(node)
                } else {
                    tree.extra(function_tree + 5)
                }
            }
            StatementForm::StaticBlockBody { class_start } => class_start,
            StatementForm::ExportedExpression => Expression::new(tree, node).source_offset(),
            StatementForm::ClassFieldInitializer => Expression::new(tree, tree.a(tree.b(node))).source_offset(),
            StatementForm::ExpressionBodyReturn
            | StatementForm::ExportedFunction
            | StatementForm::DefaultConstructorBody
            | StatementForm::DefaultConstructorReturn => tree.pos(node),
        }
    }

    pub fn range(&self) -> SourceRange {
        let tree = self.tree;
        let node = self.node;
        match self.form {
            StatementForm::Node if tree.tag(node) == tag::BLOCK => {
                tree.range_between(tree.pos(node), tree.block_end(tree.a(node)))
            }
            StatementForm::StaticBlockBody { class_start } => {
                tree.range_between(class_start, tree.block_end(tree.a(node)))
            }
            StatementForm::ClassFieldInitializer => {
                let initializer = tree.b(node);
                let mut range = Expression::new(tree, tree.a(initializer)).range();
                range.end = tree.script.positions.lines.position(tree.pos(initializer));
                range
            }
            _ => tree.range(self.source_offset()),
        }
    }

    fn function_scope(&self, children: ScopeChildren<'a>) -> Scope<'a> {
        Scope::of_function(self.tree, children, self.tree.function_scope(self.node))
    }

    fn static_block_scope(&self) -> Scope<'a> {
        let tree = self.tree;
        let row = tree.scope_row(tree.resolution(self.node).0);
        Scope::of_function(tree, ScopeChildren::List(List::new(tree, tree.a(self.node), ())), row)
    }

    fn loop_kind(&self) -> StatementKind<'a> {
        let tree = self.tree;
        let node = self.node;
        let (a, b) = (tree.a(node), tree.b(node));
        let is_declaration = |node: u32| {
            matches!(
                tree.tag(node),
                tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING
            )
        };
        match tree.tag(node) {
            tag::FOR => {
                let (test, update, body) = (tree.extra(b), tree.extra(b + 1), tree.extra(b + 2));
                StatementKind::For(ForStatementData {
                    init: (a != 0).then(|| {
                        if is_declaration(a) {
                            ForInit::Declaration(Statement::new(tree, a))
                        } else {
                            ForInit::Expression(Expression::new(tree, a))
                        }
                    }),
                    test: (test != 0).then(|| Expression::new(tree, test)),
                    update: (update != 0).then(|| Expression::new(tree, update)),
                    body: Statement::new(tree, body),
                })
            }
            other => {
                let kind = match other {
                    tag::FOR_IN => ForInOfKind::ForIn,
                    tag::FOR_OF => ForInOfKind::ForOf,
                    _ => ForInOfKind::ForAwaitOf,
                };
                let (right, body) = (tree.extra(b), tree.extra(b + 1));
                let lhs = if matches!(tree.tag(a), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) {
                    ForInOfLhs::Pattern(BindingPattern::new(tree, a, PatternMode::Assignment))
                } else if is_declaration(a) {
                    ForInOfLhs::Declaration(Statement::new(tree, a))
                } else {
                    ForInOfLhs::Expression(Expression::new(tree, a))
                };
                StatementKind::ForInOf(ForInOfStatementData {
                    kind,
                    lhs,
                    rhs: Expression::new(tree, right),
                    body: Statement::new(tree, body),
                })
            }
        }
    }

    fn export_kind(&self) -> StatementKind<'a> {
        let tree = self.tree;
        let node = self.node;
        let (a, b) = (tree.a(node), tree.b(node));
        let (statement, is_default_export) = match tree.tag(node) {
            tag::EXPORT_ALL => (None, false),
            tag::EXPORT_DEFAULT => {
                let inner_tag = tree.tag(a);
                let statement = if inner_tag == tag::FUNCTION && b != 0 {
                    Statement::with_form(tree, a, StatementForm::ExportedFunction)
                } else if inner_tag == tag::CLASS_DECL {
                    Statement::new(tree, a)
                } else {
                    Statement::with_form(tree, a, StatementForm::ExportedExpression)
                };
                (Some(statement), true)
            }
            _ if a != 0 => (None, false),
            _ => (Some(Statement::new(tree, b)), false),
        };
        StatementKind::Export(ExportStatementData {
            tree,
            node,
            statement,
            is_default_export,
        })
    }

    pub fn kind(&self) -> StatementKind<'a> {
        let tree = self.tree;
        let node = self.node;
        match self.form {
            StatementForm::Node => {}
            StatementForm::FunctionBody => {
                let function_tree = tree.a(node);
                let body = tree.extra(function_tree + 1);
                let is_expression = tree.extra(function_tree + 6) != 0;
                let children = if is_expression {
                    ScopeChildren::One(Statement::with_form(tree, node, StatementForm::ExpressionBodyReturn))
                } else {
                    ScopeChildren::List(List::new(tree, body, ()))
                };
                return StatementKind::FunctionBody {
                    scope: self.function_scope(children),
                    in_strict_mode: tree.script.index.flags[tree.b(node) as usize] & function_flag::STRICT != 0,
                };
            }
            StatementForm::ExpressionBodyReturn => {
                return StatementKind::Return(Some(Expression::new(tree, tree.extra(tree.a(node) + 1))));
            }
            StatementForm::StaticBlockBody { .. } => {
                return StatementKind::FunctionBody {
                    scope: self.static_block_scope(),
                    in_strict_mode: true,
                };
            }
            StatementForm::ExportedExpression => return StatementKind::Expression(Expression::new(tree, node)),
            StatementForm::ExportedFunction => {
                let name = tree.script.index.name[tree.b(node) as usize];
                return StatementKind::FunctionDeclaration(tree.function_declaration(node, name));
            }
            StatementForm::ClassFieldInitializer => {
                return StatementKind::ClassFieldInitializer(ClassFieldInitializerData {
                    expression: Expression::new(tree, tree.a(tree.b(node))),
                    key: ClassElementNode {
                        tree,
                        node,
                        class_start: 0,
                    }
                    .property_key(tree.a(node), tree.tag(node) == tag::COMPUTED_CLASS_FIELD),
                });
            }
            StatementForm::DefaultConstructorBody => {
                let has_super_class = tree.extra(tree.b(node)) != 0;
                return StatementKind::Block(Scope::new(
                    tree,
                    if has_super_class {
                        ScopeChildren::One(Statement::with_form(
                            tree,
                            node,
                            StatementForm::DefaultConstructorReturn,
                        ))
                    } else {
                        ScopeChildren::None
                    },
                ));
            }
            StatementForm::DefaultConstructorReturn => {
                return StatementKind::Return(Some(Expression::with_form(
                    tree,
                    node,
                    ExpressionForm::DefaultConstructorSuperCall,
                )));
            }
        }
        let (a, b) = (tree.a(node), tree.b(node));
        match tree.tag(node) {
            tag::PROGRAM => StatementKind::Program(ProgramData {
                scope: Scope::of_function(
                    tree,
                    ScopeChildren::List(List::new(tree, a, ())),
                    tree.scope_row(tree.program_scope_row),
                ),
                program_type: tree.program_type(),
                is_strict_mode: tree.is_strict_mode,
                has_top_level_await: tree.has_top_level_await,
            }),
            tag::EMPTY => StatementKind::Empty,
            tag::DEBUGGER => StatementKind::Debugger,
            tag::EXPR_STMT => StatementKind::Expression(Expression::new(tree, a)),
            tag::SCOPED => StatementKind::Block(Scope::new(tree, ScopeChildren::One(Statement::new(tree, a)))),
            tag::FOR | tag::FOR_IN | tag::FOR_OF | tag::FOR_AWAIT_OF => self.loop_kind(),
            tag::IF_FUNCTION_BLOCK => {
                StatementKind::Block(Scope::new(tree, ScopeChildren::One(Statement::new(tree, a))))
            }
            tag::BLOCK => StatementKind::Block(Scope::of_list(tree, a)),
            tag::IF => {
                let (consequent, alternate) = (tree.extra(b), tree.extra(b + 1));
                StatementKind::If(IfStatementData {
                    test: Expression::new(tree, a),
                    consequent: Statement::new(tree, consequent),
                    alternate: (alternate != 0).then(|| Statement::new(tree, alternate)),
                })
            }
            tag::WHILE => StatementKind::While(WhileStatementData {
                test: Expression::new(tree, a),
                body: Statement::new(tree, b),
            }),
            tag::DO_WHILE => StatementKind::DoWhile(WhileStatementData {
                test: Expression::new(tree, b),
                body: Statement::new(tree, a),
            }),
            tag::SWITCH => StatementKind::Switch(SwitchStatementData {
                discriminant: Expression::new(tree, a),
                cases: List::new(tree, tree.extra(b), ()),
            }),
            tag::WITH => StatementKind::With(WithStatementData {
                object: Expression::new(tree, a),
                body: Statement::new(tree, tree.extra(b)),
            }),
            tag::LABELED => StatementKind::Labelled(LabelledStatementData {
                label: tree.name_text(Name(a)),
                item: Statement::new(tree, b),
            }),
            tag::BREAK => StatementKind::Break {
                target_label: (a != NONE).then(|| tree.name_text(Name(a))),
            },
            tag::CONTINUE => StatementKind::Continue {
                target_label: (a != NONE).then(|| tree.name_text(Name(a))),
            },
            tag::RETURN => StatementKind::Return((a != 0).then(|| Expression::new(tree, a))),
            tag::THROW => StatementKind::Throw(Expression::new(tree, a)),
            tag::TRY => {
                let (parameter, handler, finalizer) = (tree.extra(b), tree.extra(b + 1), tree.extra(b + 2));
                StatementKind::Try(TryStatementData {
                    block: Statement::new(tree, a),
                    handler: (handler != 0).then(|| CatchClause {
                        tree,
                        offset: tree.extra(b + 3),
                        parameter: (parameter != 0).then(|| {
                            if tree.tag(parameter) == tag::IDENT {
                                CatchBinding::Identifier(tree.identifier(
                                    Name(tree.a(parameter)),
                                    tree.pos(parameter),
                                    parameter,
                                ))
                            } else {
                                CatchBinding::BindingPattern(BindingPattern::new(tree, parameter, PatternMode::Binding))
                            }
                        }),
                        body: Statement::new(tree, handler),
                    }),
                    finalizer: (finalizer != 0).then(|| Statement::new(tree, finalizer)),
                })
            }
            node_tag @ (tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING) => {
                let declarations = List::new(tree, a, tree.pos(node));
                match node_tag {
                    tag::USING | tag::AWAIT_USING => StatementKind::UsingDeclaration(UsingDeclarationData {
                        is_await: node_tag == tag::AWAIT_USING,
                        declarations,
                    }),
                    _ => StatementKind::VariableDeclaration(VariableDeclarationData {
                        kind: match node_tag {
                            tag::VAR => DeclarationKind::Var,
                            tag::LET => DeclarationKind::Let,
                            _ => DeclarationKind::Const,
                        },
                        declarations,
                    }),
                }
            }
            tag::FUNCTION_DECL => StatementKind::FunctionDeclaration(tree.function_declaration(a, b)),
            tag::CLASS_DECL => StatementKind::ClassDeclaration(tree.class(a)),
            tag::IMPORT => StatementKind::Import(ImportStatementData { tree, node }),
            tag::EXPORT_ALL | tag::EXPORT_DEFAULT | tag::EXPORT_NAMED => self.export_kind(),
            other => unreachable!("tag {other} is no statement"),
        }
    }

    pub fn function_declaration_for_labelled_item(&self) -> Option<FunctionDeclarationData<'a>> {
        // https://tc39.es/ecma262/#sec-static-semantics-lexicallyscopeddeclarations
        // LabelledItem : FunctionDeclaration
        // 1. Return « |FunctionDeclaration| ».
        match self.kind() {
            StatementKind::FunctionDeclaration(function) => Some(function),
            StatementKind::Labelled(data) => data.item.function_declaration_for_labelled_item(),
            _ => None,
        }
    }
}

impl_sequence_into_iterator!(FunctionParameters, ClassElements, ScopeChildren, CallArguments);
