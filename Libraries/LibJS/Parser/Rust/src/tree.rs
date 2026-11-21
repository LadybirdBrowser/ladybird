/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tree of a parse as plain data: its columns and what scope analysis found out about it, all in slices. The
//! resolver reads the tree through [`TreeView`], and the `SyntaxTree` of LibJS wraps it with what is kept of the
//! source.

use super::columns::AstColumns;
use super::columns::tag;

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

/// Scope analysis result: how this identifier is resolved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LocalType {
    Argument,
    Variable,
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

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PatternMode {
    /// The pattern of a declaration, a parameter or a catch clause.
    Binding,
    /// The target of a destructuring assignment.
    Assignment,
}

// =============================================================================
// Names
// =============================================================================

/// An interned name of a source. Equal names of a source have equal ids.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub struct Name(pub u32);

// =============================================================================
// Resolution
// =============================================================================

/// What scope analysis found out about one identifier, packed into a `u32` of the tree's resolution column. An
/// identifier that it did not look at has the value zero.
#[derive(Clone, Copy, Default)]
pub struct Resolution(pub u32);

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
    pub const MAX_LOCAL_INDEX: u32 = u32::MAX >> Self::LOCAL_INDEX_SHIFT;

    #[inline]
    pub fn set_local(&mut self, binding: LocalBinding) {
        let local_type = match binding.local_type {
            LocalType::Argument => Self::LOCAL_ARGUMENT,
            LocalType::Variable => Self::LOCAL_VARIABLE,
        };
        self.0 = (self.0 & !Self::LOCAL_TYPE_MASK & ((1 << Self::LOCAL_INDEX_SHIFT) - 1))
            | local_type
            | (binding.index << Self::LOCAL_INDEX_SHIFT);
    }

    #[inline]
    pub fn set_declaration_kind(&mut self, kind: DeclarationKind) {
        self.0 = (self.0 & !Self::DECLARATION_KIND_MASK) | ((kind as u32) << Self::DECLARATION_KIND_SHIFT);
    }

    #[inline]
    pub fn set_is_global(&mut self) {
        self.0 |= Self::IS_GLOBAL;
    }

    #[inline]
    pub fn set_is_inside_scope_with_eval(&mut self) {
        self.0 |= Self::IS_INSIDE_SCOPE_WITH_EVAL;
    }

    #[inline]
    pub fn is_inside_scope_with_eval(self) -> bool {
        self.0 & Self::IS_INSIDE_SCOPE_WITH_EVAL != 0
    }

    /// Whether a scope with an eval around the identifier is one of the tree being resolved.
    #[inline]
    pub fn set_is_inside_tree_scope_with_eval(&mut self) {
        self.0 |= Self::IS_INSIDE_TREE_SCOPE_WITH_EVAL;
    }

    #[inline]
    pub fn is_inside_tree_scope_with_eval(self) -> bool {
        self.0 & Self::IS_INSIDE_TREE_SCOPE_WITH_EVAL != 0
    }

    #[inline]
    pub fn clear_is_global(&mut self) {
        self.0 &= !Self::IS_GLOBAL;
    }

    #[inline]
    pub fn set_is_hoisted(&mut self) {
        self.0 |= Self::IS_HOISTED;
    }

    #[inline]
    pub fn is_hoisted(self) -> bool {
        self.0 & Self::IS_HOISTED != 0
    }

    /// Marks the resolution of a function node as being that of the name of its declaration.
    #[inline]
    pub fn set_is_declared_function(&mut self) {
        self.0 |= Self::IS_DECLARED_FUNCTION;
    }

    #[inline]
    pub fn is_declared_function(self) -> bool {
        self.0 & Self::IS_DECLARED_FUNCTION != 0
    }

    #[inline]
    pub fn local_type(self) -> Option<LocalType> {
        match self.0 & Self::LOCAL_TYPE_MASK {
            Self::LOCAL_ARGUMENT => Some(LocalType::Argument),
            Self::LOCAL_VARIABLE => Some(LocalType::Variable),
            _ => None,
        }
    }

    #[inline]
    pub fn local_index(self) -> u32 {
        self.0 >> Self::LOCAL_INDEX_SHIFT
    }

    #[inline]
    pub fn declaration_kind(self) -> Option<DeclarationKind> {
        match (self.0 & Self::DECLARATION_KIND_MASK) >> Self::DECLARATION_KIND_SHIFT {
            1 => Some(DeclarationKind::Var),
            2 => Some(DeclarationKind::Let),
            3 => Some(DeclarationKind::Const),
            _ => None,
        }
    }

    #[inline]
    pub fn is_global(self) -> bool {
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
    pub scope_offsets: Option<(u32, u32)>,
    /// How many loops of its function the scope that declares it is in the body of, which is how often that scope can
    /// start again.
    pub enclosing_loops: u32,
}

/// A resolved local binding: the operand type and index assigned by scope analysis.
#[derive(Clone, Copy, Debug)]
pub struct LocalBinding {
    pub local_type: LocalType,
    pub index: u32,
}

/// A range of one of the columns of scope analysis results.
#[derive(Clone, Copy, Default)]
pub struct ColumnRange {
    pub start: u32,
    pub end: u32,
}

impl ColumnRange {
    pub fn of<T>(self, column: &[T]) -> &[T] {
        &column[self.start as usize..self.end as usize]
    }
}

