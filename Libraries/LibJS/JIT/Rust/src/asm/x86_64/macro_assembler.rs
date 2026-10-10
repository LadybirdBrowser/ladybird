/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The portable macro assembler on x86-64 (System V ABI).

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

/// General scratch register: immediates, call targets, working values.
const SCRATCH: Gpr = R11;
/// Second scratch register, only used where `SCRATCH` is already taken.
const SCRATCH2: Gpr = R10;
const FP_SCRATCH: Fpr = XMM15;

/// The macro assembler for x86-64. Wraps an `Assembler`, which stays
/// available for architecture specific code.
#[derive(Debug, Default)]
pub struct MacroAssembler {
    pub assembler: Assembler,
    /// 64-bit constants that do not fit an immediate, loaded RIP-relative
    /// from a pool after the code.
    constants: Vec<(u64, Label)>,
    /// 128-bit constants, in the same pool.
    constants128: Vec<(u128, Label)>,
}

fn condition_code(condition: Condition) -> Cond {
    match condition {
        Condition::Equal | Condition::Zero => Cond::Equal,
        Condition::NotEqual | Condition::NonZero => Cond::NotEqual,
        Condition::LessThan => Cond::Less,
        Condition::LessThanOrEqual => Cond::LessOrEqual,
        Condition::GreaterThan => Cond::Greater,
        Condition::GreaterThanOrEqual => Cond::GreaterOrEqual,
        Condition::Below => Cond::Below,
        Condition::BelowOrEqual => Cond::BelowOrEqual,
        Condition::Above => Cond::Above,
        Condition::AboveOrEqual => Cond::AboveOrEqual,
        Condition::Overflow => Cond::Overflow,
        Condition::NoOverflow => Cond::NoOverflow,
        Condition::Negative => Cond::Sign,
        Condition::PositiveOrZero => Cond::NotSign,
    }
}

/// How a double condition maps onto `ucomisd` flags.
enum DoubleFlags {
    /// One condition code, after `ucomisd lhs, rhs` or `ucomisd rhs, lhs`.
    Single { swap: bool, cond: Cond },
    /// Both condition codes must hold.
    Both(Cond, Cond),
    /// Either condition code holds.
    Either(Cond, Cond),
}

fn double_flags(condition: DoubleCondition) -> DoubleFlags {
    use DoubleFlags::*;
    // ucomisd sets ZF,PF,CF to 111 when unordered, 001 for less, 100 for
    // equal and 000 for greater. Ordered "less" conditions swap the operands
    // so they can use CF=0, which excludes unordered.
    match condition {
        DoubleCondition::Equal => Both(Cond::Equal, Cond::NotParity),
        DoubleCondition::NotEqual => Both(Cond::NotEqual, Cond::NotParity),
        DoubleCondition::LessThan => Single {
            swap: true,
            cond: Cond::Above,
        },
        DoubleCondition::LessThanOrEqual => Single {
            swap: true,
            cond: Cond::AboveOrEqual,
        },
        DoubleCondition::GreaterThan => Single {
            swap: false,
            cond: Cond::Above,
        },
        DoubleCondition::GreaterThanOrEqual => Single {
            swap: false,
            cond: Cond::AboveOrEqual,
        },
        DoubleCondition::EqualOrUnordered => Single {
            swap: false,
            cond: Cond::Equal,
        },
        DoubleCondition::NotEqualOrUnordered => Either(Cond::NotEqual, Cond::Parity),
        DoubleCondition::LessThanOrUnordered => Single {
            swap: false,
            cond: Cond::Below,
        },
        DoubleCondition::LessThanOrEqualOrUnordered => Single {
            swap: false,
            cond: Cond::BelowOrEqual,
        },
        DoubleCondition::GreaterThanOrUnordered => Single {
            swap: true,
            cond: Cond::Below,
        },
        DoubleCondition::GreaterThanOrEqualOrUnordered => Single {
            swap: true,
            cond: Cond::BelowOrEqual,
        },
        DoubleCondition::Ordered => Single {
            swap: false,
            cond: Cond::NotParity,
        },
        DoubleCondition::Unordered => Single {
            swap: false,
            cond: Cond::Parity,
        },
    }
}

