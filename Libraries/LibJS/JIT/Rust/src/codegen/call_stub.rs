/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The call stub: the code JIT code shares for every call it does not make
//! directly (`Generic` call nodes, and direct calls whose callee is not their
//! target). It makes the call the way the interpreter's `Call` fast path
//! does, with what it needs read from the callee at call time, and takes the
//! generic path through `RuntimeInfo::jit_call` for every other callee.
//!
//! Calling convention: the vm and frame registers hold the VM and the calling
//! frame, which the stub preserves (it clobbers every other register). The
//! argument registers hold:
//!
//! 0. the callee value,
//! 1. the `this` value,
//! 2. the argument count,
//! 3. the pc of the call instruction in the low 32 bits, and the pc of the
//!    instruction after it in the high 32 bits,
//! 4. the call's destination slot index,
//!
//! and the arguments are on the machine stack, where the stub's caller stored
//! them right before the call (8 bytes each, the first one lowest). The stub
//! returns a slow path control word: a continuation at the next pc once the
//! result is in the destination slot, or what the runtime returned.

use super::Codegen;
use super::Locals;
use super::target_registers;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::FprSet;
use crate::asm::Gpr;
use crate::asm::GprSet;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::bytecode::THIS_VALUE_REGISTER;
use crate::code::JitStatus;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::value;
use crate::regalloc::Allocation;
use crate::snapshot::RuntimeInfo;

/// Where the stub keeps its inputs and what must survive calls into the
/// runtime and the callee, relative to the stack pointer.
mod local {
    pub const CALLEE: u32 = 0;
    pub const THIS: u32 = 8;
    pub const ARGUMENT_COUNT: u32 = 16;
    pub const PCS: u32 = 24;
    pub const DST: u32 = 32;
    /// The callee's environment from `prepare_call_environment`, or 0.
    pub const ENVIRONMENT: u32 = 40;
    /// The callee's `this` from `prepare_call_environment`.
    pub const PREPARED_THIS: u32 = 48;
    /// The callee's executable while its frame is built, then the frame.
    pub const EXECUTABLE: u32 = 56;
    pub const SIZE: u32 = 64;
}

/// Generates the call stub, or nothing if the runtime lacks a helper it
/// needs (then JIT code makes those calls through `RuntimeInfo::jit_call`).
pub fn generate_call_stub<M: PortableMacroAssembler>(runtime: &RuntimeInfo) -> Result<Option<Vec<u8>>, CompileFailure> {
    let layout = runtime.dynamic_calls;
    if layout.object_flag_is_ecmascript_function == 0 || runtime.jit_call == 0 || runtime.finish_direct_call == 0 {
        return Ok(None);
    }
    let graph = Graph::default();
    let allocation = Allocation::default();
    let (_, pinned) = target_registers::<M>();
    let mut masm = M::new();
    let exit_stub = masm.new_label();
    let resume = masm.new_label();
    let exit_interpreter = masm.new_label();
    let mut codegen = Codegen {
        masm,
        graph: &graph,
        allocation: &allocation,
        executables: &[],
        runtime,
        stress: crate::snapshot::StressOptions::default(),
        pinned,
        frame: M::frame(GprSet::EMPTY, FprSet::EMPTY, local::SIZE),
        locals: Locals::default(),
        gpr_dump_count: 0,
        fpr_dump_count: 0,
        block_labels: Vec::new(),
        landing_blocks: Vec::new(),
        exit_stub,
        resume,
        entry_resume: resume,
        exit_interpreter,
        sites: Vec::new(),
        exit_site_stubs: Vec::new(),
        annotations: Vec::new(),
        block: BlockId(0),
        deferred_cache_probes: Vec::new(),
        leave_arguments: Vec::new(),
        leave_stubs: Vec::new(),
        leave_frame: None,
        leave_tail: exit_stub,
        slow_path_values: None,
        invalidation_points: Vec::new(),
        fused_probe_failure: None,
        deferred_allocations: Vec::new(),
        deferred_storage_growths: Vec::new(),
        absorbed_initializations: Vec::new(),
    };
    codegen.emit_call_stub()?;
    let (code, _) = codegen
        .masm
        .finish_with_data_offset()
        .map_err(|_| CompileFailure::CodeGeneration)?;
    Ok(Some(code))
}