/// What scope analysis found out about a function, a static block or a program.
#[derive(Clone, Copy, Default)]
pub struct ScopeRow {
    pub local_variables: ColumnRange,
    pub annexb_function_names: ColumnRange,
    pub non_local_var_count: u32,
    pub non_local_var_count_for_parameter_expressions: u32,
    pub has_function_scope_data: bool,
    pub has_function_named_arguments: bool,
    pub has_argument_parameter: bool,
    pub has_lexically_declared_arguments: bool,
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub contains_direct_call_to_eval: bool,
    pub contains_access_to_arguments_object: bool,
    /// Whether the code of the scope, which is everything but what its declaration instantiation does, can change the
    /// running environment. The code of nested functions has its own.
    pub creates_environment: bool,
}

/// The results of scope analysis, which are the same for the whole life of a tree.
#[derive(Clone, Copy, Default)]
pub struct ScopeTable<'t> {
    /// One per function of the function index, then one per static block, then the program's.
    pub rows: &'t [ScopeRow],
    pub local_variables: &'t [LocalVariable],
    pub annexb_function_names: &'t [Name],
}

// =============================================================================
// The tree
// =============================================================================

/// The tree of the code of a compile job and the results of scope analysis on that code, as slices of plain data.
#[derive(Clone, Copy)]
pub struct TreeView<'t> {
    pub ast: AstColumns<'t>,
    pub root: u32,
    pub first_token_start: u32,
    pub is_strict_mode: bool,
    pub has_top_level_await: bool,
    pub eval_referenced_private_names: &'t [Name],
    /// The resolution of the identifier at each node, if there is one.
    pub resolutions: &'t [Resolution],
    pub scopes: ScopeTable<'t>,
    /// The row of the program in `scopes`.
    pub program_scope_row: u32,
    /// The row in `scopes` of each function node of the tree whose code the tree holds, sorted by node.
    pub function_scope_rows: &'t [(u32, u32)],
}

impl TreeView<'_> {
    #[inline]
    pub fn tag(&self, node: u32) -> u8 {
        self.ast.tag[node as usize]
    }

    #[inline]
    pub fn a(&self, node: u32) -> u32 {
        self.ast.a[node as usize]
    }

    #[inline]
    pub fn b(&self, node: u32) -> u32 {
        self.ast.b[node as usize]
    }

    #[inline]
    pub fn pos(&self, node: u32) -> u32 {
        self.ast.pos[node as usize]
    }

    #[inline]
    pub fn extra(&self, index: u32) -> u32 {
        self.ast.extra[index as usize]
    }

    #[inline]
    pub fn list(&self, start: u32) -> &[u32] {
        let start = start as usize;
        let count = self.ast.extra[start] as usize;
        &self.ast.extra[start + 1..start + 1 + count]
    }

    /// Where the block statement at `node` ends.
    #[inline]
    pub fn end_of_block(&self, node: u32) -> u32 {
        self.block_end(self.a(node))
    }

    /// Where the code after a block starts, given the list of the block's statements.
    #[inline]
    pub fn block_end(&self, list: u32) -> u32 {
        self.extra(list + 1 + self.extra(list))
    }

    #[inline]
    pub fn resolution(&self, node: u32) -> Resolution {
        self.resolutions[node as usize]
    }

    #[inline]
    pub fn scope_row(&self, row: u32) -> &ScopeRow {
        &self.scopes.rows[row as usize]
    }

    pub fn function_scope(&self, function: u32) -> &ScopeRow {
        let index = self
            .function_scope_rows
            .binary_search_by_key(&function, |&(node, _)| node)
            .expect("the tree holds the code of the function");
        self.scope_row(self.function_scope_rows[index].1)
    }

    /// What scope analysis found out about the program.
    #[inline]
    pub fn program_scope(&self) -> &ScopeRow {
        self.scope_row(self.program_scope_row)
    }

    /// Whether the pattern at `node` has a computed key or an initializer, nested patterns included.
    pub fn pattern_contains_expression(&self, node: u32) -> bool {
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

    /// The optional chain at `node`, which reaches down to its innermost optional link, and what is below that is its
    /// base. Hands the links to `push_link`, outermost first, and returns the base.
    pub fn optional_chain(&self, node: u32, mut push_link: impl FnMut(u32)) -> u32 {
        let mut base = node;
        let mut remaining_optional_links = 0;
        let mut probe = node;
        while is_optional_chain_link(self.tag(probe)) {
            remaining_optional_links += u32::from(is_optional_link(self.tag(probe)));
            probe = self.a(probe);
        }
        while remaining_optional_links > 0 {
            remaining_optional_links -= u32::from(is_optional_link(self.tag(base)));
            push_link(base);
            base = self.a(base);
        }
        base
    }
}

/// Whether a node of this tag can be a link of an optional chain.
fn is_optional_chain_link(node_tag: u8) -> bool {
    matches!(
        node_tag,
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

/// Whether a link of an optional chain with this tag is an optional one.
pub fn is_optional_link(node_tag: u8) -> bool {
    matches!(
        node_tag,
        tag::OPT_MEMBER | tag::OPT_INDEX | tag::OPT_PRIVATE_MEMBER | tag::OPT_CALL
    )
}
