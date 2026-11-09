/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Parser state, scope analysis and the function boundary.
//!
//! Scope analysis keeps no map per scope. Each name has one row of state, and scopes are numbered in opening order, so
//! a scope's subtree is every scope id at or above its own once it closes:
//!
//! - A use pushes a row on the `uses` stack, linked to the name's previous use. Repeated uses in one scope are free.
//! - When a scope closes, each of its bindings pops the name's uses from that subtree. A popped use from another
//!   function means the binding is captured. The work is proportional to bindings, not to references.
//! - When a function closes, what is left of its uses are its free names. They collapse to one row each, which is what
//!   the function index keeps, and which is all an enclosing scope ever needs to know about the function.
//!
//! Errors do not unwind. The first one is recorded and the token stream ends, so every loop in the parser finishes on
//! its own, and no parse function has to return a Result.

use super::columns::*;
use super::intern::Interner;
use super::token::*;
use super::unit::Unit;

pub const USE_DEAD: u32 = u32::MAX;
/// On the function of a use: the use is inside a `with`.
pub const USE_IN_WITH: u32 = 1 << 31;

/// What the lexer finds past the end of a source without padding once a parse has given up on it.
static NO_MORE_SOURCE: [u16; 64] = [0; 64];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    /// Only keep the tree of top-level code and of functions that are likely called right away.
    Lazy,
    /// Keep the tree of everything.
    Full,
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub(crate) struct NameState {
    use_head: u32,
    use_scope: u32,
    decl_head: u32,
    stamp: u32,
}

#[derive(Clone, Copy)]
pub(crate) struct UseRow {
    name: u32,
    scope: u32,
    func: u32,
    prev: u32,
}

#[derive(Clone, Copy)]
pub(crate) struct DeclRow {
    name: u32,
    scope: u32,
    flags: u32,
    next_same_name: u32,
    next_in_scope: u32,
    /// The innermost scope a `var` of this name was written in, on the row in the function. Scopes are numbered in
    /// opening order, so that an open scope contains such a `var` exactly when this is at or above its id.
    last_var_scope: u32,
    /// The row in the tree that this binding got.
    out_binding: u32,
    /// Where this binding is in the summary of its function, or NONE, or what is known of one that comes up early.
    summary_slot: u32,
}

/// Stands for the row of the top level in what goes into summaries, which sorts after all functions.
const PROGRAM_SUMMARY_ROW: u32 = 0x7FFF_FFFE;
/// The `summary_slot` of a binding that comes up before its declaration.
const SUMMARY_SLOT_EARLY: u32 = 1 << 31;
/// The `summary_slot` of a binding of a scope inside a function, which has no place in the bitmap.
const SUMMARY_SLOT_INNER: u32 = 1 << 30;

pub mod decl {
    pub const LEXICAL: u32 = 1 << 0;
    pub const FUNCTION: u32 = 1 << 1;
    pub const VAR: u32 = 1 << 2;
    pub const SIMPLE_CATCH: u32 = 1 << 3;
    pub const PARAMETER: u32 = 1 << 4;
    pub const FUNCTION_NAME: u32 = 1 << 5;
    pub const ANNEX_B: u32 = 1 << 6;
    pub const CAPTURED: u32 = 1 << 7;
    /// The inner binding of a class's own name.
    pub const CLASS_NAME: u32 = 1 << 8;
    /// A function in a block whose name Annex B.3.3 also makes a `var`.
    pub const HOISTED: u32 = 1 << 9;
    /// A name bound by a catch parameter pattern, which no `var` may redeclare.
    pub const CATCH_PATTERN: u32 = 1 << 10;
    /// A `var` of a function body that a parameter expression refers to the name of.
    pub const REFERENCED_IN_PARAMETERS: u32 = 1 << 11;
    pub const DECLARED_LET: u32 = 1 << 12;
    pub const DECLARED_CONST: u32 = 1 << 13;
    pub const DECLARED_VAR: u32 = 1 << 14;
    /// A sloppy function declared in a block that may get hoisted.
    pub const HOISTABLE: u32 = 1 << 15;
    /// The name of a function that Annex B.3.3 makes a `var` here.
    pub const ANNEX_B_NAME: u32 = 1 << 16;
    /// Something in a `with` uses the name, so that the name may mean a property of the object.
    pub const USED_IN_WITH: u32 = 1 << 17;
}

pub mod scope_flag {
    pub const TOP: u16 = 1 << 0;
    pub const FUNCTION: u16 = 1 << 1;
    pub const STATIC_BLOCK: u16 = 1 << 2;
    pub const ARROW: u16 = 1 << 3;
    pub const BODY_OF_PARAMETERS: u16 = 1 << 4;
    pub const SWITCH: u16 = 1 << 5;
    pub const VAR_SCOPE: u16 = TOP | FUNCTION | STATIC_BLOCK | BODY_OF_PARAMETERS;
    pub const CATCH: u16 = 1 << 6;
    pub const CLASS: u16 = 1 << 7;
    pub const WITH: u16 = 1 << 8;
    pub const CLASS_FIELD: u16 = 1 << 9;
    pub const CATCH_BODY: u16 = 1 << 10;
}

/// Bits of a scope's row in the tree, above its scope flags.
pub const SCOPE_ROW_EVAL_IN_FUNCTION: u32 = 1 << 16;
pub const SCOPE_ROW_POISONED: u32 = 1 << 17;

#[derive(Clone, Copy)]
pub(crate) struct Scope {
    pub id: u32,
    pub flags: u16,
    pub func: u32,
    /// Where the summary of the function finds the scope.
    pub start: u32,
    pub decl_head: u32,
    /// Whether this scope or anything nested in it calls eval directly.
    pub has_eval: bool,
    /// Whether this scope or a scope of the same function nested in it calls eval directly.
    pub eval_in_function: bool,
    /// Whether code directly in this scope calls eval.
    pub eval_here: bool,
    /// The index of the scope that a `var` here belongs to.
    pub var_scope: u32,
    /// Whether code directly in this scope reads `arguments` in sloppy mode.
    pub arguments_access: bool,
    /// Whether a scope nested in this one does, which a parameter can not turn out to explain.
    pub nested_arguments_access: bool,
    pub use_mark: u32,
    pub hoist_mark: u32,
}

/// A sloppy function declaration on its way up to the scope that Annex B.3.3 hoists it to.
#[derive(Clone, Copy)]
pub(crate) struct Hoist {
    pub name: u32,
    /// Its declaration, whose binding row in the tree says what to mark.
    pub declaration: u32,
    pub own_scope: u32,
}

pub mod ctx {
    pub const GENERATOR: u32 = 1 << 0;
    pub const ASYNC: u32 = 1 << 1;
    pub const CAN_AWAIT: u32 = 1 << 2;
    pub const SUPER_PROPERTY: u32 = 1 << 3;
    pub const SUPER_CALL: u32 = 1 << 4;
    pub const NEW_TARGET: u32 = 1 << 5;
    pub const STATIC_BLOCK: u32 = 1 << 6;
    pub const NO_ARGUMENTS: u32 = 1 << 7;
    pub const RETURN: u32 = 1 << 8;
}

#[derive(Clone, Copy)]
pub(crate) struct Label {
    /// 0 for a plain label, 1 for a loop, 2 for a switch.
    pub kind: u8,
    pub statement_start: u32,
}

/// What a function saves of its surroundings while it parses.
pub(crate) struct FunctionState {
    pub marks: AstMarks,
    pub row: u32,
    pub ctx: u32,
    pub strict: bool,
    pub labels_base: u32,
    pub yield_pos: u32,
    pub await_pos: u32,
    pub await_ident_pos: u32,
    pub cover_error: u32,
    pub retain: bool,
    pub duplicate_parameter: u32,
    pub start: u32,
    pub flags: u32,
    pub has_body_scope: bool,
    pub body_start: u32,
    /// The first row of the body.
    pub first_body_row: u32,
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
    pub might_need_arguments: bool,
    pub in_class_of_sloppy_function: bool,
    pub is_arrow: bool,
    pub summary_ordinal: u32,
}

/// The first syntax error, at a source offset.
pub struct PendingError {
    pub pos: u32,
    pub message: String,
}

/// What the first parse of a top level went by, which parsing it again has to go by too.
#[derive(Clone, Copy)]
pub(crate) struct ProgramContext {
    pub(crate) context: u32,
    pub(crate) starts_in_strict_mode: bool,
    pub(crate) initiated_by_eval: bool,
    /// Whether no name means a global binding.
    pub(crate) has_no_globals: bool,
}

