/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The JavaScript parser. Lexing is driven by the parser, the syntax tree is a set of flat columns, and scope analysis
//! happens while parsing, with no map per scope. [`crate::ast`] is how everything after the parser reads the tree.
//!
//! A source is parsed once for all of its early errors. That leaves a [`ScriptData`], which is all that is kept of the
//! source, and the tree of its top-level code. A function is parsed again on its own when it is first called. Its tree
//! lives as long as the compile job that needs it, and nested functions are skipped by what the first parse recorded.

pub(crate) mod base;
pub(crate) mod columns;
pub(crate) mod expr;
mod intern;
mod lexer;
pub(crate) mod literals;
pub(crate) mod resolve;
mod stmt;
pub(crate) mod token;
pub(crate) mod unit;

use crate::ast::Name;
use crate::ast::NameTable;
use crate::ast::Position;
use crate::ast::ProgramType;
use crate::ast::Resolution;
use crate::ast::ScopeTable;
use crate::ast::SyntaxTree;
use crate::u32_from_usize;
use base::Mode;
use base::Parser;
use columns::Ast;
use columns::FunctionIndex;
use columns::function_flag;
use core::sync::atomic::AtomicBool;
use core::sync::atomic::AtomicUsize;
use core::sync::atomic::Ordering;
use intern::Interner;
pub(crate) use resolve::EnclosingCode;
pub(crate) use resolve::OuterResolutions;
use std::cell::Cell;
use std::sync::Arc;
use std::sync::Mutex;
use unit::Unit;

/// How many zero code units the lexer needs after the source.
const SOURCE_PADDING: usize = 64;

/// A function of a source, as the row that the first parse of the source gave it.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct FunctionRow(pub(crate) u32);

