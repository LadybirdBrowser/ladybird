/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::{CraneliftInsn, RuntimeHelpers};

use cranelift_codegen::ir::condcodes::{FloatCC, IntCC};
use cranelift_codegen::ir::types;
use cranelift_codegen::ir::{
    AbiParam, Function, InstBuilder, MemFlags, Signature, StackSlotData, StackSlotKind, UserFuncName,
};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_codegen::{self, Context};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Variable};
use cranelift_native;

// Opcode constants generated from Opcode.h (see build.rs.)
#[allow(dead_code)]
mod op {
    include!(concat!(env!("OUT_DIR"), "/opcodes.rs"));
}

const REG_COUNT: usize = 8;
const STACK_MARKER: u8 = 8;
const CALLREC_BASE: u8 = 9;

/// Control flow frame tracking for structured control flow.
struct ControlFrame {
    kind: ControlKind,
    /// The cranelift block to branch to (for `br` targeting this frame).
    /// For blocks: the merge/after block.
    /// For loops: the loop header block.
    branch_target: cranelift_codegen::ir::Block,
    /// Where execution continues after this construct ends.
    after_block: cranelift_codegen::ir::Block,
    /// Number of result values.
    arity: u32,
    /// Number of parameters consumed from outer stack.
    param_count: usize,
    /// Stack depth at block entry, tracked for br*.
    stack_depth_at_entry: i32,
    /// Real value-stack size at block entry, minus this block's param count.
    /// Only meaningful (and only set) when vstack is disabled (max_stack_depth == 0).
    entry_real_depth_var: Option<Variable>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ControlKind {
    Block,
    Loop,
    If,
}

pub struct CraneliftCompiler;

impl CraneliftCompiler {
    pub fn compile_to_bytes(
        insns: &[CraneliftInsn],
        helpers: &RuntimeHelpers,
        outcome_return_value: u64,
        result_arity: u32,
    ) -> Result<Vec<u8>, &'static str> {
        for insn in insns {
            if !Self::is_supported(insn) {
                return Err("unsupported instruction");
            }
            if matches!(insn.opcode, op::BLOCK | op::LOOP | op::IF) {
                let arity = insn.imm3 & 0xffff;
                if arity > 1 {
                    return Err("multi-value blocks not supported");
                }
            }
            // Note: op::CALL is used for multi-value returns but also for some
            // single-return calls. We handle it via flush_vstack_to_real before the call.
        }

        let mut flag_builder = settings::builder();
        flag_builder.set("opt_level", "speed").unwrap();
        flag_builder.set("is_pic", "false").unwrap();
        let flags = settings::Flags::new(flag_builder);
        let isa = cranelift_native::builder()
            .map_err(|_| "unsupported host architecture")?
            .finish(flags)
            .map_err(|_| "failed to build ISA")?;

        // Function signature matches handler_ptr:
        //   u64 fn(void* interpreter, void* configuration, void* insn, u32 short_ip, void* cc, void* addrs)
        let ptr_type = isa.pointer_type();
        let host_cc = isa.default_call_conv();
        let mut sig = Signature::new(host_cc);
        sig.params.push(AbiParam::new(ptr_type)); // interpreter
        sig.params.push(AbiParam::new(ptr_type)); // configuration
        sig.params.push(AbiParam::new(ptr_type)); // instruction (unused)
        sig.params.push(AbiParam::new(types::I32)); // short_ip (unused)
        sig.params.push(AbiParam::new(ptr_type)); // cc (unused)
        sig.params.push(AbiParam::new(ptr_type)); // addresses_ptr (unused)
        sig.returns.push(AbiParam::new(types::I64)); // Outcome

        let mut func = Function::with_name_signature(UserFuncName::user(0, 0), sig);
        let mut builder_ctx = FunctionBuilderContext::new();
        let mut builder = FunctionBuilder::new(&mut func, &mut builder_ctx);

        // Declare variables for virtual registers R0-R7.
        // We store everything as i64 and bitcast for floats.
        let reg_vars: [Variable; REG_COUNT] = std::array::from_fn(|i| Variable::from_u32(i as u32));
        for var in &reg_vars {
            builder.declare_var(*var, types::I64);
        }

        let entry_block = builder.create_block();
        builder.append_block_params_for_function_params(entry_block);
        builder.switch_to_block(entry_block);
        builder.seal_block(entry_block);

        let interpreter_val = builder.block_params(entry_block)[0];
        let configuration_val = builder.block_params(entry_block)[1];

        // Load regs[0..7] from configuration. regs is at offset `regs_offset` from Configuration*.
        // Each Value is `value_size` bytes; the low 8 bytes are the i64 payload.
        let regs_offset = helpers.regs_offset as i32;
        let value_size = helpers.value_size as i32;
        for (i, var) in reg_vars.iter().enumerate() {
            let offset = regs_offset + (i as i32) * value_size;
            let val = builder
                .ins()
                .load(types::I64, MemFlags::trusted(), configuration_val, offset);
            builder.def_var(*var, val);
        }

        let epilogue_block = builder.create_block();
        let trap_block = builder.create_block();

        // Build helper call signatures. We import them as indirect calls via function pointers.
        macro_rules! sig {
            (@ty ptr) => { ptr_type };
            (@ty i32) => { types::I32 };
            (@ty i64) => { types::I64 };
            (@def $name:ident : void fn($($param:ident),*)) => {
                let $name = {
                    let mut s = Signature::new(host_cc);
                    $(s.params.push(AbiParam::new(sig!(@ty $param)));)*
                    builder.import_signature(s)
                };
            };
            (@def $name:ident : $ret:ident fn($($param:ident),*)) => {
                let $name = {
                    let mut s = Signature::new(host_cc);
                    $(s.params.push(AbiParam::new(sig!(@ty $param)));)*
                    s.returns.push(AbiParam::new(sig!(@ty $ret)));
                    builder.import_signature(s)
                };
            };
            ($($name:ident : $ret:ident fn($($param:ident),*);)*) => { $(sig!(@def $name : $ret fn($($param),*));)* }
        }

        sig! {
            call_fn_sig:       i32 fn(ptr, ptr, i32);
            call_fn1_sig:      i32 fn(ptr, ptr, i32, i64);
            call_fn2_sig:      i32 fn(ptr, ptr, i32, i64, i64);
            call_fn3_sig:      i32 fn(ptr, ptr, i32, i64, i64, i64);
            call_indirect_sig: i32 fn(ptr, ptr, i32, i32, i32);
            memory_copy_sig:   i32 fn(ptr, ptr, i32, i32, i32, i32, i32);
            memory_fill_sig:   i32 fn(ptr, ptr, i32, i32, i32, i32);
            mem_load_sig:      i64 fn(ptr, i32, i64);
            mem_store_sig:     i32 fn(ptr, i32, i64, i64);
            mem_size_sig:      i64 fn(ptr, i32);
            mem_grow_sig:      i32 fn(ptr, i32, i32);
            read_global_sig:   i64 fn(ptr, i32);
            stack_pop_sig:     i64 fn(ptr);
            stack_size_sig:    i64 fn(ptr);
            callrec_read_sig:  i64 fn(ptr, i32);
            call_wr_sig:       i32 fn(ptr, ptr, i32);
            set_trap_sig:      void fn(ptr, ptr, i32);
            write_global_sig:  void fn(ptr, i32, i64);
            stack_push_sig:    void fn(ptr, i64);
            stack_cleanup_sig: void fn(ptr, i64, i32);
            callrec_write_sig: void fn(ptr, i32, i64);
        }

        // Helper function pointers, rematerialized as iconst at each use site.
        macro_rules! h {
            ($($name:ident = helpers.$field:ident;)*) => { $(let $name = helpers.$field as i64;)* };
        }
        h! {
            h_call_fn       = helpers.call_function;
            h_direct_call_0 = helpers.direct_call_0;
            h_direct_call_1 = helpers.direct_call_1;
            h_direct_call_2 = helpers.direct_call_2;
            h_direct_call_3 = helpers.direct_call_3;
            h_set_trap      = helpers.set_trap;
            h_mem_load8_s   = helpers.memory_load8_s;
            h_mem_load8_u   = helpers.memory_load8_u;
            h_mem_load16_s  = helpers.memory_load16_s;
            h_mem_load16_u  = helpers.memory_load16_u;
            h_mem_load32_s  = helpers.memory_load32_s;
            h_mem_load32_u  = helpers.memory_load32_u;
            h_mem_load64    = helpers.memory_load64;
            h_mem_store8    = helpers.memory_store8;
            h_mem_store16   = helpers.memory_store16;
            h_mem_store32   = helpers.memory_store32;
            h_mem_store64   = helpers.memory_store64;
            h_mem_size      = helpers.memory_size;
            h_mem_grow      = helpers.memory_grow;
            h_read_global   = helpers.read_global;
            h_write_global  = helpers.write_global;
            h_stack_push    = helpers.stack_push;
            h_stack_pop     = helpers.stack_pop;
            h_stack_size    = helpers.stack_size;
            h_stack_cleanup = helpers.stack_cleanup;
            h_callrec_read  = helpers.callrec_read;
            h_callrec_write = helpers.callrec_write;
            h_call_wr       = helpers.call_with_record;
            h_call_indirect = helpers.call_indirect;
            h_memory_copy   = helpers.memory_copy;
            h_memory_fill   = helpers.memory_fill;
        }
        let locals_base_offset = helpers.locals_base_offset as i32;
        let default_memory_base_offset = helpers.default_memory_base_offset as i32;
        let compiled_call_result_scratch_offset = helpers.compiled_call_result_scratch_offset as i32;
        let interp_var = Variable::from_u32(8);
        builder.declare_var(interp_var, ptr_type);
        builder.def_var(interp_var, interpreter_val);
        let config_var = Variable::from_u32(9);
        builder.declare_var(config_var, ptr_type);
        builder.def_var(config_var, configuration_val);
        let locals_base_var = Variable::from_u32(10);
        builder.declare_var(locals_base_var, ptr_type);
        let initial_locals_base =
            builder
                .ins()
                .load(ptr_type, MemFlags::trusted(), configuration_val, locals_base_offset);
        builder.def_var(locals_base_var, initial_locals_base);
        let default_memory_base_var = Variable::from_u32(11);
        builder.declare_var(default_memory_base_var, ptr_type);
        let initial_default_memory_base = builder.ins().load(
            ptr_type,
            MemFlags::trusted(),
            configuration_val,
            default_memory_base_offset,
        );
        builder.def_var(default_memory_base_var, initial_default_memory_base);

