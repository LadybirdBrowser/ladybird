/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AArch64 (A64) instruction encoder.

mod macro_assembler;
#[cfg(test)]
mod tests;

pub use macro_assembler::MacroAssembler;

use super::Fpr;
use super::Gpr;
use super::Width;
use super::buffer::AsmError;
use super::buffer::CodeBuffer;
use super::buffer::FixupKind;
use super::buffer::Label;

pub const X0: Gpr = Gpr(0);
pub const X1: Gpr = Gpr(1);
pub const X2: Gpr = Gpr(2);
pub const X3: Gpr = Gpr(3);
pub const X4: Gpr = Gpr(4);
pub const X5: Gpr = Gpr(5);
pub const X6: Gpr = Gpr(6);
pub const X7: Gpr = Gpr(7);
pub const X8: Gpr = Gpr(8);
pub const X9: Gpr = Gpr(9);
pub const X10: Gpr = Gpr(10);
pub const X11: Gpr = Gpr(11);
pub const X12: Gpr = Gpr(12);
pub const X13: Gpr = Gpr(13);
pub const X14: Gpr = Gpr(14);
pub const X15: Gpr = Gpr(15);
pub const X16: Gpr = Gpr(16);
pub const X17: Gpr = Gpr(17);
pub const X18: Gpr = Gpr(18);
pub const X19: Gpr = Gpr(19);
pub const X20: Gpr = Gpr(20);
pub const X21: Gpr = Gpr(21);
pub const X22: Gpr = Gpr(22);
pub const X23: Gpr = Gpr(23);
pub const X24: Gpr = Gpr(24);
pub const X25: Gpr = Gpr(25);
pub const X26: Gpr = Gpr(26);
pub const X27: Gpr = Gpr(27);
pub const X28: Gpr = Gpr(28);
/// The frame pointer.
pub const X29: Gpr = Gpr(29);
/// The link register.
pub const X30: Gpr = Gpr(30);
/// Encoding 31 where the instruction treats it as the stack pointer.
pub const SP: Gpr = Gpr(31);
/// Encoding 31 where the instruction treats it as the zero register.
pub const XZR: Gpr = Gpr(31);
pub const FP: Gpr = X29;
pub const LR: Gpr = X30;

pub const D0: Fpr = Fpr(0);
pub const D1: Fpr = Fpr(1);
pub const D2: Fpr = Fpr(2);
pub const D3: Fpr = Fpr(3);
pub const D4: Fpr = Fpr(4);
pub const D5: Fpr = Fpr(5);
pub const D6: Fpr = Fpr(6);
pub const D7: Fpr = Fpr(7);
pub const D8: Fpr = Fpr(8);
pub const D9: Fpr = Fpr(9);
pub const D10: Fpr = Fpr(10);
pub const D11: Fpr = Fpr(11);
pub const D12: Fpr = Fpr(12);
pub const D13: Fpr = Fpr(13);
pub const D14: Fpr = Fpr(14);
pub const D15: Fpr = Fpr(15);
pub const D16: Fpr = Fpr(16);
pub const D17: Fpr = Fpr(17);
pub const D29: Fpr = Fpr(29);
pub const D30: Fpr = Fpr(30);
pub const D31: Fpr = Fpr(31);

/// Condition codes, by their encoding.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cond {
    Eq = 0x0,
    Ne = 0x1,
    /// Carry set / unsigned higher or same.
    Hs = 0x2,
    /// Carry clear / unsigned lower.
    Lo = 0x3,
    Mi = 0x4,
    Pl = 0x5,
    Vs = 0x6,
    Vc = 0x7,
    Hi = 0x8,
    Ls = 0x9,
    Ge = 0xa,
    Lt = 0xb,
    Gt = 0xc,
    Le = 0xd,
    Al = 0xe,
}

impl Cond {
    pub const fn invert(self) -> Self {
        match self {
            Cond::Eq => Cond::Ne,
            Cond::Ne => Cond::Eq,
            Cond::Hs => Cond::Lo,
            Cond::Lo => Cond::Hs,
            Cond::Mi => Cond::Pl,
            Cond::Pl => Cond::Mi,
            Cond::Vs => Cond::Vc,
            Cond::Vc => Cond::Vs,
            Cond::Hi => Cond::Ls,
            Cond::Ls => Cond::Hi,
            Cond::Ge => Cond::Lt,
            Cond::Lt => Cond::Ge,
            Cond::Gt => Cond::Le,
            Cond::Le => Cond::Gt,
            Cond::Al => panic!("AL has no inverse"),
        }
    }
}

/// Shift applied to the second register of a data processing instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shift {
    Lsl = 0,
    Lsr = 1,
    Asr = 2,
    Ror = 3,
}

/// Extension applied to a register operand.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Extend {
    Uxtb = 0,
    Uxth = 1,
    Uxtw = 2,
    /// Also written LSL when the other operand is a 64-bit register or SP.
    Uxtx = 3,
    Sxtb = 4,
    Sxth = 5,
    Sxtw = 6,
    Sxtx = 7,
}

/// A single-register load or store, by its unscaled-immediate base encoding.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum MemOp {
    Strb = 0x3800_0000,
    Ldrb = 0x3840_0000,
    /// Sign-extends a byte into an X register.
    LdrsbX = 0x3880_0000,
    /// Sign-extends a byte into a W register.
    LdrsbW = 0x38c0_0000,
    Strh = 0x7800_0000,
    Ldrh = 0x7840_0000,
    LdrshX = 0x7880_0000,
    LdrshW = 0x78c0_0000,
    StrW = 0xb800_0000,
    LdrW = 0xb840_0000,
    Ldrsw = 0xb880_0000,
    StrX = 0xf800_0000,
    LdrX = 0xf840_0000,
    StrS = 0xbc00_0000,
    LdrS = 0xbc40_0000,
    StrD = 0xfc00_0000,
    LdrD = 0xfc40_0000,
    /// 128-bit SIMD&FP registers, whose size is not in the size field.
    StrQ = 0x3c80_0000,
    LdrQ = 0x3cc0_0000,
}

impl MemOp {
    /// log2 of the access size in bytes.
    pub const fn size_log2(self) -> u32 {
        match self {
            MemOp::StrQ | MemOp::LdrQ => 4,
            _ => (self as u32) >> 30,
        }
    }
}

