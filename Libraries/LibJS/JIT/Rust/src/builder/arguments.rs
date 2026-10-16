/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The arguments object as a virtual object.
//!
//! `CreateArguments` copies the frame's passed arguments into a new object.
//! When the function never writes its argument slots (and nothing writes the
//! parameter bindings a mapped arguments object aliases), the frame and the
//! bindings keep those values, so the
//! compiled function's own `CreateArguments` creates nothing: its slot holds
//! a stand-in constant (`value::VIRTUAL_*_ARGUMENTS`) that only frame states
//! refer to. Uses of it read the frame instead: `arguments.length` is the
//! passed argument count, `arguments[i]` an argument slot (exiting for any
//! index that is not an int32 within the passed arguments), and
//! `f.apply(this_arg, arguments)` a call of `f` with the frame's arguments.
//!
//! The object is created where something else could see it: exits (the
//! runtime creates it for slots whose exit value is the stand-in), and
//! generic nodes that leave the compiled code (codegen creates it for the
//! stand-in slots of their frame state before the interpreter continues).
//! Any other use of the stand-in, as the input of a node, means that the
//! graph cannot do without the object, and the builder starts over without
//! virtual arguments objects.
//!
//! Inlined callees keep their arguments objects virtual the same way, with
//! the values their call passed: `arguments.length` is the passed argument
//! count, `arguments[i]` the passed value for a constant `i`, and
//! `f.apply(this_arg, arguments)` an inlined call with those values. Their
//! frame states list every passed argument, so that exits create the object
//! from the frames they materialize. Anything else that needs the object
//! (including returning it, or a use that fails the build) has the graph
//! built again without virtual arguments objects in inlined callees, which
//! then are not inlined.
//!
//! On-stack replacement entries load every slot from the frame, where the
//! interpreter's arguments object is, so the stand-in meets that object at
//! loop headers. A graph that only uses the object there is built again
//! without the entries: the frame already looping stays in the interpreter,
//! and every later call runs without an arguments object.

use super::GraphBuilder;
use super::inlining::InlineValue;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::MAPPED_ARGUMENTS;
use crate::bytecode::OpCode;
use crate::bytecode::Operand;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use crate::snapshot::Intrinsic;

/// Whether an instruction with `opcode` may write a binding of the function's
/// environment (where a mapped arguments object's parameters live), or create
/// a closure, which may.
pub(super) fn may_write_parameter_bindings(opcode: OpCode) -> bool {
    use OpCode as O;
    matches!(
        opcode,
        O::NewFunction
            | O::NewClass
            | O::InitializeVariableBinding
            | O::DynamicInitializeLexicalBinding
            | O::DynamicInitializeVariableBinding
            | O::SetLexicalBinding
            | O::SetVariableBinding
            | O::DynamicSetLexicalBinding
            | O::DynamicSetVariableBinding
            | O::SetResolvedBinding
            | O::DeleteVariable
            | O::EnterObjectEnvironment
            | O::CreateVariableEnvironment
            | O::CallDirectEval
            | O::CallDirectEvalWithArgumentArray
    )
}

/// Whether `instruction` of `function` is a `CreateArguments` that can build a
/// virtual arguments object, as far as the function itself can tell.
pub(super) fn can_create_virtual_arguments(function: &super::Function<'_>, instruction: &Instruction) -> bool {
    let Instruction::CreateArguments {
        dst,
        kind,
        creates_parameter_bindings,
        ..
    } = *instruction
    else {
        return false;
    };
    // NB: Creating the parameter bindings is a side effect that a virtual arguments object would skip.
    dst.is_some()
        && !creates_parameter_bindings
        && !function.writes_arguments
        && !(kind == MAPPED_ARGUMENTS
            && function.executable.mapped_arguments_alias_parameters
            && function.may_write_parameter_bindings)
}