impl MacroAssembler {
    /// `dst = lhs op rhs` for a commutative operation.
    fn commutative(&mut self, op: AluOp, width: Width, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        if dst == lhs {
            self.assembler.alu_rr(op, width, dst, rhs);
        } else if dst == rhs {
            self.assembler.alu_rr(op, width, dst, lhs);
        } else {
            self.assembler.mov_rr(width, dst, lhs);
            self.assembler.alu_rr(op, width, dst, rhs);
        }
    }

    /// `dst = lhs - rhs`, leaving the flags of the subtraction.
    fn subtract(&mut self, width: Width, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        if dst == lhs {
            self.assembler.sub_rr(width, dst, rhs);
        } else if dst == rhs {
            self.assembler.mov_rr(width, SCRATCH, lhs);
            self.assembler.sub_rr(width, SCRATCH, rhs);
            self.assembler.mov_rr(width, dst, SCRATCH);
        } else {
            self.assembler.mov_rr(width, dst, lhs);
            self.assembler.sub_rr(width, dst, rhs);
        }
    }

    /// `dst = lhs op imm`, with `imm` already in the instruction's range.
    fn with_imm32(&mut self, op: AluOp, width: Width, dst: Gpr, lhs: Gpr, imm: i32) {
        if dst != lhs {
            self.assembler.mov_rr(width, dst, lhs);
        }
        self.assembler.alu_ri(op, width, dst, imm);
    }

    /// `dst = lhs op imm` for a 64-bit operation with an arbitrary immediate.
    fn with_imm64(&mut self, op: AluOp, dst: Gpr, lhs: Gpr, imm: i64) {
        if let Ok(imm) = i32::try_from(imm) {
            self.with_imm32(op, W64, dst, lhs, imm);
        } else {
            // NB: The immediate is an operand in the constant pool.
            self.mov_if_needed(W64, dst, lhs);
            let label = self.constant(imm as u64);
            self.assembler.alu_r_label(op, dst, label);
        }
    }

    fn mov_if_needed(&mut self, width: Width, dst: Gpr, src: Gpr) {
        if dst != src {
            self.assembler.mov_rr(width, dst, src);
        }
    }

    fn shift_imm(&mut self, op: ShiftOp, width: Width, dst: Gpr, src: Gpr, amount: u8) {
        let amount = amount & (width.bits() as u8 - 1);
        if amount == 0 {
            // A 32-bit move still zero-extends, even onto itself.
            if dst != src || width == W32 {
                self.assembler.mov_rr(width, dst, src);
            }
            return;
        }
        self.mov_if_needed(width, dst, src);
        self.assembler.shift_ri(op, width, dst, amount);
    }

