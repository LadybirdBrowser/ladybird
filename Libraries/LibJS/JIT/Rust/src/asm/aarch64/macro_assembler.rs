/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The portable macro assembler on AArch64 (AAPCS64).

use super::super::Address;
use super::super::Architecture;
use super::super::AsmError;
use super::super::Condition;
use super::super::DoubleCondition;
use super::super::Fpr;
use super::super::FprSet;
use super::super::Gpr;
use super::super::GprSet;
use super::super::Label;
use super::super::MachineFrame;
use super::super::NegativeZero;
use super::super::PortableMacroAssembler;
use super::super::Width;
use super::super::Width::W32;
use super::super::Width::W64;
use super::*;

/// Scratch register for address legalization and call targets.
const ADDRESS_SCRATCH: Gpr = X16;
/// Scratch register for immediates and intermediate values.
const VALUE_SCRATCH: Gpr = X17;
const FP_SCRATCH: Fpr = D31;

/// The macro assembler for AArch64. Wraps an `Assembler`, which stays
/// available for architecture specific code.
///
/// In this layer, register 31 always means SP.
#[derive(Debug, Default)]
pub struct MacroAssembler {
    pub assembler: Assembler,
    /// 64-bit constants that would take more than two instructions to
    /// build, loaded PC-relative from a pool after the code.
    constants: Vec<(u64, Label)>,
    /// 128-bit constants, in the same pool.
    constants128: Vec<(u128, Label)>,
}

fn condition_code(condition: Condition) -> Cond {
    match condition {
        Condition::Equal | Condition::Zero => Cond::Eq,
        Condition::NotEqual | Condition::NonZero => Cond::Ne,
        Condition::LessThan => Cond::Lt,
        Condition::LessThanOrEqual => Cond::Le,
        Condition::GreaterThan => Cond::Gt,
        Condition::GreaterThanOrEqual => Cond::Ge,
        Condition::Below => Cond::Lo,
        Condition::BelowOrEqual => Cond::Ls,
        Condition::Above => Cond::Hi,
        Condition::AboveOrEqual => Cond::Hs,
        Condition::Overflow => Cond::Vs,
        Condition::NoOverflow => Cond::Vc,
        Condition::Negative => Cond::Mi,
        Condition::PositiveOrZero => Cond::Pl,
    }
}

/// How a double condition maps onto the flags of `fcmp lhs, rhs`.
enum DoubleFlags {
    Single(Cond),
    /// Either condition code holds.
    Either(Cond, Cond),
}

fn double_flags(condition: DoubleCondition) -> DoubleFlags {
    use DoubleFlags::*;
    // fcmp sets NZCV to 0011 when unordered, 1000 for less, 0110 for equal
    // and 0010 for greater.
    match condition {
        DoubleCondition::Equal => Single(Cond::Eq),
        DoubleCondition::NotEqual => Either(Cond::Mi, Cond::Gt),
        DoubleCondition::LessThan => Single(Cond::Mi),
        DoubleCondition::LessThanOrEqual => Single(Cond::Ls),
        DoubleCondition::GreaterThan => Single(Cond::Gt),
        DoubleCondition::GreaterThanOrEqual => Single(Cond::Ge),
        DoubleCondition::EqualOrUnordered => Either(Cond::Eq, Cond::Vs),
        DoubleCondition::NotEqualOrUnordered => Single(Cond::Ne),
        DoubleCondition::LessThanOrUnordered => Single(Cond::Lt),
        DoubleCondition::LessThanOrEqualOrUnordered => Single(Cond::Le),
        DoubleCondition::GreaterThanOrUnordered => Single(Cond::Hi),
        DoubleCondition::GreaterThanOrEqualOrUnordered => Single(Cond::Pl),
        DoubleCondition::Ordered => Single(Cond::Vc),
        DoubleCondition::Unordered => Single(Cond::Vs),
    }
}

/// An add/sub immediate as `(imm12, shifted by 12)`, if encodable.
fn arithmetic_immediate(imm: u64) -> Option<(u32, bool)> {
    if imm < 4096 {
        Some((imm as u32, false))
    } else if imm & 0xfff == 0 && imm >> 12 < 4096 {
        Some(((imm >> 12) as u32, true))
    } else {
        None
    }
}

/// How an integer operation uses an immediate.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ArithmeticOp {
    Add,
    Sub,
}