/// A parser that keeps reparsing the functions of one source: it knows the names of the source.
enum ReparseContext {
    Narrow(Box<Parser<'static, u8>>),
    Wide(Box<Parser<'static, u16>>),
}

// SAFETY: The parser of a context only points at what the ScriptData holding the context owns, or, during a reparse, into
//         the memory of that reparse.
unsafe impl Send for ReparseContext {}

/// All that is kept of a source once it is parsed: the source, one row for each of its functions, and the names that the
/// rows speak of. Compile jobs on any thread parse the functions again from this.
pub struct ScriptData {
    pub(crate) index: FunctionIndex,
    pub(crate) names: NameTable,
    pub(crate) positions: Arc<SourcePositions>,
    pub(crate) program_type: ProgramType,
    pub(crate) program_context: base::ProgramContext,
    /// The rows of the top level with marks, or that they did not fit the stack, until the compile of the top level.
    top_level_rows: Mutex<Option<TopLevelRows>>,
    pub(crate) default_export_name: Name,
    pub(crate) default_constructor_parameter_name: Name,
    /// The undeclared names that something in a `with` uses, which are not simply globals, sorted.
    global_names_used_in_with: Vec<u32>,
    /// Filled in when the first function gets parsed again, for the ones after it.
    reparse_context: Mutex<Option<ReparseContext>>,
    /// Whether the code that gets compiled for the functions of this source is worth keeping, because the program may
    /// get linked again.
    retains_compiled_functions: AtomicBool,
    /// The bytes of the code kept so far, for [`ScriptData::memory_size()`].
    retained_code_size: AtomicUsize,
}

/// The memory of the tree of a function that a compile job is done with, for the next one on the thread.
struct SpareTreeMemory {
    ast: Ast,
    resolutions: Vec<Resolution>,
    scopes: ScopeTable,
    function_scope_rows: Vec<(u32, u32)>,
}

/// The most nodes of a tree whose memory stays around.
const MAXIMUM_SPARE_NODES: usize = 1 << 16;

thread_local! {
    static SPARE_TREE_MEMORY: Cell<Option<SpareTreeMemory>> = const { Cell::new(None) };
}

impl SyntaxTree {
    /// Leaves the memory of this tree, which is one of a function, to the next one parsed on the thread.
    pub(crate) fn give_back_memory(self) {
        if self.ast.tag.capacity() > MAXIMUM_SPARE_NODES {
            return;
        }
        let Self {
            mut ast,
            mut resolutions,
            mut scopes,
            mut function_scope_rows,
            ..
        } = self;
        ast.reset();
        resolutions.clear();
        scopes.clear();
        function_scope_rows.clear();
        SPARE_TREE_MEMORY.set(Some(SpareTreeMemory {
            ast,
            resolutions,
            scopes,
            function_scope_rows,
        }));
    }
}

/// A function could not be parsed or compiled, because the stack that is left does not hold it.
#[derive(Clone, Copy, Debug)]
pub struct OutOfStack;

/// The address that the stack can only go `bytes` below where it is now.
pub fn stack_limit_for_budget(bytes: usize) -> usize {
    ak::stack::limit_for_budget(bytes)
}

/// How low a VM lets the stack of this thread go, or `None` if that cannot be told.
pub(crate) fn vm_stack_limit_of_this_thread() -> Option<usize> {
    ak::stack::lowest_address_of_this_thread()
        .map(|lowest_address| lowest_address.saturating_add(crate::build_configuration::VM_STACK_SPACE_LIMIT as usize))
}

impl ScriptData {
    pub(crate) fn retains_compiled_functions(&self) -> bool {
        self.retains_compiled_functions.load(Ordering::Relaxed)
    }

    pub(crate) fn add_retained_code_size(&self, size: usize) {
        self.retained_code_size.fetch_add(size, Ordering::Relaxed);
    }

    pub(crate) fn set_retains_compiled_functions(&self, retains_compiled_functions: bool) {
        self.retains_compiled_functions
            .store(retains_compiled_functions, Ordering::Relaxed);
    }

    /// The bytes of this source and of the tables that compile jobs read, and of the code kept for its functions so far.
    pub(crate) fn memory_size(&self) -> usize {
        size_of::<Self>()
            + crate::compile::utf16_string_memory_size(&self.positions.source)
            + self.index.memory_size()
            + self.names.memory_size()
            + self.positions.lines.memory_size()
            + self.global_names_used_in_with.capacity() * size_of::<u32>()
            + self.retained_code_size.load(Ordering::Relaxed)
    }

    /// The rows of the top level for a sweep, which the parse of the source left here for its one compile.
    pub(crate) fn take_top_level_rows(self: &Arc<Self>) -> Result<(SyntaxTree, u32), OutOfStack> {
        let rows = self.top_level_rows.lock().expect("nothing panics with the rows").take();
        let (ast, node) = rows.expect("a top level compiles once")?;
        let tree = SyntaxTree {
            script: self.clone(),
            ast,
            root: node,
            first_token_start: 0,
            is_strict_mode: false,
            has_top_level_await: false,
            eval_referenced_private_names: Vec::new(),
            resolutions: Vec::new(),
            scopes: ScopeTable::default(),
            program_scope_row: 0,
            function_scope_rows: Vec::new(),
            outer_resolutions: None,
        };
        Ok((tree, node))
    }

    /// Parses the function at `row` again into a tree for a sweep, which goes by the summary of the first parse: nothing
    /// keeps track of scopes.
    pub(crate) fn reparse_for_sweep(
        self: &Arc<Self>,
        row: FunctionRow,
        stack_limit: usize,
    ) -> Result<(SyntaxTree, u32), OutOfStack> {
        let memory = SPARE_TREE_MEMORY.take().unwrap_or_else(|| SpareTreeMemory {
            ast: Ast::with_capacity(256),
            resolutions: Vec::new(),
            scopes: ScopeTable::default(),
            function_scope_rows: Vec::new(),
        });
        let (ast, node) = {
            let mut context = self.reparse_context.lock().expect("a reparse does not panic");
            let context = context.get_or_insert_with(|| self.make_reparse_context());
            match (context, self.positions.source.as_units()) {
                (
                    ReparseContext::Narrow(parser),
                    ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units),
                ) => {
                    parser.tracks_scopes = false;
                    parser.marks = true;
                    let result = self.reparse_code_units(parser, memory.ast, units, row, stack_limit);
                    parser.tracks_scopes = true;
                    parser.marks = false;
                    result
                }
                (ReparseContext::Wide(parser), ak::Utf16StringUnits::Utf16(units)) => {
                    parser.tracks_scopes = false;
                    parser.marks = true;
                    let result = self.reparse_code_units(parser, memory.ast, units, row, stack_limit);
                    parser.tracks_scopes = true;
                    parser.marks = false;
                    result
                }
                _ => unreachable!("the storage of a source does not change"),
            }
        }?;
        let tree = SyntaxTree {
            script: self.clone(),
            ast,
            root: node,
            first_token_start: 0,
            is_strict_mode: false,
            has_top_level_await: false,
            eval_referenced_private_names: Vec::new(),
            resolutions: memory.resolutions,
            scopes: memory.scopes,
            program_scope_row: 0,
            function_scope_rows: memory.function_scope_rows,
            outer_resolutions: None,
        };
        Ok((tree, node))
    }

    fn make_reparse_context(&self) -> ReparseContext {
        let module = self.program_type == ProgramType::Module;
        let length = u32_from_usize(self.positions.source.length_in_code_units());
        let interner = Interner::with_names(self.names.parser_names());
        // SAFETY: The context lives in this ScriptData, whose index stays where it is, since it is shared by an Arc,
        //         and never changes.
        unsafe {
            match self.positions.source.as_units() {
                ak::Utf16StringUnits::Ascii(_) | ak::Utf16StringUnits::Latin1(_) => {
                    ReparseContext::Narrow(Box::new(Parser::for_reparsing(module, interner, &self.index, length)))
                }
                ak::Utf16StringUnits::Utf16(_) => {
                    ReparseContext::Wide(Box::new(Parser::for_reparsing(module, interner, &self.index, length)))
                }
            }
        }
    }

    /// Bytes after a function are real source, so that it can be read in place. What the lexer sees of a function that
    /// comes too close to the end of the source for that gets copied, with the padding that the end of the source has
    /// to have.
    fn reparse_code_units<U: Unit + Default>(
        &self,
        parser: &mut Parser<'static, U>,
        ast: Ast,
        source: &[U],
        row: FunctionRow,
        stack_limit: usize,
    ) -> Result<(Ast, u32), OutOfStack> {
        let row_index = row.0 as usize;
        let needs_padding = self.index.flags[row_index] & function_flag::REACHES_SOURCE_END != 0;
        let mut tail = Vec::new();
        // SAFETY: The source outlives the parse, and the first parse found out that the lexer stays clear of its end
        //         unless the tail is what it reads.
        unsafe {
            if needs_padding {
                let first_read = self.index.params_start[row_index] as usize;
                tail.reserve_exact(source.len() - first_read + SOURCE_PADDING);
                tail.extend_from_slice(&source[first_read..]);
                tail.resize(tail.capacity(), U::default());
                parser.set_reparse_source(tail.as_ptr(), u32_from_usize(first_read), u32_from_usize(source.len()));
            } else {
                parser.set_reparse_source(source.as_ptr(), 0, u32_from_usize(source.len()));
            }
        }
        parser.stack_limit = stack_limit;
        parser.ast = ast;
        let node = parser.reparse_function(row.0);
        let error = parser.err.take();
        let ast = std::mem::replace(&mut parser.ast, Ast::unallocated());
        parser.finish_reparse();
        match error {
            None => Ok((ast, node)),
            Some(error) => {
                assert_eq!(
                    error.message,
                    base::RECURSION_LIMIT_MESSAGE,
                    "the first parse found no errors"
                );
                Err(OutOfStack)
            }
        }
    }

    pub(crate) fn is_global_name_used_in_with(&self, name: Name) -> bool {
        self.global_names_used_in_with.binary_search(&name.0).is_ok()
    }
}

pub struct ParseError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

/// What code needs of its source to report positions, which it keeps for as long as it lives. That can be long after the
/// rest of [`ScriptData`] is of no use to anything.
pub struct SourcePositions {
    pub(crate) source: ak::Utf16String,
    pub(crate) lines: LineTable,
}

/// Maps source offsets to lines and columns. Lines count from the line number of the first line, and columns count
/// code points.
pub(crate) struct LineTable {
    line_starts: Vec<u32>,
    /// The offsets of the second halves of surrogate pairs.
    low_surrogates: Vec<u32>,
    source_length: u32,
    /// Whether the source ends with a line terminator, which only counts as one if something follows it.
    ends_with_lone_line_terminator: bool,
    initial_line_number: u32,
    /// The index of the line that the last lookup hit, since lookups tend to come in source order.
    last_line: AtomicUsize,
}

impl LineTable {
    fn memory_size(&self) -> usize {
        (self.line_starts.capacity() + self.low_surrogates.capacity()) * size_of::<u32>()
    }