        let mut control_stack: Vec<ControlFrame> = Vec::new();

        // Virtual stack, to avoid touching the interpreter-side stack as much as possible.
        let has_raw_call = insns
            .iter()
            .any(|i| i.opcode == op::CALL || i.opcode == op::CALL_INDIRECT);
        let max_stack_depth = match has_raw_call {
            true => 0,
            // We can't easily track across control flow merges, so count dests instead.
            false => insns.iter().filter(|i| i.destination == STACK_MARKER).count().max(16),
        };
        let mut is_unreachable = false;
        let mut dirty_regs = [false; REG_COUNT];
        let mut stack_vars: Vec<Variable> = Vec::with_capacity(max_stack_depth);
        const VSTACK_VAR_BASE: u32 = 12;

        for i in 0..max_stack_depth {
            let var = Variable::from_u32(VSTACK_VAR_BASE + i as u32);
            builder.declare_var(var, types::I64);
            let zero = builder.ins().iconst(types::I64, 0);
            builder.def_var(var, zero);
            stack_vars.push(var);
        }
        let mut sp: usize = 0;

        // If we allocate something on the stack, make sure to restore the stack at the end.
        let initial_stack_size_var = Variable::from_u32(VSTACK_VAR_BASE + max_stack_depth as u32);
        builder.declare_var(initial_stack_size_var, types::I64);
        let mut next_var_id: u32 = VSTACK_VAR_BASE + max_stack_depth as u32 + 1;

        if has_raw_call {
            let stack_size_fp = builder.ins().iconst(ptr_type, h_stack_size);
            let cfg_for_size = builder.use_var(config_var);
            let stack_size_call = builder
                .ins()
                .call_indirect(stack_size_sig, stack_size_fp, &[cfg_for_size]);
            let initial_stack_size = builder.inst_results(stack_size_call)[0];
            builder.def_var(initial_stack_size_var, initial_stack_size);
        } else {
            let zero = builder.ins().iconst(types::I64, 0);
            builder.def_var(initial_stack_size_var, zero);
        }

