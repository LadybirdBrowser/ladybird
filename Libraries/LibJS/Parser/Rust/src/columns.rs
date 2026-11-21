/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The syntax tree as flat columns. A node is a tag, a source offset and two `u32` operands, and shapes that need more
//! keep it in `extra`. Children come before their parents, so everything a function's body produced is one contiguous
//! tail of every column, and dropping a function's tree is one truncate of each column.
//!
//! Names and literals are not copied: an identifier operand is an interned name id, and a literal is its source offset.

use core::cell::Cell;

use ak::Zone;
use ak::ZoneVec;

use crate::u32_from_usize;

pub const NONE: u32 = u32::MAX;

#[rustfmt::skip]
pub mod tag {
    pub const INVALID: u8 = 0;
    // Expressions with no children.
    pub const IDENT: u8 = 1; pub const NUM: u8 = 2; pub const BIGINT: u8 = 3; pub const STR: u8 = 4;
    pub const REGEX: u8 = 5; pub const TRUE: u8 = 6; pub const FALSE: u8 = 7; pub const NULL: u8 = 8;
    pub const THIS: u8 = 9; pub const SUPER: u8 = 10; pub const NEW_TARGET: u8 = 11; pub const IMPORT_META: u8 = 12;
    pub const PRIVATE_NAME: u8 = 13; pub const FUNCTION: u8 = 14; pub const HOLE: u8 = 15;
    // a = child, b = name or second child.
    pub const MEMBER: u8 = 16; pub const INDEX: u8 = 17; pub const PRIVATE_MEMBER: u8 = 18;
    pub const OPT_MEMBER: u8 = 19; pub const OPT_INDEX: u8 = 20; pub const OPT_PRIVATE_MEMBER: u8 = 21;
    pub const CHAIN: u8 = 22; pub const PAREN: u8 = 23; pub const SPREAD: u8 = 24; pub const YIELD: u8 = 25;
    pub const YIELD_STAR: u8 = 26; pub const AWAIT: u8 = 27; pub const CONDITIONAL: u8 = 28;
    pub const UNARY: u8 = 29; pub const PREFIX_UPDATE: u8 = 30; pub const POSTFIX_UPDATE: u8 = 31;
    pub const TAGGED_TEMPLATE: u8 = 32; pub const IMPORT_CALL: u8 = 33;
    // a = callee, b = list.
    pub const CALL: u8 = 34; pub const OPT_CALL: u8 = 35; pub const NEW: u8 = 36;
    // a = list.
    pub const ARRAY: u8 = 37; pub const OBJECT: u8 = 38; pub const SEQUENCE: u8 = 39; pub const TEMPLATE: u8 = 40;
    pub const ARRAY_PATTERN: u8 = 41; pub const OBJECT_PATTERN: u8 = 42;
    // Object members. a = key, b = value.
    pub const PROPERTY: u8 = 43; pub const COMPUTED_PROPERTY: u8 = 44; pub const SHORTHAND: u8 = 45;
    pub const METHOD: u8 = 46; pub const GETTER: u8 = 47; pub const SETTER: u8 = 48;
    pub const COMPUTED_METHOD: u8 = 49; pub const COMPUTED_GETTER: u8 = 50; pub const COMPUTED_SETTER: u8 = 51;
    pub const REST: u8 = 52; pub const ASSIGN_PATTERN: u8 = 53;
    pub const CLASS: u8 = 54; pub const CLASS_FIELD: u8 = 55; pub const CLASS_STATIC_BLOCK: u8 = 56;
    // Statements.
    pub const EXPR_STMT: u8 = 57; pub const BLOCK: u8 = 58; pub const EMPTY: u8 = 59; pub const DEBUGGER: u8 = 60;
    pub const VAR: u8 = 61; pub const LET: u8 = 62; pub const CONST: u8 = 63; pub const USING: u8 = 64;
    pub const AWAIT_USING: u8 = 65; pub const DECLARATOR: u8 = 66; pub const IF: u8 = 67; pub const FOR: u8 = 68;
    pub const FOR_IN: u8 = 69; pub const FOR_OF: u8 = 70; pub const FOR_AWAIT_OF: u8 = 71; pub const WHILE: u8 = 72;
    pub const DO_WHILE: u8 = 73; pub const RETURN: u8 = 74; pub const THROW: u8 = 75; pub const BREAK: u8 = 76;
    pub const CONTINUE: u8 = 77; pub const TRY: u8 = 78; pub const SWITCH: u8 = 79; pub const CASE: u8 = 80;
    pub const LABELED: u8 = 81; pub const WITH: u8 = 82; pub const FUNCTION_DECL: u8 = 83; pub const CLASS_DECL: u8 = 84;
    pub const IMPORT: u8 = 85; pub const EXPORT_NAMED: u8 = 86; pub const EXPORT_DEFAULT: u8 = 87;
    pub const EXPORT_ALL: u8 = 88; pub const PROGRAM: u8 = 89; pub const SPECIFIER: u8 = 90;
    // A statement with the scope that was open around it. a = statement, b = scope row.
    pub const SCOPED: u8 = 91;
    // A class element marked `static`. a = element.
    pub const STATIC_ELEMENT: u8 = 92;
    pub const COMPUTED_CLASS_FIELD: u8 = 93;
    // The block that Annex B.3.4 wraps around the function of an `if`. a = function declaration, b = scope row.
    pub const IF_FUNCTION_BLOCK: u8 = 94;
    // A template's text. pos = its end, a = its start, b = whether it has an escape only a tagged template may contain.
    pub const TEMPLATE_CHUNK: u8 = 95;
    // The initializer of a class field. pos = where its expression ends, a = the expression, b = scope row.
    pub const FIELD_INITIALIZER: u8 = 96;
    // Operators: the tag base plus the token's offset in its group.
    pub const ASSIGN_BASE: u8 = 100; // 15 of them
    pub const BINARY_BASE: u8 = 120; // 25 of them
    // Rows that nothing refers to, which say what comes next to whoever goes through the rows in order.
    pub const MARK_CALLEE: u8 = 150; pub const MARK_TARGET: u8 = 151; pub const MARK_READ_TARGET: u8 = 152;
    pub const MARK_DECLARATION: u8 = 153; pub const MARK_TYPEOF: u8 = 154; pub const MARK_DELETE: u8 = 155;
    pub const MARK_KEY: u8 = 156; pub const MARK_COMPUTED_KEY: u8 = 157; pub const MARK_OBJECT: u8 = 158;
    pub const MARK_CONDITIONAL_TEST: u8 = 159; pub const MARK_CONDITIONAL_ELSE: u8 = 160;
    pub const MARK_LOGICAL: u8 = 161; pub const MARK_IF_TEST: u8 = 162; pub const MARK_ELSE: u8 = 163;
    pub const MARK_LOOP_HEAD: u8 = 164; pub const MARK_WHILE_TEST: u8 = 165; pub const MARK_DO_HEAD: u8 = 166;
    pub const MARK_DO_TEST: u8 = 167; pub const MARK_BLOCK: u8 = 171;
    // The start of a construct whose rows get walked by what they are rather than in order. a = its root row, or 0 if
    // it turned out to be nothing of the kind, b = what depends on the construct.
    pub const MARK_SUBTREE: u8 = 175;
    // A list starts to be built as an array. a = how many elements came before it.
    pub const MARK_ARRAY_START: u8 = 176;
    // An element appended to a list that is being built as an array is complete.
    pub const MARK_APPEND: u8 = 177;
    // `?.`, with a telling whether it is the first of its chain.
    pub const MARK_OPTIONAL: u8 = 178;
    // The target of `&&=`, `||=` or `??=` got read, and the rest may not happen. a = which one it is.
    pub const MARK_LOGICAL_ASSIGNMENT: u8 = 179;
    // What a row turned into once a mark came up for it, in a tree that only a sweep reads. A kind of row plus the mark
    // minus MARK_CALLEE.
    pub const IDENT_MARKED: u8 = 180; pub const MEMBER_MARKED: u8 = 190; pub const INDEX_MARKED: u8 = 200;
    pub const STR_KEY: u8 = 210; pub const NUM_KEY: u8 = 211;
    pub const IDENT_CALLEE: u8 = 180; pub const IDENT_TARGET: u8 = 181; pub const IDENT_READ_TARGET: u8 = 182;
    pub const IDENT_DECLARATION: u8 = 183; pub const IDENT_TYPEOF: u8 = 184; pub const IDENT_DELETE: u8 = 185;
    pub const IDENT_KEY: u8 = 186;
    pub const MEMBER_CALLEE: u8 = 190; pub const MEMBER_TARGET: u8 = 191; pub const MEMBER_READ_TARGET: u8 = 192;
    pub const MEMBER_DELETE: u8 = 195;
    pub const INDEX_CALLEE: u8 = 200; pub const INDEX_TARGET: u8 = 201; pub const INDEX_READ_TARGET: u8 = 202;
    pub const INDEX_DELETE: u8 = 205;
    // What a list built as an array is part of, once the list is complete. A spread element of an object.
    pub const CALL_ARRAY: u8 = 212; pub const NEW_ARRAY: u8 = 213; pub const ARRAY_SPREAD: u8 = 214;
    pub const OBJECT_SPREAD: u8 = 215;
    // The last link of an optional chain, if it gets called.
    pub const OPT_MEMBER_CALLEE: u8 = 216; pub const OPT_INDEX_CALLEE: u8 = 217;
    // `super` as what gets called, and that call.
    pub const SUPER_CALLEE: u8 = 218; pub const SUPER_CALL: u8 = 219;
    // Properties of `super`, and those of private names, in the way of MEMBER and MEMBER_MARKED.
    pub const SUPER_MEMBER_MARKED: u8 = 220; pub const SUPER_INDEX_MARKED: u8 = 227;
    pub const SUPER_MEMBER_CALLEE: u8 = 220; pub const SUPER_MEMBER_TARGET: u8 = 221;
    pub const SUPER_MEMBER_READ_TARGET: u8 = 222; pub const SUPER_MEMBER_DELETE: u8 = 225;
    pub const SUPER_INDEX_CALLEE: u8 = 227; pub const SUPER_INDEX_TARGET: u8 = 228;
    pub const SUPER_INDEX_READ_TARGET: u8 = 229; pub const SUPER_INDEX_DELETE: u8 = 232;
    pub const SUPER_MEMBER: u8 = 234; pub const SUPER_INDEX: u8 = 235;
    pub const PRIVATE_MEMBER_MARKED: u8 = 236;
    pub const PRIVATE_MEMBER_CALLEE: u8 = 236; pub const PRIVATE_MEMBER_TARGET: u8 = 237;
    pub const PRIVATE_MEMBER_READ_TARGET: u8 = 238;
    // A call of a name that makes it a direct `eval` if the name turns out to mean that, in a tree that only a sweep reads.
    pub const CALL_DIRECT_EVAL: u8 = 239; pub const CALL_DIRECT_EVAL_ARRAY: u8 = 240;
}