/// The registers `emit_stub_call_environment()` works with: the callee's
/// shared data (which it clobbers), the callee and its call metadata, and two
/// it clobbers.
struct StubCallEnvironmentRegisters {
    shared_data: Gpr,
    function: Gpr,
    metadata: Gpr,
    template: Gpr,
    cell: Gpr,
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Allocates the function environment of a call of the callee inline from
    /// its shared data's call environment template (see
    /// `DynamicCallLayout::shared_data_call_environment_template`), and keeps
    /// it and the `this` value of the call in the stub's locals. Branches to
    /// `from_runtime` without a template, without room on the local free list
    /// or before the next collection, and for `this` values the call would
    /// convert. Clobbers the scratch register.
    fn emit_stub_call_environment(&mut self, registers: StubCallEnvironmentRegisters, from_runtime: Label) {
        let StubCallEnvironmentRegisters {
            shared_data,
            function,
            metadata,
            template,
            cell,
        } = registers;
        let layout = self.runtime.dynamic_calls;
        let heap_info = self.runtime.object_allocation.clone();
        let scratch = self.pinned.scratch;
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);
        let environment_local = self.stub_local(local::ENVIRONMENT);
        let prepared_this = self.stub_local(local::PREPARED_THIS);
        let this_local = self.stub_local(local::THIS);

        self.masm
            .load64(template, &at(shared_data, layout.shared_data_call_environment_template));
        self.masm
            .branch_test64(Condition::Zero, template, u64::MAX, from_runtime);
        // NB: The template is a cell, whose pointer is decoded into the heap
        //     region like a cell value.
        self.emit_unbox_cell(template, template);

        // The `this` value: as is for strict callees and objects only for
        // sloppy ones, like the runtime binds it then, or none.
        let binds_this = self.masm.new_label();
        let bound = self.masm.new_label();
        self.masm
            .load64(scratch, &at(template, layout.call_environment_template_binds_this));
        self.masm
            .branch_test64(Condition::NonZero, scratch, u64::MAX, binds_this);
        self.masm.store_imm64(&prepared_this, value::EMPTY);
        self.masm.jump(bound);
        self.masm.bind(binds_this);
        self.masm.load64(scratch, &this_local);
        let strict = self.masm.new_label();
        self.masm
            .branch_test64(Condition::NonZero, metadata, layout.metadata_strict, strict);
        self.branch_on_tag(Condition::NotEqual, scratch, value::OBJECT_TAG, cell, from_runtime);
        self.masm.bind(strict);
        self.masm.store64(&prepared_this, scratch);
        self.masm.bind(bound);

        // Room before the next collection, and a cell from the local free list.
        let heap = shared_data;
        self.masm.move_imm64(heap, heap_info.heap);
        self.masm
            .load64(cell, &at(template, layout.call_environment_template_cell_size));
        self.masm.load64(scratch, &at(heap, heap_info.heap_threshold_offset));
        self.masm.sub64(scratch, scratch, cell);
        self.masm.branch64_memory(
            Condition::GreaterThan,
            &at(heap, heap_info.heap_allocated_bytes_offset),
            scratch,
            from_runtime,
        );
        self.masm
            .load64(scratch, &at(template, layout.call_environment_template_size_class));
        self.masm
            .and32_imm(scratch, scratch, layout.function_environment_size_class_mask);
        self.masm.move_imm64(cell, layout.function_environment_free_lists);
        self.masm.load64(
            scratch,
            &Address {
                base: cell,
                index: Some((scratch, Scale::Eight)),
                displacement: 0,
            },
        );
        self.emit_pop_free_list_of(scratch, cell, heap, from_runtime);

