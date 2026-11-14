/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Inlining calls whose call feedback saw a single callee, and constructs
//! of a single base constructor.
//!
//! An inlined callee's frame is virtual: its slots are SSA values in its own
//! abstract frame, and its blocks are built into the caller's graph by a
//! nested walk over its bytecode. The frame becomes real whenever the runtime
//! can observe it. Exits describe it in their frame state chain and the
//! runtime materializes it. Slow paths and calls that are not inlined take
//! their operands as values and run in the frames of the inlined calls,
//! which the runtime publishes from their frame state (see
//! `SiteKind::Publish`) and compiled code pops again; calls that can, run
//! without them (see `Op::CallDirect`). The handlers of an inlined callee
//! are not built: an exception one of them catches leaves compiled code,
//! and the interpreter runs the handler in the callee's materialized frame.
//!
//! Returns jump to a continuation block in the caller, where the call's
//! result is the phi of the returned values.
//!
//! An inlined construct allocates `this` as an `AllocateObject` of the shape
//! the constructor's construct gives it, after checking that the
//! constructor's "prototype" still holds the prototype it had, and its
//! result is `this` (its returns return nothing else). Materialized frames
//! of inlined constructs are construct frames, which return `this`.

use super::AbstractFrame;
use super::Flow;
use super::Function;
use super::GraphBuilder;
use super::SlotState;
use super::arguments;
use crate::CompileFailure;
use crate::bitset::BitSet;
use crate::builder::Handling;
use crate::builder::handling;
use crate::bytecode::Instruction;
use crate::bytecode::OpCode;
use crate::bytecode::Operand;
use crate::bytecode::SlotKind;
use crate::bytecode::THIS_VALUE_REGISTER;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::code::ResumeMode;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::CALLEE_SLOT;
use crate::ir::FrameState;
use crate::ir::FrameStateId;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::ir::value;
use crate::snapshot::CallFeedbackSnapshot;
use crate::snapshot::CellId;
use crate::snapshot::ConstructTarget;
use crate::snapshot::Forwarding;
use crate::snapshot::Intrinsic;
use crate::snapshot::SuperConstructTarget;
use crate::snapshot::call_feedback_flags;

/// How an inlined callee was called.
#[derive(Debug, Clone)]
pub(super) struct InlineCall {
    /// The caller's frame state at the call: resuming after it, with the
    /// result in `dst`. Every frame state inside the callee chains to it.
    pub caller_frame_state: FrameStateId,
    /// The caller's abstract frame at the call.
    pub caller_frame: AbstractFrame,
    /// For a call of a closure of the callee's function: the closure, whose
    /// environments the callee's frame has, and which its materialized
    /// frames run.
    pub closure: Option<NodeId>,
    /// For an inlined builtin that calls its first argument back: what the
    /// call site's feedback saw that argument be.
    pub callback: Option<CallbackTarget>,
    /// The callee's `this`, or the empty value if it never reads it.
    pub this: NodeId,
    pub passed_argument_count: u32,
    /// Whether this call must preserve arguments for legacy `arguments`.
    pub has_legacy_arguments: bool,
    /// For callees with legacy `arguments`: the values the call passes beyond
    /// the arguments the callee's bytecode reads (see
    /// `legacy_argument_values()`).
    pub untracked_arguments: Vec<NodeId>,
    /// Where returns continue, once the first one is built.
    pub continuation: Option<BlockId>,
    /// The values returned, one per predecessor of the continuation, with
    /// what was known at each return.
    pub returns: Vec<NodeId>,
    /// The shapes every return knew objects have (see
    /// `AbstractFrame::fresh_shapes`), once the first return is built.
    pub return_fresh_shapes: Option<Vec<(NodeId, CellId)>>,
    /// Interpreter stack bytes of this callee's frame and those of the
    /// inlined calls it is in.
    pub frame_bytes: u64,
    /// Whether the call is a construct, whose result is `this`.
    pub construct: bool,
    /// The new target of a construct.
    pub new_target: Option<CellId>,
    /// For a construct of a derived constructor: what it does (see
    /// `ConstructTarget`). Its `this` starts empty, and super() binds it.
    pub derived_construct: Option<ConstructTarget>,
}

/// The callbacks a call site passed to the builtin it called, which it calls
/// back: the function at index `index` of the snapshot, the first callback,
/// or with `closures`, any closure of it.
#[derive(Debug, Clone, Copy)]
pub(super) struct CallbackTarget {
    index: u32,
    closures: bool,
}

/// How many functions a call whose callee is a phi of function constants may
/// switch on (see `GraphBuilder::try_inline_callee_cases()`).
const MAX_CALLEE_CASES: usize = 4;

/// Whether the callee can be built as an inlined call: its frame must not be
/// needed in memory outside generic nodes, its environments must stay those
/// of its function object, it must be fully compilable, and its exception
/// handlers must never have caught anything.
fn can_inline(callee: &Function<'_>, virtual_arguments: bool, derived_construct: bool) -> bool {
    if !callee.written_constants.is_empty() {
        return false;
    }
    callee.instructions.iter().all(|instruction| {
        let opcode = instruction.instruction.opcode();
        // NB: An arguments object the callee never creates reads no frame (see `arguments`).
        if virtual_arguments && arguments::can_create_virtual_arguments(callee, &instruction.instruction) {
            return true;
        }
        // NB: Handlers of inlined callees are never built. An exception they catch leaves compiled code, and the
        //     interpreter runs the handler in the callee's materialized frame (see `translate::leave()` in the
        //     runtime). A handler that caught before would make the code leave again and again.
        if opcode == OpCode::Catch {
            return !super::has_run(callee.executable, instruction.pc);
        }
        // NB: An inlined callee reads what its call passed for constant indices (see `try_build_argument()`).
        if opcode == OpCode::GetArgument {
            return virtual_arguments && arguments::has_constant_index(callee, &instruction.instruction);
        }
        let reads_frame_fields = matches!(
            opcode,
            OpCode::GetLexicalEnvironment
                | OpCode::SetLexicalEnvironment
                | OpCode::CreateLexicalEnvironment
                | OpCode::CreateVariableEnvironment
                | OpCode::EnterObjectEnvironment
                | OpCode::LeavePrivateEnvironment
                | OpCode::CreateArguments
                | OpCode::CreateRestParams
                | OpCode::CallDirectEval
                | OpCode::CallDirectEvalWithArgumentArray
                | OpCode::Debugger
                // NB: These read the function, the this value or the new target of the running frame, which is the
                //     caller's while an inlined callee runs (see `function_with_frame_as_this_environment()` in the
                //     runtime).
                | OpCode::SuperCallWithArgumentArray
        ) || (!derived_construct
            && matches!(
                opcode,
                OpCode::GetSuperConstructor
                    | OpCode::SuperCall
                    | OpCode::SuperCallWithForwardedArguments
                    | OpCode::BindThisFromSuperCall
            ));
        // NB: The new target of a callee with its own new.target is known (see `try_build_new_target()`).
        let unknown_new_target = opcode == OpCode::GetNewTarget
            && !callee
                .executable
                .function
                .is_some_and(|function| function.has_own_new_target);
        // NB: The base of `super` property access is built from the callee's home object (see
        //     `try_build_resolve_super_base()`).
        let unknown_super_base = opcode == OpCode::ResolveSuperBase
            && callee
                .executable
                .function
                .is_none_or(|function| function.super_base.is_none());
        !reads_frame_fields
            && !unknown_super_base
            && !unknown_new_target
            && !matches!(handling(opcode), Handling::Unsupported(_))
    })
}

/// Whether every return of `callee` returns a constant that is no object, or
/// `this`: then an inlined construct of it always results in `this`.
fn returns_no_object(callee: &Function<'_>) -> bool {
    callee
        .instructions
        .iter()
        .all(|instruction| match instruction.instruction {
            Instruction::Return { value } => {
                value.raw() == THIS_VALUE_REGISTER
                    || match callee.layout.slot_kind(value) {
                        SlotKind::Constant(index) => callee
                            .executable
                            .constants
                            .get(index as usize)
                            .is_some_and(|constant| value::tag(*constant) != value::OBJECT_TAG),
                        _ => false,
                    }
            }
            _ => true,
        })
}

/// Whether every path through the derived constructor `callee` calls
/// super() exactly once before anything else reads `this` or the frame
/// returns, and every return returns undefined (or, right after super(),
/// what super() returned, like default derived constructors): then the
/// super() of an inlined construct of it (see
/// `GraphBuilder::try_inline_super_call()`) always binds `this` while it is
/// empty, and the result of the construct is always `this`. Its frames
/// materialized before super() run it in the interpreter, and those
/// materialized after super() return their this value (see
/// `ExecutionContext::caller_is_construct` in the runtime).
///
/// A constructor with exception handlers never qualifies: a handler that
/// catches what super() or its arguments throw runs in the materialized
/// frame and can return while `this` is still empty, and nothing checks the
/// value such a frame returns.
fn calls_super_once_before_returning(callee: &Function<'_>) -> bool {
    if callee
        .instructions
        .iter()
        .any(|instruction| instruction.instruction.opcode() == OpCode::Catch)
    {
        return false;
    }
    let mut jump_targets = Vec::new();
    for instruction in callee.instructions {
        instruction
            .instruction
            .for_each_jump_target(|target| jump_targets.push(target.0));
    }
    let mut super_call_result = None;
    let mut bound_this_at = None;
    let mut previous_opcode = None;
    for instruction in callee.instructions {
        let opcode = instruction.instruction.opcode();
        // NB: Then nothing changed what super() returned.
        let follows_bind_this =
            previous_opcode == Some(OpCode::BindThisFromSuperCall) && !jump_targets.contains(&instruction.pc);
        previous_opcode = Some(opcode);
        let called_super = super_call_result.is_some();
        if bound_this_at.is_none() {
            // NB: The instructions up to the BindThisFromSuperCall run once, one after the other.
            match instruction.instruction {
                Instruction::SuperCall { dst, .. } | Instruction::SuperCallWithForwardedArguments { dst, .. }
                    if !called_super =>
                {
                    super_call_result = Some(dst);
                    continue;
                }
                _ => {}
            }
            match instruction.instruction {
                Instruction::BindThisFromSuperCall { result } if Some(result) == super_call_result => {
                    bound_this_at = Some(instruction.pc);
                    continue;
                }
                _ => {}
            }
            match opcode {
                OpCode::GetSuperConstructor => {}
                OpCode::SuperCall
                | OpCode::SuperCallWithForwardedArguments
                | OpCode::BindThisFromSuperCall
                | OpCode::ResolveThisBinding
                | OpCode::CreateArguments => return false,
                _ if instruction.instruction.is_terminator() => return false,
                _ => {}
            }
            continue;
        }
        // NB: What super() returned is `this`.
        let returns_undefined_or_this = |value: Operand| match callee.layout.slot_kind(value) {
            SlotKind::Constant(index) => callee.executable.constants.get(index as usize) == Some(&value::UNDEFINED),
            _ => follows_bind_this && Some(value) == super_call_result,
        };
        match instruction.instruction {
            Instruction::Return { value } | Instruction::End { value } if !returns_undefined_or_this(value) => {
                return false;
            }
            _ => {}
        }
        if matches!(
            opcode,
            OpCode::GetSuperConstructor
                | OpCode::SuperCall
                | OpCode::SuperCallWithForwardedArguments
                | OpCode::BindThisFromSuperCall
                | OpCode::CreateArguments
        ) {
            return false;
        }
    }
    bound_this_at.is_some_and(|bound_this_at| jump_targets.iter().all(|target| *target > bound_this_at))
}