pub struct Parser<'a, U: Unit> {
    /// Where in `rows_by_params_start` the last lookup ended up.
    row_cursor: core::cell::Cell<usize>,
    /// Whether the tree is one for a sweep: rows get marks, and change their tag where a mark is about them.
    pub(crate) marks: bool,
    /// The row in the index of the code of a class that was parsed last, and what it knows of that code by now.
    pub(crate) pseudo_function_row: u32,
    pub(crate) pseudo_function_flags: u32,
    /// The mark of the block being parsed, for the functions it declares, and the function that it declared last.
    pub(crate) block_mark: u32,
    pub(crate) last_function_of_block: u32,
    /// Whether the list of arguments that was parsed last has a spread element.
    pub(crate) arguments_have_spread: bool,
    /// Whether anything uses what scopes declare and what code refers to, which a sweep with a summary does not.
    pub(crate) tracks_scopes: bool,
    /// Whether the first parse leaves a summary of the bindings of each function.
    records_summaries: bool,
    /// How many bindings of the function being parsed have a place in its summary.
    pub(crate) summary_ordinal: u32,
    pub(crate) src: *const U,
    pub len: u32,
    pub(crate) pos: u32,
    pub(crate) tok: T,
    pub(crate) tok_start: u32,
    pub(crate) tok_end: u32,
    pub(crate) tok_name: u32,
    pub(crate) nl_before: bool,
    pub(crate) tok_escaped: bool,
    /// A legacy octal number or a string with a legacy octal escape, both of which strict mode rejects.
    pub(crate) tok_legacy: bool,
    pub(crate) prev_end: u32,
    pub(crate) prev_start: u32,
    pub(crate) regex_body_end: u32,
    pub(crate) name_buffer: Vec<u8>,
    pub interner: Interner,

    pub ast: Ast,
    pub index: FunctionIndex,
    pub(crate) scratch: Vec<u32>,
    pub(crate) mode: Mode,
    pub(crate) retain: bool,

    pub module: bool,
    pub strict: bool,
    pub(crate) ctx: u32,
    pub(crate) labels: Vec<Label>,
    pub(crate) labels_base: u32,
    pub(crate) stack_limit: usize,
    recursion_depth: u32,
    pub(crate) potential_arrow_at: u32,
    pub(crate) cover_error: u32,
    pub(crate) yield_pos: u32,
    pub(crate) await_pos: u32,
    pub(crate) await_ident_pos: u32,
    pub(crate) duplicate_parameter: u32,
    /// The node of the last arrow function, which no operator may follow.
    pub(crate) last_arrow: u32,
    /// Whether the function expression about to be parsed follows `(` or `!`.
    /// Where the expression in the parentheses opened last starts. A function there is likely to be called, but not one
    /// that starts after the `(` of a list of arguments, which an arrow function usually is.
    pub(crate) parenthesized_expression_start: u32,
    /// Whether this is the left side of `for await`, where `async of` is no arrow function.
    pub(crate) for_await_head: bool,

    pub(crate) names: Vec<NameState>,
    pub(crate) uses: Vec<UseRow>,
    pub(crate) decls: Vec<DeclRow>,
    pub(crate) scopes: Vec<Scope>,
    pub(crate) cur_scope: u32,
    pub(crate) cur_func: u32,
    pub(crate) next_scope: u32,
    pub(crate) next_func: u32,
    /// The index row of each function id, or NONE for what only counts as a function.
    pub(crate) function_rows: Vec<u32>,
    /// The row whose summary tells of the scopes of each function id, which is that of the function around a `with`.
    pub(crate) summary_rows: Vec<u32>,
    pub(crate) stamp: u32,

    pub has_top_level_await: bool,
    pub first_token_start: u32,
    pub(crate) last_pseudo_function_row: u32,
    pub(crate) class_start: u32,
    pub(crate) if_start: u32,
    pub(crate) retain_next_function: bool,
    /// While reparsing: what the first parse found out about the functions of the source.
    first_pass_index: *const FunctionIndex,
    /// While reparsing: where the function being reparsed starts over.
    pub(crate) reparse_target: u32,
    pub(crate) with_depth: u32,
    pub(crate) names_used_in_with: Vec<u32>,
    pub(crate) free_names_scratch: Vec<u32>,
    /// The innermost label of each name, as its index in `labels`.
    pub(crate) label_indices: std::collections::HashMap<u32, u32>,
    /// The undeclared names that something in a `with` uses, which are not simply globals.
    pub global_names_used_in_with: Vec<u32>,
    pub(crate) hoists: Vec<Hoist>,
    pub(crate) uses_this: bool,
    pub(crate) uses_this_from_environment: bool,
    pub(crate) might_need_arguments: bool,
    /// Whether the code being parsed is in a class in a function that is not strict. The code of a class is strict, but
    /// `arguments` in its heritage and computed keys is the arguments object of that function.
    pub(crate) in_class_of_sloppy_function: bool,
    /// Whether the closest function, counting static blocks, is an arrow function.
    pub(crate) in_arrow: bool,

    /// The private names of each open class, created when a class first declares one.
    pub(crate) private_declared: Vec<std::collections::HashMap<u32, u8>>,
    pub(crate) private_used: Vec<(u32, u32)>,
    pub(crate) class_marks: Vec<(u32, u32)>,
    pub(crate) export_names: std::collections::HashSet<u32>,
    pub(crate) export_locals: Vec<(u32, u32)>,

    pub err: Option<PendingError>,
    /// Whether this is the code of an eval, whose private names the code around it may declare.
    pub initiated_by_eval: bool,
    /// The private names that no class in this code declares, for an eval.
    pub eval_referenced_private_names: Vec<u32>,
    _marker: core::marker::PhantomData<&'a [U]>,
}

/// How much native stack parsing may use by default. Nesting past this is a syntax error rather than a crash.
pub const DEFAULT_STACK_BUDGET: usize = 4 << 20;

/// How deeply statements and expressions may nest.
const MAX_RECURSION_DEPTH: u32 = 2000;

pub(crate) const RECURSION_LIMIT_MESSAGE: &str = "Maximum parser recursion depth exceeded";

impl<'a, U: Unit> Parser<'a, U> {
    /// # Safety
    /// `source` must be followed by at least 64 units of zeros within the same allocation.
    pub unsafe fn new(source: &'a [U], module: bool, mode: Mode) -> Self {
        let estimated_nodes = match mode {
            Mode::Lazy => (source.len() / 3 + 16).min(1 << 16),
            Mode::Full => source.len() / 3 + 16,
        };
        let estimated_functions = source.len() / 128;
        let mut parser = Self::with_capacities(
            source.as_ptr(),
            source.len(),
            module,
            mode,
            Interner::new(source.len()),
            estimated_nodes,
            estimated_functions,
            (source.len() / 8 + 16).min(1 << 16),
        );
        if source.len() >= u32::MAX as usize - 64 {
            parser.len = 0;
            parser.fail(0, "Source too long");
        }
        parser.skip_hashbang();
        parser
    }

    fn skip_hashbang(&mut self) {
        if self.at(0) == 0x23 && self.at(1) == 0x21 {
            // Hashbang comment.
            let mut p = 2;
            while p < self.len as usize && !matches!(self.at(p), 0x0A | 0x0D | 0x2028 | 0x2029) {
                p += 1;
            }
            self.pos = p as u32;
        }
    }

    /// A parser that only parses functions again, one at a time, knowing what the first parse of their source found.
    /// Its source comes with each function.
    ///
    /// # Safety
    /// `first_pass_index` must stay valid and unchanged for as long as the parser reparses functions.
    pub(crate) unsafe fn for_reparsing(
        module: bool,
        interner: Interner,
        first_pass_index: &FunctionIndex,
        source_length: u32,
    ) -> Self {
        let mut parser = Self::with_capacities(
            core::ptr::null(),
            source_length as usize,
            module,
            Mode::Lazy,
            interner,
            256,
            0,
            256,
        );
        parser.first_pass_index = first_pass_index;
        parser
    }

    #[allow(clippy::too_many_arguments)]
    fn with_capacities(
        source: *const U,
        length: usize,
        module: bool,
        mode: Mode,
        interner: Interner,
        estimated_nodes: usize,
        estimated_functions: usize,
        estimated_uses: usize,
    ) -> Self {
        let estimated_names = if estimated_uses < 1 << 16 {
            interner.len().next_power_of_two()
        } else {
            1 << 12
        };
        let mut parser = Self {
            row_cursor: core::cell::Cell::new(0),
            marks: false,
            pseudo_function_row: NONE,
            pseudo_function_flags: 0,
            block_mark: NONE,
            last_function_of_block: 0,
            arguments_have_spread: false,
            tracks_scopes: true,
            records_summaries: true,
            summary_ordinal: 0,
            src: source,
            len: length as u32,
            pos: 0,
            tok: T::Eof,
            tok_start: 0,
            tok_end: 0,
            tok_name: 0,
            nl_before: false,
            tok_escaped: false,
            tok_legacy: false,
            prev_end: 0,
            prev_start: 0,
            regex_body_end: 0,
            name_buffer: Vec::new(),
            interner,
            ast: Ast::with_capacity(estimated_nodes),
            index: FunctionIndex {
                start: Vec::with_capacity(estimated_functions),
                params_start: Vec::with_capacity(estimated_functions),
                end: Vec::with_capacity(estimated_functions),
                flags: Vec::with_capacity(estimated_functions),
                length: Vec::with_capacity(estimated_functions),
                parameter_count: Vec::with_capacity(estimated_functions),
                ctx: Vec::with_capacity(estimated_functions),
                name: Vec::with_capacity(estimated_functions),
                capture_start: Vec::with_capacity(estimated_functions),
                capture_count: Vec::with_capacity(estimated_functions),
                capture_names: Vec::new(),
                pending_captures: Vec::with_capacity(estimated_functions * 2),
                summaries: Vec::new(),
                declared_names: Vec::new(),
                summary_start: Vec::new(),
                pending_summaries: Vec::new(),
                pending_scope_bindings: Vec::new(),
                program_summary_end: 0,
                program_has_direct_eval: false,
                ..FunctionIndex::default()
            },
            scratch: Vec::with_capacity(estimated_uses.min(4096)),
            mode,
            retain: true,
            module,
            strict: module,
            ctx: if module { ctx::CAN_AWAIT } else { 0 },
            labels: Vec::new(),
            labels_base: 0,
            stack_limit: 0,
            recursion_depth: 0,
            potential_arrow_at: NONE,
            cover_error: NONE,
            yield_pos: 0,
            await_pos: 0,
            await_ident_pos: 0,
            duplicate_parameter: NONE,
            last_arrow: 0,
            parenthesized_expression_start: NONE,
            for_await_head: false,
            names: vec![
                NameState {
                    use_head: NONE,
                    use_scope: NONE,
                    decl_head: NONE,
                    stamp: 0
                };
                estimated_names
            ],
            uses: Vec::with_capacity(estimated_uses),
            decls: Vec::with_capacity(estimated_uses / 16),
            scopes: Vec::with_capacity((length / 64 + 1).min(256)),
            cur_scope: 0,
            cur_func: 0,
            next_scope: 1,
            next_func: 1,
            function_rows: vec![NONE],
            summary_rows: vec![PROGRAM_SUMMARY_ROW],
            stamp: 0,
            has_top_level_await: false,
            first_token_start: 0,
            last_pseudo_function_row: NONE,
            class_start: 0,
            if_start: 0,
            retain_next_function: false,
            first_pass_index: core::ptr::null(),
            reparse_target: NONE,
            with_depth: 0,
            names_used_in_with: Vec::new(),
            free_names_scratch: Vec::new(),
            label_indices: std::collections::HashMap::new(),
            global_names_used_in_with: Vec::new(),
            hoists: Vec::new(),
            uses_this: false,
            uses_this_from_environment: false,
            might_need_arguments: false,
            in_class_of_sloppy_function: false,
            in_arrow: false,

            private_declared: Vec::new(),
            private_used: Vec::new(),
            class_marks: Vec::new(),
            export_names: std::collections::HashSet::new(),
            export_locals: Vec::new(),
            err: None,
            initiated_by_eval: false,
            eval_referenced_private_names: Vec::new(),
            _marker: core::marker::PhantomData,
        };
        // NB: Code can start parsing with less stack left than the budget, as eval near the end of the stack does.
        parser.stack_limit = super::stack_limit_for_budget(DEFAULT_STACK_BUDGET)
            .max(super::vm_stack_limit_of_this_thread().unwrap_or(0));
        parser
    }

    /// Allows what `context` allows from the start of the program.
    pub fn add_context(&mut self, context: u32) {
        self.ctx |= context;
    }

    pub fn parse_program(&mut self) -> u32 {
        self.open_scope_with(scope_flag::TOP, 0);
        if !self.strict {
            self.strict = self.has_use_strict_directive(self.pos as usize);
        }
        self.lex();
        self.first_token_start = self.tok_start;
        let mark = self.scratch.len();
        while self.tok != T::Eof {
            let statement = self.parse_statement_list_item(true);
            self.scratch.push(statement);
        }
        let list = self.finish_list(mark);
        if self.is_reparsing() {
            let scope_row = self.close_scope();
            return self.ast.push(tag::PROGRAM, 0, list, scope_row);
        }
        if self.module {
            self.check_exported_locals();
        }
        self.check_unresolved_private_names();
        self.index.program_has_direct_eval = self.scopes.last().unwrap().eval_in_function;
        let scope_row = self.close_scope();
        for row in &self.uses {
            if row.name != USE_DEAD && row.func & USE_IN_WITH != 0 {
                self.global_names_used_in_with.push(row.name);
            }
        }
        self.group_captures();
        self.group_summaries();
        self.ast.push(tag::PROGRAM, 0, list, scope_row)
    }