        // The environment, from the template's words and the call.
        for index in 0..layout.function_environment_words {
            self.masm.load64(
                scratch,
                &at(template, layout.call_environment_template_words + 8 * index),
            );
            self.masm.store64(&at(cell, 8 * index), scratch);
        }
        let outside_the_cell = self.masm.new_label();
        self.masm.load64(
            scratch,
            &at(template, layout.call_environment_template_binding_values_offset),
        );
        self.masm
            .branch_test64(Condition::Zero, scratch, u64::MAX, outside_the_cell);
        self.masm.add64(scratch, scratch, cell);
        self.masm
            .store64(&at(cell, layout.function_environment_binding_values), scratch);
        self.masm.bind(outside_the_cell);
        self.masm.load64(
            scratch,
            &at(function, self.runtime.offsets.ecmascript_function_environment),
        );
        self.masm.store64(&at(cell, layout.function_environment_outer), scratch);
        self.masm
            .store64(&at(cell, layout.function_environment_function_object), function);
        let no_this = self.masm.new_label();
        self.masm
            .load64(scratch, &at(template, layout.call_environment_template_binds_this));
        self.masm.branch_test64(Condition::Zero, scratch, u64::MAX, no_this);
        self.masm.load64(scratch, &prepared_this);
        self.masm
            .store64(&at(cell, layout.function_environment_this_value), scratch);
        self.masm.bind(no_this);
        self.masm.store64(&environment_local, cell);