/// The tree that a parse makes, as columns that grow in the work zone of the parse.
pub struct Ast<'z> {
    pub tag: ZoneVec<'z, u8>,
    pub pos: ZoneVec<'z, u32>,
    pub a: ZoneVec<'z, u32>,
    pub b: ZoneVec<'z, u32>,
    pub extra: ZoneVec<'z, u32>,
    /// One row per binding of every scope.
    pub binding_name: ZoneVec<'z, u32>,
    pub binding_flags: ZoneVec<'z, u32>,
    /// One row per scope that has bindings: where its bindings start, and its flags.
    pub scope_first_binding: ZoneVec<'z, u32>,
    pub scope_flags: ZoneVec<'z, u32>,
    /// The most nodes that the tree is likely to have, which the columns make room for at once if they outgrow what
    /// they started with (see `grow()`).
    pub likely_most_nodes: usize,
}

impl<'z> Ast<'z> {
    pub fn with_capacity(zone: &'z Zone, nodes: usize) -> Self {
        Self::with_capacities(zone, nodes, nodes / 2)
    }

    pub fn with_capacities(zone: &'z Zone, nodes: usize, extra: usize) -> Self {
        let mut ast = Self {
            tag: ZoneVec::with_capacity_in(nodes, zone),
            pos: ZoneVec::with_capacity_in(nodes, zone),
            a: ZoneVec::with_capacity_in(nodes, zone),
            b: ZoneVec::with_capacity_in(nodes, zone),
            extra: ZoneVec::with_capacity_in(extra, zone),
            binding_name: ZoneVec::new_in(zone),
            binding_flags: ZoneVec::new_in(zone),
            scope_first_binding: ZoneVec::new_in(zone),
            scope_flags: ZoneVec::new_in(zone),
            likely_most_nodes: 0,
        };
        // Node zero stands for "no node" and for what failed to parse, and so does offset zero for "no list".
        ast.push(tag::INVALID, 0, 0, 0);
        ast.extra.push(0);
        ast
    }

