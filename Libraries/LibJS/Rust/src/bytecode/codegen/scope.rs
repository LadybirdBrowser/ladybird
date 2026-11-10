/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a name means in the code being generated. The first parse left a summary of the bindings of the function: which
//! ones functions nested in it capture, and which ones come up before their declaration. Going through the code in
//! source order, that is all it takes: a name that is not declared by the time it comes up is free.
//!
//! A binding gets its place when its declaration comes up. What has to happen for it at the start of the function goes
//! into a head segment that ends up in front of everything else.

use std::sync::Arc;

use libjs_abi::ArgumentsKind;

use crate::ast::DeclarationKind;
use crate::ast::FunctionKind;
use crate::ast::LocalType;
use crate::ast::Name;
use crate::ast::Resolution;
use crate::ast::SyntaxTree;
use crate::bytecode::executable_data::ConstantValue;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::executable_data::FunctionSfdMetadata;
use crate::bytecode::executable_data::LocalVariableMetadata;
use crate::bytecode::executable_data::PendingClassBlueprint;
use crate::bytecode::executable_data::PendingSharedFunctionData;
use crate::bytecode::executable_data::PrecompiledFunction;
use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;
use crate::bytecode::source_map::SourceOffset;
use crate::parser::EnclosingCode;
use crate::parser::FunctionRow;
use crate::parser::columns::NONE;
use crate::parser::columns::function_flag;
use crate::parser::token::N_ARGUMENTS;
use crate::parser::token::N_DOT_NEW_TARGET;
use crate::parser::token::N_DOT_SUPER;
use crate::parser::token::N_DOT_THIS;
use crate::parser::token::N_DOT_UNBOUND;
use crate::slice_block::SliceBlock;
use crate::u32_from_usize;

use super::EnclosingEnvironmentScope;
use super::bindings::*;
use super::context::Generator;
use super::environment::BindingLocation;
use super::nodes::Identifier;
use super::registers::ScopedOperand;
use super::stream::Control;
use super::stream::HeldRegisters;
use super::stream::ScopeEnvironment;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Location {
    Local(u32),
    Argument(u32),
    Environment,
    Outer,
}

/// A binding of the function being generated, or what a free name means.
#[derive(Clone, Copy)]
struct Binding {
    name: Name,
    location: Location,
    kind: Option<DeclarationKind>,
    is_global: bool,
    /// The block that declares it.
    block: BlockId,
    /// The binding of the same name that this one hides.
    shadowed: Option<BindingId>,
    /// Whether this is what a `catch` clause calls its value.
    is_catch_parameter: bool,
    /// Whether this is of the parameters, which have a scope apart from the body, and has no `var` of the same name yet.
    in_parameters: bool,
    /// Whether this has the name of a parameter of a function with a scope for its parameters.
    is_parameter: bool,
}

/// What is known of a name in the function being generated.
#[derive(Clone, Copy, Default)]
struct NameEntry {
    generation: u32,
    binding: Option<BindingId>,
}

/// A binding in the table of the function being generated.
#[derive(Clone, Copy, PartialEq, Eq)]
struct BindingId(u32);

/// A block of the function being generated, where zero is the function itself.
#[derive(Clone, Copy, PartialEq, Eq, Default)]
struct BlockId(u32);

#[derive(Clone, Copy)]
struct LocalSlot {
    name: Name,
    is_mutable: bool,
    is_lexical: bool,
    /// Where in the source the block that declares it starts and ends, if a block does.
    block_range: Option<(u32, u32)>,
}

/// The name that a function nested in the one being generated gets from around it.
enum NestedFunctionName {
    None,
    Identifier(IdentifierTableIndex),
    /// The initializer of a field, with what the field is called if that is known and whether that is a private name.
    Field(Option<ak::Utf16String>, bool),
}

/// A function nested in the one being generated.
struct NestedFunction {
    row: u32,
    name: NestedFunctionName,
    /// What the function sees of environments if it is in the environment of a scope, which only it sees.
    environment: Option<Arc<EnclosingEnvironmentScope>>,
    /// Whether the function gets compiled along with the code around it.
    is_eager: bool,
}

/// The bindings of a scope inside a function that exist from the start of the scope, as a range of the summary.
#[derive(Clone, Copy)]
pub(super) struct ScopeBindings {
    position: u32,
    /// Where in the pool of summaries the bindings start.
    start: u32,
    count: u32,
    captured_count: u32,
}

/// What the summary says of a binding that exists from the start of its scope.
#[derive(Clone, Copy)]
struct ScopeBinding {
    name: Name,
    kind: BindingKind,
    is_captured: bool,
}

/// How the summary tells kinds of bindings apart.
#[derive(Clone, Copy, PartialEq, Eq)]
enum BindingKind {
    Plain,
    Let,
    Const,
}

impl ScopeBinding {
    #[inline(always)]
    fn from_entry(entry: u32) -> Self {
        Self {
            name: Name(entry >> 3),
            kind: match (entry >> 1) & 3 {
                1 => BindingKind::Let,
                2 => BindingKind::Const,
                _ => BindingKind::Plain,
            },
            is_captured: entry & 1 != 0,
        }
    }
}

/// What `declare_parameters()` has to know of a parameter that is more than a name.
#[derive(Clone, Copy)]
pub(super) struct ParameterShape {
    /// How many names it binds.
    pub(super) name_count: u32,
    /// Whether it binds one name in the place of its argument, which a default or being a rest parameter leaves it at.
    pub(super) is_name: bool,
    /// Whether anything gets evaluated in it.
    pub(super) has_expression: bool,
}

/// What the top level of a program does with names, which is what declaration instantiation sees to.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum ProgramKind {
    /// A name that nothing binds is global.
    Script,
    /// A name that nothing binds is up to the environments around the code.
    WithoutGlobals,
    /// A name is in the environment of the module, or else global.
    Module,
}

/// The names of the function being generated, which keep their memory for the next function on the thread.
#[derive(Default)]
pub struct CodeScope {
    pub(super) is_active: bool,
    row: u32,
    /// The flags of the function, or what stands for them at the top level.
    flags: u32,
    /// What the code is, if it is the top level of a program.
    program: Option<ProgramKind>,
    /// Whether the next nested function is one to compile right away.
    next_function_is_eager: bool,
    generation: u32,
    entries: Vec<NameEntry>,
    bindings: Vec<Binding>,
    /// Where the bindings of the open blocks start, as indices of `bindings`, and the block around each.
    block_starts: Vec<(u32, BlockId)>,
    next_block: BlockId,
    current_block: BlockId,
    locals: Vec<LocalSlot>,
    parameters: Vec<(Name, bool)>,
    /// What is left of the bitmap of the summary, as a range of its bytes in the index, and the next bit.
    bitmap_start: usize,
    bitmap_end: usize,
    next_bit: usize,
    environment_binding_count: u32,
    /// The names of simple parameters, and whether each is in the environment.
    simple_parameters: Vec<(Name, bool)>,
    /// Whether the body has a scope of its own, apart from the parameters.
    has_parameter_scope: bool,
    /// Whether a name can still be in the dead zone of a parameter.
    checks_parameters: bool,
    is_in_parameters: bool,
    has_function_in_parameters: bool,
    has_mapped_parameters: bool,
    /// What the summary has on bindings that come up early, as a range of it, which waits for the end of the parameters.
    deferred_early_bindings: Option<(usize, usize)>,
    /// Whether nested functions capture a `var` of the name `arguments`, which is the binding of the arguments object.
    var_of_arguments_is_captured: bool,
    /// Whether a lexical declaration of the name `arguments` takes the place of the arguments object.
    has_lexical_arguments: bool,
    /// Whether a call of the function creates an environment of its own, which the environment coordinates count.
    has_function_environment: bool,
    /// Whether the lexical declarations of the function have an environment of their own.
    has_lexical_environment: bool,
    /// Whether the parameters have an environment of their own, for what an `eval` in them declares.
    has_parameter_expressions_with_eval: bool,
    /// How many of the bindings are in the function environment, if the body has a variable environment of its own.
    function_environment_binding_count: Option<u32>,
    /// The scopes with bindings that exist from their start, by position.
    scope_bindings: Vec<ScopeBindings>,
    /// How many environments of scopes are open.
    scope_environment_depth: u32,
    /// How many `with` statements are around the code.
    with_depth: u32,
    /// What functions nested in scopes see of the bindings of those scopes, which are gone by the end of the function.
    scope_resolutions: Vec<(u64, Resolution)>,
    nested_functions: Vec<NestedFunction>,
    /// The memory of what `sweep_parameter_names()` puts together.
    pub(super) spare_parameter_names: Vec<Name>,
    pub(super) spare_parameter_shapes: Vec<ParameterShape>,
    /// What a name means that no scope around the function binds, once something asked.
    unbound: Option<Resolution>,
}

impl CodeScope {
    pub(super) fn clear(&mut self) {
        if !self.is_active {
            return;
        }
        self.is_active = false;
        self.bindings.clear();
        self.block_starts.clear();
        self.locals.clear();
        self.parameters.clear();
        self.nested_functions.clear();
        self.scope_bindings.clear();
        self.scope_resolutions.clear();
        self.scope_environment_depth = 0;
        self.with_depth = 0;
        self.unbound = None;
        self.generation = self.generation.wrapping_add(1);
        if self.generation == 0 {
            self.entries.fill(NameEntry::default());
            self.generation = 1;
        }
    }