    /// Goes through the top level once more with marks for a sweep, past all functions, after a parse that went well.
    pub(crate) fn parse_top_level_rows(&mut self, context: ProgramContext) -> Result<(Ast, u32), super::OutOfStack> {
        let tree = std::mem::replace(&mut self.ast, Ast::with_capacity(256));
        let private_names = std::mem::take(&mut self.eval_referenced_private_names);
        let (first_token_start, strict, has_top_level_await) =
            (self.first_token_start, self.strict, self.has_top_level_await);
        self.finish_reparse();
        self.export_names.clear();
        self.first_pass_index = &raw const self.index;
        self.tracks_scopes = false;
        self.marks = true;
        let node = self.reparse_program(context);
        self.first_pass_index = core::ptr::null();
        let error = self.err.take();
        let rows = std::mem::replace(&mut self.ast, tree);
        self.eval_referenced_private_names = private_names;
        self.first_token_start = first_token_start;
        self.strict = strict;
        self.has_top_level_await = has_top_level_await;
        match error {
            None => Ok((rows, node)),
            Some(error) => {
                assert_eq!(
                    error.message, RECURSION_LIMIT_MESSAGE,
                    "the first parse found no errors"
                );
                Err(super::OutOfStack)
            }
        }
    }

    /// Gets ready to go through the top level again, past all functions, like the first parse did.
    pub(crate) fn reparse_program(&mut self, context: ProgramContext) -> u32 {
        self.reparse_target = NONE;
        self.strict = context.starts_in_strict_mode;
        self.initiated_by_eval = context.initiated_by_eval;
        self.ctx = if self.module { ctx::CAN_AWAIT } else { 0 } | context.context;
        self.retain = true;
        self.retain_next_function = false;
        self.uses_this = false;
        self.uses_this_from_environment = false;
        self.might_need_arguments = false;
        self.in_arrow = false;
        self.cover_error = NONE;
        self.potential_arrow_at = NONE;
        self.last_arrow = 0;
        self.pos = 0;
        self.tok_end = 0;
        self.prev_end = 0;
        self.prev_start = 0;
        self.skip_hashbang();
        let node = self.parse_program();
        self.initiated_by_eval = false;
        node
    }

    /// Groups the captures by function, now that all of them are known.
    fn group_captures(&mut self) {
        let index = &mut self.index;
        let pending = std::mem::take(&mut index.pending_captures);
        for &(row, _) in &pending {
            index.capture_count[row as usize] += 1;
        }
        let mut next = 0;
        for row in 0..index.capture_start.len() {
            index.capture_start[row] = next;
            next += index.capture_count[row];
        }
        index.capture_names.resize(next as usize, 0);
        let mut filled = vec![0u32; index.capture_start.len()];
        for (row, name) in pending {
            let slot = index.capture_start[row as usize] + filled[row as usize];
            filled[row as usize] += 1;
            index.capture_names[slot as usize] = name;
        }
    }

    /// Writes out the summaries of all functions, now that all of their scopes closed.
    fn group_summaries(&mut self) {
        fn write_varint(bytes: &mut Vec<u8>, mut value: u32) {
            while value >= 0x80 {
                bytes.push(value as u8 | 0x80);
                value >>= 7;
            }
            bytes.push(value as u8);
        }
        fn write_scope_bindings(bytes: &mut Vec<u8>, bindings: &[(u32, u32, u32)]) {
            let scope_count = bindings.chunk_by(|a, b| a.1 == b.1).count();
            write_varint(bytes, scope_count as u32);
            let mut last_position = 0;
            for scope in bindings.chunk_by(|a, b| a.1 == b.1) {
                write_varint(bytes, scope[0].1 - last_position);
                last_position = scope[0].1;
                write_varint(bytes, scope.len() as u32);
                for binding in scope {
                    write_varint(bytes, binding.2);
                }
            }
        }
        let index = &mut self.index;
        let mut pending = std::mem::take(&mut index.pending_summaries);
        pending.sort_unstable();
        // NB: Rows with bit 31 set sort after all the others, so that each kind is a run of its own.
        let captured_start = pending.partition_point(|&(row, _)| row & SUMMARY_SLOT_EARLY == 0);
        let (mut early, mut captured) = (0, captured_start);
        let mut scope_bindings = std::mem::take(&mut index.pending_scope_bindings);
        scope_bindings.sort_unstable();
        let mut scope_binding = 0;
        let mut bitmap = Vec::new();
        // NB: The top level has no row of its own, so its summary comes first, and it has only scopes in it.
        let of_program = scope_bindings.partition_point(|binding| binding.0 != PROGRAM_SUMMARY_ROW);
        write_varint(&mut index.summaries, 0);
        write_scope_bindings(&mut index.summaries, &scope_bindings[of_program..]);
        index.program_summary_end = index.summaries.len() as u32;
        scope_bindings.truncate(of_program);
        for row in 0..index.summary_start.len() as u32 {
            index.summary_start[row as usize] = index.summaries.len() as u32;
            let early_start = early;
            while early < captured_start && pending[early].0 == row {
                early += 1;
            }
            bitmap.clear();
            while captured < pending.len() && pending[captured].0 & !SUMMARY_SLOT_EARLY == row {
                let ordinal = pending[captured].1 as usize;
                if ordinal / 8 >= bitmap.len() {
                    bitmap.resize(ordinal / 8 + 1, 0u8);
                }
                bitmap[ordinal / 8] |= 1 << (ordinal % 8);
                captured += 1;
            }
            let scope_bindings_start = scope_binding;
            while scope_binding < scope_bindings.len() && scope_bindings[scope_binding].0 == row {
                scope_binding += 1;
            }
            let bindings_of_scopes = &scope_bindings[scope_bindings_start..scope_binding];
            if early == early_start && bitmap.is_empty() && bindings_of_scopes.is_empty() {
                continue;
            }
            write_varint(&mut index.summaries, (early - early_start) as u32);
            for entry in &pending[early_start..early] {
                write_varint(&mut index.summaries, entry.1);
            }
            write_scope_bindings(&mut index.summaries, bindings_of_scopes);
            index.summaries.extend_from_slice(&bitmap);
        }
    }

    /// A declaration of something that was only the name of a function expression makes that a binding like any other.
    #[inline(always)]
    fn note_redeclared_function_name(&mut self, declaration: u32, name: u32, scope_index: usize, bind: Bind) {
        if self.decls[declaration as usize].flags & !decl::FUNCTION_NAME == 0 {
            self.note_new_binding_of(declaration as usize, name, scope_index, bind);
        }
    }

    /// A new binding of the function being parsed came up.
    #[inline(always)]
    fn note_new_binding(&mut self, name: u32, scope_index: usize, bind: Bind) {
        self.note_new_binding_of(self.decls.len() - 1, name, scope_index, bind);
    }

    #[inline(always)]
    fn note_new_binding_of(&mut self, d: usize, name: u32, scope_index: usize, bind: Bind) {
        if !self.records_summaries {
            return;
        }
        let scope = &self.scopes[scope_index];
        let head = self.names[name as usize].use_head;
        let is_function_level = scope.flags & scope_flag::VAR_SCOPE != 0;
        let kind = match bind {
            Bind::Let | Bind::Lexical | Bind::CatchPattern => 1,
            Bind::Const => 2,
            _ => 0,
        };
        // NB: A declaration of the name `arguments` has a say in the arguments object, which comes before all else.
        let is_of_arguments = name == N_ARGUMENTS && is_function_level;
        let is_early = bind != Bind::Parameter
            && (is_of_arguments || (head != NONE && self.uses[head as usize].scope >= scope.id));
        self.decls[d].summary_slot = if is_early {
            SUMMARY_SLOT_EARLY | kind
        } else if is_function_level {
            self.summary_ordinal += 1;
            self.summary_ordinal - 1
        } else {
            SUMMARY_SLOT_INNER | kind
        };
    }

    /// What the summary of its function says of a binding whose scope closes.
    #[inline(always)]
    fn note_closed_binding(&mut self, summary_slot: u32, name: u32, is_captured: bool, scope: Scope) {
        if summary_slot == NONE {
            return;
        }
        let index_row = self.summary_rows[scope.func as usize];
        if index_row == NONE {
            return;
        }
        // NB: What the top level itself declares is no business of a summary, that is declaration instantiation.
        if index_row == PROGRAM_SUMMARY_ROW && scope.flags & scope_flag::VAR_SCOPE != 0 {
            return;
        }
        let is_early = summary_slot & SUMMARY_SLOT_EARLY != 0;
        let entry = name << 3 | (summary_slot & 3) << 1 | u32::from(is_captured);
        if scope.flags & scope_flag::VAR_SCOPE == 0 {
            // NB: A `case` can be skipped, so that a declaration in a switch is no sign of having been initialized.
            if is_early || is_captured || scope.flags & scope_flag::SWITCH != 0 {
                self.index.pending_scope_bindings.push((index_row, scope.start, entry));
            }
        } else if is_early {
            self.index.pending_summaries.push((index_row, entry));
        } else if is_captured {
            self.index
                .pending_summaries
                .push((index_row | SUMMARY_SLOT_EARLY, summary_slot));
        }
    }

    #[cold]
    #[inline(never)]
    pub(crate) fn fail(&mut self, pos: u32, message: &'static str) {
        self.fail_with(pos, message.to_string());
    }

    #[cold]
    #[inline(never)]
    pub(crate) fn fail_with(&mut self, pos: u32, message: String) {
        if self.err.is_none() {
            self.err = Some(PendingError { pos, message });
        }
        if self.is_reparsing() {
            // The end of the source may have no padding, and the lexer is about to read it.
            self.src = NO_MORE_SOURCE.as_ptr().cast::<U>().wrapping_sub(self.len as usize);
        }
        self.tok = T::Eof;
        self.tok_start = self.len;
        self.tok_end = self.len;
        self.pos = self.len;
    }

    #[cold]
    #[inline(never)]
    pub(crate) fn unexpected(&mut self) {
        let message = self.unexpected_token_message();
        self.fail_with(self.tok_start, message);
    }

    #[cold]
    #[inline(never)]
    pub(crate) fn expected(&mut self, what: &str) {
        let message = format!("{}. Expected {what}", self.unexpected_token_message());
        self.fail_with(self.tok_start, message);
    }

    /// Fails for a token where a list goes on with a comma or ends.
    #[cold]
    #[inline(never)]
    fn expected_comma_or(&mut self, close: T) {
        self.expected(&format!("',' or {}", close.spelling()));
    }

    /// What a list expects after an item: a comma, or the token that ends the list, which the caller looks for.
    #[inline(always)]
    pub(crate) fn expect_comma_or(&mut self, close: T) {
        if self.tok == T::Comma {
            self.next();
        } else {
            self.expected_comma_or(close);
        }
    }

    /// Calls the current token what the source calls it.
    fn unexpected_token_message(&self) -> String {
        const LONGEST_TOKEN_SHOWN: u32 = 40;
        if self.tok == T::Eof {
            return "Unexpected end of input".to_string();
        }
        let end = self.tok_end.min(self.tok_start + LONGEST_TOKEN_SHOWN);
        let units: Vec<u16> = (self.tok_start..end)
            .map(|index| self.at(index as usize) as u16)
            .collect();
        let ellipsis = if end < self.tok_end { "..." } else { "" };
        format!("Unexpected token '{}{ellipsis}'", String::from_utf16_lossy(&units))
    }