    fn new<U: Unit>(source: &[U], initial_line_number: u32) -> Self {
        let mut line_starts = vec![0];
        let mut low_surrogates = Vec::new();
        let mut ends_with_lone_line_terminator = false;
        let mut index = 0;
        while index < source.len() {
            let code_unit = source[index].code();
            index += 1;
            ends_with_lone_line_terminator = false;
            if code_unit == 0x0D && source.get(index).is_some_and(|next| next.code() == 0x0A) {
                index += 1;
                line_starts.push(u32_from_usize(index));
            } else if matches!(code_unit, 0x0A | 0x0D | 0x2028 | 0x2029) {
                line_starts.push(u32_from_usize(index));
                ends_with_lone_line_terminator = true;
            } else if U::WIDE
                && (0xD800..0xDC00).contains(&code_unit)
                && source
                    .get(index)
                    .is_some_and(|low| (0xDC00..0xE000).contains(&low.code()))
            {
                low_surrogates.push(u32_from_usize(index));
                index += 1;
            }
        }
        Self {
            line_starts,
            low_surrogates,
            source_length: u32_from_usize(source.len()),
            ends_with_lone_line_terminator,
            initial_line_number,
            last_line: AtomicUsize::new(0),
        }
    }

    fn line_of(&self, offset: u32) -> usize {
        let last_line = self.last_line.load(Ordering::Relaxed);
        let is_on_last_line = self.line_starts[last_line] <= offset
            && self.line_starts.get(last_line + 1).is_none_or(|&next| offset < next);
        let line = if is_on_last_line {
            last_line
        } else {
            self.line_starts.partition_point(|&start| start <= offset) - 1
        };
        self.last_line.store(line, Ordering::Relaxed);
        line
    }