    #[inline(always)]
    fn entry(&mut self, name: Name) -> &mut NameEntry {
        let index = name.0 as usize;
        if index >= self.entries.len() {
            self.entries
                .resize((index + 1).next_power_of_two(), NameEntry::default());
        }
        &mut self.entries[index]
    }

    #[inline(always)]
    fn binding_of(&mut self, name: Name) -> Option<BindingId> {
        let generation = self.generation;
        let entry = self.entry(name);
        if entry.generation == generation {
            entry.binding
        } else {
            None
        }
    }

    #[inline(always)]
    fn binding(&self, id: BindingId) -> Binding {
        self.bindings[id.0 as usize]
    }

    #[inline(always)]
    fn add_binding(&mut self, binding: Binding) -> BindingId {
        let id = BindingId(u32_from_usize(self.bindings.len()));
        self.bindings.push(binding);
        let generation = self.generation;
        *self.entry(binding.name) = NameEntry {
            generation,
            binding: Some(id),
        };
        id
    }

    #[inline(always)]
    pub(super) fn is_local_lexical(&self, index: u32) -> bool {
        self.locals.get(index as usize).is_some_and(|local| local.is_lexical)
    }

    pub(super) fn local_count(&self) -> u32 {
        u32_from_usize(self.locals.len())
    }

    #[inline(always)]
    pub(super) fn has_simple_parameters(&self) -> bool {
        self.flags & function_flag::SIMPLE_PARAMETERS != 0
    }

    #[inline(always)]
    pub(super) fn is_program(&self) -> bool {
        self.program.is_some()
    }

    #[inline(always)]
    pub(super) fn is_in_with(&self) -> bool {
        self.with_depth != 0
    }

    #[inline(always)]
    pub(super) fn is_in_block(&self) -> bool {
        self.current_block != BlockId::default()
    }

    pub(super) fn block_begin(&mut self) {
        self.block_starts
            .push((u32_from_usize(self.bindings.len()), self.current_block));
        self.next_block = BlockId(self.next_block.0 + 1);
        self.current_block = self.next_block;
    }

    /// The block that ends here goes from `start` to `end` in the source, which is what a debugger goes by for its locals.
    pub(super) fn note_block_range(&mut self, start: u32, end: u32) {
        let first_binding = self.block_starts.last().expect("a block is open").0 as usize;
        for binding in &self.bindings[first_binding..] {
            if binding.block == self.current_block
                && let Location::Local(index) = binding.location
            {
                self.locals[index as usize].block_range = Some((start, end));
            }
        }
    }

    pub(super) fn block_end(&mut self) {
        let (start, outer_block) = self.block_starts.pop().expect("a block is open");
        // NB: Everything that a block declares goes away with it, and what is left in here are free names and `var`s.
        for index in (start as usize..self.bindings.len()).rev() {
            let binding = self.bindings[index];
            if binding.block == self.current_block {
                let generation = self.generation;
                *self.entry(binding.name) = NameEntry {
                    generation,
                    binding: binding.shadowed,
                };
            }
        }
        self.current_block = outer_block;
    }
}

/// Whether the function at a row with `flags` gets `this` from its environment, which the code of a class always does.
fn this_comes_from_environment(flags: u32) -> bool {
    use function_flag::*;
    flags
        & (SCOPE_THIS_FROM_ENVIRONMENT
            | INSIGHT_THIS_FROM_ENVIRONMENT
            | FIELD_INITIALIZER
            | STATIC_BLOCK
            | DEFAULT_CONSTRUCTOR)
        != 0
}

/// Reads a variable length integer of a summary.
#[inline(always)]
fn read_varint(bytes: &[u8], at: &mut usize) -> u32 {
    let mut value = 0u32;
    let mut shift = 0;
    loop {
        let byte = bytes[*at];
        *at += 1;
        value |= u32::from(byte & 0x7F) << shift;
        if byte & 0x80 == 0 {
            return value;
        }
        shift += 7;
    }
}

