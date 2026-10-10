/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Direct calls (`Op::CallDirect`): JIT code calls the single callee a
//! `Call` instruction saw the way the interpreter's `Call` fast path does.
//! It builds the callee's frame on the interpreter stack with plain stores,
//! links it to the calling frame (marked as returning to a native caller)
//! and enters the callee's JIT code, loading its entry at call time since
//! the callee may be compiled (or discarded) at any time. When the callee
//! returns, JIT code pops its frame with the bookkeeping of the interpreter's
//! `Return`. Callees without JIT code, and callees whose JIT code exits to
//! the interpreter, are run to completion by `RuntimeInfo::finish_direct_call`.
//!
//! Dynamic calls: calls of other callees, and generic `Call` nodes, call any
//! ECMAScript function the interpreter's `Call` fast path could call the same
//! way, with the frame layout read from the function object at call time
//! (`RuntimeInfo::dynamic_calls`). Calls the fast path cannot make take the
//! generic path through `RuntimeInfo::jit_call`.
//!
//! Native calls (`Op::CallNative`): JIT code calls the single raw native
//! function a `Call` instruction saw in the lightweight frame the
//! interpreter's `Call` fast path builds for raw native functions, and lets
//! the runtime unwind the frame if the function throws.

use super::Codegen;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Fpr;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::bytecode::THIS_VALUE_REGISTER;
use crate::code::INLINED_CALL_SITE_BIT;
use crate::code::JitStatus;
use crate::code::SiteKind;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use crate::snapshot::CellId;
use crate::snapshot::DirectCallTarget;
use crate::snapshot::EnvironmentTemplateSnapshot;
use crate::snapshot::InlinedFunctionSnapshot;
use crate::snapshot::NativeCallTarget;
use crate::snapshot::RuntimeOffsets;

/// A field JIT code stores a constant into: its offset, its size in bytes
/// and its value (in the low `size` bytes).
pub(super) type ConstantField = (u32, u32, u64);

/// The most inputs a `CallDirect` or `CallNative` node takes in registers:
/// the callee, the `this` value and the arguments. Calls with more operands
/// take them anywhere and store them to the frame (see `Op::CallDirect`).
pub const DIRECT_CALL_MAX_INPUTS: usize = 6;

/// The temps of a `CallDirect` or `CallNative` node with inputs.
pub const DIRECT_CALL_TEMPS: u8 = 3;

/// Where a call finds one of its operands.
#[derive(Debug, Clone, Copy)]
enum CallOperand {
    /// The frame slot of a bytecode operand.
    Slot(crate::bytecode::Operand),
    /// A register: an input of the call's node, or a value loaded already.
    Register(Gpr),
    Undefined,
}

/// What `emit_js_call()` worked out about a `Call` instruction before
/// emitting it.
struct JsCall {
    executable: u32,
    pc: u32,
    next_pc: u32,
    dst: Operand,
    callee: Operand,
    this_value: Operand,
    arguments: Vec<Operand>,
    /// The registers of the operands, for a `CallDirect` node that has them
    /// as inputs.
    input_registers: Option<Vec<Gpr>>,
    site: Option<InlinedCallSite>,
    callee_operand: CallOperand,
    this_operand: CallOperand,
    argument_operands: Vec<CallOperand>,
    /// The registers a direct call builds the callee's frame with.
    callee_frame: Gpr,
    temp: Gpr,
    aux: Gpr,
    argument_count: u32,
    /// The generic call through the runtime's call helper.
    slow: Label,
    /// Where every call checks how to continue.
    check: Label,
    done: Label,
    /// The call through the call stub, or `slow` without one.
    dynamic_call: Label,
}

/// The header of a new interpreter frame.
struct FrameHeader {
    function: InlinedFunctionSnapshot,
    executable: CellId,
    passed_argument_count: u32,
    /// The number of argument slots: at least the formal parameter count.
    argument_count: u32,
    slot_count: u32,
    return_pc: u32,
    dst: u32,
    /// Set for frames whose caller waits for them in native code.
    returns_to_native_caller: bool,
}

/// A direct or native call in an inlined callee that runs without the frames
/// of the inlined calls it is in (see `Op::CallDirect`).
struct InlinedCallSite {
    /// The index of its `SiteKind::Call` site.
    index: u32,
    /// The pc of the call of the compiled function the call is inlined in.
    outer_pc: u32,
    /// The room for the frames of the inlined calls below the callee's frame.
    frame_bytes: u32,
    /// How many inlined calls the call is in.
    depth: usize,
}

impl InlinedCallSite {
    /// What the callee's frame has as its return pc: the site (see
    /// `INLINED_CALL_SITE_BIT`).
    fn return_pc(&self) -> u32 {
        INLINED_CALL_SITE_BIT | self.index
    }
}

/// The fields of a new interpreter frame that do not depend on its function.
struct FrameHeaderConstants {
    passed_argument_count: u32,
    return_pc: u32,
    dst: u32,
    returns_to_native_caller: bool,
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    fn boxed_object(&self, cell: CellId) -> u64 {
        (u64::from(value::OBJECT_TAG) << 48) | (cell.0 & self.runtime.heap_region_offset_mask)
    }

    /// Its inputs in registers are where `registers` says.
    pub(super) fn emit_call_direct(
        &mut self,
        node: NodeId,
        executable: u32,
        pc: u32,
        registers: &super::InputRegisters,
    ) -> Result<(), CompileFailure> {
        let invalid = |reason| CompileFailure::InvalidBytecode { pc, reason };
        let snapshot = &self.executables[executable as usize];
        let decoded = self.decoded(executable, pc)?;
        let Instruction::Call { call_feedback, .. } = decoded.instruction else {
            return Err(invalid("direct call of a non-Call instruction"));
        };
        let target: DirectCallTarget = snapshot
            .feedback
            .call
            .get(usize::from(call_feedback))
            .and_then(|feedback| feedback.direct_call)
            .ok_or(invalid("direct call without a target"))?;
        if self.stores_operands(node) {
            let operands = crate::builder::js_call_operands(&decoded.instruction).inputs;
            self.emit_store_operand_inputs(node, &operands, registers)?;
        }
        self.emit_js_call(executable, pc, Some((target, node)), None)
    }

    /// Whether the call `node` takes all its operands as inputs anywhere and
    /// stores them to their frame slots (see `Op::CallDirect`).
    fn stores_operands(&self, node: NodeId) -> bool {
        matches!(
            self.graph.node(node).op,
            Op::CallDirect {
                stores_operands: true,
                ..
            } | Op::CallNative {
                stores_operands: true,
                ..
            }
        )
    }

    /// Whether the callee input of the direct or native call `node` (if it
    /// has inputs) is the constant `function`, as after the check of a
    /// global function's value, which makes checking it again unnecessary.
    fn callee_is_target_constant(&self, node: Option<NodeId>, function: CellId) -> bool {
        let Some(node) = node else {
            return false;
        };
        let inputs = &self.graph.node(node).inputs;
        !self.stores_operands(node)
            && inputs
                .first()
                .is_some_and(|callee| self.graph.node(*callee).op == Op::Constant(self.boxed_object(function)))
    }

    /// After a call that left its result in its destination slot (not one
    /// in an inlined callee without the frames of its inlined calls, which
    /// has it in the return register already): loads it into the node's
    /// output.
    fn emit_load_call_result(
        &mut self,
        node: NodeId,
        dst: crate::bytecode::Operand,
        site: Option<&InlinedCallSite>,
    ) -> Result<(), CompileFailure> {
        if site.is_some() || self.graph.node(node).repr.is_none() {
            return Ok(());
        }
        let address = self.slot_address(dst.raw())?;
        self.masm.load64(M::RETURN_GPRS[0], &address);
        Ok(())
    }

    /// A generic `Call`: through the dynamic call path, calling the `this`
    /// value directly if the site's single callee is `Function.prototype.call`.
    pub(super) fn emit_generic_js_call(&mut self, executable: u32, pc: u32) -> Result<(), CompileFailure> {
        let snapshot = &self.executables[executable as usize];
        let call_function = match self.decoded(executable, pc) {
            Ok(decoded) => match decoded.instruction {
                Instruction::Call { call_feedback, .. } => snapshot
                    .feedback
                    .call
                    .get(usize::from(call_feedback))
                    .and_then(|feedback| feedback.function_prototype_call_target()),
                _ => None,
            },
            Err(_) => None,
        };
        self.emit_js_call(executable, pc, None, call_function)
    }