impl ArithmeticOp {
    fn opposite(self) -> Self {
        match self {
            ArithmeticOp::Add => ArithmeticOp::Sub,
            ArithmeticOp::Sub => ArithmeticOp::Add,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum LogicalOp {
    And,
    Or,
    Xor,
}

impl MacroAssembler {
    /// Turns a portable address into an encodable operand for an access of
    /// `1 << size_log2` bytes, using `ADDRESS_SCRATCH` when needed.
    fn legalize(&mut self, address: &Address, size_log2: u32) -> MemOperand {
        let base = address.base;
        let displacement = address.displacement;
        let size = 1 << size_log2;
        let Some((index, scale)) = address.index else {
            let scaled = displacement >= 0 && displacement % size == 0 && displacement / size < 4096;
            if scaled || (-256..256).contains(&displacement) {
                return MemOperand::Offset(base, displacement);
            }
            self.assembler.mov_imm(W64, ADDRESS_SCRATCH, displacement as i64 as u64);
            return MemOperand::Register {
                base,
                index: ADDRESS_SCRATCH,
                extend: Extend::Uxtx,
                shifted: false,
            };
        };
        let scale = u32::from(scale.log2());
        let index_fits = scale == 0 || scale == size_log2;
        if displacement == 0 && index_fits {
            return MemOperand::Register {
                base,
                index,
                extend: Extend::Uxtx,
                shifted: scale != 0,
            };
        }
        if displacement == 0 {
            // The extended form accepts SP as the base.
            self.assembler
                .add_extended(W64, ADDRESS_SCRATCH, base, index, Extend::Uxtx, scale as u8);
            return MemOperand::Offset(ADDRESS_SCRATCH, 0);
        }
        self.assembler.mov_imm(W64, ADDRESS_SCRATCH, displacement as i64 as u64);
        self.assembler
            .add_extended(W64, ADDRESS_SCRATCH, base, ADDRESS_SCRATCH, Extend::Uxtx, 0);
        if index_fits {
            return MemOperand::Register {
                base: ADDRESS_SCRATCH,
                index,
                extend: Extend::Uxtx,
                shifted: scale != 0,
            };
        }
        self.assembler
            .add(W64, ADDRESS_SCRATCH, ADDRESS_SCRATCH, index, Shift::Lsl, scale as u8);
        MemOperand::Offset(ADDRESS_SCRATCH, 0)
    }

    fn load_store(&mut self, op: MemOp, rt: u8, address: &Address) {
        let operand = self.legalize(address, op.size_log2());
        self.assembler.load_store(op, rt, operand);
    }

    /// `dst = lhs +/- imm`. Either register may be SP.
    fn arithmetic_imm(&mut self, op: ArithmeticOp, width: Width, dst: Gpr, lhs: Gpr, imm: i64) {
        let (op, magnitude) = if imm < 0 {
            (op.opposite(), imm.unsigned_abs())
        } else {
            (op, imm as u64)
        };
        let magnitude = match width {
            W32 => magnitude & 0xffff_ffff,
            W64 => magnitude,
        };
        if let Some((imm12, shift12)) = arithmetic_immediate(magnitude) {
            match op {
                ArithmeticOp::Add => self.assembler.add_imm(width, dst, lhs, imm12, shift12),
                ArithmeticOp::Sub => self.assembler.sub_imm(width, dst, lhs, imm12, shift12),
            }
            return;
        }
        self.mov_imm(width, VALUE_SCRATCH, magnitude);
        let uses_stack_pointer = dst == SP || lhs == SP;
        match (op, uses_stack_pointer) {
            // Only the extended register form accepts SP.
            (ArithmeticOp::Add, true) => {
                self.assembler
                    .add_extended(W64, dst, lhs, VALUE_SCRATCH, Extend::Uxtx, 0);
            }
            (ArithmeticOp::Sub, true) => {
                self.assembler
                    .sub_extended(W64, dst, lhs, VALUE_SCRATCH, Extend::Uxtx, 0);
            }
            (ArithmeticOp::Add, false) => self.assembler.add(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
            (ArithmeticOp::Sub, false) => self.assembler.sub(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
        }
    }

    /// The label of `value` in the constant pool.
    fn constant(&mut self, value: u64) -> Label {
        if let Some((_, label)) = self.constants.iter().find(|(constant, _)| *constant == value) {
            return *label;
        }
        let label = self.assembler.new_label();
        self.constants.push((value, label));
        label
    }

    /// The label of the 128-bit `value` in the constant pool.
    fn constant128(&mut self, value: u128) -> Label {
        if let Some((_, label)) = self.constants128.iter().find(|(constant, _)| *constant == value) {
            return *label;
        }
        let label = self.assembler.new_label();
        self.constants128.push((value, label));
        label
    }

    /// `dst = imm`, from the constant pool if building it would take more
    /// than two instructions.
    fn mov_imm(&mut self, width: Width, dst: Gpr, imm: u64) {
        if width == W64 && Assembler::mov_imm_instruction_count(imm) > 2 {
            let label = self.constant(imm);
            self.assembler.ldr_literal(dst, label);
        } else {
            self.assembler.mov_imm(width, dst, imm);
        }
    }

    /// Adds `imm` to the value of `width` at `address`.
    fn add_to_memory_imm(&mut self, width: Width, address: &Address, imm: i32) {
        let (load, store) = match width {
            W32 => (MemOp::LdrW, MemOp::StrW),
            W64 => (MemOp::LdrX, MemOp::StrX),
        };
        // NB: Addressing may use ADDRESS_SCRATCH, which is free once the
        //     value is loaded.
        self.load_store(load, VALUE_SCRATCH.0, address);
        let magnitude = u64::from(imm.unsigned_abs());
        if arithmetic_immediate(magnitude).is_some() {
            self.arithmetic_imm(ArithmeticOp::Add, width, VALUE_SCRATCH, VALUE_SCRATCH, i64::from(imm));
        } else {
            self.mov_imm(width, ADDRESS_SCRATCH, i64::from(imm) as u64);
            self.assembler
                .add(width, VALUE_SCRATCH, VALUE_SCRATCH, ADDRESS_SCRATCH, Shift::Lsl, 0);
        }
        self.load_store(store, VALUE_SCRATCH.0, address);
    }

    /// A register move of `width`; 32-bit moves zero-extend even onto themselves.
    fn move_width(&mut self, width: Width, dst: Gpr, src: Gpr) {
        if dst != src || width == W32 {
            self.assembler.mov(width, dst, src);
        }
    }

    /// `dst = lhs +/- imm`, setting the flags.
    fn arithmetic_imm_set_flags(&mut self, op: ArithmeticOp, width: Width, dst: Gpr, lhs: Gpr, imm: i64) {
        // Negating i32::MIN or i64::MIN gives a magnitude that is never
        // encodable, so the flags of the opposite operation always match.
        if let Some((imm12, shift12)) = arithmetic_immediate(imm.unsigned_abs()) {
            let op = if imm < 0 { op.opposite() } else { op };
            match op {
                ArithmeticOp::Add => self.assembler.adds_imm(width, dst, lhs, imm12, shift12),
                ArithmeticOp::Sub => self.assembler.subs_imm(width, dst, lhs, imm12, shift12),
            }
            return;
        }
        self.mov_imm(width, VALUE_SCRATCH, imm as u64);
        match op {
            ArithmeticOp::Add => self.assembler.adds(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
            ArithmeticOp::Sub => self.assembler.subs(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
        }
    }

    fn logical_imm(&mut self, op: LogicalOp, width: Width, dst: Gpr, lhs: Gpr, imm: u64) {
        let imm = match width {
            W32 => imm & 0xffff_ffff,
            W64 => imm,
        };
        let all_ones = match width {
            W32 => u64::from(u32::MAX),
            W64 => u64::MAX,
        };
        // The bitmask encoding cannot express all zeros or all ones.
        if imm == 0 {
            match op {
                LogicalOp::And => self.assembler.movz(width, dst, 0, 0),
                LogicalOp::Or | LogicalOp::Xor => self.move_width(width, dst, lhs),
            }
            return;
        }
        if imm == all_ones {
            match op {
                LogicalOp::And => self.move_width(width, dst, lhs),
                LogicalOp::Or => self.assembler.movn(width, dst, 0, 0),
                LogicalOp::Xor => self.assembler.mvn(width, dst, lhs),
            }
            return;
        }
        if encode_logical_immediate(imm, width).is_some() {
            match op {
                LogicalOp::And => self.assembler.and_imm(width, dst, lhs, imm),
                LogicalOp::Or => self.assembler.orr_imm(width, dst, lhs, imm),
                LogicalOp::Xor => self.assembler.eor_imm(width, dst, lhs, imm),
            }
            return;
        }
        self.mov_imm(width, VALUE_SCRATCH, imm);
        match op {
            LogicalOp::And => self.assembler.and(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
            LogicalOp::Or => self.assembler.orr(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
            LogicalOp::Xor => self.assembler.eor(width, dst, lhs, VALUE_SCRATCH, Shift::Lsl, 0),
        }
    }

    fn compare_imm(&mut self, width: Width, lhs: Gpr, imm: i64) {
        self.arithmetic_imm_set_flags(ArithmeticOp::Sub, width, XZR, lhs, imm);
    }

    fn shift_imm(&mut self, op: Shift, width: Width, dst: Gpr, src: Gpr, amount: u8) {
        let amount = amount & (width.bits() as u8 - 1);
        if amount == 0 {
            return self.move_width(width, dst, src);
        }
        match op {
            Shift::Lsl => self.assembler.lsl_imm(width, dst, src, amount),
            Shift::Lsr => self.assembler.lsr_imm(width, dst, src, amount),
            Shift::Asr => self.assembler.asr_imm(width, dst, src, amount),
            Shift::Ror => unreachable!(),
        }
    }

    /// Branches on `value & mask` for a Zero/NonZero/Negative/PositiveOrZero
    /// condition.
    fn branch_test(&mut self, width: Width, condition: Condition, value: Gpr, mask: u64, target: Label) {
        let zero_test = matches!(condition, Condition::Zero | Condition::Equal);
        let non_zero_test = matches!(condition, Condition::NonZero | Condition::NotEqual);
        if mask.is_power_of_two() && (zero_test || non_zero_test) {
            let bit = mask.trailing_zeros() as u8;
            if zero_test {
                self.assembler.tbz(value, bit, target);
            } else {
                self.assembler.tbnz(value, bit, target);
            }
            return;
        }
        self.test_mask(width, value, mask);
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn test_mask(&mut self, width: Width, value: Gpr, mask: u64) {
        let all_ones = match width {
            W32 => u64::from(u32::MAX),
            W64 => u64::MAX,
        };
        if mask == all_ones {
            self.assembler.tst(width, value, value);
        } else if encode_logical_immediate(mask, width).is_some() {
            self.assembler.tst_imm(width, value, mask);
        } else {
            self.mov_imm(width, VALUE_SCRATCH, mask);
            self.assembler.tst(width, value, VALUE_SCRATCH);
        }
    }

    fn branch_compare_imm(&mut self, width: Width, condition: Condition, lhs: Gpr, imm: i64, target: Label) {
        if imm == 0 {
            match condition {
                Condition::Equal | Condition::Zero => return self.assembler.cbz(width, lhs, target),
                Condition::NotEqual | Condition::NonZero => return self.assembler.cbnz(width, lhs, target),
                _ => {}
            }
        }
        self.compare_imm(width, lhs, imm);
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn set_double_condition(&mut self, condition: DoubleCondition, dst: Gpr) {
        match double_flags(condition) {
            DoubleFlags::Single(cond) => self.assembler.cset(W32, dst, cond),
            DoubleFlags::Either(first, second) => {
                // dst = first; then dst = second ? 1 : dst.
                self.assembler.cset(W32, dst, first);
                self.assembler.csinc(W32, dst, dst, XZR, second.invert());
            }
        }
    }

    /// Pushes registers in pairs with pre-indexed stores, in ascending order.
    fn push_registers(&mut self, registers: &[u8], is_fpr: bool) {
        for pair in registers.chunks(2) {
            match (pair, is_fpr) {
                ([a, b], false) => self.assembler.stp(Gpr(*a), Gpr(*b), PairOperand::PreIndex(SP, -16)),
                ([a, b], true) => self.assembler.stp_d(Fpr(*a), Fpr(*b), PairOperand::PreIndex(SP, -16)),
                ([a], false) => self.assembler.str(W64, Gpr(*a), MemOperand::PreIndex(SP, -16)),
                ([a], true) => self.assembler.str_d(Fpr(*a), MemOperand::PreIndex(SP, -16)),
                _ => unreachable!(),
            }
        }
    }

    /// Pops what `push_registers` pushed.
    fn pop_registers(&mut self, registers: &[u8], is_fpr: bool) {
        for pair in registers.chunks(2).rev() {
            match (pair, is_fpr) {
                ([a, b], false) => self.assembler.ldp(Gpr(*a), Gpr(*b), PairOperand::PostIndex(SP, 16)),
                ([a, b], true) => self.assembler.ldp_d(Fpr(*a), Fpr(*b), PairOperand::PostIndex(SP, 16)),
                ([a], false) => self.assembler.ldr(W64, Gpr(*a), MemOperand::PostIndex(SP, 16)),
                ([a], true) => self.assembler.ldr_d(Fpr(*a), MemOperand::PostIndex(SP, 16)),
                _ => unreachable!(),
            }
        }
    }
}

impl PortableMacroAssembler for MacroAssembler {
    const ARCHITECTURE: Architecture = Architecture::AArch64;
    const ARGUMENT_GPRS: &'static [Gpr] = &[X0, X1, X2, X3, X4, X5, X6, X7];
    const ARGUMENT_FPRS: &'static [Fpr] = &[D0, D1, D2, D3, D4, D5, D6, D7];
    const RETURN_GPRS: [Gpr; 2] = [X0, X1];
    const CALLEE_SAVED_GPRS: GprSet = GprSet(0x7ff8_0000);
    const CALLEE_SAVED_FPRS: FprSet = FprSet(0x0000_ff00);
    /// x0-x17. x18 is the platform register and never touched.
    const CALLER_SAVED_GPRS: GprSet = GprSet(0x0003_ffff);
    const CALLER_SAVED_FPRS: FprSet = FprSet(0xffff_00ff);
    const SCRATCH_GPRS: GprSet = GprSet::of(&[ADDRESS_SCRATCH, VALUE_SCRATCH]);
    const SCRATCH_FPRS: FprSet = FprSet::of(&[FP_SCRATCH]);
    /// x0-x15 and x19-x28.
    const ALLOCATABLE_GPRS: GprSet = GprSet(0x1ff8_ffff);
    const ALLOCATABLE_FPRS: FprSet = FprSet(0x7fff_ffff);
    const STACK_POINTER: Gpr = SP;
    const FRAME_POINTER: Gpr = FP;

    fn new() -> Self {
        Self::default()
    }

    fn offset(&self) -> usize {
        self.assembler.offset()
    }

    fn new_label(&mut self) -> Label {
        self.assembler.new_label()
    }

    fn bind(&mut self, label: Label) {
        self.assembler.bind(label);
    }

    fn label_offset(&self, label: Label) -> Option<usize> {
        self.assembler.label_offset(label)
    }

    fn patchable_nop(&mut self) -> usize {
        let offset = self.assembler.offset();
        self.assembler.nop();
        offset
    }

    fn jump_patch(from: usize, to: usize) -> Vec<u8> {
        // NB: `b` with a 26-bit word offset.
        let words = (to as i64 - from as i64) / 4;
        assert!((-(1 << 25)..(1 << 25)).contains(&words), "code is smaller than 128 MiB");
        (0x1400_0000 | (words as u32 & 0x03ff_ffff)).to_le_bytes().to_vec()
    }

    fn finish_with_data_offset(mut self) -> Result<(Vec<u8>, usize), AsmError> {
        let data_offset = self.assembler.offset();
        if !self.constants128.is_empty() {
            self.assembler.align_data(16);
            for (value, label) in std::mem::take(&mut self.constants128) {
                self.assembler.bind(label);
                self.assembler.data_u64(value as u64);
                self.assembler.data_u64((value >> 64) as u64);
            }
        }
        if !self.constants.is_empty() {
            self.assembler.align_data(8);
            for (value, label) in std::mem::take(&mut self.constants) {
                self.assembler.bind(label);
                self.assembler.data_u64(value);
            }
        }
        Ok((self.assembler.finish()?, data_offset))
    }

    fn unreachable(&mut self) {
        self.assembler.udf(0);
    }

    fn move64(&mut self, dst: Gpr, src: Gpr) {
        if dst == src {
            return;
        }
        if dst == SP || src == SP {
            self.assembler.mov_sp(dst, src);
        } else {
            self.assembler.mov(W64, dst, src);
        }
    }

    fn move32(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.mov(W32, dst, src);
    }

    fn move_imm64(&mut self, dst: Gpr, imm: u64) {
        self.mov_imm(W64, dst, imm);
    }

    fn move_imm32(&mut self, dst: Gpr, imm: u32) {
        self.assembler.mov_imm(W32, dst, u64::from(imm));
    }

    fn sign_extend32_to_64(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.sxtw(dst, src);
    }

    fn load8(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::Ldrb, dst.0, address);
    }

    fn load8_sign_extend(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::LdrsbX, dst.0, address);
    }

    fn load16(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::Ldrh, dst.0, address);
    }

    fn load16_sign_extend(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::LdrshX, dst.0, address);
    }

    fn load32(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::LdrW, dst.0, address);
    }

    fn load32_sign_extend(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::Ldrsw, dst.0, address);
    }

    fn load64(&mut self, dst: Gpr, address: &Address) {
        self.load_store(MemOp::LdrX, dst.0, address);
    }

    fn store8(&mut self, address: &Address, src: Gpr) {
        self.load_store(MemOp::Strb, src.0, address);
    }

    fn store16(&mut self, address: &Address, src: Gpr) {
        self.load_store(MemOp::Strh, src.0, address);
    }

    fn store32(&mut self, address: &Address, src: Gpr) {
        self.load_store(MemOp::StrW, src.0, address);
    }

    fn store64(&mut self, address: &Address, src: Gpr) {
        self.load_store(MemOp::StrX, src.0, address);
    }

    fn store_imm32(&mut self, address: &Address, imm: u32) {
        let source = if imm == 0 {
            XZR
        } else {
            self.assembler.mov_imm(W32, VALUE_SCRATCH, u64::from(imm));
            VALUE_SCRATCH
        };
        self.load_store(MemOp::StrW, source.0, address);
    }

    fn store_imm64(&mut self, address: &Address, imm: u64) {
        let source = if imm == 0 {
            XZR
        } else {
            self.mov_imm(W64, VALUE_SCRATCH, imm);
            VALUE_SCRATCH
        };
        self.load_store(MemOp::StrX, source.0, address);
    }

    fn add32_to_memory_imm(&mut self, address: &Address, imm: i32) {
        self.add_to_memory_imm(W32, address, imm);
    }

    fn add64_to_memory_imm(&mut self, address: &Address, imm: i32) {
        self.add_to_memory_imm(W64, address, imm);
    }

    fn load_effective_address(&mut self, dst: Gpr, address: &Address) {
        let mut base = address.base;
        if let Some((index, scale)) = address.index {
            self.assembler
                .add_extended(W64, dst, address.base, index, Extend::Uxtx, scale.log2());
            base = dst;
        }
        if address.displacement != 0 {
            self.arithmetic_imm(ArithmeticOp::Add, W64, dst, base, i64::from(address.displacement));
        } else {
            self.move64(dst, base);
        }
    }

    fn load_double(&mut self, dst: Fpr, address: &Address) {
        self.load_store(MemOp::LdrD, dst.0, address);
    }

    fn store_double(&mut self, address: &Address, src: Fpr) {
        self.load_store(MemOp::StrD, src.0, address);
    }

    fn load_imm128(&mut self, dst: Fpr, imm: u128) {
        let label = self.constant128(imm);
        self.assembler.ldr_literal_quad(dst, label);
    }

    fn store128(&mut self, address: &Address, src: Fpr) {
        self.load_store(MemOp::StrQ, src.0, address);
    }

    fn load_float_as_double(&mut self, dst: Fpr, address: &Address) {
        self.load_store(MemOp::LdrS, dst.0, address);
        self.assembler.fcvt_double_from_single(dst, dst);
    }

    fn store_double_as_float(&mut self, address: &Address, src: Fpr) {
        self.assembler.fcvt_single_from_double(FP_SCRATCH, src);
        self.load_store(MemOp::StrS, FP_SCRATCH.0, address);
    }

    fn add32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.add(W32, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn add32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i32) {
        self.arithmetic_imm(ArithmeticOp::Add, W32, dst, lhs, i64::from(imm));
    }

    fn add64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.add(W64, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn add64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i64) {
        self.arithmetic_imm(ArithmeticOp::Add, W64, dst, lhs, imm);
    }

    fn sub32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.sub(W32, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn sub32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i32) {
        self.arithmetic_imm(ArithmeticOp::Sub, W32, dst, lhs, i64::from(imm));
    }

    fn sub64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.sub(W64, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn sub64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i64) {
        self.arithmetic_imm(ArithmeticOp::Sub, W64, dst, lhs, imm);
    }

    fn and32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.and(W32, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn and32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.logical_imm(LogicalOp::And, W32, dst, lhs, u64::from(imm));
    }

    fn and64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.and(W64, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn and64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64) {
        self.logical_imm(LogicalOp::And, W64, dst, lhs, imm);
    }

    fn or32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.orr(W32, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn or32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.logical_imm(LogicalOp::Or, W32, dst, lhs, u64::from(imm));
    }

    fn or64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.orr(W64, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn or64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64) {
        self.logical_imm(LogicalOp::Or, W64, dst, lhs, imm);
    }

    fn xor32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.eor(W32, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn xor32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.logical_imm(LogicalOp::Xor, W32, dst, lhs, u64::from(imm));
    }

    fn xor64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.eor(W64, dst, lhs, rhs, Shift::Lsl, 0);
    }

    fn xor64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64) {
        self.logical_imm(LogicalOp::Xor, W64, dst, lhs, imm);
    }

    fn mul32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.mul(W32, dst, lhs, rhs);
    }

    fn mul32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.mov_imm(W32, VALUE_SCRATCH, u64::from(imm));
        self.assembler.mul(W32, dst, lhs, VALUE_SCRATCH);
    }

    fn mul64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.mul(W64, dst, lhs, rhs);
    }

