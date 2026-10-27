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
use crate::snapshot::CellId;
use crate::snapshot::ConstructTarget;
use crate::snapshot::Forwarding;
use crate::snapshot::Intrinsic;
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
fn can_inline(callee: &Function<'_>, virtual_arguments: bool) -> bool {
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
        );
        !reads_frame_fields && !matches!(handling(opcode), Handling::Unsupported(_))
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
            closure: None,
            check_closure: false,
            callback: None,
        }
    }
}

impl GraphBuilder<'_> {
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

    /// The slots in `live` whose value frame memory holds, which a frame
    /// state of the function being walked lists without a value: the
    /// compiled function's slots that are in sync, and the slots a slow path
    /// wrote into the frames pushed for it. Reserved registers are not
    /// listed.
    fn frame_state_in_frame(&self, live: &BitSet) -> Vec<u32> {
        let is_virtual = self.function.is_virtual();
        // NB: Exits never clear the reserved registers, which frames keep.
        live.iter()
            .filter(|slot| *slot >= crate::bytecode::RESERVED_REGISTER_COUNT as usize)
            .filter(|slot| match self.frame.slots[*slot] {
                SlotState::InMemory => true,
                SlotState::Value { in_sync, .. } => in_sync && !is_virtual,
            })
            .map(|slot| self.function.layout.operand_for_tracked_index(slot).raw())
            .collect()
    }

    /// The values a frame state of the function being walked lists for the
    /// slots in `live`: those frame memory does not hold, or every one for
    /// virtual frames but their exception register. Inlined callees that are
    /// not strict list all their passed arguments too, since their legacy
    /// `arguments` shows them in the frames exits and slow paths materialize.
    fn frame_state_values(&self, live: &BitSet) -> Vec<(u32, NodeId)> {
        let is_virtual = self.function.is_virtual();
        // NB: Materialized frames start with an empty exception register, and only the runtime writes it: when a
        //     handler of the frame catches an exception, before a leave writes the rest of the frame.
        let exception_register = self
            .function
            .layout
            .tracked_index(Operand::from_raw(crate::bytecode::EXCEPTION_REGISTER));
        let mut values = live
            .iter()
            .filter(|slot| !is_virtual || Some(*slot) != exception_register)
            .filter_map(|slot| match self.frame.slots[slot] {
                SlotState::Value { node, in_sync } if is_virtual || !in_sync => {
                    Some((self.function.layout.operand_for_tracked_index(slot).raw(), node))
                }
                _ => None,
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
            if let SlotState::Value { node, .. } = self.frame.slots[slot] {
                values.push((layout.arguments_base() + argument, node));
            }
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
    /// call directly as a `CallDirect` node (or a `CallNative` node for raw
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
        if let Some(native) = feedback.native_call {
            self.reserve_call_frame(slots_offset + 8 * u64::from(*argument_count));
            self.embed(native.function);
            self.embed(native.realm);
            // NB: Native calls take the callee, the `this` value and the
            //     arguments as inputs like direct calls do.
            let mut inputs = vec![*callee, *this_value];
            inputs.extend(arguments.iter().copied());
            if let Some(inlined_frame_bytes) = self.inlined_frame_bytes_of_call(&inputs) {
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
        let SlotState::Value { node, .. } = self.peek(*callee) else {
            return Ok(None);
        };
        match self.graph.constant_value(node) {
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
        // NB: Direct calls take the callee, the `this` value if the callee
        //     reads it, and the arguments as inputs (see
        //     `codegen::DIRECT_CALL_MAX_INPUTS`), and store them straight
        //     into the callee's frame, except in inlined callees whose frames
        //     are written whole, and for the virtual arguments object.
        // NB: Calls without the frames of the inlined calls they are in
        //     always take `this`, for their slow paths.
        let mut inputs = vec![*callee, *this_value];
        inputs.extend(arguments.iter().copied());
        if let Some(inlined_frame_bytes) = self.inlined_frame_bytes_of_call(&inputs) {
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
        if self.function.has_legacy_arguments() {
            let layout = self.function.layout;
            let arguments_are_values = (0..layout.number_of_arguments).all(|argument| {
                let slot = (layout.registers_and_locals_count + argument) as usize;
                matches!(self.frame.slots[slot], SlotState::Value { .. })
            });
            if !arguments_are_values {
                return None;
            }
        }
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

    /// Inlines a `Call`, if its feedback saw a single callee that qualifies,
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
            // NB: Builtins that take callbacks record them as forwarded calls.
            let callback = feedback
                .forwarded
                .filter(|forwarded| forwarded.forwarding == Forwarding::Callback)
                .and_then(|forwarded| {
                    Some(CallbackTarget {
                        index: forwarded.inline_executable?,
                        closures: feedback.flags & call_feedback_flags::FORWARDED_CLOSURES != 0,
                    })
                });
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
        let SlotState::Value { node, .. } = self.peek(callee) else {
            return None;
        };
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
        let holders = self
            .frame
            .slots
            .iter()
            .filter(|slot| matches!(slot, SlotState::Value { node, .. } if *node == callee_phi))
            .count();
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
        //     for the destination, which the cases' values join in, and slots
        //     only frame memory held, which reading the operands in a case
        //     may load. The slow path keeps the destination in an SSA value,
        //     and touches no other slot.
        let Some(dst_slot) = self.function.layout.tracked_index(*dst) else {
            return Ok(None);
        };
        let operands = std::iter::once(*callee)
            .chain(std::iter::once(*this_value))
            .chain(arguments.iter().copied())
            .collect::<Vec<_>>();
        if self.is_reserved(*dst)
            || self.graph.legacy_arguments.contains(&dst.raw())
            || info.reads_whole_frame
            || crate::codegen::slow_path_call(OpCode::Call).is_none()
            || !super::slow_paths::is_ssa_slow_path_output(&self.function.layout, *dst, self.function.is_virtual())
            || operands.iter().any(|operand| self.holds_virtual_arguments(*operand))
        {
            return Ok(None);
        }
        let SlotState::Value {
            node: callee_phi,
            in_sync: callee_in_sync,
        } = self.peek(*callee)
        else {
            unreachable!("the callee is a phi");
        };
        let callee_slot = self
            .function
            .layout
            .tracked_index(*callee)
            .expect("the callee is in a slot");
        let edges = self.callee_case_edges(callee_phi, &cases);
        let callee_value = match edges {
            Some(_) => None,
            None => {
                for operand in &operands {
                    self.read(*operand)?;
                }
                Some(self.read(*callee)?)
            }
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
                    self.frame.slots[callee_slot] = SlotState::Value {
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
                let SlotState::Value { node, .. } = self.frame.slots[dst_slot] else {
                    unreachable!("the destination holds the call's value");
                };
                debug_assert!(
                    (0..frame.slots.len()).all(|slot| slot == dst_slot
                        || (slot == callee_slot && edges.is_some())
                        || frame.slots[slot] == self.frame.slots[slot]
                        || (frame.slots[slot] == SlotState::InMemory
                            && matches!(self.frame.slots[slot], SlotState::Value { in_sync: true, .. }))),
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
            self.frame.slots[callee_slot] = SlotState::Value {
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
        let SlotState::Value { node: closure, .. } = self.peek(*callee) else {
            return Ok(None);
        };
        let Op::AllocateFunction {
            shared_function_data_index,
        } = self.graph.node(closure).op
        else {
            return Ok(None);
        };
        let Some(index) = self.snapshot.executables[0]
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

    /// `this` of an inlined construct of `function`: the object its
    /// construct creates, an empty plain object of the prototype in its
    /// "prototype", which must still be the one it was at compile time.
    fn construct_this(&mut self, function: CellId, construct: &ConstructTarget) -> NodeId {
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
        self.emit(
            Op::AllocateObject {
                shape: construct.this_shape,
                property_count: 0,
                reserve: construct.reserve,
            },
            Vec::new(),
            Some(Repr::Tagged),
        )
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

    /// Whether calls of `callee_executable` may be inlined here at all: it is
    /// no executable being inlined already, the inlined frames are not as
    /// deep as they may be, and no inlined call exited here before.
    fn may_inline_calls_of(&self, callee_executable: &crate::snapshot::ExecutableSnapshot) -> bool {
        let recursive = self.function.executable.cell == callee_executable.cell
            || self
                .callers
                .iter()
                .any(|caller| caller.executable.cell == callee_executable.cell);
        !recursive
            && self.callers.len() < self.snapshot.options.inlining.max_depth as usize
            && self.may_speculate(ExitKind::BadCallTarget)
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
            closure,
            check_closure,
            callback,
        } = call;
        // NB: The callee's frame cannot hold the virtual arguments object:
        //     the callee's generic instructions could not read it.
        if std::iter::once(&this)
            .chain(arguments.iter())
            .any(|value| matches!(value, InlineValue::Operand(operand) if self.holds_virtual_arguments(*operand)))
        {
            return Ok(None);
        }
        let snapshot = self.snapshot;
        let Some(callee_executable) = snapshot.executables.get(index as usize) else {
            return Ok(None);
        };
        let Some(function) = callee_executable.function else {
            return Ok(None);
        };
        let limits = snapshot.options.inlining;
        if !self.may_inline_calls_of(callee_executable) {
            return Ok(None);
        }
        let Ok(decoded) = super::decoded_function(callee_executable, &self.decoded[index as usize]) else {
            return Ok(None);
        };
        let instruction_count = decoded.instructions.len();
        // NB: Builtins are always inlined (see the snapshot's inlining rules).
        let within_budget = callee_executable.builtin
            || crate::bytecode::inlining_size(&decoded.instructions) <= limits.always_inlined_instructions as usize
            || (construct.is_some() && instruction_count <= limits.max_construct_instructions as usize)
            || (instruction_count <= limits.max_instructions as usize
                && self.inlined_instructions + instruction_count <= limits.budget_instructions as usize);
        if !within_budget {
            return Ok(None);
        }
        // NB: CheckClosure checks shared data, not legacy-property eligibility.
        //     Keep non-strict closure calls conservative. Frames with a virtual
        //     arguments object keep every passed argument too, which exits
        //     create the object from.
        let virtual_arguments =
            self.virtualize_arguments.compiled_function && self.virtualize_arguments.inlined_callees;
        let creates_arguments = decoded
            .instructions
            .iter()
            .any(|instruction| instruction.instruction.opcode() == OpCode::CreateArguments);
        let has_legacy_arguments = callee_executable.supports_legacy_arguments
            || (closure.is_some() && callee_executable.is_non_strict_function())
            || (virtual_arguments && creates_arguments);
        let Ok(mut callee_function) = Function::new(index, callee_executable, decoded, has_legacy_arguments) else {
            return Ok(None);
        };
        if !can_inline(&callee_function, virtual_arguments) {
            return Ok(None);
        }
        if construct.is_some() && !returns_no_object(&callee_function) {
            return Ok(None);
        }

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
        let receiver = match &construct {
            Some(construct) => self.construct_this(function.function, construct),
            None => receiver,
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
            SlotState::Value {
                node: empty,
                in_sync: true,
            };
            layout.tracked_slot_count()
        ];
        slots[crate::bytecode::THIS_VALUE_REGISTER as usize] = SlotState::Value {
            node: receiver,
            in_sync: true,
        };
        for argument in 0..layout.number_of_arguments {
            let slot = layout
                .tracked_index(Operand::from_raw(layout.arguments_base() + argument))
                .expect("arguments are tracked");
            let node = argument_values.get(argument as usize).copied().unwrap_or(undefined);
            slots[slot] = SlotState::Value { node, in_sync: true };
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
    pub(super) fn build_inline_return(&mut self, value: NodeId, empty_is_undefined: bool) -> Flow {
        let construct_this = self
            .function
            .inline
            .as_ref()
            .and_then(|call| call.construct.then_some(call.this));
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
        Flow::Ended
    }
}