    #[inline(always)]
    pub(crate) fn eat(&mut self, kind: T) -> bool {
        if self.tok == kind {
            self.next();
            true
        } else {
            false
        }
    }

    #[inline(always)]
    pub(crate) fn expect(&mut self, kind: T) {
        if self.tok == kind {
            self.next();
        } else {
            self.expected(kind.spelling());
        }
    }

    #[inline(always)]
    pub(crate) fn is_contextual(&self, name: u32) -> bool {
        self.tok == T::Name && self.tok_name == name && !self.tok_escaped
    }

    #[inline(always)]
    pub(crate) fn eat_contextual(&mut self, name: u32) -> bool {
        if self.is_contextual(name) {
            self.next();
            true
        } else {
            false
        }
    }

    #[inline(always)]
    pub(crate) fn can_insert_semicolon(&self) -> bool {
        self.tok == T::Eof || self.tok == T::RBrace || self.nl_before
    }

    #[inline(always)]
    pub(crate) fn semicolon(&mut self) {
        if !self.eat(T::Semi) && !self.can_insert_semicolon() {
            self.expected("';'");
        }
    }

    /// Runs `look` on the token stream and puts it back. An error it runs into comes up again if the parser gets there.
    pub(crate) fn look_ahead<R>(&mut self, look: impl FnOnce(&mut Self) -> R) -> R {
        let saved = (
            self.pos,
            self.tok,
            self.tok_start,
            self.tok_end,
            self.tok_name,
            self.nl_before,
            self.tok_escaped,
            self.tok_legacy,
            self.prev_end,
            self.prev_start,
        );
        let had_error = self.err.is_some();
        let result = look(self);
        if !had_error {
            self.err = None;
        }
        (
            self.pos,
            self.tok,
            self.tok_start,
            self.tok_end,
            self.tok_name,
            self.nl_before,
            self.tok_escaped,
            self.tok_legacy,
            self.prev_end,
            self.prev_start,
        ) = saved;
        result
    }

    /// The kind and name of the token after the current one, and whether a newline comes before it.
    pub(crate) fn peek(&mut self) -> (T, u32, bool) {
        self.look_ahead(|parser| {
            parser.lex();
            (parser.tok, parser.tok_name, parser.nl_before)
        })
    }

    #[inline(always)]
    pub(crate) fn finish_list(&mut self, mark: usize) -> u32 {
        let start = self.ast.extra.len() as u32;
        let count = self.scratch.len() - mark;
        self.ast.extra.push(count as u32);
        self.ast.extra.extend_from_slice(&self.scratch[mark..]);
        self.scratch.truncate(mark);
        start
    }

    #[inline(always)]
    pub fn list(&self, start: u32) -> &[u32] {
        let count = self.ast.extra[start as usize] as usize;
        &self.ast.extra[start as usize + 1..start as usize + 1 + count]
    }

    /// Leaves a mark in the rows, for a sweep over them.
    #[inline(always)]
    pub(crate) fn mark(&mut self, mark: u8, a: u32) {
        self.mark_at(mark, a, self.tok_start);
    }

    /// Leaves a mark in the rows. What it makes of a reference goes by `position` from then on, which is where whatever the
    /// reference is for is in the source.
    #[inline(always)]
    pub(crate) fn mark_at(&mut self, mark: u8, a: u32, position: u32) {
        if !self.marks {
            return;
        }
        if (tag::MARK_CALLEE..=tag::MARK_KEY).contains(&mark) {
            let mut node = self.ast.tag.len() - 1;
            while self.ast.tag[node] == tag::PAREN {
                node = self.ast.a[node] as usize;
            }
            // NB: What `delete` of an optional chain is about is the end of the chain.
            if mark == tag::MARK_DELETE && self.ast.tag[node] == tag::CHAIN {
                node = self.ast.a[node] as usize;
                self.ast.tag[node] = match self.ast.tag[node] {
                    tag::MEMBER | tag::OPT_MEMBER => tag::MEMBER_DELETE,
                    tag::INDEX | tag::OPT_INDEX => tag::INDEX_DELETE,
                    other => other,
                };
                return;
            }
            // NB: What gets called on what comes from the end of the chain too, through the parentheses around it.
            if mark == tag::MARK_CALLEE && self.ast.tag[node] == tag::CHAIN {
                node = self.ast.a[node] as usize;
                self.ast.tag[node] = match self.ast.tag[node] {
                    tag::MEMBER | tag::OPT_MEMBER => tag::MEMBER_CALLEE,
                    tag::INDEX | tag::OPT_INDEX => tag::INDEX_CALLEE,
                    tag::PRIVATE_MEMBER | tag::OPT_PRIVATE_MEMBER => tag::PRIVATE_MEMBER_CALLEE,
                    tag::SUPER_MEMBER => tag::SUPER_MEMBER_CALLEE,
                    tag::SUPER_INDEX => tag::SUPER_INDEX_CALLEE,
                    other => other,
                };
                return;
            }
            let offset = mark - tag::MARK_CALLEE;
            let marked = match self.ast.tag[node] {
                tag::IDENT => tag::IDENT_MARKED + offset,
                tag::MEMBER if mark != tag::MARK_KEY && mark != tag::MARK_DECLARATION && mark != tag::MARK_TYPEOF => {
                    tag::MEMBER_MARKED + offset
                }
                tag::INDEX if mark != tag::MARK_KEY && mark != tag::MARK_DECLARATION && mark != tag::MARK_TYPEOF => {
                    tag::INDEX_MARKED + offset
                }
                tag::SUPER if mark == tag::MARK_CALLEE => tag::SUPER_CALLEE,
                tag::SUPER_MEMBER
                    if mark != tag::MARK_KEY && mark != tag::MARK_DECLARATION && mark != tag::MARK_TYPEOF =>
                {
                    tag::SUPER_MEMBER_MARKED + offset
                }
                tag::SUPER_INDEX
                    if mark != tag::MARK_KEY && mark != tag::MARK_DECLARATION && mark != tag::MARK_TYPEOF =>
                {
                    tag::SUPER_INDEX_MARKED + offset
                }
                tag::PRIVATE_MEMBER
                    if mark != tag::MARK_KEY && mark != tag::MARK_DECLARATION && mark != tag::MARK_TYPEOF =>
                {
                    tag::PRIVATE_MEMBER_MARKED + offset
                }
                tag::OPT_MEMBER if mark == tag::MARK_CALLEE => tag::OPT_MEMBER_CALLEE,
                tag::OPT_INDEX if mark == tag::MARK_CALLEE => tag::OPT_INDEX_CALLEE,
                tag::OPT_PRIVATE_MEMBER if mark == tag::MARK_CALLEE => tag::PRIVATE_MEMBER_CALLEE,
                tag::STR if mark == tag::MARK_KEY => tag::STR_KEY,
                tag::NUM if mark == tag::MARK_KEY => tag::NUM_KEY,
                _ => 0,
            };
            if marked != 0 {
                self.ast.tag[node] = marked;
                if mark == tag::MARK_DECLARATION {
                    self.ast.b[node] = a;
                } else if mark == tag::MARK_TYPEOF || (mark == tag::MARK_CALLEE && marked == tag::IDENT_CALLEE) {
                    // NB: A name is where it is until it takes more than reading it.
                    self.ast.b[node] = position;
                } else if mark != tag::MARK_KEY {
                    self.ast.pos[node] = position;
                }
                return;
            }
        }
        self.ast.push(mark, self.prev_start, a, 0);
    }

    /// Leaves a mark at the start of a construct that does not go through the rows in order, for `end_subtree()`.
    #[inline(always)]
    pub(crate) fn begin_subtree(&mut self) -> u32 {
        if !self.marks {
            return NONE;
        }
        self.ast.push(tag::MARK_SUBTREE, self.tok_start, 0, 0)
    }

    /// A list starts to have spread elements in it, after `count` others, in something that is at `position`.
    #[inline(always)]
    pub(crate) fn mark_spread_start(&mut self, count: usize, position: u32) {
        if self.marks {
            self.ast.push(tag::MARK_SPREAD_START, position, count as u32, 0);
        }
    }

    /// The construct that `begin_subtree()` left `mark` for ended with the row `root`.
    #[inline(always)]
    pub(crate) fn end_subtree(&mut self, mark: u32, root: u32) -> u32 {
        if mark != NONE {
            self.ast.a[mark as usize] = root;
        }
        root
    }

    /// Whether the row is that of a plain identifier, marked or not.
    #[inline(always)]
    pub(crate) fn is_identifier_row(&self, node: u32) -> bool {
        let row_tag = self.ast.tag[node as usize];
        row_tag == tag::IDENT || (tag::IDENT_MARKED..tag::MEMBER_MARKED).contains(&row_tag)
    }

    // === Scopes ===

    #[inline(always)]
    fn name_state(&mut self, name: u32) -> &mut NameState {
        if name as usize >= self.names.len() {
            self.grow_names(name);
        }
        // SAFETY: The table covers the name now.
        unsafe { self.names.get_unchecked_mut(name as usize) }
    }

    #[cold]
    #[inline(never)]
    fn grow_names(&mut self, name: u32) {
        let new_len = ((name as usize + 1) * 2).next_power_of_two();
        self.names.resize(
            new_len,
            NameState {
                use_head: NONE,
                use_scope: NONE,
                decl_head: NONE,
                stamp: 0,
            },
        );
    }

    #[inline(always)]
    pub(crate) fn note_use(&mut self, name: u32) {
        if !self.tracks_scopes {
            return;
        }
        let scope = self.cur_scope;
        let func = self.cur_func;
        let next = self.uses.len() as u32;
        let state = self.name_state(name);
        if state.use_scope == scope {
            return;
        }
        let prev = state.use_head;
        state.use_head = next;
        state.use_scope = scope;
        self.uses.push(UseRow {
            name,
            scope,
            func,
            prev,
        });
    }

    #[inline(always)]
    pub(crate) fn open_scope(&mut self, flags: u16) {
        self.open_scope_with(flags, self.cur_func);
    }

    #[inline(always)]
    pub(crate) fn open_scope_with(&mut self, flags: u16, func: u32) {
        let id = self.next_scope;
        self.next_scope += 1;
        let index = self.scopes.len() as u32;
        let var_scope = if flags & scope_flag::VAR_SCOPE != 0 {
            index
        } else {
            self.scopes.last().map_or(0, |scope| scope.var_scope)
        };
        self.scopes.push(Scope {
            id,
            flags,
            func,
            start: self.tok_start,
            decl_head: NONE,
            has_eval: false,
            eval_in_function: false,
            eval_here: false,
            var_scope,
            arguments_access: false,
            nested_arguments_access: false,
            use_mark: self.uses.len() as u32,
            hoist_mark: self.hoists.len() as u32,
        });
        self.cur_scope = id;
        self.cur_func = func;
    }