/// Where the `this` value or an argument of an inlined call comes from.
#[derive(Debug, Clone, Copy)]
pub(super) enum InlineValue {
    Operand(Operand),
    Constant(u64),
    /// The running frame's argument at this index, whose arguments object
    /// the code never created.
    FrameArgument(u32),
    /// A value the code has, like an argument of an inlined callee.
    Node(NodeId),
}

/// What an inlined call does (see `GraphBuilder::inlinable_callee()`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum InlinedCallKind {
    Call,
    /// A construct of a base constructor.
    Construct,
    /// A construct of a derived constructor, whose super() is inlined too.
    DerivedConstruct,
}

/// A callee that `GraphBuilder::inline_call()` may inline (see
/// `GraphBuilder::inlinable_callee()`).
struct InlinableCallee<'a> {
    executable: &'a crate::snapshot::ExecutableSnapshot,
    function: Function<'a>,
    has_legacy_arguments: bool,
}

/// A call to inline.
struct InlinedCallSite {
    /// The snapshot index of the callee's executable.
    index: u32,
    /// The operand holding the function value checked to be the callee, or
    /// `None` if the caller checked the callee itself...
    callee: Option<Operand>,
    /// ...if it is not the inlined function itself (a bound function).
    expected_callee: Option<CellId>,
    this: InlineValue,
    arguments: Vec<InlineValue>,
    passed_argument_count: u32,
    /// Whether the callee gets other arguments than the `Call` instruction's.
    forwarded: bool,
    dst: Operand,
    /// For a `CallConstruct`: what its construct does.
    construct: Option<ConstructTarget>,
    /// The new target of a construct, if it is not the callee: the derived
    /// constructor whose super() constructs with the callee.
    new_target: Option<CellId>,
    /// The closure called, if the callee is a closure of the inlined
    /// executable's function rather than that function itself...
    closure: Option<NodeId>,
    /// ...and whether to check that it is one.
    check_closure: bool,
    /// For a builtin: the callbacks the call site passed it.
    callback: Option<CallbackTarget>,
}

impl InlinedCallSite {
    /// A call of snapshot executable `index` through
    /// `Function.prototype.call` with `arguments`, whose first is `this`.
    fn through_call(index: u32, arguments: &[Operand], argument_count: u32, dst: Operand) -> Self {
        let this = arguments
            .first()
            .map_or(InlineValue::Constant(value::UNDEFINED), |this| {
                InlineValue::Operand(*this)
            });
        Self::forwarded(
            index,
            this,
            arguments.iter().skip(1).copied().map(InlineValue::Operand).collect(),
            argument_count.saturating_sub(1),
            dst,
        )
    }

    /// A call of snapshot executable `index` that gets other arguments than
    /// the `Call` instruction's.
    fn forwarded(index: u32, this: InlineValue, arguments: Vec<InlineValue>, count: u32, dst: Operand) -> Self {
        Self {
            forwarded: true,
            ..Self::new(index, this, arguments, count, dst)
        }
    }

    /// A call of snapshot executable `index` with `this` and `arguments`,
    /// into `dst`, which checks no callee and forwards nothing.
    fn new(
        index: u32,
        this: InlineValue,
        arguments: Vec<InlineValue>,
        passed_argument_count: u32,
        dst: Operand,
    ) -> Self {
        Self {
            index,
            callee: None,
            expected_callee: None,
            this,
            arguments,
            passed_argument_count,
            forwarded: false,
            dst,
            construct: None,
            new_target: None,
            closure: None,
            check_closure: false,
            callback: None,
        }
    }
}

impl<'a> GraphBuilder<'a> {
    pub(super) fn boxed_object(&self, cell: CellId) -> u64 {
        (u64::from(value::OBJECT_TAG) << 48) | (cell.0 & self.runtime.heap_region_offset_mask)
    }

    /// A frame state of the function being walked at its current
    /// instruction, resuming with `mode`, for the slots in `live`: every one
    /// is listed, with its value (`frame_state_values()`) or as held by
    /// frame memory. The destination of a `ResumeAfter` has the value it had
    /// before the instruction, which is what the frame needs if the
    /// instruction did not write it (see `ir::FrameState`).
    pub(super) fn new_frame_state(&self, live: &BitSet, mode: ResumeMode) -> FrameState {
        let values = self.frame_state_values(live);
        let in_frame = self.frame_state_in_frame(live);
        FrameState {
            executable: self.function.index,
            pc: self.function.pc,
            mode,
            values,
            in_frame,
            parent: self.parent_frame_state(),
            passed_argument_count: None,
            legacy_arguments: self.legacy_argument_slots(),
        }
    }

    /// The frame state after the instruction being built, which wrote
    /// `dst` (`Operand::INVALID` if nothing), for the slots live after it.
    pub(super) fn resume_after_frame_state(&mut self, dst: u32) -> FrameStateId {
        let live = self.function.liveness.live_out(self.function.instruction_index);
        let frame_state = self.new_frame_state(&live, ResumeMode::ResumeAfter { dst });
        self.graph.add_frame_state(frame_state)
    }

    /// Like `resume_after_frame_state()`, for an instruction that writes the slots `outputs` to the frame where it
    /// does not continue: the frame state lists them as held by the frame. Frames of inlined calls have no slots in
    /// sync to say so.
    pub(super) fn resume_after_frame_state_with_outputs_in_frame(
        &mut self,
        dst: u32,
        outputs: &[usize],
    ) -> FrameStateId {
        let live = self.function.liveness.live_out(self.function.instruction_index);
        let mut frame_state = self.new_frame_state(&live, ResumeMode::ResumeAfter { dst });
        for slot in outputs.iter().copied().filter(|slot| live.contains(*slot)) {
            let operand = self.function.layout.operand_for_tracked_index(slot).raw();
            frame_state.values.retain(|(listed, _)| *listed != operand);
            // NB: Exits never clear the reserved registers, which frames keep.
            if slot >= crate::bytecode::RESERVED_REGISTER_COUNT as usize
                && let Err(position) = frame_state.in_frame.binary_search(&operand)
            {
                frame_state.in_frame.insert(position, operand);
            }
        }
        self.graph.add_frame_state(frame_state)
    }

    /// The slots in `live` whose value frame memory holds, which a frame
    /// state of the function being walked lists without a value: the
    /// compiled function's slots that are in sync. Reserved registers are
    /// not listed.
    fn frame_state_in_frame(&self, live: &BitSet) -> Vec<u32> {
        if self.function.is_virtual() {
            return Vec::new();
        }
        // NB: Exits never clear the reserved registers, which frames keep.
        live.iter()
            .filter(|slot| {
                *slot >= crate::bytecode::RESERVED_REGISTER_COUNT as usize && self.frame.slots[*slot].in_sync
            })
            .map(|slot| self.function.layout.operand_for_tracked_index(slot).raw())
            .collect()
    }

    /// The values a frame state of the function being walked lists for the
    /// slots in `live`: those frame memory does not hold, or every one for
    /// virtual frames, but the exception register. Inlined callees that are
    /// not strict list all their passed arguments too, since their legacy
    /// `arguments` shows them in the frames exits and slow paths materialize.
    fn frame_state_values(&self, live: &BitSet) -> Vec<(u32, NodeId)> {
        let is_virtual = self.function.is_virtual();
        // NB: Only the runtime writes the exception register (materialized frames start with it empty): when a
        //     handler of the frame catches an exception, before a leave writes the rest of the frame, which must keep
        //     it. Compiled code only ever holds what frame memory holds.
        let exception_register = self
            .function
            .layout
            .tracked_index(Operand::from_raw(crate::bytecode::EXCEPTION_REGISTER));
        let mut values = live
            .iter()
            .filter(|slot| Some(*slot) != exception_register)
            .filter_map(|slot| {
                let SlotState { node, in_sync } = self.frame.slots[slot];
                (is_virtual || !in_sync).then(|| (self.function.layout.operand_for_tracked_index(slot).raw(), node))
            })
            .collect::<Vec<_>>();
        for (operand, node) in self.legacy_argument_values() {
            if !values.iter().any(|(slot, _)| *slot == operand) {
                values.push((operand, node));
            }
        }
        // NB: Exits materialize frames of inlined closures with the closure
        //     that was called.
        if let Some(closure) = self.function.inline.as_ref().and_then(|call| call.closure) {
            values.push((CALLEE_SLOT, closure));
        }
        values
    }

    /// For an inlined callee with legacy `arguments`: the values of
    /// all its passed arguments, which its legacy `arguments` shows. Those
    /// its bytecode reads are its frame's, the others the values the call
    /// passed.
    fn legacy_argument_values(&self) -> Vec<(u32, NodeId)> {
        let function = &self.function;
        let Some(call) = function.inline.as_ref().filter(|_| function.has_legacy_arguments()) else {
            return Vec::new();
        };
        let layout = function.layout;
        let mut values = Vec::new();
        for argument in 0..layout.number_of_arguments {
            let slot = (layout.registers_and_locals_count + argument) as usize;
            values.push((layout.arguments_base() + argument, self.frame.slots[slot].node));
        }
        for (argument, node) in (layout.number_of_arguments..).zip(&call.untracked_arguments) {
            values.push((layout.arguments_base() + argument, *node));
        }
        values
    }

    /// Ends the instruction at `instruction_index` just built (with the
    /// instructions it built as well), whose nodes are those from
    /// `first_node` on, with an `AssumeLegacyArgumentsUnread` if code that
    /// some of its nodes run may read the legacy `arguments` of the frames of
    /// the inlined calls it is in. Its frame state resumes at the next
    /// instruction.
    pub(super) fn guard_legacy_arguments(&mut self, instruction_index: usize, first_node: usize) {
        let Some(parent) = self.parent_frame_state() else {
            return;
        };
        let has_legacy_arguments = self.function.has_legacy_arguments()
            || self
                .graph
                .frame_state_chain(parent)
                .iter()
                .any(|id| !self.graph.frame_state(*id).legacy_arguments.is_empty());
        if !has_legacy_arguments {
            return;
        }
        let function_index = self.function.index;
        let graph = &self.graph;
        let nodes = (first_node..graph.nodes.len())
            .map(|index| NodeId(index as u32))
            .filter(|node| {
                let data = graph.node(*node);
                // NB: Nodes of the calls the instruction inlined run in frames
                //     of their own, after which those calls guard themselves.
                data.op.properties().runs_code
                    && data.frame_state.is_some_and(|frame_state| {
                        let innermost = graph.frame_state(frame_state);
                        innermost.executable == function_index && innermost.parent == Some(parent)
                    })
            })
            .collect::<Vec<_>>();
        if nodes.is_empty() {
            return;
        }
        let last = instruction_index + self.function.skipped_instructions;
        let frame_state = FrameState {
            pc: self.function.instructions[last].next_pc(),
            ..self.new_frame_state(&self.function.liveness.live_out(last), ResumeMode::ResumeAt)
        };
        let frame_state = self.graph.add_frame_state(frame_state);
        let guard = self.emit(Op::AssumeLegacyArgumentsUnread, Vec::new(), None);
        self.graph.nodes[guard.index()].frame_state = Some(frame_state);
        self.graph.legacy_arguments_guards.push((guard, nodes));
    }