    fn column_of(&self, line: usize, offset: u32) -> u32 {
        let line_start = self.line_starts[line];
        if self.low_surrogates.is_empty() {
            return offset - line_start + 1;
        }
        let low_surrogates = self.low_surrogates.partition_point(|&low| low < offset)
            - self.low_surrogates.partition_point(|&low| low < line_start);
        offset - line_start + 1 - u32_from_usize(low_surrogates)
    }

    pub(crate) fn position(&self, offset: u32) -> Position {
        let at_end = offset == self.source_length && self.source_length != 0;
        let (line, column) = if at_end && self.ends_with_lone_line_terminator {
            // NB: The end of the source is on the line of a line terminator that ends it.
            let line = self.line_starts.len() - 2;
            (line, self.column_of(line, offset - 1) + 1)
        } else if at_end && self.line_starts.last() == Some(&offset) {
            // NB: A CRLF that ends the source starts a line before its LF.
            (self.line_starts.len() - 1, 2)
        } else {
            let line = self.line_of(offset);
            (line, self.column_of(line, offset))
        };
        Position {
            line: self.initial_line_number + u32_from_usize(line),
            column,
            offset,
        }
    }
}

/// What the code being parsed may do, which the code around it decides.
#[derive(Clone, Copy)]
pub struct ParseOptions {
    pub program_type: ProgramType,
    pub initial_line_number: u32,
    pub starts_in_strict_mode: bool,
    pub initiated_by_eval: bool,
    pub in_eval_function_context: bool,
    pub allow_super_property_lookup: bool,
    pub allow_super_constructor_call: bool,
    pub in_class_field_initializer: bool,
    /// The code is that of a function body of this kind.
    pub function_body: Option<crate::ast::FunctionKind>,
    /// No identifier may resolve to a global binding.
    pub suppress_globals: bool,
    /// Keep the tree of everything, for a dump of it, rather than just of what runs first.
    pub keep_whole_tree: bool,
}

impl ParseOptions {
    /// What parsing the top level again has to know of how it got parsed.
    fn program_context(&self) -> base::ProgramContext {
        use base::ctx;
        let mut context = 0;
        if self.in_eval_function_context {
            context |= ctx::NEW_TARGET;
        }
        if self.allow_super_property_lookup {
            context |= ctx::SUPER_PROPERTY;
        }
        if self.allow_super_constructor_call {
            context |= ctx::SUPER_CALL;
        }
        if self.in_class_field_initializer {
            context |= ctx::NO_ARGUMENTS;
        }
        if let Some(kind) = self.function_body {
            use crate::ast::FunctionKind;
            context |= ctx::RETURN | ctx::NEW_TARGET;
            if matches!(kind, FunctionKind::Generator | FunctionKind::AsyncGenerator) {
                context |= ctx::GENERATOR;
            }
            if matches!(kind, FunctionKind::Async | FunctionKind::AsyncGenerator) {
                context |= ctx::ASYNC | ctx::CAN_AWAIT;
            }
        }
        base::ProgramContext {
            context,
            starts_in_strict_mode: self.starts_in_strict_mode,
            initiated_by_eval: self.initiated_by_eval,
            has_no_globals: self.initiated_by_eval || self.suppress_globals,
        }
    }