    /// Whether generic `Call` nodes try to call ECMAScript functions directly
    /// before taking the generic path.
    pub(super) fn makes_dynamic_calls(&self) -> bool {
        self.runtime.dynamic_calls.call_stub != 0 && self.runtime.jit_call != 0
    }

    /// Emits the `Call` instruction at `pc`: a direct call of `target` (made
    /// by the `CallDirect` node it comes with) if the callee is it, otherwise
    /// (if the runtime allows it) a direct call of any ECMAScript function
    /// the interpreter's `Call` fast path could call, with the frame layout
    /// read from the function at call time, and the generic path through
    /// `RuntimeInfo::jit_call` for everything else.
    ///
    /// A `CallDirect` node with inputs has the callee, the `this` value if
    /// the target reads it, and the arguments in them (see
    /// `DIRECT_CALL_MAX_INPUTS`); other calls read their operands from the
    /// frame.
    ///
    /// With `call_function` (`Function.prototype.call`, which the code must
    /// embed), the call is made like `Function.prototype.call` does if the
    /// callee is it: the `this` value is called with the first argument as
    /// its `this` and the other arguments.
    pub(super) fn emit_js_call(
        &mut self,
        executable: u32,
        pc: u32,
        target: Option<(DirectCallTarget, NodeId)>,
        call_function: Option<CellId>,
    ) -> Result<(), CompileFailure> {
        let invalid = |reason| CompileFailure::InvalidBytecode { pc, reason };
        let decoded = self.decoded(executable, pc)?;
        let next_pc = decoded.next_pc();
        let Instruction::Call {
            dst,
            callee,
            this_value,
            arguments,
            ..
        } = decoded.instruction
        else {
            return Err(invalid("direct call of a non-Call instruction"));
        };
        let runtime = self.runtime;
        if call_function.is_some() && (target.is_some() || runtime.dynamic_calls.call_stub == 0) {
            return Err(invalid("unsupported Function.prototype.call call"));
        }
        let registers = M::ARGUMENT_GPRS;

        // Where the call's operands are, and the registers the direct call
        // builds the callee's frame with: the node's temps if its operands
        // are inputs, which may be in any register.
        let input_registers = target
            .filter(|(_, node)| !self.stores_operands(*node))
            .map(|(_, node)| {
                (0..self.graph.node(node).inputs.len())
                    .map(|index| self.input(node, index))
                    .collect::<Vec<_>>()
            })
            .filter(|inputs| !inputs.is_empty());
        let site = match target {
            Some((_, node)) => self.inlined_call_site(node),
            None => None,
        };
        let (callee_operand, this_operand, argument_operands, [callee_frame, temp, aux]) =
            match (&input_registers, target) {
                (Some(inputs), Some((_, node))) => {
                    // NB: Calls take `this` even where their target does not
                    //     read it, for their slow paths.
                    let this_operand = CallOperand::Register(inputs[1]);
                    let argument_operands = inputs[2..]
                        .iter()
                        .map(|register| CallOperand::Register(*register))
                        .collect::<Vec<_>>();
                    if argument_operands.len() != arguments.len() {
                        return Err(invalid("direct call with the wrong number of inputs"));
                    }
                    let temps = [self.temp(node, 0), self.temp(node, 1), self.temp(node, 2)];
                    (CallOperand::Register(inputs[0]), this_operand, argument_operands, temps)
                }
                _ => {
                    // What the called function gets: the `this` value and the
                    // arguments, which `Function.prototype.call` takes from its
                    // own arguments.
                    let (this_operand, argument_operands) = match call_function {
                        Some(_) => (
                            arguments
                                .first()
                                .map_or(CallOperand::Undefined, |operand| CallOperand::Slot(*operand)),
                            arguments
                                .iter()
                                .skip(1)
                                .map(|operand| CallOperand::Slot(*operand))
                                .collect(),
                        ),
                        None => (
                            CallOperand::Slot(this_value),
                            arguments.iter().map(|operand| CallOperand::Slot(*operand)).collect(),
                        ),
                    };
                    // NB: The callee is loaded into the second register.
                    (
                        CallOperand::Register(registers[2]),
                        this_operand,
                        argument_operands,
                        [registers[1], registers[2], registers[4]],
                    )
                }
            };
        let argument_count = u32::try_from(argument_operands.len()).map_err(|_| invalid("too many arguments"))?;
        let slow = self.masm.new_label();
        let dynamic_call = if runtime.dynamic_calls.call_stub != 0 {
            self.masm.new_label()
        } else {
            slow
        };
        let call = JsCall {
            executable,
            pc,
            next_pc,
            dst,
            callee,
            this_value,
            arguments,
            input_registers,
            site,
            callee_operand,
            this_operand,
            argument_operands,
            callee_frame,
            temp,
            aux,
            argument_count,
            slow,
            check: self.masm.new_label(),
            done: self.masm.new_label(),
            dynamic_call,
        };

        // Stack traces show the caller at its call, which is the call of the
        // compiled function that the call is inlined in, if it is.
        let program_counter = self.frame_field(runtime.offsets.execution_context_program_counter);
        self.masm
            .store_imm32(&program_counter, call.site.as_ref().map_or(pc, |site| site.outer_pc));

        if call.input_registers.is_none() {
            let callee_address = self.slot_address(callee.raw())?;
            self.masm.load64(temp, &callee_address);
            if let Some(call_function) = call_function {
                self.masm
                    .branch64_imm(Condition::NotEqual, temp, self.boxed_object(call_function) as i64, slow);
                let function_address = self.slot_address(this_value.raw())?;
                self.masm.load64(temp, &function_address);
            }
        }

        let Some((target, target_node)) = target else {
            return self.emit_js_call_fallbacks(&call, None);
        };
        self.emit_direct_call_frame(&call, target, target_node)?;
        let returned = self.emit_direct_call_entry(&call, target, target_node)?;
        self.emit_js_call_fallbacks(&call, Some(target_node))?;
        self.masm.bind(returned);
        Ok(())
    }

    /// Calls `function(VM*, ExecutionContext* frame, u32 value)` of the
    /// runtime with the running frame, like the runtime's call helper and
    /// the slow paths that run a bytecode instruction at the pc `value`.
    pub(super) fn emit_frame_runtime_call(&mut self, function: u64, value: u32) {
        let arguments = M::ARGUMENT_GPRS;
        self.masm.move64(arguments[0], self.pinned.vm);
        self.masm.move64(arguments[1], self.pinned.frame);
        self.masm.move_imm32(arguments[2], value);
        self.masm.call_absolute(function);
    }

    /// Calls `function(VM*, ExecutionContext* frame, u32 site, u8* frame_pointer)`
    /// of the runtime, which materializes the frames of the inlined calls of
    /// call site `site` that compiled code describes from its frame pointer.
    fn emit_inlined_site_runtime_call(&mut self, function: u64, site: u32) {
        let arguments = M::ARGUMENT_GPRS;
        self.masm.move64(arguments[0], self.pinned.vm);
        self.masm.move64(arguments[1], self.pinned.frame);
        self.masm.move_imm32(arguments[2], site);
        self.masm.move64(arguments[3], M::FRAME_POINTER);
        self.masm.call_absolute(function);
    }