        // Count the cell like the heap does.
        self.masm
            .load64(template, &at(template, layout.call_environment_template_cell_size));
        self.masm.move_imm64(heap, heap_info.heap);
        for offset in [
            heap_info.heap_allocated_bytes_offset,
            heap_info.heap_total_allocated_bytes_offset,
        ] {
            self.masm.load64(scratch, &at(heap, offset));
            self.masm.add64(scratch, scratch, template);
            self.masm.store64(&at(heap, offset), scratch);
        }
    }

    fn stub_local(&self, offset: u32) -> Address {
        self.frame.local(M::STACK_POINTER, offset)
    }

    /// The address of the `index`th argument the stub's caller stored.
    fn stub_argument(&self, index: crate::asm::Gpr) -> Address {
        Address {
            base: M::STACK_POINTER,
            index: Some((index, Scale::Eight)),
            displacement: self.frame.caller_stack_offset as i32,
        }
    }

    fn emit_call_stub(&mut self) -> Result<(), CompileFailure> {
        let runtime = self.runtime;
        let offsets = runtime.offsets;
        let layout = runtime.dynamic_calls;
        let registers = M::ARGUMENT_GPRS;
        let [callee_value, this_argument, argument_count, pcs, dst] =
            [registers[0], registers[1], registers[2], registers[3], registers[4]];
        let executable_register = registers[0];
        let callee_frame = registers[1];
        let temp = registers[2];
        let other = registers[3];
        let function_register = registers[4];
        let receiver = registers[5];
        let scratch = self.pinned.scratch;
        let vm = self.pinned.vm;
        let frame = self.pinned.frame;
        let vm_field = |offset: u32| Address::new(vm, offset as i32);
        let slot = |index: u32| offsets.execution_context_slots + 8 * index;

        let slow = self.masm.new_label();
        let leave = self.masm.new_label();

        let machine_frame = self.frame;
        self.masm.emit_prologue(&machine_frame);
        for (offset, register) in [
            (local::CALLEE, callee_value),
            (local::THIS, this_argument),
            (local::ARGUMENT_COUNT, argument_count),
            (local::PCS, pcs),
            (local::DST, dst),
        ] {
            let address = self.stub_local(offset);
            self.masm.store64(&address, register);
        }

        // Stack traces show the caller at its call.
        self.masm.store32(
            &Address::new(frame, offsets.execution_context_program_counter as i32),
            pcs,
        );

        // The callee must be an object.
        self.masm.move64(temp, callee_value);
        self.branch_on_tag(Condition::NotEqual, temp, value::OBJECT_TAG, scratch, slow);
        self.emit_unbox_cell(function_register, temp);
        self.masm
            .load16(temp, &Address::new(function_register, offsets.object_flags as i32));

        // Raw native functions are called in their lightweight frame.
        let ecmascript_function = self.masm.new_label();
        self.masm.branch_test32(
            Condition::NonZero,
            temp,
            u32::from(layout.object_flag_is_ecmascript_function),
            ecmascript_function,
        );
        if layout.object_flag_is_raw_native_function != 0 && runtime.raw_native_exception != 0 {
            self.masm.branch_test32(
                Condition::Zero,
                temp,
                u32::from(layout.object_flag_is_raw_native_function),
                slow,
            );
            self.emit_stub_native_call(slow, leave)?;
        } else {
            self.masm.jump(slow);
        }

        // An ECMAScript function the interpreter's call fast path calls in an
        // inline frame.
        self.masm.bind(ecmascript_function);
        let load_function_data = |codegen: &mut Self| {
            codegen.masm.load64(
                other,
                &Address::new(function_register, layout.ecmascript_function_shared_data as i32),
            );
            codegen.masm.load64(
                executable_register,
                &Address::new(other, layout.shared_data_executable as i32),
            );
            codegen.masm.load64(
                receiver,
                &Address::new(other, layout.shared_data_asm_call_metadata as i32),
            );
        };
        load_function_data(self);
        self.masm
            .branch_test64(Condition::Zero, receiver, layout.metadata_can_inline_call, slow);
        self.masm.branch64_imm(Condition::Equal, executable_register, 0, slow);
        let environment_local = self.stub_local(local::ENVIRONMENT);
        self.masm.store_imm64(&environment_local, 0);

        // Callees that need a function environment or the resolution of
        // their `this` value get both from the runtime before their frame is
        // built.
        let resume = self.masm.new_label();
        if layout.prepare_call_environment != 0 {
            self.masm.branch_test64(
                Condition::Zero,
                receiver,
                layout.metadata_needs_environment_or_this_value_resolution,
                resume,
            );
            if layout.shared_data_call_environment_template != 0 && runtime.object_allocation.heap != 0 {
                let from_runtime = self.masm.new_label();
                self.emit_stub_call_environment(
                    StubCallEnvironmentRegisters {
                        shared_data: other,
                        function: function_register,
                        metadata: receiver,
                        template: callee_frame,
                        cell: temp,
                    },
                    from_runtime,
                );
                self.masm.jump(resume);
                self.masm.bind(from_runtime);
            }
            let this_local = self.stub_local(local::THIS);
            self.masm.load64(registers[2], &this_local);
            self.masm.move64(registers[1], function_register);
            self.masm.move64(registers[0], vm);
            self.masm.call_absolute(layout.prepare_call_environment);
            let [environment, this] = [M::RETURN_GPRS[0], M::RETURN_GPRS[1]];
            self.masm.branch64_imm(Condition::Equal, environment, 0, slow);
            self.masm.store64(&environment_local, environment);
            let prepared_this = self.stub_local(local::PREPARED_THIS);
            self.masm.store64(&prepared_this, this);
            // The runtime may have collected garbage: load the function and
            // what comes from it again.
            let callee_local = self.stub_local(local::CALLEE);
            self.masm.load64(temp, &callee_local);
            self.emit_unbox_cell(function_register, temp);
            load_function_data(self);
        } else {
            self.masm.branch_test64(
                Condition::NonZero,
                receiver,
                layout.metadata_needs_environment_or_this_value_resolution,
                slow,
            );
        }
        self.masm.bind(resume);

        // The number of argument slots: the formal parameter count (in the
        // low bits of the metadata), or more if more were passed.
        let formal_count = other;
        let argument_count_local = self.stub_local(local::ARGUMENT_COUNT);
        self.masm.move32(formal_count, receiver);
        self.masm.load64(temp, &argument_count_local);
        let enough = self.masm.new_label();
        self.masm.branch32(Condition::AboveOrEqual, formal_count, temp, enough);
        self.masm.move32(formal_count, temp);
        self.masm.bind(enough);

        // Bind `this` like the interpreter's call fast path: as is for strict
        // callees, objects only for sloppy ones (the generic call boxes
        // primitives and resolves null and undefined), and as the runtime
        // prepared it for callees with an environment.
        let have_receiver = self.masm.new_label();
        let own_environment = self.masm.new_label();
        self.masm
            .branch64_memory_imm(Condition::Equal, &environment_local, 0, own_environment);
        let prepared_this = self.stub_local(local::PREPARED_THIS);
        self.masm.load64(receiver, &prepared_this);
        self.masm.jump(have_receiver);
        self.masm.bind(own_environment);
        let uses_this = self.masm.new_label();
        self.masm
            .branch_test64(Condition::NonZero, receiver, layout.metadata_uses_this, uses_this);
        self.masm.move_imm64(receiver, value::EMPTY);
        self.masm.jump(have_receiver);
        self.masm.bind(uses_this);
        let strict = self.masm.new_label();
        self.masm
            .branch_test64(Condition::NonZero, receiver, layout.metadata_strict, strict);
        let this_local = self.stub_local(local::THIS);
        self.masm.load64(receiver, &this_local);
        self.branch_on_tag(Condition::NotEqual, receiver, value::OBJECT_TAG, temp, slow);
        self.masm.jump(have_receiver);
        self.masm.bind(strict);
        self.masm.load64(receiver, &this_local);
        self.masm.bind(have_receiver);

        self.emit_native_stack_check(slow);

        // The frame's slot count, and its allocation on the interpreter stack.
        self.masm.load32(
            temp,
            &Address::new(
                executable_register,
                layout.executable_registers_and_locals_and_constants_count as i32,
            ),
        );
        self.masm.branch_add32_overflow(temp, temp, formal_count, slow);
        self.masm
            .load64(callee_frame, &vm_field(offsets.vm_interpreter_stack_top));
        self.masm.load_effective_address(
            scratch,
            &Address {
                base: callee_frame,
                index: Some((temp, Scale::Eight)),
                displacement: offsets.execution_context_slots as i32,
            },
        );
        self.masm.branch64_memory(
            Condition::Below,
            &vm_field(offsets.vm_interpreter_stack_limit),
            scratch,
            slow,
        );
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), scratch);

        // Fill in the frame like the interpreter's call fast path.
        let field = |offset: u32| Address::new(callee_frame, offset as i32);
        self.emit_function_fields(callee_frame, function_register, scratch);
        let prepared_environment = self.masm.new_label();
        self.masm.load64(scratch, &environment_local);
        self.masm
            .branch64_imm(Condition::Equal, scratch, 0, prepared_environment);
        self.masm
            .store64(&field(offsets.execution_context_lexical_environment), scratch);
        self.masm
            .store64(&field(offsets.execution_context_variable_environment), scratch);
        self.masm.bind(prepared_environment);
        self.masm
            .load64(scratch, &Address::new(function_register, offsets.object_shape as i32));
        self.masm
            .load64(scratch, &Address::new(scratch, layout.shape_realm as i32));
        self.masm.store64(&field(offsets.execution_context_realm), scratch);
        self.masm
            .store64(&field(offsets.execution_context_executable), executable_register);
        self.masm.store32(&field(offsets.execution_context_slot_count), temp);
        self.masm
            .store32(&field(offsets.execution_context_argument_count), formal_count);
        self.masm
            .store64(&field(offsets.execution_context_this_value), receiver);
        self.masm.store64(&field(slot(THIS_VALUE_REGISTER)), receiver);
        let mut constants = self.emit_stub_call_site_fields(callee_frame, scratch, true);
        for register in (0..RESERVED_REGISTER_COUNT).filter(|register| *register != THIS_VALUE_REGISTER) {
            constants.push((slot(register), 8, value::EMPTY));
        }
        self.emit_constant_fields_with_vector(callee_frame, scratch, &mut constants, Some(M::ARGUMENT_FPRS[0]), &[])?;

        // The arguments start after the registers, locals and constants: slot
        // count minus formal count. Copy the passed ones and fill the formal
        // parameters not passed with undefined.
        self.masm.sub32(temp, temp, formal_count);
        self.masm.load_effective_address(
            scratch,
            &Address {
                base: callee_frame,
                index: Some((temp, Scale::Eight)),
                displacement: offsets.execution_context_slots as i32,
            },
        );
        let executable_local = self.stub_local(local::EXECUTABLE);
        self.masm.store64(&executable_local, executable_register);
        self.masm.load64(temp, &argument_count_local);
        self.emit_stub_copy_arguments(scratch, temp, executable_register, receiver);
        // The formal parameters not passed, up to formal_count.
        self.masm.sub32(formal_count, formal_count, temp);
        let fill = self.masm.new_label();
        let filled = self.masm.new_label();
        self.masm.branch32_imm(Condition::Equal, formal_count, 0, filled);
        self.masm.move_imm64(receiver, value::UNDEFINED);
        self.masm.bind(fill);
        self.masm.store64(
            &Address {
                base: scratch,
                index: Some((temp, Scale::Eight)),
                displacement: 0,
            },
            receiver,
        );
        self.masm.add32_imm(temp, temp, 1);
        self.masm.sub32_imm(formal_count, formal_count, 1);
        self.masm.branch32_imm(Condition::NotEqual, formal_count, 0, fill);
        self.masm.bind(filled);
        self.masm.load64(other, &executable_local);

        // Enter the callee through its executable's entry in the JIT entry
        // table, with its frame (in callee_frame) as the second argument: its
        // JIT code, which makes the frame the running one when something else
        // may see it (see `Op::PublishFrame`), or else (also while a debugger
        // is attached) an entry that has the interpreter run the frame. The
        // frame's address replaces the executable in its local. A slot that
        // does not belong to the executable enters through slot 0, which has
        // the interpreter run the frame.
        self.masm.store64(&executable_local, callee_frame);
        let (entry_slot, table) = (temp, registers[0]);
        self.masm.load32(
            entry_slot,
            &Address::new(other, layout.executable_jit_entry_slot as i32),
        );
        self.masm.and32_imm(entry_slot, entry_slot, layout.jit_entry_slot_mask);
        self.masm.move_imm64(table, layout.jit_entry_table);
        let owned = self.masm.new_label();
        self.masm.branch64_memory(
            Condition::Equal,
            &Address {
                base: table,
                index: Some((entry_slot, Scale::Eight)),
                displacement: layout.jit_entry_table_owners as i32,
            },
            other,
            owned,
        );
        self.masm.move_imm32(entry_slot, 0);
        self.masm.bind(owned);
        let entry = receiver;
        self.masm.load64(
            entry,
            &Address {
                base: table,
                index: Some((entry_slot, Scale::Eight)),
                displacement: 0,
            },
        );
        self.masm.move64(registers[0], vm);
        self.masm.call_register(entry);

        // The callee returned: pop its frame like the interpreter's Return.
        let [value_register, status] = [M::RETURN_GPRS[0], M::RETURN_GPRS[1]];
        let not_returned = self.masm.new_label();
        self.masm
            .branch64_imm(Condition::NotEqual, status, JitStatus::Returned as i64, not_returned);
        // NB: JIT code never returns the empty value.
        self.emit_stub_store_result(value_register, temp);
        self.masm.load64(temp, &executable_local);
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), temp);
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), frame);
        self.masm
            .add32_to_memory_imm(&vm_field(offsets.vm_execution_generation), 1);
        self.emit_stub_continuation();
        self.masm.jump(leave);

        // The callee's frame still runs: let the runtime finish the call.
        self.masm.bind(not_returned);
        self.masm.move64(registers[3], status);
        self.masm.move64(registers[0], vm);
        self.masm.move64(registers[1], frame);
        let pcs_local = self.stub_local(local::PCS);
        self.masm.load32(registers[2], &pcs_local);
        self.masm.call_absolute(runtime.finish_direct_call);
        self.masm.jump(leave);

        // Everything else takes the generic path.
        self.masm.bind(slow);
        self.masm.move64(registers[0], vm);
        self.masm.move64(registers[1], frame);
        self.masm.load32(registers[2], &pcs_local);
        self.masm.call_absolute(runtime.jit_call);

        self.masm.bind(leave);
        self.masm.emit_epilogue(&machine_frame);
        self.masm.ret();
        Ok(())
    }

    /// Stores the fields of a new frame at `new_frame` that describe the call
    /// site (from the stub's locals): its passed argument count, return pc
    /// and destination. Returns the constant fields of a frame running no
    /// code yet, for the caller to store with its other constants. Clobbers
    /// `temp`.
    fn emit_stub_call_site_fields(
        &mut self,
        new_frame: crate::asm::Gpr,
        temp: crate::asm::Gpr,
        ecmascript: bool,
    ) -> Vec<super::call::ConstantField> {
        let offsets = self.runtime.offsets;
        let field = |offset: u32| Address::new(new_frame, offset as i32);
        let argument_count_local = self.stub_local(local::ARGUMENT_COUNT);
        self.masm.load32(temp, &argument_count_local);
        self.masm
            .store32(&field(offsets.execution_context_passed_argument_count), temp);
        let pcs_local = Address::new(M::STACK_POINTER, self.stub_local(local::PCS).displacement + 4);
        self.masm.load32(temp, &pcs_local);
        self.masm
            .store32(&field(offsets.execution_context_caller_return_pc), temp);
        let dst_local = self.stub_local(local::DST);
        self.masm.load32(temp, &dst_local);
        self.masm
            .store32(&field(offsets.execution_context_caller_dst_raw), temp);
        self.new_frame_constants(ecmascript)
    }

    /// Copies the `count` arguments the stub's caller stored to
    /// `destination` (an address of the new frame). Clobbers `index` and
    /// `value_register`.
    fn emit_stub_copy_arguments(
        &mut self,
        destination: crate::asm::Gpr,
        count: crate::asm::Gpr,
        index: crate::asm::Gpr,
        value_register: crate::asm::Gpr,
    ) {
        self.masm.move_imm32(index, 0);
        let copy = self.masm.new_label();
        let copied = self.masm.new_label();
        self.masm.branch32(Condition::Equal, index, count, copied);
        self.masm.bind(copy);
        let source = self.stub_argument(index);
        self.masm.load64(value_register, &source);
        self.masm.store64(
            &Address {
                base: destination,
                index: Some((index, Scale::Eight)),
                displacement: 0,
            },
            value_register,
        );
        self.masm.add32_imm(index, index, 1);
        self.masm.branch32(Condition::NotEqual, index, count, copy);
        self.masm.bind(copied);
    }

    /// Stores the call's result into the calling frame's destination slot.
    /// Clobbers `temp`.
    fn emit_stub_store_result(&mut self, result: crate::asm::Gpr, temp: crate::asm::Gpr) {
        let dst_local = self.stub_local(local::DST);
        self.masm.load32(temp, &dst_local);
        self.masm.store64(
            &Address {
                base: self.pinned.frame,
                index: Some((temp, Scale::Eight)),
                displacement: self.runtime.offsets.execution_context_slots as i32,
            },
            result,
        );
        // Stack traces show the caller at its next instruction from now on.
        let next_pc = Address::new(M::STACK_POINTER, self.stub_local(local::PCS).displacement + 4);
        self.masm.load32(temp, &next_pc);
        self.masm.store32(
            &Address::new(
                self.pinned.frame,
                self.runtime.offsets.execution_context_program_counter as i32,
            ),
            temp,
        );
    }

    /// Puts a continuation at the call's next pc in the first return
    /// register.
    fn emit_stub_continuation(&mut self) {
        let control = M::RETURN_GPRS[0];
        let next_pc = Address::new(M::STACK_POINTER, self.stub_local(local::PCS).displacement + 4);
        self.masm.load32(control, &next_pc);
        self.masm.or64_imm(control, control, super::CONTINUATION_BIT);
    }

    /// Calls the raw native function in the function register (registers[4])
    /// in its lightweight frame, like `emit_native_frame_call()` does for the
    /// stub's dynamic argument count. Jumps to `slow` before anything
    /// observable happened if the stacks are too full, and to `leave` with a
    /// control word in the first return register otherwise.
    fn emit_stub_native_call(&mut self, slow: Label, leave: Label) -> Result<(), CompileFailure> {
        let runtime = self.runtime;
        let offsets = runtime.offsets;
        let layout = runtime.dynamic_calls;
        let registers = M::ARGUMENT_GPRS;
        let native_frame = registers[1];
        let temp = registers[2];
        let other = registers[3];
        let function_register = registers[4];
        let count = registers[5];
        let scratch = self.pinned.scratch;
        let vm = self.pinned.vm;
        let frame = self.pinned.frame;
        let vm_field = |offset: u32| Address::new(vm, offset as i32);
        let frame_field = |offset: u32| Address::new(frame, offset as i32);
        let field = |offset: u32| Address::new(native_frame, offset as i32);

        self.emit_native_stack_check(slow);

        // Allocate the frame on the interpreter stack: the header and one
        // slot per argument.
        let argument_count_local = self.stub_local(local::ARGUMENT_COUNT);
        self.masm.load64(count, &argument_count_local);
        self.masm
            .load64(native_frame, &vm_field(offsets.vm_interpreter_stack_top));
        self.masm.load_effective_address(
            temp,
            &Address {
                base: native_frame,
                index: Some((count, Scale::Eight)),
                displacement: offsets.execution_context_slots as i32,
            },
        );
        self.masm.load64(other, &vm_field(offsets.vm_interpreter_stack_limit));
        self.masm.branch64(Condition::Above, temp, other, slow);
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), temp);

        // Fill in the frame like the interpreter's call fast path.
        for offset in [
            offsets.execution_context_lexical_environment,
            offsets.execution_context_variable_environment,
            offsets.execution_context_private_environment,
        ] {
            self.masm.load64(temp, &frame_field(offset));
            self.masm.store64(&field(offset), temp);
        }
        let this_local = self.stub_local(local::THIS);
        self.masm.load64(temp, &this_local);
        self.masm.store64(&field(offsets.execution_context_this_value), temp);
        // NB: Frames get their id when the debugger first needs it.
        self.masm.store_imm64(&field(offsets.execution_context_frame_id), 0);
        self.masm.store64(&field(offsets.execution_context_caller_frame), frame);
        self.masm
            .store64(&field(offsets.execution_context_function), function_register);
        self.masm
            .load64(temp, &Address::new(function_register, offsets.object_shape as i32));
        self.masm.load64(temp, &Address::new(temp, layout.shape_realm as i32));
        self.masm.store64(&field(offsets.execution_context_realm), temp);
        self.masm.store32(&field(offsets.execution_context_slot_count), count);
        self.masm
            .store32(&field(offsets.execution_context_argument_count), count);
        let mut constants = self.emit_stub_call_site_fields(native_frame, temp, false);
        constants.extend([
            (offsets.execution_context_script_or_module, 8, 0),
            (offsets.execution_context_script_or_module + 8, 8, 0),
            (offsets.execution_context_executable, 8, 0),
        ]);
        self.emit_constant_fields_with_vector(native_frame, temp, &mut constants, Some(M::ARGUMENT_FPRS[0]), &[])?;
        self.masm.load_effective_address(
            other,
            &Address::new(native_frame, offsets.execution_context_slots as i32),
        );
        self.emit_stub_copy_arguments(other, count, registers[0], temp);
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), native_frame);

        // Call the native function: it returns a ThrowCompletionOr<Value>,
        // the value or exception in the first return register and whether it
        // is an exception in the low byte of the second.
        self.masm.load32(
            temp,
            &Address::new(function_register, layout.raw_native_function_index as i32),
        );
        self.masm.and32_imm(temp, temp, layout.native_function_table_index_mask);
        self.masm.shl64_imm(
            temp,
            temp,
            u8::try_from(layout.native_function_table_entry_size.trailing_zeros()).unwrap_or(0),
        );
        self.masm.load64(scratch, &vm_field(layout.vm_native_function_table));
        self.masm.load64(
            scratch,
            &Address {
                base: scratch,
                index: Some((temp, Scale::One)),
                displacement: layout.native_function_table_entry_function as i32,
            },
        );
        self.masm.move64(registers[0], vm);
        self.masm.call_register(scratch);
        let [value_register, variant] = [M::RETURN_GPRS[0], M::RETURN_GPRS[1]];
        let threw = self.masm.new_label();
        self.masm.branch_test32(Condition::NonZero, variant, 0xFF, threw);
        // Pop the frame, which is the running one, like the interpreter does.
        self.masm.load64(temp, &vm_field(offsets.vm_running_execution_context));
        self.masm.store64(&vm_field(offsets.vm_interpreter_stack_top), temp);
        self.masm
            .store64(&vm_field(offsets.vm_running_execution_context), frame);
        self.emit_stub_store_result(value_register, temp);
        self.emit_stub_continuation();
        self.masm.jump(leave);

        // The runtime unwinds the frame of a native function that threw.
        self.masm.bind(threw);
        self.masm.move64(registers[1], value_register);
        self.masm.move64(registers[0], self.pinned.vm);
        self.masm.call_absolute(runtime.raw_native_exception);
        self.masm.jump(leave);
        Ok(())
    }
}