    /// Records uses of a name in nested functions that do not resolve here, for a name that matters here anyway, since a
    /// reparse would not see them otherwise.
    fn record_uses_as_captures(&mut self, name: u32, head: u32, scope_id: u32, func: u32) {
        let mut use_row = head;
        while use_row != NONE {
            let row = self.uses[use_row as usize];
            if row.scope < scope_id {
                break;
            }
            let use_func = row.func & !USE_IN_WITH;
            if use_func != func {
                let index_row = self.function_rows[use_func as usize];
                if index_row != NONE {
                    self.index
                        .pending_captures
                        .push((index_row, name | (row.func & USE_IN_WITH)));
                }
            }
            use_row = row.prev;
        }
    }

    /// Annex B.3.3, for the hoists that reached the innermost scope, which is about to close.
    fn process_hoists(&mut self, scope: Scope, top: usize) {
        if self.hoists.len() <= scope.hoist_mark as usize {
            return;
        }
        let mut kept = scope.hoist_mark as usize;
        let is_var_scope = scope.flags & scope_flag::VAR_SCOPE != 0;
        for i in scope.hoist_mark as usize..self.hoists.len() {
            let hoist = self.hoists[i];
            let row = self.find_declaration(hoist.name, scope.id);
            let flags = if row == NONE { 0 } else { self.decls[row as usize].flags };
            let is_own = hoist.own_scope == scope.id;
            if !is_var_scope && row != NONE && flags & (decl::SIMPLE_CATCH | decl::CATCH_PATTERN) != 0 {
                // The name now also means the function, so it has to stay reachable by name.
                self.decls[row as usize].flags |= decl::HOISTABLE;
            }
            let lexical = flags & decl::LEXICAL != 0 && flags & decl::SIMPLE_CATCH == 0;
            if lexical && self.decls[row as usize].summary_slot & SUMMARY_SLOT_INNER != 0 {
                // NB: This binding may come after the function, which has to see it to know that it does not hoist.
                self.decls[row as usize].summary_slot |= SUMMARY_SLOT_EARLY;
            }
            if lexical || flags & decl::CATCH_PATTERN != 0 || (!is_own && flags & decl::HOISTABLE != 0) {
                continue;
            }
            if !is_var_scope {
                self.hoists[kept] = hoist;
                kept += 1;
                continue;
            }
            // That a hoist got here at all keeps the name out of registers, whether or not it makes a binding.
            let mut is_parameter = flags & decl::PARAMETER != 0;
            if scope.flags & scope_flag::BODY_OF_PARAMETERS != 0 {
                let outer = self.find_declaration(hoist.name, self.scopes[top - 1].id);
                if outer != NONE && self.decls[outer as usize].flags & decl::PARAMETER != 0 {
                    is_parameter = true;
                    self.decls[outer as usize].flags |= decl::ANNEX_B;
                }
            }
            if row == NONE {
                self.add_declaration(hoist.name, top, decl::ANNEX_B);
                if self.records_summaries {
                    let d = self.decls.len() - 1;
                    self.decls[d].summary_slot = SUMMARY_SLOT_EARLY;
                }
            } else if flags & !decl::FUNCTION_NAME == 0 {
                // NB: The name of a function expression is bound outside of the function, which gets a `var` of its own.
                self.decls[row as usize].flags |= decl::ANNEX_B;
                if self.records_summaries {
                    self.decls[row as usize].summary_slot = SUMMARY_SLOT_EARLY;
                }
            } else {
                self.decls[row as usize].flags |= decl::ANNEX_B;
                if !is_parameter {
                    // NB: Evaluating the function stores into the binding, which may come before its declaration.
                    self.make_binding_early(row, top);
                }
            }
            let row = self.find_declaration(hoist.name, scope.id);
            if is_parameter {
                continue;
            }
            self.decls[row as usize].flags |= decl::ANNEX_B_NAME;
            let binding = if hoist.declaration == NONE {
                NONE
            } else {
                self.decls[hoist.declaration as usize].out_binding
            };
            if binding != NONE {
                self.ast.binding_flags[binding as usize] |= decl::HOISTED;
            }
        }
        self.hoists.truncate(kept);
    }

    /// Makes declaration `d` of the scope at `top` one that the code of its function knows from the start, and not only
    /// from where it comes up. The declarations after it in the order of the summary move up into its place.
    fn make_binding_early(&mut self, d: u32, top: usize) {
        if !self.records_summaries {
            return;
        }
        let slot = self.decls[d as usize].summary_slot;
        if slot & (SUMMARY_SLOT_EARLY | SUMMARY_SLOT_INNER) != 0 {
            return;
        }
        let mut other = self.scopes[top].decl_head;
        while other != NONE {
            let declaration = &mut self.decls[other as usize];
            let other_slot = declaration.summary_slot;
            if other_slot & (SUMMARY_SLOT_EARLY | SUMMARY_SLOT_INNER) == 0 && other_slot > slot {
                declaration.summary_slot = other_slot - 1;
            }
            other = declaration.next_in_scope;
        }
        self.decls[d as usize].summary_slot = SUMMARY_SLOT_EARLY;
    }

    /// Resolves the bindings of the innermost scope against the uses in its subtree, and returns its row in the tree.
    pub(crate) fn close_scope(&mut self) -> u32 {
        if !self.tracks_scopes {
            self.scopes.pop();
            if let Some(parent) = self.scopes.last() {
                self.cur_scope = parent.id;
                self.cur_func = parent.func;
            }
            return NONE;
        }
        let top = self.scopes.len() - 1;
        let current = self.scopes[top];
        self.process_hoists(current, top);
        let scope = self.scopes.pop().unwrap();
        let mut row = NONE;
        let keep = self.retain;
        if keep && (scope.decl_head != NONE || scope.eval_in_function || scope.has_eval) {
            row = self.ast.scope_flags.len() as u32;
            self.ast.scope_first_binding.push(self.ast.binding_name.len() as u32);
            let mut flags = scope.flags as u32;
            if scope.eval_in_function {
                flags |= SCOPE_ROW_EVAL_IN_FUNCTION;
            }
            if scope.has_eval {
                flags |= SCOPE_ROW_POISONED;
            }
            self.ast.scope_flags.push(flags);
        }
        let parent_id = self.scopes.last().map_or(NONE, |parent| parent.id);
        let mut d = scope.decl_head;
        while d != NONE {
            // SAFETY: Declaration links stay within the table.
            let declaration = unsafe { *self.decls.get_unchecked(d as usize) };
            let name = declaration.name;
            let mut flags = declaration.flags;
            let state = self.names[name as usize];
            // A function expression's own name resolves through the environment, so its uses stay free.
            let name_only = flags & !(decl::FUNCTION_NAME) == 0;
            if name_only {
                self.record_uses_as_captures(name, state.use_head, scope.id, scope.func);
            }
            // A `var` only passes through the scopes between it and its function, which is where it binds.
            let passing_through = scope.flags & scope_flag::VAR_SCOPE == 0
                && flags & (decl::LEXICAL | decl::FUNCTION | decl::PARAMETER | decl::CLASS_NAME) == 0;
            if !name_only && !passing_through {
                let mut captured = scope.has_eval;
                let mut used_in_with = false;
                let mut head = state.use_head;
                while head != NONE {
                    // SAFETY: Use links stay within the table.
                    let row = unsafe { self.uses.get_unchecked_mut(head as usize) };
                    if row.scope < scope.id {
                        break;
                    }
                    captured |= row.func != scope.func;
                    used_in_with |= row.func & USE_IN_WITH != 0;
                    let func = row.func & !USE_IN_WITH;
                    let in_with = row.func & USE_IN_WITH;
                    row.name = USE_DEAD;
                    if func != scope.func {
                        // SAFETY: Every function id has a row here.
                        let index_row = unsafe { *self.function_rows.get_unchecked(func as usize) };
                        if index_row != NONE {
                            self.index.pending_captures.push((index_row, name | in_with));
                        }
                    }
                    head = row.prev;
                }
                let new_scope = if head == NONE {
                    NONE
                } else {
                    self.uses[head as usize].scope
                };
                if scope.flags & scope_flag::BODY_OF_PARAMETERS != 0
                    && new_scope != NONE
                    && new_scope >= parent_id
                    && flags & decl::VAR != 0
                {
                    flags |= decl::REFERENCED_IN_PARAMETERS;
                    self.record_uses_as_captures(name, head, parent_id, scope.func);
                }
                let state = &mut self.names[name as usize];
                state.use_head = head;
                state.use_scope = new_scope;
                if captured {
                    flags |= decl::CAPTURED;
                }
                if used_in_with {
                    flags |= decl::USED_IN_WITH;
                }
                if self.records_summaries {
                    let summary_slot = self.decls[d as usize].summary_slot;
                    self.note_closed_binding(summary_slot, name, captured || used_in_with, scope);
                }
            }
            self.names[name as usize].decl_head = declaration.next_same_name;
            if keep {
                // A function directly in a switch stays out of this: it gets its binding through the case it is in.
                if scope.flags & scope_flag::SWITCH == 0 {
                    self.decls[d as usize].out_binding = self.ast.binding_name.len() as u32;
                }
                self.ast.binding_name.push(name);
                self.ast.binding_flags.push(flags);
            }
            d = declaration.next_in_scope;
        }
        if scope.flags & scope_flag::WITH != 0 {
            for row in &mut self.uses[scope.use_mark as usize..] {
                if row.name != USE_DEAD {
                    row.func |= USE_IN_WITH;
                }
            }
        }
        if let Some(parent) = self.scopes.last_mut() {
            parent.has_eval |= scope.has_eval;
            if scope.flags & scope_flag::FUNCTION == 0 {
                parent.eval_in_function |= scope.eval_in_function;
                parent.eval_here |= scope.eval_here;
                parent.arguments_access |= scope.arguments_access;
                parent.nested_arguments_access |= scope.arguments_access;
            } else if scope.flags & scope_flag::ARROW != 0 && (scope.arguments_access || scope.eval_here) {
                // An arrow function has no arguments object of its own, so the function around it provides it, which a
                // direct eval in the arrow function can reach as well.
                parent.arguments_access = true;
                parent.nested_arguments_access = true;
            }
            self.cur_scope = parent.id;
            self.cur_func = parent.func;
        }
        row
    }

    /// Whether the name is declared so far in the current function, as far as code that reads `arguments` can tell.
    pub(crate) fn is_declared_in_current_function(&self, name: u32) -> bool {
        let mut d = self.names.get(name as usize).map_or(NONE, |state| state.decl_head);
        let mut function_scope = 0;
        let mut declaring_flags = decl::LEXICAL | decl::VAR | decl::PARAMETER | decl::HOISTABLE;
        for scope in self.scopes.iter().rev() {
            if scope.flags & (scope_flag::FUNCTION | scope_flag::STATIC_BLOCK) != 0 {
                function_scope = scope.id;
                if scope.flags & (scope_flag::FUNCTION | scope_flag::ARROW) == scope_flag::FUNCTION {
                    // NB: A `var` in a function with an arguments object of its own is the binding of that object.
                    declaring_flags = decl::LEXICAL | decl::FUNCTION | decl::PARAMETER | decl::HOISTABLE;
                }
                break;
            }
        }
        while d != NONE {
            let row = &self.decls[d as usize];
            if row.scope < function_scope {
                break;
            }
            if row.flags & declaring_flags != 0 {
                return true;
            }
            d = row.next_same_name;
        }
        false
    }