    /// Branches to `fail` unless `value` is a closure of the function whose
    /// shared data is `shared_data`. Clobbers `temp`.
    pub(super) fn emit_closure_check(&mut self, value: Gpr, shared_data: CellId, temp: Gpr, fail: Label) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.dynamic_calls;
        self.branch_on_tag(Condition::NotEqual, value, value::OBJECT_TAG, scratch, fail);
        self.emit_unbox_cell(temp, value);
        self.masm
            .load16(scratch, &Address::new(temp, self.runtime.offsets.object_flags as i32));
        self.masm.branch_test32(
            Condition::Zero,
            scratch,
            u32::from(layout.object_flag_is_ecmascript_function),
            fail,
        );
        self.masm.load64(
            scratch,
            &Address::new(temp, layout.ecmascript_function_shared_data as i32),
        );
        self.masm.move_imm64(temp, shared_data.0);
        self.masm.branch64(Condition::NotEqual, scratch, temp, fail);
    }

    /// Branches to the dynamic call of `call` unless its callee is `target`,
    /// or a closure of its function if the site called those.
    fn emit_direct_callee_check(&mut self, call: &JsCall, target: DirectCallTarget, target_node: NodeId) {
        let (function, aux, dynamic_call) = (target.function, call.aux, call.dynamic_call);
        let CallOperand::Register(callee_register) = call.callee_operand else {
            unreachable!("the callee is in a register");
        };
        if target.closures {
            self.emit_closure_check(callee_register, function.shared_data, aux, dynamic_call);
        } else if !self.callee_is_target_constant(Some(target_node), function.function) {
            self.masm.branch64_imm(
                Condition::NotEqual,
                callee_register,
                self.boxed_object(function.function) as i64,
                dynamic_call,
            );
        }
    }

    /// Loads the `this` value of a direct call of `function` into `call.aux`,
    /// bound like the interpreter's call fast path does it, if the function
    /// reads it.
    fn emit_direct_call_this(
        &mut self,
        call: &JsCall,
        function: InlinedFunctionSnapshot,
    ) -> Result<(), CompileFailure> {
        let (aux, temp, slow, this_operand) = (call.aux, call.temp, call.slow, call.this_operand);
        // Bind `this` like the interpreter's call fast path: as is for
        // strict callees, the global `this` for null and undefined in
        // sloppy ones, and objects only (the generic call boxes
        // primitives). `aux` holds it until it is stored.
        if function.uses_this {
            self.load_call_operand(aux, this_operand)?;
            if !function.strict {
                let bound = self.masm.new_label();
                let not_nullish = self.masm.new_label();
                self.masm.shr64_imm(temp, aux, 48);
                self.masm.or32_imm(temp, temp, 1);
                self.masm
                    .branch32_imm(Condition::NotEqual, temp, i32::from(value::NULL_TAG), not_nullish);
                self.masm.move_imm64(aux, self.boxed_object(function.global_this));
                self.masm.jump(bound);
                self.masm.bind(not_nullish);
                self.branch_on_tag(Condition::NotEqual, aux, value::OBJECT_TAG, temp, slow);
                self.masm.bind(bound);
            }
        }

        Ok(())
    }

    /// Builds the frame of a direct call of `target` on the interpreter
    /// stack, in `call.callee_frame`, after checking that the callee is the
    /// target.
    fn emit_direct_call_frame(
        &mut self,
        call: &JsCall,
        target: DirectCallTarget,
        target_node: NodeId,
    ) -> Result<(), CompileFailure> {
        let invalid = |reason| CompileFailure::InvalidBytecode { pc: call.pc, reason };
        let runtime = self.runtime;
        let offsets = runtime.offsets;
        let (vm, frame) = (self.pinned.vm, self.pinned.frame);
        let vm_field = |offset: u32| Address::new(vm, offset as i32);
        let slot = |index: u32| offsets.execution_context_slots + 8 * index;
        let (callee_frame, temp, aux, slow) = (call.callee_frame, call.temp, call.aux, call.slow);
        let (site, next_pc, dst, argument_count) = (&call.site, call.next_pc, call.dst, call.argument_count);
        let environment = match target.environment {
            Some(index) => Some(
                self.executables[call.executable as usize]
                    .environment_templates
                    .get(index as usize)
                    .cloned()
                    .ok_or(invalid("direct call without its environment template"))?,
            ),
            None => None,
        };
        let function = target.function;
        let formal_count = argument_count.max(function.formal_parameter_count);
        let arguments_base = target.registers_and_locals_and_constants_count;
        let (slot_count, frame_size) =
            direct_call_frame_size(&target, argument_count, &offsets).ok_or(invalid("frame too large"))?;
        let CallOperand::Register(callee_register) = call.callee_operand else {
            unreachable!("the callee is in a register");
        };
        self.emit_direct_callee_check(call, target, target_node);
        self.emit_direct_call_this(call, function)?;

        // Allocate the frame on the interpreter stack.
        // NB: The callee's JIT code checks the native stack at entry, and
        //     this code checked at its entry that the interpreter stack
        //     has room for the frame.
        // NB: A call without the frames of the inlined calls it is in
        //     leaves room for them below its callee's frame.
        self.masm
            .load64(callee_frame, &vm_field(offsets.vm_interpreter_stack_top));
        if let Some(site) = &site {
            self.masm
                .add64_imm(callee_frame, callee_frame, i64::from(site.frame_bytes));
        }
        self.masm.add64_imm(temp, callee_frame, i64::from(frame_size));
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), temp);

        // Fill in the frame like the interpreter's call fast path: its
        // constant fields first, with placeholders for the others.
        let field = |offset: u32| Address::new(callee_frame, offset as i32);
        let header = FrameHeader {
            function,
            executable: target.executable,
            passed_argument_count: argument_count,
            argument_count: formal_count,
            slot_count,
            return_pc: site.as_ref().map_or(next_pc, InlinedCallSite::return_pc),
            dst: dst.raw(),
            returns_to_native_caller: true,
        };
        let mut constants = self.frame_header_constants(&FrameHeaderConstants {
            passed_argument_count: header.passed_argument_count,
            return_pc: header.return_pc,
            dst: header.dst,
            returns_to_native_caller: header.returns_to_native_caller,
        });
        let fields = target.function_fields;
        constants.extend([
            (offsets.execution_context_function, 8, function.function.0),
            (offsets.execution_context_realm, 8, function.realm.0),
            (offsets.execution_context_executable, 8, header.executable.0),
            (offsets.execution_context_slot_count, 4, u64::from(header.slot_count)),
            (
                offsets.execution_context_argument_count,
                4,
                u64::from(header.argument_count),
            ),
            (
                offsets.execution_context_script_or_module,
                8,
                fields.script_or_module[0],
            ),
            (
                offsets.execution_context_script_or_module + 8,
                8,
                fields.script_or_module[1],
            ),
            (offsets.execution_context_lexical_environment, 8, fields.environment),
            (offsets.execution_context_variable_environment, 8, fields.environment),
            (
                offsets.execution_context_private_environment,
                8,
                fields.private_environment,
            ),
            (offsets.execution_context_this_value, 8, value::EMPTY),
            // NB: Frames get their id when the debugger first needs it.
            (offsets.execution_context_frame_id, 8, 0),
            // NB: A placeholder for the field stored below (see
            //     `overwritten`).
            (offsets.execution_context_caller_frame, 8, 0),
        ]);
        for register in 0..RESERVED_REGISTER_COUNT {
            constants.push((slot(register), 8, value::EMPTY));
        }
        for index in argument_count..formal_count {
            constants.push((slot(arguments_base + index), 8, value::UNDEFINED));
        }
        let mut overwritten = vec![offsets.execution_context_caller_frame];
        if function.uses_this {
            overwritten.extend([offsets.execution_context_this_value, slot(THIS_VALUE_REGISTER)]);
        }
        if environment.is_some() || target.closures {
            overwritten.extend([
                offsets.execution_context_lexical_environment,
                offsets.execution_context_variable_environment,
            ]);
        }
        if target.closures {
            overwritten.extend([
                offsets.execution_context_function,
                offsets.execution_context_private_environment,
            ]);
        }
        self.emit_constant_fields_with_vector(
            callee_frame,
            temp,
            &mut constants,
            Some(M::ARGUMENT_FPRS[0]),
            &overwritten,
        )?;
        self.masm.store64(&field(offsets.execution_context_caller_frame), frame);
        if site.is_some() {
            self.emit_store_inlined_call_frame_pointer(&field(offsets.execution_context_caller_dst_raw));
        }
        if function.uses_this {
            self.masm.store64(&field(offsets.execution_context_this_value), aux);
            self.masm.store64(&field(slot(THIS_VALUE_REGISTER)), aux);
        }

        // A closure's frame has the closure and its environments.
        if target.closures {
            match call.input_registers {
                Some(_) => self.emit_unbox_cell(aux, callee_register),
                None => {
                    let callee_address = self.slot_address(call.callee.raw())?;
                    self.masm.load64(aux, &callee_address);
                    self.emit_unbox_cell(aux, aux);
                }
            }
            self.masm.store64(&field(offsets.execution_context_function), aux);
            self.masm
                .load64(temp, &Address::new(aux, offsets.ecmascript_function_environment as i32));
            self.masm
                .store64(&field(offsets.execution_context_lexical_environment), temp);
            self.masm
                .store64(&field(offsets.execution_context_variable_environment), temp);
            self.masm.load64(
                temp,
                &Address::new(aux, offsets.ecmascript_function_private_environment as i32),
            );
            self.masm
                .store64(&field(offsets.execution_context_private_environment), temp);
        }

        for (index, argument) in (0..).zip(&call.argument_operands) {
            let destination = Address::new(
                callee_frame,
                super::checked_i32(u64::from(slot(arguments_base + index)))?,
            );
            self.store_call_operand(&destination, *argument)?;
        }

        if let Some(template) = &environment {
            let room_below = site.as_ref().map_or(0, |site| site.frame_bytes);
            self.emit_call_environment(template, callee_frame, room_below, temp, aux, slow)?;
        }
        Ok(())
    }

    /// Enters the callee of a direct call through its executable's call
    /// entry, and pops its frame where it returned. Returns the label after
    /// the call.
    fn emit_direct_call_entry(
        &mut self,
        call: &JsCall,
        target: DirectCallTarget,
        target_node: NodeId,
    ) -> Result<Label, CompileFailure> {
        let invalid = |reason| CompileFailure::InvalidBytecode { pc: call.pc, reason };
        let runtime = self.runtime;
        let offsets = runtime.offsets;
        let registers = M::ARGUMENT_GPRS;
        let other = registers[3];
        let (vm, frame) = (self.pinned.vm, self.pinned.frame);
        let vm_field = |offset: u32| Address::new(vm, offset as i32);
        let (site, pc, next_pc, dst, check) = (&call.site, call.pc, call.next_pc, call.dst, call.check);
        let (callee_frame, argument_count) = (call.callee_frame, call.argument_count);
        let program_counter = self.frame_field(offsets.execution_context_program_counter);
        // Enter the callee through its executable's call entry, with its
        // frame (in callee_frame) as the second argument: its JIT code, which
        // makes the frame the running one when something else may see it
        // (see `Op::PublishFrame`), or else (also while a debugger is
        // attached) an entry that has the interpreter run the frame. Operands
        // that are inputs are no longer needed, so the argument registers
        // are free.
        self.masm.move64(registers[1], callee_frame);
        self.masm.move_imm64(other, target.entry);
        let entry = registers[5];
        self.masm.load64(entry, &Address::new(other, 0));
        self.masm.move64(registers[0], vm);
        self.masm.call_register(entry);

        // The callee returned: pop its frame like the interpreter's Return.
        let [value_register, status] = [M::RETURN_GPRS[0], M::RETURN_GPRS[1]];
        let not_returned = self.masm.new_label();
        self.masm
            .branch64_imm(Condition::NotEqual, status, JitStatus::Returned as i64, not_returned);
        // NB: JIT code never returns the empty value. Calls with an output
        //     have it there, in the return register, and calls without one
        //     leave it in their destination slot like the interpreter.
        let has_output = self.graph.node(target_node).repr.is_some();
        if site.is_none() && !has_output {
            let destination = self.slot_address(dst.raw())?;
            self.masm.store64(&destination, value_register);
            self.masm.store_imm32(&program_counter, next_pc);
        }
        let returned = self.masm.new_label();
        // NB: The callee's frame is the top one on the interpreter stack.
        let popped = registers[2];
        self.masm.load64(popped, &vm_field(offsets.vm_interpreter_stack_top));
        let (_, frame_size) =
            direct_call_frame_size(&target, argument_count, &offsets).ok_or(invalid("frame too large"))?;
        let pushed_bytes = i64::from(frame_size) + site.as_ref().map_or(0, |site| i64::from(site.frame_bytes));
        self.masm.sub64_imm(popped, popped, pushed_bytes);
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), popped);
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), frame);
        self.masm
            .add32_to_memory_imm(&vm_field(offsets.vm_execution_generation), 1);
        self.masm.jump(returned);

        // The callee's frame still runs: let the runtime finish the call.
        self.masm.bind(not_returned);
        self.masm.move64(registers[3], status);
        if let Some(site) = &site {
            // NB: The runtime materializes the frames of the inlined calls
            //     in the room below the callee's frame first.
            self.masm.move64(registers[4], registers[3]);
            self.emit_inlined_site_runtime_call(runtime.finish_inlined_direct_call, site.index);
        } else {
            self.emit_frame_runtime_call(runtime.finish_direct_call, pc);
        }
        self.masm.jump(check);

        Ok(returned)
    }

    /// The generic call of `call`, through the call stub if the runtime has
    /// one and the runtime's call helper otherwise, and the continuation of
    /// every path, with the result of the `CallDirect` node `target_node`,
    /// if there is one.
    fn emit_js_call_fallbacks(&mut self, call: &JsCall, target_node: Option<NodeId>) -> Result<(), CompileFailure> {
        let runtime = self.runtime;
        let call_stub = runtime.dynamic_calls.call_stub;
        let (site, pc, next_pc, dst) = (&call.site, call.pc, call.next_pc, call.dst);
        let (callee_operand, this_operand) = (call.callee_operand, call.this_operand);
        let (callee, this_value, arguments) = (call.callee, call.this_value, &call.arguments);
        let argument_operands = &call.argument_operands;
        let input_registers = &call.input_registers;
        let (slow, check, done, dynamic_call) = (call.slow, call.check, call.done, call.dynamic_call);
        // The generic call, which the call stub also falls back to, reads
        // its operands from the frame.
        let operand_slots = [(callee, callee_operand), (this_value, this_operand)]
            .into_iter()
            .chain(arguments.iter().copied().zip(argument_operands.iter().copied()))
            .filter_map(|(operand, source)| match source {
                CallOperand::Register(register) if input_registers.is_some() => Some((operand, register)),
                _ => None,
            })
            .collect::<Vec<_>>();
        let store_operands = |codegen: &mut Self| -> Result<(), CompileFailure> {
            for (operand, register) in &operand_slots {
                let address = codegen.slot_address(operand.raw())?;
                codegen.masm.store64(&address, *register);
            }
            Ok(())
        };
        let inputs = input_registers.clone().unwrap_or_default();
        if call_stub != 0 {
            // The call stub makes every other call.
            self.masm.bind(dynamic_call);
            if let Some(site) = &site {
                self.emit_push_inlined_call_frames(site, &inputs);
            }
            store_operands(self)?;
            self.emit_call_stub_call(callee_operand, this_operand, argument_operands, pc, next_pc, dst.raw())?;
            self.masm.jump(check);
        }

        self.masm.bind(slow);
        if let Some(site) = &site {
            self.emit_push_inlined_call_frames(site, &inputs);
        }
        store_operands(self)?;
        self.emit_frame_runtime_call(runtime.jit_call, pc);
        self.masm.bind(check);
        self.emit_continuation_check(next_pc, &[]);
        if let Some(site) = &site {
            self.emit_pop_inlined_call_frames(site, dst)?;
        }
        self.masm.bind(done);
        if let Some(target_node) = target_node {
            self.emit_load_call_result(target_node, dst, site.as_ref())?;
        }
        Ok(())
    }

    /// The inlined call site of a direct or native call `node` that runs
    /// without the frames of the inlined calls it is in (see
    /// `Op::CallDirect`), whose `SiteKind::Call` site describes them.
    fn inlined_call_site(&mut self, node: NodeId) -> Option<InlinedCallSite> {
        let frame_bytes = match self.graph.node(node).op {
            Op::CallDirect {
                inlined_frame_bytes, ..
            }
            | Op::CallNative {
                inlined_frame_bytes, ..
            } if inlined_frame_bytes != 0 => inlined_frame_bytes,
            _ => return None,
        };
        let frame_state = self.graph.node(node).frame_state.expect("calls have a frame state");
        let chain = self.graph.frame_state_chain(frame_state);
        let outer = *chain.last().expect("a frame state chain has a frame state");
        let index = u32::try_from(self.sites.len()).expect("site count fits in u32");
        self.sites.push((node, SiteKind::Call));
        Some(InlinedCallSite {
            index,
            outer_pc: self.graph.frame_state(outer).pc,
            frame_bytes,
            depth: chain.len() - 1,
        })
    }

    /// Materializes the frames of the inlined calls a call without them is
    /// in, with the runtime, for its generic paths: on top of the
    /// interpreter stack, the innermost one running and in the frame
    /// register. Keeps `inputs`.
    fn emit_push_inlined_call_frames(&mut self, site: &InlinedCallSite, inputs: &[Gpr]) {
        let saved =
            |codegen: &Self, register: Gpr| codegen.local_address(codegen.locals.dump + 8 * u32::from(register.0));
        for register in inputs {
            let address = saved(self, *register);
            self.masm.store64(&address, *register);
        }
        self.emit_inlined_site_runtime_call(self.runtime.push_inlined_call_frames, site.index);
        self.masm.move64(self.pinned.frame, M::RETURN_GPRS[0]);
        for register in inputs {
            let address = saved(self, *register);
            self.masm.load64(*register, &address);
        }
    }

    /// Stores the low half of the frame pointer at `address`, the
    /// destination of the callee frame of a call without the frames of the
    /// inlined calls it is in, where stack walks find the values of those
    /// frames (see `INLINED_CALL_SITE_BIT`).
    fn emit_store_inlined_call_frame_pointer(&mut self, address: &Address) {
        self.masm.store32(address, M::FRAME_POINTER);
    }

    /// After the slow paths of a call without the frames of the inlined
    /// calls it is in, which ran in those frames: puts the result the call
    /// left in their innermost one in the call's output and pops them.
    fn emit_pop_inlined_call_frames(
        &mut self,
        site: &InlinedCallSite,
        dst: crate::bytecode::Operand,
    ) -> Result<(), CompileFailure> {
        self.masm.load64(
            self.pinned.frame,
            &Address::new(self.pinned.vm, self.runtime.offsets.vm_running_execution_context as i32),
        );
        let destination = self.slot_address(dst.raw())?;
        self.masm.load64(M::RETURN_GPRS[0], &destination);
        for _ in 0..site.depth {
            self.emit_pop_inline_frame();
        }
        Ok(())
    }

    /// Allocates the function environment of a direct call from `template`
    /// and makes it the lexical and variable environment of the callee's
    /// frame (in `callee_frame`), whose `this` value is bound already. If it
    /// cannot allocate inline, frees the frame again and branches to `slow`.
    /// Clobbers `temp`, `cell` and the scratch register.
    fn emit_call_environment(
        &mut self,
        template: &EnvironmentTemplateSnapshot,
        callee_frame: Gpr,
        room_below: u32,
        temp: Gpr,
        cell: Gpr,
        slow: Label,
    ) -> Result<(), CompileFailure> {
        let offsets = self.runtime.offsets;
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);
        let template_bytes = 8 * template.words.len() as u64;
        if template.allocator == 0
            || template_bytes > u64::from(template.cell_size)
            || u64::from(template.binding_values_offset) + 8 > template_bytes
            || u64::from(template.this_value_offset) + 8 > template_bytes
        {
            return Err(CompileFailure::CodeGeneration);
        }
        let no_room = self.masm.new_label();
        let allocated = self.masm.new_label();
        self.emit_check_heap_threshold(template.cell_size, temp, cell, no_room);
        self.emit_pop_free_list(template.allocator, cell, temp, no_room);
        self.emit_count_allocation(template.cell_size, template.cell_size, temp, false);

        let mut overwritten = vec![template.binding_values_offset];
        if template.binds_this {
            overwritten.push(template.this_value_offset);
        }
        let mut fields: Vec<ConstantField> = (0u32..)
            .zip(&template.words)
            .map(|(index, word)| (8 * index, 8, *word))
            .filter(|(offset, _, _)| !overwritten.contains(offset))
            .collect();
        self.emit_constant_fields_with_vector(cell, temp, &mut fields, Some(M::ARGUMENT_FPRS[0]), &[])?;
        if template.inline_binding_values {
            self.masm
                .load_effective_address(temp, &Address::new(cell, super::checked_i32(template_bytes)?));
            self.masm.store64(&at(cell, template.binding_values_offset), temp);
        } else {
            self.masm.store_imm64(&at(cell, template.binding_values_offset), 0);
        }
        if template.binds_this {
            self.masm
                .load64(temp, &at(callee_frame, offsets.execution_context_this_value));
            self.masm.store64(&at(cell, template.this_value_offset), temp);
        }
        self.masm
            .store64(&at(callee_frame, offsets.execution_context_lexical_environment), cell);
        self.masm
            .store64(&at(callee_frame, offsets.execution_context_variable_environment), cell);
        self.masm.jump(allocated);

        // NB: The callee's frame is the top of the interpreter stack, above
        //     the room it may leave for frames of inlined calls.
        self.masm.bind(no_room);
        if room_below != 0 {
            self.masm.sub64_imm(callee_frame, callee_frame, i64::from(room_below));
        }
        self.masm.store64(
            &Address::new(self.pinned.vm, offsets.vm_interpreter_stack_top as i32),
            callee_frame,
        );
        self.masm.jump(slow);
        self.masm.bind(allocated);
        Ok(())
    }

    /// Puts the value of a call operand in `dst`.
    fn load_call_operand(&mut self, dst: Gpr, operand: CallOperand) -> Result<(), CompileFailure> {
        match operand {
            CallOperand::Slot(operand) => {
                let address = self.slot_address(operand.raw())?;
                self.masm.load64(dst, &address);
            }
            CallOperand::Register(register) => self.masm.move64(dst, register),
            CallOperand::Undefined => self.masm.move_imm64(dst, value::UNDEFINED),
        }
        Ok(())
    }

    /// Stores the value of a call operand at `address`. Uses the scratch
    /// register unless the operand is in a register.
    fn store_call_operand(&mut self, address: &Address, operand: CallOperand) -> Result<(), CompileFailure> {
        let register = match operand {
            CallOperand::Register(register) => register,
            _ => {
                let scratch = self.pinned.scratch;
                self.load_call_operand(scratch, operand)?;
                scratch
            }
        };
        self.masm.store64(address, register);
        Ok(())
    }

    /// Calls the call stub (see `call_stub.rs`) for the call at `pc` of
    /// `callee`, with `this` and `arguments`, which it stores on the machine
    /// stack for the stub. Leaves the stub's control word in the first
    /// return register.
    fn emit_call_stub_call(
        &mut self,
        callee: CallOperand,
        this: CallOperand,
        arguments: &[CallOperand],
        pc: u32,
        next_pc: u32,
        dst: u32,
    ) -> Result<(), CompileFailure> {
        let registers = M::ARGUMENT_GPRS;
        let scratch = self.pinned.scratch;
        let argument_count = u32::try_from(arguments.len()).map_err(|_| CompileFailure::InvalidBytecode {
            pc,
            reason: "too many arguments",
        })?;
        let argument_bytes = (8 * i64::from(argument_count) + 15) & !15;
        if argument_bytes != 0 {
            self.masm.sub64_imm(M::STACK_POINTER, M::STACK_POINTER, argument_bytes);
        }
        for (index, argument) in (0u32..).zip(arguments) {
            let address = Address::new(M::STACK_POINTER, super::checked_i32(8 * u64::from(index))?);
            self.store_call_operand(&address, *argument)?;
        }
        // NB: Operands may be in the argument registers, so the callee
        //     moves after `this` is safe in the scratch register.
        self.load_call_operand(scratch, this)?;
        self.load_call_operand(registers[0], callee)?;
        self.masm.move64(registers[1], scratch);
        self.masm.move_imm32(registers[2], argument_count);
        self.masm
            .move_imm64(registers[3], u64::from(pc) | (u64::from(next_pc) << 32));
        self.masm.move_imm32(registers[4], dst);
        self.masm.call_absolute(self.runtime.dynamic_calls.call_stub);
        if argument_bytes != 0 {
            self.masm.add64_imm(M::STACK_POINTER, M::STACK_POINTER, argument_bytes);
        }
        Ok(())
    }

    /// Takes `slow` if the native stack is nearly exhausted, since a callee's
    /// JIT code runs on it.
    pub(super) fn emit_native_stack_check(&mut self, slow: Label) {
        let limit = Address::new(self.pinned.vm, self.runtime.offsets.vm_jit_native_stack_limit as i32);
        self.masm.branch_if_stack_pointer_below(&limit, slow);
    }

    /// `CallNative`: calls the raw native function a `Call` instruction saw
    /// the way the interpreter's `Call` fast path does: in a lightweight frame
    /// without an executable, linked to the calling frame, whose slots are
    /// the arguments. Other callees take the generic path.
    ///
    /// A node with inputs has the callee, the `this` value and the arguments
    /// in them (see `DIRECT_CALL_MAX_INPUTS`) and builds the frame in its
    /// temps; other nodes read their operands from the frame.
    /// Its inputs in registers are where `input_registers` says.
    pub(super) fn emit_call_native(
        &mut self,
        node: NodeId,
        executable: u32,
        pc: u32,
        input_registers: &super::InputRegisters,
    ) -> Result<(), CompileFailure> {
        let invalid = |reason| CompileFailure::InvalidBytecode { pc, reason };
        let snapshot = &self.executables[executable as usize];
        let decoded = self.decoded(executable, pc)?;
        let stored_operands = crate::builder::js_call_operands(&decoded.instruction).inputs;
        let next_pc = decoded.next_pc();
        let Instruction::Call {
            call_feedback,
            dst,
            callee,
            this_value,
            arguments,
            ..
        } = decoded.instruction
        else {
            return Err(invalid("native call of a non-Call instruction"));
        };
        let target: NativeCallTarget = snapshot
            .feedback
            .call
            .get(usize::from(call_feedback))
            .and_then(|feedback| feedback.native_call)
            .ok_or(invalid("native call without a target"))?;
        let runtime = self.runtime;
        let offsets = runtime.offsets;
        let registers = M::ARGUMENT_GPRS;

        if self.stores_operands(node) {
            self.emit_store_operand_inputs(node, &stored_operands, input_registers)?;
        }
        let inputs = if self.stores_operands(node) {
            Vec::new()
        } else {
            (0..self.graph.node(node).inputs.len())
                .map(|index| self.input(node, index))
                .collect::<Vec<_>>()
        };
        let (callee_operand, this_operand, argument_operands, temps) = if inputs.is_empty() {
            (
                CallOperand::Slot(callee),
                CallOperand::Slot(this_value),
                arguments
                    .iter()
                    .map(|operand| CallOperand::Slot(*operand))
                    .collect::<Vec<_>>(),
                [registers[1], registers[2], registers[3]],
            )
        } else {
            if inputs.len() != 2 + arguments.len() {
                return Err(invalid("native call with the wrong number of inputs"));
            }
            (
                CallOperand::Register(inputs[0]),
                CallOperand::Register(inputs[1]),
                inputs[2..]
                    .iter()
                    .map(|register| CallOperand::Register(*register))
                    .collect(),
                [self.temp(node, 0), self.temp(node, 1), self.temp(node, 2)],
            )
        };

        let site = self.inlined_call_site(node);
        let slow = self.masm.new_label();
        let check = self.masm.new_label();
        let done = self.masm.new_label();

        // Stack traces show the caller at its call, which is the call of the
        // compiled function that the call is inlined in, if it is.
        let program_counter = self.frame_field(offsets.execution_context_program_counter);
        self.masm
            .store_imm32(&program_counter, site.as_ref().map_or(pc, |site| site.outer_pc));

        // The callee must be the target; otherwise this is a generic call.
        if !self.callee_is_target_constant(Some(node), target.function) {
            let callee_register = match callee_operand {
                CallOperand::Register(register) => register,
                _ => {
                    self.load_call_operand(temps[1], callee_operand)?;
                    temps[1]
                }
            };
            self.masm.branch64_imm(
                Condition::NotEqual,
                callee_register,
                self.boxed_object(target.function) as i64,
                slow,
            );
        }

        self.emit_native_frame_call(
            target,
            this_operand,
            &argument_operands,
            next_pc,
            dst,
            site.as_ref(),
            temps,
            [slow, check, done],
        )?;

        // The generic call reads its operands from the frame.
        self.masm.bind(slow);
        if let Some(site) = &site {
            self.emit_push_inlined_call_frames(site, &inputs);
        }
        let operands = [callee, this_value].into_iter().chain(arguments.iter().copied());
        for (operand, source) in operands.zip(inputs.iter().copied()) {
            let address = self.slot_address(operand.raw())?;
            self.masm.store64(&address, source);
        }
        self.emit_frame_runtime_call(runtime.jit_call, pc);
        self.masm.bind(check);
        self.emit_continuation_check(next_pc, &[]);
        if let Some(site) = &site {
            self.emit_pop_inlined_call_frames(site, dst)?;
        }
        self.masm.bind(done);
        self.emit_load_call_result(node, dst, site.as_ref())
    }

    /// Calls a raw native function the way the interpreter's `Call` fast
    /// path does: in a lightweight frame without an executable, linked to the
    /// calling frame, whose slots are the arguments, built in `temps`. Jumps
    /// to `slow` (before anything observable happened) if the native stack is
    /// too full (the code checked at entry that the interpreter stack has
    /// room), to `check` once the runtime unwound the frame of a function
    /// that threw, and to `done` once the result is in `dst`, or for a call
    /// at an inlined call `site`, in the return register.
    #[allow(clippy::too_many_arguments)]
    fn emit_native_frame_call(
        &mut self,
        target: NativeCallTarget,
        this_operand: CallOperand,
        arguments: &[CallOperand],
        next_pc: u32,
        dst: crate::bytecode::Operand,
        site: Option<&InlinedCallSite>,
        [native_frame, temp, other]: [Gpr; 3],
        [slow, check, done]: [Label; 3],
    ) -> Result<(), CompileFailure> {
        let runtime = self.runtime;
        let offsets = runtime.offsets;
        let argument_count = u32::try_from(arguments.len()).map_err(|_| CompileFailure::InvalidBytecode {
            pc: next_pc,
            reason: "too many arguments",
        })?;
        let frame_size =
            super::checked_i32(u64::from(offsets.execution_context_slots) + 8 * u64::from(argument_count))?;

        let registers = M::ARGUMENT_GPRS;
        let scratch = self.pinned.scratch;
        let vm = self.pinned.vm;
        let frame = self.pinned.frame;
        let vm_field = |offset: u32| Address::new(vm, offset as i32);
        let frame_field = |offset: u32| Address::new(frame, offset as i32);
        let field = |offset: u32| Address::new(native_frame, offset as i32);

        // The native function runs on this native stack, so take the generic
        // path (which throws) if it is nearly exhausted.
        self.emit_native_stack_check(slow);

        // Allocate the frame on the interpreter stack, above room for the
        // frames of the inlined calls at an inlined call site.
        // NB: This code checked at its entry that the interpreter stack has
        //     room for the frame.
        self.masm
            .load64(native_frame, &vm_field(offsets.vm_interpreter_stack_top));
        if let Some(site) = site {
            self.masm
                .add64_imm(native_frame, native_frame, i64::from(site.frame_bytes));
        }
        self.masm.add64_imm(temp, native_frame, i64::from(frame_size));
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), temp);

        // Fill in the frame like the interpreter's call fast path.
        for (source, destination) in [
            (
                offsets.execution_context_lexical_environment,
                offsets.execution_context_lexical_environment,
            ),
            (
                offsets.execution_context_variable_environment,
                offsets.execution_context_variable_environment,
            ),
            (
                offsets.execution_context_private_environment,
                offsets.execution_context_private_environment,
            ),
        ] {
            self.masm.load64(temp, &frame_field(source));
            self.masm.store64(&field(destination), temp);
        }
        self.store_call_operand(&field(offsets.execution_context_this_value), this_operand)?;
        // NB: Frames get their id when the debugger first needs it.
        self.masm.store_imm64(&field(offsets.execution_context_frame_id), 0);
        self.masm.store64(&field(offsets.execution_context_caller_frame), frame);
        let mut constants = self.new_frame_constants(false);
        constants.extend([
            (offsets.execution_context_script_or_module, 8, 0),
            (offsets.execution_context_script_or_module + 8, 8, 0),
            (offsets.execution_context_executable, 8, 0),
            (offsets.execution_context_slot_count, 4, u64::from(argument_count)),
            (offsets.execution_context_argument_count, 4, u64::from(argument_count)),
            (
                offsets.execution_context_passed_argument_count,
                4,
                u64::from(argument_count),
            ),
            (
                offsets.execution_context_caller_return_pc,
                4,
                u64::from(site.map_or(next_pc, InlinedCallSite::return_pc)),
            ),
            (offsets.execution_context_caller_dst_raw, 4, u64::from(dst.raw())),
        ]);
        constants.push((offsets.execution_context_function, 8, target.function.0));
        constants.push((offsets.execution_context_realm, 8, target.realm.0));
        self.emit_constant_fields(native_frame, temp, &mut constants)?;
        if site.is_some() {
            self.emit_store_inlined_call_frame_pointer(&field(offsets.execution_context_caller_dst_raw));
        }
        for (index, argument) in (0u32..).zip(arguments) {
            let destination = Address::new(
                native_frame,
                super::checked_i32(u64::from(offsets.execution_context_slots) + 8 * u64::from(index))?,
            );
            let source = match argument {
                CallOperand::Register(register) => *register,
                _ => {
                    self.load_call_operand(other, *argument)?;
                    other
                }
            };
            self.masm.store64(&destination, source);
        }
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), native_frame);

        // Call the native function: it returns a ThrowCompletionOr<Value>,
        // the value or exception in the first return register and whether it
        // is an exception in the low byte of the second.
        self.masm.move_imm64(scratch, target.entry);
        self.masm.move64(registers[0], vm);
        self.masm.call_register(scratch);
        let [value_register, variant] = [M::RETURN_GPRS[0], M::RETURN_GPRS[1]];
        let threw = self.masm.new_label();
        self.masm.branch_test32(Condition::NonZero, variant, 0xFF, threw);
        // Pop the frame, which is the running one, like the interpreter does.
        // NB: The temps may include the return registers.
        self.masm
            .load64(scratch, &vm_field(offsets.vm_running_execution_context));
        if let Some(site) = site {
            self.masm.sub64_imm(scratch, scratch, i64::from(site.frame_bytes));
        }
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), scratch);
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), frame);
        if site.is_none() {
            let destination = self.slot_address(dst.raw())?;
            self.masm.store64(&destination, value_register);
        }
        self.masm.jump(done);

        // The runtime unwinds the frame of a native function that threw, at
        // an inlined call site once it materialized the frames of the
        // inlined calls below it.
        self.masm.bind(threw);
        if let Some(site) = site {
            self.masm.move64(registers[4], value_register);
            self.emit_inlined_site_runtime_call(runtime.inlined_raw_native_exception, site.index);
        } else {
            self.masm.move64(registers[1], value_register);
            self.masm.move64(registers[0], self.pinned.vm);
            self.masm.call_absolute(runtime.raw_native_exception);
        }
        self.masm.jump(check);
        Ok(())
    }

    /// Stores the header fields of a new frame at `new_frame` (allocated on
    /// the interpreter stack) that come from the function object and the VM,
    /// linked to the running frame like the interpreter's inline calls do,
    /// and returns the constant ones for `emit_constant_fields()`. Its `this`
    /// value and slots are left to the caller. Clobbers `function_register`
    /// and `temp`.
    fn emit_frame_header(
        &mut self,
        new_frame: Gpr,
        function_register: Gpr,
        temp: Gpr,
        header: &FrameHeader,
    ) -> Vec<ConstantField> {
        let offsets = self.runtime.offsets;
        let snapshot = self
            .executables
            .iter()
            .find(|executable| executable.cell == header.executable);
        let function_fields = snapshot.and_then(|executable| executable.function_fields);
        let builtin = snapshot.is_some_and(|executable| executable.builtin);
        let mut constants = Vec::new();
        if builtin {
            // NB: Frames of builtins written in JavaScript have the
            //     environments of their caller, the running frame, and no
            //     script or module.
            let caller_field = |offset: u32| Address::new(self.pinned.frame, offset as i32);
            let field = |offset: u32| Address::new(new_frame, offset as i32);
            for offset in [
                offsets.execution_context_lexical_environment,
                offsets.execution_context_variable_environment,
                offsets.execution_context_private_environment,
            ] {
                self.masm.load64(temp, &caller_field(offset));
                self.masm.store64(&field(offset), temp);
            }
            self.masm
                .store64(&field(offsets.execution_context_caller_frame), self.pinned.frame);
            constants.extend([
                (offsets.execution_context_function, 8, header.function.function.0),
                (offsets.execution_context_script_or_module, 8, 0),
                (offsets.execution_context_script_or_module + 8, 8, 0),
                // NB: Frames get their id when the debugger first needs it.
                (offsets.execution_context_frame_id, 8, 0),
            ]);
        } else if let Some(fields) = function_fields {
            // NB: The words of the function its frames start with stay the same for its lifetime.
            self.masm.store64(
                &Address::new(new_frame, offsets.execution_context_caller_frame as i32),
                self.pinned.frame,
            );
            constants.extend([
                (offsets.execution_context_function, 8, header.function.function.0),
                (
                    offsets.execution_context_script_or_module,
                    8,
                    fields.script_or_module[0],
                ),
                (
                    offsets.execution_context_script_or_module + 8,
                    8,
                    fields.script_or_module[1],
                ),
                (offsets.execution_context_lexical_environment, 8, fields.environment),
                (offsets.execution_context_variable_environment, 8, fields.environment),
                (
                    offsets.execution_context_private_environment,
                    8,
                    fields.private_environment,
                ),
                // NB: Frames get their id when the debugger first needs it.
                (offsets.execution_context_frame_id, 8, 0),
            ]);
        } else {
            self.masm.move_imm64(function_register, header.function.function.0);
            self.emit_function_fields(new_frame, function_register, temp);
        }
        constants.extend(self.frame_header_field_constants(header));
        constants
    }

    /// The constant fields of a new frame that do not depend on its
    /// function object, apart from its realm.
    fn frame_header_field_constants(&self, header: &FrameHeader) -> Vec<ConstantField> {
        let offsets = self.runtime.offsets;
        let mut constants = self.frame_header_constants(&FrameHeaderConstants {
            passed_argument_count: header.passed_argument_count,
            return_pc: header.return_pc,
            dst: header.dst,
            returns_to_native_caller: header.returns_to_native_caller,
        });
        constants.extend([
            (offsets.execution_context_realm, 8, header.function.realm.0),
            (offsets.execution_context_executable, 8, header.executable.0),
            (offsets.execution_context_slot_count, 4, u64::from(header.slot_count)),
            (
                offsets.execution_context_argument_count,
                4,
                u64::from(header.argument_count),
            ),
        ]);
        constants
    }

    /// Stores the fields of a new frame that come from its function (whose
    /// address `function_register` holds), its frame id and its caller, the
    /// running frame. Clobbers `temp`.
    pub(super) fn emit_function_fields(&mut self, new_frame: Gpr, function_register: Gpr, temp: Gpr) {
        let offsets = self.runtime.offsets;
        let field = |offset: u32| Address::new(new_frame, offset as i32);
        self.masm
            .store64(&field(offsets.execution_context_function), function_register);
        let function_field = |offset: u32| Address::new(function_register, offset as i32);
        for word in [0, 8] {
            self.masm.load64(
                temp,
                &function_field(offsets.ecmascript_function_script_or_module + word),
            );
            self.masm
                .store64(&field(offsets.execution_context_script_or_module + word), temp);
        }
        self.masm
            .load64(temp, &function_field(offsets.ecmascript_function_environment));
        self.masm
            .store64(&field(offsets.execution_context_lexical_environment), temp);
        self.masm
            .store64(&field(offsets.execution_context_variable_environment), temp);
        self.masm
            .load64(temp, &function_field(offsets.ecmascript_function_private_environment));
        self.masm
            .store64(&field(offsets.execution_context_private_environment), temp);
        // NB: Frames get their id when the debugger first needs it.
        self.masm.store_imm64(&field(offsets.execution_context_frame_id), 0);
        self.masm
            .store64(&field(offsets.execution_context_caller_frame), self.pinned.frame);
    }

    /// The fields every new frame starts with: no code ran in it yet, it is
    /// not initialized, and whether its caller waits for it in native code
    /// (and so runs it in JIT code until the runtime finishes it, see
    /// `finish_direct_call`).
    pub(super) fn new_frame_constants(&self, returns_to_native_caller: bool) -> Vec<ConstantField> {
        let offsets = self.runtime.offsets;
        vec![
            (offsets.execution_context_program_counter, 4, 0),
            (offsets.execution_context_skip_when_determining_incumbent_counter, 4, 0),
            (
                offsets.execution_context_yield_continuation,
                4,
                u64::from(self.runtime.no_yield_continuation),
            ),
            (offsets.execution_context_yield_is_await, 1, 0),
            (offsets.execution_context_yield_value_is_iterator_result, 1, 0),
            (offsets.execution_context_caller_is_construct, 1, 0),
            (offsets.execution_context_frame_initialized, 1, 0),
            (
                offsets.execution_context_returns_to_native_caller,
                1,
                u64::from(returns_to_native_caller),
            ),
            (
                offsets.execution_context_runs_jit_code,
                1,
                u64::from(returns_to_native_caller),
            ),
        ]
    }

    /// The constant fields of a new frame that do not depend on its function.
    fn frame_header_constants(&self, header: &FrameHeaderConstants) -> Vec<ConstantField> {
        let offsets = self.runtime.offsets;
        let mut constants = self.new_frame_constants(header.returns_to_native_caller);
        constants.extend([
            (
                offsets.execution_context_passed_argument_count,
                4,
                u64::from(header.passed_argument_count),
            ),
            (
                offsets.execution_context_caller_return_pc,
                4,
                u64::from(header.return_pc),
            ),
            (offsets.execution_context_caller_dst_raw, 4, u64::from(header.dst)),
        ]);
        constants
    }

    /// Materializes the frame of an inlined call of the
    /// snapshot's executable `executable` from the running frame, like the
    /// interpreter's inline calls do, uninitialized (with its reserved
    /// registers and undefined arguments), and makes it the running frame. JIT code checked
    /// at entry that the interpreter stack has room for it. `this` is the
    /// callee's `this`, and `temps` are clobbered. A `construct` frame
    /// returns `this` unless its callee returns an object. A frame of an
    /// inlined closure runs the `closure` (its address), with its
    /// environments.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn emit_push_inline_frame(
        &mut self,
        executable: u32,
        call_pc: u32,
        return_pc: u32,
        dst: u32,
        passed_argument_count: u32,
        construct: bool,
        this: Gpr,
        closure: Option<Gpr>,
        temps: [Gpr; 3],
    ) -> Result<(), CompileFailure> {
        let snapshot = &self.executables[executable as usize];
        let function = snapshot.function.ok_or(CompileFailure::InvalidBytecode {
            pc: call_pc,
            reason: "inlined executable without a function",
        })?;
        let layout = snapshot.layout;
        let offsets = self.runtime.offsets;
        let argument_count = passed_argument_count.max(function.formal_parameter_count);
        let arguments_base = layout.arguments_base();
        let slot_count = arguments_base + argument_count;
        let frame_size = super::checked_i32(u64::from(offsets.execution_context_slots) + 8 * u64::from(slot_count))?;
        let [new_frame, temp, function_register] = temps;
        let vm = self.pinned.vm;
        let vm_field = |offset: u32| Address::new(vm, offset as i32);

        // The caller is at its call, which is where stack traces show it.
        let program_counter = self.frame_field(offsets.execution_context_program_counter);
        self.masm.store_imm32(&program_counter, call_pc);
        self.masm.load64(new_frame, &vm_field(offsets.vm_interpreter_stack_top));
        self.masm.add64_imm(temp, new_frame, i64::from(frame_size));
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), temp);

        let header = FrameHeader {
            function,
            executable: snapshot.cell,
            passed_argument_count,
            argument_count,
            slot_count,
            return_pc,
            dst,
            returns_to_native_caller: false,
        };
        let mut constants = match closure {
            Some(closure) => {
                self.emit_function_fields(new_frame, closure, temp);
                self.frame_header_field_constants(&header)
            }
            None => self.emit_frame_header(new_frame, function_register, temp, &header),
        };
        if construct {
            for field in &mut constants {
                if field.0 == offsets.execution_context_caller_is_construct {
                    field.2 = 1;
                }
            }
        }
        let slot = |index: u32| offsets.execution_context_slots + 8 * index;
        self.masm.store64(
            &Address::new(new_frame, offsets.execution_context_this_value as i32),
            this,
        );
        self.masm.store64(
            &Address::new(new_frame, super::checked_i32(u64::from(slot(THIS_VALUE_REGISTER)))?),
            this,
        );
        // NB: The garbage collector visits only the reserved registers and
        //     the arguments of frames that are not initialized.
        let reserved_registers = layout.registers_and_locals_count.min(RESERVED_REGISTER_COUNT);
        for register in (0..reserved_registers).filter(|register| *register != THIS_VALUE_REGISTER) {
            constants.push((slot(register), 8, value::EMPTY));
        }
        for index in 0..argument_count {
            constants.push((slot(arguments_base + index), 8, value::UNDEFINED));
        }
        self.emit_constant_fields(new_frame, temp, &mut constants)?;
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), new_frame);
        // Frame slots now address the new frame.
        self.masm.move64(self.pinned.frame, new_frame);
        Ok(())
    }

    /// Pops the running frame, the frame of an inlined call that compiled
    /// code or the runtime pushed for a slow path, like the runtime unwinds
    /// an inline frame, and makes its caller the running frame again.
    pub(super) fn emit_pop_inline_frame(&mut self) {
        let offsets = self.runtime.offsets;
        let (vm, frame, scratch) = (self.pinned.vm, self.pinned.frame, self.pinned.scratch);
        self.masm.load64(
            scratch,
            &Address::new(frame, offsets.execution_context_caller_frame as i32),
        );
        self.masm
            .store64(&Address::new(vm, offsets.vm_interpreter_stack_top as i32), frame);
        self.masm
            .store64(&Address::new(vm, offsets.vm_running_execution_context as i32), scratch);
        self.masm.move64(frame, scratch);
    }

    /// Stores the constant `fields` relative to `base`, merging adjacent ones
    /// into wide stores. Clobbers `temp`.
    pub(super) fn emit_constant_fields(
        &mut self,
        base: Gpr,
        temp: Gpr,
        fields: &mut [ConstantField],
    ) -> Result<(), CompileFailure> {
        self.emit_constant_fields_with_vector(base, temp, fields, None, &[])
    }

    /// Like `emit_constant_fields()`, also storing 16 bytes at a time from
    /// the constant pool through `vector` where that takes fewer
    /// instructions. The 8-byte fields at the offsets `overwritten`, which
    /// the caller stores afterwards, may get any value. Clobbers `temp` and
    /// `vector`.
    pub(super) fn emit_constant_fields_with_vector(
        &mut self,
        base: Gpr,
        temp: Gpr,
        fields: &mut [ConstantField],
        vector: Option<Fpr>,
        overwritten: &[u32],
    ) -> Result<(), CompileFailure> {
        fields.sort_unstable_by_key(|(offset, _, _)| *offset);
        // Runs of adjacent fields, as their start and bytes.
        let mut runs: Vec<(u32, Vec<u8>)> = Vec::new();
        for (offset, size, bits) in fields.iter() {
            let bytes = &bits.to_le_bytes()[..*size as usize];
            match runs.last_mut() {
                Some((start, run)) if *start + run.len() as u32 == *offset => run.extend_from_slice(bytes),
                _ => runs.push((*offset, bytes.to_vec())),
            }
        }
        // The values `temp` and `vector` hold, to store repeated values
        // without materializing them again.
        let mut held = None;
        let mut held_vector = None;
        for (start, bytes) in runs {
            let mut index = 0;
            while index < bytes.len() {
                let offset = start + index as u32;
                let address = Address::new(base, super::checked_i32(u64::from(offset))?);
                let remaining = bytes.len() - index;
                let word_cost = |word_offset: u32, word: u64, held: Option<u64>| {
                    if overwritten.contains(&word_offset) {
                        0
                    } else if i32::try_from(word as i64).is_ok() || held == Some(word) {
                        1
                    } else {
                        2
                    }
                };
                // NB: A 16-byte store takes one instruction and a load from the
                //     constant pool, which pays off when its words do not all
                //     fit store immediates.
                if let Some(vector) = vector
                    && remaining >= 16
                    && offset.is_multiple_of(8)
                {
                    let chunk = u128::from_le_bytes(bytes[index..index + 16].try_into().expect("16 bytes"));
                    let vector_cost = if held_vector == Some(chunk) { 1 } else { 2 };
                    let scalar_cost =
                        word_cost(offset, chunk as u64, held) + word_cost(offset + 8, (chunk >> 64) as u64, held);
                    if vector_cost < scalar_cost {
                        if held_vector != Some(chunk) {
                            self.masm.load_imm128(vector, chunk);
                            held_vector = Some(chunk);
                        }
                        self.masm.store128(&address, vector);
                        index += 16;
                        continue;
                    }
                }
                let size = [8, 4, 2, 1]
                    .into_iter()
                    .find(|size| remaining >= *size && offset.is_multiple_of(*size as u32))
                    .unwrap_or(1);
                let mut word = [0u8; 8];
                word[..size].copy_from_slice(&bytes[index..index + size]);
                let bits = u64::from_le_bytes(word);
                match size {
                    8 if overwritten.contains(&offset) => {}
                    8 if i32::try_from(bits as i64).is_ok() => self.masm.store_imm64(&address, bits),
                    // NB: The low half of the held vector stores as a double.
                    8 if held != Some(bits)
                        && let Some(vector) = vector
                        && held_vector.is_some_and(|held_vector| held_vector as u64 == bits) =>
                    {
                        self.masm.store_double(&address, vector);
                    }
                    8 => {
                        if held != Some(bits) {
                            self.masm.move_imm64(temp, bits);
                            held = Some(bits);
                        }
                        self.masm.store64(&address, temp);
                    }
                    4 => self.masm.store_imm32(&address, bits as u32),
                    _ => {
                        self.masm.move_imm32(temp, bits as u32);
                        held = None;
                        if size == 2 {
                            self.masm.store16(&address, temp);
                        } else {
                            self.masm.store8(&address, temp);
                        }
                    }
                }
                index += size;
            }
        }
        Ok(())
    }
}

/// The slot count and size in bytes of the frame a direct call of `target`
/// with `argument_count` arguments pushes, if it fits.
fn direct_call_frame_size(
    target: &DirectCallTarget,
    argument_count: u32,
    offsets: &RuntimeOffsets,
) -> Option<(u32, i32)> {
    let formal_count = argument_count.max(target.function.formal_parameter_count);
    let slot_count = target
        .registers_and_locals_and_constants_count
        .checked_add(formal_count)?;
    let frame_size = i32::try_from(u64::from(offsets.execution_context_slots) + 8 * u64::from(slot_count)).ok()?;
    Some((slot_count, frame_size))
}