    #[inline(always)]
    pub fn push(&mut self, tag: u8, pos: u32, a: u32, b: u32) -> u32 {
        let index = self.tag.len();
        if index == self.tag.capacity() {
            self.grow();
        }
        // SAFETY: All columns have room for the row, as grow() keeps them at the same capacity or more.
        unsafe {
            *self.tag.as_mut_ptr().add(index) = tag;
            *self.pos.as_mut_ptr().add(index) = pos;
            *self.a.as_mut_ptr().add(index) = a;
            *self.b.as_mut_ptr().add(index) = b;
            self.tag.set_len(index + 1);
            self.pos.set_len(index + 1);
            self.a.set_len(index + 1);
            self.b.set_len(index + 1);
        }
        index as u32
    }

    #[cold]
    fn grow(&mut self) {
        // NB: Columns that grow in steps leave a copy in the zone at each step, which is memory the job has used. A
        //     tree that outgrows its first room takes room for the most nodes it is likely to have in one step, and
        //     room that is never written takes no memory.
        let likely_most_nodes = core::mem::take(&mut self.likely_most_nodes);
        let wanted = self
            .tag
            .capacity()
            .max(1024)
            .max(likely_most_nodes.saturating_sub(self.tag.len()));
        self.tag.reserve(wanted);
        let capacity = self.tag.capacity();
        self.pos.reserve(capacity - self.pos.len());
        self.a.reserve(capacity - self.a.len());
        self.b.reserve(capacity - self.b.len());
        // NB: A list of the tree takes one row of `extra` for each of its items, which are nodes.
        self.extra.reserve(likely_most_nodes.saturating_sub(self.extra.len()));
    }