    /// Shifts by a register amount, which x86 needs in CL.
    fn shift_register(&mut self, op: ShiftOp, width: Width, dst: Gpr, src: Gpr, amount: Gpr) {
        if amount == RCX && dst != RCX {
            self.mov_if_needed(width, dst, src);
            self.assembler.shift_rcl(op, width, dst);
            return;
        }
        self.assembler.mov_rr(W64, SCRATCH, src);
        if amount != RCX {
            self.assembler.mov_rr(W64, SCRATCH2, RCX);
            self.assembler.mov_rr(W64, RCX, amount);
        }
        self.assembler.shift_rcl(op, width, SCRATCH);
        if amount != RCX {
            self.assembler.mov_rr(W64, RCX, SCRATCH2);
        }
        self.assembler.mov_rr(width, dst, SCRATCH);
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

    fn compare_imm(&mut self, width: Width, lhs: Gpr, imm: i64) {
        if imm == 0 {
            self.assembler.test_rr(width, lhs, lhs);
        } else if let Ok(imm) = i32::try_from(imm) {
            self.assembler.cmp_ri(width, lhs, imm);
        } else {
            let label = self.constant(imm as u64);
            self.assembler.cmp_r_label(lhs, label);
        }
    }

    fn test_mask(&mut self, width: Width, value: Gpr, mask: u64) {
        let all_ones = match width {
            W32 => u64::from(u32::MAX),
            W64 => u64::MAX,
        };
        if mask == all_ones {
            self.assembler.test_rr(width, value, value);
        } else if width == W32 {
            self.assembler.test_ri(W32, value, mask as u32 as i32);
        } else if let Ok(imm) = i32::try_from(mask as i64) {
            self.assembler.test_ri(W64, value, imm);
        } else {
            self.move_imm64(SCRATCH, mask);
            self.assembler.test_rr(W64, value, SCRATCH);
        }
    }

    fn set_condition(&mut self, cond: Cond, dst: Gpr) {
        self.assembler.setcc(cond, dst);
        self.assembler.movzx8_rr(dst, dst);
    }

    fn compare_doubles(&mut self, swap: bool, lhs: Fpr, rhs: Fpr) {
        if swap {
            self.assembler.ucomisd(rhs, lhs);
        } else {
            self.assembler.ucomisd(lhs, rhs);
        }
    }

    /// `dst = lhs op rhs` for an SSE operation where `dst` must be the left operand.
    fn double_operation(
        &mut self,
        operation: fn(&mut Assembler, Fpr, Fpr),
        commutative: bool,
        dst: Fpr,
        lhs: Fpr,
        rhs: Fpr,
    ) {
        if dst == lhs {
            operation(&mut self.assembler, dst, rhs);
        } else if dst == rhs {
            if commutative {
                operation(&mut self.assembler, dst, lhs);
            } else {
                self.assembler.movapd_rr(FP_SCRATCH, lhs);
                operation(&mut self.assembler, FP_SCRATCH, rhs);
                self.assembler.movapd_rr(dst, FP_SCRATCH);
            }
        } else {
            self.assembler.movapd_rr(dst, lhs);
            operation(&mut self.assembler, dst, rhs);
        }
    }
}

impl PortableMacroAssembler for MacroAssembler {
    const ARCHITECTURE: Architecture = Architecture::X86_64;
    const ARGUMENT_GPRS: &'static [Gpr] = &[RDI, RSI, RDX, RCX, R8, R9];
    const ARGUMENT_FPRS: &'static [Fpr] = &[XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7];
    const RETURN_GPRS: [Gpr; 2] = [RAX, RDX];
    const CALLEE_SAVED_GPRS: GprSet = GprSet::of(&[RBX, RBP, R12, R13, R14, R15]);
    const CALLEE_SAVED_FPRS: FprSet = FprSet::EMPTY;
    const CALLER_SAVED_GPRS: GprSet = GprSet::of(&[RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11]);
    const CALLER_SAVED_FPRS: FprSet = FprSet(0xffff);
    const SCRATCH_GPRS: GprSet = GprSet::of(&[SCRATCH, SCRATCH2]);
    const SCRATCH_FPRS: FprSet = FprSet::of(&[FP_SCRATCH]);
    const ALLOCATABLE_GPRS: GprSet = GprSet::of(&[RAX, RCX, RDX, RBX, RSI, RDI, R8, R9, R12, R13, R14, R15]);
    const ALLOCATABLE_FPRS: FprSet = FprSet(0x7fff);
    const STACK_POINTER: Gpr = RSP;
    const FRAME_POINTER: Gpr = RBP;

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
        // NB: As long as `jmp rel32`.
        self.assembler.nops(5);
        offset
    }

    fn jump_patch(from: usize, to: usize) -> Vec<u8> {
        let displacement = i32::try_from(to as i64 - (from as i64 + 5)).expect("code is smaller than 2 GiB");
        let mut bytes = vec![0xe9];
        bytes.extend_from_slice(&displacement.to_le_bytes());
        bytes
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
        self.assembler.ud2();
    }

    fn move64(&mut self, dst: Gpr, src: Gpr) {
        self.mov_if_needed(W64, dst, src);
    }

    fn move32(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.mov_rr(W32, dst, src);
    }

    fn move_imm64(&mut self, dst: Gpr, imm: u64) {
        if u32::try_from(imm).is_ok() || i32::try_from(imm as i64).is_ok() {
            self.assembler.mov_ri(dst, imm);
        } else {
            let label = self.constant(imm);
            self.assembler.mov_r_label(dst, label);
        }
    }

    fn move_imm32(&mut self, dst: Gpr, imm: u32) {
        self.assembler.mov_ri32(dst, imm);
    }