    fn legacy_argument_slots(&self) -> core::ops::Range<u32> {
        let function = &self.function;
        let Some(call) = function.inline.as_ref().filter(|_| function.has_legacy_arguments()) else {
            return 0..0;
        };
        let layout = function.layout;
        let count = layout.number_of_arguments + call.untracked_arguments.len() as u32;
        layout.arguments_base()..layout.arguments_base() + count
    }

    /// The parent of frame states of the function being walked.
    pub(super) fn parent_frame_state(&self) -> Option<FrameStateId> {
        self.function.inline.as_ref().map(|call| call.caller_frame_state)
    }

    /// Makes compiled code check at entry that the interpreter stack has room
    /// for a frame of `bytes` that a call pushes on top of the frames of the
    /// inlined calls it is in, so the call need not check.
    pub(super) fn reserve_call_frame(&mut self, bytes: u64) {
        let frame_bytes = self.function.inline.as_ref().map_or(0, |call| call.frame_bytes) + bytes;
        self.graph.materialized_frame_bytes = self.graph.materialized_frame_bytes.max(frame_bytes);
    }

    /// Whether a call can take the values of its operands `inputs` as inputs
    /// at all: not if one is the arguments object the code never created,
    /// which only exists once read from the frame.
    pub(super) fn call_operands_are_values(&self, inputs: &[Operand]) -> bool {
        !inputs.iter().any(|operand| self.holds_virtual_arguments(*operand))
    }

    /// Whether a direct or native call takes its operands `inputs` as the
    /// inputs of its node in registers.
    fn call_takes_inputs(&self, inputs: &[Operand]) -> bool {
        !self.function.is_virtual()
            && inputs.len() <= crate::codegen::DIRECT_CALL_MAX_INPUTS
            && !inputs.iter().any(|operand| self.holds_virtual_arguments(*operand))
    }

    /// Builds a `Call` whose feedback saw a single callee that JIT code can
    /// call directly as a `CallDirect` node (or a `CallNative` node for
    /// native functions), or returns `None` if it did not.
    pub(super) fn try_direct_call(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            call_feedback,
            argument_count,
            callee,
            this_value,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        let Some(feedback) = self.call_feedback(*call_feedback) else {
            return Ok(None);
        };
        let Handling::Generic(info) = handling(OpCode::Call) else {
            unreachable!("calls are generic instructions");
        };
        let slots_offset = u64::from(self.runtime.offsets.execution_context_slots);
        if let Some(native) = feedback.native_call.filter(|native| native.entry.is_some()) {
            // NB: Native calls take the callee, the `this` value and the
            //     arguments as inputs like direct calls do.
            let mut inputs = vec![*callee, *this_value];
            inputs.extend(arguments.iter().copied());
            // NB: Calls without the frames of the inlined calls they are in check their callee (see
            //     `check_call_without_frames_target()`), and are generic where those checks exited before.
            let without_frames = self.inlined_frame_bytes_of_call(&inputs);
            if without_frames.is_some() && !self.may_speculate(ExitKind::BadCallTarget) {
                return Ok(None);
            }
            self.reserve_call_frame(slots_offset + 8 * u64::from(*argument_count));
            self.embed(native.function);
            self.embed(native.realm);
            if let Some(inlined_frame_bytes) = without_frames {
                self.check_callee(*callee, native.function)?;
                let op = Op::CallNative {
                    executable: self.function.index,
                    pc: self.function.pc,
                    inlined_frame_bytes,
                    stores_operands: false,
                };
                return self
                    .build_call_without_inlined_frames(instruction, op, &inputs)
                    .map(Some);
            }
            let stores_operands = !self.call_takes_inputs(&inputs);
            if stores_operands && !self.call_operands_are_values(&inputs) {
                inputs.clear();
            }
            let op = Op::CallNative {
                executable: self.function.index,
                pc: self.function.pc,
                inlined_frame_bytes: 0,
                stores_operands: stores_operands && !inputs.is_empty(),
            };
            return self
                .build_generic_node_with_inputs(instruction, info, op, &inputs, None)
                .map(Some);
        }
        if let Some(function) = feedback.function_prototype_call_target() {
            // NB: The generic call compares its callee with it.
            self.embed(function);
            return Ok(None);
        }
        let Some(target) = feedback.direct_call else {
            return self.try_direct_call_of_constant(instruction);
        };
        self.build_direct_call(instruction, target, None)
    }

    /// Builds a `Call` of a function the code holds as a constant, like a
    /// method of a prototype, as a `CallDirect` node, or returns `None` if
    /// it cannot call the callee directly.
    fn try_direct_call_of_constant(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call { callee, .. } = instruction else {
            return Ok(None);
        };
        let Some(slot) = self.peek(*callee) else {
            return Ok(None);
        };
        match self.graph.constant_value(slot.node) {
            Some(bits) => self.try_direct_call_of_function(instruction, bits),
            None => Ok(None),
        }
    }

    /// Builds a `Call` whose callee is the function `bits` as a `CallDirect`
    /// node, or returns `None` if it cannot call the function directly.
    fn try_direct_call_of_function(
        &mut self,
        instruction: &Instruction,
        bits: u64,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some(target) = self.direct_call_functions.get(&bits).copied() else {
            return Ok(None);
        };
        let index = u32::try_from(self.graph.direct_call_targets.len()).expect("target count fits in u32");
        self.graph.direct_call_targets.push(target);
        self.build_direct_call(instruction, target, Some(index))
    }

