/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The JavaScript parser. Lexing is driven by the parser, the syntax tree is a set of flat columns, and scope analysis
//! happens while parsing, with no map per scope. [`crate::ast`] is how everything after the parser reads the tree.
//!
//! The parser itself is the `libjs_parser` crate, which does all of its work in the work zone of its thread. This
//! starts the parse jobs, and copies what they keep out of the zone.
//!
//! A source is parsed once for all of its early errors. That leaves a [`ScriptData`], which is all that is kept of the
//! source, and the tree of its top-level code, which lives as long as the job of the parse, so the work that needs it is
//! part of that job. A function is parsed again on its own when it is first called. Its tree lives as long as the
//! compile job that needs it, and nested functions are skipped by what the first parse recorded.

mod enclosing;
mod kept;
pub(crate) mod literals;

use libjs_parser::base;
use libjs_parser::columns;
use libjs_parser::intern;
use libjs_parser::resolve;
use libjs_parser::unit;

use crate::ast::Name;
use crate::ast::NameTable;
use crate::ast::Position;
use crate::ast::ProgramType;
use crate::ast::ScopeTable;
use crate::ast::SyntaxTree;
use crate::ast::TreeView;
use crate::source_text::SourceText;
use crate::u32_from_usize;
use crate::work_zone::with_work_zone;
use ak::Zone;
use ak::ZoneVec;
use base::Mode;
use base::OutOfStack;
use base::Parser;
use base::SOURCE_PADDING;
use columns::Ast;
use columns::AstColumns;
use columns::FunctionIndexBuilder;
use columns::function_flag;
use core::sync::atomic::AtomicBool;
use core::sync::atomic::AtomicU8;
use core::sync::atomic::AtomicUsize;
use core::sync::atomic::Ordering;
pub(crate) use enclosing::EnclosingCode;
pub(crate) use enclosing::OuterResolutions;
pub(crate) use enclosing::outer_entry;
use intern::Interner;
use kept::FunctionIndex;
use kept::KeptNames;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::Weak;
use unit::Unit;

/// A function of a source, as the row that the first parse of the source gave it.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct FunctionRow(pub(crate) u32);

/// What the parses of the functions of one source again hand on to each other. A parse of a function has its work data
/// in the work zone of its thread, and this on the heap.
struct ReparseContext {
    /// The names of the source.
    names: KeptNames,
    /// The state of each name, which is clean between two parses.
    name_states: Box<[base::NameState]>,
    /// The last stamp of a name state.
    stamp: u32,
}

/// The sources that have a reparse context now, for [`release_idle_reparse_contexts()`].
static SOURCES_WITH_REPARSE_CONTEXT: Mutex<Vec<Weak<ScriptData>>> = Mutex::new(Vec::new());

/// How many collections the reparse context of a source stays around for without a function of the source being parsed
/// again.
const REPARSE_CONTEXT_IDLE_COLLECTIONS: u8 = 2;

/// Lets go of the reparse contexts that no function was parsed with for a few collections. A context holds every name
/// of its source a second time, in tables sized for lookups, and most sources stop compiling functions once their page
/// has loaded. The next function of such a source to be parsed again builds the context again.
pub(crate) fn release_idle_reparse_contexts() {
    let mut released = Vec::new();
    let mut sources = SOURCES_WITH_REPARSE_CONTEXT
        .lock()
        .expect("nothing panics with the sources");
    sources.retain(|source| {
        let Some(source) = source.upgrade() else {
            return false;
        };
        // NB: The count stops at its largest value, for a compile job that holds the context for very many collections.
        let idle_collections = source
            .reparse_context_idle_collections
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |count| {
                Some(count.saturating_add(1))
            })
            .expect("the update always gives a value")
            .saturating_add(1);
        if idle_collections < REPARSE_CONTEXT_IDLE_COLLECTIONS {
            return true;
        }
        // NB: A compile job that holds the context is using it, and counts from zero again when it is done.
        let Ok(mut context) = source.reparse_context.try_lock() else {
            return true;
        };
        released.push(context.take());
        false
    });
    // NB: The contexts are freed without the list locked, since compile jobs on other threads wait for it.
    drop(sources);
}