    pub fn new(program_type: ProgramType) -> Self {
        Self {
            program_type,
            initial_line_number: 1,
            starts_in_strict_mode: false,
            initiated_by_eval: false,
            in_eval_function_context: false,
            allow_super_property_lookup: false,
            allow_super_constructor_call: false,
            in_class_field_initializer: false,
            function_body: None,
            suppress_globals: false,
            keep_whole_tree: false,
        }
    }
}

/// What parsing a source without errors leaves behind.
/// The rows of a top level with marks, and the row of the program.
type TopLevelRows = Result<(Ast, u32), OutOfStack>;

/// What a parse is for, which decides whether there is any use in rows for a sweep.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ParsePurpose {
    Compile,
    CheckSyntax,
}

struct ParserOutput {
    ast: Ast,
    top_level_rows: Option<TopLevelRows>,
    index: FunctionIndex,
    interner: Interner,
    root: u32,
    first_token_start: u32,
    is_strict_mode: bool,
    has_top_level_await: bool,
    eval_referenced_private_names: Vec<u32>,
    global_names_used_in_with: Vec<u32>,
    lines: LineTable,
}

/// Parses `source`, from a copy of it that is followed by the padding the lexer needs for as long as the parse takes.
fn parse_code_units<U: Unit + Default>(
    source: &[U],
    options: &ParseOptions,
    purpose: ParsePurpose,
) -> Result<ParserOutput, Vec<ParseError>> {
    let mut padded = Vec::with_capacity(source.len() + SOURCE_PADDING);
    padded.extend_from_slice(source);
    padded.resize(source.len() + SOURCE_PADDING, U::default());
    // SAFETY: The source is followed by the padding.
    let mut parser = unsafe {
        Parser::new(
            &padded[..source.len()],
            options.program_type == ProgramType::Module,
            if options.keep_whole_tree {
                Mode::Full
            } else {
                Mode::Lazy
            },
        )
    };
    parser.strict |= options.starts_in_strict_mode;
    parser.initiated_by_eval = options.initiated_by_eval;
    parser.add_context(options.program_context().context);
    let root = parser.parse_program();
    let lines = LineTable::new(source, options.initial_line_number);
    if let Some(error) = parser.err.take() {
        let position = lines.position(error.pos.min(u32_from_usize(source.len())));
        return Err(vec![ParseError {
            message: error.message,
            line: position.line,
            column: position.column,
        }]);
    }
    let top_level_rows =
        (purpose == ParsePurpose::Compile).then(|| parser.parse_top_level_rows(options.program_context()));
    Ok(ParserOutput {
        top_level_rows,
        root,
        first_token_start: parser.first_token_start,
        is_strict_mode: parser.strict,
        has_top_level_await: parser.has_top_level_await,
        ast: parser.ast,
        index: parser.index,
        interner: parser.interner,
        eval_referenced_private_names: parser.eval_referenced_private_names,
        global_names_used_in_with: parser.global_names_used_in_with,
        lines,
    })
}