/// Whether a node of `graph` takes a virtual arguments object as an input,
/// which only a real object can be.
pub(super) fn uses_virtual_arguments(graph: &Graph) -> bool {
    graph.nodes.iter().any(|node| {
        node.inputs.iter().any(|input| {
            graph
                .constant_value(*input)
                .and_then(value::virtual_arguments_kind)
                .is_some()
        })
    })
}

impl GraphBuilder<'_> {
    /// Whether `node` is the virtual arguments object.
    pub(super) fn is_virtual_arguments(&self, node: NodeId) -> bool {
        self.graph
            .constant_value(node)
            .and_then(value::virtual_arguments_kind)
            .is_some()
    }

    /// Whether `operand` holds the virtual arguments object, without
    /// building anything to read it.
    pub(super) fn holds_virtual_arguments(&self, operand: Operand) -> bool {
        self.function
            .layout
            .tracked_index(operand)
            .is_some_and(|slot| matches!(self.frame.slots[slot], super::SlotState::Value { node, .. } if self.is_virtual_arguments(node)))
    }

    /// Builds `CreateArguments` as a virtual arguments object, if it can be
    /// one. Returns false, having built nothing, otherwise.
    pub(super) fn try_build_virtual_arguments(&mut self, instruction: &Instruction) -> Result<bool, CompileFailure> {
        let Instruction::CreateArguments {
            dst: Some(dst), kind, ..
        } = *instruction
        else {
            return Ok(false);
        };
        let virtualization = self.virtualize_arguments;
        let can_virtualize = virtualization.compiled_function
            && (!self.function.is_virtual() || virtualization.inlined_callees)
            && can_create_virtual_arguments(&self.function, instruction);
        if !can_virtualize {
            return Ok(false);
        }
        if self.function.is_virtual() {
            virtualization.inlined_callees_virtualized.set(true);
        }
        let bits = if kind == MAPPED_ARGUMENTS {
            value::VIRTUAL_MAPPED_ARGUMENTS
        } else {
            value::VIRTUAL_UNMAPPED_ARGUMENTS
        };
        let arguments = self.constant(bits);
        self.write(dst, arguments)?;
        Ok(true)
    }

    /// The value the call of the inlined callee being built passed it as
    /// argument `argument`, if it passed one, or `None` outside inlined
    /// callees.
    fn inlined_call_argument(&self, argument: u32) -> Option<NodeId> {
        let call = self.function.inline.as_ref()?;
        if argument >= call.passed_argument_count {
            return None;
        }
        let layout = self.function.layout;
        if argument >= layout.number_of_arguments {
            return Some(call.untracked_arguments[(argument - layout.number_of_arguments) as usize]);
        }
        let slot = layout
            .tracked_index(Operand::from_raw(layout.arguments_base() + argument))
            .expect("arguments are tracked");
        match self.frame.slots[slot] {
            super::SlotState::Value { node, .. } => Some(node),
            super::SlotState::InMemory => unreachable!("inlined callees hold their slots as values"),
        }
    }

    /// The values the call of the inlined callee being built passed it, all
    /// of them, as arguments of a call it inlines, and their count; `None`
    /// outside inlined callees.
    pub(super) fn inlined_call_arguments(&self) -> Option<(Vec<InlineValue>, u32)> {
        let count = self.function.inline.as_ref()?.passed_argument_count;
        let values = (0..count)
            .map(|argument| {
                let node = self.inlined_call_argument(argument).expect("the call passed it");
                InlineValue::Node(node)
            })
            .collect();
        Some((values, count))
    }

    /// Fails the build where an inlined callee returns its virtual arguments
    /// object, which would leave it to its caller's frame.
    pub(super) fn check_not_returning_virtual_arguments(&self, value: NodeId) -> Result<(), CompileFailure> {
        if self.function.is_virtual() && self.is_virtual_arguments(value) {
            return Err(CompileFailure::UnsupportedInstruction {
                pc: self.function.pc,
                opcode: OpCode::Return,
                reason: "returns the arguments object of an inlined callee",
            });
        }
        Ok(())
    }

    /// Builds `dst = base.length` for a virtual arguments object. Returns
    /// false, having built nothing, for other bases.
    pub(super) fn try_build_arguments_length(&mut self, dst: Operand, base: Operand) -> Result<bool, CompileFailure> {
        if !self.holds_virtual_arguments(base) {
            return Ok(false);
        }
        if let Some(call) = &self.function.inline {
            let count = self.constant(value::int32(call.passed_argument_count.cast_signed()));
            self.write(dst, count)?;
            return Ok(true);
        }
        let count = self.emit(Op::ArgumentCount, Vec::new(), Some(Repr::Tagged));
        self.write(dst, count)?;
        Ok(true)
    }

    /// Builds `dst = base[property]` for a virtual arguments object. Returns
    /// false, having built nothing, for other bases, or if reading the frame
    /// already failed here.
    pub(super) fn try_build_arguments_index(
        &mut self,
        dst: Operand,
        base: Operand,
        property: Operand,
    ) -> Result<bool, CompileFailure> {
        if !self.holds_virtual_arguments(base) {
            return Ok(false);
        }
        // NB: Inlined callees read constant indices of what their call passed.
        if self.function.is_virtual() {
            let index = self.read(property)?;
            let Some(value) = self
                .graph
                .constant_value(index)
                .and_then(value::as_int32)
                .and_then(|index| u32::try_from(index).ok())
                .and_then(|index| self.inlined_call_argument(index))
            else {
                return Ok(false);
            };
            self.write(dst, value)?;
            return Ok(true);
        }
        if !self.may_speculate(ExitKind::ArgumentsIndex) {
            return Ok(false);
        }
        let index = self.read(property)?;
        let arguments_base = self.function.layout.arguments_base();
        let argument = self.emit_checked(Op::LoadArgument { arguments_base }, vec![index], Some(Repr::Tagged));
        self.write(dst, argument)?;
        Ok(true)
    }

    /// Builds `slice.call(arguments)` and `slice.call(arguments, start)` with
    /// a virtual arguments object, where `slice` is `Array.prototype.slice`,
    /// as a `SliceArguments`. Returns `None`, having built nothing, for
    /// anything else.
    pub(super) fn try_build_arguments_slice(
        &mut self,
        instruction: &Instruction,
    ) -> Result<Option<super::Flow>, CompileFailure> {
        let Instruction::Call {
            call_feedback,
            dst,
            callee,
            this_value,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        let (forwarded, start) = match arguments.as_slice() {
            [forwarded] => (*forwarded, None),
            [forwarded, start] => (*forwarded, Some(*start)),
            _ => return Ok(None),
        };
        if !self.holds_virtual_arguments(forwarded)
            || self.function.is_virtual()
            || !self.may_speculate(ExitKind::ArgumentsIndex)
        {
            return Ok(None);
        }
        if self.constant_intrinsic(*callee) != Some(Intrinsic::FunctionPrototypeCall) {
            return Ok(None);
        }
        // NB: The function `call` calls: a constant `slice`, or the `slice` the
        //     call always forwarded to, checked.
        let slice_check = if self.constant_intrinsic(*this_value) == Some(Intrinsic::ArrayPrototypeSlice) {
            None
        } else {
            let forwarded_slice = self
                .call_feedback(*call_feedback)
                .and_then(|feedback| feedback.forwarded)
                .filter(|forwarded| forwarded.target_intrinsic == Some(Intrinsic::ArrayPrototypeSlice));
            let Some(forwarded_slice) = forwarded_slice else {
                return Ok(None);
            };
            if !self.may_speculate(ExitKind::BadCallTarget) {
                return Ok(None);
            }
            Some(forwarded_slice.target)
        };
        let start = match start {
            Some(start) => self.read(start)?,
            None => self.constant(value::int32(0)),
        };
        if let Some(slice) = slice_check {
            self.check_callee(*this_value, slice)?;
        }
        let array = self.emit_checked(Op::SliceArguments, vec![start], Some(Repr::Tagged));
        self.write(*dst, array)?;
        Ok(Some(super::Flow::Continue))
    }

    /// The last argument of a `Call` that forwards a virtual arguments object
    /// through `Function.prototype.apply`, if it is one.
    pub(super) fn forwarded_arguments(&self, instruction: &Instruction) -> Option<Operand> {
        let Instruction::Call { callee, arguments, .. } = instruction else {
            return None;
        };
        let [_, forwarded] = arguments.as_slice() else {
            return None;
        };
        // NB: CallForwardingArguments reads the running frame, which is not an
        //     inlined callee's.
        if !self.holds_virtual_arguments(*forwarded) || self.function.is_virtual() {
            return None;
        }
        (self.constant_intrinsic(*callee) == Some(Intrinsic::FunctionPrototypeApply)).then_some(*forwarded)
    }

    /// Builds `g(...arguments)` with a virtual arguments object: the
    /// instructions `NewArray` (the one at hand, of `array`), `ArrayAppend`
    /// of a spread of the arguments object, and `CallWithArgumentArray` with
    /// the array, which nothing else reads, as one call with the frame's
    /// arguments (unless iterating them would be observable, which the
    /// runtime checks). Returns `None`, having built nothing, for anything
    /// else.
    pub(super) fn try_build_spread_forwarding(
        &mut self,
        array: Operand,
    ) -> Result<Option<super::Flow>, CompileFailure> {
        let index = self.function.instruction_index;
        let (Some(append), Some(call)) = (
            self.function.instructions.get(index + 1),
            self.function.instructions.get(index + 2),
        ) else {
            return Ok(None);
        };
        let Instruction::ArrayAppend {
            dst,
            src,
            is_spread: true,
        } = append.instruction
        else {
            return Ok(None);
        };
        let Instruction::CallWithArgumentArray { arguments, .. } = call.instruction else {
            return Ok(None);
        };
        let call_pc = call.pc;
        let call_instruction = call.instruction.clone();
        let block = self.function.cfg.block_for_pc(self.function.pc);
        let in_one_block = block.is_some()
            && block == self.function.cfg.block_for_pc(append.pc)
            && block == self.function.cfg.block_for_pc(call_pc);
        let array_dies = self
            .function
            .layout
            .tracked_index(array)
            .is_some_and(|slot| !self.function.liveness.live_out(index + 2).contains(slot));
        if dst != array
            || self.function.is_virtual()
            || arguments != array
            || !in_one_block
            || !array_dies
            || !self.holds_virtual_arguments(src)
            || !self.instruction_has_run(call_pc)
        {
            return Ok(None);
        }
        let super::Handling::Generic(info) = super::handling(OpCode::CallWithArgumentArray) else {
            unreachable!("calls are generic instructions");
        };
        self.function.instruction_index = index + 2;
        self.function.pc = call_pc;
        self.function.eager_frame_state = None;
        self.function.skipped_instructions = 2;
        let op = Op::CallForwardingArguments {
            executable: self.function.index,
            pc: call_pc,
        };
        self.build_generic_node(&call_instruction, info, op).map(Some)
    }

    /// Builds `f.apply(this_arg, arguments)` with a virtual arguments object.
    pub(super) fn build_forwarding_call(
        &mut self,
        instruction: &Instruction,
        forwarded: Operand,
    ) -> Result<super::Flow, CompileFailure> {
        let super::Handling::Generic(info) = super::handling(OpCode::Call) else {
            unreachable!("calls are generic instructions");
        };
        let op = Op::CallForwardingArguments {
            executable: self.function.index,
            pc: self.function.pc,
        };
        self.build_generic_node_with_inputs(instruction, info, op, &[], Some(forwarded))
    }
}
