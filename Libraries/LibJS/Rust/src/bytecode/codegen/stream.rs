/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The emission core of the bytecode generator: one byte stream, jump labels, and registers with stack discipline.
//!
//! The sweep walks code in evaluation order and calls the functions here, one per thing that it comes across. The core
//! keeps a stack of what was evaluated so far. Locals, arguments and constants stay on it as operands and are never
//! copied to read them, and a temporary is the next free register, which goes back when whatever consumes the value
//! is done with it. There are no handles and nothing has to run when a value dies.
//!
//! A local that sits on the stack can be overwritten before it gets consumed, as in `x + (x = 1)`. Locals are never
//! captured, so that only a write in the code being generated can do that: a write takes a copy for what holds the local
//! on the stack right before it happens. A copy made in one arm of a branch would not happen in the other arm, so
//! before a branch inside an expression every local on the stack gets copied.

use crate::ast::BinaryOp;
use crate::ast::DeclarationKind;
use crate::ast::LocalType;
use crate::ast::Name;
use crate::ast::UnaryOp;
use crate::ast::UpdateOp;
use crate::bytecode::constant_fold::constant_to_boolean;
use crate::bytecode::constant_fold::try_constant_fold_binary;
use crate::bytecode::constant_fold::try_constant_fold_unary;
use crate::bytecode::executable_data::ConstantValue;
use crate::bytecode::executable_data::ExceptionHandler;
use crate::bytecode::instruction::Instruction;
use crate::bytecode::instruction::specialize_instruction;
use crate::bytecode::instruction::typed as op;
use crate::bytecode::instruction::typed::TypedInstruction;
use crate::bytecode::operand::*;
use crate::bytecode::source_map::SourceOffset;
use crate::bytecode::source_map::SourceOffsetEntry;
use crate::slice_block::SliceBlock;
use crate::slice_block::SliceBlockBuilder;
use crate::u32_from_usize;

use libjs_abi::FunctionNamePrefix;

use super::bindings::*;
use super::context::Generator;
use super::emitter::AssembledBytecode;
use super::emitter::BlockEncoder;
use super::emitter::BlockPosition;
use super::emitter::FrameLayout;
use super::generators::DisposalKind;
use super::generators::emit_dispose_resources;
use super::generators::generate_await_of_value;
use super::generators::generate_yield_of_value;
use super::members::*;
use super::names::NameClass;
use super::nodes::Identifier;
use super::operators::emit_binary_op;
use super::registers::ScopedOperand;

const UNBOUND: u32 = u32::MAX;

/// Where the destination of a binary operator is in its instruction, which all have the same layout up to there.
const DESTINATION_OFFSET_OF_BINARY_OPERATOR: u32 = 4;

/// What an evaluated expression is, for whatever consumes it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ValKind {
    /// `a` holds the value.
    Value,
    /// `a` holds a function and `b` what it gets called on.
    Callee,
    /// A local that is about to be written: `a`.
    LocalTarget,
    /// A global or a binding of an environment that is about to be written.
    NameTarget,
    /// `a.name`, about to be written.
    MemberTarget,
    /// `a[b]`, about to be written.
    IndexTarget,
    /// A property of `super`, about to be written. `a` is the base, `b` the key unless `name` says what it is.
    SuperTarget,
    /// `a.#name`, about to be written.
    PrivateTarget,
    /// `this`, on its way to being what a property of `super` gets looked up for.
    SuperThis,
    /// What code got to the point of writing to something that it cannot write to, which it does not get past.
    InvalidTarget,
    /// The abstract operation `name` of the engine, about to be called.
    BuiltinCallee,
}

/// An entry of the stack of evaluated expressions.
#[derive(Clone, Copy)]
pub struct Val {
    pub kind: ValKind,
    /// The first register that is free again once this is consumed.
    pub first_temporary: u32,
    pub a: Operand,
    pub b: Operand,
    /// The name of a target, and the number of properties so far of an object literal being built.
    pub name: u32,
    /// The shape cache of an object literal being built, and where a member target is in the source.
    pub cache: u32,
    pub is_global: bool,
    pub is_const: bool,
    /// A `var` binding, which is known to be initialized.
    pub is_var: bool,
}

impl Val {
    #[inline(always)]
    fn is_target(&self) -> bool {
        matches!(
            self.kind,
            ValKind::LocalTarget
                | ValKind::NameTarget
                | ValKind::MemberTarget
                | ValKind::IndexTarget
                | ValKind::SuperTarget
                | ValKind::PrivateTarget
        )
    }
}

/// A jump whose target is the next thing emitted can go away again.
#[derive(Clone, Copy)]
struct LastJump {
    start: u32,
    end: u32,
    target: Label,
    label_position_count: u32,
}

/// A comparison that was just emitted, in case it turns out to feed a conditional jump.
#[derive(Clone, Copy)]
struct LastComparison {
    start: u32,
    end: u32,
    operand_position_count: u32,
    operator: BinaryOp,
    arith_feedback: u16,
    dst: Operand,
    lhs: Operand,
    rhs: Operand,
}

/// The destination of the instruction that was just emitted, in case it can write straight to where the value goes.
#[derive(Clone, Copy)]
struct LastDestination {
    end: u32,
    position: u32,
    dst: Operand,
}

/// Where an exception goes from the code that is being emitted.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct Handler {
    pub(super) label: Label,
    /// Whether this is a `catch` clause, and not a way through a `finally`.
    pub(super) catches: bool,
}

/// The registers that are in use at some point of the code, to go back to.
#[derive(Clone, Copy)]
pub(super) struct HeldRegisters {
    first_temporary: u32,
    next_temporary: u32,
    floor: u32,
}

/// The way out of the code being emitted for a `break`, a `continue` or a `return`.
#[derive(Clone, Copy, PartialEq)]
pub(super) enum Exit {
    /// Goes to `target`, with `depth` entries of the control stack left.
    Jump {
        depth: u32,
        target: Label,
    },
    Return,
}

/// How a `try` statement with a `finally` clause got to that clause.
pub struct Finally {
    pub(super) completion_type: Operand,
    pub(super) completion_value: Operand,
    pub(super) body: Label,
    /// The exits that went through the clause, which go on from there. Their completion types follow the fixed ones.
    pub(super) exits: Vec<Exit>,
}

impl Finally {
    pub(super) const NORMAL: i32 = 0;
    pub const THROW: i32 = 1;
    pub(super) const FIRST_EXIT: i32 = 2;
}

/// The kind of a disposable resource, as the hint operand of AddDisposableResource encodes it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum DisposableResourceKind {
    SyncDispose = 0,
    AsyncDispose = 1,
}

/// The resources of `using` and `await using` declarations, which go when control leaves the code they are in.
#[derive(Clone, Copy)]
pub(super) struct Disposal {
    stack: Operand,
    finally: u32,
    handler: Handler,
    outer_handler: Option<Handler>,
    entry_environment: Operand,
    held: HeldRegisters,
    is_async: bool,
}

/// An iterator that has to get closed if control leaves the code that uses it.
#[derive(Clone, Copy)]
pub(super) struct OpenIterator {
    pub(super) object: Operand,
    pub(super) next: Operand,
    pub(super) done: Operand,
    /// Where exceptions go from closing the iterator.
    pub(super) outer_handler: Option<Handler>,
    /// Whether the iterator may have run out, as it may in the middle of a pattern.
    pub(super) may_be_done: bool,
    /// Whether this is the iterator of `for await`, whose `return` method gets awaited.
    pub(super) is_async: bool,
}

/// The environment of a scope, while its code is running.
#[derive(Clone, Copy)]
pub(super) struct ScopeEnvironment {
    pub(super) register: Operand,
    pub(super) parent: Operand,
    pub(super) held: HeldRegisters,
}

/// Where the body starts in everything that grows along the way.
#[derive(Clone, Copy)]
struct BodyStart {
    offset: u32,
    operand_positions: usize,
    label_positions: usize,
    source_entries: usize,
    labels: usize,
    handler_changes: usize,
}

#[derive(Default)]
pub(super) struct Segment {
    bytes: Vec<u8>,
    operand_positions: Vec<BlockPosition>,
    label_positions: Vec<BlockPosition>,
    /// (offset of the first instruction from there on, where in the source that came from)
    source_entries: Vec<(u32, SourceOffset)>,
    last_source_offset: Option<SourceOffset>,
    dead: bool,
}

impl Segment {
    fn clear(&mut self) {
        self.bytes.clear();
        self.operand_positions.clear();
        self.label_positions.clear();
        self.source_entries.clear();
        self.last_source_offset = None;
        self.dead = false;
    }
}

/// What is going on around the code being emitted, innermost last.
#[derive(Clone, Copy)]
pub(super) enum Control {
    Conditional {
        dst: Operand,
        else_label: Label,
        end_label: Label,
        first_temporary: u32,
    },
    Logical {
        dst: Operand,
        end_label: Label,
        first_temporary: u32,
    },
    If {
        else_label: Label,
        end_label: Option<Label>,
    },
    Loop {
        continue_label: Label,
        break_label: Label,
        /// How many entries of the control stack a `break` leaves, which may be fewer than are below this one.
        break_depth: u32,
        kind: LoopKind,
    },
    Block {
        environment: Option<ScopeEnvironment>,
    },
    /// `&&=`, `||=` or `??=`, whose value goes to `dst`.
    LogicalAssignment {
        dst: Operand,
        end_label: Label,
    },
    /// An optional chain, whose value goes to `dst` unless the chain ends early.
    Chain {
        dst: Operand,
        short_circuit: Label,
        first_temporary: u32,
    },
    Switch {
        break_label: Label,
    },
    Labeled {
        name: Name,
        break_label: Label,
    },
    /// Everything that leaves goes through the `finally` clause at this index of `finallies`.
    Finally(u32),
    /// Everything that leaves disposes of the resources at this index of `disposals`.
    Disposal(u32),
    Iterator(OpenIterator),
}

#[derive(Clone, Copy)]
pub(super) enum LoopKind {
    /// Whatever put this on the stack knows what the loop looks like.
    Structured,
    While {
        head: Label,
    },
    DoWhile {
        head: Label,
    },
}

/// The state of a compile, which keeps its memory for the next one on the thread.
#[derive(Default)]
pub struct Stream {
    pub(super) is_active: bool,
    current: Segment,
    /// What comes before everything else, for code that finds out as it goes what has to happen at the start.
    head: Segment,
    /// What comes at the start of a body that has code for its parameters in front of it, and where that is.
    body_head: Segment,
    body_start: Option<BodyStart>,
    labels: Vec<u32>,
    label_is_referenced: Vec<bool>,
    number_of_arguments: u32,
    values: Vec<Val>,
    controls: Vec<Control>,
    first_temporary: u32,
    next_temporary: u32,
    /// Registers below this stay taken until the end of the statement, for copies of locals made along the way.
    floor: u32,
    temporary_count: u32,
    last_jump: Option<LastJump>,
    last_comparison: Option<LastComparison>,
    last_destination: Option<LastDestination>,
    has_resolved_this: bool,
    /// Whether `this` got resolved in code that everything else comes after.
    has_resolved_this_for_good: bool,
    has_left_entry: bool,
    /// The operands of a call or an array literal being put together.
    operands: Vec<Operand>,
    handler: Option<Handler>,
    /// Where the handler changes, in order, as offsets of the main segment.
    handler_changes: Vec<(u32, Option<Handler>)>,
    pub(super) finallies: Vec<Finally>,
    disposals: Vec<Disposal>,
    /// What an anonymous function about to come up takes as its name, which nothing in its rows says.
    pub(super) pending_function_name: Option<IdentifierTableIndex>,
    /// Where the value of the last statement goes, in code that has one.
    completion: Option<Operand>,
    /// The computed key that the next anonymous class gets as its name.
    pub(super) pending_computed_class_name: Option<Operand>,
    /// The row of an expression whose value nothing looks at, even though no statement ends with it.
    unused_value_row: u32,
}

impl Stream {
    pub(super) fn clear(&mut self) {
        if !self.is_active {
            return;
        }
        self.is_active = false;
        self.current.clear();
        self.head.clear();
        self.body_head.clear();
        self.body_start = None;
        self.labels.clear();
        self.label_is_referenced.clear();
        self.number_of_arguments = 0;
        self.values.clear();
        self.controls.clear();
        self.last_jump = None;
        self.last_comparison = None;
        self.last_destination = None;
        self.has_resolved_this = false;
        self.has_resolved_this_for_good = false;
        self.has_left_entry = false;
        self.unused_value_row = u32::MAX;
        self.handler = None;
        self.handler_changes.clear();
        self.finallies.clear();
        self.disposals.clear();
        self.pending_function_name = None;
        self.pending_computed_class_name = None;
        self.completion = None;
    }

    #[inline(always)]
    fn note_control_flow(&mut self) {
        self.has_left_entry = true;
        self.has_resolved_this = self.has_resolved_this_for_good;
    }
}