    pub fn truncate(&mut self, marks: &AstMarks) {
        self.tag.truncate(marks.nodes as usize);
        self.pos.truncate(marks.nodes as usize);
        self.a.truncate(marks.nodes as usize);
        self.b.truncate(marks.nodes as usize);
        self.extra.truncate(marks.extra as usize);
        self.binding_name.truncate(marks.bindings as usize);
        self.binding_flags.truncate(marks.bindings as usize);
        self.scope_first_binding.truncate(marks.scopes as usize);
        self.scope_flags.truncate(marks.scopes as usize);
    }

    pub fn marks(&self) -> AstMarks {
        AstMarks {
            nodes: self.tag.len() as u32,
            extra: self.extra.len() as u32,
            bindings: self.binding_name.len() as u32,
            scopes: self.scope_flags.len() as u32,
        }
    }

    /// The columns, for as long as the zone holds them.
    #[inline]
    pub fn into_columns(self) -> AstColumns<'z> {
        AstColumns {
            tag: self.tag.into_slice(),
            pos: self.pos.into_slice(),
            a: self.a.into_slice(),
            b: self.b.into_slice(),
            extra: self.extra.into_slice(),
            binding_name: self.binding_name.into_slice(),
            binding_flags: self.binding_flags.into_slice(),
            scope_first_binding: self.scope_first_binding.into_slice(),
            scope_flags: self.scope_flags.into_slice(),
        }
    }
}