    /// Pops the uses of a name that a function binds implicitly, and returns whether there were any, and whether one
    /// of them came from a nested function.
    pub(crate) fn resolve_implicit(&mut self, name: u32, scope_id: u32, func: u32) -> (bool, bool) {
        let mut used = false;
        let mut captured = false;
        let mut head = self.names[name as usize].use_head;
        while head != NONE {
            let row = &mut self.uses[head as usize];
            if row.scope < scope_id {
                break;
            }
            used = true;
            captured |= row.func != func;
            let use_func = row.func & !USE_IN_WITH;
            let in_with = row.func & USE_IN_WITH;
            row.name = USE_DEAD;
            head = row.prev;
            if use_func != func {
                let index_row = self.function_rows[use_func as usize];
                if index_row != NONE {
                    self.index.pending_captures.push((index_row, name | in_with));
                }
            }
        }
        if used {
            let new_scope = if head == NONE {
                NONE
            } else {
                self.uses[head as usize].scope
            };
            let state = &mut self.names[name as usize];
            state.use_head = head;
            state.use_scope = new_scope;
        }
        (used, captured)
    }

    /// Whether a name has a declaration in the scope with the given id, and which.
    #[inline(always)]
    fn find_declaration(&self, name: u32, scope_id: u32) -> u32 {
        let mut d = self.names[name as usize].decl_head;
        while d != NONE {
            let row = &self.decls[d as usize];
            if row.scope <= scope_id {
                return if row.scope == scope_id { d } else { NONE };
            }
            d = row.next_same_name;
        }
        NONE
    }

    fn add_declaration(&mut self, name: u32, scope_index: usize, flags: u32) {
        if self.records_summaries {
            let word = name as usize / 64;
            if word >= self.index.declared_names.len() {
                self.index.declared_names.resize(word + 1, 0);
            }
            self.index.declared_names[word] |= 1 << (name % 64);
        }
        let scope_id = self.scopes[scope_index].id;
        let d = self.decls.len() as u32;
        // Keep the name's chain ordered from the innermost scope out.
        let mut previous = NONE;
        let mut next = self.names[name as usize].decl_head;
        while next != NONE && self.decls[next as usize].scope > scope_id {
            previous = next;
            next = self.decls[next as usize].next_same_name;
        }
        self.decls.push(DeclRow {
            name,
            scope: scope_id,
            flags,
            next_same_name: next,
            next_in_scope: self.scopes[scope_index].decl_head,
            last_var_scope: 0,
            out_binding: NONE,
            summary_slot: NONE,
        });
        self.scopes[scope_index].decl_head = d;
        if previous == NONE {
            self.names[name as usize].decl_head = d;
        } else {
            self.decls[previous as usize].next_same_name = d;
        }
    }

    fn treats_functions_as_var(&self, flags: u16) -> bool {
        flags & (scope_flag::FUNCTION | scope_flag::BODY_OF_PARAMETERS | scope_flag::STATIC_BLOCK) != 0
            || (!self.module && flags & scope_flag::TOP != 0)
    }

    pub(crate) fn treat_functions_as_var(&self) -> bool {
        self.treats_functions_as_var(self.scopes.last().unwrap().flags)
    }

    /// Whether a `var` of the name was written in the scope at `top` or a scope nested in it.
    fn contains_var_named(&self, name: u32, top: usize) -> bool {
        let var_scope = &self.scopes[self.scopes[top].var_scope as usize];
        let row = self.find_declaration(name, var_scope.id);
        row != NONE && self.decls[row as usize].last_var_scope >= self.scopes[top].id
    }

    /// The body of a catch clause may not lexically redeclare what the clause's parameter binds.
    fn conflicts_with_catch_parameter(&self, name: u32, flags: u16, top: usize) -> bool {
        if flags & scope_flag::CATCH_BODY == 0 {
            return false;
        }
        let parameter = self.find_declaration(name, self.scopes[top - 1].id);
        parameter != NONE && self.decls[parameter as usize].flags & decl::LEXICAL != 0
    }

    fn fail_redeclaration(&mut self, name: u32, pos: u32) {
        let name = String::from_utf8_lossy(self.interner.text(name)).into_owned();
        self.fail_with(pos, format!("Identifier '{name}' already declared"));
    }

    pub(crate) fn declare(&mut self, name: u32, bind: Bind, pos: u32) {
        if !self.tracks_scopes {
            return;
        }
        self.name_state(name).stamp |= DECLARED_BIT;
        let top = self.scopes.len() - 1;
        let flags = self.scopes[top].flags;
        let scope_id = self.scopes[top].id;
        let existing = self.find_declaration(name, scope_id);
        let existing_flags = if existing == NONE {
            0
        } else {
            self.decls[existing as usize].flags
        };
        let new_flags = match bind {
            Bind::None => return,
            Bind::Lexical | Bind::Let | Bind::Const => {
                let mut redeclared =
                    existing_flags & (decl::LEXICAL | decl::FUNCTION | decl::VAR | decl::PARAMETER) != 0;
                if flags & scope_flag::BODY_OF_PARAMETERS != 0 {
                    let parameters = self.find_declaration(name, self.scopes[top - 1].id);
                    redeclared |= parameters != NONE && self.decls[parameters as usize].flags & decl::PARAMETER != 0;
                }
                redeclared |= self.conflicts_with_catch_parameter(name, flags, top);
                redeclared |= self.contains_var_named(name, top);
                if redeclared {
                    self.fail_redeclaration(name, pos);
                    return;
                }
                decl::LEXICAL
                    | match bind {
                        Bind::Let => decl::DECLARED_LET,
                        Bind::Const => decl::DECLARED_CONST,
                        _ => 0,
                    }
            }
            Bind::SimpleCatch => decl::LEXICAL | decl::SIMPLE_CATCH,
            Bind::CatchPattern => {
                if existing_flags & decl::LEXICAL != 0 {
                    self.fail_redeclaration(name, pos);
                    return;
                }
                decl::LEXICAL | decl::CATCH_PATTERN
            }
            Bind::Function => {
                let mask = if self.treats_functions_as_var(flags) {
                    decl::LEXICAL
                } else {
                    decl::LEXICAL | decl::VAR
                };
                let var_inside = !self.treats_functions_as_var(flags) && self.contains_var_named(name, top);
                if existing_flags & mask != 0 || var_inside || self.conflicts_with_catch_parameter(name, flags, top) {
                    self.fail_redeclaration(name, pos);
                    return;
                }
                if self.treats_functions_as_var(flags) {
                    decl::FUNCTION | decl::VAR
                } else {
                    decl::FUNCTION | decl::HOISTABLE
                }
            }
            Bind::Parameter => {
                if existing_flags & decl::PARAMETER != 0 && self.duplicate_parameter == NONE {
                    self.duplicate_parameter = pos;
                }
                decl::PARAMETER
            }
            Bind::Var => {
                if self.with_depth != 0 {
                    // The declaration's initializer may assign to a property of the object instead.
                    self.note_use(name);
                }
                let var_index = self.scopes[top].var_scope as usize;
                let var_scope = self.scopes[var_index];
                // Only declarations of this very name can be in the way, and those are linked from the name.
                let mut d = self.names[name as usize].decl_head;
                let mut row = NONE;
                while d != NONE {
                    let declaration = self.decls[d as usize];
                    if declaration.scope < var_scope.id {
                        break;
                    }
                    if declaration.scope == var_scope.id {
                        row = d;
                    }
                    let lexical_conflict =
                        declaration.flags & decl::LEXICAL != 0 && declaration.flags & decl::SIMPLE_CATCH == 0;
                    let is_block_function = declaration.flags & decl::FUNCTION != 0
                        && (declaration.scope != var_scope.id || !self.treats_functions_as_var(var_scope.flags));
                    if lexical_conflict || is_block_function {
                        self.fail_redeclaration(name, pos);
                        return;
                    }
                    if declaration.flags & decl::SIMPLE_CATCH != 0 {
                        self.decls[d as usize].flags |= decl::VAR | decl::DECLARED_VAR;
                    }
                    d = declaration.next_same_name;
                }
                if row == NONE {
                    self.add_declaration(name, var_index, decl::VAR | decl::DECLARED_VAR);
                    self.note_new_binding(name, var_index, bind);
                    row = self.decls.len() as u32 - 1;
                } else {
                    self.note_redeclared_function_name(row, name, var_index, bind);
                    self.decls[row as usize].flags |= decl::VAR | decl::DECLARED_VAR;
                }
                self.decls[row as usize].last_var_scope = scope_id;
                return;
            }
        };
        if existing == NONE {
            self.add_declaration(name, top, new_flags);
            self.note_new_binding(name, top, bind);
        } else {
            self.note_redeclared_function_name(existing, name, top, bind);
            self.decls[existing as usize].flags |= new_flags;
        }
    }

    /// Annex B.3.3: a sloppy function in a block may also make a `var` in the enclosing function. That is only known
    /// once the scopes in between have closed, since a later `let` can rule it out.
    pub(crate) fn queue_annex_b_hoist(&mut self, name: u32) {
        let scope = self.scopes.last().unwrap();
        if scope.flags & scope_flag::VAR_SCOPE == 0 {
            let own_scope = scope.id;
            let declaration = self.find_declaration(name, own_scope);
            self.hoists.push(Hoist {
                name,
                declaration,
                own_scope,
            });
        }
    }

    pub(crate) fn declare_function_name(&mut self, name: u32) {
        self.name_state(name).stamp |= DECLARED_BIT;
        let top = self.scopes.len() - 1;
        self.add_declaration(name, top, decl::FUNCTION_NAME);
    }

    /// The inner binding of a class's name, in the scope the class opened.
    pub(crate) fn declare_class_name(&mut self, name: u32) {
        self.name_state(name).stamp |= DECLARED_BIT;
        let top = self.scopes.len() - 1;
        self.add_declaration(name, top, decl::CLASS_NAME | decl::LEXICAL);
    }

    pub(crate) fn has_declaration_at_top_of_scope(&self, name: u32) -> bool {
        (name as usize) < self.names.len() && self.find_declaration(name, self.cur_scope) != NONE
    }

    pub(crate) fn is_declared_at_top(&self, name: u32) -> bool {
        let id = self.scopes[0].id;
        (name as usize) < self.names.len() && self.find_declaration(name, id) != NONE
    }

    // === Functions ===

    /// Where a function of the first parse starts and ends, its name and its flags.
    pub(crate) fn function_row_summary(&self, row: u32) -> (u32, u32, u32, u32) {
        let index = self.first_pass_index();
        let row = row as usize;
        (index.start[row], index.end[row], index.name[row], index.flags[row])
    }

    #[inline(always)]
    pub(crate) fn open_index_row(&mut self, start: u32) -> u32 {
        let index = &mut self.index;
        let row = index.start.len() as u32;
        index.start.push(start);
        index.params_start.push(0);
        index.end.push(0);
        index.flags.push(0);
        index.length.push(0);
        index.parameter_count.push(0);
        index.ctx.push(0);
        index.name.push(NONE);
        index.capture_start.push(0);
        index.capture_count.push(0);
        index.summary_start.push(0);
        row
    }