impl Generator<'_> {
    // --- Emitting ---

    /// Starts sending what gets emitted to the stream.
    pub(super) fn begin_stream(&mut self) {
        self.context.stream.is_active = true;
    }

    /// Sends what gets emitted to the head, until `end_head()`.
    pub(super) fn begin_head(&mut self) {
        let stream = &mut self.context.stream;
        std::mem::swap(&mut stream.current, &mut stream.head);
    }

    pub(super) fn end_head(&mut self) {
        self.begin_head();
    }

    /// Emits what has to happen at the start of the code of the body.
    pub(super) fn in_head(&mut self, emit: impl FnOnce(&mut Self)) {
        let stream = &mut self.context.stream;
        if stream.body_start.is_none() {
            self.in_function_head(emit);
            return;
        }
        std::mem::swap(&mut stream.current, &mut stream.body_head);
        let saved_source_offset = self.current_source_offset;
        emit(self);
        self.current_source_offset = saved_source_offset;
        let stream = &mut self.context.stream;
        std::mem::swap(&mut stream.current, &mut stream.body_head);
    }

    /// Emits what has to happen at the start of the function, before any parameter.
    pub(super) fn in_function_head(&mut self, emit: impl FnOnce(&mut Self)) {
        self.begin_head();
        let saved_source_offset = self.current_source_offset;
        emit(self);
        self.current_source_offset = saved_source_offset;
        self.end_head();
    }

    /// The code of the parameters ended here, and from now on `in_head()` is about what comes right after that.
    pub(super) fn begin_body_head(&mut self) {
        let stream = &mut self.context.stream;
        stream.body_start = Some(BodyStart {
            offset: u32_from_usize(stream.current.bytes.len()),
            operand_positions: stream.current.operand_positions.len(),
            label_positions: stream.current.label_positions.len(),
            source_entries: stream.current.source_entries.len(),
            labels: stream.labels.len(),
            handler_changes: stream.handler_changes.len(),
        });
        stream.last_jump = None;
        stream.last_comparison = None;
        stream.last_destination = None;
    }

    /// Everything from the start of the body on moves over for what comes at the start of the body.
    fn insert_body_head(&mut self) {
        let stream = &mut self.context.stream;
        let Some(start) = stream.body_start.take() else {
            return;
        };
        let head = std::mem::take(&mut stream.body_head);
        let size = u32_from_usize(head.bytes.len());
        if size == 0 {
            stream.body_head = head;
            return;
        }
        let current = &mut stream.current;
        current
            .bytes
            .splice(start.offset as usize..start.offset as usize, head.bytes.iter().copied());
        for position in &mut current.operand_positions[start.operand_positions..] {
            position.offset += size;
        }
        for position in &mut current.label_positions[start.label_positions..] {
            position.offset += size;
        }
        for entry in &mut current.source_entries[start.source_entries..] {
            entry.0 += size;
        }
        current
            .operand_positions
            .extend(head.operand_positions.iter().map(|position| BlockPosition {
                block: 0,
                offset: position.offset + start.offset,
            }));
        current.source_entries.splice(
            start.source_entries..start.source_entries,
            head.source_entries
                .iter()
                .map(|&(offset, source)| (offset + start.offset, source)),
        );
        for label in &mut stream.labels[start.labels..] {
            if *label != UNBOUND {
                *label += size;
            }
        }
        for change in &mut stream.handler_changes[start.handler_changes..] {
            change.0 += size;
        }
        stream.body_head = head;
    }

    /// The registers that the code before the temporaries uses stay out of the way of the temporaries.
    pub(super) fn begin_stream_temporaries(&mut self) {
        let first_temporary = Register::RESERVED_COUNT;
        let stream = &mut self.context.stream;
        stream.first_temporary = first_temporary;
        stream.next_temporary = first_temporary;
        stream.floor = first_temporary;
        stream.temporary_count = first_temporary;
    }

    #[inline(always)]
    pub(super) fn is_stream_dead(&self) -> bool {
        self.context.stream.current.dead
    }

    #[inline(always)]
    fn encode_into_stream(&mut self, encode: impl FnOnce(bool, &mut BlockEncoder<'_>), watched: Operand) -> u32 {
        let strict = self.strict;
        let context = &mut *self.context;
        let stream = &mut context.stream;
        let segment = &mut stream.current;
        if segment.last_source_offset != Some(self.current_source_offset) {
            segment.last_source_offset = Some(self.current_source_offset);
            segment
                .source_entries
                .push((u32_from_usize(segment.bytes.len()), self.current_source_offset));
        }
        let mut encoder = BlockEncoder::for_stream(
            &mut segment.bytes,
            &mut segment.operand_positions,
            &mut segment.label_positions,
            &mut stream.number_of_arguments,
            watched,
        );
        encode(strict, &mut encoder);
        encoder.watched_position
    }

    /// Emits an instruction.
    #[inline(always)]
    pub(super) fn put<I: TypedInstruction>(&mut self, instruction: I) {
        self.put_watching(instruction, Operand::invalid());
    }

    #[inline(always)]
    fn put_watching<I: TypedInstruction>(&mut self, instruction: I, watched: Operand) -> u32 {
        if self.context.stream.current.dead {
            return UNBOUND;
        }
        let position = if instruction.may_specialize() {
            self.put_specialized(&instruction.to_instruction(), watched)
        } else {
            self.encode_into_stream(|strict, encoder| instruction.encode(strict, encoder), watched)
        };
        if I::IS_TERMINATOR {
            self.context.stream.current.dead = true;
        }
        position
    }

    #[inline(never)]
    fn put_specialized(&mut self, instruction: &Instruction, watched: Operand) -> u32 {
        let specialized = specialize_instruction(instruction, &self.context.constants.values);
        let instruction = specialized.as_ref().unwrap_or(instruction);
        self.encode_into_stream(
            |strict, encoder| instruction.encode_into_block(strict, encoder),
            watched,
        )
    }

    /// Emits an instruction with a destination that the next thing to happen may redirect.
    #[inline(always)]
    pub(super) fn put_with_destination<I: TypedInstruction>(&mut self, instruction: I, dst: Operand) {
        let position = self.put_watching(instruction, dst);
        let stream = &mut self.context.stream;
        stream.last_destination = (position != UNBOUND).then_some(LastDestination {
            end: u32_from_usize(stream.current.bytes.len()),
            position,
            dst,
        });
    }

    // --- Temporaries and the stack of values ---

    #[inline(always)]
    pub fn temporary(&mut self) -> Operand {
        let stream = &mut self.context.stream;
        let register = stream.next_temporary;
        stream.next_temporary += 1;
        stream.temporary_count = stream.temporary_count.max(stream.next_temporary);
        Operand::register(Register(register))
    }

    #[inline(always)]
    pub(super) fn release_temporaries_from(&mut self, first_temporary: u32) {
        let stream = &mut self.context.stream;
        stream.next_temporary = first_temporary.max(stream.floor);
    }

    #[inline(always)]
    pub(super) fn next_temporary(&self) -> u32 {
        self.context.stream.next_temporary
    }

    /// Gets ready for registers that outlive the statements of a construct, until `release_held_registers()`.
    pub(super) fn hold_registers(&self) -> HeldRegisters {
        let stream = &self.context.stream;
        HeldRegisters {
            first_temporary: stream.first_temporary,
            next_temporary: stream.next_temporary,
            floor: stream.floor,
        }
    }

    /// A register that nothing else gets until the registers go back.
    pub(super) fn held_register(&mut self) -> Operand {
        let register = self.temporary();
        let stream = &mut self.context.stream;
        stream.first_temporary = stream.next_temporary;
        stream.floor = stream.next_temporary;
        register
    }

    pub(super) fn release_held_registers(&mut self, held: HeldRegisters) {
        let stream = &mut self.context.stream;
        // NB: A value below the held registers on the stack may have got a copy above them, and it outlives them.
        let first_free = stream
            .values
            .iter()
            .flat_map(|value| [value.a, value.b])
            .filter(|operand| operand.is_register())
            .map(|operand| operand.index() + 1)
            .fold(held.next_temporary, u32::max);
        stream.first_temporary = held.first_temporary;
        stream.floor = if first_free > held.next_temporary {
            first_free
        } else {
            held.floor
        };
        stream.next_temporary = first_free;
    }

    // --- Exception handlers ---

    #[inline(always)]
    pub(super) fn handler(&self) -> Option<Handler> {
        self.context.stream.handler
    }

    /// Sends the exceptions of what gets emitted from here on to `handler`.
    pub(super) fn set_handler(&mut self, handler: Option<Handler>) {
        let stream = &mut self.context.stream;
        stream.handler = handler;
        let offset = u32_from_usize(stream.current.bytes.len());
        match stream.handler_changes.last_mut() {
            Some(last) if last.0 == offset => last.1 = handler,
            _ => stream.handler_changes.push((offset, handler)),
        }
        // NB: The offset has to stay where it is, and whatever is in the handler's range may never run.
        stream.last_jump = None;
        stream.last_comparison = None;
        stream.last_destination = None;
        stream.note_control_flow();
    }

    /// Binds a label that something gets to, whatever comes before it: an exception, say.
    pub(super) fn bind_reachable_label(&mut self, label: Label) {
        let stream = &mut self.context.stream;
        stream.label_is_referenced[label.0 as usize] = true;
        self.bind_label(label);
    }

    // --- What is going on around the code ---

    #[inline(always)]
    pub(super) fn push_control(&mut self, control: Control) {
        self.context.stream.controls.push(control);
    }

    #[inline(always)]
    pub(super) fn pop_control(&mut self) {
        self.context.stream.controls.pop();
    }

    #[inline(always)]
    pub(super) fn control_depth(&self) -> u32 {
        u32_from_usize(self.context.stream.controls.len())
    }

    /// Takes control out of whatever is going on around the code.
    pub(super) fn emit_exit(&mut self, exit: Exit, value: Operand) {
        let handler = self.handler();
        let stop = match exit {
            Exit::Jump { depth, .. } => depth as usize,
            Exit::Return => 0,
        };
        let mut index = self.context.stream.controls.len();
        while index > stop {
            index -= 1;
            match self.context.stream.controls[index] {
                Control::Block {
                    environment: Some(environment),
                } => {
                    let parent = self.scoped_operand(environment.parent);
                    self.restore_lexical_environment(&parent);
                }
                Control::Iterator(iterator) => {
                    self.set_handler(iterator.outer_handler);
                    self.put_iterator_close(iterator);
                }
                Control::Disposal(disposal) => {
                    let finally = self.context.stream.disposals[disposal as usize].finally;
                    self.exit_through_finally(finally as usize, exit, value);
                    if self.handler() != handler {
                        self.set_handler(handler);
                    }
                    return;
                }
                Control::Finally(finally) => {
                    self.exit_through_finally(finally as usize, exit, value);
                    if self.handler() != handler {
                        self.set_handler(handler);
                    }
                    return;
                }
                _ => {}
            }
        }
        match exit {
            Exit::Jump { target, .. } => self.jump(target),
            Exit::Return => self.put_return(value),
        }
        if self.handler() != handler {
            self.set_handler(handler);
        }
    }

    fn exit_through_finally(&mut self, index: usize, exit: Exit, value: Operand) {
        let finally = &mut self.context.stream.finallies[index];
        let position = finally
            .exits
            .iter()
            .position(|&known| known == exit)
            .unwrap_or_else(|| {
                finally.exits.push(exit);
                finally.exits.len() - 1
            });
        let (completion_type, completion_value, body) =
            (finally.completion_type, finally.completion_value, finally.body);
        if exit == Exit::Return {
            self.put(op::Mov {
                dst: completion_value,
                src: value,
            });
        }
        let kind =
            self.add_constant_i32(Finally::FIRST_EXIT + i32::try_from(position).expect("a function has few exits"));
        self.put(op::Mov {
            dst: completion_type,
            src: kind.operand(),
        });
        self.jump(body);
    }

    /// A new set of completions for what comes next, which ends with `finally_dispatch()`.
    pub(super) fn begin_finally(&mut self) -> u32 {
        let completion_type = self.held_register();
        let completion_value = self.held_register();
        let body = self.new_label();
        let stream = &mut self.context.stream;
        let index = u32_from_usize(stream.finallies.len());
        stream.finallies.push(Finally {
            completion_type,
            completion_value,
            body,
            exits: Vec::new(),
        });
        index
    }

    /// Code got to the end of what a `finally` clause is around, which is what comes next if there is one.
    pub(super) fn leave_finally_part(&mut self, finally: u32) {
        if self.is_stream_dead() {
            return;
        }
        let finally = &self.context.stream.finallies[finally as usize];
        let (completion_type, body) = (finally.completion_type, finally.body);
        let normal = self.add_constant_i32(Finally::NORMAL);
        self.put(op::Mov {
            dst: completion_type,
            src: normal.operand(),
        });
        self.jump(body);
    }

    /// What goes wrong in the code that a `finally` clause is around goes here, which is what `handler` is for.
    pub(super) fn bind_finally_handler(
        &mut self,
        finally: usize,
        handler: Handler,
        entry_environment: &ScopedOperand,
    ) -> Label {
        let finally = &self.context.stream.finallies[finally];
        let (completion_type, completion_value, body) =
            (finally.completion_type, finally.completion_value, finally.body);
        self.bind_reachable_label(handler.label);
        let value_feedback = self.next_value_feedback();
        self.put(op::Catch {
            value_feedback,
            dst: completion_value,
        });
        self.restore_lexical_environment(entry_environment);
        let throw = self.add_constant_i32(Finally::THROW);
        self.put(op::Mov {
            dst: completion_type,
            src: throw.operand(),
        });
        body
    }

    /// Goes on from the end of a `finally` clause with whatever got to it.
    pub(super) fn finally_dispatch(&mut self, index: usize, end_label: Label) {
        if self.is_stream_dead() {
            return;
        }
        let finally = &mut self.context.stream.finallies[index];
        let (completion_type, completion_value) = (finally.completion_type, finally.completion_value);
        let exits = std::mem::take(&mut finally.exits);

        let compare_and_branch = |generator: &mut Self, kind: i32, target: Label, next: Label| {
            let kind = generator.add_constant_i32(kind);
            let arith_feedback = generator.next_arith_feedback();
            generator.reference_label(target);
            generator.reference_label(next);
            generator.put(op::JumpStrictlyEquals {
                arith_feedback,
                lhs: completion_type,
                rhs: kind.operand(),
                true_target: target,
                false_target: next,
            });
        };

        let next = self.new_label();
        compare_and_branch(self, Finally::NORMAL, end_label, next);
        self.bind_label(next);
        for (position, &exit) in exits.iter().enumerate() {
            let this_exit = self.new_label();
            let next = self.new_label();
            let kind = Finally::FIRST_EXIT + i32::try_from(position).expect("a function has few exits");
            compare_and_branch(self, kind, this_exit, next);
            self.bind_label(this_exit);
            self.emit_exit(exit, completion_value);
            self.bind_label(next);
        }
        self.put(op::Throw { src: completion_value });
    }

    /// A `using` or `await using` declaration comes up, whose resource goes when control leaves the code it is directly
    /// in. Nothing can be there to dispose of before the first of them, so that is where that starts.
    pub(super) fn begin_disposal(&mut self, is_async: bool) {
        if let Some(&Control::Disposal(index)) = self.context.stream.controls.last() {
            self.context.stream.disposals[index as usize].is_async |= is_async;
            return;
        }
        let held = self.hold_registers();
        let stack = self.held_register();
        self.put(op::NewDisposableResourceStack { dst: stack });
        let finally = self.begin_finally();
        let outer_handler = self.handler();
        let handler = Handler {
            label: self.new_label(),
            catches: false,
        };
        let entry_environment = self.current_lexical_environment().operand();
        let stream = &mut self.context.stream;
        let index = u32_from_usize(stream.disposals.len());
        stream.disposals.push(Disposal {
            stack,
            finally,
            handler,
            outer_handler,
            entry_environment,
            held,
            is_async,
        });
        self.push_control(Control::Disposal(index));
        self.set_handler(Some(handler));
    }

    /// Adds `value` to the resources of the code around.
    pub(super) fn add_disposable_resource(&mut self, value: Operand, is_async: bool) {
        let Some(&Control::Disposal(index)) = self.context.stream.controls.last() else {
            unreachable!("a resource comes after begin_disposal()")
        };
        let stack = self.context.stream.disposals[index as usize].stack;
        let kind = if is_async {
            DisposableResourceKind::AsyncDispose
        } else {
            DisposableResourceKind::SyncDispose
        };
        self.put(op::AddDisposableResource {
            stack,
            value,
            hint: kind as u32,
        });
    }

    /// The code that a declaration is directly in ended, which disposes of its resources if it has any.
    pub(super) fn end_disposal(&mut self) {
        let Some(&Control::Disposal(index)) = self.context.stream.controls.last() else {
            return;
        };
        self.pop_control();
        let disposal = self.context.stream.disposals[index as usize];
        let finally = disposal.finally;
        self.leave_finally_part(finally);
        self.set_handler(disposal.outer_handler);
        let entry_environment = self.scoped_operand(disposal.entry_environment);
        let body = self.bind_finally_handler(finally as usize, disposal.handler, &entry_environment);
        self.bind_label(body);
        let (completion_type, completion_value) = {
            let finally = &self.context.stream.finallies[finally as usize];
            (finally.completion_type, finally.completion_value)
        };
        emit_dispose_resources(
            self,
            if disposal.is_async {
                DisposalKind::Async
            } else {
                DisposalKind::Sync
            },
            &ScopedOperand::plain(disposal.stack),
            &ScopedOperand::plain(completion_type),
            &ScopedOperand::plain(completion_value),
        );
        let end_label = self.new_label();
        self.finally_dispatch(finally as usize, end_label);
        self.bind_label(end_label);
        self.release_held_registers(disposal.held);
    }

    /// Calls the `return` method of an async iterator and awaits what that gives, if it has one, and returns what it gave.
    pub(super) fn put_async_iterator_return(&mut self, iterator: OpenIterator, after_return: Label) -> Operand {
        let method = self.allocate_register().operand();
        let key = self.intern_property_key_text(crate::utf16::Utf16View::Ascii(b"return"));
        let cache = self.next_property_lookup_cache();
        self.put(op::GetMethod {
            dst: method,
            object: iterator.object,
            property: key,
            cache,
        });
        let call = self.new_label();
        self.reference_label(after_return);
        self.reference_label(call);
        self.put(op::JumpUndefined {
            condition: method,
            true_target: after_return,
            false_target: call,
        });
        self.bind_label(call);
        let result = self.allocate_register().operand();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put(op::Call {
            value_feedback,
            call_feedback,
            dst: result,
            callee: method,
            this_value: iterator.object,
            argument_count: 0,
            expression_string: None,
            arguments: &[],
        });
        generate_await_of_value(self, &ScopedOperand::plain(result)).operand()
    }

    fn put_iterator_close(&mut self, iterator: OpenIterator) {
        if iterator.is_async {
            let held = self.hold_registers();
            let end = self.new_label();
            let result = self.put_async_iterator_return(iterator, end);
            self.put(op::ThrowIfNotObject { src: result });
            self.bind_label(end);
            self.release_held_registers(held);
            return;
        }
        let done = iterator.may_be_done.then(|| {
            let label = self.new_label();
            self.reference_label(label);
            self.put_conditional(op::JumpTrue {
                condition: iterator.done,
                target: label,
            });
            label
        });
        let undefined = self.context.constants.add_undefined();
        let cache = self.next_property_lookup_cache();
        self.put(op::IteratorClose {
            iterator_object: iterator.object,
            iterator_next: iterator.next,
            iterator_done: iterator.done,
            completion_type: libjs_abi::CompletionType::Normal as u32,
            completion_value: undefined,
            cache,
        });
        if let Some(done) = done {
            self.bind_label(done);
        }
    }

    /// What it takes for the function to return `value`, wherever it is.
    pub(super) fn put_return(&mut self, value: Operand) {
        if self.is_in_generator_or_async_function() {
            self.put(op::Yield {
                continuation_label: None,
                value,
            });
        } else {
            self.put(op::Return { value });
        }
    }

    #[inline(always)]
    pub(super) fn push_value(&mut self, operand: Operand, first_temporary: u32) {
        self.context.stream.values.push(Val {
            kind: ValKind::Value,
            first_temporary,
            a: operand,
            b: Operand::invalid(),
            name: 0,
            cache: u32::MAX,
            is_global: false,
            is_const: false,
            is_var: false,
        });
    }

    #[inline(always)]
    pub(super) fn pop_value(&mut self) -> Val {
        self.context.stream.values.pop().expect("something was evaluated")
    }

    /// The operand of the value on the stack that a consumer takes as is.
    #[inline(always)]
    pub(super) fn pop_operand(&mut self) -> (Operand, u32) {
        let value = self.pop_value();
        debug_assert!(matches!(value.kind, ValKind::Value | ValKind::Callee));
        (value.a, value.first_temporary)
    }

    /// Takes a copy of `local` for everything on the stack that holds it, right before something writes it.
    #[inline(always)]
    fn before_write_to_local(&mut self, local: Operand) {
        if !self.context.stream.values.is_empty() {
            self.copy_local_on_stack(local);
        }
    }

    fn copy_local_on_stack(&mut self, local: Operand) {
        for index in 0..self.context.stream.values.len() {
            let value = self.context.stream.values[index];
            if value.kind == ValKind::LocalTarget {
                continue;
            }
            if value.a == local {
                let copy = self.copy_to_kept_temporary(local);
                self.context.stream.values[index].a = copy;
            }
            if value.b == local {
                let copy = self.copy_to_kept_temporary(local);
                self.context.stream.values[index].b = copy;
            }
        }
    }

    fn copy_to_kept_temporary(&mut self, operand: Operand) -> Operand {
        let copy = self.temporary();
        self.put(op::Mov {
            dst: copy,
            src: operand,
        });
        let stream = &mut self.context.stream;
        stream.floor = stream.next_temporary;
        copy
    }

    /// Whatever is about to happen happens in only some of the ways through the code, so copies have to come first.
    pub(super) fn copy_locals_on_stack(&mut self) {
        for index in 0..self.context.stream.values.len() {
            let value = self.context.stream.values[index];
            if value.kind == ValKind::LocalTarget {
                continue;
            }
            if value.a.is_local() || value.a.is_argument() {
                let copy = self.copy_to_kept_temporary(value.a);
                self.context.stream.values[index].a = copy;
            }
            if value.b.is_local() || value.b.is_argument() {
                let copy = self.copy_to_kept_temporary(value.b);
                self.context.stream.values[index].b = copy;
            }
        }
    }

    /// Makes `dst` hold `value`, which is either in a temporary that the last instruction wrote or takes a move.
    pub(super) fn move_value_to(&mut self, dst: Operand, value: Operand) {
        if dst == value {
            return;
        }
        let stream = &mut self.context.stream;
        // NB: A handler sees a local as it was if the instruction throws, so that it cannot write the local itself.
        if let Some(last) = stream.last_destination
            && last.dst == value
            && last.end as usize == stream.current.bytes.len()
            && (stream.handler.is_none() || dst.is_register())
        {
            let position = last.position as usize;
            stream.current.bytes[position..position + 4].copy_from_slice(&dst.raw().to_ne_bytes());
            stream.last_destination = None;
            if !dst.is_register() {
                stream.current.operand_positions.push(BlockPosition {
                    block: 0,
                    offset: last.position,
                });
            }
            if dst.is_argument() {
                stream.number_of_arguments = stream.number_of_arguments.max(dst.index() + 1);
            }
            return;
        }
        self.put(op::Mov { dst, src: value });
    }

    // --- Labels and jumps ---

    #[inline(always)]
    pub(super) fn new_label(&mut self) -> Label {
        let stream = &mut self.context.stream;
        let label = Label(u32_from_usize(stream.labels.len()));
        stream.labels.push(UNBOUND);
        stream.label_is_referenced.push(false);
        label
    }

    #[inline(always)]
    pub(super) fn reference_label(&mut self, label: Label) {
        let stream = &mut self.context.stream;
        if !stream.current.dead {
            stream.label_is_referenced[label.0 as usize] = true;
        }
        stream.note_control_flow();
    }

    pub(super) fn bind_label(&mut self, label: Label) {
        let stream = &mut self.context.stream;
        let segment = &mut stream.current;
        if let Some(jump) = stream.last_jump.take()
            && jump.target == label
            && jump.end as usize == segment.bytes.len()
        {
            // NB: Nothing else knows about this jump, so that it is as if it never was.
            segment.bytes.truncate(jump.start as usize);
            segment.label_positions.truncate(jump.label_position_count as usize);
            segment.dead = false;
        }
        if stream.label_is_referenced[label.0 as usize] {
            segment.dead = false;
        }
        stream.labels[label.0 as usize] = u32_from_usize(segment.bytes.len());
        stream.last_comparison = None;
        stream.last_destination = None;
        stream.note_control_flow();
    }

    #[inline(always)]
    pub(super) fn jump(&mut self, target: Label) {
        if self.context.stream.current.dead {
            return;
        }
        self.reference_label(target);
        let stream = &self.context.stream;
        let start = u32_from_usize(stream.current.bytes.len());
        let label_position_count = u32_from_usize(stream.current.label_positions.len());
        self.put(op::Jump { target });
        let stream = &mut self.context.stream;
        stream.last_jump = Some(LastJump {
            start,
            end: u32_from_usize(stream.current.bytes.len()),
            target,
            label_position_count,
        });
    }

    pub(super) fn jump_back(&mut self, target: Label) {
        if self.context.stream.current.dead {
            return;
        }
        self.reference_label(target);
        self.put(op::JumpLoop { target });
    }

    /// Takes the comparison that made `condition` back, if that is what the code just did.
    #[inline(always)]
    fn take_last_comparison(&mut self, condition: Operand) -> Option<LastComparison> {
        let stream = &mut self.context.stream;
        let comparison = stream.last_comparison?;
        if comparison.dst != condition || comparison.end as usize != stream.current.bytes.len() {
            return None;
        }
        stream.current.bytes.truncate(comparison.start as usize);
        stream
            .current
            .operand_positions
            .truncate(comparison.operand_position_count as usize);
        stream.last_comparison = None;
        stream.last_destination = None;
        Some(comparison)
    }

    fn constant_truthiness(&self, condition: Operand) -> Option<bool> {
        if !condition.is_constant() {
            return None;
        }
        self.context.constants.get(condition).and_then(constant_to_boolean)
    }

    /// Falls through if `condition` holds, and otherwise goes to `target`.
    pub(super) fn jump_unless(&mut self, condition: Operand, target: Label) {
        if let Some(is_truthy) = self.constant_truthiness(condition) {
            if !is_truthy {
                self.jump(target);
            }
            return;
        }
        self.reference_label(target);
        if let Some(comparison) = self.take_last_comparison(condition) {
            let next = self.new_label();
            self.reference_label(next);
            self.put_fused_jump(comparison, next, target, false);
            self.bind_label(next);
            return;
        }
        self.put_conditional(op::JumpFalse { condition, target });
    }

    /// Goes to `target` if `condition` holds, and otherwise falls through.
    pub(super) fn jump_if(&mut self, condition: Operand, target: Label) {
        if let Some(is_truthy) = self.constant_truthiness(condition) {
            if is_truthy {
                self.jump(target);
            }
            return;
        }
        self.reference_label(target);
        if let Some(comparison) = self.take_last_comparison(condition) {
            let next = self.new_label();
            self.reference_label(next);
            self.put_fused_jump(comparison, target, next, false);
            self.bind_label(next);
            return;
        }
        self.put_conditional(op::JumpTrue { condition, target });
    }

    /// Goes back to `target` if `condition` holds, and otherwise falls through.
    pub(super) fn jump_back_if(&mut self, condition: Operand, target: Label) {
        if let Some(is_truthy) = self.constant_truthiness(condition) {
            if is_truthy {
                self.jump_back(target);
            }
            return;
        }
        self.reference_label(target);
        if let Some(comparison) = self.take_last_comparison(condition) {
            let next = self.new_label();
            self.reference_label(next);
            self.put_fused_jump(comparison, target, next, true);
            self.bind_label(next);
            return;
        }
        self.put_conditional(op::JumpTrueLoop { condition, target });
    }

    /// Emits a jump that falls through if it does not go anywhere.
    #[inline(always)]
    pub(super) fn put_conditional<I: TypedInstruction>(&mut self, instruction: I) {
        let was_dead = self.context.stream.current.dead;
        self.put(instruction);
        self.context.stream.current.dead = was_dead;
    }

    fn put_fused_jump(&mut self, comparison: LastComparison, true_target: Label, false_target: Label, back_edge: bool) {
        let LastComparison {
            operator,
            arith_feedback,
            lhs,
            rhs,
            ..
        } = comparison;
        macro_rules! fused {
            ($($operator:ident => $jump:ident, $jump_loop:ident;)*) => {
                match operator {
                    $(BinaryOp::$operator => {
                        if back_edge {
                            self.put(op::$jump_loop { arith_feedback, lhs, rhs, true_target, false_target });
                        } else {
                            self.put(op::$jump { arith_feedback, lhs, rhs, true_target, false_target });
                        }
                    })*
                    _ => unreachable!("only comparisons get fused"),
                }
            };
        }
        fused!(
            LessThan => JumpLessThan, JumpLessThanLoop;
            LessThanEquals => JumpLessThanEquals, JumpLessThanEqualsLoop;
            GreaterThan => JumpGreaterThan, JumpGreaterThanLoop;
            GreaterThanEquals => JumpGreaterThanEquals, JumpGreaterThanEqualsLoop;
            LooselyEquals => JumpLooselyEquals, JumpLooselyEqualsLoop;
            LooselyInequals => JumpLooselyInequals, JumpLooselyInequalsLoop;
            StrictlyEquals => JumpStrictlyEquals, JumpStrictlyEqualsLoop;
            StrictlyInequals => JumpStrictlyInequals, JumpStrictlyInequalsLoop;
        );
    }

    // --- Leaves ---

    #[inline(always)]
    pub(super) fn value_of_constant(&mut self, operand: Operand) {
        self.push_value(operand, self.next_temporary());
    }

    #[inline(always)]
    pub(super) fn value_of_number(&mut self, number: f64) {
        let operand = self.context.constants.add_number(number);
        self.value_of_constant(operand);
    }

    pub(super) fn value_of_this(&mut self) {
        if self.this_value_needs_environment_resolution && !self.context.stream.has_resolved_this {
            self.put(op::ResolveThisBinding {});
            let stream = &mut self.context.stream;
            stream.has_resolved_this = true;
            stream.has_resolved_this_for_good = !stream.has_left_entry;
        }
        self.value_of_constant(Operand::register(Register::THIS_VALUE));
    }

    #[inline(always)]
    fn local_operand(&mut self, ident: &Identifier) -> Operand {
        match ident.local_type {
            Some(LocalType::Argument) => Operand::argument(ident.local_index),
            _ => Operand::local(ident.local_index),
        }
    }

    #[inline(always)]
    fn check_local_is_initialized(&mut self, ident: &Identifier, local: Operand) {
        if ident.local_type == Some(LocalType::Argument) {
            if self.checks_parameters() && !self.is_argument_initialized(ident.local_index) {
                let empty = self.context.constants.add_empty();
                self.put(op::Mov { dst: local, src: empty });
                self.put(op::ThrowIfTDZ { src: local });
            }
        } else if self.local_has_temporal_dead_zone(ident.local_index) && !self.is_local_initialized(ident.local_index)
        {
            // NB: The initializer of a local sees it in its dead zone, whatever the iteration before left in there.
            let is_being_initialized = self
                .context
                .stream
                .values
                .iter()
                .any(|value| value.kind == ValKind::LocalTarget && value.a == local);
            if is_being_initialized {
                let empty = self.context.constants.add_empty();
                self.put(op::Mov { dst: local, src: empty });
            }
            self.put(op::ThrowIfTDZ { src: local });
        }
    }

    /// The value of a reference to `ident`.
    #[inline(always)]
    pub(super) fn value_of_identifier(&mut self, ident: Identifier) {
        let first_temporary = self.next_temporary();
        if ident.is_local() {
            let local = self.local_operand(&ident);
            self.check_local_is_initialized(&ident, local);
            self.push_value(local, first_temporary);
            return;
        }
        if ident.is_global {
            match self.name_class(ident.name) {
                NameClass::Undefined => {
                    let operand = self.context.constants.add_undefined();
                    self.push_value(operand, first_temporary);
                    return;
                }
                NameClass::NaN => {
                    let operand = self.context.constants.add_number(f64::NAN);
                    self.push_value(operand, first_temporary);
                    return;
                }
                NameClass::Infinity => {
                    let operand = self.context.constants.add_number(f64::INFINITY);
                    self.push_value(operand, first_temporary);
                    return;
                }
                _ => {
                    if let Some(constant) = self.builtin_constant(ident.name) {
                        self.push_value(constant.operand(), first_temporary);
                        return;
                    }
                }
            }
        }
        let dst = self.temporary();
        let identifier = self.intern_identifier(ident.name);
        if ident.is_global {
            let cache = self.next_global_variable_cache();
            let value_feedback = self.next_value_feedback();
            self.put_with_destination(
                op::GetGlobal {
                    value_feedback,
                    dst,
                    identifier,
                    cache,
                },
                dst,
            );
        } else {
            emit_get_binding(
                self,
                dst,
                identifier,
                ident.declaration_kind == Some(DeclarationKind::Var),
            );
        }
        self.push_value(dst, first_temporary);
    }

    /// What `typeof ident` gives, which unlike a reference does not throw for a name that nothing binds.
    /// `typeof` of an identifier, of which the keyword is at `position`.
    pub(super) fn value_of_typeof_identifier(&mut self, ident: Identifier, position: SourceOffset) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        if ident.is_local() {
            let local = self.local_operand(&ident);
            self.check_local_is_initialized(&ident, local);
            self.put_with_destination(op::Typeof { dst, src: local }, dst);
        } else {
            self.current_source_offset = position;
            let identifier = self.intern_identifier(ident.name);
            if ident.is_global {
                let cache = self.next_global_variable_cache();
                self.put_with_destination(op::TypeofGlobal { dst, identifier, cache }, dst);
            } else {
                emit_typeof_binding(self, dst, identifier);
            }
        }
        self.push_value(dst, first_temporary);
    }

    // --- Member access ---

    /// `value.name`, for the value on top of the stack.
    #[inline(always)]
    pub(super) fn value_of_member(&mut self, name: Name) {
        let (base, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        self.put_get_by_id(dst, base, name);
        self.push_value(dst, first_temporary);
    }

    #[inline(always)]
    pub(super) fn put_get_by_id(&mut self, dst: Operand, base: Operand, name: Name) {
        let key = self.intern_property_key(name);
        if self.context.names.is_length_property_key(key) {
            self.length_identifier = Some(key);
            let cache = self.next_property_lookup_cache();
            let value_feedback = self.next_value_feedback();
            let keyed_feedback = self.next_keyed_feedback();
            self.put_with_destination(
                op::GetLength {
                    value_feedback,
                    keyed_feedback,
                    dst,
                    base,
                    base_identifier: None,
                    cache,
                },
                dst,
            );
        } else {
            let cache = self.next_property_lookup_cache();
            let value_feedback = self.next_value_feedback();
            self.put_with_destination(
                op::GetById {
                    value_feedback,
                    dst,
                    base,
                    property: key,
                    base_identifier: None,
                    cache,
                },
                dst,
            );
        }
    }

    /// `base[key]`, for the two values on top of the stack.
    pub(super) fn value_of_index(&mut self) {
        let (key, _) = self.pop_operand();
        let (base, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        self.put_get_by_value(dst, base, key);
        self.push_value(dst, first_temporary);
    }

    pub(super) fn put_get_by_value(&mut self, dst: Operand, base: Operand, key: Operand) {
        emit_get_by_value(
            self,
            &ScopedOperand::plain(dst),
            &ScopedOperand::plain(base),
            &ScopedOperand::plain(key),
            None,
        );
    }

    // --- Properties of `super` and private names ---

    /// `this`, for the property of `super` that comes next.
    pub(super) fn value_of_super_this(&mut self) {
        self.value_of_this();
        self.context.stream.values.last_mut().expect("this is `this`").kind = ValKind::SuperThis;
    }

    fn pop_super_base(&mut self) -> (Operand, u32) {
        let this_value = self.pop_value();
        debug_assert!(this_value.kind == ValKind::SuperThis);
        let base = self.temporary();
        self.put(op::ResolveSuperBase { dst: base });
        (base, this_value.first_temporary)
    }

    fn put_get_of_super(&mut self, dst: Operand, base: Operand, key: Result<Name, Operand>) {
        let this_value = ScopedOperand::plain(Operand::register(Register::THIS_VALUE));
        let dst = ScopedOperand::plain(dst);
        let base = ScopedOperand::plain(base);
        match key {
            Ok(name) => emit_get_by_id_with_this(self, &dst, &base, name, &this_value),
            Err(key) => {
                let key = ScopedOperand::plain(key);
                emit_get_by_value_with_this(self, &dst, &base, &key, &this_value);
            }
        }
    }

    /// `super.name`, or `super[key]` with the key on top of the stack, as a value or something to call.
    pub(super) fn value_of_super_property(&mut self, name: Option<Name>, is_callee: bool) {
        let key = match name {
            Some(name) => Ok(name),
            None => Err(self.pop_operand().0),
        };
        let (base, first_temporary) = self.pop_super_base();
        let dst = self.temporary();
        self.put_get_of_super(dst, base, key);
        if is_callee {
            self.push_callee(dst, Operand::register(Register::THIS_VALUE), first_temporary);
        } else {
            self.push_value(dst, first_temporary);
        }
    }

    /// `super.name`, or `super[key]` with the key on top of the stack, about to be written.
    pub(super) fn target_of_super_property(&mut self, name: Option<Name>) {
        let key = if name.is_none() {
            let (key, _) = self.pop_operand();
            key
        } else {
            Operand::invalid()
        };
        let (base, first_temporary) = self.pop_super_base();
        self.context.stream.values.push(Val {
            kind: ValKind::SuperTarget,
            first_temporary,
            a: base,
            b: key,
            name: name.map_or(0, |name| name.0),
            cache: u32::MAX,
            is_global: false,
            is_const: false,
            is_var: false,
        });
    }

    /// `super` as what gets called, whose constructor comes before the arguments.
    pub(super) fn value_of_super_constructor(&mut self) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        self.put(op::GetSuperConstructor { dst });
        self.push_value(dst, first_temporary);
    }

    /// `super(...)`, with the array of arguments on top of the stack.
    pub(super) fn super_call(&mut self) {
        let (arguments, _) = self.pop_operand();
        let (super_constructor, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::SuperCallWithArgumentArray {
                value_feedback,
                call_feedback,
                dst,
                super_constructor,
                arguments,
                is_synthetic: false,
            },
            dst,
        );
        self.push_value(dst, first_temporary);
    }

    /// `#name in value`, for the value on top of the stack, which the name stands in for below it.
    pub(super) fn private_name_in(&mut self, name: Name) {
        let (base, _) = self.pop_operand();
        let (_, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let property = intern_private_name(self, name);
        self.put_with_destination(op::HasPrivateId { dst, base, property }, dst);
        self.push_value(dst, first_temporary);
    }

    /// `value.#name`, for the value on top of the stack.
    pub(super) fn value_of_private_member(&mut self, name: Name) {
        let (base, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let property = intern_private_name(self, name);
        self.put_with_destination(op::GetPrivateById { dst, base, property }, dst);
        self.push_value(dst, first_temporary);
    }

    pub(super) fn callee_of_private_member(&mut self, name: Name) {
        let (base, first_temporary) = self.pop_operand();
        let callee = self.temporary();
        let property = intern_private_name(self, name);
        self.put(op::GetPrivateById {
            dst: callee,
            base,
            property,
        });
        self.push_callee(callee, base, first_temporary);
    }

    pub(super) fn target_of_private_member(&mut self, name: Name) {
        let value = self.context.stream.values.last_mut().expect("something was evaluated");
        value.kind = ValKind::PrivateTarget;
        value.name = name.0;
    }

    pub(super) fn value_of_new_target(&mut self) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        self.put_with_destination(op::GetNewTarget { dst }, dst);
        self.push_value(dst, first_temporary);
    }

    // --- Callees ---

    /// The value on top of the stack, as something that gets called on nothing, by a call at `position`.
    pub(super) fn callee_of_identifier(&mut self, ident: Identifier, position: SourceOffset) {
        let first_temporary = self.next_temporary();
        if !ident.is_local() && !ident.is_global {
            self.current_source_offset = position;
            let callee = self.temporary();
            let this_value = self.temporary();
            let identifier = self.intern_identifier(ident.name);
            emit_get_callee_and_this_from_environment(self, callee, this_value, identifier);
            self.push_callee(callee, this_value, first_temporary);
            return;
        }
        self.value_of_identifier(ident);
        let (callee, _) = self.pop_operand();
        let undefined = self.context.constants.add_undefined();
        self.push_callee(callee, undefined, first_temporary);
    }

    /// A call of the abstract operation `name` comes up, whose arguments come next.
    pub(super) fn callee_of_builtin_operation(&mut self, name: Name) {
        let first_temporary = self.next_temporary();
        self.context.stream.values.push(Val {
            kind: ValKind::BuiltinCallee,
            first_temporary,
            a: Operand::invalid(),
            b: Operand::invalid(),
            name: name.0,
            cache: u32::MAX,
            is_global: false,
            is_const: false,
            is_var: false,
        });
    }

    fn push_callee(&mut self, callee: Operand, this_value: Operand, first_temporary: u32) {
        self.context.stream.values.push(Val {
            kind: ValKind::Callee,
            first_temporary,
            a: callee,
            b: this_value,
            name: 0,
            cache: u32::MAX,
            is_global: false,
            is_const: false,
            is_var: false,
        });
    }

    /// `value.name`, about to be called on the value on top of the stack.
    pub(super) fn callee_of_member(&mut self, name: Name) {
        let (base, first_temporary) = self.pop_operand();
        let callee = self.temporary();
        self.put_get_by_id(callee, base, name);
        self.push_callee(callee, base, first_temporary);
    }

    pub(super) fn callee_of_index(&mut self) {
        let (key, _) = self.pop_operand();
        let (base, first_temporary) = self.pop_operand();
        let callee = self.temporary();
        self.put_get_by_value(callee, base, key);
        self.push_callee(callee, base, first_temporary);
    }

    // --- Calls ---

    fn take_operands(&mut self, count: usize) -> Vec<Operand> {
        let mut operands = std::mem::take(&mut self.context.stream.operands);
        operands.clear();
        let stream = &mut self.context.stream;
        let start = stream.values.len() - count;
        operands.extend(stream.values[start..].iter().map(|value| value.a));
        stream.values.truncate(start);
        operands
    }

    #[inline(always)]
    pub(super) fn call(&mut self, argument_count: usize) {
        let arguments = self.take_operands(argument_count);
        let callee = self.pop_value();
        if callee.kind == ValKind::BuiltinCallee {
            self.release_temporaries_from(callee.first_temporary);
            let dst = self.temporary();
            self.builtin_operation(Name(callee.name), &arguments, dst);
            self.context.stream.operands = arguments;
            self.push_value(dst, callee.first_temporary);
            return;
        }
        let (callee_operand, this_value) = if callee.kind == ValKind::Callee {
            (callee.a, callee.b)
        } else {
            (callee.a, self.context.constants.add_undefined())
        };
        self.release_temporaries_from(callee.first_temporary);
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::Call {
                value_feedback,
                call_feedback,
                dst,
                callee: callee_operand,
                this_value,
                argument_count: u32_from_usize(arguments.len()),
                expression_string: None,
                arguments: &arguments,
            },
            dst,
        );
        self.context.stream.operands = arguments;
        self.push_value(dst, callee.first_temporary);
    }

    pub(super) fn construct(&mut self, argument_count: usize) {
        let arguments = self.take_operands(argument_count);
        let (callee, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::CallConstruct {
                value_feedback,
                call_feedback,
                dst,
                callee,
                argument_count: u32_from_usize(arguments.len()),
                expression_string: None,
                arguments: &arguments,
            },
            dst,
        );
        self.context.stream.operands = arguments;
        self.push_value(dst, first_temporary);
    }

    /// A call of what is below the array of arguments on top of the stack.
    /// A call of the name `eval`, which is a direct eval if that is what the name means by then.
    pub(super) fn direct_eval(&mut self, argument_count: usize) {
        let arguments = self.take_operands(argument_count);
        let callee = self.pop_value();
        let (callee_operand, this_value) = if callee.kind == ValKind::Callee {
            (callee.a, callee.b)
        } else {
            (callee.a, self.context.constants.add_undefined())
        };
        self.release_temporaries_from(callee.first_temporary);
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::CallDirectEval {
                value_feedback,
                call_feedback,
                dst,
                callee: callee_operand,
                this_value,
                argument_count: u32_from_usize(arguments.len()),
                expression_string: None,
                arguments: &arguments,
            },
            dst,
        );
        self.context.stream.operands = arguments;
        self.push_value(dst, callee.first_temporary);
    }

    pub(super) fn direct_eval_with_array(&mut self) {
        let (arguments, _) = self.pop_operand();
        let callee = self.pop_value();
        let (callee_operand, this_value) = if callee.kind == ValKind::Callee {
            (callee.a, callee.b)
        } else {
            (callee.a, self.context.constants.add_undefined())
        };
        self.release_temporaries_from(callee.first_temporary);
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::CallDirectEvalWithArgumentArray {
                value_feedback,
                call_feedback,
                dst,
                callee: callee_operand,
                this_value,
                arguments,
                expression_string: None,
            },
            dst,
        );
        self.push_value(dst, callee.first_temporary);
    }

    pub(super) fn call_with_array(&mut self) {
        let (arguments, _) = self.pop_operand();
        let callee = self.pop_value();
        let (callee_operand, this_value) = if callee.kind == ValKind::Callee {
            (callee.a, callee.b)
        } else {
            (callee.a, self.context.constants.add_undefined())
        };
        self.release_temporaries_from(callee.first_temporary);
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::CallWithArgumentArray {
                value_feedback,
                call_feedback,
                dst,
                callee: callee_operand,
                this_value,
                arguments,
                expression_string: None,
            },
            dst,
        );
        self.push_value(dst, callee.first_temporary);
    }

    pub(super) fn construct_with_array(&mut self) {
        let (arguments, _) = self.pop_operand();
        let (callee, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let this_value = self.context.constants.add_undefined();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put_with_destination(
            op::CallConstructWithArgumentArray {
                value_feedback,
                call_feedback,
                dst,
                callee,
                this_value,
                arguments,
                expression_string: None,
            },
            dst,
        );
        self.push_value(dst, first_temporary);
    }

    /// A spread element comes up in a list, so that the `count` elements before it go into an array, and the ones after
    /// it get appended to that as they come.
    pub(super) fn spread_begin(&mut self, count: usize) {
        let first_temporary = if count == 0 {
            self.next_temporary()
        } else {
            let values = &self.context.stream.values;
            values[values.len() - count].first_temporary
        };
        let elements = self.take_operands(count);
        let dst = self.temporary();
        self.put(op::NewArray {
            dst,
            element_count: u32_from_usize(elements.len()),
            elements: &elements,
        });
        self.context.stream.operands = elements;
        self.release_temporaries_from(dst.index() + 1);
        self.push_value(dst, first_temporary);
        let position = self.current_source_offset.0;
        self.context.stream.values.last_mut().expect("a value was pushed").cache = position;
    }

    /// Appends the value on top of the stack to the array below it, or all that it iterates over if `is_spread`.
    pub(super) fn append_element(&mut self, is_spread: bool) {
        let (src, _) = self.pop_operand();
        let array = *self.context.stream.values.last().expect("an array is being built");
        // NB: Going through what gets spread is up to what the list belongs to.
        self.current_source_offset = SourceOffset(array.cache);
        let array = array.a;
        self.put(op::ArrayAppend {
            dst: array,
            src,
            is_spread,
        });
        self.release_temporaries_from(array.index() + 1);
    }

    // --- Optional chains ---

    /// `?.`, after the value on top of the stack: what follows only happens if that is neither `null` nor `undefined`.
    pub(super) fn optional_link(&mut self, is_first_of_chain: bool) {
        self.copy_locals_on_stack();
        if is_first_of_chain {
            let first_temporary = self
                .context
                .stream
                .values
                .last()
                .expect("a chain has a base")
                .first_temporary;
            let stream = &mut self.context.stream;
            // NB: What a copy of a local holds for the code around the chain stays there.
            let dst = first_temporary.max(stream.floor);
            stream.temporary_count = stream.temporary_count.max(dst + 1);
            let short_circuit = self.new_label();
            self.context.stream.controls.push(Control::Chain {
                dst: Operand::register(Register(dst)),
                short_circuit,
                first_temporary,
            });
        }
        let short_circuit = self
            .context
            .stream
            .controls
            .iter()
            .rev()
            .find_map(|control| match control {
                Control::Chain { short_circuit, .. } => Some(*short_circuit),
                _ => None,
            })
            .expect("a chain is going on");
        let condition = self.context.stream.values.last().expect("a chain has a base").a;
        let next = self.new_label();
        self.reference_label(short_circuit);
        self.reference_label(next);
        self.put(op::JumpNullish {
            condition,
            true_target: short_circuit,
            false_target: next,
        });
        self.bind_label(next);
    }

    /// What gets called on top of the stack is what an optional chain gives if it gets to its end, and if it does not,
    /// `undefined` is what gets called and what it gets called on.
    pub(super) fn chain_end_of_callee(&mut self) {
        let Some(Control::Chain {
            dst,
            short_circuit,
            first_temporary,
        }) = self.context.stream.controls.pop()
        else {
            unreachable!("an optional chain is what is going on")
        };
        let callee = self.pop_value();
        let this_value = self.temporary();
        self.put(op::Mov {
            dst: this_value,
            src: callee.b,
        });
        self.put(op::Mov { dst, src: callee.a });
        let end = self.new_label();
        self.jump(end);
        self.bind_label(short_circuit);
        let undefined = self.context.constants.add_undefined();
        self.put(op::Mov { dst, src: undefined });
        self.put(op::Mov {
            dst: this_value,
            src: undefined,
        });
        self.bind_label(end);
        self.push_callee(dst, this_value, first_temporary);
    }

    /// The value on top of the stack is what an optional chain gives if it gets to its end, and if it does not, it gives
    /// `true` if it is what a `delete` is about, or else `undefined`.
    pub(super) fn chain_end(&mut self, is_delete: bool) {
        let Some(Control::Chain {
            dst,
            short_circuit,
            first_temporary,
        }) = self.context.stream.controls.pop()
        else {
            unreachable!("an optional chain is what is going on")
        };
        let (value, _) = self.pop_operand();
        self.move_value_to(dst, value);
        let end = self.new_label();
        self.jump(end);
        self.bind_label(short_circuit);
        let src = if is_delete {
            self.context.constants.add_boolean(true)
        } else {
            self.context.constants.add_undefined()
        };
        self.put(op::Mov { dst, src });
        self.bind_label(end);
        self.release_temporaries_from(dst.index() + 1);
        self.push_value(dst, first_temporary);
    }

    // --- Operators ---

    #[inline(always)]
    pub(super) fn binary(&mut self, operator: BinaryOp) {
        let (rhs, _) = self.pop_operand();
        let (lhs, first_temporary) = self.pop_operand();
        if lhs.is_constant() && rhs.is_constant() {
            let folded =
                try_constant_fold_binary(self, operator, &ScopedOperand::plain(lhs), &ScopedOperand::plain(rhs));
            if let Some(folded) = folded {
                self.release_temporaries_from(first_temporary);
                self.push_value(folded.operand(), first_temporary);
                return;
            }
        }
        // NB: Operands of bitwise operators that are constants get converted right away.
        let rhs = self.bitwise_constant_operand(operator, rhs);
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let is_comparison = matches!(
            operator,
            BinaryOp::LessThan
                | BinaryOp::LessThanEquals
                | BinaryOp::GreaterThan
                | BinaryOp::GreaterThanEquals
                | BinaryOp::LooselyEquals
                | BinaryOp::LooselyInequals
                | BinaryOp::StrictlyEquals
                | BinaryOp::StrictlyInequals
        );
        let stream = &self.context.stream;
        let start = u32_from_usize(stream.current.bytes.len());
        let operand_position_count = u32_from_usize(stream.current.operand_positions.len());
        let arith_feedback_before = self.next_arith_feedback;
        emit_binary_op(
            self,
            operator,
            &ScopedOperand::plain(dst),
            &ScopedOperand::plain(lhs),
            &ScopedOperand::plain(rhs),
        );
        let stream = &mut self.context.stream;
        let end = u32_from_usize(stream.current.bytes.len());
        if !stream.current.dead {
            // NB: Every operator writes its destination, and that is the first operand.
            stream.last_destination = Some(LastDestination {
                end,
                position: start + DESTINATION_OFFSET_OF_BINARY_OPERATOR,
                dst,
            });
        }
        if is_comparison {
            stream.last_comparison = Some(LastComparison {
                start,
                end,
                operand_position_count,
                operator,
                arith_feedback: u16::try_from(arith_feedback_before).unwrap_or(u16::MAX),
                dst,
                lhs,
                rhs,
            });
        }
        self.push_value(dst, first_temporary);
    }

    fn bitwise_constant_operand(&mut self, operator: BinaryOp, rhs: Operand) -> Operand {
        if !rhs.is_constant() {
            return rhs;
        }
        let Some(ConstantValue::Number(number)) = self.context.constants.get(rhs) else {
            return rhs;
        };
        let number = *number;
        match operator {
            BinaryOp::BitwiseAnd | BinaryOp::BitwiseOr | BinaryOp::BitwiseXor => self
                .context
                .constants
                .add_number(f64::from(crate::bytecode::constant_fold::to_int32(number))),
            BinaryOp::LeftShift | BinaryOp::RightShift | BinaryOp::UnsignedRightShift => self
                .context
                .constants
                .add_number(f64::from(crate::bytecode::constant_fold::to_u32(number))),
            _ => rhs,
        }
    }

    pub(super) fn unary(&mut self, operator: UnaryOp) {
        let (value, first_temporary) = self.pop_operand();
        if value.is_constant()
            && let Some(folded) = try_constant_fold_unary(self, operator, &ScopedOperand::plain(value))
        {
            self.release_temporaries_from(first_temporary);
            self.push_value(folded.operand(), first_temporary);
            return;
        }
        if operator == UnaryOp::Void {
            self.release_temporaries_from(first_temporary);
            let undefined = self.context.constants.add_undefined();
            self.push_value(undefined, first_temporary);
            return;
        }
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        match operator {
            UnaryOp::Not => self.put_with_destination(op::Not { dst, src: value }, dst),
            UnaryOp::Typeof => self.put_with_destination(op::Typeof { dst, src: value }, dst),
            UnaryOp::BitwiseNot => {
                let arith_feedback = self.next_arith_feedback();
                self.put_with_destination(
                    op::BitwiseNot {
                        arith_feedback,
                        dst,
                        src: value,
                    },
                    dst,
                );
            }
            UnaryOp::Plus => {
                let arith_feedback = self.next_arith_feedback();
                self.put_with_destination(
                    op::UnaryPlus {
                        arith_feedback,
                        dst,
                        src: value,
                    },
                    dst,
                );
            }
            UnaryOp::Minus => {
                let arith_feedback = self.next_arith_feedback();
                self.put_with_destination(
                    op::UnaryMinus {
                        arith_feedback,
                        dst,
                        src: value,
                    },
                    dst,
                );
            }
            UnaryOp::Void | UnaryOp::Delete => unreachable!("these get handled elsewhere"),
        }
        self.push_value(dst, first_temporary);
    }

    /// `delete ident`.
    pub(super) fn value_of_delete_identifier(&mut self, ident: Identifier) {
        let first_temporary = self.next_temporary();
        if ident.is_local() {
            let no = self.context.constants.add_boolean(false);
            self.push_value(no, first_temporary);
            return;
        }
        let dst = self.temporary();
        let identifier = self.intern_identifier(ident.name);
        self.emit(op::DeleteVariable { dst, identifier });
        self.push_value(dst, first_temporary);
    }

    /// `delete` of the value on top of the stack, which is no reference.
    pub(super) fn delete_value(&mut self) {
        let (_, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let yes = self.context.constants.add_boolean(true);
        self.push_value(yes, first_temporary);
    }

    pub(super) fn value_of_bigint(&mut self, digits: String) {
        let operand = self.add_constant_bigint(digits).operand();
        self.value_of_constant(operand);
    }

    pub(super) fn value_of_import_meta(&mut self) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        self.put(op::GetImportMeta { dst });
        self.push_value(dst, first_temporary);
    }

    /// `import(specifier, options)`, for the two on top of the stack.
    pub(super) fn import_call(&mut self, has_options: bool) {
        let options = if has_options {
            self.pop_operand().0
        } else {
            self.context.constants.add_undefined()
        };
        let (specifier, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        self.put(op::ImportCall {
            dst,
            specifier,
            options,
        });
        self.push_value(dst, first_temporary);
    }

    /// `delete` of the target on top of the stack.
    pub(super) fn delete_target(&mut self) {
        let target = self.pop_value();
        self.release_temporaries_from(target.first_temporary);
        let dst = self.temporary();
        match target.kind {
            ValKind::MemberTarget => {
                let property = self.intern_property_key(Name(target.name));
                self.put_with_destination(
                    op::DeleteById {
                        dst,
                        base: target.a,
                        property,
                    },
                    dst,
                );
            }
            ValKind::IndexTarget => {
                self.put_with_destination(
                    op::DeleteByValue {
                        dst,
                        base: target.a,
                        property: target.b,
                    },
                    dst,
                );
            }
            ValKind::SuperTarget => {
                let error_string = self.intern_string_text(utf16!("Can't delete a property on 'super'").into());
                self.put(op::NewReferenceError { dst, error_string });
                self.put(op::Throw { src: dst });
            }
            _ => unreachable!("only a reference to a property gets here"),
        }
        self.push_value(dst, target.first_temporary);
    }

    /// The value on top of the stack is what code tries to write to, which takes an exception once it got evaluated.
    pub(super) fn invalid_target(&mut self) {
        let (_, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let error_string = self.intern_string_text(utf16!("Invalid left-hand side in assignment").into());
        self.put(op::NewReferenceError { dst, error_string });
        self.put(op::Throw { src: dst });
        self.context.stream.values.push(Val {
            kind: ValKind::InvalidTarget,
            first_temporary,
            a: dst,
            b: Operand::invalid(),
            name: 0,
            cache: u32::MAX,
            is_global: false,
            is_const: false,
            is_var: false,
        });
    }

    /// A write to the invalid target on top of the stack is an expression like any other for what comes after it, which is
    /// code that never runs.
    pub(super) fn result_of_invalid_target(&mut self) {
        let value = self.context.stream.values.last_mut().expect("something was evaluated");
        debug_assert!(value.kind == ValKind::InvalidTarget);
        value.kind = ValKind::Value;
    }

    // --- Suspending ---

    /// Awaits the value on top of the stack.
    pub(super) fn await_value(&mut self) {
        let (value, first_temporary) = self.pop_operand();
        let held = self.hold_registers();
        let result = generate_await_of_value(self, &ScopedOperand::plain(value));
        self.take_result_of_held_registers(result.operand(), held, first_temporary);
    }

    /// Yields the value on top of the stack if there is one, or what it iterates over if `is_yield_from`.
    pub(super) fn yield_value(&mut self, has_value: bool, is_yield_from: bool) {
        let (value, first_temporary) = if has_value {
            self.pop_operand()
        } else {
            (self.context.constants.add_undefined(), self.next_temporary())
        };
        let held = self.hold_registers();
        let received_completion = self.allocate_register();
        let received_completion_type = self.allocate_register();
        let received_completion_value = self.allocate_register();
        let result = generate_yield_of_value(
            self,
            ScopedOperand::plain(value),
            is_yield_from,
            &received_completion,
            &received_completion_type,
            &received_completion_value,
        );
        self.take_result_of_held_registers(result.operand(), held, first_temporary);
    }

    /// Makes `result` the value of an expression whose code took the registers since `held`.
    fn take_result_of_held_registers(&mut self, result: Operand, held: HeldRegisters, first_temporary: u32) {
        self.release_held_registers(held);
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        self.put(op::Mov { dst, src: result });
        self.push_value(dst, first_temporary);
    }

    // --- Branches inside expressions ---

    /// The value on top of the stack is the test of a conditional expression.
    pub(super) fn conditional_after_test(&mut self) {
        let (test, first_temporary) = self.pop_operand();
        self.copy_locals_on_stack();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let else_label = self.new_label();
        let end_label = self.new_label();
        self.jump_unless(test, else_label);
        self.context.stream.controls.push(Control::Conditional {
            dst,
            else_label,
            end_label,
            first_temporary,
        });
    }

    pub(super) fn conditional_after_consequent(&mut self) {
        let Some(&Control::Conditional {
            dst,
            else_label,
            end_label,
            ..
        }) = self.context.stream.controls.last()
        else {
            unreachable!("a conditional expression is what is going on")
        };
        let (value, _) = self.pop_operand();
        self.move_value_to(dst, value);
        self.release_temporaries_from(dst.index() + 1);
        self.jump(end_label);
        self.bind_label(else_label);
    }

    pub(super) fn conditional_end(&mut self) {
        let Some(Control::Conditional {
            dst,
            end_label,
            first_temporary,
            ..
        }) = self.context.stream.controls.pop()
        else {
            unreachable!("a conditional expression is what is going on")
        };
        let (value, _) = self.pop_operand();
        self.move_value_to(dst, value);
        self.release_temporaries_from(dst.index() + 1);
        self.bind_label(end_label);
        self.push_value(dst, first_temporary);
    }

    /// The value on top of the stack is the left side of `&&`, `||` or `??`.
    pub(super) fn logical_after_left(&mut self, operator: crate::ast::LogicalOp) {
        use crate::ast::LogicalOp;
        let (left, first_temporary) = self.pop_operand();
        self.copy_locals_on_stack();
        self.release_temporaries_from(first_temporary);
        let dst = self.temporary();
        let end_label = self.new_label();
        self.move_value_to(dst, left);
        // NB: The value of a comparison is what the expression gives if it short-circuits.
        self.context.stream.last_comparison = None;
        match operator {
            LogicalOp::And => self.jump_unless(dst, end_label),
            LogicalOp::Or => {
                self.reference_label(end_label);
                self.put_conditional(op::JumpTrue {
                    condition: dst,
                    target: end_label,
                });
            }
            LogicalOp::NullishCoalescing => {
                let next = self.new_label();
                self.reference_label(end_label);
                self.reference_label(next);
                self.put(op::JumpNullish {
                    condition: dst,
                    true_target: next,
                    false_target: end_label,
                });
                self.bind_label(next);
            }
        }
        self.context.stream.controls.push(Control::Logical {
            dst,
            end_label,
            first_temporary,
        });
    }

    pub(super) fn logical_end(&mut self) {
        let Some(Control::Logical {
            dst,
            end_label,
            first_temporary,
        }) = self.context.stream.controls.pop()
        else {
            unreachable!("a logical expression is what is going on")
        };
        let (value, _) = self.pop_operand();
        self.move_value_to(dst, value);
        self.release_temporaries_from(dst.index() + 1);
        self.bind_label(end_label);
        self.push_value(dst, first_temporary);
    }

    /// Drops all but the last of `count` values on top of the stack.
    pub(super) fn sequence(&mut self, count: usize) {
        let stream = &mut self.context.stream;
        let last = stream.values.pop().expect("a sequence has values");
        let start = stream.values.len() + 1 - count;
        let first_temporary = stream.values[start].first_temporary;
        stream.values.truncate(start);
        stream.values.push(Val {
            first_temporary,
            ..last
        });
    }

    // --- Targets ---

    pub(super) fn target_of_identifier(&mut self, ident: Identifier) {
        let first_temporary = self.next_temporary();
        let is_const = ident.declaration_kind == Some(DeclarationKind::Const);
        if ident.is_local() {
            let local = self.local_operand(&ident);
            self.context.stream.values.push(Val {
                kind: ValKind::LocalTarget,
                first_temporary,
                a: local,
                b: Operand::invalid(),
                name: ident.name.0,
                cache: u32::MAX,
                is_global: false,
                is_const,
                is_var: false,
            });
            return;
        }
        // NB: Unless it is known where the binding is, the reference is what it is before the value gets evaluated.
        let mut resolved = Operand::invalid();
        let mut cache = u32::MAX;
        if !ident.is_global || self.strict {
            let identifier = self.intern_identifier(ident.name);
            if self.environment_coordinate_for_identifier(identifier).is_none() {
                resolved = self.temporary();
                if ident.is_global {
                    cache = self.next_global_variable_cache();
                    self.put(op::ResolveGlobalBinding {
                        dst: resolved,
                        identifier,
                        cache,
                    });
                } else {
                    self.put(op::ResolveBinding {
                        dst: resolved,
                        identifier,
                    });
                }
            }
        }
        self.context.stream.values.push(Val {
            kind: ValKind::NameTarget,
            first_temporary,
            a: resolved,
            b: Operand::invalid(),
            name: ident.name.0,
            cache,
            is_global: ident.is_global,
            is_const,
            is_var: ident.declaration_kind == Some(DeclarationKind::Var),
        });
    }

    /// `value.name` is what gets written, with the value on top of the stack.
    #[inline(always)]
    pub(super) fn target_of_member(&mut self, name: Name) {
        let value = self.context.stream.values.last_mut().expect("something was evaluated");
        value.kind = ValKind::MemberTarget;
        value.name = name.0;
    }

    pub(super) fn target_of_index(&mut self) {
        let (key, _) = self.pop_operand();
        let value = self.context.stream.values.last_mut().expect("something was evaluated");
        value.kind = ValKind::IndexTarget;
        value.b = key;
    }

    /// Stores `value` to `target`.
    pub(super) fn store_to_target(&mut self, target: Val, value: Operand, initializes: bool) {
        match target.kind {
            ValKind::LocalTarget => self.store_to_local(target.a, value, target.is_const && !initializes),
            ValKind::NameTarget => {
                let identifier = self.intern_identifier(Name(target.name));
                if target.a != Operand::invalid() {
                    let resolved_binding = if target.is_global {
                        ResolvedBinding::Global {
                            is_resolvable: ScopedOperand::plain(target.a),
                            identifier,
                            cache: target.cache,
                        }
                    } else {
                        ResolvedBinding::Environment {
                            environment: ScopedOperand::plain(target.a),
                            identifier,
                        }
                    };
                    emit_set_resolved_binding(self, &resolved_binding, &ScopedOperand::plain(value));
                } else if target.is_global {
                    let cache = self.next_global_variable_cache();
                    self.put(op::SetGlobal {
                        identifier,
                        src: value,
                        cache,
                    });
                } else if initializes {
                    emit_initialize_lexical_binding(self, identifier, value);
                } else {
                    emit_set_lexical_binding(self, identifier, value);
                }
            }
            ValKind::MemberTarget => {
                let key = self.intern_property_key(Name(target.name));
                let cache = self.next_property_lookup_cache();
                self.put(op::PutById {
                    base: target.a,
                    property: key,
                    src: value,
                    cache,
                    base_identifier: None,
                    kind: libjs_abi::PutKind::Normal as u32,
                });
            }
            ValKind::IndexTarget => {
                emit_put_normal_by_value(
                    self,
                    &ScopedOperand::plain(target.a),
                    &ScopedOperand::plain(target.b),
                    &ScopedOperand::plain(value),
                    None,
                );
            }
            ValKind::SuperTarget => {
                let this_value = ScopedOperand::plain(Operand::register(Register::THIS_VALUE));
                let base = ScopedOperand::plain(target.a);
                let value = ScopedOperand::plain(value);
                if target.b != Operand::invalid() {
                    let key = ScopedOperand::plain(target.b);
                    emit_put_normal_by_value_with_this(self, &base, &key, &this_value, &value);
                } else {
                    let key = self.intern_property_key(Name(target.name));
                    let cache = self.next_property_lookup_cache();
                    self.put(op::PutByIdWithThis {
                        base: base.operand(),
                        this_value: this_value.operand(),
                        property: key,
                        src: value.operand(),
                        cache,
                        kind: libjs_abi::PutKind::Normal as u32,
                    });
                }
            }
            ValKind::PrivateTarget => {
                let property = intern_private_name(self, Name(target.name));
                self.put(op::PutPrivateById {
                    base: target.a,
                    property,
                    src: value,
                });
            }
            ValKind::InvalidTarget => {}
            ValKind::Value | ValKind::Callee | ValKind::SuperThis | ValKind::BuiltinCallee => {
                unreachable!("this is no target")
            }
        }
    }

    #[inline(always)]
    fn store_to_local(&mut self, local: Operand, value: Operand, is_const: bool) {
        if is_const {
            self.put(op::ThrowConstAssignment {});
            return;
        }
        self.before_write_to_local(local);
        self.move_value_to(local, value);
    }

    /// Whether the local being written has to be checked for being initialized.
    pub(super) fn check_target_is_initialized(&mut self, ident: &Identifier) {
        if ident.is_local() {
            let local = self.local_operand(ident);
            self.check_local_is_initialized(ident, local);
        }
    }

    /// A binding that a declaration declared gets `value`, in a way that depends on the `kind` of declaration.
    pub(super) fn store_declared(&mut self, ident: Identifier, value: Operand, kind: DeclarationKind) {
        self.target_of_declaration(ident);
        let target = self.pop_value();
        self.store_to_target(target, value, kind != DeclarationKind::Var);
        if ident.local_type == Some(LocalType::Variable) {
            self.mark_local_initialized(ident.local_index);
        }
    }

    /// `target = value`, for the two on top of the stack.
    pub(super) fn assign(&mut self, target_ident: Option<Identifier>) {
        let (value, _) = self.pop_operand();
        let target = self.pop_value();
        if let Some(ident) = target_ident {
            self.check_target_is_initialized(&ident);
        }
        self.store_to_target(target, value, false);
        let result = if target.kind == ValKind::LocalTarget && !target.is_const {
            target.a
        } else {
            value
        };
        // NB: A value in a temporary stays where it is, and the registers above it stay taken with it.
        if result.is_register() && result.index() >= target.first_temporary {
            self.release_temporaries_from(result.index() + 1);
        } else {
            self.release_temporaries_from(target.first_temporary);
        }
        self.push_value(result, target.first_temporary);
    }

    /// Reads what is on top of the stack for a compound assignment or an update, keeping the target on the stack.
    pub(super) fn read_target(&mut self, target_ident: Option<Identifier>) {
        let target = *self.context.stream.values.last().expect("a target was evaluated");
        let first_temporary = self.next_temporary();
        match target.kind {
            ValKind::LocalTarget => {
                if let Some(ident) = target_ident {
                    self.check_target_is_initialized(&ident);
                }
                // NB: Whatever the right side does to the local, the old value is what counts.
                self.push_value(target.a, first_temporary);
            }
            ValKind::NameTarget => {
                let dst = self.temporary();
                let identifier = self.intern_identifier(Name(target.name));
                if target.a != Operand::invalid() && !target.is_global {
                    self.put(op::GetResolvedBinding {
                        dst,
                        environment: target.a,
                        identifier,
                    });
                } else if target.is_global {
                    let cache = self.next_global_variable_cache();
                    let value_feedback = self.next_value_feedback();
                    self.put(op::GetGlobal {
                        value_feedback,
                        dst,
                        identifier,
                        cache,
                    });
                } else {
                    emit_get_binding(self, dst, identifier, target.is_var);
                }
                self.push_value(dst, first_temporary);
            }
            ValKind::MemberTarget => {
                let dst = self.temporary();
                self.put_get_by_id(dst, target.a, Name(target.name));
                self.context.stream.last_destination = None;
                self.push_value(dst, first_temporary);
            }
            ValKind::IndexTarget => {
                let key = self.read_modify_write_key(target.a, target.b);
                self.context.stream.values.last_mut().expect("a target was evaluated").b = key;
                let dst = self.temporary();
                self.put_get_by_value(dst, target.a, key);
                self.push_value(dst, first_temporary);
            }
            ValKind::SuperTarget => {
                let dst = self.temporary();
                let this_value = ScopedOperand::plain(Operand::register(Register::THIS_VALUE));
                let base = ScopedOperand::plain(target.a);
                if target.b != Operand::invalid() {
                    let key = self.read_modify_write_key(target.a, target.b);
                    self.context.stream.values.last_mut().expect("a target was evaluated").b = key;
                    let key = ScopedOperand::plain(key);
                    emit_get_by_value_with_this(self, &ScopedOperand::plain(dst), &base, &key, &this_value);
                } else {
                    emit_get_by_id_with_this(self, &ScopedOperand::plain(dst), &base, Name(target.name), &this_value);
                }
                self.push_value(dst, first_temporary);
            }
            ValKind::PrivateTarget => {
                let dst = self.temporary();
                let property = intern_private_name(self, Name(target.name));
                self.put(op::GetPrivateById {
                    dst,
                    base: target.a,
                    property,
                });
                self.push_value(dst, first_temporary);
            }
            ValKind::InvalidTarget | ValKind::Value | ValKind::Callee | ValKind::SuperThis | ValKind::BuiltinCallee => {
                unreachable!("this is no target")
            }
        }
    }

    fn read_modify_write_key(&mut self, base: Operand, key: Operand) -> Operand {
        if key.is_constant() {
            return key;
        }
        self.put(op::ThrowIfNullish { src: base });
        let converted = self.temporary();
        self.put(op::ToPrimitiveWithStringHint {
            dst: converted,
            value: key,
        });
        converted
    }

    /// The old value of the target of `&&=`, `||=` or `??=` is on top of the stack, and the value that may replace it comes
    /// next.
    pub(super) fn logical_assignment_after_read(&mut self, operator: crate::ast::LogicalOp) {
        use crate::ast::LogicalOp;
        let (old_value, _) = self.pop_operand();
        self.copy_locals_on_stack();
        let dst = self.temporary();
        let stream = &mut self.context.stream;
        stream.floor = stream.next_temporary;
        self.put(op::Mov { dst, src: old_value });
        let end_label = self.new_label();
        match operator {
            LogicalOp::And => self.jump_unless(dst, end_label),
            LogicalOp::Or => self.jump_if(dst, end_label),
            LogicalOp::NullishCoalescing => {
                let next = self.new_label();
                self.reference_label(end_label);
                self.reference_label(next);
                self.put(op::JumpNullish {
                    condition: dst,
                    true_target: next,
                    false_target: end_label,
                });
                self.bind_label(next);
            }
        }
        self.context
            .stream
            .controls
            .push(Control::LogicalAssignment { dst, end_label });
    }

    /// `target &&= value`, for the two on top of the stack.
    pub(super) fn logical_assignment_end(&mut self) {
        let Some(Control::LogicalAssignment { dst, end_label }) = self.context.stream.controls.pop() else {
            unreachable!("a logical assignment is what is going on")
        };
        let (value, _) = self.pop_operand();
        self.move_value_to(dst, value);
        let target = self.pop_value();
        self.store_to_target(target, dst, false);
        self.bind_label(end_label);
        self.release_temporaries_from(dst.index() + 1);
        self.push_value(dst, target.first_temporary);
    }

    /// `target op= value`, for the target, the old value and the new one on top of the stack.
    pub(super) fn compound_assign(&mut self, operator: BinaryOp) {
        let target_index = self.context.stream.values.len() - 3;
        let target = self.context.stream.values[target_index];
        let (rhs, _) = self.pop_operand();
        let (lhs, _) = self.pop_operand();
        let dst = if target.kind == ValKind::LocalTarget && !target.is_const {
            self.before_write_to_local(target.a);
            target.a
        } else {
            self.temporary()
        };
        emit_binary_op(
            self,
            operator,
            &ScopedOperand::plain(dst),
            &ScopedOperand::plain(lhs),
            &ScopedOperand::plain(rhs),
        );
        let target = self.pop_value();
        if target.kind != ValKind::LocalTarget || target.is_const {
            self.store_to_target(target, dst, false);
        }
        if dst.is_register() {
            self.release_temporaries_from(dst.index() + 1);
        } else {
            self.release_temporaries_from(target.first_temporary);
        }
        self.push_value(dst, target.first_temporary);
    }

    /// `++` or `--` of the target on top of the stack, with its old value above it, and whether anything looks at the
    /// value of the expression.
    pub(super) fn update(&mut self, operator: UpdateOp, prefixed: bool, value_is_used: bool) {
        let (old_value, _) = self.pop_operand();
        let target = self.pop_value();
        if target.kind == ValKind::LocalTarget {
            let local = target.a;
            if target.is_const {
                // NB: The value gets converted to a number before the write fails, and stays what it is.
                let copy = self.temporary();
                self.put(op::Mov { dst: copy, src: local });
                let old_value = self.temporary();
                self.put_postfix_update(operator, old_value, copy);
                self.put(op::ThrowConstAssignment {});
                self.push_value(local, target.first_temporary);
                return;
            }
            self.before_write_to_local(local);
            if prefixed || !value_is_used {
                let arith_feedback = self.next_arith_feedback();
                match operator {
                    UpdateOp::Increment => self.put(op::Increment {
                        arith_feedback,
                        dst: local,
                    }),
                    UpdateOp::Decrement => self.put(op::Decrement {
                        arith_feedback,
                        dst: local,
                    }),
                }
                self.push_value(local, target.first_temporary);
                return;
            }
            self.release_temporaries_from(target.first_temporary);
            let dst = self.temporary();
            self.put_postfix_update(operator, dst, local);
            self.push_value(dst, target.first_temporary);
            return;
        }
        // NB: The old value is in a temporary of its own by now.
        let value = old_value;
        let result = if prefixed {
            let arith_feedback = self.next_arith_feedback();
            match operator {
                UpdateOp::Increment => self.put(op::Increment {
                    arith_feedback,
                    dst: value,
                }),
                UpdateOp::Decrement => self.put(op::Decrement {
                    arith_feedback,
                    dst: value,
                }),
            }
            value
        } else {
            let dst = self.temporary();
            self.put_postfix_update(operator, dst, value);
            dst
        };
        self.store_to_target(target, value, false);
        self.release_temporaries_from(result.index() + 1);
        self.push_value(result, target.first_temporary);
    }

    fn put_postfix_update(&mut self, operator: UpdateOp, dst: Operand, src: Operand) {
        let arith_feedback = self.next_arith_feedback();
        match operator {
            UpdateOp::Increment => self.put(op::PostfixIncrement {
                arith_feedback,
                dst,
                src,
            }),
            UpdateOp::Decrement => self.put(op::PostfixDecrement {
                arith_feedback,
                dst,
                src,
            }),
        }
    }

    // --- Literals ---

    /// An array of the `count` values on top of the stack.
    pub(super) fn array(&mut self, count: usize) {
        let first_temporary = if count == 0 {
            self.next_temporary()
        } else {
            let values = &self.context.stream.values;
            values[values.len() - count].first_temporary
        };
        let elements = self.take_operands(count);
        // NB: The array exists before the elements go in, so that the temporaries of the elements stay as they are.
        let dst = self.temporary();
        if !elements.is_empty() && self.put_primitive_array(dst, &elements) {
            self.context.stream.operands = elements;
            self.release_temporaries_from(dst.index() + 1);
            self.push_value(dst, first_temporary);
            return;
        }
        self.put_with_destination(
            op::NewArray {
                dst,
                element_count: u32_from_usize(elements.len()),
                elements: &elements,
            },
            dst,
        );
        self.context.stream.operands = elements;
        self.release_temporaries_from(dst.index() + 1);
        self.push_value(dst, first_temporary);
    }

    /// An array of nothing but numbers, booleans, nulls and holes comes with its values.
    fn put_primitive_array(&mut self, dst: Operand, elements: &[Operand]) -> bool {
        use crate::bytecode::constant_fold::*;
        let mut values = std::mem::take(&mut self.context.primitive_values);
        values.clear();
        for &element in elements {
            let value = match self.context.constants.get(element) {
                Some(ConstantValue::Number(number)) => nanboxed_number(*number),
                Some(ConstantValue::Boolean(boolean)) => nanboxed_boolean(*boolean),
                Some(ConstantValue::Null) => nanboxed_null(),
                Some(ConstantValue::Empty) => nanboxed_empty(),
                _ => {
                    self.context.primitive_values = values;
                    return false;
                }
            };
            values.push(value);
        }
        self.put_with_destination(
            op::NewPrimitiveArray {
                dst,
                element_count: u32_from_usize(values.len()),
                elements: &values,
            },
            dst,
        );
        self.context.primitive_values = values;
        true
    }

    /// An object literal starts, whose members are all plain properties if `is_simple`.
    pub(super) fn object_begin(&mut self, is_simple: bool) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        let cache = if is_simple {
            self.next_object_shape_cache()
        } else {
            u32::MAX
        };
        self.put(op::NewObject { dst, cache });
        self.push_value(dst, first_temporary);
        self.context
            .stream
            .values
            .last_mut()
            .expect("the object was just pushed")
            .cache = cache;
    }

    /// `name: value`, with the value and the object on top of the stack.
    pub(super) fn object_property(&mut self, key: PropertyKeyTableIndex) {
        let (value, _) = self.pop_operand();
        let object = self.context.stream.values.last_mut().expect("an object is being built");
        let (dst, cache, slot) = (object.a, object.cache, object.name);
        object.name += 1;
        if cache != u32::MAX {
            self.put(op::InitObjectLiteralProperty {
                object: dst,
                property: key,
                src: value,
                shape_cache_index: cache,
                property_slot: slot,
            });
        } else {
            let cache = self.next_property_lookup_cache();
            self.put(op::PutById {
                base: dst,
                property: key,
                src: value,
                cache,
                base_identifier: None,
                kind: libjs_abi::PutKind::Own as u32,
            });
        }
        self.release_temporaries_from(dst.index() + 1);
    }

    /// `__proto__: value`, with the value and the object on top of the stack.
    pub(super) fn object_prototype_property(&mut self) {
        let key = self.intern_property_key_text(crate::utf16::Utf16View::Ascii(b"__proto__"));
        self.object_put_by_id(key, libjs_abi::PutKind::Prototype);
    }

    /// What goes through a property key of the source, with the value and the object on top of the stack.
    pub(super) fn object_put_by_id(&mut self, key: PropertyKeyTableIndex, kind: libjs_abi::PutKind) {
        let (value, _) = self.pop_operand();
        let object = self.context.stream.values.last().expect("an object is being built").a;
        let cache = self.next_property_lookup_cache();
        self.put(op::PutById {
            base: object,
            property: key,
            src: value,
            cache,
            base_identifier: None,
            kind: kind as u32,
        });
        self.release_temporaries_from(object.index() + 1);
    }

    /// `1: value`, with the value and the object on top of the stack.
    pub(super) fn object_number_property(&mut self, key: f64, kind: libjs_abi::PutKind) {
        let (value, _) = self.pop_operand();
        let key = self.context.constants.add_number(key);
        let object = self.context.stream.values.last().expect("an object is being built").a;
        emit_put_by_value(
            self,
            &ScopedOperand::plain(object),
            &ScopedOperand::plain(key),
            &ScopedOperand::plain(value),
            kind,
        );
        self.release_temporaries_from(object.index() + 1);
    }

    /// `...value`, with the value and the object on top of the stack.
    pub(super) fn object_spread(&mut self) {
        let (src, _) = self.pop_operand();
        let object = self.context.stream.values.last().expect("an object is being built").a;
        self.put(op::PutBySpread { base: object, src });
        self.release_temporaries_from(object.index() + 1);
    }

    /// A method of the kind `kind` with a computed key, with the function, the key and the object on top of the stack.
    pub(super) fn object_computed_method(&mut self, kind: libjs_abi::PutKind, prefix: FunctionNamePrefix) {
        let (value, _) = self.pop_operand();
        let (key, _) = self.pop_operand();
        let object = self.context.stream.values.last().expect("an object is being built").a;
        self.put(op::SetFunctionName {
            function: value,
            name: key,
            prefix: prefix as u32,
        });
        emit_put_by_value(
            self,
            &ScopedOperand::plain(object),
            &ScopedOperand::plain(key),
            &ScopedOperand::plain(value),
            kind,
        );
        self.release_temporaries_from(object.index() + 1);
    }

    /// The object that is being built, below `distance` values on top of the stack.
    pub(super) fn object_below(&self, distance: usize) -> Operand {
        let values = &self.context.stream.values;
        values[values.len() - 1 - distance].a
    }

    /// `[key]: value`, with the value, the key and the object on top of the stack. An anonymous function takes the key
    /// as its name, if `value_is_anonymous_function`.
    pub(super) fn object_computed_property(&mut self, value_is_anonymous_function: bool) {
        let (value, _) = self.pop_operand();
        let (key, _) = self.pop_operand();
        if value_is_anonymous_function {
            self.put(op::SetFunctionName {
                function: value,
                name: key,
                prefix: FunctionNamePrefix::None as u32,
            });
        }
        let object = *self.context.stream.values.last().expect("an object is being built");
        emit_put_by_value(
            self,
            &ScopedOperand::plain(object.a),
            &ScopedOperand::plain(key),
            &ScopedOperand::plain(value),
            libjs_abi::PutKind::Own,
        );
        self.release_temporaries_from(object.a.index() + 1);
    }

    /// The key on top of the stack is a computed one.
    pub(super) fn object_computed_key(&mut self) {
        let (key, first_temporary) = self.pop_operand();
        if key.is_constant() {
            self.push_value(key, first_temporary);
            return;
        }
        let dst = if key.is_register() && key.index() >= first_temporary {
            key
        } else {
            self.temporary()
        };
        self.put(op::ToPrimitiveWithStringHint { dst, value: key });
        self.push_value(dst, first_temporary);
    }

    pub(super) fn object_end(&mut self) {
        let object = self.context.stream.values.last_mut().expect("an object is being built");
        let (dst, cache) = (object.a, object.cache);
        object.name = 0;
        if cache != u32::MAX {
            self.put(op::CacheObjectShape { object: dst, cache });
        }
    }

    pub(super) fn value_of_regular_expression(
        &mut self,
        source_index: StringTableIndex,
        flags_index: StringTableIndex,
    ) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        self.put_with_destination(
            op::NewRegExp {
                dst,
                source_index,
                flags_index,
            },
            dst,
        );
        self.push_value(dst, first_temporary);
    }

    /// A function object for the function at index `shared_function_data_index`.
    pub(super) fn value_of_function(
        &mut self,
        shared_function_data_index: u32,
        lhs_name: Option<IdentifierTableIndex>,
        home_object: Option<Operand>,
    ) {
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        self.put_with_destination(
            op::NewFunction {
                dst,
                shared_function_data_index,
                lhs_name,
                home_object,
            },
            dst,
        );
        self.push_value(dst, first_temporary);
    }

    // --- Statements ---

    /// What the statement evaluated is of no use.
    #[inline(always)]
    pub(super) fn end_of_expression_statement(&mut self) {
        let value = self.pop_value();
        if let Some(completion) = self.context.stream.completion {
            self.move_value_to(completion, value.a);
        }
        self.end_of_statement();
    }

    /// The code keeps track of the value of its last statement from here on.
    pub(super) fn begin_completion(&mut self) {
        let completion = self.held_register();
        self.context.stream.completion = Some(completion);
        self.reset_completion();
    }

    /// A statement starts whose value is `undefined` unless something in it has one.
    pub(super) fn reset_completion(&mut self) {
        if let Some(completion) = self.context.stream.completion {
            let undefined = self.context.constants.add_undefined();
            self.put(op::Mov {
                dst: completion,
                src: undefined,
            });
        }
    }

    /// A `finally` clause starts, whose value goes away unless it ends abruptly. Gives where the value before it is.
    pub(super) fn save_completion(&mut self) -> Option<Operand> {
        let completion = self.context.stream.completion?;
        let saved = self.held_register();
        self.put(op::Mov {
            dst: saved,
            src: completion,
        });
        self.reset_completion();
        Some(saved)
    }

    pub(super) fn restore_completion(&mut self, saved: Option<Operand>) {
        if let (Some(completion), Some(saved)) = (self.context.stream.completion, saved) {
            self.put(op::Mov {
                dst: completion,
                src: saved,
            });
        }
    }

    #[inline(always)]
    pub(super) fn end_of_statement(&mut self) {
        let stream = &mut self.context.stream;
        stream.floor = stream.first_temporary;
        stream.next_temporary = stream.first_temporary;
    }

    pub(super) fn declare(
        &mut self,
        target_ident: Identifier,
        has_initializer: bool,
        kind: DeclarationKind,
        resource: Option<bool>,
    ) {
        if has_initializer {
            let (value, _) = self.pop_operand();
            if let Some(is_async) = resource {
                self.add_disposable_resource(value, is_async);
            }
            let target = self.pop_value();
            self.store_to_target(target, value, kind != DeclarationKind::Var);
        } else {
            let target = self.pop_value();
            if kind != DeclarationKind::Var {
                let undefined = self.context.constants.add_undefined();
                self.store_to_target(target, undefined, true);
            }
        }
        if target_ident.local_type == Some(LocalType::Variable) {
            self.mark_local_initialized(target_ident.local_index);
        }
        self.end_of_statement();
    }

    pub(super) fn if_after_test(&mut self) {
        let (test, _) = self.pop_operand();
        self.end_of_statement();
        let else_label = self.new_label();
        self.jump_unless(test, else_label);
        self.context.stream.controls.push(Control::If {
            else_label,
            end_label: None,
        });
    }

    pub(super) fn if_else(&mut self) {
        let end = self.new_label();
        let Some(Control::If { else_label, end_label }) = self.context.stream.controls.last_mut() else {
            unreachable!("an if statement is what is going on")
        };
        *end_label = Some(end);
        let else_label = *else_label;
        self.jump(end);
        self.bind_label(else_label);
    }

    pub(super) fn if_end(&mut self) {
        let Some(Control::If { else_label, end_label }) = self.context.stream.controls.pop() else {
            unreachable!("an if statement is what is going on")
        };
        self.bind_label(end_label.unwrap_or(else_label));
    }

    pub(super) fn while_head(&mut self) {
        let head = self.new_label();
        let break_label = self.new_label();
        self.reference_label(head);
        self.bind_label(head);
        let break_depth = self.control_depth() + 1;
        self.context.stream.controls.push(Control::Loop {
            continue_label: head,
            break_label,
            break_depth,
            kind: LoopKind::While { head },
        });
    }

    pub(super) fn while_after_test(&mut self) {
        let (test, _) = self.pop_operand();
        self.end_of_statement();
        let Some(&Control::Loop { break_label, .. }) = self.context.stream.controls.last() else {
            unreachable!("a loop is what is going on")
        };
        self.jump_unless(test, break_label);
    }

    pub(super) fn while_end(&mut self) {
        let Some(Control::Loop {
            break_label,
            kind: LoopKind::While { head },
            ..
        }) = self.context.stream.controls.pop()
        else {
            unreachable!("a while loop is what is going on")
        };
        self.jump_back(head);
        self.bind_label(break_label);
    }

    pub(super) fn do_head(&mut self) {
        let head = self.new_label();
        let continue_label = self.new_label();
        let break_label = self.new_label();
        self.reference_label(head);
        self.bind_label(head);
        let break_depth = self.control_depth() + 1;
        self.context.stream.controls.push(Control::Loop {
            continue_label,
            break_label,
            break_depth,
            kind: LoopKind::DoWhile { head },
        });
    }

    pub(super) fn do_before_test(&mut self) {
        let Some(&Control::Loop { continue_label, .. }) = self.context.stream.controls.last() else {
            unreachable!("a loop is what is going on")
        };
        self.bind_label(continue_label);
    }

    pub(super) fn do_end(&mut self) {
        let Some(Control::Loop {
            break_label,
            kind: LoopKind::DoWhile { head },
            ..
        }) = self.context.stream.controls.pop()
        else {
            unreachable!("a do loop is what is going on")
        };
        let (test, _) = self.pop_operand();
        self.end_of_statement();
        self.jump_back_if(test, head);
        self.bind_label(break_label);
    }

    /// `break`, out of the statement that `label` names if it has one.
    pub(super) fn break_statement(&mut self, label: Option<Name>) {
        let exit = self
            .context
            .stream
            .controls
            .iter()
            .enumerate()
            .rev()
            .find_map(|(index, control)| match (control, label) {
                (
                    Control::Loop {
                        break_label,
                        break_depth,
                        ..
                    },
                    None,
                ) => Some(Exit::Jump {
                    depth: *break_depth,
                    target: *break_label,
                }),
                (Control::Switch { break_label }, None) => Some(Exit::Jump {
                    depth: u32_from_usize(index + 1),
                    target: *break_label,
                }),
                (Control::Labeled { name, break_label }, Some(label)) if *name == label => Some(Exit::Jump {
                    depth: u32_from_usize(index + 1),
                    target: *break_label,
                }),
                _ => None,
            })
            .expect("the parser found what this leaves");
        self.emit_exit(exit, Operand::invalid());
    }

    /// `continue`, with the loop that `label` names if it has one.
    pub(super) fn continue_statement(&mut self, label: Option<Name>) {
        let controls = &self.context.stream.controls;
        let first = label.map_or(0, |label| {
            controls
                .iter()
                .rposition(|control| matches!(control, Control::Labeled { name, .. } if *name == label))
                .expect("the parser found the loop")
        });
        let loop_of = |(index, control): (usize, &Control)| match control {
            Control::Loop { continue_label, .. } => Some(Exit::Jump {
                depth: u32_from_usize(index + 1),
                target: *continue_label,
            }),
            _ => None,
        };
        let exit = if label.is_some() {
            controls.iter().enumerate().skip(first).find_map(loop_of)
        } else {
            controls.iter().enumerate().rev().find_map(loop_of)
        };
        self.emit_exit(exit.expect("the parser found the loop"), Operand::invalid());
    }

    pub(super) fn return_statement(&mut self, has_value: bool) {
        let value = if has_value {
            if self.is_in_async_generator_function() {
                self.await_value();
            }
            let (value, _) = self.pop_operand();
            value
        } else {
            self.context.constants.add_undefined()
        };
        self.emit_exit(Exit::Return, value);
        self.end_of_statement();
    }

    pub(super) fn throw_statement(&mut self) {
        let (src, _) = self.pop_operand();
        self.end_of_statement();
        self.put(op::Throw { src });
    }

    /// A block starts at `position`.
    pub(super) fn block_begin(&mut self, position: u32) {
        let environment = self.begin_scope(position, false);
        self.context.stream.controls.push(Control::Block { environment });
    }

    pub(super) fn block_end(&mut self) {
        self.end_disposal();
        let Some(Control::Block { environment }) = self.context.stream.controls.pop() else {
            unreachable!("a block is what is going on");
        };
        self.end_scope(environment);
    }

    /// Whether the value on top of the stack is something to write to.
    pub(super) fn is_top_value_target(&self) -> bool {
        self.context.stream.values.last().is_some_and(Val::is_target)
    }

    /// `target = value`, for the two on top of the stack, with what the names mean known to the core.
    pub(super) fn assign_to_binding(&mut self) {
        let values = &self.context.stream.values;
        let target = values[values.len() - 2];
        let ident = (target.kind == ValKind::LocalTarget).then(|| self.identifier_of(Name(target.name)));
        self.assign(ident);
    }

    #[inline(always)]
    pub(super) fn is_unused_value_row(&self, row: usize) -> bool {
        self.context.stream.unused_value_row == u32_from_usize(row)
    }

    pub(super) fn set_unused_value_row(&mut self, row: u32) {
        self.context.stream.unused_value_row = row;
    }

    #[inline(always)]
    pub(super) fn top_value_kind(&self) -> ValKind {
        self.context.stream.values.last().expect("something was evaluated").kind
    }

    pub(super) fn drop_value(&mut self) {
        self.context.stream.values.pop();
    }

    /// Whether what the value on top of the stack gets assigned to is nothing that can be written.
    pub(super) fn is_invalid_target_below_top(&self) -> bool {
        let values = &self.context.stream.values;
        values.len() >= 2 && values[values.len() - 2].kind == ValKind::InvalidTarget
    }

    /// The target of a declaration, which nothing can get in the way of.
    pub(super) fn target_of_declaration(&mut self, ident: Identifier) {
        // NB: The object of a `with` can have a property of the name of a `var`, which is what gets written then.
        if self.context.scope.is_in_with() && ident.declaration_kind == Some(DeclarationKind::Var) {
            self.target_of_identifier(Identifier {
                is_global: false,
                ..ident
            });
            return;
        }
        // NB: What a `var` of a program is, is as open as for any other reference.
        if self.context.scope.is_program() && ident.declaration_kind == Some(DeclarationKind::Var) && !ident.is_local()
        {
            self.target_of_identifier(ident);
            return;
        }
        let first_temporary = self.next_temporary();
        let kind = if ident.is_local() {
            ValKind::LocalTarget
        } else {
            ValKind::NameTarget
        };
        let local = if ident.is_local() {
            self.local_operand(&ident)
        } else {
            Operand::invalid()
        };
        self.context.stream.values.push(Val {
            kind,
            first_temporary,
            a: local,
            b: Operand::invalid(),
            name: ident.name.0,
            cache: u32::MAX,
            is_global: ident.is_global,
            is_const: false,
            is_var: false,
        });
    }

    /// Ends the code, so that nothing comes after this but assembling it.
    pub(super) fn seal_stream(&mut self) {
        if !self.is_stream_dead() {
            let value = match self.context.stream.completion {
                Some(completion) => completion,
                None => self.context.constants.add_undefined(),
            };
            self.current_source_offset = SourceOffset::NONE;
            if self.is_in_generator_or_async_function() {
                self.put(op::Yield {
                    continuation_label: None,
                    value,
                });
            } else {
                self.put(op::End { value });
            }
        }
        self.insert_body_head();
    }

    /// Assembles what was emitted into a new block, which has `other_tables_size` bytes left: the operands that come
    /// after the registers get their place in the frame, and labels their offsets.
    pub(super) fn finish_stream(&mut self, other_tables_size: usize) -> (AssembledBytecode, SliceBlockBuilder) {
        let frame = FrameLayout {
            register_count: self.context.stream.temporary_count.max(Register::RESERVED_COUNT),
            local_count: self.context.scope.local_count(),
            constant_count: u32_from_usize(self.context.constants.values.len()),
        };
        let stream = &mut self.context.stream;
        let head = &stream.head;
        let segment = &mut stream.current;
        let head_size = u32_from_usize(head.bytes.len());
        let mut exception_handlers = Vec::new();
        for (index, &(start, handler)) in stream.handler_changes.iter().enumerate() {
            let end = stream
                .handler_changes
                .get(index + 1)
                .map_or(u32_from_usize(segment.bytes.len()), |change| change.0);
            if let Some(handler) = handler
                && start != end
            {
                exception_handlers.push(ExceptionHandler {
                    start_offset: start + head_size,
                    end_offset: end + head_size,
                    handler_offset: stream.labels[handler.label.0 as usize] + head_size,
                    catches_exception: handler.catches,
                });
            }
        }
        let source_entry_count = head.source_entries.len() + segment.source_entries.len();
        // SAFETY: The slices and the block end up in an ExecutableData, which drops the slices first.
        let mut block = unsafe {
            SliceBlock::builder(
                SliceBlock::room_for::<u8>(head.bytes.len() + segment.bytes.len())
                    + SliceBlock::room_for::<SourceOffsetEntry>(source_entry_count)
                    + SliceBlock::room_for::<ExceptionHandler>(exception_handlers.len())
                    + other_tables_size,
            )
        };
        let mut bytecode = block.slice_from_parts(&head.bytes, &segment.bytes);
        let head_positions = head.operand_positions.iter().map(|position| position.offset);
        let positions = segment
            .operand_positions
            .iter()
            .map(|position| position.offset + head_size);
        for offset in head_positions.chain(positions) {
            let at = offset as usize;
            let bytes = &mut bytecode[at..at + 4];
            let raw = u32::from_ne_bytes((&*bytes).try_into().expect("an operand is four bytes"));
            let mut operand = Operand::from_raw(raw);
            operand.offset_index_by(frame.start_of(operand.operand_type()));
            bytes.copy_from_slice(&operand.raw().to_ne_bytes());
        }
        for position in &segment.label_positions {
            let at = (position.offset + head_size) as usize;
            let bytes = &mut bytecode[at..at + 4];
            let label = u32::from_ne_bytes((&*bytes).try_into().expect("a label is four bytes"));
            bytes.copy_from_slice(&(stream.labels[label as usize] + head_size).to_ne_bytes());
        }
        let head_entries = head.source_entries.iter().copied();
        let entries = segment
            .source_entries
            .iter()
            .map(|&(bytecode_offset, source_offset)| (bytecode_offset + head_size, source_offset));
        let source_map = block.slice_of_length(
            source_entry_count,
            head_entries
                .chain(entries)
                .map(|(bytecode_offset, source_offset)| SourceOffsetEntry {
                    bytecode_offset,
                    source_offset,
                }),
        );
        let assembled = AssembledBytecode {
            bytecode,
            source_map,
            exception_handlers: block.slice(exception_handlers.into_iter()),
            number_of_registers: frame.register_count,
            number_of_arguments: stream.number_of_arguments,
        };
        (assembled, block)
    }
}