/// The columns of a tree that is done, which is how everything after the parser reads it.
#[derive(Clone, Copy)]
pub struct AstColumns<'t> {
    pub tag: &'t [u8],
    pub pos: &'t [u32],
    pub a: &'t [u32],
    pub b: &'t [u32],
    pub extra: &'t [u32],
    pub binding_name: &'t [u32],
    pub binding_flags: &'t [u32],
    pub scope_first_binding: &'t [u32],
    pub scope_flags: &'t [u32],
}

#[derive(Clone, Copy)]
pub struct AstMarks {
    pub nodes: u32,
    pub extra: u32,
    pub bindings: u32,
    pub scopes: u32,
}

/// What a function leaves behind once its tree is gone, one row per function, as columns, which is how parses read it.
/// The first parse of a source builds it in its work zone (see [`FunctionIndexBuilder`]), and a copy of it stays with
/// the source (see `FunctionIndex`).
#[derive(Clone, Copy)]
pub struct FunctionIndexView<'t> {
    pub start: &'t [u32],
    /// Where parsing the function starts over. This is what tells two functions apart that start at the same offset,
    /// and for the code of a class that runs as a function, the offset of the class element or the class.
    pub params_start: &'t [u32],
    pub end: &'t [u32],
    pub flags: &'t [u32],
    pub length: &'t [u32],
    pub parameter_count: &'t [u32],
    /// What the function's code may do, as parser context bits.
    pub ctx: &'t [u16],
    /// The function's own name, if it has one.
    pub name: &'t [u32],
    /// Where each function's captures start in `capture_names`, and how many it has.
    pub capture_start: &'t [u32],
    pub capture_count: &'t [u32],
    /// What each function captures of the function around it, which is all that reparsing that function needs to know
    /// about a function it skips. A name that only passes through on its way further out is not in here: it is a capture
    /// of the outer function that it passes through.
    pub capture_names: &'t [u32],
    /// The rows by `params_start`, unless that is the order they are in. It often is not, as an arrow function only gets
    /// its row after its parameters, and so after the functions in them.
    pub rows_by_params_start: &'t [u32],
    /// What is left of the bindings of each function for code that goes without a resolver, as a pool of bytes: the
    /// number of bindings of the function itself that come up before their declaration and those, the number of scopes
    /// inside the function with bindings that have to exist when the scope starts and those scopes, then a bitmap of
    /// which of the other bindings of the function itself are captured, in the order of their declarations. A scope is
    /// the distance from the scope before it, the number of bindings and those. A function with nothing to say has no
    /// bytes.
    pub summaries: &'t [u8],
    pub summary_start: &'t [u32],
    /// A bit for every name of the source that anything declares, which all other names are free everywhere.
    pub declared_names: &'t [u64],
    /// Where the summary of the top level ends, which is the first thing in the pool.
    pub program_summary_end: u32,
    /// Whether the top level has a direct `eval`, so that names may mean more than they seem to there.
    pub program_has_direct_eval: bool,
}

/// The function index that the first parse of a source builds in its work zone. See [`FunctionIndexView`] for the
/// columns.
pub struct FunctionIndexBuilder<'z> {
    pub start: ZoneVec<'z, u32>,
    pub params_start: ZoneVec<'z, u32>,
    pub end: ZoneVec<'z, u32>,
    pub flags: ZoneVec<'z, u32>,
    pub length: ZoneVec<'z, u32>,
    pub parameter_count: ZoneVec<'z, u32>,
    pub ctx: ZoneVec<'z, u16>,
    pub name: ZoneVec<'z, u32>,
    pub capture_start: ZoneVec<'z, u32>,
    pub capture_count: ZoneVec<'z, u32>,
    pub capture_names: ZoneVec<'z, u32>,
    pub summaries: ZoneVec<'z, u8>,
    pub summary_start: ZoneVec<'z, u32>,
    pub declared_names: ZoneVec<'z, u64>,
    pub program_summary_end: u32,
    pub program_has_direct_eval: bool,
}