    /// A new function id, for a function with a row in the index or for what only counts as one.
    pub(crate) fn new_function_id(&mut self, row: u32) -> u32 {
        let func = self.next_func;
        self.next_func += 1;
        self.function_rows.push(row);
        self.summary_rows.push(row);
        func
    }

    pub(crate) fn begin_function(&mut self, start: u32, new_ctx: u32, flags: u32) -> FunctionState {
        let state = self.save_function_state(start, new_ctx, flags);
        let func = self.new_function_id(state.row);
        self.open_scope_with(scope_flag::FUNCTION, func);
        state
    }

    fn save_function_state(&mut self, start: u32, new_ctx: u32, flags: u32) -> FunctionState {
        // NB: A function that is parsed again already has its row, which is for the caller to find.
        let row = if self.is_reparsing() {
            NONE
        } else {
            let row = self.open_index_row(start);
            self.index.ctx[row as usize] = new_ctx as u16;
            row
        };
        let state = FunctionState {
            marks: self.ast.marks(),
            row,
            ctx: self.ctx,
            strict: self.strict,
            labels_base: self.labels_base,
            yield_pos: self.yield_pos,
            await_pos: self.await_pos,
            await_ident_pos: self.await_ident_pos,
            cover_error: self.cover_error,
            retain: self.retain,
            duplicate_parameter: self.duplicate_parameter,
            start,
            flags,
            has_body_scope: false,
            body_start: 0,
            first_body_row: 0,
            uses_this: self.uses_this,
            uses_this_from_environment: self.uses_this_from_environment,
            might_need_arguments: self.might_need_arguments,
            in_class_of_sloppy_function: self.in_class_of_sloppy_function,
            is_arrow: self.in_arrow,
            summary_ordinal: self.summary_ordinal,
        };
        self.summary_ordinal = 0;
        self.uses_this = false;
        self.uses_this_from_environment = false;
        self.might_need_arguments = false;
        self.in_arrow = flags & function_flag::ARROW != 0;
        self.in_class_of_sloppy_function &= self.in_arrow;
        self.ctx = new_ctx;
        self.labels_base = self.labels.len() as u32;
        self.yield_pos = 0;
        self.await_pos = 0;
        self.await_ident_pos = 0;
        self.cover_error = NONE;
        self.duplicate_parameter = NONE;
        self.retain = self.mode == Mode::Full || core::mem::take(&mut self.retain_next_function);
        state
    }

    /// Turns the cover scope that the parenthesized expression before `=>` opened into the scope of the arrow function.
    pub(crate) fn begin_arrow_from_cover(
        &mut self,
        start: u32,
        marks: AstMarks,
        new_ctx: u32,
        flags: u32,
    ) -> FunctionState {
        let mut state = self.save_function_state(start, new_ctx, flags);
        let func = self.new_function_id(state.row);
        state.marks = marks;
        let outer = self.cur_func;
        let index = self.scopes.len() as u32 - 1;
        let scope = self.scopes.last_mut().unwrap();
        scope.flags = scope_flag::FUNCTION | scope_flag::ARROW;
        scope.func = func;
        scope.var_scope = index;
        let mark = scope.use_mark as usize;
        self.cur_func = func;
        // Uses in the parameters happen when the arrow function is called, not where it is written.
        let mut parameters_use_this = false;
        for row in &mut self.uses[mark..] {
            if row.func == outer {
                row.func = func;
            }
            parameters_use_this |= row.name == N_DOT_THIS || row.name == N_DOT_NEW_TARGET;
        }
        if parameters_use_this || self.scopes.last().unwrap().eval_here {
            self.uses_this = true;
            self.uses_this_from_environment = true;
        }
        state
    }

    pub(crate) fn open_function_body_scope(&mut self, state: &mut FunctionState) {
        state.has_body_scope = true;
        let eval_in_parameters = self.scopes.last().unwrap().has_eval;
        self.open_scope(scope_flag::BODY_OF_PARAMETERS);
        // Scope analysis treats the parameters and the body as one scope, so an eval in either reaches both.
        self.scopes.last_mut().unwrap().has_eval = eval_in_parameters;
    }

    /// Closes the scopes of a function that scopes are tracked for, and works out what they say about it: its flags, and
    /// the rows of its scope and of the scope of its body.
    fn close_scopes_of_function(
        &mut self,
        state: &FunctionState,
        name: u32,
        is_expression_body: bool,
    ) -> (u32, u32, u32) {
        let mut flags = state.flags;
        let mut body_scope_row = NONE;
        let mut name_is_alone_in_its_scopes = true;
        if state.has_body_scope {
            let body_scope = self.scopes.last().unwrap();
            flags |= if body_scope.arguments_access {
                function_flag::ARGUMENTS_ACCESS
            } else {
                0
            };
            flags |= if body_scope.eval_here {
                function_flag::DIRECT_EVAL
            } else {
                0
            };
            if flags & function_flag::NAMED_EXPRESSION != 0 {
                let current = *body_scope;
                let top = self.scopes.len() - 1;
                self.process_hoists(current, top);
                name_is_alone_in_its_scopes = self.find_declaration(name, current.id) == NONE;
            }
            body_scope_row = self.close_scope();
        }
        let scope = *self.scopes.last().unwrap();
        if scope.arguments_access {
            flags |= function_flag::ARGUMENTS_ACCESS;
        }
        // Whether anything in the function could refer to its own name.
        let name_used = self.names.get(name as usize).is_some_and(|name_state| {
            name_state.use_head != NONE && self.uses[name_state.use_head as usize].scope >= scope.id
        });
        let name_declaration = if flags & function_flag::NAMED_EXPRESSION != 0 {
            self.find_declaration(name, scope.id)
        } else {
            NONE
        };
        let scope_row = self.close_scope();
        if self.strict {
            flags |= function_flag::STRICT;
        }
        if scope.has_eval {
            flags |= function_flag::CONTAINS_EVAL;
        }
        if scope.eval_here {
            flags |= function_flag::DIRECT_EVAL;
        }
        if !name_used
            && !scope.has_eval
            && name_is_alone_in_its_scopes
            && name_declaration != NONE
            && self.decls[name_declaration as usize].flags & !decl::FUNCTION_NAME == 0
        {
            flags |= function_flag::NAME_UNUSED;
        }
        if state.flags & function_flag::ARROW == 0 {
            let (used, captured) = self.resolve_implicit(N_DOT_THIS, scope.id, scope.func);
            if used {
                flags |= function_flag::USES_THIS;
            }
            if captured {
                flags |= function_flag::CAPTURES_THIS;
            }
            let (used, captured) = self.resolve_implicit(N_ARGUMENTS, scope.id, scope.func);
            if used {
                flags |= function_flag::USES_ARGUMENTS;
            }
            if captured {
                flags |= function_flag::CAPTURES_ARGUMENTS;
            }
            self.resolve_implicit(N_DOT_NEW_TARGET, scope.id, scope.func);
            self.resolve_implicit(N_DOT_SUPER, scope.id, scope.func);
        }
        self.collapse_uses(scope.use_mark, scope.func);
        let is_arrow = state.flags & function_flag::ARROW != 0;
        if self.uses_this {
            flags |= function_flag::SCOPE_USES_THIS;
        }
        if self.uses_this && !(is_arrow && is_expression_body) {
            flags |= function_flag::INSIGHT_USES_THIS;
        }
        if self.uses_this_from_environment {
            flags |= function_flag::INSIGHT_THIS_FROM_ENVIRONMENT | function_flag::SCOPE_THIS_FROM_ENVIRONMENT;
        }
        if self.might_need_arguments && !is_arrow {
            flags |= function_flag::MIGHT_NEED_ARGUMENTS;
        }
        if self.might_need_arguments {
            flags |= function_flag::MENTIONS_ARGUMENTS;
        }
        (flags, scope_row, body_scope_row)
    }