        // Read a value from a source location (register, virtual stack, or call record)
        macro_rules! read_src {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    $builder.use_var(reg_vars[src as usize])
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        // we have vstack, so just allocate on the native stack.
                        sp -= 1;
                        $builder.use_var(stack_vars[sp])
                    } else {
                        let fp = $builder.ins().iconst(ptr_type, h_stack_pop);
                        let cfg = $builder.use_var(config_var);
                        let call = $builder.ins().call_indirect(stack_pop_sig, fp, &[cfg]);
                        $builder.inst_results(call)[0]
                    }
                } else {
                    let fp = $builder.ins().iconst(ptr_type, h_callrec_read);
                    let cfg = $builder.use_var(config_var);
                    let idx = $builder.ins().iconst(types::I32, i64::from(src - CALLREC_BASE));
                    let call = $builder.ins().call_indirect(callrec_read_sig, fp, &[cfg, idx]);
                    $builder.inst_results(call)[0]
                }
            }};
        }

        // flush virtual stack slots 0..sp to the real value stack
        macro_rules! flush_vstack_to_real {
            ($builder:expr) => {{
                if max_stack_depth > 0 {
                    for i in 0..sp {
                        let val = $builder.use_var(stack_vars[i]);
                        let fp = $builder.ins().iconst(ptr_type, h_stack_push);
                        let cfg = $builder.use_var(config_var);
                        $builder.ins().call_indirect(stack_push_sig, fp, &[cfg, val]);
                    }
                }
                sp = 0;
            }};
        }
        // push only the top n values from vstack to real stack
        macro_rules! push_top_n_to_real {
            ($builder:expr, $n:expr) => {{
                let n = $n as usize;
                if max_stack_depth > 0 && sp >= n {
                    // vstack enabled: push only top n values to real stack.
                    for i in 0..n {
                        let idx = sp - n + i;
                        let val = $builder.use_var(stack_vars[idx]);
                        let fp = $builder.ins().iconst(ptr_type, h_stack_push);
                        let cfg = $builder.use_var(config_var);
                        $builder.ins().call_indirect(stack_push_sig, fp, &[cfg, val]);
                    }
                }
            }};
        }

        macro_rules! write_dst {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars[dst as usize], val);
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars[sp], val);
                        sp += 1;
                    } else {
                        let fp = $builder.ins().iconst(ptr_type, h_stack_push);
                        let cfg = $builder.use_var(config_var);
                        $builder.ins().call_indirect(stack_push_sig, fp, &[cfg, val]);
                    }
                } else {
                    let fp = $builder.ins().iconst(ptr_type, h_callrec_write);
                    let cfg = $builder.use_var(config_var);
                    let idx = $builder.ins().iconst(types::I32, i64::from(dst - CALLREC_BASE));
                    $builder
                        .ins()
                        .call_indirect(callrec_write_sig, fp, &[cfg, idx, val]);
                }
            }};
        }

        // Note that all reads from sources have to be in order (sources[0] before sources[1])
        macro_rules! i32_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs = $builder.ins().ireduce(types::I32, lhs_raw);
                let rhs = $builder.ins().ireduce(types::I32, rhs_raw);
                let result = $builder.ins().$op(lhs, rhs);
                let result = $builder.ins().sextend(types::I64, result);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs = read_src!($builder, $insn.sources[0]);
                let lhs = read_src!($builder, $insn.sources[1]);
                let result = $builder.ins().$op(lhs, rhs);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i32_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src_raw = read_src!($builder, $insn.sources[0]);
                let src = $builder.ins().ireduce(types::I32, src_raw);
                let result = $builder.ins().$op(src);
                let result = $builder.ins().sextend(types::I64, result);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src = read_src!($builder, $insn.sources[0]);
                let result = $builder.ins().$op(src);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i32_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs = $builder.ins().ireduce(types::I32, lhs_raw);
                let rhs = $builder.ins().ireduce(types::I32, rhs_raw);
                let cmp = $builder.ins().icmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src!($builder, $insn.sources[0]);
                let lhs = read_src!($builder, $insn.sources[1]);
                let cmp = $builder.ins().icmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f32_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs_i32 = $builder.ins().ireduce(types::I32, lhs_raw);
                let rhs_i32 = $builder.ins().ireduce(types::I32, rhs_raw);
                let lhs = $builder.ins().bitcast(types::F32, MemFlags::new(), lhs_i32);
                let rhs = $builder.ins().bitcast(types::F32, MemFlags::new(), rhs_i32);
                let result = $builder.ins().$op(lhs, rhs);
                let result = $builder.ins().bitcast(types::I32, MemFlags::new(), result);
                let result = $builder.ins().sextend(types::I64, result);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs = $builder.ins().bitcast(types::F64, MemFlags::new(), lhs_raw);
                let rhs = $builder.ins().bitcast(types::F64, MemFlags::new(), rhs_raw);
                let result = $builder.ins().$op(lhs, rhs);
                let result = $builder.ins().bitcast(types::I64, MemFlags::new(), result);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f32_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src_raw = read_src!($builder, $insn.sources[0]);
                let src_i32 = $builder.ins().ireduce(types::I32, src_raw);
                let src = $builder.ins().bitcast(types::F32, MemFlags::new(), src_i32);
                let result = $builder.ins().$op(src);
                let result = $builder.ins().bitcast(types::I32, MemFlags::new(), result);
                let result = $builder.ins().sextend(types::I64, result);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src_raw = read_src!($builder, $insn.sources[0]);
                let src = $builder.ins().bitcast(types::F64, MemFlags::new(), src_raw);
                let result = $builder.ins().$op(src);
                let result = $builder.ins().bitcast(types::I64, MemFlags::new(), result);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f32_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs_i32 = $builder.ins().ireduce(types::I32, lhs_raw);
                let rhs_i32 = $builder.ins().ireduce(types::I32, rhs_raw);
                let lhs = $builder.ins().bitcast(types::F32, MemFlags::new(), lhs_i32);
                let rhs = $builder.ins().bitcast(types::F32, MemFlags::new(), rhs_i32);
                let cmp = $builder.ins().fcmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs = $builder.ins().bitcast(types::F64, MemFlags::new(), lhs_raw);
                let rhs = $builder.ins().bitcast(types::F64, MemFlags::new(), rhs_raw);
                let cmp = $builder.ins().fcmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }

        // locals_base is a Value*, we're only interested in the first 8 bytes of *(locals_base + index * 16).
        macro_rules! read_local_inline {
            ($builder:expr, $idx_imm:expr) => {{
                let lb = $builder.use_var(locals_base_var);
                let offset = ($idx_imm as i32) * value_size;
                $builder.ins().load(types::I64, MemFlags::trusted(), lb, offset)
            }};
        }
        // write_local_inline: store i64 to first 8 bytes, zero next 8 bytes
        macro_rules! write_local_inline {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let lb = $builder.use_var(locals_base_var);
                let offset = ($idx_imm as i32) * value_size;
                $builder.ins().store(MemFlags::trusted(), $val, lb, offset);
                let zero = $builder.ins().iconst(types::I64, 0);
                $builder.ins().store(MemFlags::trusted(), zero, lb, offset + 8);
            }};
        }

        // Call + trap-check macro for helper calls that do not consume caller register state.
        // The callee gets arguments explicitly (register immediates, stack, or call record), and the caller's virtual registers stay live in SSA across the call.
        macro_rules! do_call_and_check {
            ($builder:expr, $sig:expr, $ptr:expr, $args:expr) => {{
                let call = $builder.ins().call_indirect($sig, $ptr, $args);
                let trapped = $builder.inst_results(call)[0];
                let is_trap = $builder.ins().icmp_imm(IntCC::NotEqual, trapped, 0);
                let cont = $builder.create_block();
                $builder.ins().brif(is_trap, trap_block, &[], cont, &[]);
                $builder.switch_to_block(cont);
                $builder.seal_block(cont);
                // Reload locals_base (frame_stack may have reallocated) and default_memory_base (callee may have grown memory).
                let _cfg_for_lb = $builder.use_var(config_var);
                let new_lb = $builder.ins().load(ptr_type, MemFlags::trusted(), _cfg_for_lb, locals_base_offset);
                $builder.def_var(locals_base_var, new_lb);
                let new_default_memory_base = $builder.ins().load(ptr_type, MemFlags::trusted(), _cfg_for_lb, default_memory_base_offset);
                $builder.def_var(default_memory_base_var, new_default_memory_base);
            }};
        }

        let mut ip = 0usize;
        while ip < insns.len() {
            let insn = &insns[ip];
            let opc = insn.opcode;

            match opc {
                op::NOP => {}

                op::UNREACHABLE => {
                    Self::sync_regs_to_config(
                        &mut builder,
                        &reg_vars,
                        config_var,
                        regs_offset,
                        value_size,
                        &dirty_regs,
                    );
                    let msg = b"unreachable executed";
                    let ss = builder.create_sized_stack_slot(StackSlotData::new(
                        StackSlotKind::ExplicitSlot,
                        msg.len() as u32,
                        0,
                    ));
                    for (i, &byte) in msg.iter().enumerate() {
                        let b = builder.ins().iconst(types::I8, i64::from(byte));
                        builder.ins().stack_store(b, ss, i as i32);
                    }
                    let msg_ptr = builder.ins().stack_addr(ptr_type, ss, 0);
                    let msg_len = builder.ins().iconst(types::I32, msg.len() as i64);
                    let st_ptr = builder.ins().iconst(ptr_type, h_set_trap);
                    let interp = builder.use_var(interp_var);
                    builder
                        .ins()
                        .call_indirect(set_trap_sig, st_ptr, &[interp, msg_ptr, msg_len]);
                    builder.ins().jump(trap_block, &[]);
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::BLOCK => {
                    let arity = insn.imm3 & 0xffff;
                    let param_count = (insn.imm3 >> 16) as usize;
                    let after = builder.create_block();
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(var, types::I64);
                        let stack_size_fp = builder.ins().iconst(ptr_type, h_stack_size);
                        let cfg = builder.use_var(config_var);
                        let call = builder.ins().call_indirect(stack_size_sig, stack_size_fp, &[cfg]);
                        let cur = builder.inst_results(call)[0];
                        let entry = builder.ins().iadd_imm(cur, -(param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    control_stack.push(ControlFrame {
                        kind: ControlKind::Block,
                        branch_target: after,
                        after_block: after,
                        arity,
                        param_count,
                        stack_depth_at_entry: (sp - param_count) as i32,
                        entry_real_depth_var,
                    });
                }

                op::LOOP => {
                    let arity = insn.imm3 & 0xffff;
                    let param_count = (insn.imm3 >> 16) as usize;
                    let header = builder.create_block();
                    let after = builder.create_block();
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(var, types::I64);
                        let stack_size_fp = builder.ins().iconst(ptr_type, h_stack_size);
                        let cfg = builder.use_var(config_var);
                        let call = builder.ins().call_indirect(stack_size_sig, stack_size_fp, &[cfg]);
                        let cur = builder.inst_results(call)[0];
                        let entry = builder.ins().iadd_imm(cur, -(param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    builder.ins().jump(header, &[]);
                    builder.switch_to_block(header);
                    control_stack.push(ControlFrame {
                        kind: ControlKind::Loop,
                        branch_target: header,
                        after_block: after,
                        arity,
                        param_count,
                        stack_depth_at_entry: (sp - param_count) as i32,
                        entry_real_depth_var,
                    });
                }

                op::IF => {
                    let arity = insn.imm3 & 0xffff;
                    let _param_count = (insn.imm3 >> 16) as usize;
                    let has_else = insn.imm2 >= 0;
                    let then_block = builder.create_block();
                    let else_block = builder.create_block();
                    let after = builder.create_block();

                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm(IntCC::NotEqual, cond_raw, 0);
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(var, types::I64);
                        let stack_size_fp = builder.ins().iconst(ptr_type, h_stack_size);
                        let cfg = builder.use_var(config_var);
                        let call = builder.ins().call_indirect(stack_size_sig, stack_size_fp, &[cfg]);
                        let cur = builder.inst_results(call)[0];
                        let entry = builder.ins().iadd_imm(cur, -(_param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    if has_else {
                        builder.ins().brif(cond, then_block, &[], else_block, &[]);
                    } else {
                        builder.ins().brif(cond, then_block, &[], after, &[]);
                    }

                    builder.switch_to_block(then_block);
                    builder.seal_block(then_block);

                    control_stack.push(ControlFrame {
                        kind: ControlKind::If,
                        branch_target: after,
                        after_block: if has_else { else_block } else { after },
                        arity,
                        param_count: _param_count,
                        stack_depth_at_entry: (sp - _param_count) as i32,
                        entry_real_depth_var,
                    });
                }

                op::ELSE => {
                    if let Some(frame) = control_stack.last() {
                        let else_block = frame.after_block;
                        let after = frame.branch_target;
                        let entry_depth = frame.stack_depth_at_entry;
                        let pc = frame.param_count;
                        builder.ins().jump(after, &[]);
                        builder.switch_to_block(else_block);
                        builder.seal_block(else_block);
                        // Reset sp to entry depth + param_count (else branch inherits params).
                        sp = (entry_depth as usize) + pc;
                        if let Some(frame) = control_stack.last_mut() {
                            frame.after_block = after;
                        }
                    }
                }

                op::END | op::SYNTHETIC_END_EXPRESSION => {
                    if let Some(frame) = control_stack.pop() {
                        let after = if frame.kind == ControlKind::If && frame.after_block != frame.branch_target {
                            // If without else: the after_block is the branch_target.
                            frame.branch_target
                        } else {
                            frame.after_block
                        };

                        builder.ins().jump(after, &[]);
                        builder.switch_to_block(after);
                        is_unreachable = false;

                        // After end of block, sp = entry depth + arity.
                        sp = (frame.stack_depth_at_entry + frame.arity as i32) as usize;

                        if frame.kind == ControlKind::Loop {
                            builder.seal_block(frame.branch_target); // loop header
                        }
                        builder.seal_block(after);
                    } else if !is_unreachable {
                        push_top_n_to_real!(builder, result_arity);
                        builder.ins().jump(epilogue_block, &[]);
                        let dead = builder.create_block();
                        builder.switch_to_block(dead);
                        builder.seal_block(dead);
                    }
                }

                op::BR | op::SYNTHETIC_BR_NOSTACK => {
                    let label_idx = insn.imm1 as usize;
                    if label_idx < control_stack.len() {
                        let target_idx = control_stack.len() - 1 - label_idx;
                        let frame = &control_stack[target_idx];
                        let target = frame.branch_target;
                        let arity = if frame.kind == ControlKind::Loop {
                            0
                        } else {
                            frame.arity
                        };
                        let entry = frame.stack_depth_at_entry as usize;
                        if max_stack_depth > 0 {
                            // vstack enabled: move top arity values to entry position.
                            if arity > 0 {
                                let result = if sp > 0 {
                                    builder.use_var(stack_vars[sp - 1])
                                } else {
                                    let fp = builder.ins().iconst(ptr_type, h_stack_pop);
                                    let cfg = builder.use_var(config_var);
                                    let call = builder.ins().call_indirect(stack_pop_sig, fp, &[cfg]);
                                    builder.inst_results(call)[0]
                                };
                                builder.def_var(stack_vars[entry], result);
                            }
                        } else {
                            // vstack disabled: trim the real value stack down to the target label's entry depth + arity, preserving the top arity values.
                            let entry_depth_var = frame
                                .entry_real_depth_var
                                .expect("entry_real_depth_var must be set when vstack is disabled");
                            let target_size = builder.use_var(entry_depth_var);
                            let arity_val = builder.ins().iconst(types::I32, arity as i64);
                            let cfg = builder.use_var(config_var);
                            let cleanup_fp = builder.ins().iconst(ptr_type, h_stack_cleanup);
                            builder
                                .ins()
                                .call_indirect(stack_cleanup_sig, cleanup_fp, &[cfg, target_size, arity_val]);
                        }
                        builder.ins().jump(target, &[]);
                    } else {
                        // br to function label = return.
                        push_top_n_to_real!(builder, result_arity);
                        builder.ins().jump(epilogue_block, &[]);
                    }
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::BR_IF | op::SYNTHETIC_BR_IF_NOSTACK => {
                    let label_idx = insn.imm1 as usize;
                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm(IntCC::NotEqual, cond_raw, 0);

                    if label_idx < control_stack.len() {
                        let target_idx = control_stack.len() - 1 - label_idx;
                        let frame = &control_stack[target_idx];
                        let target = frame.branch_target;
                        let arity = if frame.kind == ControlKind::Loop {
                            0
                        } else {
                            frame.arity
                        };
                        let entry = frame.stack_depth_at_entry as usize;
                        let extras = (sp as i32 - entry as i32 - arity as i32).max(0);
                        if max_stack_depth == 0 {
                            // not vstack: real value stack may have extras between the target label's entry depth and the result on top.
                            // On the taken path, call stack_cleanup using the saved entry-depth variable.
                            let entry_depth_var = frame
                                .entry_real_depth_var
                                .expect("entry_real_depth_var must be set when vstack is disabled");
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            let target_size = builder.use_var(entry_depth_var);
                            let arity_val = builder.ins().iconst(types::I32, arity as i64);
                            let cfg = builder.use_var(config_var);
                            let cleanup_fp = builder.ins().iconst(ptr_type, h_stack_cleanup);
                            builder
                                .ins()
                                .call_indirect(stack_cleanup_sig, cleanup_fp, &[cfg, target_size, arity_val]);
                            builder.ins().jump(target, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else if extras > 0 {
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            if arity > 0 {
                                let result = builder.use_var(stack_vars[sp - 1]);
                                builder.def_var(stack_vars[entry], result);
                            }
                            // Note: we don't change sp here since fallthrough needs the original sp.
                            builder.ins().jump(target, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else {
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, target, &[], fallthrough, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        }
                    } else {
                        if sp > 0 {
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            push_top_n_to_real!(builder, result_arity);
                            builder.ins().jump(epilogue_block, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else {
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, epilogue_block, &[], fallthrough, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        }
                    }
                }

                op::RETURN => {
                    push_top_n_to_real!(builder, result_arity);
                    builder.ins().jump(epilogue_block, &[]);
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::I32_CONST | op::I64_CONST | op::F32_CONST | op::F64_CONST => {
                    let val = builder.ins().iconst(types::I64, insn.imm1);
                    write_dst!(builder, insn.destination, val);
                }

                op::LOCAL_GET | op::SYNTHETIC_ARGUMENT_GET => {
                    let result = read_local_inline!(builder, insn.imm1);
                    write_dst!(builder, insn.destination, result);
                }
                op::LOCAL_SET | op::SYNTHETIC_ARGUMENT_SET => {
                    let val = read_src!(builder, insn.sources[0]);
                    write_local_inline!(builder, insn.imm1, val);
                }
                op::LOCAL_TEE | op::SYNTHETIC_ARGUMENT_TEE => {
                    let val = read_src!(builder, insn.sources[0]);
                    write_local_inline!(builder, insn.imm1, val);
                    write_dst!(builder, insn.destination, val);
                }

                opc if (op::SYNTHETIC_LOCAL_GET_0..=op::SYNTHETIC_LOCAL_GET_7).contains(&opc) => {
                    let local_idx = (opc - op::SYNTHETIC_LOCAL_GET_0) as i64;
                    let result = read_local_inline!(builder, local_idx);
                    write_dst!(builder, insn.destination, result);
                }
                opc if (op::SYNTHETIC_LOCAL_SET_0..=op::SYNTHETIC_LOCAL_SET_7).contains(&opc) => {
                    let local_idx = (opc - op::SYNTHETIC_LOCAL_SET_0) as i64;
                    let val = read_src!(builder, insn.sources[0]);
                    write_local_inline!(builder, local_idx, val);
                }
                op::SYNTHETIC_LOCAL_COPY => {
                    let val = read_local_inline!(builder, insn.imm1);
                    write_local_inline!(builder, insn.imm2, val);
                }

                op::GLOBAL_GET => {
                    let idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _uv_config_var = builder.use_var(config_var);
                    let _ic_0 = builder.ins().iconst(ptr_type, h_read_global);
                    let call = builder
                        .ins()
                        .call_indirect(read_global_sig, _ic_0, &[_uv_config_var, idx]);
                    let result = builder.inst_results(call)[0];
                    write_dst!(builder, insn.destination, result);
                }
                op::GLOBAL_SET => {
                    let val = read_src!(builder, insn.sources[0]);
                    let idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _uv_config_var = builder.use_var(config_var);
                    let _ic_0 = builder.ins().iconst(ptr_type, h_write_global);
                    builder
                        .ins()
                        .call_indirect(write_global_sig, _ic_0, &[_uv_config_var, idx, val]);
                }

                op::DROP => {
                    if insn.sources[0] == STACK_MARKER {
                        read_src!(builder, insn.sources[0]);
                    }
                    // No need to do anything if it's not on the real stack.
                }

                op::SELECT | op::SELECT_TYPED => {
                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let rhs = read_src!(builder, insn.sources[1]);
                    let lhs = read_src!(builder, insn.sources[2]);
                    let cond = builder.ins().icmp_imm(IntCC::NotEqual, cond_raw, 0);
                    let result = builder.ins().select(cond, lhs, rhs);
                    write_dst!(builder, insn.destination, result);
                }

                op::BR_TABLE => {
                    let inline_count = (insn.imm3 & 0xff) as usize;
                    if inline_count == 0xff {
                        return Err("br_table too large for inline encoding");
                    }

                    let default_label = ((insn.imm3 >> 8) & 0xffff) as usize;

                    // Collect all labels; first 8 from this instruction, rest from continuations.
                    let mut all_labels: Vec<usize> = Vec::with_capacity(inline_count);
                    for i in 0..inline_count {
                        let packed = if i < 4 { insn.imm1 as u64 } else { insn.imm2 as u64 };
                        all_labels.push(((packed >> ((i % 4) * 16)) & 0xffff) as usize);
                    }
                    while ip + 1 < insns.len() && insns[ip + 1].opcode == op::SYNTHETIC_BR_TABLE_CONT {
                        ip += 1;
                        let cont = &insns[ip];
                        let chunk = (cont.imm3 & 0xff) as usize;
                        for j in 0..chunk {
                            let packed = if j < 4 { cont.imm1 as u64 } else { cont.imm2 as u64 };
                            all_labels.push(((packed >> ((j % 4) * 16)) & 0xffff) as usize);
                        }
                    }

                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let cond = builder.ins().ireduce(types::I32, cond_raw);

                    let branch_to_label = |builder: &mut FunctionBuilder, label_idx: usize| {
                        if label_idx < control_stack.len() {
                            let target_idx = control_stack.len() - 1 - label_idx;
                            let frame = &control_stack[target_idx];
                            let target = frame.branch_target;
                            let arity = if frame.kind == ControlKind::Loop {
                                0
                            } else {
                                frame.arity
                            };
                            let entry = frame.stack_depth_at_entry as usize;
                            if max_stack_depth > 0 && arity > 0 {
                                let result = if sp > 0 {
                                    builder.use_var(stack_vars[sp - 1])
                                } else {
                                    let fp = builder.ins().iconst(ptr_type, h_stack_pop);
                                    let cfg = builder.use_var(config_var);
                                    let call = builder.ins().call_indirect(stack_pop_sig, fp, &[cfg]);
                                    builder.inst_results(call)[0]
                                };
                                builder.def_var(stack_vars[entry], result);
                            } else if max_stack_depth == 0 {
                                let entry_depth_var = frame
                                    .entry_real_depth_var
                                    .expect("entry_real_depth_var must be set when vstack is disabled");
                                let target_size = builder.use_var(entry_depth_var);
                                let arity_val = builder.ins().iconst(types::I32, arity as i64);
                                let cfg = builder.use_var(config_var);
                                let cleanup_fp = builder.ins().iconst(ptr_type, h_stack_cleanup);
                                builder.ins().call_indirect(
                                    stack_cleanup_sig,
                                    cleanup_fp,
                                    &[cfg, target_size, arity_val],
                                );
                            }
                            builder.ins().jump(target, &[]);
                        } else {
                            push_top_n_to_real!(builder, result_arity);
                            builder.ins().jump(epilogue_block, &[]);
                        }
                    };

                    for (i, &label) in all_labels.iter().enumerate() {
                        let case_block = builder.create_block();
                        let next_fallthrough = builder.create_block();
                        let compare = builder.ins().icmp_imm(IntCC::Equal, cond, i as i64);
                        builder.ins().brif(compare, case_block, &[], next_fallthrough, &[]);

                        builder.switch_to_block(case_block);
                        builder.seal_block(case_block);
                        branch_to_label(&mut builder, label);

                        builder.switch_to_block(next_fallthrough);
                        builder.seal_block(next_fallthrough);
                    }

                    branch_to_label(&mut builder, default_label);
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::I32_ADD => i32_binop!(builder, insn, iadd),
                op::I32_SUB => i32_binop!(builder, insn, isub),
                op::I32_MUL => i32_binop!(builder, insn, imul),
                op::I32_AND => i32_binop!(builder, insn, band),
                op::I32_OR => i32_binop!(builder, insn, bor),
                op::I32_XOR => i32_binop!(builder, insn, bxor),
                op::I32_SHL => i32_binop!(builder, insn, ishl),
                op::I32_SHRS => i32_binop!(builder, insn, sshr),
                op::I32_SHRU => i32_binop!(builder, insn, ushr),
                op::I32_ROTL => i32_binop!(builder, insn, rotl),
                op::I32_ROTR => i32_binop!(builder, insn, rotr),
                op::I32_DIVS => i32_binop!(builder, insn, sdiv),
                op::I32_DIVU => i32_binop!(builder, insn, udiv),
                op::I32_REMS => i32_binop!(builder, insn, srem),
                op::I32_REMU => i32_binop!(builder, insn, urem),
                op::I32_CLZ => i32_unop!(builder, insn, clz),
                op::I32_CTZ => i32_unop!(builder, insn, ctz),
                op::I32_POPCNT => i32_unop!(builder, insn, popcnt),

                op::I64_ADD => i64_binop!(builder, insn, iadd),
                op::I64_SUB => i64_binop!(builder, insn, isub),
                op::I64_MUL => i64_binop!(builder, insn, imul),
                op::I64_AND => i64_binop!(builder, insn, band),
                op::I64_OR => i64_binop!(builder, insn, bor),
                op::I64_XOR => i64_binop!(builder, insn, bxor),
                op::I64_SHL => i64_binop!(builder, insn, ishl),
                op::I64_SHRS => i64_binop!(builder, insn, sshr),
                op::I64_SHRU => i64_binop!(builder, insn, ushr),
                op::I64_ROTL => i64_binop!(builder, insn, rotl),
                op::I64_ROTR => i64_binop!(builder, insn, rotr),
                op::I64_DIVS => i64_binop!(builder, insn, sdiv),
                op::I64_DIVU => i64_binop!(builder, insn, udiv),
                op::I64_REMS => i64_binop!(builder, insn, srem),
                op::I64_REMU => i64_binop!(builder, insn, urem),
                op::I64_CLZ => i64_unop!(builder, insn, clz),
                op::I64_CTZ => i64_unop!(builder, insn, ctz),
                op::I64_POPCNT => i64_unop!(builder, insn, popcnt),

                op::I32_EQZ => {
                    let src_raw = read_src!(builder, insn.sources[0]);
                    let src = builder.ins().ireduce(types::I32, src_raw);
                    let r = builder.ins().icmp_imm(IntCC::Equal, src, 0);
                    let result = builder.ins().uextend(types::I64, r);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_EQ => i32_cmp!(builder, insn, IntCC::Equal),
                op::I32_NE => i32_cmp!(builder, insn, IntCC::NotEqual),
                op::I32_LTS => i32_cmp!(builder, insn, IntCC::SignedLessThan),
                op::I32_LTU => i32_cmp!(builder, insn, IntCC::UnsignedLessThan),
                op::I32_GTS => i32_cmp!(builder, insn, IntCC::SignedGreaterThan),
                op::I32_GTU => i32_cmp!(builder, insn, IntCC::UnsignedGreaterThan),
                op::I32_LES => i32_cmp!(builder, insn, IntCC::SignedLessThanOrEqual),
                op::I32_LEU => i32_cmp!(builder, insn, IntCC::UnsignedLessThanOrEqual),
                op::I32_GES => i32_cmp!(builder, insn, IntCC::SignedGreaterThanOrEqual),
                op::I32_GEU => i32_cmp!(builder, insn, IntCC::UnsignedGreaterThanOrEqual),

                op::I64_EQZ => {
                    let src = read_src!(builder, insn.sources[0]);
                    let r = builder.ins().icmp_imm(IntCC::Equal, src, 0);
                    let result = builder.ins().uextend(types::I64, r);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EQ => i64_cmp!(builder, insn, IntCC::Equal),
                op::I64_NE => i64_cmp!(builder, insn, IntCC::NotEqual),
                op::I64_LTS => i64_cmp!(builder, insn, IntCC::SignedLessThan),
                op::I64_LTU => i64_cmp!(builder, insn, IntCC::UnsignedLessThan),
                op::I64_GTS => i64_cmp!(builder, insn, IntCC::SignedGreaterThan),
                op::I64_GTU => i64_cmp!(builder, insn, IntCC::UnsignedGreaterThan),
                op::I64_LES => i64_cmp!(builder, insn, IntCC::SignedLessThanOrEqual),
                op::I64_LEU => i64_cmp!(builder, insn, IntCC::UnsignedLessThanOrEqual),
                op::I64_GES => i64_cmp!(builder, insn, IntCC::SignedGreaterThanOrEqual),
                op::I64_GEU => i64_cmp!(builder, insn, IntCC::UnsignedGreaterThanOrEqual),

                op::F32_ADD => f32_binop!(builder, insn, fadd),
                op::F32_SUB => f32_binop!(builder, insn, fsub),
                op::F32_MUL => f32_binop!(builder, insn, fmul),
                op::F32_DIV => f32_binop!(builder, insn, fdiv),
                op::F32_MIN => f32_binop!(builder, insn, fmin),
                op::F32_MAX => f32_binop!(builder, insn, fmax),
                op::F32_COPYSIGN => f32_binop!(builder, insn, fcopysign),
                op::F32_ABS => f32_unop!(builder, insn, fabs),
                op::F32_NEG => f32_unop!(builder, insn, fneg),
                op::F32_CEIL => f32_unop!(builder, insn, ceil),
                op::F32_FLOOR => f32_unop!(builder, insn, floor),
                op::F32_TRUNC => f32_unop!(builder, insn, trunc),
                op::F32_NEAREST => f32_unop!(builder, insn, nearest),
                op::F32_SQRT => f32_unop!(builder, insn, sqrt),

                op::F64_ADD => f64_binop!(builder, insn, fadd),
                op::F64_SUB => f64_binop!(builder, insn, fsub),
                op::F64_MUL => f64_binop!(builder, insn, fmul),
                op::F64_DIV => f64_binop!(builder, insn, fdiv),
                op::F64_MIN => f64_binop!(builder, insn, fmin),
                op::F64_MAX => f64_binop!(builder, insn, fmax),
                op::F64_COPYSIGN => f64_binop!(builder, insn, fcopysign),
                op::F64_ABS => f64_unop!(builder, insn, fabs),
                op::F64_NEG => f64_unop!(builder, insn, fneg),
                op::F64_CEIL => f64_unop!(builder, insn, ceil),
                op::F64_FLOOR => f64_unop!(builder, insn, floor),
                op::F64_TRUNC => f64_unop!(builder, insn, trunc),
                op::F64_NEAREST => f64_unop!(builder, insn, nearest),
                op::F64_SQRT => f64_unop!(builder, insn, sqrt),

                op::F32_EQ => f32_cmp!(builder, insn, FloatCC::Equal),
                op::F32_NE => f32_cmp!(builder, insn, FloatCC::NotEqual),
                op::F32_LT => f32_cmp!(builder, insn, FloatCC::LessThan),
                op::F32_GT => f32_cmp!(builder, insn, FloatCC::GreaterThan),
                op::F32_LE => f32_cmp!(builder, insn, FloatCC::LessThanOrEqual),
                op::F32_GE => f32_cmp!(builder, insn, FloatCC::GreaterThanOrEqual),

                op::F64_EQ => f64_cmp!(builder, insn, FloatCC::Equal),
                op::F64_NE => f64_cmp!(builder, insn, FloatCC::NotEqual),
                op::F64_LT => f64_cmp!(builder, insn, FloatCC::LessThan),
                op::F64_GT => f64_cmp!(builder, insn, FloatCC::GreaterThan),
                op::F64_LE => f64_cmp!(builder, insn, FloatCC::LessThanOrEqual),
                op::F64_GE => f64_cmp!(builder, insn, FloatCC::GreaterThanOrEqual),

                op::I32_WRAP_I64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND_SI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND_UI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().uextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_EXTEND8_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I8, src);
                    let extended = builder.ins().sextend(types::I32, narrowed);
                    let result = builder.ins().sextend(types::I64, extended);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_EXTEND16_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I16, src);
                    let extended = builder.ins().sextend(types::I32, narrowed);
                    let result = builder.ins().sextend(types::I64, extended);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND8_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I8, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND16_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I16, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND32_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                // Float-int conversions
                op::F32_CONVERT_SI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f32_val = builder.ins().fcvt_from_sint(types::F32, i32_val);
                    let result = builder.ins().bitcast(types::I32, MemFlags::new(), f32_val);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }
                op::F32_CONVERT_UI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f32_val = builder.ins().fcvt_from_uint(types::F32, i32_val);
                    let result = builder.ins().bitcast(types::I32, MemFlags::new(), f32_val);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }
                op::F32_CONVERT_SI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fcvt_from_sint(types::F32, src);
                    let result = builder.ins().bitcast(types::I32, MemFlags::new(), f32_val);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }
                op::F32_CONVERT_UI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fcvt_from_uint(types::F32, src);
                    let result = builder.ins().bitcast(types::I32, MemFlags::new(), f32_val);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }
                op::F64_CONVERT_SI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f64_val = builder.ins().fcvt_from_sint(types::F64, i32_val);
                    let result = builder.ins().bitcast(types::I64, MemFlags::new(), f64_val);
                    write_dst!(builder, insn.destination, result);
                }
                op::F64_CONVERT_UI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f64_val = builder.ins().fcvt_from_uint(types::F64, i32_val);
                    let result = builder.ins().bitcast(types::I64, MemFlags::new(), f64_val);
                    write_dst!(builder, insn.destination, result);
                }
                op::F64_CONVERT_SI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fcvt_from_sint(types::F64, src);
                    let result = builder.ins().bitcast(types::I64, MemFlags::new(), f64_val);
                    write_dst!(builder, insn.destination, result);
                }
                op::F64_CONVERT_UI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fcvt_from_uint(types::F64, src);
                    let result = builder.ins().bitcast(types::I64, MemFlags::new(), f64_val);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_REINTERPRET_F32
                | op::F32_REINTERPRET_I32
                | op::I64_REINTERPRET_F64
                | op::F64_REINTERPRET_I64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    write_dst!(builder, insn.destination, src);
                }
                op::F32_DEMOTE_F64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f64_val = builder.ins().bitcast(types::F64, MemFlags::new(), src);
                    let f32_val = builder.ins().fdemote(types::F32, f64_val);
                    let result = builder.ins().bitcast(types::I32, MemFlags::new(), f32_val);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }
                op::F64_PROMOTE_F32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f32_val = builder.ins().bitcast(types::F32, MemFlags::new(), i32_val);
                    let f64_val = builder.ins().fpromote(types::F64, f32_val);
                    let result = builder.ins().bitcast(types::I64, MemFlags::new(), f64_val);
                    write_dst!(builder, insn.destination, result);
                }
                // Truncation conversions (these can trap in wasm)
                op::I32_TRUNC_SF32
                | op::I32_TRUNC_UF32
                | op::I32_TRUNC_SF64
                | op::I32_TRUNC_UF64
                | op::I64_TRUNC_SF32
                | op::I64_TRUNC_UF32
                | op::I64_TRUNC_SF64
                | op::I64_TRUNC_UF64 => {
                    // Use cranelift's fcvt_to_sint/fcvt_to_uint which traps on overflow/NaN.
                    let src = read_src!(builder, insn.sources[0]);
                    let is_f32_src = matches!(
                        opc,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_UF32 | op::I64_TRUNC_SF32 | op::I64_TRUNC_UF32
                    );
                    let is_i32_dst = matches!(
                        opc,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_UF32 | op::I32_TRUNC_SF64 | op::I32_TRUNC_UF64
                    );
                    let is_signed = matches!(
                        opc,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_SF64 | op::I64_TRUNC_SF32 | op::I64_TRUNC_SF64
                    );

                    let float_val = if is_f32_src {
                        let i32_val = builder.ins().ireduce(types::I32, src);
                        builder.ins().bitcast(types::F32, MemFlags::new(), i32_val)
                    } else {
                        builder.ins().bitcast(types::F64, MemFlags::new(), src)
                    };

                    let int_type = if is_i32_dst { types::I32 } else { types::I64 };
                    let int_val = if is_signed {
                        builder.ins().fcvt_to_sint(int_type, float_val)
                    } else {
                        builder.ins().fcvt_to_uint(int_type, float_val)
                    };

                    let result = if is_i32_dst {
                        builder.ins().sextend(types::I64, int_val)
                    } else {
                        int_val
                    };
                    write_dst!(builder, insn.destination, result);
                }

                op::I32_TRUNC_SAT_F32_S
                | op::I32_TRUNC_SAT_F32_U
                | op::I32_TRUNC_SAT_F64_S
                | op::I32_TRUNC_SAT_F64_U
                | op::I64_TRUNC_SAT_F32_S
                | op::I64_TRUNC_SAT_F32_U
                | op::I64_TRUNC_SAT_F64_S
                | op::I64_TRUNC_SAT_F64_U => {
                    let src = read_src!(builder, insn.sources[0]);
                    let is_f32_src = matches!(
                        opc,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F32_U
                            | op::I64_TRUNC_SAT_F32_S
                            | op::I64_TRUNC_SAT_F32_U
                    );
                    let is_i32_dst = matches!(
                        opc,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F32_U
                            | op::I32_TRUNC_SAT_F64_S
                            | op::I32_TRUNC_SAT_F64_U
                    );
                    let is_signed = matches!(
                        opc,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F64_S
                            | op::I64_TRUNC_SAT_F32_S
                            | op::I64_TRUNC_SAT_F64_S
                    );

                    let float_val = if is_f32_src {
                        let i32_val = builder.ins().ireduce(types::I32, src);
                        builder.ins().bitcast(types::F32, MemFlags::new(), i32_val)
                    } else {
                        builder.ins().bitcast(types::F64, MemFlags::new(), src)
                    };

                    let int_type = if is_i32_dst { types::I32 } else { types::I64 };
                    let int_val = if is_signed {
                        builder.ins().fcvt_to_sint_sat(int_type, float_val)
                    } else {
                        builder.ins().fcvt_to_uint_sat(int_type, float_val)
                    };

                    let result = if is_i32_dst {
                        builder.ins().sextend(types::I64, int_val)
                    } else {
                        int_val
                    };
                    write_dst!(builder, insn.destination, result);
                }

                op::I32_LOAD
                | op::I64_LOAD
                | op::F32_LOAD
                | op::F64_LOAD
                | op::I32_LOAD8_S
                | op::I32_LOAD8_U
                | op::I32_LOAD16_S
                | op::I32_LOAD16_U
                | op::I64_LOAD8_S
                | op::I64_LOAD8_U
                | op::I64_LOAD16_S
                | op::I64_LOAD16_U
                | op::I64_LOAD32_S
                | op::I64_LOAD32_U => {
                    let use_direct_memory = (insn.imm3 & (1u32 << 31)) != 0;
                    let memory_load_helper = match opc {
                        op::I32_LOAD | op::F32_LOAD => h_mem_load32_u,
                        op::I64_LOAD | op::F64_LOAD => h_mem_load64,
                        op::I32_LOAD8_S | op::I64_LOAD8_S => h_mem_load8_s,
                        op::I32_LOAD8_U | op::I64_LOAD8_U => h_mem_load8_u,
                        op::I32_LOAD16_S | op::I64_LOAD16_S => h_mem_load16_s,
                        op::I32_LOAD16_U | op::I64_LOAD16_U => h_mem_load16_u,
                        op::I64_LOAD32_S => h_mem_load32_s,
                        op::I64_LOAD32_U => h_mem_load32_u,
                        _ => unreachable!(),
                    };

                    // addr = base (from source reg as u32) + offset.
                    let base_raw = read_src!(builder, insn.sources[0]);
                    let base_u32 = builder.ins().ireduce(types::I32, base_raw);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);

                    let result = if use_direct_memory {
                        let memory_base = builder.use_var(default_memory_base_var);
                        let addr_offset = if ptr_type == types::I64 {
                            addr
                        } else {
                            builder.ins().ireduce(ptr_type, addr)
                        };
                        let native_addr = builder.ins().iadd(memory_base, addr_offset);
                        match opc {
                            op::I32_LOAD | op::F32_LOAD | op::I64_LOAD32_U => {
                                let loaded = builder.ins().load(types::I32, MemFlags::new(), native_addr, 0);
                                builder.ins().uextend(types::I64, loaded)
                            }
                            op::I64_LOAD | op::F64_LOAD => {
                                builder.ins().load(types::I64, MemFlags::new(), native_addr, 0)
                            }
                            op::I32_LOAD8_S | op::I64_LOAD8_S => {
                                let loaded = builder.ins().load(types::I8, MemFlags::new(), native_addr, 0);
                                builder.ins().sextend(types::I64, loaded)
                            }
                            op::I32_LOAD8_U | op::I64_LOAD8_U => {
                                let loaded = builder.ins().load(types::I8, MemFlags::new(), native_addr, 0);
                                builder.ins().uextend(types::I64, loaded)
                            }
                            op::I32_LOAD16_S | op::I64_LOAD16_S => {
                                let loaded = builder.ins().load(types::I16, MemFlags::new(), native_addr, 0);
                                builder.ins().sextend(types::I64, loaded)
                            }
                            op::I32_LOAD16_U | op::I64_LOAD16_U => {
                                let loaded = builder.ins().load(types::I16, MemFlags::new(), native_addr, 0);
                                builder.ins().uextend(types::I64, loaded)
                            }
                            op::I64_LOAD32_S => {
                                let loaded = builder.ins().load(types::I32, MemFlags::new(), native_addr, 0);
                                builder.ins().sextend(types::I64, loaded)
                            }
                            _ => unreachable!(),
                        }
                    } else {
                        let mem_idx = builder.ins().iconst(types::I32, i64::from(insn.imm3 & 0x7fff_ffff));
                        let _xv_config_var = builder.use_var(config_var);
                        let _xc_0 = builder.ins().iconst(ptr_type, memory_load_helper);
                        let call = builder
                            .ins()
                            .call_indirect(mem_load_sig, _xc_0, &[_xv_config_var, mem_idx, addr]);
                        builder.inst_results(call)[0]
                    };
                    write_dst!(builder, insn.destination, result);
                }

                op::I32_STORE
                | op::I64_STORE
                | op::F32_STORE
                | op::F64_STORE
                | op::I32_STORE8
                | op::I32_STORE16
                | op::I64_STORE8
                | op::I64_STORE16
                | op::I64_STORE32 => {
                    let use_direct_memory = (insn.imm3 & (1u32 << 31)) != 0;
                    let memory_store_helper = match opc {
                        op::I32_STORE | op::F32_STORE | op::I64_STORE32 => h_mem_store32,
                        op::I64_STORE | op::F64_STORE => h_mem_store64,
                        op::I32_STORE8 | op::I64_STORE8 => h_mem_store8,
                        op::I32_STORE16 | op::I64_STORE16 => h_mem_store16,
                        _ => unreachable!(),
                    };

                    let val = read_src!(builder, insn.sources[0]);
                    let base_raw = read_src!(builder, insn.sources[1]);
                    let base_u32 = builder.ins().ireduce(types::I32, base_raw);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);

                    if use_direct_memory {
                        let memory_base = builder.use_var(default_memory_base_var);
                        let addr_offset = if ptr_type == types::I64 {
                            addr
                        } else {
                            builder.ins().ireduce(ptr_type, addr)
                        };
                        let native_addr = builder.ins().iadd(memory_base, addr_offset);
                        match opc {
                            op::I32_STORE | op::F32_STORE | op::I64_STORE32 => {
                                let narrowed = builder.ins().ireduce(types::I32, val);
                                builder.ins().store(MemFlags::new(), narrowed, native_addr, 0);
                            }
                            op::I64_STORE | op::F64_STORE => {
                                builder.ins().store(MemFlags::new(), val, native_addr, 0);
                            }
                            op::I32_STORE8 | op::I64_STORE8 => {
                                let narrowed = builder.ins().ireduce(types::I8, val);
                                builder.ins().store(MemFlags::new(), narrowed, native_addr, 0);
                            }
                            op::I32_STORE16 | op::I64_STORE16 => {
                                let narrowed = builder.ins().ireduce(types::I16, val);
                                builder.ins().store(MemFlags::new(), narrowed, native_addr, 0);
                            }
                            _ => unreachable!(),
                        }
                    } else {
                        let mem_idx = builder.ins().iconst(types::I32, i64::from(insn.imm3 & 0x7fff_ffff));
                        let _xv_config_var = builder.use_var(config_var);
                        let _xc_0 = builder.ins().iconst(ptr_type, memory_store_helper);
                        let call =
                            builder
                                .ins()
                                .call_indirect(mem_store_sig, _xc_0, &[_xv_config_var, mem_idx, addr, val]);
                        let trapped = builder.inst_results(call)[0];
                        let is_trap = builder.ins().icmp_imm(IntCC::NotEqual, trapped, 0);
                        let cont = builder.create_block();
                        builder.ins().brif(is_trap, trap_block, &[], cont, &[]);
                        builder.switch_to_block(cont);
                        builder.seal_block(cont);
                    }
                }

                op::MEMORY_SIZE => {
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _xv_config_var = builder.use_var(config_var);
                    let _xc_0 = builder.ins().iconst(ptr_type, h_mem_size);
                    let call = builder
                        .ins()
                        .call_indirect(mem_size_sig, _xc_0, &[_xv_config_var, mem_idx]);
                    let result = builder.inst_results(call)[0];
                    write_dst!(builder, insn.destination, result);
                }

                op::MEMORY_GROW => {
                    let pages = read_src!(builder, insn.sources[0]);
                    let pages_i32 = builder.ins().ireduce(types::I32, pages);
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _xv_config_var = builder.use_var(config_var);
                    let _xc_0 = builder.ins().iconst(ptr_type, h_mem_grow);
                    let call = builder
                        .ins()
                        .call_indirect(mem_grow_sig, _xc_0, &[_xv_config_var, mem_idx, pages_i32]);
                    let result = builder.inst_results(call)[0];
                    let refreshed_memory_base = builder.ins().load(
                        ptr_type,
                        MemFlags::trusted(),
                        _xv_config_var,
                        default_memory_base_offset,
                    );
                    builder.def_var(default_memory_base_var, refreshed_memory_base);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::MEMORY_COPY => {
                    // imm1 = dst_mem, imm2 = src_mem
                    // sources: [0]=count, [1]=src_offset, [2]=dst_offset
                    let count = read_src!(builder, insn.sources[0]);
                    let src_offset = read_src!(builder, insn.sources[1]);
                    let dst_offset = read_src!(builder, insn.sources[2]);
                    let count_i32 = builder.ins().ireduce(types::I32, count);
                    let src_i32 = builder.ins().ireduce(types::I32, src_offset);
                    let dst_i32 = builder.ins().ireduce(types::I32, dst_offset);
                    let dst_mem = builder.ins().iconst(types::I32, insn.imm1);
                    let src_mem = builder.ins().iconst(types::I32, insn.imm2);
                    let cfp = builder.ins().iconst(ptr_type, h_memory_copy);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(
                        builder,
                        memory_copy_sig,
                        cfp,
                        &[iv, cv, dst_mem, src_mem, dst_i32, src_i32, count_i32]
                    );
                }

                op::MEMORY_FILL => {
                    // imm1 = mem_idx
                    // sources: [0]=count, [1]=value, [2]=offset
                    let count = read_src!(builder, insn.sources[0]);
                    let value = read_src!(builder, insn.sources[1]);
                    let offset = read_src!(builder, insn.sources[2]);
                    let count_i32 = builder.ins().ireduce(types::I32, count);
                    let value_i32 = builder.ins().ireduce(types::I32, value);
                    let offset_i32 = builder.ins().ireduce(types::I32, offset);
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let cfp = builder.ins().iconst(ptr_type, h_memory_fill);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(
                        builder,
                        memory_fill_sig,
                        cfp,
                        &[iv, cv, mem_idx, offset_i32, value_i32, count_i32]
                    );
                }

                // For CALL and CALL_INDIRECT, we have no extra information and have to use the interpreter stack for args and returns.
                op::CALL => {
                    // Flush virtual stack, args are already on it from previous instructions.
                    flush_vstack_to_real!(builder);
                    let func_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let cfp = builder.ins().iconst(ptr_type, h_call_fn);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(builder, call_fn_sig, cfp, &[iv, cv, func_idx]);
                    // The helper pushes results to value_stack; pop to the actual destination.
                    if insn.destination != STACK_MARKER {
                        let pop_fp = builder.ins().iconst(ptr_type, h_stack_pop);
                        let cfg = builder.use_var(config_var);
                        let call = builder.ins().call_indirect(stack_pop_sig, pop_fp, &[cfg]);
                        let result = builder.inst_results(call)[0];
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::CALL_INDIRECT => {
                    flush_vstack_to_real!(builder);
                    let element_index = read_src!(builder, insn.sources[0]);
                    let element_index = builder.ins().ireduce(types::I32, element_index);
                    let type_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let table_idx = builder.ins().iconst(types::I32, insn.imm2);
                    let cfp = builder.ins().iconst(ptr_type, h_call_indirect);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(
                        builder,
                        call_indirect_sig,
                        cfp,
                        &[iv, cv, table_idx, type_idx, element_index]
                    );
                    if insn.destination != STACK_MARKER {
                        let pop_fp = builder.ins().iconst(ptr_type, h_stack_pop);
                        let cfg = builder.use_var(config_var);
                        let call = builder.ins().call_indirect(stack_pop_sig, pop_fp, &[cfg]);
                        let result = builder.inst_results(call)[0];
                        write_dst!(builder, insn.destination, result);
                    }
                }

                opc if (op::SYNTHETIC_CALL_00..=op::SYNTHETIC_CALL_31).contains(&opc) => {
                    let variant = (opc - op::SYNTHETIC_CALL_00) as usize;
                    let param_count = variant / 2;
                    let result_count = variant % 2;
                    let func_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    match param_count {
                        0 => {
                            let cfp = builder.ins().iconst(ptr_type, h_direct_call_0);
                            do_call_and_check!(builder, call_fn_sig, cfp, &[iv, cv, func_idx]);
                        }
                        1 => {
                            let arg0 = read_src!(builder, insn.sources[0]);
                            let cfp = builder.ins().iconst(ptr_type, h_direct_call_1);
                            do_call_and_check!(builder, call_fn1_sig, cfp, &[iv, cv, func_idx, arg0]);
                        }
                        2 => {
                            let s0 = read_src!(builder, insn.sources[0]); // last param (top)
                            let s1 = read_src!(builder, insn.sources[1]); // first param
                            let cfp = builder.ins().iconst(ptr_type, h_direct_call_2);
                            do_call_and_check!(builder, call_fn2_sig, cfp, &[iv, cv, func_idx, s1, s0]);
                        }
                        3 => {
                            let s0 = read_src!(builder, insn.sources[0]); // last param (top)
                            let s1 = read_src!(builder, insn.sources[1]); // middle param
                            let s2 = read_src!(builder, insn.sources[2]); // first param
                            let cfp = builder.ins().iconst(ptr_type, h_direct_call_3);
                            do_call_and_check!(builder, call_fn3_sig, cfp, &[iv, cv, func_idx, s2, s1, s0]);
                        }
                        _ => unreachable!(),
                    }

                    if result_count > 0 {
                        let cv = builder.use_var(config_var);
                        let result = builder.ins().load(
                            types::I64,
                            MemFlags::trusted(),
                            cv,
                            compiled_call_result_scratch_offset,
                        );
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::SYNTHETIC_CALL_WITH_RECORD_0 | op::SYNTHETIC_CALL_WITH_RECORD_1 => {
                    let func_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let cwp = builder.ins().iconst(ptr_type, h_call_wr);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(builder, call_wr_sig, cwp, &[iv, cv, func_idx]);
                    if opc == op::SYNTHETIC_CALL_WITH_RECORD_1 {
                        let cv2 = builder.use_var(config_var);
                        let result = builder.ins().load(
                            types::I64,
                            MemFlags::trusted(),
                            cv2,
                            compiled_call_result_scratch_offset,
                        );
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::SYNTHETIC_LOCAL_SETI32_CONST | op::SYNTHETIC_LOCAL_SETI64_CONST => {
                    let val = builder.ins().iconst(types::I64, insn.imm1);
                    write_local_inline!(builder, insn.imm2, val);
                }

                op::SYNTHETIC_I32_ADD2LOCAL => {
                    let r1 = read_local_inline!(builder, insn.imm1);
                    let v1 = builder.ins().ireduce(types::I32, r1);
                    let r2 = read_local_inline!(builder, insn.imm2);
                    let v2 = builder.ins().ireduce(types::I32, r2);
                    let result = builder.ins().iadd(v1, v2);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_ADDCONSTLOCAL => {
                    let r = read_local_inline!(builder, insn.imm2);
                    let v = builder.ins().ireduce(types::I32, r);
                    let k = builder.ins().iconst(types::I32, insn.imm1);
                    let result = builder.ins().iadd(v, k);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_ANDCONSTLOCAL => {
                    let r = read_local_inline!(builder, insn.imm2);
                    let v = builder.ins().ireduce(types::I32, r);
                    let k = builder.ins().iconst(types::I32, insn.imm1);
                    let result = builder.ins().band(v, k);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                opc if (op::SYNTHETIC_I32_SUB2LOCAL..=op::SYNTHETIC_I32_SHRS2LOCAL).contains(&opc) => {
                    let r1 = read_local_inline!(builder, insn.imm1);
                    let v1 = builder.ins().ireduce(types::I32, r1);
                    let r2 = read_local_inline!(builder, insn.imm2);
                    let v2 = builder.ins().ireduce(types::I32, r2);
                    let result = match opc {
                        op::SYNTHETIC_I32_SUB2LOCAL => builder.ins().isub(v1, v2),
                        op::SYNTHETIC_I32_MUL2LOCAL => builder.ins().imul(v1, v2),
                        op::SYNTHETIC_I32_AND2LOCAL => builder.ins().band(v1, v2),
                        op::SYNTHETIC_I32_OR2LOCAL => builder.ins().bor(v1, v2),
                        op::SYNTHETIC_I32_XOR2LOCAL => builder.ins().bxor(v1, v2),
                        op::SYNTHETIC_I32_SHL2LOCAL => builder.ins().ishl(v1, v2),
                        op::SYNTHETIC_I32_SHRU2LOCAL => builder.ins().ushr(v1, v2),
                        op::SYNTHETIC_I32_SHRS2LOCAL => builder.ins().sshr(v1, v2),
                        _ => unreachable!(),
                    };
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I64_ADD2LOCAL => {
                    let v1 = read_local_inline!(builder, insn.imm1);
                    let v2 = read_local_inline!(builder, insn.imm2);
                    let result = builder.ins().iadd(v1, v2);
                    write_dst!(builder, insn.destination, result);
                }
                op::SYNTHETIC_I64_ADDCONSTLOCAL => {
                    let v = read_local_inline!(builder, insn.imm2);
                    let k = builder.ins().iconst(types::I64, insn.imm1);
                    let result = builder.ins().iadd(v, k);
                    write_dst!(builder, insn.destination, result);
                }
                op::SYNTHETIC_I64_ANDCONSTLOCAL => {
                    let v = read_local_inline!(builder, insn.imm2);
                    let k = builder.ins().iconst(types::I64, insn.imm1);
                    let result = builder.ins().band(v, k);
                    write_dst!(builder, insn.destination, result);
                }
                opc if (op::SYNTHETIC_I64_SUB2LOCAL..=op::SYNTHETIC_I64_SHRS2LOCAL).contains(&opc) => {
                    let v1 = read_local_inline!(builder, insn.imm1);
                    let v2 = read_local_inline!(builder, insn.imm2);
                    let result = match opc {
                        op::SYNTHETIC_I64_SUB2LOCAL => builder.ins().isub(v1, v2),
                        op::SYNTHETIC_I64_MUL2LOCAL => builder.ins().imul(v1, v2),
                        op::SYNTHETIC_I64_AND2LOCAL => builder.ins().band(v1, v2),
                        op::SYNTHETIC_I64_OR2LOCAL => builder.ins().bor(v1, v2),
                        op::SYNTHETIC_I64_XOR2LOCAL => builder.ins().bxor(v1, v2),
                        op::SYNTHETIC_I64_SHL2LOCAL => builder.ins().ishl(v1, v2),
                        op::SYNTHETIC_I64_SHRU2LOCAL => builder.ins().ushr(v1, v2),
                        op::SYNTHETIC_I64_SHRS2LOCAL => builder.ins().sshr(v1, v2),
                        _ => unreachable!(),
                    };
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_STORELOCAL | op::SYNTHETIC_I64_STORELOCAL => {
                    let use_direct_memory = (insn.imm3 & (1u32 << 31)) != 0;
                    let base_raw = read_src!(builder, insn.sources[0]);
                    let val = read_local_inline!(builder, insn.imm2);
                    let base_u32 = builder.ins().ireduce(types::I32, base_raw);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);
                    if use_direct_memory {
                        let memory_base = builder.use_var(default_memory_base_var);
                        let addr_offset = if ptr_type == types::I64 {
                            addr
                        } else {
                            builder.ins().ireduce(ptr_type, addr)
                        };
                        let native_addr = builder.ins().iadd(memory_base, addr_offset);
                        if opc == op::SYNTHETIC_I32_STORELOCAL {
                            let narrowed = builder.ins().ireduce(types::I32, val);
                            builder.ins().store(MemFlags::new(), narrowed, native_addr, 0);
                        } else {
                            builder.ins().store(MemFlags::new(), val, native_addr, 0);
                        }
                    } else {
                        let mem_idx = builder.ins().iconst(types::I32, i64::from(insn.imm3 & 0x7fff_ffff));
                        let memory_store_helper = if opc == op::SYNTHETIC_I32_STORELOCAL {
                            h_mem_store32
                        } else {
                            h_mem_store64
                        };
                        let _uv_config_var = builder.use_var(config_var);
                        let _ic_0 = builder.ins().iconst(ptr_type, memory_store_helper);
                        let call =
                            builder
                                .ins()
                                .call_indirect(mem_store_sig, _ic_0, &[_uv_config_var, mem_idx, addr, val]);
                        let trapped = builder.inst_results(call)[0];
                        let is_trap = builder.ins().icmp_imm(IntCC::NotEqual, trapped, 0);
                        let cont = builder.create_block();
                        builder.ins().brif(is_trap, trap_block, &[], cont, &[]);
                        builder.switch_to_block(cont);
                        builder.seal_block(cont);
                    }
                }

                _ => {
                    return Err("unsupported instruction during codegen");
                }
            }

            ip += 1;
        }

        // If we fell through without hitting end, sync all results and head to the epilogue.
        if !is_unreachable {
            push_top_n_to_real!(builder, result_arity);
            builder.ins().jump(epilogue_block, &[]);
        }

        builder.switch_to_block(trap_block);
        builder.seal_block(trap_block);
        // Helper already set the trap for us.
        let trap_ret = builder.ins().iconst(types::I64, outcome_return_value as i64);
        builder.ins().return_(&[trap_ret]);

        builder.switch_to_block(epilogue_block);
        builder.seal_block(epilogue_block);
        // Clean up excess values on the real stack (e.g. from BR out of nested blocks); nothing to touch if we have vstack info.
        if has_raw_call {
            let cleanup_fp = builder.ins().iconst(ptr_type, h_stack_cleanup);
            let cfg = builder.use_var(config_var);
            let init_size = builder.use_var(initial_stack_size_var);
            let arity = builder.ins().iconst(types::I32, result_arity as i64);
            builder
                .ins()
                .call_indirect(stack_cleanup_sig, cleanup_fp, &[cfg, init_size, arity]);
        }
        Self::sync_regs_to_config(
            &mut builder,
            &reg_vars,
            config_var,
            regs_offset,
            value_size,
            &dirty_regs,
        );
        let ret_val = builder.ins().iconst(types::I64, outcome_return_value as i64);
        builder.ins().return_(&[ret_val]);

        builder.finalize();

        let mut ctx = Context::for_function(func);
        let code = ctx
            .compile(&*isa, &mut Default::default())
            .map_err(|_| "cranelift compilation failed")?;

        Ok(code.code_buffer().to_vec())
    }

    fn is_supported(insn: &CraneliftInsn) -> bool {
        let opc = insn.opcode;
        matches!(
            opc,
            op::NOP
                | op::UNREACHABLE
                | op::BLOCK
                | op::LOOP
                | op::IF
                | op::ELSE
                | op::END
                | op::BR
                | op::BR_IF
                | op::BR_TABLE
                | op::RETURN
                | op::CALL
                | op::DROP
                | op::SELECT
                | op::SELECT_TYPED
                | op::LOCAL_GET
                | op::LOCAL_SET
                | op::LOCAL_TEE
                | op::GLOBAL_GET
                | op::GLOBAL_SET
                | op::I32_CONST
                | op::I64_CONST
                | op::F32_CONST
                | op::F64_CONST
                | op::I32_EQZ..=op::I32_GEU
                | op::I64_EQZ..=op::I64_GEU
                | op::F32_EQ..=op::F64_GE
                | op::I32_CLZ..=op::I32_ROTR
                | op::I64_CLZ..=op::I64_ROTR
                | op::F32_ABS..=op::F32_COPYSIGN
                | op::F64_ABS..=op::F64_COPYSIGN
                | op::I32_WRAP_I64..=op::F64_REINTERPRET_I64
                | op::I32_EXTEND8_S..=op::I64_EXTEND32_S
                | op::I32_LOAD..=op::I64_STORE32
                | op::MEMORY_SIZE
                | op::MEMORY_GROW
                | op::CALL_INDIRECT
                | op::I32_TRUNC_SAT_F32_S..=op::I64_TRUNC_SAT_F64_U
                | op::MEMORY_COPY
                | op::MEMORY_FILL
                | op::SYNTHETIC_END_EXPRESSION
                | op::SYNTHETIC_LOCAL_GET_0..=op::SYNTHETIC_LOCAL_GET_7
                | op::SYNTHETIC_LOCAL_SET_0..=op::SYNTHETIC_LOCAL_SET_7
                | op::SYNTHETIC_LOCAL_COPY
                | op::SYNTHETIC_BR_NOSTACK
                | op::SYNTHETIC_BR_IF_NOSTACK
                | op::SYNTHETIC_CALL_00..=op::SYNTHETIC_CALL_31
                | op::SYNTHETIC_I32_ADD2LOCAL..=op::SYNTHETIC_I32_ANDCONSTLOCAL
                | op::SYNTHETIC_I32_STORELOCAL
                | op::SYNTHETIC_LOCAL_SETI32_CONST
                | op::SYNTHETIC_CALL_WITH_RECORD_0
                | op::SYNTHETIC_CALL_WITH_RECORD_1
                | op::SYNTHETIC_ARGUMENT_GET
                | op::SYNTHETIC_ARGUMENT_SET
                | op::SYNTHETIC_ARGUMENT_TEE
                | op::SYNTHETIC_I32_SUB2LOCAL..=op::SYNTHETIC_I32_SHRS2LOCAL
                | op::SYNTHETIC_I64_ADD2LOCAL..=op::SYNTHETIC_LOCAL_SETI64_CONST
                | op::SYNTHETIC_BR_TABLE_CONT
        )
    }

    fn sync_regs_to_config(
        builder: &mut FunctionBuilder,
        reg_vars: &[Variable; REG_COUNT],
        config_var: Variable,
        regs_offset: i32,
        value_size: i32,
        dirty: &[bool; REG_COUNT],
    ) {
        let config = builder.use_var(config_var);
        for i in 0..REG_COUNT {
            if !dirty[i] {
                continue;
            }
            let val = builder.use_var(reg_vars[i]);
            let offset = regs_offset + (i as i32) * value_size;
            builder.ins().store(MemFlags::trusted(), val, config, offset);
            let zero = builder.ins().iconst(types::I64, 0);
            builder.ins().store(MemFlags::trusted(), zero, config, offset + 8);
        }
    }
}
