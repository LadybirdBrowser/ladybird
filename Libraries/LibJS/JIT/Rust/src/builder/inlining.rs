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
//! without them (see `Op::CallDirect`).
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
    /// For callees that are not strict: the values the call passes beyond
    /// the arguments the callee's bytecode reads (see
    /// `legacy_argument_values()`).
    pub untracked_arguments: Vec<NodeId>,
    /// Where returns continue, once the first one is built.
    pub continuation: Option<BlockId>,
    /// The values returned, one per predecessor of the continuation, with
    /// what was known at each return.
    pub returns: Vec<NodeId>,
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

/// Whether the callee can be built as an inlined call: its frame must not be
/// needed in memory outside generic nodes, its environments must stay those
/// of its function object, it must be fully compilable, and it must not step
/// for-in loops.
fn can_inline(callee: &Function<'_>) -> bool {
    if !callee.written_constants.is_empty() {
        return false;
    }
    callee.instructions.iter().all(|instruction| {
        let opcode = instruction.instruction.opcode();
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
        // NB: Building for-in loops leaves slots in frame memory, which
        //     inlined frames do not have.
        let steps_for_in = opcode == OpCode::ObjectPropertyIteratorNext;
        !reads_frame_fields && !steps_for_in && !matches!(handling(opcode), Handling::Unsupported(_))
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
enum InlineValue {
    Operand(Operand),
    Constant(u64),
    /// The running frame's argument at this index, whose arguments object
    /// the code never created.
    FrameArgument(u32),
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
        }
    }

    /// The frame state after the instruction being built, which wrote
    /// `dst` (`Operand::INVALID` if nothing), for the slots live after it.
    pub(super) fn resume_after_frame_state(&mut self, dst: u32) -> FrameStateId {
        let live = self.function.liveness.live_out(self.function.instruction_index);
        let frame_state = self.new_frame_state(live, ResumeMode::ResumeAfter { dst });
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
    /// virtual frames. Inlined callees that are not strict list all their
    /// passed arguments too, since their legacy `arguments` shows them in
    /// the frames exits and slow paths materialize.
    fn frame_state_values(&self, live: &BitSet) -> Vec<(u32, NodeId)> {
        let is_virtual = self.function.is_virtual();
        let mut values = live
            .iter()
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

    /// For an inlined callee being walked that is not strict: the values of
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

    /// The parent of frame states of the function being walked.
    pub(super) fn parent_frame_state(&self) -> Option<FrameStateId> {
        self.function.inline.as_ref().map(|call| call.caller_frame_state)
    }

    /// Makes compiled code check at entry that the interpreter stack has room
    /// for a frame of `bytes` that a call pushes on top of the frames of the
    /// inlined calls it is in, so the call need not check.
    fn reserve_call_frame(&mut self, bytes: u64) {
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
            return Ok(None);
        };
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
                    forwarded: true,
                    ..InlinedCallSite::new(
                        index,
                        arguments
                            .first()
                            .map_or(InlineValue::Constant(value::UNDEFINED), |this| {
                                InlineValue::Operand(*this)
                            }),
                        arguments.iter().skip(1).copied().map(InlineValue::Operand).collect(),
                        argument_count.saturating_sub(1),
                        *dst,
                    )
                }
            }
            Forwarding::Apply => {
                let [this, forwarded_arguments] = arguments.as_slice() else {
                    return Ok(None);
                };
                if self.constant_intrinsic(*callee) != Some(Intrinsic::FunctionPrototypeApply)
                    || !self.holds_virtual_arguments(*forwarded_arguments)
                    || !self.may_speculate(ExitKind::UnexpectedValue)
                {
                    return Ok(None);
                }
                InlinedCallSite {
                    callee: Some(*this_value),
                    forwarded: true,
                    ..InlinedCallSite::new(
                        index,
                        InlineValue::Operand(*this),
                        (0..forwarded.argument_count).map(InlineValue::FrameArgument).collect(),
                        forwarded.argument_count,
                        *dst,
                    )
                }
            }
            Forwarding::Bound => {
                let Some(bound_function) = feedback.target else {
                    return Ok(None);
                };
                let bound_arguments = &forwarded.bound_arguments[..usize::from(forwarded.bound_argument_count)];
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
                        u32::from(forwarded.bound_argument_count) + argument_count,
                        *dst,
                    )
                }
            }
        };
        self.inline_call(call)
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
            InlineValue::FrameArgument(index) => {
                let index = self.constant(value::int32(index.cast_signed()));
                let arguments_base = self.function.layout.arguments_base();
                Ok(self.emit_checked(Op::LoadArgument { arguments_base }, vec![index], Some(Repr::Tagged)))
            }
        }
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
        let recursive = self.function.executable.cell == callee_executable.cell
            || self
                .callers
                .iter()
                .any(|caller| caller.executable.cell == callee_executable.cell);
        let limits = snapshot.options.inlining;
        if recursive
            || self.callers.len() + 1 > limits.max_depth as usize
            || !self.may_speculate(ExitKind::BadCallTarget)
        {
            return Ok(None);
        }
        let Ok(mut callee_function) = Function::new(index, callee_executable) else {
            return Ok(None);
        };
        let instruction_count = callee_function.instructions.len();
        // NB: Builtins are always inlined (see the snapshot's inlining rules).
        let within_budget = callee_executable.builtin
            || instruction_count <= limits.always_inlined_instructions as usize
            || (construct.is_some() && instruction_count <= limits.max_construct_instructions as usize)
            || (instruction_count <= limits.max_instructions as usize
                && self.inlined_instructions + instruction_count <= limits.budget_instructions as usize);
        if !within_budget || !can_inline(&callee_function) {
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
        let mut caller_live = self.function.liveness.live_out(self.function.instruction_index).clone();
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
        };
        callee_function.inline = Some(InlineCall {
            caller_frame_state,
            caller_frame: self.frame.clone(),
            closure,
            callback,
            this: receiver,
            passed_argument_count: *argument_count,
            untracked_arguments: if callee_function.is_non_strict_function() {
                argument_values
                    .get(layout.number_of_arguments as usize..)
                    .unwrap_or_default()
                    .to_vec()
            } else {
                Vec::new()
            },
            continuation: None,
            returns: Vec::new(),
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
        self.function
            .inline
            .as_mut()
            .expect("returns from inlined callees")
            .returns
            .push(value);
        self.set_control(Op::Jump { target: continuation }, Vec::new());
        self.graph.blocks[continuation.index()].predecessors.push(self.block);
        Flow::Ended
    }
}