impl<'z> FunctionIndexBuilder<'z> {
    /// An index with room for `functions` rows.
    pub fn with_capacity(zone: &'z Zone, functions: usize) -> Self {
        Self {
            start: ZoneVec::with_capacity_in(functions, zone),
            params_start: ZoneVec::with_capacity_in(functions, zone),
            end: ZoneVec::with_capacity_in(functions, zone),
            flags: ZoneVec::with_capacity_in(functions, zone),
            length: ZoneVec::with_capacity_in(functions, zone),
            parameter_count: ZoneVec::with_capacity_in(functions, zone),
            ctx: ZoneVec::with_capacity_in(functions, zone),
            name: ZoneVec::with_capacity_in(functions, zone),
            capture_start: ZoneVec::with_capacity_in(functions, zone),
            capture_count: ZoneVec::with_capacity_in(functions, zone),
            capture_names: ZoneVec::new_in(zone),
            summaries: ZoneVec::new_in(zone),
            summary_start: ZoneVec::with_capacity_in(functions, zone),
            declared_names: ZoneVec::new_in(zone),
            program_summary_end: 0,
            program_has_direct_eval: false,
        }
    }

    /// The index as it is when the parse is done, with the rows sorted by where parsing them starts over.
    pub fn finish(self) -> FunctionIndexView<'z> {
        let zone = self.start.zone();
        let params_start = self.params_start.into_slice();
        let rows_by_params_start: &[u32] = if params_start.is_sorted() {
            &[]
        } else {
            let rows = zone.alloc_slice_from_iter(0..u32_from_usize(params_start.len()));
            // NB: Rows that start over at the same offset keep their order.
            rows.sort_unstable_by_key(|&row| (params_start[row as usize], row));
            rows
        };
        FunctionIndexView {
            start: self.start.into_slice(),
            params_start,
            end: self.end.into_slice(),
            flags: self.flags.into_slice(),
            length: self.length.into_slice(),
            parameter_count: self.parameter_count.into_slice(),
            ctx: self.ctx.into_slice(),
            name: self.name.into_slice(),
            capture_start: self.capture_start.into_slice(),
            capture_count: self.capture_count.into_slice(),
            capture_names: self.capture_names.into_slice(),
            rows_by_params_start,
            summaries: self.summaries.into_slice(),
            summary_start: self.summary_start.into_slice(),
            declared_names: self.declared_names.into_slice(),
            program_summary_end: self.program_summary_end,
            program_has_direct_eval: self.program_has_direct_eval,
        }
    }
}

impl FunctionIndexView<'_> {
    /// Where lookups for what comes after the function of `row` start out.
    #[inline]
    pub fn position_after_row(&self, row: usize) -> usize {
        let rows = self.rows_by_params_start;
        if rows.is_empty() {
            return row + 1;
        }
        let params_start = self.params_start[row];
        rows.partition_point(|&other| self.params_start[other as usize] <= params_start)
    }

    /// The row of the function that starts over at `params_start`. Lookups follow the source, so that the answer is mostly
    /// right where the last one left `cursor`.
    pub fn row_at_params_start(&self, params_start: u32, cursor: &Cell<usize>) -> Option<u32> {
        let rows = self.rows_by_params_start;
        let row_at = |position: usize| {
            if rows.is_empty() {
                (position < self.params_start.len()).then(|| u32_from_usize(position))
            } else {
                rows.get(position).copied()
            }
        };
        let offset_at = |position: usize| row_at(position).map(|row| self.params_start[row as usize]);
        let last = cursor.get();
        let position = match offset_at(last) {
            Some(next) if next == params_start => last,
            Some(next)
                if next > params_start && (last == 0 || offset_at(last - 1).is_some_and(|o| o < params_start)) =>
            {
                return None;
            }
            _ => {
                // NB: What comes next tends to be close by, past the functions nested in the last one.
                let is_before = |position: usize| offset_at(position).is_some_and(|offset| offset < params_start);
                let (mut low, mut high) = (last, last);
                let mut step = 1;
                if is_before(last) {
                    while is_before(high) {
                        low = high;
                        high = high.saturating_add(step).min(self.params_start.len());
                        step *= 2;
                    }
                } else {
                    while low != 0 && !is_before(low) {
                        high = low;
                        low = low.saturating_sub(step);
                        step *= 2;
                    }
                }
                while low < high {
                    let middle = low + (high - low) / 2;
                    if is_before(middle) {
                        low = middle + 1;
                    } else {
                        high = middle;
                    }
                }
                low
            }
        };
        cursor.set(position);
        row_at(position).filter(|&row| self.params_start[row as usize] == params_start)
    }

    /// Whether anything in the source declares `name`.
    #[inline(always)]
    pub fn is_declared_anywhere(&self, name: u32) -> bool {
        self.declared_names
            .get(name as usize / 64)
            .is_some_and(|bits| bits & (1 << (name % 64)) != 0)
    }

    /// Where the summary of the function at `row` is in the pool.
    #[inline(always)]
    pub fn summary_range(&self, row: u32) -> (usize, usize) {
        let row = row as usize;
        let start = self.summary_start[row] as usize;
        let end = self
            .summary_start
            .get(row + 1)
            .map_or(self.summaries.len(), |&end| end as usize);
        (start, end)
    }
}