    fn sign_extend32_to_64(&mut self, dst: Gpr, src: Gpr) {
        self.assembler.movsxd_rr(dst, src);
    }

    fn load8(&mut self, dst: Gpr, address: &Address) {
        self.assembler.movzx8_rm(dst, address);
    }

    fn load8_sign_extend(&mut self, dst: Gpr, address: &Address) {
        self.assembler.movsx8_rm(W64, dst, address);
    }

    fn load16(&mut self, dst: Gpr, address: &Address) {
        self.assembler.movzx16_rm(dst, address);
    }

    fn load16_sign_extend(&mut self, dst: Gpr, address: &Address) {
        self.assembler.movsx16_rm(W64, dst, address);
    }

    fn load32(&mut self, dst: Gpr, address: &Address) {
        self.assembler.mov_rm(W32, dst, address);
    }

    fn load32_sign_extend(&mut self, dst: Gpr, address: &Address) {
        self.assembler.movsxd_rm(dst, address);
    }

    fn load64(&mut self, dst: Gpr, address: &Address) {
        self.assembler.mov_rm(W64, dst, address);
    }

    fn store8(&mut self, address: &Address, src: Gpr) {
        self.assembler.mov8_mr(address, src);
    }

    fn store16(&mut self, address: &Address, src: Gpr) {
        self.assembler.mov16_mr(address, src);
    }

    fn store32(&mut self, address: &Address, src: Gpr) {
        self.assembler.mov_mr(W32, address, src);
    }

    fn store64(&mut self, address: &Address, src: Gpr) {
        self.assembler.mov_mr(W64, address, src);
    }

    fn store_imm32(&mut self, address: &Address, imm: u32) {
        self.assembler.mov_mi(W32, address, imm as i32);
    }

    fn store_imm64(&mut self, address: &Address, imm: u64) {
        if let Ok(imm) = i32::try_from(imm as i64) {
            self.assembler.mov_mi(W64, address, imm);
        } else {
            self.move_imm64(SCRATCH, imm);
            self.assembler.mov_mr(W64, address, SCRATCH);
        }
    }

    fn add32_to_memory_imm(&mut self, address: &Address, imm: i32) {
        self.assembler.alu_mi(AluOp::Add, W32, address, imm);
    }

    fn add64_to_memory_imm(&mut self, address: &Address, imm: i32) {
        self.assembler.alu_mi(AluOp::Add, W64, address, imm);
    }

    fn load_effective_address(&mut self, dst: Gpr, address: &Address) {
        self.assembler.lea(W64, dst, address);
    }

    fn load_double(&mut self, dst: Fpr, address: &Address) {
        self.assembler.movsd_rm(dst, address);
    }

    fn store_double(&mut self, address: &Address, src: Fpr) {
        self.assembler.movsd_mr(address, src);
    }

    fn load_imm128(&mut self, dst: Fpr, imm: u128) {
        let label = self.constant128(imm);
        self.assembler.movups_r_label(dst, label);
    }

    fn store128(&mut self, address: &Address, src: Fpr) {
        self.assembler.movups_mr(address, src);
    }

    fn load_float_as_double(&mut self, dst: Fpr, address: &Address) {
        self.assembler.movss_rm(dst, address);
        self.assembler.cvtss2sd(dst, dst);
    }

    fn store_double_as_float(&mut self, address: &Address, src: Fpr) {
        self.assembler.cvtsd2ss(FP_SCRATCH, src);
        self.assembler.movss_mr(address, FP_SCRATCH);
    }