    /// Builds a `Call` that calls `target` directly as a `CallDirect` node,
    /// whose `target` is `constant_target` (see `Op::CallDirect`).
    fn build_direct_call(
        &mut self,
        instruction: &Instruction,
        target: crate::snapshot::DirectCallTarget,
        constant_target: Option<u32>,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            argument_count,
            callee,
            this_value,
            arguments,
            ..
        } = instruction
        else {
            unreachable!("direct calls are of Call instructions");
        };
        let Handling::Generic(info) = handling(OpCode::Call) else {
            unreachable!("calls are generic instructions");
        };
        // NB: Direct calls take the callee, the `this` value if the callee
        //     reads it, and the arguments as inputs (see
        //     `codegen::DIRECT_CALL_MAX_INPUTS`), and store them straight
        //     into the callee's frame, except in inlined callees whose frames
        //     are written whole, and for the virtual arguments object.
        // NB: Calls without the frames of the inlined calls they are in
        //     always take `this`, for their slow paths.
        let mut inputs = vec![*callee, *this_value];
        inputs.extend(arguments.iter().copied());
        // NB: Calls without those frames check their callee, unless it is constant (see
        //     `check_call_without_frames_target()`), and are generic where those checks exited before.
        let without_frames = self.inlined_frame_bytes_of_call(&inputs);
        if without_frames.is_some() && constant_target.is_none() && !self.may_speculate(ExitKind::BadCallTarget) {
            return Ok(None);
        }
        let slots_offset = u64::from(self.runtime.offsets.execution_context_slots);
        let slot_count = u64::from(target.registers_and_locals_and_constants_count)
            + u64::from((*argument_count).max(target.function.formal_parameter_count));
        self.reserve_call_frame(slots_offset + 8 * slot_count);
        // NB: Calls of closures check the callee's shared data. The code
        //     must not keep the closure the site saw first alive, with all
        //     its environment holds.
        if target.closures {
            self.embed(target.function.shared_data);
        } else {
            self.embed(target.function.function);
        }
        self.embed(target.executable);
        self.embed(target.function.realm);
        if target.function.uses_this && !target.function.strict {
            self.embed(target.function.global_this);
        }
        if let Some(index) = target.environment {
            let Some(template) = self.function.executable.environment_templates.get(index as usize) else {
                return Ok(None);
            };
            for cell in template.cells.clone() {
                self.embed(cell);
            }
        }
        if let Some(inlined_frame_bytes) = without_frames {
            if constant_target.is_none() {
                self.check_call_without_frames_target(*callee, target)?;
            }
            let op = Op::CallDirect {
                executable: self.function.index,
                pc: self.function.pc,
                inlined_frame_bytes,
                stores_operands: false,
                target: constant_target,
            };
            return self
                .build_call_without_inlined_frames(instruction, op, &inputs)
                .map(Some);
        }
        let stores_operands = !self.call_takes_inputs(&inputs);
        if stores_operands && !self.call_operands_are_values(&inputs) {
            inputs.clear();
        }
        let op = Op::CallDirect {
            executable: self.function.index,
            pc: self.function.pc,
            inlined_frame_bytes: 0,
            stores_operands: stores_operands && !inputs.is_empty(),
            target: constant_target,
        };
        self.build_generic_node_with_inputs(instruction, info, op, &inputs, None)
            .map(Some)
    }

    /// Checks that the callee of a direct call of `target` that runs without
    /// the frames of the inlined calls it is in is the target (or a closure
    /// of its function, if the site called those), exiting otherwise.
    /// Where such checks exited before, the call is generic instead.
    ///
    /// OPTIMIZATION: Calling another callee there takes the frames of the
    ///               inlined calls created by the runtime first, every time,
    ///               which costs more than a generic call. Its exit lets
    ///               the next compilation make one.
    fn check_call_without_frames_target(
        &mut self,
        callee: Operand,
        target: crate::snapshot::DirectCallTarget,
    ) -> Result<(), CompileFailure> {
        if target.closures {
            let value = self.read(callee)?;
            self.emit_checked(
                Op::CheckClosure {
                    shared_data: target.function.shared_data,
                },
                vec![value],
                None,
            );
            return Ok(());
        }
        self.check_callee(callee, target.function.function)
    }

    /// For a direct or native call in an inlined callee that can run
    /// without the frames of the inlined calls it is in, with its operands
    /// `inputs` as inputs: the bytes of those frames, which the call leaves
    /// room for below its callee's frame (see `Op::CallDirect`).
    ///
    /// Stack walks that only look at the realms and scripts of frames do
    /// not see those frames, so they must all run in the compiled
    /// function's script (inlined callees run in its realm). The legacy
    /// `arguments` of those that have them come from their frame states, so
    /// their arguments must all be values.
    fn inlined_frame_bytes_of_call(&self, inputs: &[Operand]) -> Option<u32> {
        let call = self.function.inline.as_ref()?;
        if inputs.len() > crate::codegen::DIRECT_CALL_MAX_INPUTS
            || inputs.iter().any(|operand| self.holds_virtual_arguments(*operand))
        {
            return None;
        }
        let compiled_function_fields = self.callers.first()?.executable.function_fields?;
        let runs_like_compiled_function = |executable: &crate::snapshot::ExecutableSnapshot| {
            executable.builtin
                || executable
                    .function_fields
                    .is_some_and(|fields| fields.has_script_or_module_of(&compiled_function_fields))
        };
        let inlined = core::iter::once(&self.function).chain(self.callers.iter().skip(1));
        if !inlined
            .into_iter()
            .all(|function| runs_like_compiled_function(function.executable))
        {
            return None;
        }
        u32::try_from(call.frame_bytes).ok()
    }

    /// Builds a direct or native call in an inlined callee that runs without
    /// the frames of the inlined calls it is in (see `Op::CallDirect`), with
    /// the values of `inputs` as inputs. Its value is the call's result.
    fn build_call_without_inlined_frames(
        &mut self,
        instruction: &Instruction,
        op: Op,
        inputs: &[Operand],
    ) -> Result<Flow, CompileFailure> {
        let input_nodes = inputs
            .iter()
            .map(|operand| self.read(*operand))
            .collect::<Result<Vec<_>, _>>()?;
        let mut destination = None;
        instruction.for_each_operand(|operand, role| {
            if role.is_written() {
                destination.get_or_insert(operand);
            }
        });
        let destination = destination.expect("a call writes its destination");
        let node = self.emit(op, input_nodes, Some(Repr::Tagged));
        // NB: Resuming after the call, the frame's destination holds the
        //     result already.
        let frame_state = self.resume_after_frame_state(destination.raw());
        self.graph.nodes[node.index()].frame_state = Some(frame_state);
        self.write(destination, node)?;
        Ok(Flow::Continue)
    }

    /// Inlines a `Call`, if its callee is a function the code knows, or its feedback saw a single callee that qualifies,
    /// or a single function its callee forwarded every call to: through
    /// `Function.prototype.call` (`f.call(this_arg, ...)`), through
    /// `Function.prototype.apply` with the arguments object the code never
    /// created (`f.apply(this_arg, arguments)`), or as a bound function.
    /// Returns `None`, having built nothing that matters, if it does not.
    pub(super) fn try_inline_call(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            call_feedback,
            dst,
            callee,
            this_value,
            argument_count,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        if let Some(flow) = self.try_inline_closure_call(instruction)? {
            return Ok(Some(flow));
        }
        // NB: A callee the code knows exactly is what the call calls, whatever callees its feedback saw.
        if let Some(index) = self.constant_inlinable_function(*callee) {
            let callback = self
                .call_feedback(*call_feedback)
                .filter(|feedback| feedback.inline_executable == Some(index))
                .and_then(|feedback| forwarded_callback(&feedback));
            let call = InlinedCallSite {
                callback,
                ..InlinedCallSite::new(
                    index,
                    InlineValue::Operand(*this_value),
                    arguments.iter().copied().map(InlineValue::Operand).collect(),
                    *argument_count,
                    *dst,
                )
            };
            if let Some(flow) = self.inline_call(call)? {
                return Ok(Some(flow));
            }
        }
        // NB: The feedback of a builtin's calls comes from all its callers,
        //     whose callbacks it calls, so it says nothing about this one.
        //     Calls of its callback inline what its call site passed it.
        if self.function.executable.builtin {
            let callback = self.function.inline.as_ref().and_then(|call| call.callback);
            let callback_argument = Operand::from_raw(self.function.layout.arguments_base());
            let Some(callback) = callback.filter(|_| *callee == callback_argument) else {
                return Ok(None);
            };
            let closure = if callback.closures {
                Some(self.read(*callee)?)
            } else {
                None
            };
            let call = InlinedCallSite {
                callee: closure.is_none().then_some(*callee),
                closure,
                check_closure: closure.is_some(),
                ..InlinedCallSite::new(
                    callback.index,
                    InlineValue::Operand(*this_value),
                    arguments.iter().copied().map(InlineValue::Operand).collect(),
                    *argument_count,
                    *dst,
                )
            };
            return self.inline_call(call);
        }
        if let Some(flow) = self.try_inline_callee_cases(instruction)? {
            return Ok(Some(flow));
        }
        let Some(feedback) = self.call_feedback(*call_feedback) else {
            return Ok(None);
        };
        if let Some(index) = feedback.inline_executable {
            let callback = forwarded_callback(&feedback);
            let call = InlinedCallSite {
                callee: Some(*callee),
                callback,
                ..InlinedCallSite::new(
                    index,
                    InlineValue::Operand(*this_value),
                    arguments.iter().copied().map(InlineValue::Operand).collect(),
                    *argument_count,
                    *dst,
                )
            };
            return self.inline_call(call);
        }
        if feedback.forwarded.is_none()
            && let Some(flow) = self.try_inline_constant_callee(instruction)?
        {
            return Ok(Some(flow));
        }
        let Some(forwarded) = feedback.forwarded else {
            return Ok(None);
        };
        let Some(index) = forwarded.inline_executable else {
            return Ok(None);
        };
        let call = match forwarded.forwarding {
            // NB: Callbacks are inlined into the builtin they were passed to.
            Forwarding::Callback => return Ok(None),
            Forwarding::Call => {
                if self.constant_intrinsic(*callee) != Some(Intrinsic::FunctionPrototypeCall) {
                    return Ok(None);
                }
                InlinedCallSite {
                    callee: Some(*this_value),
                    ..InlinedCallSite::through_call(index, arguments, *argument_count, *dst)
                }
            }
            Forwarding::Apply => {
                let [this, forwarded_arguments] = arguments.as_slice() else {
                    return Ok(None);
                };
                if self.constant_intrinsic(*callee) != Some(Intrinsic::FunctionPrototypeApply)
                    || !self.holds_virtual_arguments(*forwarded_arguments)
                {
                    return Ok(None);
                }
                let (arguments, count) = match self.inlined_call_arguments() {
                    Some(arguments) => arguments,
                    None if self.may_speculate(ExitKind::UnexpectedValue) => (
                        (0..forwarded.argument_count).map(InlineValue::FrameArgument).collect(),
                        forwarded.argument_count,
                    ),
                    None => return Ok(None),
                };
                InlinedCallSite {
                    callee: Some(*this_value),
                    ..InlinedCallSite::forwarded(index, InlineValue::Operand(*this), arguments, count, *dst)
                }
            }
            Forwarding::Bound => {
                let Some(bound_function) = feedback.target else {
                    return Ok(None);
                };
                let start = forwarded.bound_arguments_start as usize;
                let Some(bound_arguments) = self
                    .function
                    .executable
                    .feedback
                    .bound_arguments
                    .get(start..start + forwarded.bound_argument_count as usize)
                else {
                    return Ok(None);
                };
                InlinedCallSite {
                    callee: Some(*callee),
                    expected_callee: Some(bound_function),
                    forwarded: true,
                    ..InlinedCallSite::new(
                        index,
                        InlineValue::Constant(forwarded.bound_this),
                        bound_arguments
                            .iter()
                            .copied()
                            .map(InlineValue::Constant)
                            .chain(arguments.iter().copied().map(InlineValue::Operand))
                            .collect(),
                        forwarded.bound_argument_count + argument_count,
                        *dst,
                    )
                }
            }
        };
        self.inline_call(call)
    }

    /// Inlines a `Call` of a function the code holds as a constant, which
    /// needs no feedback: called itself (`f(...)`), through
    /// `Function.prototype.call` (`f.call(this_arg, ...)`), or in an inlined
    /// callee, through `Function.prototype.apply` with its virtual arguments
    /// object (`f.apply(this_arg, arguments)`). Sites that call
    /// several functions, like a method of a constructor every class shares,
    /// call one of them where the code knows which. Returns `None`, having
    /// built nothing that matters, if it does not.
    fn try_inline_constant_callee(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            dst,
            callee,
            this_value,
            argument_count,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        if let Some(index) = self.constant_inlinable_function(*callee) {
            let call = InlinedCallSite::new(
                index,
                InlineValue::Operand(*this_value),
                arguments.iter().copied().map(InlineValue::Operand).collect(),
                *argument_count,
                *dst,
            );
            return self.inline_call(call);
        }
        let Some(index) = self.constant_inlinable_function(*this_value) else {
            return Ok(None);
        };
        // NB: An inlined callee forwards what its call passed it.
        if self.constant_intrinsic(*callee) == Some(Intrinsic::FunctionPrototypeApply)
            && let [this, forwarded_arguments] = arguments.as_slice()
            && self.holds_virtual_arguments(*forwarded_arguments)
            && let Some((arguments, count)) = self.inlined_call_arguments()
        {
            let call = InlinedCallSite::forwarded(index, InlineValue::Operand(*this), arguments, count, *dst);
            return self.inline_call(call);
        }
        if self.constant_intrinsic(*callee) != Some(Intrinsic::FunctionPrototypeCall) {
            return Ok(None);
        }
        self.inline_call(InlinedCallSite::through_call(index, arguments, *argument_count, *dst))
    }

    /// The functions the callee of a call may be, with the snapshot
    /// executables of those whose calls may be inlined, if the callee is a
    /// phi of a few function constants in the current block, like the
    /// methods a property lookup found for the shapes it switched on.
    fn callee_cases(&self, callee: Operand) -> Option<Vec<(u64, Option<u32>)>> {
        let node = self.peek(callee)?.node;
        // NB: Phis of loop headers get the inputs of their back edges later.
        let block = &self.graph.blocks[self.block.index()];
        if block.is_loop_header || !block.phis.contains(&node) {
            return None;
        }
        let mut cases: Vec<(u64, Option<u32>)> = Vec::new();
        for input in &self.graph.node(node).inputs {
            let bits = self.graph.constant_value(*input)?;
            if value::tag(bits) != value::OBJECT_TAG {
                return None;
            }
            if !cases.iter().any(|(case, _)| *case == bits) {
                cases.push((bits, self.inlinable_functions.get(&bits).copied()));
            }
        }
        if cases.len() < 2
            || cases.len() > MAX_CALLEE_CASES
            || cases
                .iter()
                .all(|(bits, index)| index.is_none() && !self.direct_call_functions.contains_key(bits))
        {
            return None;
        }
        Some(cases)
    }

    /// The edges into the current block that bring the callee of a call to
    /// each of `cases`, by case, if the block is a join of nothing but them:
    /// it has no body, its only phi is the callee's, and all its
    /// predecessors jump to it. Each case can then start on its edges, which
    /// keep what the code knew there, like the shape of the receiver the
    /// `GetById` that found the methods switched on.
    fn callee_case_edges(&self, callee_phi: NodeId, cases: &[(u64, Option<u32>)]) -> Option<Vec<Vec<BlockId>>> {
        let block = &self.graph.blocks[self.block.index()];
        if !block.body.is_empty() || block.phis != [callee_phi] {
            return None;
        }
        // NB: The phi reaches only the first case, so only the callee's slot,
        //     which each case gives its function, may hold it.
        let holders = self.frame.slots.iter().filter(|slot| slot.node == callee_phi).count();
        if holders != 1 {
            return None;
        }
        let jumps_here = |predecessor: &BlockId| matches!(self.graph.control(*predecessor).op, Op::Jump { target } if target == self.block);
        if !block.predecessors.iter().all(jumps_here) {
            return None;
        }
        let inputs = &self.graph.node(callee_phi).inputs;
        Some(
            cases
                .iter()
                .map(|(bits, _)| {
                    block
                        .predecessors
                        .iter()
                        .zip(inputs)
                        .filter(|(_, input)| self.graph.constant_value(**input) == Some(*bits))
                        .map(|(predecessor, _)| *predecessor)
                        .collect()
                })
                .collect(),
        )
    }

    /// Makes `edges`, edges into the current block `join` (see
    /// `callee_case_edges()`), lead to a block of their own, and returns it:
    /// for the first case, `join` itself, which keeps only those edges.
    fn start_callee_case_on_edges(
        &mut self,
        join: BlockId,
        callee_phi: NodeId,
        case: usize,
        edges: &[BlockId],
    ) -> BlockId {
        if case == 0 {
            let block = &self.graph.blocks[join.index()];
            let kept_inputs = block
                .predecessors
                .iter()
                .zip(&self.graph.node(callee_phi).inputs)
                .filter(|(predecessor, _)| edges.contains(predecessor))
                .map(|(_, input)| *input)
                .collect::<Vec<_>>();
            self.graph.nodes[callee_phi.index()].inputs = kept_inputs;
            self.graph.blocks[join.index()].predecessors = edges.to_vec();
            return join;
        }
        let block = self.graph.add_block(Block {
            predecessors: edges.to_vec(),
            ..Block::default()
        });
        self.order.push(block);
        for predecessor in edges {
            self.graph.replace_successor(*predecessor, join, block);
        }
        block
    }

    /// Inlines a `Call` whose callee is one of a few function constants (see
    /// `callee_cases()`): an inlined call of each, or the call's slow path
    /// for those that cannot be inlined, on the edges that bring it where
    /// it can (see `callee_case_edges()`), and branching on the callee
    /// otherwise. Returns `None`, having built nothing, if the callee is no
    /// such phi.
    fn try_inline_callee_cases(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            dst,
            callee,
            this_value,
            argument_count,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        let Some(cases) = self.callee_cases(*callee) else {
            return Ok(None);
        };
        // NB: Without a case to inline or call directly, one generic call does
        //     better than a slow path call for each case.
        if !cases.iter().any(|(bits, index)| {
            index.is_some_and(|index| self.may_inline_callee_case(index))
                || self.direct_call_functions.contains_key(bits)
        }) {
            return Ok(None);
        }
        let Handling::Generic(info) = handling(OpCode::Call) else {
            unreachable!("calls are generic instructions");
        };
        // NB: Every case continues with the frame the call starts with, but
        //     for the destination, which the cases' values join in. The slow
        //     path keeps the destination in an SSA value, and touches no
        //     other slot.
        let Some(dst_slot) = self.function.layout.tracked_index(*dst) else {
            return Ok(None);
        };
        let operands = std::iter::once(*callee)
            .chain(std::iter::once(*this_value))
            .chain(arguments.iter().copied())
            .collect::<Vec<_>>();
        if self.graph.legacy_arguments.contains(&dst.raw())
            || info.reads_whole_frame
            || crate::codegen::slow_path_call(OpCode::Call).is_none()
            || operands.iter().any(|operand| self.holds_virtual_arguments(*operand))
        {
            return Ok(None);
        }
        let SlotState {
            node: callee_phi,
            in_sync: callee_in_sync,
        } = self.peek(*callee).expect("the callee is a phi");
        let callee_slot = self
            .function
            .layout
            .tracked_index(*callee)
            .expect("the callee is in a slot");
        let edges = self.callee_case_edges(callee_phi, &cases);
        let callee_value = match edges {
            Some(_) => None,
            None => Some(self.read(*callee)?),
        };
        let join = self.block;
        let frame = self.frame.clone();
        let mut results = Vec::new();
        let mut case_shapes = Vec::new();
        for (case, (bits, index)) in cases.iter().enumerate() {
            let mut next_case = None;
            match (&edges, callee_value) {
                (Some(edges), _) => {
                    self.block = self.start_callee_case_on_edges(join, callee_phi, case, &edges[case]);
                    // NB: Here the callee is the case's function, which frame
                    //     states of the case give it as.
                    let function = self.constant(*bits);
                    self.frame.slots[callee_slot] = SlotState {
                        node: function,
                        in_sync: callee_in_sync,
                    };
                    self.function.eager_frame_state = None;
                }
                // NB: The callee is one of the cases, so the last one needs
                //     no branch.
                (None, Some(callee_value)) if case + 1 < cases.len() => {
                    let expected = self.constant(*bits);
                    let (if_true, if_false) = self.branch_both_ways(
                        BranchCondition::TaggedEquals { equal: true },
                        vec![callee_value, expected],
                    );
                    self.block = if_true;
                    next_case = Some(if_false);
                }
                _ => {}
            }
            let inlined = match index {
                Some(index) if self.may_inline_callee_case(*index) => self.inline_call(InlinedCallSite::new(
                    *index,
                    InlineValue::Operand(*this_value),
                    arguments.iter().copied().map(InlineValue::Operand).collect(),
                    *argument_count,
                    *dst,
                ))?,
                _ => None,
            };
            let inlined = match inlined {
                Some(flow) => Some(flow),
                None => self.try_direct_call_of_function(instruction, *bits)?,
            };
            let flow = match inlined {
                Some(flow) => flow,
                None => self
                    .try_build_slow_path_call(instruction, info)?
                    .expect("calls have a slow path on values"),
            };
            if matches!(flow, Flow::Continue) {
                let node = self.frame.slots[dst_slot].node;
                debug_assert!(
                    (0..frame.slots.len()).all(|slot| slot == dst_slot
                        || (slot == callee_slot && edges.is_some())
                        || frame.slots[slot] == self.frame.slots[slot]),
                    "cases change no slot but the destination"
                );
                results.push((self.block, node, *bits));
                case_shapes.push(std::mem::take(&mut self.frame.fresh_shapes));
            }
            self.frame = frame.clone();
            if let Some(next_case) = next_case {
                self.block = next_case;
            }
        }
        self.function.eager_frame_state = None;
        if results.is_empty() {
            return Ok(Some(Flow::Ended));
        }
        self.join_blocks(results.iter().map(|(block, _, _)| *block).collect());
        if edges.is_some() {
            // NB: The callee's phi no longer reaches here.
            let functions = results
                .iter()
                .map(|(_, _, bits)| self.constant(*bits))
                .collect::<Vec<_>>();
            let function = if functions.iter().all(|function| *function == functions[0]) {
                functions[0]
            } else {
                self.add_phi(functions)
            };
            self.frame.slots[callee_slot] = SlotState {
                node: function,
                in_sync: callee_in_sync,
            };
        }
        let values = results
            .iter()
            .map(|(block, value, _)| self.tagged_at_end_of(*block, *value))
            .collect::<Vec<_>>();
        let result = if values.iter().all(|value| *value == values[0]) {
            values[0]
        } else {
            self.add_phi(values)
        };
        self.frame.fresh_shapes = AbstractFrame::common_fresh_shapes(case_shapes.iter().map(Vec::as_slice));
        self.write(*dst, result)?;
        Ok(Some(Flow::Continue))
    }

    /// Inlines the call of the getter whose executable is snapshot executable
    /// `index` with `this` as its `this`, into `dst`, for a `GetById` that
    /// checked that it calls that getter, or with `getter`, the function it
    /// calls, checked to be a closure of that executable. Returns `None`,
    /// having built nothing that matters, if it does not.
    pub(super) fn try_inline_getter_call(
        &mut self,
        index: u32,
        getter: Option<NodeId>,
        this: Operand,
        dst: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        self.inline_call(InlinedCallSite {
            closure: getter,
            check_closure: getter.is_some(),
            ..InlinedCallSite::new(index, InlineValue::Operand(this), Vec::new(), 0, dst)
        })
    }

    /// Inlines the call of the setter whose executable is snapshot executable
    /// `index` with `this` as its `this` and `value` as its argument, for a
    /// `PutById` that checked that it calls that setter, or with `setter`
    /// like `try_inline_getter_call()`. The setter's result goes into `dst`,
    /// which nothing reads. Returns `None`, having built nothing that
    /// matters, if it does not.
    pub(super) fn try_inline_setter_call(
        &mut self,
        index: u32,
        setter: Option<NodeId>,
        this: Operand,
        value: Operand,
        dst: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        self.inline_call(InlinedCallSite {
            closure: setter,
            check_closure: setter.is_some(),
            ..InlinedCallSite::new(
                index,
                InlineValue::Operand(this),
                vec![InlineValue::Operand(value)],
                1,
                dst,
            )
        })
    }

    /// Inlines a `Call` of a closure the compiled function creates itself,
    /// whose executable is that of its `NewFunction` instruction. Needs no
    /// feedback and no check. Returns `None`, having built nothing that
    /// matters, if the callee is no such closure.
    fn try_inline_closure_call(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            dst,
            callee,
            this_value,
            argument_count,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        let Some(SlotState { node: closure, .. }) = self.peek(*callee) else {
            return Ok(None);
        };
        let Op::AllocateFunction {
            executable,
            shared_function_data_index,
            ..
        } = self.graph.node(closure).op
        else {
            return Ok(None);
        };
        let Some(index) = self.snapshot.executables[executable as usize]
            .closure_templates
            .get(shared_function_data_index as usize)
            .and_then(Option::as_ref)
            .and_then(|template| template.inline_executable)
        else {
            return Ok(None);
        };
        let call = InlinedCallSite {
            closure: Some(closure),
            ..InlinedCallSite::new(
                index,
                InlineValue::Operand(*this_value),
                arguments.iter().copied().map(InlineValue::Operand).collect(),
                *argument_count,
                *dst,
            )
        };
        self.inline_call(call)
    }

    /// Inlines a `CallConstruct` whose feedback saw a single constructor
    /// whose construct JIT code can inline (see `ConstructTarget`). Returns
    /// `None`, having built nothing that matters, if it does not.
    pub(super) fn try_inline_construct(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::CallConstruct {
            call_feedback,
            dst,
            callee,
            argument_count,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        let Some(feedback) = self.call_feedback(*call_feedback) else {
            return Ok(None);
        };
        let Some(construct) = feedback.construct else {
            return Ok(None);
        };
        if !self.may_speculate(ExitKind::BadShape) || !self.may_speculate(ExitKind::UnexpectedValue) {
            return Ok(None);
        }
        if let Some(super_constructor) = construct.super_constructor
            && !self.may_inline_super_constructor(construct.executable, super_constructor)
        {
            return Ok(None);
        }
        let call = InlinedCallSite {
            callee: Some(*callee),
            construct: Some(construct),
            ..InlinedCallSite::new(
                construct.executable,
                InlineValue::Constant(value::UNDEFINED),
                arguments.iter().copied().map(InlineValue::Operand).collect(),
                *argument_count,
                *dst,
            )
        };
        self.inline_call(call)
    }

    /// Whether the super() of an inlined construct of the derived
    /// constructor `constructor` here can inline the construct of `target`
    /// (see `try_inline_super_call()`), one inlined call deeper, and so on
    /// down the chain of super constructors if `target` is derived too. Each
    /// construct is checked like `inline_call()` checks it (see
    /// `inlinable_callee()`), and no super() in the chain may have exited
    /// because `inline_call()` refused its construct after all.
    fn may_inline_super_constructor(&self, constructor: u32, mut target: SuperConstructTarget) -> bool {
        let Some(constructor_executable) = self.snapshot.executables.get(constructor as usize) else {
            return false;
        };
        if self.super_call_exited(constructor) {
            return false;
        }
        // NB: The constructors inlined above `target` by then, from the derived constructor here down.
        let mut inlined_above = vec![constructor_executable.cell];
        loop {
            let kind = if target.derived {
                InlinedCallKind::DerivedConstruct
            } else {
                InlinedCallKind::Construct
            };
            let Some(InlinableCallee { executable, .. }) =
                self.inlinable_callee(target.executable, kind, false, true, &inlined_above)
            else {
                return false;
            };
            if !target.derived {
                return true;
            }
            if self.super_call_exited(target.executable) {
                return false;
            }
            let Some(next) = executable.super_constructor else {
                return false;
            };
            inlined_above.push(executable.cell);
            target = next;
        }
    }

    /// Whether compiled code exited at a super() of the derived constructor
    /// of snapshot executable `index` because `inline_call()` refused the
    /// construct there (see `try_inline_super_call()`).
    fn super_call_exited(&self, index: u32) -> bool {
        let Some(executable) = self.snapshot.executables.get(index as usize) else {
            return true;
        };
        let Ok(decoded) = super::decoded_function(executable, &self.decoded[index as usize]) else {
            return true;
        };
        decoded.instructions.iter().any(|instruction| {
            matches!(
                instruction.instruction.opcode(),
                OpCode::SuperCall | OpCode::SuperCallWithForwardedArguments
            ) && executable
                .exit_sites
                .contains(&(instruction.pc, ExitKind::BadCallTarget))
        })
    }

    /// `GetSuperConstructor` in an inlined construct of a derived
    /// constructor: its [[Prototype]], which stays the same while its shape
    /// does.
    pub(super) fn try_build_super_constructor(&mut self, dst: Operand) -> Result<bool, CompileFailure> {
        let Some(construct) = self.function.inline.as_ref().and_then(|call| call.derived_construct) else {
            return Ok(false);
        };
        let (function, target) = self.derived_construct_function(&construct)?;
        // 13.3.7.2 GetSuperConstructor ( ), https://tc39.es/ecma262/#sec-getsuperconstructor
        // 1. Let envRec be GetThisEnvironment().
        // 2. Assert: envRec is a Function Environment Record.
        // 3. Let activeFunction be envRec.[[FunctionObject]].
        // 4. Assert: activeFunction is an ECMAScript function object.
        // 5. Let superConstructor be ! activeFunction.[[GetPrototypeOf]]().
        let function = self.boxed_object(function);
        let function = self.constant(function);
        self.check_shapes(
            function,
            &[ShapeCheck {
                shape: target.function_shape,
                dictionary_generation: None,
            }],
        );
        // 6. Return superConstructor.
        self.embed(target.function);
        let super_constructor = self.boxed_object(target.function);
        let super_constructor = self.constant(super_constructor);
        self.write(dst, super_constructor)?;
        Ok(true)
    }

    /// The derived constructor of an inlined construct, and the
    /// constructor its super() constructs with.
    fn derived_construct_function(
        &self,
        construct: &ConstructTarget,
    ) -> Result<(CellId, SuperConstructTarget), CompileFailure> {
        let invalid = |reason| CompileFailure::InvalidBytecode {
            pc: self.function.pc,
            reason,
        };
        let function = self
            .function
            .executable
            .function
            .ok_or(invalid("inlined construct without a known function"))?;
        let target = construct
            .super_constructor
            .ok_or(invalid("derived construct without a super constructor"))?;
        Ok((function.function, target))
    }

    /// `SuperCall` in an inlined construct of a derived constructor: inlines
    /// the construct of its super constructor, with the derived constructor
    /// as the new target. Its result is `this`, which `BindThisFromSuperCall`
    /// binds (see `try_build_bind_this_from_super_call()`).
    pub(super) fn try_inline_super_call(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Some(construct) = self.function.inline.as_ref().and_then(|call| call.derived_construct) else {
            return Ok(None);
        };
        let (dst, super_constructor, arguments, argument_count, forwarded) = match instruction {
            Instruction::SuperCall {
                dst,
                super_constructor,
                argument_count,
                arguments,
                ..
            } => (
                dst,
                super_constructor,
                arguments.iter().copied().map(InlineValue::Operand).collect::<Vec<_>>(),
                *argument_count,
                false,
            ),
            // NB: The passed arguments of the derived constructor are all known, as it keeps them like legacy
            //     `arguments` (see `inline_call()`).
            Instruction::SuperCallWithForwardedArguments {
                dst, super_constructor, ..
            } => {
                let call = self
                    .function
                    .inline
                    .as_ref()
                    .expect("derived constructs are inlined calls");
                let count = call.passed_argument_count;
                let arguments = self
                    .legacy_argument_values()
                    .into_iter()
                    .take(count as usize)
                    .map(|(_, node)| InlineValue::Node(node))
                    .collect::<Vec<_>>();
                if arguments.len() != count as usize {
                    return Err(CompileFailure::InvalidBytecode {
                        pc: self.function.pc,
                        reason: "forwarded arguments of an inlined derived construct are not known",
                    });
                }
                (dst, super_constructor, arguments, count, true)
            }
            _ => return Ok(None),
        };
        let (function, target) = self.derived_construct_function(&construct)?;
        // NB: GetSuperConstructor put the super constructor there.
        let super_constructor_value = self.read(*super_constructor)?;
        let expected = self.boxed_object(target.function);
        if self.graph.constant_value(super_constructor_value) != Some(expected) {
            self.emit_checked(
                Op::CheckValue {
                    expected,
                    kind: ExitKind::UnexpectedValue,
                },
                vec![super_constructor_value],
                None,
            );
        }
        // 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        // 6. Let result be ? Construct(superCtor, argList, newTarget).
        // NB: OrdinaryCreateFromConstructor of the construct reads "prototype" of the new target, the derived
        //     constructor.
        // NB: A derived super constructor's super() constructs with its own parent (see
        //     `may_inline_super_constructor()`).
        let next_super_constructor = if target.derived {
            let next = self
                .snapshot
                .executables
                .get(target.executable as usize)
                .and_then(|executable| executable.super_constructor);
            if next.is_none() {
                return Err(CompileFailure::InvalidBytecode {
                    pc: self.function.pc,
                    reason: "derived super constructor without a super constructor",
                });
            }
            next
        } else {
            None
        };
        let new_target = self.function.inline.as_ref().and_then(|call| call.new_target);
        let call = InlinedCallSite {
            construct: Some(ConstructTarget {
                executable: target.executable,
                super_constructor: next_super_constructor,
                ..construct
            }),
            new_target: Some(new_target.unwrap_or(function)),
            forwarded,
            ..InlinedCallSite::new(
                target.executable,
                InlineValue::Constant(value::UNDEFINED),
                arguments,
                argument_count,
                *dst,
            )
        };
        if let Some(flow) = self.inline_call(call)? {
            return Ok(Some(flow));
        }
        // NB: `may_inline_super_constructor()` checks ahead what `inline_call()` checks, but not the values at the
        //     call site, like a virtual arguments object, nor what may be speculated here. A generic SuperCall
        //     cannot run in the inlined frame, which is not the running frame: exit, which keeps the next
        //     compilation from inlining the construct.
        let frame_state = self.eager_frame_state();
        let exit = self.set_control(
            Op::Exit {
                kind: ExitKind::BadCallTarget,
            },
            Vec::new(),
        );
        self.graph.nodes[exit.index()].frame_state = Some(frame_state);
        Ok(Some(Flow::Ended))
    }

    /// `BindThisFromSuperCall` in an inlined construct of a derived
    /// constructor whose fields JIT code can define (see
    /// `ExecutableSnapshot::derived_instance_fields`), whose `this` is still
    /// empty there and whose super() created `this` in the graph: binds it
    /// and defines the fields after checking that `this` has the shape they
    /// start from. Returns false, having built nothing, otherwise.
    pub(super) fn try_build_bind_this_defining_fields(&mut self, result: Operand) -> Result<bool, CompileFailure> {
        if self
            .function
            .inline
            .as_ref()
            .and_then(|call| call.derived_construct)
            .is_none()
            || !self.may_speculate(ExitKind::BadShape)
        {
            return Ok(false);
        }
        let Some(fields) = self.function.executable.derived_instance_fields.clone() else {
            return Ok(false);
        };
        let this_register = Operand::from_raw(THIS_VALUE_REGISTER);
        let this_value = self.read(this_register)?;
        if self.graph.constant_value(this_value) != Some(value::EMPTY) {
            return Ok(false);
        }
        let object = self.read(result)?;
        let allocation = self.graph.unrefined(object);
        let Op::AllocateObject {
            shape: allocated_shape,
            property_count,
            reserve,
        } = self.graph.node(allocation).op
        else {
            return Ok(false);
        };
        let added_count = fields.fields.iter().map(|(offset, _, _)| offset + 1).max().unwrap_or(0);
        let reserve = reserve.max(added_count);
        if self.runtime.object_allocation.size_class_for(reserve).is_none() {
            return Ok(false);
        }
        self.graph.nodes[allocation.index()].op = Op::AllocateObject {
            shape: allocated_shape,
            property_count,
            reserve,
        };

        // NB: The shape check exits before `this` is bound, to the BindThisFromSuperCall in the interpreter.
        let object = self.check_shapes(
            object,
            &[ShapeCheck {
                shape: fields.shape,
                dictionary_generation: None,
            }],
        );
        // 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        // 9. Perform ? BindThisValue(thisER, result).
        // NB: The result of the inlined construct is its `this`, an object.
        self.write(this_register, object)?;
        // 10. Let funcObj be thisER.[[FunctionObject]].
        // 11. Assert: funcObj is an ECMAScript function object.
        // 12. Perform ? InitializeInstanceElements(result, funcObj).
        // NB: The fields have literal values, so this cannot throw, and since `this` has the shape the fields start
        //     from (checked above), which allows extensions and has none of their names, each field adds its property
        //     or sets the value of an earlier field of the same name.
        for (offset, value, added) in fields.fields {
            let value = self.constant(value);
            match added {
                Some(shape) => {
                    self.embed(shape);
                    let address = self.cell_address(object);
                    self.emit(
                        Op::AddNamed {
                            offset,
                            shape,
                            property_count: offset + 1,
                        },
                        vec![object, value, address],
                        None,
                    );
                }
                None => self.store_named_holding(object, offset, value, true),
            }
        }
        // 13. Return result.
        Ok(true)
    }

    /// `BindThisFromSuperCall` in an inlined construct of a derived
    /// constructor with fields or private methods, whose `this` is still
    /// empty there (see `calls_super_once_before_returning()`): its slow
    /// path, which runs in the published frames of the inlined calls, binds
    /// `this` and initializes the instance elements. Its result is `this`
    /// once it continues. Returns `None` for other constructors.
    pub(super) fn try_build_bind_this_initializing_instance_elements(
        &mut self,
        instruction: &Instruction,
        result: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        if self
            .function
            .inline
            .as_ref()
            .and_then(|call| call.derived_construct)
            .is_none()
            || !self.function.executable.has_instance_elements
        {
            return Ok(None);
        }
        let this_register = Operand::from_raw(THIS_VALUE_REGISTER);
        let this_value = self.read(this_register)?;
        if self.graph.constant_value(this_value) != Some(value::EMPTY) {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "super() of an inlined derived construct with this bound",
            });
        }
        let invalid = CompileFailure::InvalidBytecode {
            pc: self.function.pc,
            reason: "BindThisFromSuperCall has no slow path call",
        };
        let info = super::handling::handling(OpCode::BindThisFromSuperCall)
            .generic()
            .ok_or(invalid)?;
        // NB: The published frame of the derived constructor has the empty this value of the frame state, which the
        //     slow path binds.
        let flow = self.try_build_slow_path_call(instruction, info)?.ok_or(invalid)?;
        // 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        // 9. Perform ? BindThisValue(thisER, result).
        let result = self.read(result)?;
        self.write(this_register, result)?;
        Ok(Some(flow))
    }

    /// `BindThisFromSuperCall` in an inlined construct of a derived
    /// constructor, whose `this` is still empty there (see
    /// `calls_super_once_before_returning()`) and which has no fields.
    pub(super) fn try_build_bind_this_from_super_call(&mut self, result: Operand) -> Result<bool, CompileFailure> {
        if self
            .function
            .inline
            .as_ref()
            .and_then(|call| call.derived_construct)
            .is_none()
        {
            return Ok(false);
        }
        let this_register = Operand::from_raw(THIS_VALUE_REGISTER);
        let this_value = self.read(this_register)?;
        if self.graph.constant_value(this_value) != Some(value::EMPTY) {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "super() of an inlined derived construct with this bound",
            });
        }
        // 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        // 7. Let thisER be GetThisEnvironment().
        // 8. Assert: thisER is a Function Environment Record.
        // 9. Perform ? BindThisValue(thisER, result).
        // NB: The result of the inlined construct is its `this`, an object.
        let result = self.read(result)?;
        self.write(this_register, result)?;
        // 10. Let funcObj be thisER.[[FunctionObject]].
        // 11. Assert: funcObj is an ECMAScript function object.
        // 12. Perform ? InitializeInstanceElements(result, funcObj).
        // NB: The derived constructor has no fields or private methods.
        // 13. Return result.
        Ok(true)
    }

    /// `ResolveSuperBase` in an inlined callee, whose home object is known: its prototype, which stays the same while
    /// its shape does. The interpreter's slow path reads the running function, which is the caller's here.
    pub(super) fn try_build_resolve_super_base(&mut self, dst: Operand) -> Result<bool, CompileFailure> {
        if !self.function.is_virtual() {
            return Ok(false);
        }
        let super_base = self
            .function
            .executable
            .function
            .and_then(|function| function.super_base)
            .ok_or(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "inlined ResolveSuperBase without a known home object",
            })?;
        self.embed(super_base.home);
        let home = self.boxed_object(super_base.home);
        let home = self.constant(home);
        self.check_shapes(
            home,
            &[ShapeCheck {
                shape: super_base.home_shape,
                dictionary_generation: None,
            }],
        );
        let base = match super_base.prototype {
            Some(prototype) => {
                self.embed(prototype);
                self.boxed_object(prototype)
            }
            None => value::NULL,
        };
        let base = self.constant(base);
        self.write(dst, base)?;
        Ok(true)
    }

    /// `GetNewTarget` in an inlined callee with its own new.target: undefined for a call, and for a construct the
    /// constructor, which is the new target of the constructs JIT code inlines. The interpreter's slow path reads the
    /// running frame, which is the caller's here.
    pub(super) fn try_build_new_target(&mut self, dst: Operand) -> Result<bool, CompileFailure> {
        let Some(call) = self.function.inline.as_ref() else {
            return Ok(false);
        };
        let value = if let Some(new_target) = call.new_target {
            self.embed(new_target);
            self.boxed_object(new_target)
        } else {
            value::UNDEFINED
        };
        let value = self.constant(value);
        self.write(dst, value)?;
        Ok(true)
    }

    /// `this` of an inlined construct of `function`: the object its
    /// construct creates, a plain object of the prototype in its
    /// "prototype", which must still be the one it was at compile time,
    /// with the `fields` of the base constructor (see
    /// `ExecutableSnapshot::literal_instance_fields`).
    fn construct_this(&mut self, function: CellId, construct: &ConstructTarget, fields: &[(u32, u64)]) -> NodeId {
        // 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
        let constructor = self.boxed_object(function);
        let constructor = self.constant(constructor);
        let constructor = self.check_shapes(
            constructor,
            &[ShapeCheck {
                shape: construct.function_shape,
                dictionary_generation: None,
            }],
        );
        self.load_constant_named(constructor, construct.prototype_offset, construct.prototype);
        self.embed(construct.this_shape);
        let property_count = fields.iter().map(|(offset, _)| offset + 1).max().unwrap_or(0);
        let this = self.emit(
            Op::AllocateObject {
                shape: construct.this_shape,
                property_count,
                reserve: construct.reserve.max(property_count),
            },
            Vec::new(),
            Some(Repr::Tagged),
        );
        // 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
        // 6.b. Let initializeResult be Completion(InitializeInstanceElements(thisArgument, F)).
        // NB: The fields have literal values, so this cannot throw, and `this` has their shape already.
        for &(offset, value) in fields {
            let value = self.constant(value);
            self.emit(Op::InitializeNamed { offset }, vec![this, value], None);
        }
        this
    }

    /// The value of `value` for an inlined call.
    fn inline_value(&mut self, value: InlineValue) -> Result<NodeId, CompileFailure> {
        match value {
            InlineValue::Operand(operand) => self.read(operand),
            InlineValue::Constant(bits) => Ok(self.constant(bits)),
            InlineValue::Node(node) => Ok(node),
            InlineValue::FrameArgument(index) => {
                let index = self.constant(value::int32(index.cast_signed()));
                let arguments_base = self.function.layout.arguments_base();
                Ok(self.emit_checked(Op::LoadArgument { arguments_base }, vec![index], Some(Repr::Tagged)))
            }
        }
    }

    /// Whether calls of `callee_executable` may be inlined here at all: it runs in at most two of the frames being
    /// compiled already, the inlined frames are not as deep as they may be, and no inlined call exited here before.
    /// This lets recursive functions inline their own calls twice over, and lets functions that share one executable
    /// inline each other, like the constructors that `Class.create()` returns in Prototype-style code.
    fn may_inline_calls_of(&self, callee_executable: &crate::snapshot::ExecutableSnapshot) -> bool {
        self.may_inline_calls_of_below(callee_executable, &[])
    }

    /// Like `may_inline_calls_of()`, once the calls of the executables
    /// `inlined_above` (their cells) are inlined here, one in the other.
    fn may_inline_calls_of_below(
        &self,
        callee_executable: &crate::snapshot::ExecutableSnapshot,
        inlined_above: &[CellId],
    ) -> bool {
        self.may_inline_unchecked_calls_of_below(callee_executable, inlined_above)
            && self.may_speculate(ExitKind::BadCallTarget)
    }

    /// Like `may_inline_calls_of_below()`, for a call that inlines its
    /// callee without checking it, since the code knows exactly what it is:
    /// such a call cannot exit for another callee, even where checks of
    /// the callee exited before.
    fn may_inline_unchecked_calls_of_below(
        &self,
        callee_executable: &crate::snapshot::ExecutableSnapshot,
        inlined_above: &[CellId],
    ) -> bool {
        const MAX_RECURSION_DEPTH: usize = 2;
        let recursion_depth = core::iter::once(&self.function)
            .chain(&self.callers)
            .map(|function| function.executable.cell)
            .chain(inlined_above.iter().copied())
            .filter(|cell| *cell == callee_executable.cell)
            .count();
        recursion_depth <= MAX_RECURSION_DEPTH
            && self.callers.len() + inlined_above.len() < self.snapshot.options.inlining.max_depth as usize
    }

    /// Whether a call with a few callees (see `try_inline_callee_cases()`)
    /// may inline its call of snapshot executable `index`: calls of it may
    /// be inlined here, and it fits in the inlining budget. Small callees
    /// are charged to the budget here too, as one call inlines up to
    /// `MAX_CALLEE_CASES` of them, which may have calls with a few callees
    /// themselves.
    fn may_inline_callee_case(&self, index: u32) -> bool {
        let Some(callee_executable) = self.snapshot.executables.get(index as usize) else {
            return false;
        };
        if callee_executable.function.is_none() || !self.may_inline_calls_of(callee_executable) {
            return false;
        }
        // NB: Builtins are always inlined (see the snapshot's inlining rules).
        if callee_executable.builtin {
            return true;
        }
        let Ok(decoded) = super::decoded_function(callee_executable, &self.decoded[index as usize]) else {
            return false;
        };
        self.inlined_instructions + decoded.instructions.len()
            <= self.snapshot.options.inlining.budget_instructions as usize
    }

    /// The snapshot executable `index`, if a call of the
    /// kind `kind` (a closure call if `closure`, which checks its callee if
    /// `checks_callee`) may inline it here once the
    /// executables `inlined_above` (their cells) are inlined here, one in the
    /// other: the checks of `inline_call()` that do not depend on the values
    /// at the call site, which `may_inline_super_constructor()` makes ahead
    /// of the calls that inlined constructs inline for super().
    fn inlinable_callee(
        &self,
        index: u32,
        kind: InlinedCallKind,
        closure: bool,
        checks_callee: bool,
        inlined_above: &[CellId],
    ) -> Option<InlinableCallee<'a>> {
        let snapshot = self.snapshot;
        let callee_executable = snapshot.executables.get(index as usize)?;
        callee_executable.function?;
        let may_inline = if checks_callee {
            self.may_inline_calls_of_below(callee_executable, inlined_above)
        } else {
            self.may_inline_unchecked_calls_of_below(callee_executable, inlined_above)
        };
        if !may_inline {
            return None;
        }
        let decoded = super::decoded_function(callee_executable, &self.decoded[index as usize]).ok()?;
        let instruction_count = decoded.instructions.len();
        let limits = snapshot.options.inlining;
        // NB: Builtins are always inlined (see the snapshot's inlining rules).
        let within_budget = callee_executable.builtin
            || crate::bytecode::inlining_size(&decoded.instructions) <= limits.always_inlined_instructions as usize
            || (kind != InlinedCallKind::Call && instruction_count <= limits.max_construct_instructions as usize)
            || (instruction_count <= limits.max_instructions as usize
                && self.inlined_instructions + instruction_count <= limits.budget_instructions as usize);
        if !within_budget {
            return None;
        }
        // NB: CheckClosure checks shared data, not legacy-property eligibility.
        //     Keep non-strict closure calls conservative. Frames with a virtual
        //     arguments object keep every passed argument too, which exits
        //     create the object from.
        let virtual_arguments =
            self.virtualize_arguments.compiled_function && self.virtualize_arguments.inlined_callees;
        let uses_passed_arguments = decoded.instructions.iter().any(|instruction| {
            matches!(
                instruction.instruction.opcode(),
                OpCode::CreateArguments | OpCode::GetArgument
            )
        });
        // NB: A derived constructor that passes its arguments on to super() keeps them all, like legacy `arguments`.
        let derived_construct = kind == InlinedCallKind::DerivedConstruct;
        let forwards_arguments_to_super = derived_construct
            && decoded
                .instructions
                .iter()
                .any(|instruction| instruction.instruction.opcode() == OpCode::SuperCallWithForwardedArguments);
        let has_legacy_arguments = callee_executable.supports_legacy_arguments
            || (closure && callee_executable.is_non_strict_function())
            || (virtual_arguments && uses_passed_arguments)
            || forwards_arguments_to_super;
        let callee_function = Function::new(index, callee_executable, decoded, has_legacy_arguments).ok()?;
        if !can_inline(&callee_function, virtual_arguments, derived_construct) {
            return None;
        }
        // NB: Closures of a function share its code but may have other home objects, and the base of `super` property
        //     access is guarded by a shape check.
        let resolves_super_base = callee_function
            .instructions
            .iter()
            .any(|instruction| instruction.instruction.opcode() == OpCode::ResolveSuperBase);
        if resolves_super_base && (closure || !self.may_speculate(ExitKind::BadShape)) {
            return None;
        }
        let qualifies = match kind {
            InlinedCallKind::Call => true,
            InlinedCallKind::Construct => {
                returns_no_object(&callee_function)
                    && (!callee_executable.has_instance_elements || callee_executable.literal_instance_fields.is_some())
            }
            InlinedCallKind::DerivedConstruct => calls_super_once_before_returning(&callee_function),
        };
        qualifies.then_some(InlinableCallee {
            executable: callee_executable,
            function: callee_function,
            has_legacy_arguments,
        })
    }

    /// Inlines the call `call` describes, if its callee qualifies.
    fn inline_call(&mut self, call: InlinedCallSite) -> Result<Option<Flow>, CompileFailure> {
        let InlinedCallSite {
            index,
            callee,
            expected_callee,
            this,
            arguments,
            passed_argument_count,
            forwarded,
            dst,
            construct,
            new_target,
            closure,
            check_closure,
            callback,
        } = call;
        let derived_construct = construct.filter(|construct| construct.super_constructor.is_some());
        // NB: The callee's frame cannot hold the virtual arguments object:
        //     the callee's generic instructions could not read it.
        if std::iter::once(&this)
            .chain(arguments.iter())
            .any(|value| matches!(value, InlineValue::Operand(operand) if self.holds_virtual_arguments(*operand)))
        {
            return Ok(None);
        }
        let Some(InlinableCallee {
            executable: callee_executable,
            function: mut callee_function,
            has_legacy_arguments,
        }) = self.inlinable_callee(
            index,
            match (construct, derived_construct) {
                (None, _) => InlinedCallKind::Call,
                (Some(_), None) => InlinedCallKind::Construct,
                (Some(_), Some(_)) => InlinedCallKind::DerivedConstruct,
            },
            closure.is_some(),
            callee.is_some() || check_closure || construct.is_some(),
            &[],
        )
        else {
            return Ok(None);
        };
        // NB: What a real frame would read for an argument that the call did not pass depends on %Object.prototype%.
        if arguments::reads_argument_not_passed(&callee_function, passed_argument_count) {
            return Ok(None);
        }
        let function = callee_executable.function.expect("inlined callees have a function");
        let instruction_count = callee_function.instructions.len();

        // NB: Bind `this` like the interpreter's inline calls do.
        let receiver = if construct.is_some() {
            // NB: Allocated once the callee is checked, below.
            self.constant(value::EMPTY)
        } else if !function.uses_this {
            self.constant(value::EMPTY)
        } else {
            let this = self.inline_value(this)?;
            if function.strict {
                this
            } else {
                match self.graph.constant_value(this) {
                    Some(value::UNDEFINED | value::NULL) => {
                        self.embed(function.global_this);
                        let global_this = self.boxed_object(function.global_this);
                        self.constant(global_this)
                    }
                    // NB: Objects are bound as they are; primitives would be boxed.
                    Some(bits) if value::tag(bits) == value::OBJECT_TAG => this,
                    Some(_) => return Ok(None),
                    None if self.may_speculate(ExitKind::NotObject) => self.check_object(this),
                    None => return Ok(None),
                }
            }
        };

        self.embed(function.function);
        // NB: Materialized frames store the realm.
        self.embed(function.realm);
        if let Some(closure) = closure
            && check_closure
        {
            self.embed(function.shared_data);
            self.emit_checked(
                Op::CheckClosure {
                    shared_data: function.shared_data,
                },
                vec![closure],
                None,
            );
        }
        let expected_callee = expected_callee.unwrap_or(function.function);
        self.embed(expected_callee);
        // NB: A callee folded to this function (a method from a prototype)
        //     needs no check.
        if let Some(callee) = callee {
            self.check_callee(callee, expected_callee)?;
        }
        // NB: super() creates `this` of a derived constructor.
        let receiver = match &construct {
            Some(construct) if derived_construct.is_none() => self.construct_this(
                new_target.unwrap_or(function.function),
                construct,
                callee_executable.literal_instance_fields.as_deref().unwrap_or_default(),
            ),
            _ => receiver,
        };
        // NB: Arguments forwarded from the frame are as many as when the
        //     forwarding was seen.
        if arguments
            .iter()
            .any(|argument| matches!(argument, InlineValue::FrameArgument(_)))
        {
            let count = self.emit(Op::ArgumentCount, Vec::new(), Some(Repr::Tagged));
            self.emit_checked(
                Op::CheckValue {
                    expected: value::int32(passed_argument_count.cast_signed()),
                    kind: ExitKind::UnexpectedValue,
                },
                vec![count],
                None,
            );
        }
        let argument_values = arguments
            .into_iter()
            .map(|argument| self.inline_value(argument))
            .collect::<Result<Vec<_>, _>>()?;
        let argument_count = &passed_argument_count;
        let dst = &dst;

        // NB: Exits in the callee resume the caller after the call.
        let mut caller_live = self.function.liveness.live_out(self.function.instruction_index);
        if let Some(slot) = self.function.layout.tracked_index(*dst) {
            caller_live.remove(slot);
        }
        let caller_frame_state = FrameState {
            passed_argument_count: forwarded.then_some(passed_argument_count),
            ..self.new_frame_state(&caller_live, ResumeMode::ResumeAfter { dst: dst.raw() })
        };
        let caller_frame_state = self.graph.add_frame_state(caller_frame_state);
        let frame_bytes = self.function.inline.as_ref().map_or(0, |call| call.frame_bytes)
            + callee_executable.frame_size(self.runtime.offsets.execution_context_slots, *argument_count);
        self.graph.materialized_frame_bytes = self.graph.materialized_frame_bytes.max(frame_bytes);

        // NB: The callee's frame starts like the interpreter's inline calls set it up.
        let layout = callee_function.layout;
        let empty = self.constant(value::EMPTY);
        let undefined = self.constant(value::UNDEFINED);
        let mut slots = vec![
            SlotState {
                node: empty,
                in_sync: true,
            };
            layout.tracked_slot_count()
        ];
        slots[crate::bytecode::THIS_VALUE_REGISTER as usize] = SlotState {
            node: receiver,
            in_sync: true,
        };
        for argument in 0..layout.number_of_arguments {
            let slot = layout
                .tracked_index(Operand::from_raw(layout.arguments_base() + argument))
                .expect("arguments are tracked");
            let node = argument_values.get(argument as usize).copied().unwrap_or(undefined);
            slots[slot] = SlotState { node, in_sync: true };
        }
        let callee_frame = AbstractFrame {
            slots,
            fields: Default::default(),
            fresh_shapes: self.frame.fresh_shapes.clone(),
        };
        callee_function.inline = Some(InlineCall {
            caller_frame_state,
            caller_frame: self.frame.clone(),
            closure,
            callback,
            this: receiver,
            passed_argument_count: *argument_count,
            has_legacy_arguments,
            untracked_arguments: if has_legacy_arguments {
                argument_values
                    .get(layout.number_of_arguments as usize..)
                    .unwrap_or_default()
                    .to_vec()
            } else {
                Vec::new()
            },
            continuation: None,
            returns: Vec::new(),
            return_fresh_shapes: None,
            frame_bytes,
            construct: construct.is_some(),
            new_target: construct.map(|_| new_target.unwrap_or(function.function)),
            derived_construct,
        });

        // NB: Walk the callee's bytecode, then continue in the caller.
        let caller = std::mem::replace(&mut self.function, callee_function);
        self.callers.push(caller);
        self.frame = callee_frame;
        let call_block = self.block;
        self.enter_function(call_block);
        let result = self.build_function();
        let caller = self.callers.pop().expect("the caller was suspended");
        let callee_function = std::mem::replace(&mut self.function, caller);
        result?;
        self.inlined_instructions += instruction_count;

        let call = callee_function.inline.expect("inlined functions have a call");
        let Some(continuation) = call.continuation else {
            // NB: The callee never returns.
            return Ok(Some(Flow::Ended));
        };
        self.order.push(continuation);
        self.block = continuation;
        self.frame = call.caller_frame;
        self.frame.fresh_shapes = call.return_fresh_shapes.unwrap_or_default();
        let values = call.returns;
        let result = if values.iter().all(|value| *value == values[0]) {
            values[0]
        } else {
            self.add_phi(values)
        };
        self.write(*dst, result)?;
        Ok(Some(Flow::Continue))
    }

    /// Ends the current block of an inlined callee with a return of `value`
    /// to the continuation in the caller.
    pub(super) fn build_inline_return(
        &mut self,
        value: NodeId,
        empty_is_undefined: bool,
    ) -> Result<Flow, CompileFailure> {
        let call = self.function.inline.as_ref().expect("returns from inlined callees");
        let construct_this = if call.derived_construct.is_some() {
            // NB: The callee returns undefined only, once super() bound `this` (see
            //     `calls_super_once_before_returning()`), so the result is always `this`.
            Some(self.read(Operand::from_raw(THIS_VALUE_REGISTER))?)
        } else {
            call.construct.then_some(call.this)
        };
        let value = if let Some(this) = construct_this {
            // 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
            // NB: The callee returns no objects but `this` (see
            //     `returns_no_object`), so the result is always `this`.
            this
        } else if !empty_is_undefined {
            value
        } else {
            match self.graph.constant_value(value) {
                Some(value::EMPTY) => self.constant(value::UNDEFINED),
                Some(_) => value,
                None => self.emit(Op::EmptyToUndefined, vec![value], Some(Repr::Tagged)),
            }
        };
        let continuation = match self.function.inline.as_ref().and_then(|call| call.continuation) {
            Some(continuation) => continuation,
            None => {
                let continuation = self.graph.add_block(Block::default());
                self.function
                    .inline
                    .as_mut()
                    .expect("returns from inlined callees")
                    .continuation = Some(continuation);
                continuation
            }
        };
        let call = self.function.inline.as_mut().expect("returns from inlined callees");
        call.returns.push(value);
        let fresh_shapes = &self.frame.fresh_shapes;
        match &mut call.return_fresh_shapes {
            Some(shapes) => shapes.retain(|shape| fresh_shapes.contains(shape)),
            None => call.return_fresh_shapes = Some(fresh_shapes.clone()),
        }
        self.set_control(Op::Jump { target: continuation }, Vec::new());
        self.graph.blocks[continuation.index()].predecessors.push(self.block);
        Ok(Flow::Ended)
    }
}

/// The callback a call of a builtin that takes callbacks passed it, which such builtins record as forwarded calls.
fn forwarded_callback(feedback: &CallFeedbackSnapshot) -> Option<CallbackTarget> {
    let forwarded = feedback
        .forwarded
        .filter(|forwarded| forwarded.forwarding == Forwarding::Callback)?;
    Some(CallbackTarget {
        index: forwarded.inline_executable?,
        closures: feedback.flags & call_feedback_flags::FORWARDED_CLOSURES != 0,
    })
}