impl Generator<'_> {
    /// Gets ready for the function at `row`, which the summary says the bindings of.
    pub(super) fn begin_function_scope(&mut self, row: FunctionRow) {
        let tree = self.tree;
        let index = &tree.script.index;
        let flags = index.flags[row.0 as usize];
        let (summary_start, summary_end) = index.summary_range(row.0);
        let summary = &index.summaries[summary_start..summary_end];
        self.strict = flags & function_flag::STRICT != 0;
        self.this_value_needs_environment_resolution = this_comes_from_environment(flags);
        self.enclosing_function_kind = FunctionKind::from_async_generator(
            flags & function_flag::ASYNC != 0,
            flags & function_flag::GENERATOR != 0,
        );
        self.creates_environment = false;
        self.contains_direct_call_to_eval_in_non_strict_mode =
            flags & function_flag::DIRECT_EVAL != 0 && flags & function_flag::STRICT == 0;
        let is_arrow = flags & function_flag::ARROW != 0;

        self.reset_scope(row.0, flags, None);
        let scope = &mut self.context.scope;

        let mut at = 0;
        let early_count = if summary.is_empty() {
            0
        } else {
            read_varint(summary, &mut at)
        };
        let early_start = at;
        let mut has_captures = false;
        let mut has_var_of_arguments = false;
        for _ in 0..early_count {
            let early = ScopeBinding::from_entry(read_varint(summary, &mut at));
            has_captures |= early.is_captured;
            if early.name.0 == N_ARGUMENTS && flags & function_flag::SIMPLE_PARAMETERS != 0 {
                if early.kind == BindingKind::Plain {
                    has_var_of_arguments = true;
                    scope.var_of_arguments_is_captured = early.is_captured;
                } else {
                    scope.has_lexical_arguments = true;
                }
            }
        }
        let early_end = at;
        if !summary.is_empty() {
            read_scope_bindings(&mut scope.scope_bindings, summary, summary_start, &mut at);
        }
        has_captures |= at < summary.len();
        scope.bitmap_start = summary_start + at;
        scope.bitmap_end = summary_end;

        self.begin_stream();
        self.begin_head();
        self.put(op::Enter {});
        let needs_function_environment = has_captures
            || flags & function_flag::CONTAINS_EVAL != 0
            || (!is_arrow && self.this_value_needs_environment_resolution)
            || (!is_arrow
                && flags & function_flag::CAPTURES_ARGUMENTS != 0
                && flags & (function_flag::MIGHT_NEED_ARGUMENTS | function_flag::USES_ARGUMENTS) != 0)
            || (!is_arrow
                && flags & function_flag::ARGUMENTS_ACCESS != 0
                && flags & function_flag::SIMPLE_PARAMETERS != 0
                && index.parameter_count[row.0 as usize] != 0);
        self.context.scope.has_function_environment = needs_function_environment;
        self.capture_saved_lexical_environment_with_coordinates(needs_function_environment);
        self.end_head();
        self.begin_stream_temporaries();

        use function_flag::*;
        if flags & (FIELD_INITIALIZER | STATIC_BLOCK | DEFAULT_CONSTRUCTOR) == 0
            && (flags & SIMPLE_PARAMETERS == 0
                || self.contains_direct_call_to_eval_in_non_strict_mode
                || (has_var_of_arguments && !is_arrow))
        {
            // NB: The parameters do not see what is in the body, and what an `eval` declares has to clash with `let`.
            //     A `var` of the name `arguments` has to find the arguments object.
            let scope = &mut self.context.scope;
            scope.is_in_parameters = true;
            scope.deferred_early_bindings = Some((summary_start + early_start, summary_start + early_end));
        } else {
            self.declare_early_bindings(summary_start + early_start, summary_start + early_end);
        }
    }

    fn reset_scope(&mut self, row: u32, flags: u32, program: Option<ProgramKind>) {
        let scope = &mut self.context.scope;
        scope.is_active = true;
        scope.row = row;
        scope.flags = flags;
        scope.program = program;
        scope.next_function_is_eager = false;
        scope.next_block = BlockId::default();
        scope.current_block = BlockId::default();
        scope.environment_binding_count = 0;
        scope.has_parameter_scope = false;
        scope.checks_parameters = false;
        scope.is_in_parameters = false;
        scope.has_function_in_parameters = false;
        scope.var_of_arguments_is_captured = false;
        scope.has_lexical_arguments = false;
        scope.has_function_environment = false;
        scope.has_mapped_parameters = false;
        scope.deferred_early_bindings = None;
        scope.has_parameter_expressions_with_eval = false;
        scope.has_lexical_environment = false;
        scope.function_environment_binding_count = None;
        scope.next_bit = 0;
        if scope.generation == 0 {
            scope.generation = 1;
        }
    }

    /// Gets ready for the top level of a program, whose own declarations are up to declaration instantiation.
    pub(super) fn begin_program_scope(&mut self, kind: ProgramKind, is_async_module: bool) {
        let script = self.tree.script.clone();
        let index = &script.index;
        let summary = &index.summaries[..index.program_summary_end as usize];
        self.creates_environment = false;
        let flags = if index.program_has_direct_eval {
            function_flag::DIRECT_EVAL
        } else {
            0
        };
        self.reset_scope(u32::MAX, flags, Some(kind));
        if !summary.is_empty() {
            let mut at = 0;
            read_varint(summary, &mut at);
            read_scope_bindings(&mut self.context.scope.scope_bindings, summary, 0, &mut at);
        }
        let scope = &mut self.context.scope;
        scope.bitmap_start = 0;
        scope.bitmap_end = 0;

        self.begin_stream();
        self.begin_head();
        self.put(op::Enter {});
        self.capture_saved_lexical_environment_with_coordinates(is_async_module);
        self.end_head();
        self.begin_stream_temporaries();
        if !is_async_module {
            self.begin_completion();
        }
    }

    /// The function about to come up gets compiled along with the code around it.
    pub(super) fn note_next_function_is_eager(&mut self) {
        self.context.scope.next_function_is_eager = true;
    }

    /// What the summary says comes up before its declaration, which then finds it here.
    fn declare_early_bindings(&mut self, mut at: usize, end: usize) {
        let tree = self.tree;
        let summaries = &tree.script.index.summaries;
        while at < end {
            let early = ScopeBinding::from_entry(read_varint(summaries, &mut at));
            if early.kind != BindingKind::Plain {
                self.add_early_lexical_binding(early);
                continue;
            }
            let scope = &mut self.context.scope;
            let existing = scope
                .binding_of(early.name)
                .filter(|&binding| scope.binding(binding).location != Location::Outer);
            match existing {
                Some(parameter) if scope.binding(parameter).in_parameters => {
                    self.add_var_of_parameter(parameter, early.is_captured);
                }
                Some(_) => {}
                None => {
                    self.add_var_binding(early.name, early.is_captured, None);
                }
            }
        }
    }

    /// A scope inside the function starts at `position`, and its environment if it needs one.
    pub(super) fn begin_scope(&mut self, position: u32, is_catch: bool) -> Option<ScopeEnvironment> {
        let scope = &mut self.context.scope;
        scope.block_begin();
        if scope.scope_bindings.is_empty() {
            return None;
        }
        let bindings = self.scope_bindings_at(position)?;
        self.declare_scope_bindings(bindings);
        (bindings.captured_count != 0).then(|| self.enter_scope_environment(bindings, is_catch, None))
    }

    pub(super) fn end_scope(&mut self, environment: Option<ScopeEnvironment>) {
        if let Some(environment) = environment {
            self.leave_scope_environment(environment);
        }
        self.context.scope.block_end();
    }

    pub(super) fn scope_bindings_at(&self, position: u32) -> Option<ScopeBindings> {
        let scopes = &self.context.scope.scope_bindings;
        let index = scopes
            .binary_search_by_key(&position, |bindings| bindings.position)
            .ok()?;
        Some(scopes[index])
    }

    /// Calls `callback` with what the summary says of each binding of a scope.
    fn for_each_scope_binding(&mut self, bindings: ScopeBindings, mut callback: impl FnMut(&mut Self, ScopeBinding)) {
        let tree = self.tree;
        let summaries = &tree.script.index.summaries;
        let mut at = bindings.start as usize;
        for _ in 0..bindings.count {
            callback(self, ScopeBinding::from_entry(read_varint(summaries, &mut at)));
        }
    }

    /// The bindings of a scope that exist from its start get their place. Those in locals are in their dead zone, whatever
    /// a run through the scope before left in them.
    fn declare_scope_bindings(&mut self, bindings: ScopeBindings) {
        self.for_each_scope_binding(bindings, |generator, binding| {
            let scope = &mut generator.context.scope;
            let location = if binding.is_captured {
                Location::Environment
            } else {
                let index = scope.local_count();
                scope.locals.push(LocalSlot {
                    name: binding.name,
                    is_mutable: binding.kind != BindingKind::Const,
                    is_lexical: binding.kind != BindingKind::Plain,
                    block_range: None,
                });
                Location::Local(index)
            };
            let shadowed = scope.binding_of(binding.name);
            let block = scope.current_block;
            scope.add_binding(Binding {
                name: binding.name,
                location,
                kind: Some(match binding.kind {
                    BindingKind::Const => DeclarationKind::Const,
                    _ => DeclarationKind::Let,
                }),
                is_global: false,
                block,
                shadowed,
                is_catch_parameter: false,
                in_parameters: false,
                is_parameter: false,
            });
            if let Location::Local(index) = location {
                let empty = generator.context.constants.add_empty();
                generator.put(op::Mov {
                    dst: Operand::local(index),
                    src: empty,
                });
            }
        });
    }

    /// The code changes the running environment somewhere, so that it has to know the one that it started with.
    fn note_creates_environment(&mut self) {
        if !self.creates_environment {
            self.creates_environment = true;
            self.in_function_head(|generator| {
                generator.emit(op::GetLexicalEnvironment {
                    dst: Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT),
                });
            });
        }
    }

    /// Creates the environment of a scope, in `register` if the scope had one before.
    pub(super) fn enter_scope_environment(
        &mut self,
        bindings: ScopeBindings,
        is_catch: bool,
        register: Option<Operand>,
    ) -> ScopeEnvironment {
        self.note_creates_environment();
        let parent = self.current_lexical_environment().operand();
        let held = self.hold_registers();
        let register = register.unwrap_or_else(|| self.held_register());
        let shape_cache = self.next_environment_shape_cache();
        self.emit(op::CreateLexicalEnvironment {
            dst: register,
            parent,
            capacity: bindings.captured_count,
            shape_cache,
            is_catch_environment: is_catch,
        });
        self.push_static_lexical_environment(ScopedOperand::plain(register));
        self.context.scope.scope_environment_depth += 1;
        self.for_each_scope_binding(bindings, |generator, binding| {
            if !binding.is_captured {
                return;
            }
            let identifier = generator.intern_identifier(binding.name);
            let is_const = binding.kind == BindingKind::Const;
            generator.emit(op::CreateVariable {
                identifier,
                mode: libjs_abi::EnvironmentMode::Lexical as u32,
                is_immutable: is_const,
                is_global: false,
                is_strict: is_const,
            });
        });
        ScopeEnvironment { register, parent, held }
    }

    pub(super) fn leave_scope_environment(&mut self, environment: ScopeEnvironment) {
        self.pop_tracked_lexical_environment();
        self.context.scope.scope_environment_depth -= 1;
        if !self.is_stream_dead() {
            let parent = self.scoped_operand(environment.parent);
            self.restore_lexical_environment(&parent);
        }
        self.release_held_registers(environment.held);
    }

    /// The body of a `with` starts, whose environment is `object`.
    pub(super) fn enter_object_environment(&mut self, object: Operand, held: HeldRegisters) -> ScopeEnvironment {
        self.note_creates_environment();
        let parent = self.current_lexical_environment().operand();
        let register = self.held_register();
        self.emit(op::EnterObjectEnvironment { dst: register, object });
        self.push_dynamic_lexical_environment(ScopedOperand::plain(register));
        let scope = &mut self.context.scope;
        scope.scope_environment_depth += 1;
        scope.with_depth += 1;
        ScopeEnvironment { register, parent, held }
    }

    pub(super) fn leave_object_environment(&mut self, environment: ScopeEnvironment) {
        self.context.scope.with_depth -= 1;
        self.leave_scope_environment(environment);
    }

    /// Gives a loop a new copy of the environment of its head for the next iteration.
    pub(super) fn copy_scope_environment(&mut self, bindings: ScopeBindings, environment: ScopeEnvironment) {
        let mut names = Vec::with_capacity(bindings.captured_count as usize);
        self.for_each_scope_binding(bindings, |generator, binding| {
            if binding.is_captured {
                let identifier = generator.intern_identifier(binding.name);
                let value = generator.temporary();
                emit_get_binding(generator, value, identifier, false);
                names.push((identifier, value));
            }
        });
        self.pop_tracked_lexical_environment();
        self.context.scope.scope_environment_depth -= 1;
        let parent = self.scoped_operand(environment.parent);
        self.restore_lexical_environment(&parent);
        self.enter_scope_environment(bindings, false, Some(environment.register));
        for (identifier, value) in names {
            emit_initialize_lexical_binding(self, identifier, value);
        }
        self.end_of_statement();
    }

    /// A `var` of the body with the name of what `parameter` is, which starts out with the value of that. Without anything
    /// that sees them apart they stay one binding.
    fn add_var_of_parameter(&mut self, parameter: BindingId, is_captured: bool) -> BindingId {
        let scope = &mut self.context.scope;
        let binding = scope.binding(parameter);
        if !is_captured && binding.location != Location::Environment {
            let binding = &mut scope.bindings[parameter.0 as usize];
            binding.in_parameters = false;
            binding.kind = Some(DeclarationKind::Var);
            return parameter;
        }
        self.add_var_binding(binding.name, is_captured, Some(binding))
    }

    /// A `var` of the function, which starts out as `undefined` or with the value of `parameter`.
    fn add_var_binding(&mut self, name: Name, is_captured: bool, parameter: Option<Binding>) -> BindingId {
        let initial_value = |generator: &mut Self| match parameter.map(|parameter| parameter.location) {
            None => generator.context.constants.add_undefined(),
            Some(Location::Local(index)) => Operand::local(index),
            Some(Location::Argument(index)) => Operand::argument(index),
            Some(_) => {
                let value = generator.temporary();
                let identifier = generator.intern_identifier(name);
                // NB: This runs where the body starts, before any scope around the `var` has an environment.
                let location = generator
                    .variable_environment_coordinate_for_identifier(identifier)
                    .map(BindingLocation::Environment);
                emit_get_binding_at(generator, value, identifier, location, false);
                value
            }
        };
        let location = if is_captured {
            self.in_head(|generator| {
                let first_temporary = generator.next_temporary();
                let identifier = generator.intern_identifier(name);
                let value = initial_value(generator);
                generator.emit(op::CreateVariable {
                    identifier,
                    mode: libjs_abi::EnvironmentMode::Var as u32,
                    is_immutable: false,
                    is_global: false,
                    is_strict: false,
                });
                emit_initialize_variable_binding(generator, identifier, value);
                generator.release_temporaries_from(first_temporary);
            });
            self.context.scope.environment_binding_count += 1;
            Location::Environment
        } else {
            let index = self.context.scope.local_count();
            self.context.scope.locals.push(LocalSlot {
                name,
                is_mutable: true,
                is_lexical: false,
                block_range: None,
            });
            self.in_head(|generator| {
                let first_temporary = generator.next_temporary();
                let value = initial_value(generator);
                generator.put(op::Mov {
                    dst: Operand::local(index),
                    src: value,
                });
                generator.release_temporaries_from(first_temporary);
            });
            Location::Local(index)
        };
        let scope = &mut self.context.scope;
        if let Some(parameter) = parameter
            && let Some(binding) = scope.binding_of(name)
            && scope.binding(binding).location == parameter.location
        {
            scope.bindings[binding.0 as usize].in_parameters = false;
        }
        let shadowed = scope.binding_of(name);
        scope.add_binding(Binding {
            name,
            location,
            kind: Some(DeclarationKind::Var),
            is_global: false,
            block: BlockId::default(),
            shadowed,
            is_catch_parameter: false,
            in_parameters: false,
            is_parameter: parameter.is_some_and(|parameter| parameter.is_parameter),
        })
    }

    /// A `let` or `const` of the function that something refers to before its declaration.
    fn add_early_lexical_binding(&mut self, binding: ScopeBinding) {
        let is_const = binding.kind == BindingKind::Const;
        let name = binding.name;
        let location = if binding.is_captured {
            self.in_head(|generator| {
                let identifier = generator.intern_identifier(name);
                generator.emit(op::CreateVariable {
                    identifier,
                    mode: libjs_abi::EnvironmentMode::Lexical as u32,
                    is_immutable: is_const,
                    is_global: false,
                    is_strict: is_const,
                });
            });
            self.note_lexical_environment_binding();
            Location::Environment
        } else {
            let index = self.context.scope.local_count();
            self.context.scope.locals.push(LocalSlot {
                name,
                is_mutable: !is_const,
                is_lexical: true,
                block_range: None,
            });
            Location::Local(index)
        };
        let scope = &mut self.context.scope;
        let kind = if is_const {
            DeclarationKind::Const
        } else {
            DeclarationKind::Let
        };
        scope.add_binding(Binding {
            name,
            location,
            kind: Some(kind),
            is_global: false,
            block: BlockId::default(),
            shadowed: None,
            is_catch_parameter: false,
            in_parameters: false,
            is_parameter: false,
        });
    }

    /// A lexical declaration of the function got a place in an environment, which may be one of their own.
    fn note_lexical_environment_binding(&mut self) {
        let scope = &mut self.context.scope;
        if !scope.has_lexical_environment {
            scope.environment_binding_count += 1;
        }
    }

    /// Whether the next binding that the code declares is one that nested functions capture.
    #[inline(always)]
    fn next_binding_is_captured(&mut self) -> bool {
        let scope = &mut self.context.scope;
        let bit = scope.next_bit;
        scope.next_bit += 1;
        let at = scope.bitmap_start + bit / 8;
        at < scope.bitmap_end && self.tree.script.index.summaries[at] & (1 << (bit % 8)) != 0
    }

    /// Works out what the parameters bind, and makes the `arguments` object if the code asks for one. `names` is what
    /// they bind, in the order of the source, and `shapes` says what each is unless they are all just names. Anything
    /// that takes code to get a value is up to whoever goes through the code of the parameters.
    pub(super) fn declare_parameters(&mut self, names: &[Name], shapes: &[ParameterShape]) {
        let flags = self.context.scope.flags;
        let is_simple = flags & function_flag::SIMPLE_PARAMETERS != 0;
        let mentions_arguments = flags & function_flag::ARROW == 0
            && flags & (function_flag::MIGHT_NEED_ARGUMENTS | function_flag::USES_ARGUMENTS) != 0;
        if is_simple && !mentions_arguments {
            for (index, &name) in names.iter().enumerate() {
                self.declare_parameter(name, u32_from_usize(index));
            }
            return;
        }
        let is_named_arguments = names.iter().any(|name| name.0 == N_ARGUMENTS);
        let needs_arguments_object =
            mentions_arguments && !is_named_arguments && !self.context.scope.has_lexical_arguments;
        let is_mapped = needs_arguments_object && !self.strict && is_simple;
        if !is_simple {
            self.context.scope.has_parameter_scope = true;
            self.context.scope.checks_parameters = true;
            if self.contains_direct_call_to_eval_in_non_strict_mode && shapes.iter().any(|shape| shape.has_expression) {
                // NB: What an `eval` in the parameters declares stays out of the environment of the parameters.
                self.note_creates_environment();
                self.context.scope.has_parameter_expressions_with_eval = true;
                let capacity = u32_from_usize(shapes.len());
                self.in_head(|generator| {
                    generator.push_new_lexical_environment(capacity);
                });
            }
            let mut first_name = 0;
            for (index, shape) in shapes.iter().enumerate() {
                let end = first_name + shape.name_count as usize;
                self.declare_complex_parameter(&names[first_name..end], shape.is_name, u32_from_usize(index));
                first_name = end;
            }
            if needs_arguments_object {
                self.create_arguments_object(flags, false, false);
            }
            return;
        }

        let forces_environment = is_mapped && flags & function_flag::ARGUMENTS_ACCESS != 0;
        let parameter_names = names;
        let mut names = std::mem::take(&mut self.context.scope.simple_parameters);
        names.clear();
        let mut has_duplicates = false;
        for &name in parameter_names {
            // NB: A name that comes up again is the one binding, of which the summary knows once.
            let first = names.iter().find(|&&(known, _)| known == name).copied();
            has_duplicates |= first.is_some();
            let is_captured = match first {
                Some((_, is_captured)) => is_captured,
                None => self.next_binding_is_captured() || forces_environment,
            };
            names.push((name, is_captured));
        }
        let creates_bindings = is_mapped && !names.is_empty() && names.iter().all(|&(_, is_captured)| is_captured);
        let has_environment_parameters = names.iter().any(|&(_, is_captured)| is_captured);
        if has_environment_parameters || needs_arguments_object {
            self.in_head(|generator| {
                for (index, &(name, is_captured)) in names.iter().enumerate() {
                    let is_first = !names[..index].iter().any(|&(known, _)| known == name);
                    if !is_captured || !is_first {
                        continue;
                    }
                    let identifier = generator.intern_identifier(name);
                    if creates_bindings {
                        generator.record_environment_binding_created_elsewhere(identifier);
                    } else {
                        generator.emit(op::CreateVariable {
                            identifier,
                            mode: libjs_abi::EnvironmentMode::Lexical as u32,
                            is_immutable: false,
                            is_global: false,
                            is_strict: false,
                        });
                    }
                    generator.context.scope.environment_binding_count += 1;
                }
            });
        }
        if needs_arguments_object {
            self.create_arguments_object(flags, is_mapped, creates_bindings);
        }
        if has_environment_parameters {
            self.in_head(|generator| {
                let undefined = generator.context.constants.add_undefined();
                for (index, &(name, is_captured)) in names.iter().enumerate() {
                    if !is_captured {
                        continue;
                    }
                    let identifier = generator.intern_identifier(name);
                    let is_first = !names[..index].iter().any(|&(known, _)| known == name);
                    if has_duplicates {
                        if is_first {
                            emit_initialize_lexical_binding(generator, identifier, undefined);
                        }
                        emit_set_lexical_binding(generator, identifier, Operand::argument(u32_from_usize(index)));
                    } else {
                        emit_initialize_lexical_binding(
                            generator,
                            identifier,
                            Operand::argument(u32_from_usize(index)),
                        );
                    }
                }
            });
        }
        let scope = &mut self.context.scope;
        for (index, &(name, is_captured)) in names.iter().enumerate() {
            let location = if is_captured {
                Location::Environment
            } else {
                Location::Argument(u32_from_usize(index))
            };
            scope.parameters.push((name, !is_captured));
            scope.add_binding(Binding {
                name,
                location,
                kind: None,
                is_global: false,
                block: BlockId::default(),
                shadowed: None,
                is_catch_parameter: false,
                in_parameters: false,
                is_parameter: false,
            });
        }
        scope.has_mapped_parameters = is_mapped;
        scope.simple_parameters = names;
    }

    /// A parameter that is just a name, in a function that does not care about what that means for `arguments`.
    pub(super) fn declare_parameter(&mut self, name: Name, index: u32) {
        let first = self.context.scope.binding_of(name);
        let is_duplicate = first.is_some();
        // NB: A name that comes up again is the one binding, of which the summary knows once.
        let is_captured = match first {
            Some(first) => self.context.scope.binding(first).location == Location::Environment,
            None => self.next_binding_is_captured(),
        };
        let location = if is_captured {
            self.in_head(|generator| {
                let identifier = generator.intern_identifier(name);
                if is_duplicate {
                    emit_set_lexical_binding(generator, identifier, Operand::argument(index));
                    return;
                }
                generator.emit(op::CreateVariable {
                    identifier,
                    mode: libjs_abi::EnvironmentMode::Lexical as u32,
                    is_immutable: false,
                    is_global: false,
                    is_strict: false,
                });
                emit_initialize_lexical_binding(generator, identifier, Operand::argument(index));
            });
            if !is_duplicate {
                self.context.scope.environment_binding_count += 1;
            }
            Location::Environment
        } else {
            Location::Argument(index)
        };
        let scope = &mut self.context.scope;
        scope.parameters.push((name, !is_captured));
        scope.add_binding(Binding {
            name,
            location,
            kind: None,
            is_global: false,
            block: BlockId::default(),
            shadowed: None,
            is_catch_parameter: false,
            in_parameters: false,
            is_parameter: false,
        });
    }

    /// A parameter that is more than a name: everything it binds exists from the start, and has no value yet.
    fn declare_complex_parameter(&mut self, names: &[Name], is_name: bool, index: u32) {
        let argument = is_name.then_some(index);
        for &name in names {
            self.declare_complex_parameter_name(name, argument);
        }
        if argument.is_none() {
            let scope = &mut self.context.scope;
            scope.parameters.push((Name(N_DOT_UNBOUND), false));
        }
    }

    fn declare_complex_parameter_name(&mut self, name: Name, argument: Option<u32>) {
        let is_captured = self.next_binding_is_captured();
        let location = if is_captured {
            self.in_head(|generator| {
                let identifier = generator.intern_identifier(name);
                generator.emit(op::CreateVariable {
                    identifier,
                    mode: libjs_abi::EnvironmentMode::Lexical as u32,
                    is_immutable: false,
                    is_global: false,
                    is_strict: false,
                });
            });
            self.context.scope.environment_binding_count += 1;
            Location::Environment
        } else if let Some(index) = argument {
            Location::Argument(index)
        } else {
            let index = self.context.scope.local_count();
            self.context.scope.locals.push(LocalSlot {
                name,
                is_mutable: true,
                is_lexical: true,
                block_range: None,
            });
            Location::Local(index)
        };
        let scope = &mut self.context.scope;
        if argument.is_some() {
            scope.parameters.push((name, !is_captured));
        }
        scope.add_binding(Binding {
            name,
            location,
            kind: None,
            is_global: false,
            block: BlockId::default(),
            shadowed: None,
            is_catch_parameter: false,
            in_parameters: true,
            is_parameter: true,
        });
    }

    /// The arguments object of a static block, which FunctionDeclarationInstantiation gives it like any other function.
    /// `arguments` is an early error in the block, so only an `eval` can see it.
    pub(super) fn create_arguments_object_of_static_block(&mut self) {
        let flags = self.context.scope.flags;
        if flags & function_flag::CONTAINS_EVAL != 0 {
            self.create_arguments_object(flags, false, false);
        }
    }

    fn create_arguments_object(&mut self, flags: u32, is_mapped: bool, creates_bindings: bool) {
        let name = Name(N_ARGUMENTS);
        let is_captured = flags & (function_flag::CAPTURES_ARGUMENTS | function_flag::CONTAINS_EVAL) != 0
            || self.context.scope.var_of_arguments_is_captured;
        let is_strict = self.strict;
        let kind = if is_mapped {
            ArgumentsKind::Mapped
        } else {
            ArgumentsKind::Unmapped
        };
        let location = if is_captured {
            self.context.scope.environment_binding_count += 1;
            Location::Environment
        } else {
            let index = self.context.scope.local_count();
            self.context.scope.locals.push(LocalSlot {
                name,
                is_mutable: true,
                is_lexical: false,
                block_range: None,
            });
            Location::Local(index)
        };
        let dst = match location {
            Location::Local(index) => Some(Operand::local(index)),
            _ => None,
        };
        self.in_head(|generator| {
            generator.emit(op::CreateArguments {
                dst,
                kind: kind as u32,
                is_immutable: is_strict,
                creates_parameter_bindings: creates_bindings,
            });
        });
        let in_parameters = self.context.scope.has_parameter_scope;
        self.context.scope.add_binding(Binding {
            name,
            location,
            kind: Some(DeclarationKind::Var),
            is_global: false,
            block: BlockId::default(),
            shadowed: None,
            is_catch_parameter: false,
            in_parameters,
            is_parameter: false,
        });
    }

    /// The code of the parameters is done, and nothing looks at them in their dead zone anymore. What the body starts with
    /// comes after this. If something could see the bindings of the body from the parameters, they get an environment.
    pub(super) fn end_of_parameters(&mut self) {
        let scope = &mut self.context.scope;
        scope.checks_parameters = false;
        scope.is_in_parameters = false;
        let Some((early_start, early_end)) = scope.deferred_early_bindings.take() else {
            return;
        };
        let has_parameter_scope = scope.has_parameter_scope;
        if has_parameter_scope {
            self.begin_body_head();
        }
        let captured_count = self.captured_bindings_of_body(early_start, early_end);
        let scope = &self.context.scope;
        let parameters_have_environment = scope.environment_binding_count != 0;
        let needs_variable_environment = scope.has_parameter_expressions_with_eval
            || (has_parameter_scope
                && captured_count != 0
                && (scope.has_function_in_parameters || parameters_have_environment));
        if needs_variable_environment {
            self.note_creates_environment();
            let environment = ScopedOperand::plain(self.held_register());
            self.in_head(|generator| {
                generator.emit(op::CreateVariableEnvironment {
                    capacity: captured_count,
                });
                generator.emit(op::GetLexicalEnvironment {
                    dst: environment.operand(),
                });
            });
            self.push_static_variable_environment(environment);
            let scope = &mut self.context.scope;
            scope.function_environment_binding_count = Some(scope.environment_binding_count);
        }
        if self.contains_direct_call_to_eval_in_non_strict_mode && captured_count != 0 {
            // NB: The lexical declarations are apart from the variables, for a `var` from an `eval` to clash with.
            self.note_creates_environment();
            self.in_head(|generator| {
                generator.push_new_lexical_environment(captured_count);
            });
            self.context.scope.has_lexical_environment = true;
        }
        self.declare_early_bindings(early_start, early_end);
    }

    /// How many bindings of the function the summary has left that functions nested in it capture.
    fn captured_bindings_of_body(&self, mut early_start: usize, early_end: usize) -> u32 {
        let summaries = &self.tree.script.index.summaries;
        let scope = &self.context.scope;
        let mut count = 0;
        while early_start < early_end {
            count += read_varint(summaries, &mut early_start) & 1;
        }
        let first_byte = scope.bitmap_start + scope.next_bit / 8;
        if first_byte < scope.bitmap_end {
            count += (summaries[first_byte] & 0xFFu8 << (scope.next_bit % 8)).count_ones();
            count += summaries[first_byte + 1..scope.bitmap_end]
                .iter()
                .map(|byte| byte.count_ones())
                .sum::<u32>();
        }
        count
    }

    #[inline(always)]
    pub(super) fn checks_parameters(&self) -> bool {
        self.context.scope.checks_parameters
    }

    /// A declaration of `name` came up.
    pub(super) fn declare_binding(&mut self, name: Name, kind: DeclarationKind) -> Identifier {
        let existing = self.context.scope.binding_of(name);
        let block = self.context.scope.current_block;
        if self.context.scope.program.is_some() {
            // NB: Declaration instantiation sees to what the top level declares.
            if kind == DeclarationKind::Var {
                let mut ident = self.identifier_of(name);
                ident.declaration_kind = ident.declaration_kind.or(Some(DeclarationKind::Var));
                return ident;
            }
            if block == BlockId::default() {
                return Identifier {
                    name,
                    local_type: None,
                    local_index: 0,
                    is_global: false,
                    declaration_kind: Some(kind),
                };
            }
        }
        if kind == DeclarationKind::Var {
            return self.declare_var(name, existing);
        }
        if let Some(index) = existing {
            let binding = self.context.scope.binding(index);
            // NB: This one got its place when the code around it started.
            if binding.block == block && binding.location != Location::Outer && binding.kind == Some(kind) {
                return self.identifier_of_binding(index);
            }
        }
        let is_const = kind == DeclarationKind::Const;
        let is_captured = block == BlockId::default() && self.next_binding_is_captured();
        let location = if is_captured {
            self.in_head(|generator| {
                let identifier = generator.intern_identifier(name);
                generator.emit(op::CreateVariable {
                    identifier,
                    mode: libjs_abi::EnvironmentMode::Lexical as u32,
                    is_immutable: is_const,
                    is_global: false,
                    is_strict: is_const,
                });
            });
            self.note_lexical_environment_binding();
            Location::Environment
        } else {
            let index = self.context.scope.local_count();
            self.context.scope.locals.push(LocalSlot {
                name,
                is_mutable: !is_const,
                is_lexical: true,
                block_range: None,
            });
            Location::Local(index)
        };
        let scope = &mut self.context.scope;
        let index = scope.add_binding(Binding {
            name,
            location,
            kind: Some(kind),
            is_global: false,
            block,
            shadowed: existing,
            is_catch_parameter: false,
            in_parameters: false,
            is_parameter: false,
        });
        self.identifier_of_binding(index)
    }

    /// A `var` goes to the function, whatever scope it comes up in. What it writes is what its name means at that point,
    /// which is a parameter of a `catch` around it if there is one of the same name.
    fn declare_var(&mut self, name: Name, existing: Option<BindingId>) -> Identifier {
        let mut innermost_inner_binding = None;
        let mut next = existing;
        while let Some(index) = next {
            let binding = self.context.scope.binding(index);
            if binding.block == BlockId::default() {
                if binding.location != Location::Outer {
                    if binding.in_parameters {
                        let is_captured = self.next_binding_is_captured();
                        let var = self.add_var_of_parameter(index, is_captured);
                        let Some(inner) = innermost_inner_binding else {
                            return self.identifier_of_binding(var);
                        };
                        let scope = &mut self.context.scope;
                        let inner: BindingId = inner;
                        scope.bindings[inner.0 as usize].shadowed = Some(var);
                        let generation = scope.generation;
                        *scope.entry(name) = NameEntry {
                            generation,
                            binding: existing,
                        };
                    }
                    return self.identifier_of_binding(existing.unwrap_or(index));
                }
                break;
            }
            innermost_inner_binding = Some(index);
            next = binding.shadowed;
        }
        let is_captured = self.next_binding_is_captured();
        let index = self.add_var_binding(name, is_captured, None);
        let Some(last_inner_binding) = innermost_inner_binding else {
            return self.identifier_of_binding(index);
        };
        let scope = &mut self.context.scope;
        scope.bindings[last_inner_binding.0 as usize].shadowed = Some(index);
        let generation = scope.generation;
        let innermost = existing.expect("something was found");
        *scope.entry(name) = NameEntry {
            generation,
            binding: Some(innermost),
        };
        self.identifier_of_binding(innermost)
    }

    #[inline(always)]
    fn identifier_of_binding(&mut self, id: BindingId) -> Identifier {
        let binding = self.context.scope.binding(id);
        let (local_type, local_index) = match binding.location {
            Location::Local(index) => (Some(LocalType::Variable), index),
            Location::Argument(index) => (Some(LocalType::Argument), index),
            Location::Environment | Location::Outer => (None, 0),
        };
        Identifier {
            name: binding.name,
            local_type,
            local_index,
            is_global: binding.is_global,
            declaration_kind: binding.kind,
        }
    }

    /// What a reference to `name` is at this point of the code.
    #[inline(always)]
    pub(super) fn identifier_of(&mut self, name: Name) -> Identifier {
        let index = match self.context.scope.binding_of(name) {
            Some(index) => index,
            None => self.resolve_free_name(name),
        };
        let mut ident = self.identifier_of_binding(index);
        if self.context.scope.with_depth != 0 {
            ident.is_global = false;
        }
        ident
    }

    /// What a name means that nothing binds at the top level of a program.
    fn resolve_free_name_of_program(&mut self, name: Name, kind: ProgramKind) -> BindingId {
        let is_global = match kind {
            ProgramKind::WithoutGlobals => false,
            ProgramKind::Script => !self.has_direct_eval() && !self.tree.script.is_global_name_used_in_with(name),
            ProgramKind::Module => {
                let identifier = self.intern_identifier(name);
                !self.has_direct_eval()
                    && !self.tree.script.is_global_name_used_in_with(name)
                    && self.binding_location_for_identifier(identifier).is_none()
            }
        };
        self.context.scope.add_binding(Binding {
            name,
            location: Location::Outer,
            kind: None,
            is_global,
            block: BlockId::default(),
            shadowed: None,
            is_catch_parameter: false,
            in_parameters: false,
            is_parameter: false,
        })
    }

    fn resolve_free_name(&mut self, name: Name) -> BindingId {
        if let Some(kind) = self.context.scope.program {
            return self.resolve_free_name_of_program(name, kind);
        }
        let tree = self.tree;
        let script = &tree.script;
        let outer = if name.0 == N_ARGUMENTS
            || script.index.is_declared_anywhere(name.0)
            || script.is_global_name_used_in_with(name)
        {
            self.enclosing_code
                .resolution_in_function(script, self.context.scope.row, name)
        } else {
            self.unbound_resolution()
        };
        // NB: The name a function expression has for itself is in an environment that goes unnoticed from out there.
        let is_own_name = self.is_own_function_name(name);
        let is_global =
            outer.is_global() && !outer.is_inside_scope_with_eval() && !is_own_name && !self.has_direct_eval();
        self.context.scope.add_binding(Binding {
            name,
            location: Location::Outer,
            kind: outer.declaration_kind(),
            is_global,
            block: BlockId::default(),
            shadowed: None,
            is_catch_parameter: false,
            in_parameters: false,
            is_parameter: false,
        })
    }

    fn unbound_resolution(&mut self) -> Resolution {
        if let Some(unbound) = self.context.scope.unbound {
            return unbound;
        }
        let row = self.context.scope.row;
        let unbound = self
            .enclosing_code
            .resolution_in_function(&self.tree.script, row, Name(N_DOT_UNBOUND));
        self.context.scope.unbound = Some(unbound);
        unbound
    }

    /// Whether an `eval` in the code can make any name mean something else.
    fn has_direct_eval(&self) -> bool {
        self.context.scope.flags & function_flag::DIRECT_EVAL != 0
    }

    fn is_own_function_name(&self, name: Name) -> bool {
        let scope = &self.context.scope;
        scope.flags & function_flag::NAMED_EXPRESSION != 0 && self.tree.script.index.name[scope.row as usize] == name.0
    }

    /// A function declaration, at the top level of the function.
    pub(super) fn function_declaration(&mut self, name: Name, row: u32) {
        if self.context.scope.program.is_some() {
            return;
        }
        let ident = self.declare_binding(name, DeclarationKind::Var);
        let index = self.register_nested_function(row, NestedFunctionName::None);
        self.in_head(|generator| {
            if ident.local_type == Some(LocalType::Variable) {
                generator.put(op::NewFunction {
                    dst: Operand::local(ident.local_index),
                    shared_function_data_index: index,
                    home_object: None,
                    lhs_name: None,
                });
            } else if ident.local_type == Some(LocalType::Argument) {
                generator.put(op::NewFunction {
                    dst: Operand::argument(ident.local_index),
                    shared_function_data_index: index,
                    home_object: None,
                    lhs_name: None,
                });
            } else {
                let function = generator.temporary();
                generator.put(op::NewFunction {
                    dst: function,
                    shared_function_data_index: index,
                    home_object: None,
                    lhs_name: None,
                });
                let identifier = generator.intern_identifier(name);
                emit_set_variable_binding(generator, identifier, function);
            }
        });
    }

    fn register_nested_function(&mut self, row: u32, name: NestedFunctionName) -> u32 {
        let is_in_block = self.context.scope.is_in_block();
        let scope = &mut self.context.scope;
        scope.has_function_in_parameters |= scope.is_in_parameters;
        let is_eager = std::mem::take(&mut scope.next_function_is_eager);
        // NB: What the functions of a program see is known from the first parse.
        if scope.program.is_none() {
            if scope.with_depth != 0 {
                self.note_resolutions_in_with(row);
            } else if is_in_block {
                self.note_scope_resolutions(row);
            }
        }
        // NB: What a function in the parameters sees is what is there by now, whatever environment the body gets.
        let scope = &self.context.scope;
        let environment = (scope.scope_environment_depth != 0 || scope.is_in_parameters)
            .then(|| self.capture_environment_coordinate_scopes())
            .flatten();
        let nested_functions = &mut self.context.scope.nested_functions;
        nested_functions.push(NestedFunction {
            row,
            name,
            environment,
            is_eager,
        });
        u32_from_usize(nested_functions.len()) - 1
    }

    /// Whatever the function at `row` refers to goes through a `with` around it, so that nothing is known of any name.
    fn note_resolutions_in_with(&mut self, row: u32) {
        let index = &self.tree.script.index;
        let captures = index.capture_start[row as usize] as usize;
        let capture_count = index.capture_count[row as usize] as usize;
        let mut resolution = Resolution::default();
        resolution.set_is_inside_scope_with_eval();
        let names = index.capture_names[captures..captures + capture_count]
            .iter()
            .map(|&capture| Name(capture & !crate::parser::base::USE_IN_WITH))
            .chain(std::iter::once(Name(N_DOT_UNBOUND)));
        for name in names {
            self.context
                .scope
                .scope_resolutions
                .push(crate::parser::resolve::outer_entry(row, name, resolution));
        }
    }

    /// What the function at `row` captures of scopes inside this function has to be noted while those are around.
    fn note_scope_resolutions(&mut self, row: u32) {
        let index = &self.tree.script.index;
        let captures = index.capture_start[row as usize] as usize;
        let capture_count = index.capture_count[row as usize] as usize;
        for &capture in &index.capture_names[captures..captures + capture_count] {
            let name = Name(capture & !crate::parser::base::USE_IN_WITH);
            let scope = &mut self.context.scope;
            let Some(binding) = scope.binding_of(name) else {
                continue;
            };
            let binding = scope.binding(binding);
            if binding.block == BlockId::default() {
                continue;
            }
            let mut resolution = Resolution::default();
            if let Some(kind) = binding.kind {
                resolution.set_declaration_kind(kind);
            }
            scope
                .scope_resolutions
                .push(crate::parser::resolve::outer_entry(row, name, resolution));
        }
    }

    /// The function at `row`, which takes `name` if it has one and whose `super` goes by `home_object` if it has one.
    pub(super) fn value_of_function_with_text_name(
        &mut self,
        row: u32,
        name: Option<IdentifierTableIndex>,
        home_object: Option<Operand>,
    ) {
        let index = self.register_nested_function(
            row,
            name.map_or(NestedFunctionName::None, NestedFunctionName::Identifier),
        );
        self.value_of_function(index, name, home_object);
    }

    /// The function expression at `row`, which sees `name` as a binding of its own, in an environment only it sees.
    pub(super) fn value_of_named_function_at_row(&mut self, row: u32, name: Name) {
        self.note_creates_environment();
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        let parent = self.current_lexical_environment();
        let held = self.hold_registers();
        let register = self.held_register();
        let shape_cache = self.next_environment_shape_cache();
        self.emit(op::CreateLexicalEnvironment {
            dst: register,
            parent: parent.operand(),
            capacity: 1,
            shape_cache,
            is_catch_environment: false,
        });
        self.push_static_lexical_environment(ScopedOperand::plain(register));
        self.context.scope.scope_environment_depth += 1;
        let identifier = self.intern_identifier(name);
        self.emit(op::CreateVariable {
            identifier,
            mode: libjs_abi::EnvironmentMode::Lexical as u32,
            is_immutable: true,
            is_global: false,
            is_strict: false,
        });
        let shared_function_data_index = self.register_nested_function(row, NestedFunctionName::None);
        self.put(op::NewFunction {
            dst,
            shared_function_data_index,
            lhs_name: None,
            home_object: None,
        });
        emit_initialize_lexical_binding(self, identifier, dst);
        self.leave_scope_environment(ScopeEnvironment {
            register,
            parent: parent.operand(),
            held,
        });
        self.push_value(dst, first_temporary);
    }

    /// `name` is what a `catch` clause calls its value.
    pub(super) fn note_catch_parameter(&mut self, name: Name) {
        let scope = &mut self.context.scope;
        if let Some(binding) = scope.binding_of(name) {
            scope.bindings[binding.0 as usize].is_catch_parameter = true;
        }
    }

    /// A function that a block declares, which exists from the start of the block, at `row` of the index. Only a later
    /// one of a name finds that the binding has a value.
    pub(super) fn instantiate_function_of_block(&mut self, name: Name, row: u32, is_first_of_name: bool) {
        let existing = self.context.scope.binding_of(name);
        let scope = &mut self.context.scope;
        let block = scope.current_block;
        let binding = match existing.filter(|&binding| scope.binding(binding).block == block) {
            Some(binding) => binding,
            None => {
                let index = scope.local_count();
                scope.locals.push(LocalSlot {
                    name,
                    is_mutable: true,
                    is_lexical: false,
                    block_range: None,
                });
                scope.add_binding(Binding {
                    name,
                    location: Location::Local(index),
                    kind: Some(DeclarationKind::Let),
                    is_global: false,
                    block,
                    shadowed: existing,
                    is_catch_parameter: false,
                    in_parameters: false,
                    is_parameter: false,
                })
            }
        };
        let ident = self.identifier_of_binding(binding);
        let index = self.register_nested_function(row, NestedFunctionName::None);
        if ident.is_local() {
            self.put(op::NewFunction {
                dst: Operand::local(ident.local_index),
                shared_function_data_index: index,
                lhs_name: None,
                home_object: None,
            });
            self.mark_local_initialized(ident.local_index);
        } else {
            let dst = self.temporary();
            self.put(op::NewFunction {
                dst,
                shared_function_data_index: index,
                lhs_name: None,
                home_object: None,
            });
            let identifier = self.intern_identifier(name);
            if is_first_of_name {
                emit_initialize_lexical_binding(self, identifier, dst);
            } else {
                emit_set_lexical_binding(self, identifier, dst);
            }
            self.end_of_statement();
        }
    }

    /// A function declaration of a block comes up, which got evaluated when the block started. In code that is not strict
    /// a `var` of the function gets the value from here on, unless a lexical binding is in the way.
    pub(super) fn function_declaration_in_block(&mut self, name: Name) {
        let scope = &mut self.context.scope;
        let block = scope.current_block;
        let inner = scope
            .binding_of(name)
            .filter(|&binding| scope.binding(binding).block == block)
            .expect("the block binds its functions");
        if self.strict {
            return;
        }
        // NB: Only the parameter of a `catch` is no reason for a function not to hoist out of a block.
        let mut next = self.context.scope.binding(inner).shadowed;
        while let Some(index) = next {
            let binding = self.context.scope.binding(index);
            if binding.block == BlockId::default() || !binding.is_catch_parameter {
                break;
            }
            next = binding.shadowed;
        }
        if self.context.scope.program.is_some() {
            // NB: Which names hoist at all is known from the first parse.
            let is_in_the_way = next.is_some_and(|index| self.context.scope.binding(index).block != BlockId::default());
            if !is_in_the_way && self.context.annexb_function_names.contains(&name) {
                let inner_ident = self.identifier_of_binding(inner);
                self.value_of_identifier(inner_ident);
                let (value, _) = self.pop_operand();
                let identifier = self.intern_identifier(name);
                // NB: Whether the function gets stored is decided again when it is evaluated.
                let program_environment_depth = self.context.scope.scope_environment_depth;
                self.put(op::SetVariableBindingOfBlockFunction {
                    identifier,
                    src: value,
                    program_environment_depth,
                });
                self.end_of_statement();
            }
            return;
        }
        let Some(outer) = next else {
            return;
        };
        let outer_binding = self.context.scope.binding(outer);
        if outer_binding.block != BlockId::default()
            || outer_binding.location == Location::Outer
            || outer_binding.is_parameter
            || !matches!(outer_binding.kind, Some(DeclarationKind::Var))
        {
            return;
        }
        let inner_ident = self.identifier_of_binding(inner);
        self.value_of_identifier(inner_ident);
        let (value, _) = self.pop_operand();
        let outer_ident = self.identifier_of_binding(outer);
        if outer_ident.is_local() {
            self.store_declared(outer_ident, value, DeclarationKind::Var);
        } else {
            let identifier = self.intern_identifier(name);
            emit_set_variable_binding(self, identifier, value);
        }
        self.end_of_statement();
    }

    /// The initializer of a field of a class, which `NewClass` makes from its index.
    pub(super) fn register_field_initializer(
        &mut self,
        row: u32,
        name: Option<ak::Utf16String>,
        is_private: bool,
    ) -> u32 {
        self.register_nested_function(row, NestedFunctionName::Field(name, is_private))
    }

    /// The function at `row` of a class, which `NewClass` makes from its index.
    pub(super) fn register_function_of_class(&mut self, row: u32) -> u32 {
        self.register_nested_function(row, NestedFunctionName::None)
    }

    /// The scope of a class starts, whose name is a binding of its own that everything in the class sees.
    pub(super) fn begin_class_scope(&mut self, name: Option<Name>) -> ScopeEnvironment {
        self.note_creates_environment();
        let parent = self.current_lexical_environment().operand();
        let held = self.hold_registers();
        let register = self.held_register();
        let shape_cache = self.next_environment_shape_cache();
        self.emit(op::CreateLexicalEnvironment {
            dst: register,
            parent,
            capacity: u32::from(name.is_some()),
            shape_cache,
            is_catch_environment: false,
        });
        self.push_static_lexical_environment(ScopedOperand::plain(register));
        let scope = &mut self.context.scope;
        scope.scope_environment_depth += 1;
        scope.block_begin();
        if let Some(name) = name {
            let identifier = self.intern_identifier(name);
            self.emit(op::CreateVariable {
                identifier,
                mode: libjs_abi::EnvironmentMode::Lexical as u32,
                is_immutable: true,
                is_global: false,
                is_strict: true,
            });
            let scope = &mut self.context.scope;
            let shadowed = scope.binding_of(name);
            let block = scope.current_block;
            scope.add_binding(Binding {
                name,
                location: Location::Environment,
                kind: Some(DeclarationKind::Const),
                is_global: false,
                block,
                shadowed,
                is_catch_parameter: false,
                in_parameters: false,
                is_parameter: false,
            });
        }
        let environment = ScopeEnvironment { register, parent, held };
        self.push_control(Control::Block {
            environment: Some(environment),
        });
        environment
    }

    /// The scope of a class ends, whose environment lives on in what the class is made of.
    pub(super) fn end_class_scope(&mut self, environment: ScopeEnvironment) {
        let Some(Control::Block { environment: Some(_) }) = self.pop_control() else {
            unreachable!("the scope of a class is what is going on");
        };
        self.pop_tracked_lexical_environment();
        let scope = &mut self.context.scope;
        scope.scope_environment_depth -= 1;
        scope.block_end();
        let parent = self.scoped_operand(environment.parent);
        self.restore_lexical_environment(&parent);
    }

    /// Finishes the function: what the functions nested in it need, what the function object needs, and the code.
    pub(super) fn finish_function(&mut self) -> PrecompiledFunction {
        let tree: &SyntaxTree = self.tree;
        let script = &tree.script;
        let index = &script.index;
        let row = self.context.scope.row;
        let flags = self.context.scope.flags;
        let is_program = self.context.scope.program.is_some();
        let has_direct_eval = self.has_direct_eval();
        let contains_eval = flags & function_flag::CONTAINS_EVAL != 0;

        // NB: All bindings are known by now, so that all nested functions share what they see of this function.
        let enclosing_environment_scope = self.capture_environment_coordinate_scopes();
        self.end_environment_coordinate_scopes();
        let nested_count = self.context.scope.nested_functions.len();
        let enclosing_code = if nested_count == 0 || is_program {
            self.enclosing_code.clone()
        } else {
            let unbound = self.unbound_resolution();
            let mut entries = std::mem::take(&mut self.context.scope.scope_resolutions);
            for nested in 0..nested_count {
                let nested_row = self.context.scope.nested_functions[nested].row;
                let captures = index.capture_start[nested_row as usize] as usize;
                let capture_count = index.capture_count[nested_row as usize] as usize;
                for &capture in &index.capture_names[captures..captures + capture_count] {
                    let name = Name(capture & !crate::parser::base::USE_IN_WITH);
                    if matches!(name.0, N_DOT_THIS | N_DOT_NEW_TARGET | N_DOT_SUPER) {
                        continue;
                    }
                    if self.is_own_function_name(name) {
                        let mut resolution = Resolution::default();
                        resolution.set_is_inside_scope_with_eval();
                        entries.push(crate::parser::resolve::outer_entry(nested_row, name, resolution));
                        continue;
                    }
                    let scope = &mut self.context.scope;
                    let binding = scope
                        .binding_of(name)
                        .map(|binding| scope.binding(binding))
                        .filter(|binding| binding.location != Location::Outer);
                    let Some(binding) = binding else {
                        if has_direct_eval {
                            let mut resolution = Resolution::default();
                            resolution.set_is_inside_scope_with_eval();
                            entries.push(crate::parser::resolve::outer_entry(nested_row, name, resolution));
                        }
                        continue;
                    };
                    let mut resolution = Resolution::default();
                    if let Some(kind) = binding.kind {
                        resolution.set_declaration_kind(kind);
                    }
                    entries.push(crate::parser::resolve::outer_entry(nested_row, name, resolution));
                }
                let mut resolution = Resolution::default();
                if unbound.is_inside_scope_with_eval() || has_direct_eval {
                    resolution.set_is_inside_scope_with_eval();
                }
                if has_direct_eval {
                    resolution.set_is_inside_tree_scope_with_eval();
                }
                if unbound.is_global() && !unbound.is_inside_scope_with_eval() && !has_direct_eval {
                    resolution.set_is_global();
                }
                entries.push(crate::parser::resolve::outer_entry(
                    nested_row,
                    Name(N_DOT_UNBOUND),
                    resolution,
                ));
            }
            let enclosing_code = EnclosingCode::of_function(row, &mut entries, &self.enclosing_code, has_direct_eval);
            self.context.scope.scope_resolutions = entries;
            enclosing_code
        };
        self.seal_stream();
        // SAFETY: The slices and the block end up in an ExecutableData, which drops the slices first.
        let mut link_block = unsafe {
            SliceBlock::builder(
                SliceBlock::room_for::<PendingSharedFunctionData>(nested_count)
                    + SliceBlock::room_for::<PendingClassBlueprint>(self.class_blueprints.len())
                    + SliceBlock::room_for::<ConstantValue>(self.context.constants.values.len())
                    + self.context.names.room_for_link_tables(),
            )
        };
        let shared_function_data = link_block.slice((0..nested_count).map(|nested| {
            let nested_function = &mut self.context.scope.nested_functions[nested];
            let row = nested_function.row;
            let is_eager = nested_function.is_eager;
            let name = std::mem::replace(&mut nested_function.name, NestedFunctionName::None);
            let environment = nested_function
                .environment
                .take()
                .or_else(|| enclosing_environment_scope.clone());
            let mut field_name = None;
            let name_override = match name {
                NestedFunctionName::None => None,
                NestedFunctionName::Identifier(name) => Some(self.context.names.identifier_fly_string(tree, name)),
                NestedFunctionName::Field(name, is_private) => {
                    field_name = name.map(|name| (name, is_private));
                    Some(ak::Utf16FlyString::from_utf8("field"))
                }
            };
            let own_name = index.name[row as usize];
            let is_method = index.flags[row as usize] & function_flag::METHOD != 0;
            let name = name_override.or_else(|| {
                (own_name != NONE && !is_method).then(|| self.context.names.fly_string_of(tree, Name(own_name)))
            });
            let mut pending = crate::compile::pending_function_of_row(script, row, name, environment, &enclosing_code);
            pending.class_field_initializer_name = field_name;
            pending.should_eager_compile = is_eager;
            pending
        }));

        let scope = &self.context.scope;
        let variable_name_count = scope.locals.len() + scope.parameters.len();
        let other_tables_size = self.context.names.room_for_tables()
            + SliceBlock::room_for::<ak::Utf16FlyString>(variable_name_count)
            + SliceBlock::room_for::<LocalVariableMetadata>(scope.locals.len());
        let (assembled, mut block) = self.finish_stream(other_tables_size);
        let (identifier_table, property_key_table, string_table) =
            self.context.names.take_tables(tree, &mut block, &mut link_block);
        let context = &mut *self.context;
        let constants = link_block.slice(context.constants.values.drain(..));
        let scope = &context.scope;
        let local_variable_metadata = block.slice(scope.locals.iter().map(|local| {
            let (start, end) = local
                .block_range
                .unwrap_or((SourceOffset::NONE.0, SourceOffset::NONE.0));
            LocalVariableMetadata {
                is_mutable: local.is_mutable,
                scope_start: SourceOffset(start),
                scope_end: SourceOffset(end),
            }
        }));
        let local_names = scope.locals.iter().map(|local| (local.name, true));
        let variable_names = block.slice_of_length(
            variable_name_count,
            local_names
                .chain(scope.parameters.iter().copied())
                .map(|(name, has_name)| {
                    if has_name {
                        context.names.fly_string_of(tree, name)
                    } else {
                        ak::Utf16FlyString::default()
                    }
                }),
        );
        let environment_binding_count = scope.environment_binding_count;
        let function_environment_binding_count = scope
            .function_environment_binding_count
            .unwrap_or(environment_binding_count);
        let has_mapped_parameters = scope.has_mapped_parameters;
        let has_function_environment = scope.has_function_environment;
        assert!(has_function_environment || environment_binding_count == 0 || is_program);
        let parameter_names: Box<[ak::Utf16FlyString]> = if has_mapped_parameters {
            let names = &context.scope.simple_parameters;
            names
                .iter()
                .map(|&(name, _)| context.names.fly_string_of(tree, name))
                .collect()
        } else {
            Box::default()
        };
        let executable = ExecutableData {
            bytecode: assembled.bytecode,
            exception_handlers: assembled.exception_handlers,
            source_map: assembled.source_map,
            number_of_registers: assembled.number_of_registers,
            number_of_arguments: assembled.number_of_arguments,
            is_strict: self.strict,
            cache_counts: self.cache_counts(),
            identifier_table,
            string_table,
            variable_names,
            local_variable_metadata,
            length_identifier: self.length_identifier,
            property_key_table,
            constants,
            shared_function_data,
            class_blueprints: link_block.slice(std::mem::take(&mut self.class_blueprints).into_iter()),
            position_source: script.positions.clone(),
            tables: block.finish(),
            link_tables: link_block.finish(),
        };
        let this_from_environment = this_comes_from_environment(flags);
        PrecompiledFunction {
            executable: Arc::new(executable),
            metadata: FunctionSfdMetadata {
                parameter_names: if has_mapped_parameters {
                    parameter_names
                } else {
                    Box::default()
                },
                uses_this: this_from_environment
                    || flags & (function_flag::SCOPE_USES_THIS | function_flag::INSIGHT_USES_THIS) != 0,
                this_value_needs_environment_resolution: this_from_environment,
                function_environment_needed: has_function_environment,
                creates_environment: self.creates_environment,
                function_environment_bindings_count: function_environment_binding_count as usize,
                var_environment_bindings_count: (environment_binding_count - function_environment_binding_count)
                    as usize,
                might_need_arguments: flags & function_flag::MIGHT_NEED_ARGUMENTS != 0,
                contains_eval,
            },
        }
    }
}

/// Reads the part of a summary that says what the scopes in the code bind, which starts at `at` of `summary`, which is at
/// `summary_start` of the pool.
fn read_scope_bindings(scopes: &mut Vec<ScopeBindings>, summary: &[u8], summary_start: usize, at: &mut usize) {
    let scope_count = read_varint(summary, at);
    let mut position = 0;
    for _ in 0..scope_count {
        position += read_varint(summary, at);
        let count = read_varint(summary, at);
        let start = u32_from_usize(summary_start + *at);
        let mut captured_count = 0;
        for _ in 0..count {
            captured_count += read_varint(summary, at) & 1;
        }
        scopes.push(ScopeBindings {
            position,
            start,
            count,
            captured_count,
        });
    }
}