    fn neg32(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.neg(W32, dst, src);
    }

    fn neg64(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.neg(W64, dst, src);
    }

    fn not32(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.mvn(W32, dst, src);
    }

    fn not64(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.mvn(W64, dst, src);
    }

    fn shl32(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.assembler.lsl(W32, dst, src, amount);
    }

    fn shl32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(Shift::Lsl, W32, dst, src, amount);
    }

    fn shl64(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.assembler.lsl(W64, dst, src, amount);
    }

    fn shl64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(Shift::Lsl, W64, dst, src, amount);
    }

    fn shr32(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.assembler.lsr(W32, dst, src, amount);
    }

    fn shr32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(Shift::Lsr, W32, dst, src, amount);
    }

    fn shr64(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.assembler.lsr(W64, dst, src, amount);
    }

    fn shr64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(Shift::Lsr, W64, dst, src, amount);
    }

    fn sar32(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.assembler.asr(W32, dst, src, amount);
    }

    fn sar32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(Shift::Asr, W32, dst, src, amount);
    }

    fn sar64(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.assembler.asr(W64, dst, src, amount);
    }

    fn sar64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(Shift::Asr, W64, dst, src, amount);
    }

    fn branch_add32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.assembler.adds(W32, dst, lhs, rhs, Shift::Lsl, 0);
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch_add32_imm_overflow(&mut self, dst: Gpr, lhs: Gpr, imm: i32, overflow: Label) {
        self.arithmetic_imm_set_flags(ArithmeticOp::Add, W32, dst, lhs, i64::from(imm));
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch_sub32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.assembler.subs(W32, dst, lhs, rhs, Shift::Lsl, 0);
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch_sub32_imm_overflow(&mut self, dst: Gpr, lhs: Gpr, imm: i32, overflow: Label) {
        self.arithmetic_imm_set_flags(ArithmeticOp::Sub, W32, dst, lhs, i64::from(imm));
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch_mul32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.assembler.smull(VALUE_SCRATCH, lhs, rhs);
        self.assembler.check_smull_overflow(VALUE_SCRATCH);
        self.assembler.b_cond(Cond::Ne, overflow);
        self.assembler.mov(W32, dst, VALUE_SCRATCH);
    }

    fn branch_neg32_overflow(&mut self, dst: Gpr, src: Gpr, overflow: Label) {
        self.assembler.negs(W32, dst, src);
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch_add64_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.assembler.adds(W64, dst, lhs, rhs, Shift::Lsl, 0);
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch_sub64_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.assembler.subs(W64, dst, lhs, rhs, Shift::Lsl, 0);
        self.assembler.b_cond(Cond::Vs, overflow);
    }

    fn branch32(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, target: Label) {
        self.assembler.cmp(W32, lhs, rhs);
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn branch32_imm(&mut self, condition: Condition, lhs: Gpr, imm: i32, target: Label) {
        self.branch_compare_imm(W32, condition, lhs, i64::from(imm), target);
    }

    fn branch64(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, target: Label) {
        self.assembler.cmp(W64, lhs, rhs);
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn branch_if_stack_pointer_below(&mut self, limit: &Address, target: Label) {
        // NB: Comparisons of registers read register 31 as the zero
        //     register, so the stack pointer is copied first. Addressing may
        //     use ADDRESS_SCRATCH, which is free once the limit is loaded.
        self.load_store(MemOp::LdrX, VALUE_SCRATCH.0, limit);
        self.load_effective_address(ADDRESS_SCRATCH, &Address::new(SP, 0));
        self.branch64(Condition::Below, ADDRESS_SCRATCH, VALUE_SCRATCH, target);
    }

    fn branch64_imm(&mut self, condition: Condition, lhs: Gpr, imm: i64, target: Label) {
        self.branch_compare_imm(W64, condition, lhs, imm, target);
    }

    fn branch32_memory_imm(&mut self, condition: Condition, address: &Address, imm: i32, target: Label) {
        self.load_store(MemOp::LdrW, ADDRESS_SCRATCH.0, address);
        self.compare_imm(W32, ADDRESS_SCRATCH, i64::from(imm));
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn branch16_memory_imm(&mut self, condition: Condition, address: &Address, imm: u16, target: Label) {
        self.load_store(MemOp::Ldrh, ADDRESS_SCRATCH.0, address);
        self.compare_imm(W32, ADDRESS_SCRATCH, i64::from(imm));
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn branch64_memory(&mut self, condition: Condition, address: &Address, rhs: Gpr, target: Label) {
        self.load_store(MemOp::LdrX, ADDRESS_SCRATCH.0, address);
        self.assembler.cmp(W64, ADDRESS_SCRATCH, rhs);
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn branch64_memory_imm(&mut self, condition: Condition, address: &Address, imm: i64, target: Label) {
        self.load_store(MemOp::LdrX, ADDRESS_SCRATCH.0, address);
        self.compare_imm(W64, ADDRESS_SCRATCH, imm);
        self.assembler.b_cond(condition_code(condition), target);
    }

    fn branch_test32(&mut self, condition: Condition, value: Gpr, mask: u32, target: Label) {
        self.branch_test(W32, condition, value, u64::from(mask), target);
    }

    fn branch_test64(&mut self, condition: Condition, value: Gpr, mask: u64, target: Label) {
        self.branch_test(W64, condition, value, mask, target);
    }

    fn compare32_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.cmp(W32, lhs, rhs);
        self.assembler.cset(W32, dst, condition_code(condition));
    }

    fn compare32_imm_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, imm: i32) {
        self.compare_imm(W32, lhs, i64::from(imm));
        self.assembler.cset(W32, dst, condition_code(condition));
    }

    fn compare64_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.cmp(W64, lhs, rhs);
        self.assembler.cset(W32, dst, condition_code(condition));
    }

    fn compare64_imm_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, imm: i64) {
        self.compare_imm(W64, lhs, imm);
        self.assembler.cset(W32, dst, condition_code(condition));
    }

    fn select64(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, dst: Gpr, if_true: Gpr, if_false: Gpr) {
        self.assembler.cmp(W64, lhs, rhs);
        self.assembler
            .csel(W64, dst, if_true, if_false, condition_code(condition));
    }

    fn jump(&mut self, target: Label) {
        self.assembler.b(target);
    }

    fn jump_absolute(&mut self, address: u64) {
        self.mov_imm(W64, ADDRESS_SCRATCH, address);
        self.assembler.br(ADDRESS_SCRATCH);
    }

    fn call(&mut self, target: Label) {
        self.assembler.bl(target);
    }

    fn call_register(&mut self, target: Gpr) {
        self.assembler.blr(target);
    }

    fn call_absolute(&mut self, address: u64) {
        self.mov_imm(W64, ADDRESS_SCRATCH, address);
        self.assembler.blr(ADDRESS_SCRATCH);
    }

    fn ret(&mut self) {
        self.assembler.ret();
    }

    fn move_double(&mut self, dst: Fpr, src: Fpr) {
        if dst != src {
            self.assembler.fmov(dst, src);
        }
    }

    fn move_double_imm(&mut self, dst: Fpr, value: f64) {
        let bits = value.to_bits();
        if encode_fp_immediate(value).is_some() {
            self.assembler.fmov_imm(dst, value);
        } else if bits == 0 {
            self.assembler.fmov_from_gpr(dst, XZR);
        } else {
            let label = self.constant(bits);
            self.assembler.ldr_literal_double(dst, label);
        }
    }

    fn move_gpr_to_double(&mut self, dst: Fpr, src: Gpr) {
        self.assembler.fmov_from_gpr(dst, src);
    }

    fn move_double_to_gpr(&mut self, dst: Gpr, src: Fpr) {
        self.assembler.fmov_to_gpr(dst, src);
    }

    fn add_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.assembler.fadd(dst, lhs, rhs);
    }

    fn sub_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.assembler.fsub(dst, lhs, rhs);
    }

    fn mul_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.assembler.fmul(dst, lhs, rhs);
    }

    fn div_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.assembler.fdiv(dst, lhs, rhs);
    }

    fn sqrt_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.fsqrt(dst, src);
    }

    fn floor_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.frintm(dst, src);
    }

    fn ceil_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.frintp(dst, src);
    }

    fn neg_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.fneg(dst, src);
    }

    fn abs_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.fabs(dst, src);
    }

    fn convert_int32_to_double(&mut self, dst: Fpr, src: Gpr) {
        self.assembler.scvtf(W32, dst, src);
    }

    fn convert_int64_to_double(&mut self, dst: Fpr, src: Gpr) {
        self.assembler.scvtf(W64, dst, src);
    }

    fn branch_convert_double_to_int32(&mut self, dst: Gpr, src: Fpr, fail: Label, negative_zero: NegativeZero) {
        // fcvtzs saturates and turns NaN into 0, so converting back and
        // comparing catches every inexact case ("ne" includes unordered).
        self.assembler.fcvtzs(W32, dst, src);
        self.assembler.scvtf(W32, FP_SCRATCH, dst);
        self.assembler.fcmp(FP_SCRATCH, src);
        self.assembler.b_cond(Cond::Ne, fail);
        if negative_zero == NegativeZero::Fail {
            let done = self.assembler.new_label();
            self.assembler.cbnz(W32, dst, done);
            self.assembler.fmov_to_gpr(VALUE_SCRATCH, src);
            self.assembler.tbnz(VALUE_SCRATCH, 63, fail);
            self.assembler.bind(done);
        }
    }

    fn truncate_double_to_int64(&mut self, dst: Gpr, src: Fpr) {
        self.assembler.fcvtzs(W64, dst, src);
    }

    fn branch_double(&mut self, condition: DoubleCondition, lhs: Fpr, rhs: Fpr, target: Label) {
        self.assembler.fcmp(lhs, rhs);
        match double_flags(condition) {
            DoubleFlags::Single(cond) => self.assembler.b_cond(cond, target),
            DoubleFlags::Either(first, second) => {
                self.assembler.b_cond(first, target);
                self.assembler.b_cond(second, target);
            }
        }
    }

    fn compare_double_set(&mut self, condition: DoubleCondition, dst: Gpr, lhs: Fpr, rhs: Fpr) {
        self.assembler.fcmp(lhs, rhs);
        self.set_double_condition(condition, dst);
    }

    fn frame(saved_gprs: GprSet, saved_fprs: FprSet, locals_size: u32) -> MachineFrame {
        let saved_gprs = saved_gprs.without(SP).without(FP).without(LR);
        let gpr_area = 16 * saved_gprs.len().div_ceil(2) as u32;
        let fpr_area = 16 * saved_fprs.len().div_ceil(2) as u32;
        let locals_size = locals_size.next_multiple_of(16);
        MachineFrame {
            saved_gprs,
            saved_fprs,
            locals_size,
            caller_stack_offset: locals_size + fpr_area + gpr_area + 16,
        }
    }

    fn emit_prologue(&mut self, frame: &MachineFrame) {
        self.assembler.stp(FP, LR, PairOperand::PreIndex(SP, -16));
        self.assembler.mov_sp(FP, SP);
        let gprs: Vec<u8> = frame.saved_gprs.iter().map(|register| register.0).collect();
        let fprs: Vec<u8> = frame.saved_fprs.iter().map(|register| register.0).collect();
        self.push_registers(&gprs, false);
        self.push_registers(&fprs, true);
        if frame.locals_size != 0 {
            self.arithmetic_imm(ArithmeticOp::Sub, W64, SP, SP, i64::from(frame.locals_size));
        }
    }

    fn emit_epilogue(&mut self, frame: &MachineFrame) {
        if frame.locals_size != 0 {
            self.arithmetic_imm(ArithmeticOp::Add, W64, SP, SP, i64::from(frame.locals_size));
        }
        let gprs: Vec<u8> = frame.saved_gprs.iter().map(|register| register.0).collect();
        let fprs: Vec<u8> = frame.saved_fprs.iter().map(|register| register.0).collect();
        self.pop_registers(&fprs, true);
        self.pop_registers(&gprs, false);
        self.assembler.ldp(FP, LR, PairOperand::PostIndex(SP, 16));
    }
}