/// All that is kept of a source once it is parsed: the source, one row for each of its functions, and the names that the
/// rows speak of. Compile jobs on any thread parse the functions again from this.
pub struct ScriptData {
    pub(crate) index: FunctionIndex,
    pub(crate) names: NameTable,
    pub(crate) positions: Arc<SourcePositions>,
    pub(crate) program_type: ProgramType,
    pub(crate) program_context: base::ProgramContext,
    pub(crate) default_export_name: Name,
    pub(crate) default_constructor_parameter_name: Name,
    /// The undeclared names that something in a `with` uses, which are not simply globals, sorted.
    global_names_used_in_with: Vec<u32>,
    /// Filled in when the first function gets parsed again, for the ones after it.
    reparse_context: Mutex<Option<ReparseContext>>,
    /// The collections since a function was last parsed again with the reparse context.
    reparse_context_idle_collections: AtomicU8,
    /// Whether the code that gets compiled for the functions of this source is worth keeping, because the program may
    /// get linked again.
    retains_compiled_functions: AtomicBool,
    /// The bytes of the code kept so far, for [`ScriptData::memory_size()`].
    retained_code_size: AtomicUsize,
}

/// Checks a regular expression literal with the regex engine of the runtime, for the parser (see `ValidateRegex`).
fn validate_regex<'z>(zone: &'z Zone, pattern: &[u16], flags: &[u16]) -> Result<(), &'z str> {
    crate::host::validate_regex(pattern, flags).map_err(|message| {
        core::str::from_utf8(zone.alloc_slice_copy(message.as_bytes())).expect("a copy of a string is one")
    })
}

/// How low a VM lets the stack of this thread go, or `None` if that cannot be told.
pub(crate) fn vm_stack_limit_of_this_thread() -> Option<usize> {
    ak::stack::lowest_address_of_this_thread()
        .map(|lowest_address| lowest_address.saturating_add(crate::build_configuration::VM_STACK_SPACE_LIMIT as usize))
}

impl ScriptData {
    #[cfg(test)]
    pub(crate) fn has_reparse_context(&self) -> bool {
        self.reparse_context.lock().expect("a reparse does not panic").is_some()
    }

    pub(crate) fn retains_compiled_functions(&self) -> bool {
        self.retains_compiled_functions.load(Ordering::Relaxed)
    }

    pub(crate) fn add_retained_code_size(&self, size: usize) {
        self.retained_code_size.fetch_add(size, Ordering::Relaxed);
    }

    pub(crate) fn remove_retained_code_size(&self, size: usize) {
        self.retained_code_size.fetch_sub(size, Ordering::Relaxed);
    }

    pub(crate) fn set_retains_compiled_functions(&self, retains_compiled_functions: bool) {
        self.retains_compiled_functions
            .store(retains_compiled_functions, Ordering::Relaxed);
    }

    /// The bytes of this source and of the tables that compile jobs read, and of the code kept for its functions so far.
    pub(crate) fn memory_size(&self) -> usize {
        size_of::<Self>()
            + self.positions.source.memory_size()
            + self.index.memory_size()
            + self.names.memory_size()
            + self.positions.lines.memory_size()
            + self.global_names_used_in_with.capacity() * size_of::<u32>()
            + self.retained_code_size.load(Ordering::Relaxed)
    }