/// Parses `source` at the width of its storage.
fn parse_source(
    source: &ak::Utf16String,
    options: &ParseOptions,
    purpose: ParsePurpose,
) -> Result<ParserOutput, Vec<ParseError>> {
    match source.as_units() {
        ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
            parse_code_units(units, options, purpose)
        }
        ak::Utf16StringUnits::Utf16(units) => parse_code_units(units, options, purpose),
    }
}

/// Parses `source` into a tree of its top-level code with its identifiers resolved.
pub fn parse_to_tree(source: &ak::Utf16String, options: &ParseOptions) -> Result<SyntaxTree, Vec<ParseError>> {
    let mut output = parse_source(source, options, ParsePurpose::Compile)?;
    let default_export_name = Name(output.interner.intern_bytes(b"*default*"));
    let default_constructor_parameter_name = Name(output.interner.intern_bytes(b"args"));
    let names =
        NameTable::from_parser_names((0..output.interner.len()).map(|name| output.interner.text(u32_from_usize(name))));
    let mut global_names_used_in_with = output.global_names_used_in_with;
    global_names_used_in_with.sort_unstable();
    global_names_used_in_with.dedup();
    output.index.shrink_to_fit();
    let script = Arc::new(ScriptData {
        index: output.index,
        names,
        positions: Arc::new(SourcePositions {
            source: source.clone(),
            lines: output.lines,
        }),
        program_type: options.program_type,
        program_context: options.program_context(),
        top_level_rows: Mutex::new(output.top_level_rows),
        default_export_name,
        default_constructor_parameter_name,
        global_names_used_in_with,
        reparse_context: Mutex::new(None),
        retains_compiled_functions: AtomicBool::new(false),
        retained_code_size: AtomicUsize::new(0),
    });
    let mut tree = SyntaxTree {
        script,
        ast: output.ast,
        root: output.root,
        first_token_start: output.first_token_start,
        is_strict_mode: output.is_strict_mode,
        has_top_level_await: options.program_type == ProgramType::Module && output.has_top_level_await,
        eval_referenced_private_names: output
            .eval_referenced_private_names
            .iter()
            .map(|&name| Name(name))
            .collect(),
        resolutions: Vec::new(),
        scopes: ScopeTable::default(),
        program_scope_row: 0,
        function_scope_rows: Vec::new(),
        outer_resolutions: None,
    };
    resolve::resolve_program(
        &mut tree,
        &resolve::ResolveOptions {
            initiated_by_eval: options.initiated_by_eval,
            suppress_globals: options.suppress_globals,
        },
    );
    Ok(tree)
}

/// Parses `source` for its syntax errors alone.
pub fn check_syntax(source: &ak::Utf16String, options: &ParseOptions) -> Result<(), Vec<ParseError>> {
    parse_source(source, options, ParsePurpose::CheckSyntax).map(|_| ())
}