    /// Closes the function: resolves its scopes, collapses its free names, fills in its index row unless it has one
    /// from the first parse, and drops its tree unless it is retained. Returns the function's node.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn end_function(
        &mut self,
        state: FunctionState,
        end: u32,
        parameter_count: u32,
        length: u32,
        params_start: u32,
        parameters: u32,
        body: u32,
        name: u32,
        is_expression_body: bool,
    ) -> u32 {
        let retained = self.retain;
        let is_arrow = state.flags & function_flag::ARROW != 0;
        let (mut flags, scope_row, body_scope_row) = if self.tracks_scopes {
            self.close_scopes_of_function(&state, name, is_expression_body)
        } else {
            if state.has_body_scope {
                self.close_scope();
            }
            self.close_scope();
            (state.flags, NONE, NONE)
        };
        self.summary_ordinal = state.summary_ordinal;
        if !self.is_reparsing() {
            if self.tok_end as usize + super::SOURCE_PADDING > self.len as usize {
                flags |= function_flag::REACHES_SOURCE_END;
            }
            let row = state.row as usize;
            let index = &mut self.index;
            index.params_start[row] = params_start;
            index.end[row] = end;
            index.flags[row] = flags;
            index.length[row] = length;
            index.parameter_count[row] = parameter_count;
            index.name[row] = name;
        }

        self.ctx = state.ctx;
        self.strict = state.strict;
        self.labels_base = state.labels_base;
        self.yield_pos = state.yield_pos;
        self.await_pos = state.await_pos;
        self.await_ident_pos = state.await_ident_pos;
        self.cover_error = state.cover_error;
        self.retain = state.retain;
        self.duplicate_parameter = state.duplicate_parameter;
        self.uses_this |= state.uses_this;
        self.uses_this_from_environment |= state.uses_this_from_environment;
        // An arrow function has no arguments object, so that what it mentions counts for the function around it.
        self.might_need_arguments = state.might_need_arguments || (is_arrow && self.might_need_arguments);
        self.in_class_of_sloppy_function = state.in_class_of_sloppy_function;
        self.in_arrow = state.is_arrow;

        if retained {
            let extra = self.ast.extra.len() as u32;
            self.ast.extra.extend_from_slice(&[
                parameters,
                body,
                name,
                scope_row,
                body_scope_row,
                state.body_start,
                is_expression_body as u32,
                state.first_body_row,
            ]);
            self.ast.push(tag::FUNCTION, state.start, extra, state.row)
        } else {
            self.ast.truncate(&state.marks);
            // Node indices get reused from here on.
            if self.last_arrow >= state.marks.nodes {
                self.last_arrow = 0;
            }
            self.ast.push(tag::FUNCTION, state.start, 0, state.row)
        }
    }

    // === Reparsing ===

    #[inline(always)]
    pub(crate) fn is_reparsing(&self) -> bool {
        !self.first_pass_index.is_null()
    }

    fn first_pass_index(&self) -> &'a FunctionIndex {
        // SAFETY: A parser that reparses functions has an index, which for_reparsing() requires to stay valid.
        unsafe { &*self.first_pass_index }
    }

    /// Reads from `source` from now on, where offset `offset` is at the first unit, and `length` is that of the whole
    /// source.
    ///
    /// # Safety
    /// What the lexer may read of the source, up to `length` and the padding after it if it gets that far, must stay
    /// valid for as long as the parser reads it.
    pub(crate) unsafe fn set_reparse_source(&mut self, source: *const U, offset: u32, length: u32) {
        self.src = source.wrapping_sub(offset as usize);
        self.len = length;
    }

    /// Gets ready for the next function, whether this one went well or not.
    pub(crate) fn finish_reparse(&mut self) {
        for row in &self.uses {
            if row.name != USE_DEAD {
                let state = &mut self.names[row.name as usize];
                state.use_head = NONE;
                state.use_scope = NONE;
            }
        }
        self.uses.clear();
        for row in &self.decls {
            self.names[row.name as usize].decl_head = NONE;
        }
        self.decls.clear();
        self.scopes.clear();
        self.hoists.clear();
        self.labels.clear();
        self.labels_base = 0;
        self.label_indices.clear();
        self.scratch.clear();
        self.class_marks.clear();
        self.private_declared.clear();
        self.private_used.clear();
        self.names_used_in_with.clear();
        self.function_rows.truncate(1);
        self.summary_rows.truncate(1);
        self.next_func = 1;
        self.next_scope = 1;
        self.cur_scope = 0;
        self.cur_func = 0;
        self.with_depth = 0;
        self.recursion_depth = 0;
        self.reparse_target = NONE;
        self.err = None;
    }

    /// Parses one function again, on its own, from where the first parse found it. The functions nested in it get
    /// skipped by what the first parse recorded about them, so that nothing is parsed further than one level down.
    /// Returns the function's node, or the element of its class if the function is the code of a class.
    pub(crate) fn reparse_function(&mut self, row: u32) -> u32 {
        use function_flag::*;
        let index = self.first_pass_index();
        let row_index = row as usize;
        let (start, params_start) = (index.start[row_index], index.params_start[row_index]);
        let (flags, name) = (index.flags[row_index], index.name[row_index]);
        self.reparse_target = params_start;
        self.row_cursor.set(index.position_after_row(row_index));
        self.strict = flags & STRICT != 0;
        self.ctx = index.ctx[row_index] as u32;
        self.retain = flags & (FIELD_INITIALIZER | STATIC_BLOCK) != 0;
        self.retain_next_function = !self.retain;
        self.uses_this = false;
        self.uses_this_from_environment = false;
        self.might_need_arguments = false;
        self.in_arrow = false;
        self.cover_error = NONE;
        self.potential_arrow_at = NONE;
        self.last_arrow = 0;
        if flags & DEFAULT_CONSTRUCTOR != 0 {
            return self.default_constructor_class_node(row);
        }
        self.open_scope_with(0, 0);
        // The class around the function, if any, already checked its private names.
        self.class_marks.push((0, 0));
        self.private_declared.push(std::collections::HashMap::new());
        self.pos = params_start;
        self.tok_end = params_start;
        self.prev_end = params_start;
        self.prev_start = params_start;
        self.lex();
        if flags & (FIELD_INITIALIZER | STATIC_BLOCK) != 0 {
            self.class_start = start;
            return self.reparse_class_element();
        }
        let new_ctx = self.ctx;
        let own_flags = flags & (ARROW | ASYNC | GENERATOR | METHOD | NAMED_EXPRESSION);
        let node = if flags & ARROW != 0 {
            self.reparse_arrow_function(start, params_start, flags)
        } else {
            let mut state = self.begin_function(start, new_ctx, own_flags);
            let own_name = (name != NONE).then_some((name, start));
            if flags & NAMED_EXPRESSION != 0 {
                self.declare_function_name(name);
            }
            if name == N_ARGUMENTS || name == N_EVAL {
                self.might_need_arguments = true;
            }
            self.parse_function_tail(&mut state, own_name, flags & METHOD != 0)
        };
        self.ast.b[node as usize] = row;
        node
    }

    /// The row of a function about to be parsed, from where it starts over, if that can be skipped.
    #[inline(always)]
    pub(crate) fn skippable_row(&self, params_start: u32) -> Option<u32> {
        if !self.is_reparsing() || params_start == self.reparse_target {
            return None;
        }
        self.first_pass_row_at(params_start)
    }

    /// The row of an arrow function, whose parameters can be mistaken for code.
    #[inline(always)]
    pub(crate) fn skippable_arrow_row(&self, params_start: u32) -> Option<u32> {
        if !self.is_reparsing() {
            return None;
        }
        self.skippable_row(params_start)
            .filter(|&row| self.first_pass_index().flags[row as usize] & function_flag::ARROW != 0)
    }

    pub(crate) fn first_pass_row_at(&self, params_start: u32) -> Option<u32> {
        self.first_pass_index()
            .row_at_params_start(params_start, &self.row_cursor)
    }

    /// Goes past a nested function as if it had been parsed, from where it starts over: what it uses of the scopes
    /// around it comes from the first parse. Returns the function's node.
    pub(crate) fn skip_function(&mut self, mut state: FunctionState, row: u32) -> u32 {
        use function_flag::*;
        let index = self.first_pass_index();
        let index_row = row as usize;
        let flags = index.flags[index_row];
        let captures = index.capture_start[index_row] as usize;
        if !self.tracks_scopes {
            return self.skip_function_without_scopes(state, row);
        }
        let capture_count = index.capture_count[index_row] as usize;
        for i in captures..captures + capture_count {
            let capture = index.capture_names[i];
            let name = capture & !USE_IN_WITH;
            self.note_use(name);
            if capture & USE_IN_WITH != 0 {
                let head = self.names[name as usize].use_head;
                self.uses[head as usize].func |= USE_IN_WITH;
            }
        }
        self.uses_this |= flags & SCOPE_USES_THIS != 0;
        self.uses_this_from_environment |= flags & SCOPE_THIS_FROM_ENVIRONMENT != 0;
        self.might_need_arguments |= flags & MENTIONS_ARGUMENTS != 0;
        let scope = self.scopes.last_mut().unwrap();
        scope.has_eval |= flags & CONTAINS_EVAL != 0;
        scope.eval_here |= flags & DIRECT_EVAL != 0;
        scope.arguments_access |= flags & ARGUMENTS_ACCESS != 0;
        state.row = row;
        self.retain = false;
        let end = index.end[index_row];
        self.tok_start = end - 1;
        self.tok_end = end;
        self.pos = end;
        self.next();
        self.end_function(state, end, 0, 0, 0, 0, 0, index.name[index_row], false)
    }

    /// Goes past a nested function when nothing keeps track of scopes, so that only the state of the parser matters.
    fn skip_function_without_scopes(&mut self, state: FunctionState, row: u32) -> u32 {
        let end = self.first_pass_index().end[row as usize];
        self.scopes.pop();
        if let Some(parent) = self.scopes.last() {
            self.cur_scope = parent.id;
            self.cur_func = parent.func;
        }
        self.retain = false;
        self.tok_start = end - 1;
        self.tok_end = end;
        self.pos = end;
        self.next();
        self.ctx = state.ctx;
        self.strict = state.strict;
        self.labels_base = state.labels_base;
        self.yield_pos = state.yield_pos;
        self.await_pos = state.await_pos;
        self.await_ident_pos = state.await_ident_pos;
        self.cover_error = state.cover_error;
        self.retain = state.retain;
        self.duplicate_parameter = state.duplicate_parameter;
        self.uses_this = state.uses_this;
        self.uses_this_from_environment = state.uses_this_from_environment;
        self.might_need_arguments = state.might_need_arguments;
        self.in_arrow = state.is_arrow;
        self.ast.push(tag::FUNCTION, state.start, 0, row)
    }

    /// Code of a class that runs as a function captures what the functions in it capture. Their uses stay, since each
    /// of those functions has to capture the name as well.
    pub(crate) fn note_uses_of_nested_functions(&mut self, mark: u32, func: u32) {
        self.stamp += 1;
        let stamp = self.stamp;
        let mut nested_names = std::mem::take(&mut self.free_names_scratch);
        nested_names.clear();
        for row in &self.uses[mark as usize..] {
            if row.name == USE_DEAD || row.func & !USE_IN_WITH == func {
                continue;
            }
            let state = &mut self.names[row.name as usize];
            if state.stamp & !DECLARED_BIT != stamp {
                state.stamp = stamp | (state.stamp & DECLARED_BIT);
                nested_names.push(row.name);
            }
            if row.func & USE_IN_WITH != 0 {
                self.names_used_in_with.push(row.name);
            }
        }
        let scope = self.cur_scope;
        for &name in &nested_names {
            let next = self.uses.len() as u32;
            let state = &mut self.names[name as usize];
            let prev = state.use_head;
            state.use_head = next;
            state.use_scope = scope;
            self.uses.push(UseRow {
                name,
                scope,
                func,
                prev,
            });
        }
        self.free_names_scratch = nested_names;
        while let Some(name) = self.names_used_in_with.pop() {
            let head = self.names[name as usize].use_head;
            self.uses[head as usize].func |= USE_IN_WITH;
        }
    }

    /// Replaces every use above `mark` by one row per name, as seen from the enclosing scope.
    fn collapse_uses(&mut self, mark: u32, func: u32) {
        self.stamp += 1;
        let stamp = self.stamp;
        let mut free_names = std::mem::take(&mut self.free_names_scratch);
        free_names.clear();
        let mut i = self.uses.len();
        while i > mark as usize {
            i -= 1;
            // SAFETY: The index is within the table.
            let row = unsafe { *self.uses.get_unchecked(i) };
            if row.name == USE_DEAD {
                continue;
            }
            // SAFETY: A live use's name has a state row.
            let state = unsafe { self.names.get_unchecked_mut(row.name as usize) };
            state.use_head = row.prev;
            if state.stamp & !DECLARED_BIT != stamp {
                state.stamp = stamp | (state.stamp & DECLARED_BIT);
                free_names.push(row.name);
            }
            if row.func & USE_IN_WITH != 0 {
                self.names_used_in_with.push(row.name);
            }
        }
        self.uses.truncate(mark as usize);
        let scope = self.cur_scope;
        for &name in &free_names {
            let next = self.uses.len() as u32;
            let state = &mut self.names[name as usize];
            let prev = state.use_head;
            state.use_head = next;
            state.use_scope = scope;
            self.uses.push(UseRow {
                name,
                scope,
                func,
                prev,
            });
        }
        self.free_names_scratch = free_names;
        while let Some(name) = self.names_used_in_with.pop() {
            let head = self.names[name as usize].use_head;
            self.uses[head as usize].func |= USE_IN_WITH;
        }
    }

    /// Every recursive path of the parser goes through this, and through leave() on the way back. The limit on the
    /// depth also protects what walks the tree later, which recursion here reaches as deep as it reached here.
    #[inline(always)]
    pub(crate) fn enter(&mut self) -> bool {
        let marker = 0u8;
        if self.recursion_depth >= MAX_RECURSION_DEPTH || (&raw const marker as usize) < self.stack_limit {
            self.fail(self.tok_start, RECURSION_LIMIT_MESSAGE);
            return false;
        }
        self.recursion_depth += 1;
        true
    }

    #[inline(always)]
    pub(crate) fn leave(&mut self) {
        self.recursion_depth -= 1;
    }
}

const DECLARED_BIT: u32 = 1 << 31;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Bind {
    None,
    Var,
    Let,
    Const,
    /// A lexical binding with no declaration keyword of its own, such as a class or an import.
    Lexical,
    Function,
    SimpleCatch,
    CatchPattern,
    Parameter,
}