/// The addressing mode of a single-register load or store.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemOperand {
    /// `[base, #offset]`: the scaled unsigned 12-bit form when the offset is
    /// aligned and in range, else the unscaled signed 9-bit form.
    Offset(Gpr, i32),
    /// `[base, index{, extend {#log2(size)}}]`. `base` may be SP.
    Register {
        base: Gpr,
        index: Gpr,
        extend: Extend,
        shifted: bool,
    },
    /// `[base, #offset]!`
    PreIndex(Gpr, i32),
    /// `[base], #offset`
    PostIndex(Gpr, i32),
}

/// The addressing mode of a load or store pair.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PairOperand {
    Offset(Gpr, i32),
    PreIndex(Gpr, i32),
    PostIndex(Gpr, i32),
}

/// The N:immr:imms field of a logical immediate encoding `imm` for `width`,
/// or `None` if `imm` is not a valid bitmask immediate.
pub fn encode_logical_immediate(imm: u64, width: Width) -> Option<u32> {
    fn is_mask(value: u64) -> bool {
        value != 0 && (value.wrapping_add(1) & value) == 0
    }
    fn is_shifted_mask(value: u64) -> bool {
        value != 0 && is_mask((value - 1) | value)
    }

    let register_size = width.bits();
    if imm == 0 || imm == u64::MAX {
        return None;
    }
    if register_size == 32 && (imm >> 32 != 0 || imm == u64::from(u32::MAX)) {
        return None;
    }

    // Find the smallest repeating element.
    let mut size = register_size;
    loop {
        size /= 2;
        let mask = (1u64 << size) - 1;
        if (imm & mask) != ((imm >> size) & mask) {
            size *= 2;
            break;
        }
        if size <= 2 {
            break;
        }
    }

    // Find the rotation and the number of trailing ones of the element.
    let mask = u64::MAX >> (64 - size);
    let mut element = imm & mask;
    let (rotation, trailing_ones) = if is_shifted_mask(element) {
        let rotation = element.trailing_zeros();
        (rotation, (element >> rotation).trailing_ones())
    } else {
        element |= !mask;
        if !is_shifted_mask(!element) {
            return None;
        }
        let leading_ones = element.leading_ones();
        (64 - leading_ones, leading_ones + element.trailing_ones() - (64 - size))
    };

    let immr = (size - rotation) & (size - 1);
    // imms encodes the element size in its leading ones (with N for 64-bit
    // elements) and the run length in the remaining bits.
    let nimms = ((!(size - 1)) << 1) | (trailing_ones - 1);
    let n = ((nimms >> 6) & 1) ^ 1;
    Some((n << 12) | (immr << 6) | (nimms & 0x3f))
}

/// The 8-bit immediate of `fmov d, #value`, if `value` is encodable.
pub fn encode_fp_immediate(value: f64) -> Option<u8> {
    let bits = value.to_bits();
    if bits & ((1 << 48) - 1) != 0 {
        return None;
    }
    let exponent = (bits >> 52) & 0x7ff;
    let replicated = (exponent >> 2) & 0xff;
    let b = replicated & 1;
    if (replicated != 0 && replicated != 0xff) || (exponent >> 10) == b {
        return None;
    }
    let sign = bits >> 63;
    Some(((sign << 7) | (b << 6) | ((exponent & 3) << 4) | ((bits >> 48) & 0xf)) as u8)
}

const fn sf(width: Width) -> u32 {
    match width {
        Width::W32 => 0,
        Width::W64 => 1 << 31,
    }
}

const fn rd(register: Gpr) -> u32 {
    register.0 as u32 & 31
}

const fn rn(register: Gpr) -> u32 {
    (register.0 as u32 & 31) << 5
}

const fn rm(register: Gpr) -> u32 {
    (register.0 as u32 & 31) << 16
}

const fn vd(register: Fpr) -> u32 {
    register.0 as u32 & 31
}

const fn vn(register: Fpr) -> u32 {
    (register.0 as u32 & 31) << 5
}

const fn vm(register: Fpr) -> u32 {
    (register.0 as u32 & 31) << 16
}

/// Extra room left before a pending branch's deadline when deciding to emit
/// veneers, covering the instruction being emitted.
const VENEER_SLACK: usize = 64;
/// When a veneer pool is emitted, it also covers branches whose deadline is
/// this close, so pools stay rare.
const VENEER_HORIZON: usize = 4096;

/// AArch64 machine code assembler.
///
/// Conditional branches, CBZ/CBNZ and TBZ/TBNZ to labels have short ranges
/// (1 MiB / 32 KiB). Backward branches out of range use an inverted branch
/// over a B. Forward branches that are about to go out of range are
/// redirected to veneers (a B to the label, in a pool jumped over by the
/// fallthrough path) emitted between instructions.
#[derive(Debug)]
pub struct Assembler {
    buffer: CodeBuffer,
    /// Pending short-range forward branches, by fixup id.
    veneer_candidates: Vec<u32>,
    /// The offset at which the veneer candidates must be looked at again.
    next_veneer_check: usize,
}

impl Default for Assembler {
    fn default() -> Self {
        Self {
            buffer: CodeBuffer::new(),
            veneer_candidates: Vec::new(),
            next_veneer_check: usize::MAX,
        }
    }
}