pub mod function_flag {
    pub const ARROW: u32 = 1 << 0;
    pub const ASYNC: u32 = 1 << 1;
    pub const GENERATOR: u32 = 1 << 2;
    pub const METHOD: u32 = 1 << 3;
    pub const STRICT: u32 = 1 << 4;
    pub const SIMPLE_PARAMETERS: u32 = 1 << 5;
    pub const USES_THIS: u32 = 1 << 6;
    pub const CAPTURES_THIS: u32 = 1 << 7;
    pub const USES_ARGUMENTS: u32 = 1 << 8;
    pub const CONTAINS_EVAL: u32 = 1 << 9;
    /// The token after the function ends so close to the end of the source that the lexer needs padding to read it.
    pub const REACHES_SOURCE_END: u32 = 1 << 10;
    pub const DIRECT_EVAL: u32 = 1 << 11;
    /// A function expression that binds a name for itself which nothing looks up.
    pub const NAME_UNUSED: u32 = 1 << 12;
    pub const CAPTURES_ARGUMENTS: u32 = 1 << 13;
    pub const ARGUMENTS_ACCESS: u32 = 1 << 14;
    pub const INSIGHT_USES_THIS: u32 = 1 << 15;
    pub const INSIGHT_THIS_FROM_ENVIRONMENT: u32 = 1 << 16;
    pub const MIGHT_NEED_ARGUMENTS: u32 = 1 << 17;
    pub const SCOPE_USES_THIS: u32 = 1 << 18;
    pub const SCOPE_THIS_FROM_ENVIRONMENT: u32 = 1 << 19;
    /// A function expression with a name, which it binds for itself.
    pub const NAMED_EXPRESSION: u32 = 1 << 20;
    /// An arrow function whose expression body may not contain `in`.
    pub const NO_IN: u32 = 1 << 21;
    /// `arguments` or `eval` came up in the function, arrow function or not.
    pub const MENTIONS_ARGUMENTS: u32 = 1 << 22;
    /// The initializer of a class field, which runs as a function of its own.
    pub const FIELD_INITIALIZER: u32 = 1 << 23;
    /// A static block of a class, which runs as a function of its own.
    pub const STATIC_BLOCK: u32 = 1 << 24;
    /// The constructor of a class that does not spell one out, which has no source of its own.
    pub const DEFAULT_CONSTRUCTOR: u32 = 1 << 25;
    /// A constructor of a class with a superclass, whose `this` is not initialized until it calls super().
    pub const DERIVED_CONSTRUCTOR: u32 = 1 << 26;
    /// Everything the function does with `arguments` is reading `arguments.length` or `arguments[key]`, which the frame
    /// can answer without an arguments object.
    pub const ARGUMENTS_FROM_FRAME: u32 = 1 << 27;
    /// The function reads `arguments[key]`.
    pub const READS_ARGUMENTS_INDEX: u32 = 1 << 28;
    /// Something in the function may write a binding of the name of one of its parameters.
    pub const MAY_WRITE_PARAMETERS: u32 = 1 << 29;
}