    /// A tree of `columns` with the node `root`, for a sweep, which goes by the summary of the first parse: nothing
    /// in it is resolved.
    pub(crate) fn tree_of_rows<'t>(self: &Arc<Self>, columns: AstColumns<'t>, root: u32) -> SyntaxTree<'t> {
        SyntaxTree {
            script: self.clone(),
            source: self.positions.source.get(),
            view: TreeView {
                ast: columns,
                root,
                first_token_start: 0,
                is_strict_mode: false,
                has_top_level_await: false,
                eval_referenced_private_names: &[],
                resolutions: &[],
                scopes: ScopeTable::default(),
                program_scope_row: 0,
                function_scope_rows: &[],
            },
            outer_resolutions: None,
        }
    }

    /// Parses the function at `row` again into a tree in `zone` for a sweep, which goes by the summary of the first
    /// parse: nothing keeps track of scopes.
    pub(crate) fn reparse_for_sweep<'z>(
        self: &Arc<Self>,
        zone: &'z Zone,
        row: FunctionRow,
        stack_limit: usize,
        inline_argument_limit: usize,
    ) -> Result<(SyntaxTree<'z>, u32), OutOfStack> {
        let source = self.positions.source.get();
        let (ast, node) = {
            let mut context = self.reparse_context.lock().expect("a reparse does not panic");
            if context.is_none() {
                *context = Some(self.make_reparse_context(zone));
                SOURCES_WITH_REPARSE_CONTEXT
                    .lock()
                    .expect("nothing panics with the sources")
                    .push(Arc::downgrade(self));
            }
            self.reparse_context_idle_collections.store(0, Ordering::Relaxed);
            let context = context.as_mut().expect("the context was just filled in");
            let result = match source.as_units() {
                ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
                    self.reparse_code_units(zone, context, units, row, stack_limit, inline_argument_limit)
                }
                ak::Utf16StringUnits::Utf16(units) => {
                    self.reparse_code_units(zone, context, units, row, stack_limit, inline_argument_limit)
                }
            };
            // NB: Collections counted on while the context was held, and it was not idle then.
            self.reparse_context_idle_collections.store(0, Ordering::Relaxed);
            result
        }?;
        Ok((self.tree_of_rows(ast.into_columns(), node), node))
    }

    /// A reparse context, whose tables are worked out in `zone` and copied out at their final size.
    fn make_reparse_context(&self, zone: &Zone) -> ReparseContext {
        let names = KeptNames::of(&Interner::with_names(zone, self.names.parser_names()).tables());
        let name_states = vec![base::UNUSED_NAME; names.len().next_power_of_two()].into_boxed_slice();
        ReparseContext {
            names,
            name_states,
            stamp: 0,
        }
    }

    /// Bytes after a function are real source, so that it can be read in place. What the lexer sees of a function that
    /// comes too close to the end of the source for that gets copied, with the padding that the end of the source has
    /// to have.
    fn reparse_code_units<'z, U: Unit + Default>(
        &self,
        zone: &'z Zone,
        context: &mut ReparseContext,
        source: &[U],
        row: FunctionRow,
        stack_limit: usize,
        inline_argument_limit: usize,
    ) -> Result<(Ast<'z>, u32), OutOfStack> {
        let row_index = row.0 as usize;
        // NB: A tree of the whole function is the most it is likely to have, as the tree skips the code of nested
        //     functions. It starts with room for at most as many nodes as the work zone keeps memory for, and takes
        //     room for the rest in one step if it outgrows that.
        let length = (self.index.end[row_index] - self.index.start[row_index]) as usize;
        let whole_function_nodes = length / 3 + 16;
        let estimated_nodes = whole_function_nodes.min(1 << 16);
        let needs_padding = self.index.flags[row_index] & function_flag::REACHES_SOURCE_END != 0;
        let name_count = context.names.len();
        let mut parser = Parser::for_reparsing(
            zone,
            self.program_type == ProgramType::Module,
            Interner::of_tables(zone, context.names.tables_mut()),
            &mut context.name_states,
            context.stamp,
            self.index.view(),
            u32_from_usize(source.len()),
            estimated_nodes,
            validate_regex,
        );
        parser.ast.likely_most_nodes = whole_function_nodes;
        parser.tracks_scopes = false;
        parser.marks = true;
        parser.inline_argument_limit = inline_argument_limit;
        // SAFETY: The source outlives the parse, and the first parse found out that the lexer stays clear of its end
        //         unless the tail is what it reads.
        unsafe {
            if needs_padding {
                let first_read = self.index.params_start[row_index] as usize;
                let mut tail = ZoneVec::with_capacity_in(source.len() - first_read + SOURCE_PADDING, zone);
                tail.extend_from_slice(&source[first_read..]);
                tail.resize(tail.capacity(), U::default());
                parser.set_reparse_source(tail.as_ptr(), u32_from_usize(first_read), u32_from_usize(source.len()));
            } else {
                parser.set_reparse_source(source.as_ptr(), 0, u32_from_usize(source.len()));
            }
        }
        parser.stack_limit = stack_limit;
        let node = parser.reparse_function(row.0);
        let error = parser.err.take();
        // NB: The tables of the context, and the name states that cover them, stay as they are only if no name is new.
        assert_eq!(
            parser.interner.len(),
            name_count,
            "a function has only names that the first parse of its source found"
        );
        let (ast, stamp) = parser.end_reparse();
        context.stamp = stamp;
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

    /// What the resolver reads of this source.
    fn resolver_view(&self) -> resolve::ScriptView<'_> {
        resolve::ScriptView {
            index: self.index.view(),
            default_export_name: self.default_export_name,
            is_module: self.program_type == ProgramType::Module,
            global_names_used_in_with: &self.global_names_used_in_with,
        }
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
    pub(crate) source: Arc<SourceText>,
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

    /// The table of `source`, which is worked out in `zone` and copied out at its final size.
    fn new<U: Unit>(zone: &Zone, source: &[U], initial_line_number: u32) -> Self {
        let mut line_starts = ZoneVec::from_slice_in(&[0], zone);
        let mut low_surrogates = ZoneVec::new_in(zone);
        let mut index = 0;
        while index < source.len() {
            let code_unit = source[index].code();
            index += 1;
            // OPTIMIZATION: Nearly every unit is neither a line terminator nor a surrogate, which one range rules out.
            if (0x0E..0x2028).contains(&code_unit) {
                continue;
            }
            if code_unit == 0x0D && source.get(index).is_some_and(|next| next.code() == 0x0A) {
                index += 1;
                line_starts.push(u32_from_usize(index));
            } else if matches!(code_unit, 0x0A | 0x0D | 0x2028 | 0x2029) {
                line_starts.push(u32_from_usize(index));
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
        // NB: A line feed after a carriage return ends the line that the carriage return started.
        let ends_with_lone_line_terminator = match source {
            [.., before, last] if last.code() == 0x0A => before.code() != 0x0D,
            [.., last] => matches!(last.code(), 0x0A | 0x0D | 0x2028 | 0x2029),
            [] => false,
        };
        Self {
            line_starts: line_starts.to_vec(),
            low_surrogates: low_surrogates.to_vec(),
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

/// The rows of a top level with marks for a sweep, and the row of the program, or that they did not fit the stack.
pub(crate) type TopLevelRows<'z> = Result<(AstColumns<'z>, u32), OutOfStack>;

/// What a parse is for, which decides whether there is any use in a tree.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ParsePurpose {
    Compile,
    CheckSyntax,
}

/// Parses `units`, the code units of `source`, from a copy of them in `zone` that is followed by the padding the lexer
/// needs. A parse for a compile runs `job` on the tree of the source and the rows of its top level, in the zone.
fn parse_code_units<'z, U: Unit + Default, R>(
    zone: &'z Zone,
    source: &ak::Utf16String,
    units: &[U],
    options: &ParseOptions,
    purpose: ParsePurpose,
    job: impl FnOnce(&SyntaxTree<'z>, TopLevelRows<'z>) -> R,
) -> Result<Option<R>, Vec<ParseError>> {
    let mut padded = ZoneVec::with_capacity_in(units.len() + SOURCE_PADDING, zone);
    padded.extend_from_slice(units);
    padded.resize(units.len() + SOURCE_PADDING, U::default());
    let padded = padded.into_slice();
    // SAFETY: The source is followed by the padding.
    let mut parser = unsafe {
        Parser::new(
            zone,
            &padded[..units.len()],
            options.program_type == ProgramType::Module,
            if options.keep_whole_tree {
                Mode::Full
            } else {
                Mode::Lazy
            },
            &KeptNames::initial(zone).tables(),
            validate_regex,
            vm_stack_limit_of_this_thread(),
        )
    };
    parser.strict |= options.starts_in_strict_mode;
    parser.initiated_by_eval = options.initiated_by_eval;
    parser.add_context(options.program_context().context);
    let root = parser.parse_program();
    if let Some(error) = parser.err.take() {
        let lines = LineTable::new(zone, units, options.initial_line_number);
        let position = lines.position(error.pos.min(u32_from_usize(units.len())));
        return Err(vec![ParseError {
            message: error.message.to_owned(),
            line: position.line,
            column: position.column,
        }]);
    }
    if purpose == ParsePurpose::CheckSyntax {
        return Ok(None);
    }
    let lines = LineTable::new(zone, units, options.initial_line_number);
    let index = core::mem::replace(&mut parser.index, FunctionIndexBuilder::with_capacity(zone, 0)).finish();
    let top_level_rows = parser
        .parse_top_level_rows(
            options.program_context(),
            index,
            crate::bytecode::operand::MAX_OPERAND_LIST_LENGTH,
        )
        .map(|(rows, node)| (rows.into_columns(), node));
    let default_export_name = Name(parser.interner.intern_bytes(b"*default*"));
    let default_constructor_parameter_name = Name(parser.interner.intern_bytes(b"args"));
    let interner = &parser.interner;
    let names = NameTable::from_parser_names((0..interner.len()).map(|name| interner.text(u32_from_usize(name))));
    let mut global_names_used_in_with = parser.global_names_used_in_with.take();
    global_names_used_in_with.sort_unstable();
    global_names_used_in_with.dedup();
    let eval_referenced_private_names =
        zone.alloc_slice_from_iter(parser.eval_referenced_private_names.iter().map(|&name| Name(name)));
    let script = Arc::new(ScriptData {
        index: FunctionIndex::of(&index),
        names,
        positions: Arc::new(SourcePositions {
            source: SourceText::of(source),
            lines,
        }),
        program_type: options.program_type,
        program_context: options.program_context(),
        default_export_name,
        default_constructor_parameter_name,
        global_names_used_in_with: global_names_used_in_with.to_vec(),
        reparse_context: Mutex::new(None),
        reparse_context_idle_collections: AtomicU8::new(0),
        retains_compiled_functions: AtomicBool::new(false),
        retained_code_size: AtomicUsize::new(0),
    });
    let view = TreeView {
        ast: parser.ast.into_columns(),
        root,
        first_token_start: parser.first_token_start,
        is_strict_mode: parser.strict,
        has_top_level_await: options.program_type == ProgramType::Module && parser.has_top_level_await,
        eval_referenced_private_names,
        resolutions: &[],
        scopes: ScopeTable::default(),
        program_scope_row: 0,
        function_scope_rows: &[],
    };
    let resolved = resolve::resolve_program(
        zone,
        &view,
        &script.resolver_view(),
        &resolve::ResolveOptions {
            initiated_by_eval: options.initiated_by_eval,
            suppress_globals: options.suppress_globals,
        },
    );
    let tree = SyntaxTree {
        script,
        source: source.clone(),
        view: TreeView {
            resolutions: resolved.resolutions,
            scopes: resolved.scopes,
            program_scope_row: resolved.program_scope_row,
            function_scope_rows: resolved.function_scope_rows,
            ..view
        },
        outer_resolutions: OuterResolutions::of_entries(resolved.outer_entries),
    };
    Ok(Some(job(&tree, top_level_rows)))
}

/// Parses `source` at the width of its storage, in one job with the work zone of the thread for all of its work data.
fn parse_source<R>(
    source: &ak::Utf16String,
    options: &ParseOptions,
    purpose: ParsePurpose,
    job: impl for<'z> FnOnce(&SyntaxTree<'z>, TopLevelRows<'z>) -> R,
) -> Result<Option<R>, Vec<ParseError>> {
    with_work_zone(|zone| match source.as_units() {
        ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
            parse_code_units(zone, source, units, options, purpose, job)
        }
        ak::Utf16StringUnits::Utf16(units) => parse_code_units(zone, source, units, options, purpose, job),
    })
}

/// Parses `source` into a tree of its top-level code with its identifiers resolved, and runs `job` on the tree and the
/// rows of its top level for a sweep. The tree lives only as long as the job of the parse, so `job` is the rest of the
/// work that needs it.
pub fn with_parsed_tree<R>(
    source: &ak::Utf16String,
    options: &ParseOptions,
    job: impl for<'z> FnOnce(&SyntaxTree<'z>, TopLevelRows<'z>) -> R,
) -> Result<R, Vec<ParseError>> {
    parse_source(source, options, ParsePurpose::Compile, job)
        .map(|result| result.expect("a parse for a compile runs its job"))
}

/// Parses `source` for its syntax errors alone.
pub fn check_syntax(source: &ak::Utf16String, options: &ParseOptions) -> Result<(), Vec<ParseError>> {
    parse_source(source, options, ParsePurpose::CheckSyntax, |_, _| ()).map(|_| ())
}