impl Assembler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn offset(&self) -> usize {
        self.buffer.offset()
    }

    pub fn buffer(&self) -> &CodeBuffer {
        &self.buffer
    }

    pub fn new_label(&mut self) -> Label {
        self.buffer.new_label()
    }

    pub fn bind(&mut self, label: Label) {
        self.buffer.bind(label);
    }

    pub fn label_offset(&self, label: Label) -> Option<usize> {
        self.buffer.label_offset(label)
    }

    pub fn finish(self) -> Result<Vec<u8>, AsmError> {
        self.buffer.finish()
    }

    /// `finish()`, with the end of the code as the data offset: the
    /// assembler itself emits no constant pool.
    pub fn finish_with_data_offset(self) -> Result<(Vec<u8>, usize), AsmError> {
        let code = self.finish()?;
        let length = code.len();
        Ok((code, length))
    }

    /// Emits one instruction word.
    pub fn emit(&mut self, word: u32) {
        self.reserve(4);
        self.buffer.emit_u32(word);
    }

    // Data emission.

    pub fn data_u32(&mut self, value: u32) {
        self.reserve(4);
        self.buffer.emit_u32(value);
    }

    pub fn data_u64(&mut self, value: u64) {
        self.reserve(8);
        self.buffer.emit_u64(value);
    }

    pub fn data_bytes(&mut self, bytes: &[u8]) {
        self.reserve(bytes.len());
        self.buffer.emit_bytes(bytes);
    }

    /// Pads with NOPs until the offset is a multiple of `alignment` (>= 4).
    pub fn align_code(&mut self, alignment: usize) {
        assert!(alignment.is_power_of_two() && alignment >= 4);
        assert!(self.offset().is_multiple_of(4), "unaligned code offset");
        while !self.offset().is_multiple_of(alignment) {
            self.nop();
        }
    }

    /// Pads with zero bytes until the offset is a multiple of `alignment`.
    pub fn align_data(&mut self, alignment: usize) {
        let padding = self.offset().next_multiple_of(alignment) - self.offset();
        self.reserve(padding);
        self.buffer.align_with(alignment, 0);
    }

    // Veneers.

    /// Makes room for `size` bytes, emitting a veneer pool first if a pending
    /// branch would otherwise go out of range.
    fn reserve(&mut self, size: usize) {
        if self.offset() + size >= self.next_veneer_check {
            self.check_veneers(size);
        }
    }

    fn deadline(&self, id: u32) -> Option<usize> {
        self.buffer
            .pending_fixup(id)
            .map(|fixup| fixup.at + fixup.kind.max_forward_distance() as usize)
    }

    fn track_short_branch(&mut self, id: Option<u32>) {
        let Some(id) = id else {
            return;
        };
        let deadline = self.deadline(id).unwrap();
        self.veneer_candidates.push(id);
        let pool_size = 4 * (self.veneer_candidates.len() + 1);
        // The pool grows by one veneer, so every deadline moves closer.
        self.next_veneer_check = self
            .next_veneer_check
            .saturating_sub(4)
            .min(deadline.saturating_sub(pool_size + VENEER_SLACK));
    }

    fn check_veneers(&mut self, size: usize) {
        let buffer = &self.buffer;
        self.veneer_candidates.retain(|id| buffer.pending_fixup(*id).is_some());
        if self.veneer_candidates.is_empty() {
            self.next_veneer_check = usize::MAX;
            return;
        }
        let pool_size = 4 * (self.veneer_candidates.len() + 1);
        let limit = self.offset() + size + pool_size + VENEER_SLACK;
        let earliest = self
            .veneer_candidates
            .iter()
            .filter_map(|id| self.deadline(*id))
            .min()
            .unwrap();
        if earliest <= limit {
            self.emit_veneer_pool(limit + VENEER_HORIZON);
        }
        let pool_size = 4 * (self.veneer_candidates.len() + 1);
        self.next_veneer_check = self
            .veneer_candidates
            .iter()
            .filter_map(|id| self.deadline(*id))
            .min()
            .map_or(usize::MAX, |deadline| deadline.saturating_sub(pool_size + VENEER_SLACK));
    }

    /// Emits veneers for every candidate whose deadline is before `horizon`.
    fn emit_veneer_pool(&mut self, horizon: usize) {
        let (due, remaining): (Vec<u32>, Vec<u32>) = self
            .veneer_candidates
            .iter()
            .copied()
            .partition(|id| self.deadline(*id).is_some_and(|deadline| deadline <= horizon));
        self.veneer_candidates = remaining;
        // Skip over the pool on the fallthrough path.
        self.buffer.emit_u32(0x1400_0000 | (due.len() as u32 + 1));
        for id in due {
            let fixup = self.buffer.pending_fixup(id).unwrap();
            let veneer = self.offset();
            self.buffer.emit_u32(0x1400_0000);
            self.buffer.add_fixup(fixup.label, veneer, FixupKind::A64Branch26);
            self.buffer.redirect_fixup(id, veneer);
        }
    }

    // Moves and immediates.

    /// `movz rd, #imm16, lsl #shift`.
    pub fn movz(&mut self, width: Width, rd_: Gpr, imm16: u16, shift: u8) {
        self.move_wide(0x5280_0000, width, rd_, imm16, shift);
    }

    /// `movn rd, #imm16, lsl #shift`: rd = !(imm16 << shift).
    pub fn movn(&mut self, width: Width, rd_: Gpr, imm16: u16, shift: u8) {
        self.move_wide(0x1280_0000, width, rd_, imm16, shift);
    }

    /// `movk rd, #imm16, lsl #shift`: replaces one halfword.
    pub fn movk(&mut self, width: Width, rd_: Gpr, imm16: u16, shift: u8) {
        self.move_wide(0x7280_0000, width, rd_, imm16, shift);
    }

    fn move_wide(&mut self, opcode: u32, width: Width, rd_: Gpr, imm16: u16, shift: u8) {
        assert!(
            shift.is_multiple_of(16) && u32::from(shift) < width.bits(),
            "bad move wide shift"
        );
        self.emit(opcode | sf(width) | (u32::from(shift / 16) << 21) | (u32::from(imm16) << 5) | rd(rd_));
    }

    /// Materializes `imm` (truncated to `width`) in the fewest instructions:
    /// one ORR for bitmask immediates, else MOVZ or MOVN plus MOVKs. Never
    /// changes the flags. `rd_` is never SP.
    pub fn mov_imm(&mut self, width: Width, rd_: Gpr, imm: u64) {
        let imm = match width {
            Width::W32 => imm & 0xffff_ffff,
            Width::W64 => imm,
        };
        let halfwords = width.bits() as usize / 16;
        let halfword = |i: usize| (imm >> (16 * i)) as u16;
        let zero_count = (0..halfwords).filter(|i| halfword(*i) == 0).count();
        let ones_count = (0..halfwords).filter(|i| halfword(*i) == 0xffff).count();
        let instruction_count = halfwords - zero_count.max(ones_count);
        if instruction_count > 1
            && let Some(encoding) = encode_logical_immediate(imm, width)
        {
            self.logical_immediate(0x3200_0000, width, rd_, XZR, encoding);
            return;
        }
        let (skip, inverted) = if ones_count > zero_count {
            (0xffff, true)
        } else {
            (0, false)
        };
        let mut first = true;
        for i in 0..halfwords {
            let value = halfword(i);
            if value == skip {
                continue;
            }
            let shift = (16 * i) as u8;
            if !first {
                self.movk(width, rd_, value, shift);
            } else if inverted {
                self.movn(width, rd_, !value, shift);
            } else {
                self.movz(width, rd_, value, shift);
            }
            first = false;
        }
        if first {
            // Every halfword is the skipped value: all zeros or all ones.
            if inverted {
                self.movn(width, rd_, 0, 0);
            } else {
                self.movz(width, rd_, 0, 0);
            }
        }
    }

    /// `mov rd, rm` (ORR with the zero register; neither may be SP).
    pub fn mov(&mut self, width: Width, rd_: Gpr, rm_: Gpr) {
        self.orr(width, rd_, XZR, rm_, Shift::Lsl, 0);
    }

    /// `mov rd, rn` where either may be SP (ADD #0).
    pub fn mov_sp(&mut self, rd_: Gpr, rn_: Gpr) {
        self.add_imm(Width::W64, rd_, rn_, 0, false);
    }

    // Add and subtract.

    fn add_sub_imm(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, imm12: u32, shift12: bool) {
        assert!(imm12 < 4096, "add/sub immediate out of range");
        self.emit(opcode | sf(width) | (u32::from(shift12) << 22) | (imm12 << 10) | rn(rn_) | rd(rd_));
    }

    /// `add rd, rn, #imm12{, lsl #12}`. rd and rn may be SP.
    pub fn add_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm12: u32, shift12: bool) {
        self.add_sub_imm(0x1100_0000, width, rd_, rn_, imm12, shift12);
    }

    /// `adds rd, rn, #imm12{, lsl #12}`. rn may be SP, rd 31 is ZR.
    pub fn adds_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm12: u32, shift12: bool) {
        self.add_sub_imm(0x3100_0000, width, rd_, rn_, imm12, shift12);
    }

    /// `sub rd, rn, #imm12{, lsl #12}`. rd and rn may be SP.
    pub fn sub_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm12: u32, shift12: bool) {
        self.add_sub_imm(0x5100_0000, width, rd_, rn_, imm12, shift12);
    }

    /// `subs rd, rn, #imm12{, lsl #12}`. rn may be SP, rd 31 is ZR.
    pub fn subs_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm12: u32, shift12: bool) {
        self.add_sub_imm(0x7100_0000, width, rd_, rn_, imm12, shift12);
    }

    /// `cmp rn, #imm12{, lsl #12}`.
    pub fn cmp_imm(&mut self, width: Width, rn_: Gpr, imm12: u32, shift12: bool) {
        self.subs_imm(width, XZR, rn_, imm12, shift12);
    }

    /// `cmn rn, #imm12{, lsl #12}`.
    pub fn cmn_imm(&mut self, width: Width, rn_: Gpr, imm12: u32, shift12: bool) {
        self.adds_imm(width, XZR, rn_, imm12, shift12);
    }

    #[allow(clippy::too_many_arguments)]
    fn shifted_register(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        assert!(u32::from(amount) < width.bits(), "shift amount out of range");
        self.emit(
            opcode | sf(width) | ((shift as u32) << 22) | rm(rm_) | (u32::from(amount) << 10) | rn(rn_) | rd(rd_),
        );
    }

    /// `add rd, rn, rm{, shift #amount}`. Register 31 is ZR.
    pub fn add(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        assert_ne!(shift, Shift::Ror);
        self.shifted_register(0x0b00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `adds rd, rn, rm{, shift #amount}`.
    pub fn adds(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        assert_ne!(shift, Shift::Ror);
        self.shifted_register(0x2b00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `sub rd, rn, rm{, shift #amount}`.
    pub fn sub(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        assert_ne!(shift, Shift::Ror);
        self.shifted_register(0x4b00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `subs rd, rn, rm{, shift #amount}`.
    pub fn subs(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        assert_ne!(shift, Shift::Ror);
        self.shifted_register(0x6b00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `cmp rn, rm`.
    pub fn cmp(&mut self, width: Width, rn_: Gpr, rm_: Gpr) {
        self.subs(width, XZR, rn_, rm_, Shift::Lsl, 0);
    }

    /// `cmn rn, rm`.
    pub fn cmn(&mut self, width: Width, rn_: Gpr, rm_: Gpr) {
        self.adds(width, XZR, rn_, rm_, Shift::Lsl, 0);
    }

    /// `neg rd, rm`.
    pub fn neg(&mut self, width: Width, rd_: Gpr, rm_: Gpr) {
        self.sub(width, rd_, XZR, rm_, Shift::Lsl, 0);
    }

    /// `negs rd, rm`.
    pub fn negs(&mut self, width: Width, rd_: Gpr, rm_: Gpr) {
        self.subs(width, rd_, XZR, rm_, Shift::Lsl, 0);
    }

    #[allow(clippy::too_many_arguments)]
    fn extended_register(
        &mut self,
        opcode: u32,
        width: Width,
        rd_: Gpr,
        rn_: Gpr,
        rm_: Gpr,
        extend: Extend,
        amount: u8,
    ) {
        assert!(amount <= 4, "extended register shift out of range");
        self.emit(
            opcode | sf(width) | rm(rm_) | ((extend as u32) << 13) | (u32::from(amount) << 10) | rn(rn_) | rd(rd_),
        );
    }

    /// `add rd, rn, rm, extend #amount`. rd and rn may be SP.
    pub fn add_extended(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, extend: Extend, amount: u8) {
        self.extended_register(0x0b20_0000, width, rd_, rn_, rm_, extend, amount);
    }

    /// `sub rd, rn, rm, extend #amount`. rd and rn may be SP.
    pub fn sub_extended(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, extend: Extend, amount: u8) {
        self.extended_register(0x4b20_0000, width, rd_, rn_, rm_, extend, amount);
    }

    /// `subs rd, rn, rm, extend #amount`. rn may be SP, rd 31 is ZR.
    pub fn subs_extended(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, extend: Extend, amount: u8) {
        self.extended_register(0x6b20_0000, width, rd_, rn_, rm_, extend, amount);
    }

    // Logical operations.

    fn logical_immediate(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, encoding: u32) {
        self.emit(opcode | sf(width) | (encoding << 10) | rn(rn_) | rd(rd_));
    }

    fn logical_imm(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, imm: u64) {
        let Some(encoding) = encode_logical_immediate(imm, width) else {
            panic!("{imm:#x} is not a valid {}-bit logical immediate", width.bits());
        };
        self.logical_immediate(opcode, width, rd_, rn_, encoding);
    }

    /// `and rd, rn, #imm`. Panics if `imm` is not encodable (see
    /// `encode_logical_immediate`). rd may be SP.
    pub fn and_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm: u64) {
        self.logical_imm(0x1200_0000, width, rd_, rn_, imm);
    }

    /// `orr rd, rn, #imm`. rd may be SP.
    pub fn orr_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm: u64) {
        self.logical_imm(0x3200_0000, width, rd_, rn_, imm);
    }

    /// `eor rd, rn, #imm`. rd may be SP.
    pub fn eor_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm: u64) {
        self.logical_imm(0x5200_0000, width, rd_, rn_, imm);
    }

    /// `ands rd, rn, #imm`. rd 31 is ZR.
    pub fn ands_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, imm: u64) {
        self.logical_imm(0x7200_0000, width, rd_, rn_, imm);
    }

    /// `tst rn, #imm`.
    pub fn tst_imm(&mut self, width: Width, rn_: Gpr, imm: u64) {
        self.ands_imm(width, XZR, rn_, imm);
    }

    /// `and rd, rn, rm{, shift #amount}`.
    pub fn and(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        self.shifted_register(0x0a00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `bic rd, rn, rm{, shift #amount}`: rn & !rm.
    pub fn bic(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        self.shifted_register(0x0a20_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `orr rd, rn, rm{, shift #amount}`.
    pub fn orr(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        self.shifted_register(0x2a00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `orn rd, rn, rm{, shift #amount}`: rn | !rm.
    pub fn orn(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        self.shifted_register(0x2a20_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `eor rd, rn, rm{, shift #amount}`.
    pub fn eor(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        self.shifted_register(0x4a00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `ands rd, rn, rm{, shift #amount}`.
    pub fn ands(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, shift: Shift, amount: u8) {
        self.shifted_register(0x6a00_0000, width, rd_, rn_, rm_, shift, amount);
    }

    /// `tst rn, rm`.
    pub fn tst(&mut self, width: Width, rn_: Gpr, rm_: Gpr) {
        self.ands(width, XZR, rn_, rm_, Shift::Lsl, 0);
    }

    /// `mvn rd, rm`.
    pub fn mvn(&mut self, width: Width, rd_: Gpr, rm_: Gpr) {
        self.orn(width, rd_, XZR, rm_, Shift::Lsl, 0);
    }

    // Shifts and bitfield moves.

    /// `ubfm rd, rn, #immr, #imms`.
    pub fn ubfm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, immr: u8, imms: u8) {
        self.bitfield(0x5300_0000, width, rd_, rn_, immr, imms);
    }

    /// `sbfm rd, rn, #immr, #imms`.
    pub fn sbfm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, immr: u8, imms: u8) {
        self.bitfield(0x1300_0000, width, rd_, rn_, immr, imms);
    }

    fn bitfield(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, immr: u8, imms: u8) {
        assert!(u32::from(immr) < width.bits() && u32::from(imms) < width.bits());
        let n = match width {
            Width::W32 => 0,
            Width::W64 => 1 << 22,
        };
        self.emit(opcode | sf(width) | n | (u32::from(immr) << 16) | (u32::from(imms) << 10) | rn(rn_) | rd(rd_));
    }

    /// `lsl rd, rn, #amount`.
    pub fn lsl_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, amount: u8) {
        let size = width.bits() as u8;
        assert!(amount < size);
        self.ubfm(width, rd_, rn_, (size - amount) % size, size - 1 - amount);
    }

    /// `lsr rd, rn, #amount`.
    pub fn lsr_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, amount: u8) {
        self.ubfm(width, rd_, rn_, amount, width.bits() as u8 - 1);
    }

    /// `asr rd, rn, #amount`.
    pub fn asr_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, amount: u8) {
        self.sbfm(width, rd_, rn_, amount, width.bits() as u8 - 1);
    }

    fn data_processing_2(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.emit(opcode | sf(width) | rm(rm_) | rn(rn_) | rd(rd_));
    }

    /// `lsl rd, rn, rm` (shift amount modulo the width).
    pub fn lsl(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_2(0x1ac0_2000, width, rd_, rn_, rm_);
    }

    /// `lsr rd, rn, rm`.
    pub fn lsr(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_2(0x1ac0_2400, width, rd_, rn_, rm_);
    }

    /// `asr rd, rn, rm`.
    pub fn asr(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_2(0x1ac0_2800, width, rd_, rn_, rm_);
    }

    /// `ror rd, rn, rm`.
    pub fn ror(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_2(0x1ac0_2c00, width, rd_, rn_, rm_);
    }

    /// `sdiv rd, rn, rm`.
    pub fn sdiv(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_2(0x1ac0_0c00, width, rd_, rn_, rm_);
    }

    /// `udiv rd, rn, rm`.
    pub fn udiv(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_2(0x1ac0_0800, width, rd_, rn_, rm_);
    }

    /// `sxtw xd, wn`.
    pub fn sxtw(&mut self, rd_: Gpr, rn_: Gpr) {
        self.sbfm(Width::W64, rd_, rn_, 0, 31);
    }

    /// `sxtb rd, wn`.
    pub fn sxtb(&mut self, width: Width, rd_: Gpr, rn_: Gpr) {
        self.sbfm(width, rd_, rn_, 0, 7);
    }

    /// `sxth rd, wn`.
    pub fn sxth(&mut self, width: Width, rd_: Gpr, rn_: Gpr) {
        self.sbfm(width, rd_, rn_, 0, 15);
    }

    /// `uxtb wd, wn`.
    pub fn uxtb(&mut self, rd_: Gpr, rn_: Gpr) {
        self.ubfm(Width::W32, rd_, rn_, 0, 7);
    }

    /// `uxth wd, wn`.
    pub fn uxth(&mut self, rd_: Gpr, rn_: Gpr) {
        self.ubfm(Width::W32, rd_, rn_, 0, 15);
    }

    // Multiplication.

    fn data_processing_3(&mut self, opcode: u32, rd_: Gpr, rn_: Gpr, rm_: Gpr, ra: Gpr) {
        self.emit(opcode | rm(rm_) | (u32::from(ra.0 & 31) << 10) | rn(rn_) | rd(rd_));
    }

    /// `madd rd, rn, rm, ra`: ra + rn * rm.
    pub fn madd(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, ra: Gpr) {
        self.data_processing_3(0x1b00_0000 | sf(width), rd_, rn_, rm_, ra);
    }

    /// `msub rd, rn, rm, ra`: ra - rn * rm.
    pub fn msub(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, ra: Gpr) {
        self.data_processing_3(0x1b00_8000 | sf(width), rd_, rn_, rm_, ra);
    }

    /// `mul rd, rn, rm`.
    pub fn mul(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.madd(width, rd_, rn_, rm_, XZR);
    }

    /// `smull xd, wn, wm`: the full signed 64-bit product.
    pub fn smull(&mut self, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_3(0x9b20_0000, rd_, rn_, rm_, XZR);
    }

    /// `umull xd, wn, wm`.
    pub fn umull(&mut self, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_3(0x9ba0_0000, rd_, rn_, rm_, XZR);
    }

    /// `smulh xd, xn, xm`: the high 64 bits of the signed 128-bit product.
    pub fn smulh(&mut self, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_3(0x9b40_0000, rd_, rn_, rm_, XZR);
    }

    /// `umulh xd, xn, xm`.
    pub fn umulh(&mut self, rd_: Gpr, rn_: Gpr, rm_: Gpr) {
        self.data_processing_3(0x9bc0_0000, rd_, rn_, rm_, XZR);
    }

    /// Sets the flags so that `Ne` means the signed 64-bit product in
    /// `product` (from `smull`) does not fit in 32 bits: `cmp x, w, sxtw`.
    pub fn check_smull_overflow(&mut self, product: Gpr) {
        self.subs_extended(Width::W64, XZR, product, product, Extend::Sxtw, 0);
    }

    /// Sets the flags so that `Ne` means the 64-bit `mul` result `low` with
    /// `smulh` result `high` overflowed: `cmp high, low, asr #63`.
    pub fn check_smulh_overflow(&mut self, high: Gpr, low: Gpr) {
        self.subs(Width::W64, XZR, high, low, Shift::Asr, 63);
    }

    // Conditional select.

    fn conditional_select(&mut self, opcode: u32, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, cond: Cond) {
        self.emit(opcode | sf(width) | rm(rm_) | ((cond as u32) << 12) | rn(rn_) | rd(rd_));
    }

    /// `csel rd, rn, rm, cond`: cond ? rn : rm.
    pub fn csel(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, cond: Cond) {
        self.conditional_select(0x1a80_0000, width, rd_, rn_, rm_, cond);
    }

    /// `csinc rd, rn, rm, cond`: cond ? rn : rm + 1.
    pub fn csinc(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, cond: Cond) {
        self.conditional_select(0x1a80_0400, width, rd_, rn_, rm_, cond);
    }

    /// `csinv rd, rn, rm, cond`: cond ? rn : !rm.
    pub fn csinv(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, cond: Cond) {
        self.conditional_select(0x5a80_0000, width, rd_, rn_, rm_, cond);
    }

    /// `csneg rd, rn, rm, cond`: cond ? rn : -rm.
    pub fn csneg(&mut self, width: Width, rd_: Gpr, rn_: Gpr, rm_: Gpr, cond: Cond) {
        self.conditional_select(0x5a80_0400, width, rd_, rn_, rm_, cond);
    }

    /// `cset rd, cond`: cond ? 1 : 0.
    pub fn cset(&mut self, width: Width, rd_: Gpr, cond: Cond) {
        self.csinc(width, rd_, XZR, XZR, cond.invert());
    }

    /// `csetm rd, cond`: cond ? -1 : 0.
    pub fn csetm(&mut self, width: Width, rd_: Gpr, cond: Cond) {
        self.csinv(width, rd_, XZR, XZR, cond.invert());
    }

    // Loads and stores.

    /// A load or store of `op` with register `rt` (a GPR or FPR encoding;
    /// 31 is ZR for GPRs). Base registers may be SP.
    pub fn load_store(&mut self, op: MemOp, rt: u8, operand: MemOperand) {
        let size_log2 = op.size_log2();
        let rt = u32::from(rt & 31);
        let base_opcode = op as u32;
        match operand {
            MemOperand::Offset(base, offset) => {
                let size = 1 << size_log2;
                if offset >= 0 && offset % size == 0 && offset / size < 4096 {
                    let imm12 = (offset / size) as u32;
                    self.emit(base_opcode | (1 << 24) | (imm12 << 10) | rn(base) | rt);
                } else {
                    assert!(
                        (-256..256).contains(&offset),
                        "load/store offset {offset} not encodable"
                    );
                    self.emit(base_opcode | ((offset as u32 & 0x1ff) << 12) | rn(base) | rt);
                }
            }
            MemOperand::Register {
                base,
                index,
                extend,
                shifted,
            } => {
                assert!(
                    matches!(extend, Extend::Uxtw | Extend::Uxtx | Extend::Sxtw | Extend::Sxtx),
                    "bad load/store register extend"
                );
                self.emit(
                    base_opcode
                        | (1 << 21)
                        | rm(index)
                        | ((extend as u32) << 13)
                        | (u32::from(shifted) << 12)
                        | (0b10 << 10)
                        | rn(base)
                        | rt,
                );
            }
            MemOperand::PreIndex(base, offset) => {
                assert!((-256..256).contains(&offset), "pre-index offset {offset} not encodable");
                self.emit(base_opcode | ((offset as u32 & 0x1ff) << 12) | (0b11 << 10) | rn(base) | rt);
            }
            MemOperand::PostIndex(base, offset) => {
                assert!(
                    (-256..256).contains(&offset),
                    "post-index offset {offset} not encodable"
                );
                self.emit(base_opcode | ((offset as u32 & 0x1ff) << 12) | (0b01 << 10) | rn(base) | rt);
            }
        }
    }

    /// `ldr wt/xt, operand`.
    pub fn ldr(&mut self, width: Width, rt: Gpr, operand: MemOperand) {
        let op = match width {
            Width::W32 => MemOp::LdrW,
            Width::W64 => MemOp::LdrX,
        };
        self.load_store(op, rt.0, operand);
    }

    /// `str wt/xt, operand`.
    pub fn str(&mut self, width: Width, rt: Gpr, operand: MemOperand) {
        let op = match width {
            Width::W32 => MemOp::StrW,
            Width::W64 => MemOp::StrX,
        };
        self.load_store(op, rt.0, operand);
    }

    /// `ldrb wt, operand` (zero-extends).
    pub fn ldrb(&mut self, rt: Gpr, operand: MemOperand) {
        self.load_store(MemOp::Ldrb, rt.0, operand);
    }

    /// `ldrh wt, operand` (zero-extends).
    pub fn ldrh(&mut self, rt: Gpr, operand: MemOperand) {
        self.load_store(MemOp::Ldrh, rt.0, operand);
    }

    /// `ldrsb wt/xt, operand`.
    pub fn ldrsb(&mut self, width: Width, rt: Gpr, operand: MemOperand) {
        let op = match width {
            Width::W32 => MemOp::LdrsbW,
            Width::W64 => MemOp::LdrsbX,
        };
        self.load_store(op, rt.0, operand);
    }

    /// `ldrsh wt/xt, operand`.
    pub fn ldrsh(&mut self, width: Width, rt: Gpr, operand: MemOperand) {
        let op = match width {
            Width::W32 => MemOp::LdrshW,
            Width::W64 => MemOp::LdrshX,
        };
        self.load_store(op, rt.0, operand);
    }

    /// `ldrsw xt, operand`.
    pub fn ldrsw(&mut self, rt: Gpr, operand: MemOperand) {
        self.load_store(MemOp::Ldrsw, rt.0, operand);
    }

    /// `strb wt, operand`.
    pub fn strb(&mut self, rt: Gpr, operand: MemOperand) {
        self.load_store(MemOp::Strb, rt.0, operand);
    }

    /// `strh wt, operand`.
    pub fn strh(&mut self, rt: Gpr, operand: MemOperand) {
        self.load_store(MemOp::Strh, rt.0, operand);
    }

    /// `ldr dt, operand`.
    pub fn ldr_d(&mut self, rt: Fpr, operand: MemOperand) {
        self.load_store(MemOp::LdrD, rt.0, operand);
    }

    /// `str dt, operand`.
    pub fn str_d(&mut self, rt: Fpr, operand: MemOperand) {
        self.load_store(MemOp::StrD, rt.0, operand);
    }

    fn load_store_pair(&mut self, opcode: u32, rt: u8, rt2: u8, operand: PairOperand) {
        let (mode, base, offset) = match operand {
            PairOperand::PostIndex(base, offset) => (0b01, base, offset),
            PairOperand::Offset(base, offset) => (0b10, base, offset),
            PairOperand::PreIndex(base, offset) => (0b11, base, offset),
        };
        assert!(
            offset % 8 == 0 && (-512..512).contains(&offset),
            "pair offset {offset} not encodable"
        );
        let imm7 = ((offset / 8) as u32) & 0x7f;
        self.emit(opcode | (mode << 23) | (imm7 << 15) | (u32::from(rt2 & 31) << 10) | rn(base) | u32::from(rt & 31));
    }

    /// `stp xt, xt2, operand`.
    pub fn stp(&mut self, rt: Gpr, rt2: Gpr, operand: PairOperand) {
        self.load_store_pair(0xa800_0000, rt.0, rt2.0, operand);
    }

    /// `ldp xt, xt2, operand`.
    pub fn ldp(&mut self, rt: Gpr, rt2: Gpr, operand: PairOperand) {
        self.load_store_pair(0xa840_0000, rt.0, rt2.0, operand);
    }

    /// `stp dt, dt2, operand`.
    pub fn stp_d(&mut self, rt: Fpr, rt2: Fpr, operand: PairOperand) {
        self.load_store_pair(0x6c00_0000, rt.0, rt2.0, operand);
    }

    /// `ldp dt, dt2, operand`.
    pub fn ldp_d(&mut self, rt: Fpr, rt2: Fpr, operand: PairOperand) {
        self.load_store_pair(0x6c40_0000, rt.0, rt2.0, operand);
    }

    // Branches.

    /// Emits a branch with a label-relative immediate of `kind`, falling back
    /// to an inverted branch over a B for bound labels out of range.
    fn branch_to_label(&mut self, word: u32, kind: FixupKind, label: Label, invert: impl FnOnce(u32) -> u32) {
        self.reserve(8);
        if let Some(target) = self.buffer.label_offset(label)
            && !kind.reaches(self.offset(), target)
        {
            // Skip the B (8 bytes ahead) when the original condition fails.
            let skip = 2;
            self.emit(invert(word) | (skip << kind_shift(kind)));
            self.b(label);
            return;
        }
        let at = self.offset();
        self.emit(word);
        let id = self.buffer.add_fixup(label, at, kind);
        self.track_short_branch(id);
    }

    /// Emits `word` with a fixup of `kind` to `label`. Room is reserved first
    /// so that a veneer pool cannot land between the recorded offset and the
    /// word.
    fn emit_with_fixup(&mut self, word: u32, label: Label, kind: FixupKind) {
        self.reserve(4);
        let at = self.offset();
        self.buffer.emit_u32(word);
        self.buffer.add_fixup(label, at, kind);
    }

    /// `b label` (range +-128 MiB).
    pub fn b(&mut self, label: Label) {
        self.emit_with_fixup(0x1400_0000, label, FixupKind::A64Branch26);
    }

    /// `bl label`.
    pub fn bl(&mut self, label: Label) {
        self.emit_with_fixup(0x9400_0000, label, FixupKind::A64Branch26);
    }

    /// `b.cond label`.
    pub fn b_cond(&mut self, cond: Cond, label: Label) {
        assert_ne!(cond, Cond::Al);
        self.branch_to_label(0x5400_0000 | cond as u32, FixupKind::A64Branch19, label, |word| {
            word ^ 1
        });
    }

    /// `cbz rt, label`.
    pub fn cbz(&mut self, width: Width, rt: Gpr, label: Label) {
        self.branch_to_label(
            0x3400_0000 | sf(width) | rd(rt),
            FixupKind::A64Branch19,
            label,
            |word| word ^ (1 << 24),
        );
    }

    /// `cbnz rt, label`.
    pub fn cbnz(&mut self, width: Width, rt: Gpr, label: Label) {
        self.branch_to_label(
            0x3500_0000 | sf(width) | rd(rt),
            FixupKind::A64Branch19,
            label,
            |word| word ^ (1 << 24),
        );
    }

    /// `tbz rt, #bit, label`.
    pub fn tbz(&mut self, rt: Gpr, bit: u8, label: Label) {
        self.branch_to_label(
            test_bit_branch(0x3600_0000, rt, bit),
            FixupKind::A64Branch14,
            label,
            |word| word ^ (1 << 24),
        );
    }

    /// `tbnz rt, #bit, label`.
    pub fn tbnz(&mut self, rt: Gpr, bit: u8, label: Label) {
        self.branch_to_label(
            test_bit_branch(0x3700_0000, rt, bit),
            FixupKind::A64Branch14,
            label,
            |word| word ^ (1 << 24),
        );
    }

    /// `br rn`.
    pub fn br(&mut self, rn_: Gpr) {
        self.emit(0xd61f_0000 | rn(rn_));
    }

    /// `blr rn`.
    pub fn blr(&mut self, rn_: Gpr) {
        self.emit(0xd63f_0000 | rn(rn_));
    }

    /// `ret` (through x30).
    pub fn ret(&mut self) {
        self.emit(0xd65f_0000 | rn(LR));
    }

    /// `adr rd, label` (range +-1 MiB).
    /// `ldr xt, label`: a PC-relative load within +-1 MiB.
    pub fn ldr_literal(&mut self, rt: Gpr, label: Label) {
        self.emit_with_fixup(0x5800_0000 | rd(rt), label, FixupKind::A64Branch19);
    }

    /// `ldr dt, label`: a PC-relative load within +-1 MiB.
    pub fn ldr_literal_double(&mut self, rt: Fpr, label: Label) {
        self.emit_with_fixup(0x5c00_0000 | u32::from(rt.0 & 31), label, FixupKind::A64Branch19);
    }

    /// `ldr qt, label`: a PC-relative 128-bit load within +-1 MiB.
    pub fn ldr_literal_quad(&mut self, rt: Fpr, label: Label) {
        self.emit_with_fixup(0x9c00_0000 | u32::from(rt.0 & 31), label, FixupKind::A64Branch19);
    }

    /// The number of instructions `mov_imm` takes for a 64-bit value.
    pub fn mov_imm_instruction_count(imm: u64) -> usize {
        let halfword = |i: usize| (imm >> (16 * i)) as u16;
        let zero_count = (0..4).filter(|i| halfword(*i) == 0).count();
        let ones_count = (0..4).filter(|i| halfword(*i) == 0xffff).count();
        let count = (4 - zero_count.max(ones_count)).max(1);
        if count > 1 && encode_logical_immediate(imm, Width::W64).is_some() {
            return 1;
        }
        count
    }

    pub fn adr(&mut self, rd_: Gpr, label: Label) {
        self.emit_with_fixup(0x1000_0000 | rd(rd_), label, FixupKind::A64Adr21);
    }

    pub fn nop(&mut self) {
        self.emit(0xd503_201f);
    }

    /// `brk #imm16`.
    pub fn brk(&mut self, imm16: u16) {
        self.emit(0xd420_0000 | (u32::from(imm16) << 5));
    }

    /// `udf #imm16`: permanently undefined.
    pub fn udf(&mut self, imm16: u16) {
        self.emit(u32::from(imm16));
    }

    // Scalar double floating point.

    /// `fmov dd, dn`.
    pub fn fmov(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e60_4000 | vn(rn_) | vd(rd_));
    }

    /// `fmov dd, xn` (bit copy). xn 31 is XZR.
    pub fn fmov_from_gpr(&mut self, rd_: Fpr, rn_: Gpr) {
        self.emit(0x9e67_0000 | rn(rn_) | vd(rd_));
    }

    /// `fmov xd, dn` (bit copy).
    pub fn fmov_to_gpr(&mut self, rd_: Gpr, rn_: Fpr) {
        self.emit(0x9e66_0000 | vn(rn_) | rd(rd_));
    }

    /// `fmov dd, #value`. Panics if `value` is not encodable (see
    /// `encode_fp_immediate`).
    pub fn fmov_imm(&mut self, rd_: Fpr, value: f64) {
        let Some(imm8) = encode_fp_immediate(value) else {
            panic!("{value} is not an encodable fmov immediate");
        };
        self.emit(0x1e60_1000 | (u32::from(imm8) << 13) | vd(rd_));
    }

    fn fp_data_processing_2(&mut self, opcode: u32, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.emit(opcode | vm(rm_) | vn(rn_) | vd(rd_));
    }

    pub fn fadd(&mut self, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.fp_data_processing_2(0x1e60_2800, rd_, rn_, rm_);
    }

    pub fn fsub(&mut self, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.fp_data_processing_2(0x1e60_3800, rd_, rn_, rm_);
    }

    pub fn fmul(&mut self, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.fp_data_processing_2(0x1e60_0800, rd_, rn_, rm_);
    }

    pub fn fdiv(&mut self, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.fp_data_processing_2(0x1e60_1800, rd_, rn_, rm_);
    }

    pub fn fsqrt(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e61_c000 | vn(rn_) | vd(rd_));
    }

    /// Rounds toward minus infinity.
    pub fn frintm(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e65_4000 | vn(rn_) | vd(rd_));
    }

    /// Rounds toward plus infinity.
    pub fn frintp(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e64_c000 | vn(rn_) | vd(rd_));
    }

    pub fn fneg(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e61_4000 | vn(rn_) | vd(rd_));
    }

    pub fn fabs(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e60_c000 | vn(rn_) | vd(rd_));
    }

    /// `fcmp dn, dm`: NZCV = unordered 0011, less 1000, equal 0110, greater 0010.
    pub fn fcmp(&mut self, rn_: Fpr, rm_: Fpr) {
        self.emit(0x1e60_2000 | vm(rm_) | vn(rn_));
    }

    /// `fcmp dn, #0.0`.
    pub fn fcmp_zero(&mut self, rn_: Fpr) {
        self.emit(0x1e60_2008 | vn(rn_));
    }

    /// `scvtf dd, wn/xn`.
    pub fn scvtf(&mut self, width: Width, rd_: Fpr, rn_: Gpr) {
        self.emit(0x1e62_0000 | sf(width) | rn(rn_) | vd(rd_));
    }

    /// `fcvt dd, sn`: widens a float to a double.
    pub fn fcvt_double_from_single(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e22_c000 | vn(rn_) | vd(rd_));
    }

    /// `fcvt sd, dn`: rounds a double to a float.
    pub fn fcvt_single_from_double(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e62_4000 | vn(rn_) | vd(rd_));
    }

    /// `fcvtzs wd/xd, dn`: truncates toward zero, saturating; NaN gives 0.
    pub fn fcvtzs(&mut self, width: Width, rd_: Gpr, rn_: Fpr) {
        self.emit(0x1e78_0000 | sf(width) | vn(rn_) | rd(rd_));
    }

    /// `fjcvtzs wd, dn`: JavaScript ToInt32 (requires FEAT_JSCVT). Sets Z
    /// when the conversion was exact.
    pub fn fjcvtzs(&mut self, rd_: Gpr, rn_: Fpr) {
        self.emit(0x1e7e_0000 | vn(rn_) | rd(rd_));
    }
}

const fn kind_shift(kind: FixupKind) -> u32 {
    match kind {
        FixupKind::A64Branch19 | FixupKind::A64Branch14 => 5,
        _ => 0,
    }
}

fn test_bit_branch(opcode: u32, rt: Gpr, bit: u8) -> u32 {
    assert!(bit < 64, "test bit out of range");
    let b5 = u32::from(bit >> 5) << 31;
    let b40 = u32::from(bit & 31) << 19;
    opcode | b5 | b40 | rd(rt)
}