    fn add32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::Add, W32, dst, lhs, rhs);
    }

    fn add32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i32) {
        if dst == lhs {
            self.assembler.add_ri(W32, dst, imm);
        } else {
            self.assembler.lea(W32, dst, &Address::new(lhs, imm));
        }
    }

    fn add64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::Add, W64, dst, lhs, rhs);
    }

    fn add64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i64) {
        match i32::try_from(imm) {
            Ok(imm) if dst != lhs => self.assembler.lea(W64, dst, &Address::new(lhs, imm)),
            _ => self.with_imm64(AluOp::Add, dst, lhs, imm),
        }
    }

    fn sub32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.subtract(W32, dst, lhs, rhs);
    }

    fn sub32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i32) {
        self.with_imm32(AluOp::Sub, W32, dst, lhs, imm);
    }

    fn sub64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.subtract(W64, dst, lhs, rhs);
    }

    fn sub64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i64) {
        self.with_imm64(AluOp::Sub, dst, lhs, imm);
    }

    fn and32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::And, W32, dst, lhs, rhs);
    }

    fn and32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.with_imm32(AluOp::And, W32, dst, lhs, imm as i32);
    }

    fn and64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::And, W64, dst, lhs, rhs);
    }

    fn and64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64) {
        if imm == u64::from(u32::MAX) {
            // Zero-extending move.
            self.assembler.mov_rr(W32, dst, lhs);
        } else {
            self.with_imm64(AluOp::And, dst, lhs, imm as i64);
        }
    }

    fn or32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::Or, W32, dst, lhs, rhs);
    }

    fn or32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.with_imm32(AluOp::Or, W32, dst, lhs, imm as i32);
    }

    fn or64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::Or, W64, dst, lhs, rhs);
    }

    fn or64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64) {
        self.with_imm64(AluOp::Or, dst, lhs, imm as i64);
    }

    fn xor32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::Xor, W32, dst, lhs, rhs);
    }

    fn xor32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.with_imm32(AluOp::Xor, W32, dst, lhs, imm as i32);
    }

    fn xor64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.commutative(AluOp::Xor, W64, dst, lhs, rhs);
    }

    fn xor64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64) {
        self.with_imm64(AluOp::Xor, dst, lhs, imm as i64);
    }

    fn mul32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        if dst == rhs {
            self.assembler.imul_rr(W32, dst, lhs);
        } else {
            self.mov_if_needed(W32, dst, lhs);
            self.assembler.imul_rr(W32, dst, rhs);
        }
    }

    fn mul32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32) {
        self.assembler.imul_rri(W32, dst, lhs, imm as i32);
    }

    fn mul64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        if dst == rhs {
            self.assembler.imul_rr(W64, dst, lhs);
        } else {
            self.mov_if_needed(W64, dst, lhs);
            self.assembler.imul_rr(W64, dst, rhs);
        }
    }

    fn neg32(&mut self, dst: Gpr, src: Gpr) {
        self.mov_if_needed(W32, dst, src);
        self.assembler.neg(W32, dst);
    }

    fn neg64(&mut self, dst: Gpr, src: Gpr) {
        self.mov_if_needed(W64, dst, src);
        self.assembler.neg(W64, dst);
    }

    fn not32(&mut self, dst: Gpr, src: Gpr) {
        self.mov_if_needed(W32, dst, src);
        self.assembler.not(W32, dst);
    }

    fn not64(&mut self, dst: Gpr, src: Gpr) {
        self.mov_if_needed(W64, dst, src);
        self.assembler.not(W64, dst);
    }

    fn shl32(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.shift_register(ShiftOp::Shl, W32, dst, src, amount);
    }

    fn shl32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(ShiftOp::Shl, W32, dst, src, amount);
    }

    fn shl64(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.shift_register(ShiftOp::Shl, W64, dst, src, amount);
    }

    fn shl64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(ShiftOp::Shl, W64, dst, src, amount);
    }

    fn shr32(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.shift_register(ShiftOp::Shr, W32, dst, src, amount);
    }

    fn shr32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(ShiftOp::Shr, W32, dst, src, amount);
    }

    fn shr64(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.shift_register(ShiftOp::Shr, W64, dst, src, amount);
    }

    fn shr64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(ShiftOp::Shr, W64, dst, src, amount);
    }

    fn sar32(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.shift_register(ShiftOp::Sar, W32, dst, src, amount);
    }

    fn sar32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(ShiftOp::Sar, W32, dst, src, amount);
    }

    fn sar64(&mut self, dst: Gpr, src: Gpr, amount: Gpr) {
        self.shift_register(ShiftOp::Sar, W64, dst, src, amount);
    }

    fn sar64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8) {
        self.shift_imm(ShiftOp::Sar, W64, dst, src, amount);
    }

    fn branch_add32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.commutative(AluOp::Add, W32, dst, lhs, rhs);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_add32_imm_overflow(&mut self, dst: Gpr, lhs: Gpr, imm: i32, overflow: Label) {
        self.with_imm32(AluOp::Add, W32, dst, lhs, imm);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_sub32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.subtract(W32, dst, lhs, rhs);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_sub32_imm_overflow(&mut self, dst: Gpr, lhs: Gpr, imm: i32, overflow: Label) {
        self.with_imm32(AluOp::Sub, W32, dst, lhs, imm);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_mul32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.mul32(dst, lhs, rhs);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_neg32_overflow(&mut self, dst: Gpr, src: Gpr, overflow: Label) {
        self.neg32(dst, src);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_add64_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.commutative(AluOp::Add, W64, dst, lhs, rhs);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch_sub64_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label) {
        self.subtract(W64, dst, lhs, rhs);
        self.assembler.jcc(Cond::Overflow, overflow);
    }

    fn branch32(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, target: Label) {
        self.assembler.cmp_rr(W32, lhs, rhs);
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch32_imm(&mut self, condition: Condition, lhs: Gpr, imm: i32, target: Label) {
        self.compare_imm(W32, lhs, i64::from(imm));
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch64(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, target: Label) {
        self.assembler.cmp_rr(W64, lhs, rhs);
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch_if_stack_pointer_below(&mut self, limit: &Address, target: Label) {
        self.branch64_memory(Condition::Above, limit, RSP, target);
    }

    fn branch64_imm(&mut self, condition: Condition, lhs: Gpr, imm: i64, target: Label) {
        self.compare_imm(W64, lhs, imm);
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch32_memory_imm(&mut self, condition: Condition, address: &Address, imm: i32, target: Label) {
        self.assembler.cmp_mi(W32, address, imm);
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch16_memory_imm(&mut self, condition: Condition, address: &Address, imm: u16, target: Label) {
        // NB: 16-bit compares only see the low 16 bits, so a sign-extended
        //     imm8 matches the zero-extended value if it has the same bits.
        match i8::try_from(imm as i16) {
            Ok(imm8) if matches!(condition, Condition::Equal | Condition::NotEqual) => {
                self.assembler.cmp16_mi8(address, imm8);
            }
            _ => {
                self.assembler.movzx16_rm(SCRATCH, address);
                self.assembler.cmp_ri(W32, SCRATCH, i32::from(imm));
            }
        }
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch64_memory(&mut self, condition: Condition, address: &Address, rhs: Gpr, target: Label) {
        self.assembler.cmp_mr(W64, address, rhs);
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch64_memory_imm(&mut self, condition: Condition, address: &Address, imm: i64, target: Label) {
        if let Ok(imm) = i32::try_from(imm) {
            self.assembler.cmp_mi(W64, address, imm);
        } else {
            self.move_imm64(SCRATCH, imm as u64);
            self.assembler.cmp_mr(W64, address, SCRATCH);
        }
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch_test32(&mut self, condition: Condition, value: Gpr, mask: u32, target: Label) {
        self.test_mask(W32, value, u64::from(mask));
        self.assembler.jcc(condition_code(condition), target);
    }

    fn branch_test64(&mut self, condition: Condition, value: Gpr, mask: u64, target: Label) {
        self.test_mask(W64, value, mask);
        self.assembler.jcc(condition_code(condition), target);
    }

    fn compare32_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.cmp_rr(W32, lhs, rhs);
        self.set_condition(condition_code(condition), dst);
    }

    fn compare32_imm_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, imm: i32) {
        self.compare_imm(W32, lhs, i64::from(imm));
        self.set_condition(condition_code(condition), dst);
    }

    fn compare64_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, rhs: Gpr) {
        self.assembler.cmp_rr(W64, lhs, rhs);
        self.set_condition(condition_code(condition), dst);
    }

    fn compare64_imm_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, imm: i64) {
        self.compare_imm(W64, lhs, imm);
        self.set_condition(condition_code(condition), dst);
    }

    fn select64(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, dst: Gpr, if_true: Gpr, if_false: Gpr) {
        let cond = condition_code(condition);
        self.assembler.cmp_rr(W64, lhs, rhs);
        if dst == if_true {
            self.assembler.cmovcc(cond.invert(), W64, dst, if_false);
        } else {
            // A plain mov does not change the flags.
            self.mov_if_needed(W64, dst, if_false);
            self.assembler.cmovcc(cond, W64, dst, if_true);
        }
    }

    fn jump(&mut self, target: Label) {
        self.assembler.jmp(target);
    }

    fn jump_absolute(&mut self, address: u64) {
        self.move_imm64(SCRATCH, address);
        self.assembler.jmp_r(SCRATCH);
    }

    fn call(&mut self, target: Label) {
        self.assembler.call(target);
    }

    fn call_register(&mut self, target: Gpr) {
        self.assembler.call_r(target);
    }

    fn call_absolute(&mut self, address: u64) {
        let label = self.constant(address);
        self.assembler.call_label_indirect(label);
    }

    fn ret(&mut self) {
        self.assembler.ret();
    }

    fn move_double(&mut self, dst: Fpr, src: Fpr) {
        if dst != src {
            self.assembler.movapd_rr(dst, src);
        }
    }

    fn move_double_imm(&mut self, dst: Fpr, value: f64) {
        let bits = value.to_bits();
        if bits == 0 {
            self.assembler.xorpd(dst, dst);
        } else {
            let label = self.constant(bits);
            self.assembler.movsd_r_label(dst, label);
        }
    }

    fn move_gpr_to_double(&mut self, dst: Fpr, src: Gpr) {
        self.assembler.movq_xr(dst, src);
    }

    fn move_double_to_gpr(&mut self, dst: Gpr, src: Fpr) {
        self.assembler.movq_rx(dst, src);
    }

    fn add_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.double_operation(Assembler::addsd, true, dst, lhs, rhs);
    }

    fn sub_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.double_operation(Assembler::subsd, false, dst, lhs, rhs);
    }

    fn mul_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.double_operation(Assembler::mulsd, true, dst, lhs, rhs);
    }

    fn div_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.double_operation(Assembler::divsd, false, dst, lhs, rhs);
    }

    fn sqrt_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.sqrtsd(dst, src);
    }

    fn floor_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.roundsd(dst, src, 0b1001);
    }

    fn ceil_double(&mut self, dst: Fpr, src: Fpr) {
        self.assembler.roundsd(dst, src, 0b1010);
    }

    fn neg_double(&mut self, dst: Fpr, src: Fpr) {
        let label = self.constant(1 << 63);
        self.assembler.movsd_r_label(FP_SCRATCH, label);
        self.move_double(dst, src);
        self.assembler.xorpd(dst, FP_SCRATCH);
    }

    fn abs_double(&mut self, dst: Fpr, src: Fpr) {
        let label = self.constant(!(1 << 63));
        self.assembler.movsd_r_label(FP_SCRATCH, label);
        self.move_double(dst, src);
        self.assembler.andpd(dst, FP_SCRATCH);
    }

    fn convert_int32_to_double(&mut self, dst: Fpr, src: Gpr) {
        // Break the dependency on the old value of dst.
        self.assembler.xorpd(dst, dst);
        self.assembler.cvtsi2sd(W32, dst, src);
    }

    fn convert_int64_to_double(&mut self, dst: Fpr, src: Gpr) {
        self.assembler.xorpd(dst, dst);
        self.assembler.cvtsi2sd(W64, dst, src);
    }

    fn branch_convert_double_to_int32(&mut self, dst: Gpr, src: Fpr, fail: Label, negative_zero: NegativeZero) {
        self.assembler.cvttsd2si(W32, dst, src);
        self.assembler.xorpd(FP_SCRATCH, FP_SCRATCH);
        self.assembler.cvtsi2sd(W32, FP_SCRATCH, dst);
        self.assembler.ucomisd(FP_SCRATCH, src);
        self.assembler.jcc(Cond::NotEqual, fail);
        self.assembler.jcc(Cond::Parity, fail);
        if negative_zero == NegativeZero::Fail {
            let done = self.assembler.new_label();
            self.assembler.test_rr(W32, dst, dst);
            self.assembler.jcc(Cond::NotEqual, done);
            self.assembler.movmskpd(SCRATCH, src);
            self.assembler.test_ri(W32, SCRATCH, 1);
            self.assembler.jcc(Cond::NotEqual, fail);
            self.assembler.bind(done);
        }
    }

    fn truncate_double_to_int64(&mut self, dst: Gpr, src: Fpr) {
        self.assembler.cvttsd2si(W64, dst, src);
    }

    fn branch_double(&mut self, condition: DoubleCondition, lhs: Fpr, rhs: Fpr, target: Label) {
        match double_flags(condition) {
            DoubleFlags::Single { swap, cond } => {
                self.compare_doubles(swap, lhs, rhs);
                self.assembler.jcc(cond, target);
            }
            DoubleFlags::Both(first, second) => {
                let skip = self.assembler.new_label();
                self.compare_doubles(false, lhs, rhs);
                self.assembler.jcc(second.invert(), skip);
                self.assembler.jcc(first, target);
                self.assembler.bind(skip);
            }
            DoubleFlags::Either(first, second) => {
                self.compare_doubles(false, lhs, rhs);
                self.assembler.jcc(second, target);
                self.assembler.jcc(first, target);
            }
        }
    }

    fn compare_double_set(&mut self, condition: DoubleCondition, dst: Gpr, lhs: Fpr, rhs: Fpr) {
        match double_flags(condition) {
            DoubleFlags::Single { swap, cond } => {
                self.compare_doubles(swap, lhs, rhs);
                self.set_condition(cond, dst);
            }
            DoubleFlags::Both(first, second) => {
                self.compare_doubles(false, lhs, rhs);
                self.assembler.setcc(first, dst);
                self.assembler.setcc(second, SCRATCH);
                self.assembler.and_rr(W32, dst, SCRATCH);
                self.assembler.movzx8_rr(dst, dst);
            }
            DoubleFlags::Either(first, second) => {
                self.compare_doubles(false, lhs, rhs);
                self.assembler.setcc(first, dst);
                self.assembler.setcc(second, SCRATCH);
                self.assembler.or_rr(W32, dst, SCRATCH);
                self.assembler.movzx8_rr(dst, dst);
            }
        }
    }

    fn frame(saved_gprs: GprSet, saved_fprs: FprSet, locals_size: u32) -> MachineFrame {
        let saved_gprs = saved_gprs.without(RSP).without(RBP);
        let pushed = 8 * saved_gprs.len() as u32;
        let fpr_area = 8 * saved_fprs.len() as u32;
        // After `push rbp` the stack is 16-byte aligned; keep it that way.
        let mut locals_size = locals_size.next_multiple_of(8);
        if !(pushed + fpr_area + locals_size).is_multiple_of(16) {
            locals_size += 8;
        }
        MachineFrame {
            saved_gprs,
            saved_fprs,
            locals_size,
            caller_stack_offset: locals_size + fpr_area + pushed + 16,
        }
    }

    fn emit_prologue(&mut self, frame: &MachineFrame) {
        self.assembler.push(RBP);
        self.assembler.mov_rr(W64, RBP, RSP);
        for register in frame.saved_gprs.iter() {
            self.assembler.push(register);
        }
        let reserved = frame.locals_size + 8 * frame.saved_fprs.len() as u32;
        if reserved != 0 {
            self.assembler.sub_ri(W64, RSP, reserved as i32);
        }
        for (i, register) in frame.saved_fprs.iter().enumerate() {
            let offset = frame.locals_size + 8 * i as u32;
            self.assembler.movsd_mr(&Address::new(RSP, offset as i32), register);
        }
    }

    fn emit_epilogue(&mut self, frame: &MachineFrame) {
        for (i, register) in frame.saved_fprs.iter().enumerate() {
            let offset = frame.locals_size + 8 * i as u32;
            self.assembler.movsd_rm(register, &Address::new(RSP, offset as i32));
        }
        let reserved = frame.locals_size + 8 * frame.saved_fprs.len() as u32;
        if reserved != 0 {
            self.assembler.add_ri(W64, RSP, reserved as i32);
        }
        for register in frame.saved_gprs.iter().rev() {
            self.assembler.pop(register);
        }
        self.assembler.pop(RBP);
    }
}
