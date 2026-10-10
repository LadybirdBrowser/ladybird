/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::super::Width::W32;
use super::super::Width::W64;
use super::super::disassembler;
use super::super::disassembler::assert_encodings;
use super::*;

fn bytes(emit: impl FnOnce(&mut Assembler)) -> Vec<u8> {
    let mut assembler = Assembler::new();
    emit(&mut assembler);
    assembler.finish().unwrap()
}

fn word_at(code: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(code[offset..offset + 4].try_into().unwrap())
}

const NOP: u32 = 0xd503_201f;

#[test]
fn move_wide_immediates() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "mov x0, #0x1234" => |a| a.movz(W64, X0, 0x1234, 0);
        "mov w1, #-0x10000" => |a| a.movz(W32, X1, 0xffff, 16);
        "mov x2, #0x123400000000" => |a| a.movz(W64, X2, 0x1234, 32);
        "mov x2, #-0x1" => |a| a.movn(W64, X2, 0, 0);
        "mov w3, #-0x1235" => |a| a.movn(W32, X3, 0x1234, 0);
        "movk x4, #0xbeef, lsl #0x30" => |a| a.movk(W64, X4, 0xbeef, 48);
        "movk w5, #0x1, lsl #0x10" => |a| a.movk(W32, X5, 1, 16);
        "mov x0, x1" => |a| a.mov(W64, X0, X1);
        "mov w0, w30" => |a| a.mov(W32, X0, X30);
        "mov sp, x29" => |a| a.mov_sp(SP, X29);
        "mov x29, sp" => |a| a.mov_sp(X29, SP);
        "mov x0, #0x0" => |a| a.mov_imm(W64, X0, 0);
        "mov x0, #-0x1" => |a| a.mov_imm(W64, X0, u64::MAX);
        "mov w0, #-0x1" => |a| a.mov_imm(W32, X0, u64::MAX);
        "mov x0, #0x9abc; movk x0, #0x5678, lsl #0x10; movk x0, #0x1234, lsl #0x20" => |a| a.mov_imm(W64, X0, 0x1234_5678_9abc);
        "mov x0, #-0x1235; movk x0, #0x0, lsl #0x30" => |a| a.mov_imm(W64, X0, 0x0000_ffff_ffff_edcb);
        "mov x7, #-0xff00ff00ff0100" => |a| a.mov_imm(W64, X7, 0xff00_ff00_ff00_ff00);
    });
}

/// The value a sequence of MOVZ/MOVN/MOVK/ORR-immediate instructions leaves
/// in its destination register.
fn emulate_materialization(code: &[u8], width: Width) -> u64 {
    let mut value = 0u64;
    for chunk in code.chunks(4) {
        let word = u32::from_le_bytes(chunk.try_into().unwrap());
        assert_eq!(word >> 31, if width == W64 { 1 } else { 0 });
        match (word >> 23) & 0x3f {
            0b100101 => {
                let shift = 16 * ((word >> 21) & 3);
                let imm = u64::from((word >> 5) & 0xffff) << shift;
                value = match (word >> 29) & 3 {
                    0 => !imm,
                    2 => imm,
                    3 => (value & !(0xffff << shift)) | imm,
                    _ => panic!("unexpected move wide {word:#x}"),
                };
            }
            0b100100 => {
                assert_eq!((word >> 29) & 3, 1, "not an orr");
                assert_eq!((word >> 5) & 31, 31, "orr not from zr");
                value = decode_bit_masks((word >> 22) & 1, (word >> 10) & 0x3f, (word >> 16) & 0x3f, width).unwrap();
            }
            _ => panic!("unexpected instruction {word:#x}"),
        }
    }
    match width {
        W32 => value & 0xffff_ffff,
        W64 => value,
    }
}

#[test]
fn materialized_immediates_have_the_right_value() {
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        // xorshift64*
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state.wrapping_mul(0x2545_f491_4f6c_dd1d)
    };
    let mut values = vec![0, 1, u64::MAX, 0xffff, 0xffff_0000, 1 << 63, 0x5555_5555_5555_5555];
    for _ in 0..20000 {
        let random = next();
        // Bias toward halfwords of all zeros or all ones.
        let mask = next();
        let mut value = random;
        for i in 0..4 {
            match (mask >> (8 * i)) & 3 {
                0 => value &= !(0xffff << (16 * i)),
                1 => value |= 0xffff << (16 * i),
                _ => {}
            }
        }
        values.push(value);
    }
    for value in values {
        for width in [W32, W64] {
            let code = bytes(|a| a.mov_imm(width, X9, value));
            let expected = if width == W32 { value & 0xffff_ffff } else { value };
            assert_eq!(emulate_materialization(&code, width), expected, "{value:#x} {width:?}");
            assert!(
                code.len() <= width.bits() as usize / 4,
                "{value:#x} took {} bytes",
                code.len()
            );
        }
    }
}

#[test]
fn add_and_subtract() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "add x0, x1, #0xfff" => |a| a.add_imm(W64, X0, X1, 4095, false);
        "add x0, x1, #0x1, lsl #0xc" => |a| a.add_imm(W64, X0, X1, 1, true);
        "add sp, sp, #0x10" => |a| a.add_imm(W64, SP, SP, 16, false);
        "sub w0, w1, #0x1" => |a| a.sub_imm(W32, X0, X1, 1, false);
        "sub sp, sp, #0x20" => |a| a.sub_imm(W64, SP, SP, 32, false);
        "adds x0, x1, #0x1" => |a| a.adds_imm(W64, X0, X1, 1, false);
        "subs w2, w3, #0x0" => |a| a.subs_imm(W32, X2, X3, 0, false);
        "cmp x0, #0x2a" => |a| a.cmp_imm(W64, X0, 42, false);
        "cmp sp, #0x0" => |a| a.cmp_imm(W64, SP, 0, false);
        "cmn w5, #0x7" => |a| a.cmn_imm(W32, X5, 7, false);
        "add x0, x1, x2" => |a| a.add(W64, X0, X1, X2, Shift::Lsl, 0);
        "add x0, x1, x2, lsl #0x3" => |a| a.add(W64, X0, X1, X2, Shift::Lsl, 3);
        "sub w0, w1, w2, asr #0x1f" => |a| a.sub(W32, X0, X1, X2, Shift::Asr, 31);
        "adds x0, x1, x2" => |a| a.adds(W64, X0, X1, X2, Shift::Lsl, 0);
        "subs x0, x1, x2, lsr #0x1" => |a| a.subs(W64, X0, X1, X2, Shift::Lsr, 1);
        "cmp x0, x1" => |a| a.cmp(W64, X0, X1);
        "cmn w0, w1" => |a| a.cmn(W32, X0, X1);
        "neg x0, x1" => |a| a.neg(W64, X0, X1);
        "negs w0, w1" => |a| a.negs(W32, X0, X1);
        "add x16, sp, x16" => |a| a.add_extended(W64, X16, SP, X16, Extend::Uxtx, 0);
        "add x0, x1, w2, sxtw #0x2" => |a| a.add_extended(W64, X0, X1, X2, Extend::Sxtw, 2);
        "sub sp, sp, x17" => |a| a.sub_extended(W64, SP, SP, X17, Extend::Uxtx, 0);
        "cmp x17, w17, sxtw" => |a| a.subs_extended(W64, XZR, X17, X17, Extend::Sxtw, 0);
    });
}

#[test]
fn logical_operations() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "and x1, x2, #0xff00" => |a| a.and_imm(W64, X1, X2, 0xff00);
        "orr w0, w1, #0x55555555" => |a| a.orr_imm(W32, X0, X1, 0x5555_5555);
        "eor x3, x4, #0x8000000000000000" => |a| a.eor_imm(W64, X3, X4, 1 << 63);
        "ands x0, x1, #0x1" => |a| a.ands_imm(W64, X0, X1, 1);
        "tst x0, #0xffff000000000000" => |a| a.tst_imm(W64, X0, 0xffff_0000_0000_0000);
        "tst w0, #0x7" => |a| a.tst_imm(W32, X0, 7);
        "and sp, x0, #0xfffffffffffffff0" => |a| a.and_imm(W64, SP, X0, !0xf);
        "and x0, x1, x2" => |a| a.and(W64, X0, X1, X2, Shift::Lsl, 0);
        "bic x0, x1, x2" => |a| a.bic(W64, X0, X1, X2, Shift::Lsl, 0);
        "orr w0, w1, w2" => |a| a.orr(W32, X0, X1, X2, Shift::Lsl, 0);
        "orn x0, x1, x2" => |a| a.orn(W64, X0, X1, X2, Shift::Lsl, 0);
        "eor x0, x1, x2, ror #0x7" => |a| a.eor(W64, X0, X1, X2, Shift::Ror, 7);
        "ands w0, w1, w2" => |a| a.ands(W32, X0, X1, X2, Shift::Lsl, 0);
        "tst x0, x1" => |a| a.tst(W64, X0, X1);
        "mvn x0, x1" => |a| a.mvn(W64, X0, X1);
    });
}

/// DecodeBitMasks from the Arm ARM, for immediates.
fn decode_bit_masks(n: u32, imms: u32, immr: u32, width: Width) -> Option<u64> {
    let combined = (n << 6) | (!imms & 0x3f);
    if combined == 0 {
        return None;
    }
    let length = 31 - combined.leading_zeros();
    if length < 1 {
        return None;
    }
    let size = 1u32 << length;
    if size > width.bits() {
        return None;
    }
    let levels = size - 1;
    let s = imms & levels;
    let r = immr & levels;
    if s == levels {
        return None;
    }
    let ones = (1u64 << (s + 1)) - 1;
    let element = if size == 64 {
        ones.rotate_right(r)
    } else {
        let mask = (1u64 << size) - 1;
        ((ones >> r) | (ones << ((size - r) % size))) & mask
    };
    let mut value = 0;
    let mut position = 0;
    while position < width.bits() {
        value |= element << position;
        position += size;
    }
    Some(value)
}

#[test]
fn logical_immediates_round_trip_exhaustively() {
    for (width, expected_count) in [(W64, 5334), (W32, 1302)] {
        let mut values = std::collections::BTreeSet::new();
        for n in 0..2 {
            for immr in 0..64 {
                for imms in 0..64 {
                    let Some(value) = decode_bit_masks(n, imms, immr, width) else {
                        continue;
                    };
                    values.insert(value);
                    let encoding = encode_logical_immediate(value, width)
                        .unwrap_or_else(|| panic!("{value:#x} should be encodable for {width:?}"));
                    let decoded = decode_bit_masks(encoding >> 12, encoding & 0x3f, (encoding >> 6) & 0x3f, width);
                    assert_eq!(decoded, Some(value), "{value:#x} {width:?} encoded as {encoding:#x}");
                }
            }
        }
        assert_eq!(values.len(), expected_count);

        // Everything else is rejected.
        let mut state = 0x1234_5678_9abc_def1u64;
        for _ in 0..100_000 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let value = if width == W32 { state & 0xffff_ffff } else { state };
            assert_eq!(
                encode_logical_immediate(value, width).is_some(),
                values.contains(&value)
            );
        }
    }
    assert_eq!(encode_logical_immediate(0, W64), None);
    assert_eq!(encode_logical_immediate(u64::MAX, W64), None);
    assert_eq!(encode_logical_immediate(0xffff_ffff, W32), None);
    assert_eq!(encode_logical_immediate(0x1_0000_0000, W32), None);
    assert_eq!(encode_logical_immediate(0x1234, W64), None);
    assert_eq!(encode_logical_immediate(0xffff_ffff, W64).map(|_| ()), Some(()));
}

#[test]
#[should_panic(expected = "is not a valid 64-bit logical immediate")]
fn unencodable_logical_immediate_is_rejected() {
    bytes(|a| a.and_imm(W64, X0, X1, 0x1234));
}

#[test]
fn shifts_and_bitfields() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "lsl x0, x1, #0x3" => |a| a.lsl_imm(W64, X0, X1, 3);
        "lsl w0, w1, #0x1f" => |a| a.lsl_imm(W32, X0, X1, 31);
        "lsr x0, x1, #0x3f" => |a| a.lsr_imm(W64, X0, X1, 63);
        "asr w2, w3, #0x1" => |a| a.asr_imm(W32, X2, X3, 1);
        "lsl x0, x1, x2" => |a| a.lsl(W64, X0, X1, X2);
        "lsr w0, w1, w2" => |a| a.lsr(W32, X0, X1, X2);
        "asr x0, x1, x2" => |a| a.asr(W64, X0, X1, X2);
        "ror w0, w1, w2" => |a| a.ror(W32, X0, X1, X2);
        "sdiv w0, w1, w2" => |a| a.sdiv(W32, X0, X1, X2);
        "udiv x0, x1, x2" => |a| a.udiv(W64, X0, X1, X2);
        "sxtw x0, w1" => |a| a.sxtw(X0, X1);
        "sxtb x0, w1" => |a| a.sxtb(W64, X0, X1);
        "sxth w0, w1" => |a| a.sxth(W32, X0, X1);
        "uxtb w0, w1" => |a| a.uxtb(X0, X1);
        "uxth w0, w1" => |a| a.uxth(X0, X1);
        "ubfx x0, x1, #0x4, #0x8" => |a| a.ubfm(W64, X0, X1, 4, 11);
        "sbfiz x0, x1, #0x4, #0x4" => |a| a.sbfm(W64, X0, X1, 60, 3);
    });
}

#[test]
fn multiplication() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "mul w0, w1, w2" => |a| a.mul(W32, X0, X1, X2);
        "mul x0, x1, x2" => |a| a.mul(W64, X0, X1, X2);
        "madd x0, x1, x2, x3" => |a| a.madd(W64, X0, X1, X2, X3);
        "msub w0, w1, w2, w3" => |a| a.msub(W32, X0, X1, X2, X3);
        "smull x0, w1, w2" => |a| a.smull(X0, X1, X2);
        "umull x0, w1, w2" => |a| a.umull(X0, X1, X2);
        "smulh x0, x1, x2" => |a| a.smulh(X0, X1, X2);
        "umulh x0, x1, x2" => |a| a.umulh(X0, X1, X2);
        "cmp x17, w17, sxtw" => |a| a.check_smull_overflow(X17);
        "cmp x1, x0, asr #0x3f" => |a| a.check_smulh_overflow(X1, X0);
    });
}

#[test]
fn conditional_select_and_condition_codes() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "csel x0, x1, x2, eq" => |a| a.csel(W64, X0, X1, X2, Cond::Eq);
        "csinc w0, w1, w2, ne" => |a| a.csinc(W32, X0, X1, X2, Cond::Ne);
        "csinv x0, x1, x2, lt" => |a| a.csinv(W64, X0, X1, X2, Cond::Lt);
        "csneg x0, x1, x2, ge" => |a| a.csneg(W64, X0, X1, X2, Cond::Ge);
        "cset w0, lt" => |a| a.cset(W32, X0, Cond::Lt);
        "csetm x0, hi" => |a| a.csetm(W64, X0, Cond::Hi);
    });
    let conditions = [
        (Cond::Eq, "eq"),
        (Cond::Ne, "ne"),
        (Cond::Hs, "hs"),
        (Cond::Lo, "lo"),
        (Cond::Mi, "mi"),
        (Cond::Pl, "pl"),
        (Cond::Vs, "vs"),
        (Cond::Vc, "vc"),
        (Cond::Hi, "hi"),
        (Cond::Ls, "ls"),
        (Cond::Ge, "ge"),
        (Cond::Lt, "lt"),
        (Cond::Gt, "gt"),
        (Cond::Le, "le"),
    ];
    for (cond, name) in conditions {
        let code = bytes(|a| {
            let label = a.new_label();
            a.bind(label);
            a.b_cond(cond, label);
            a.cset(W64, X3, cond);
        });
        assert_eq!(
            disassembler::aarch64(&code),
            [format!("b.{name} 0x0"), format!("cset x3, {name}")]
        );
        assert_eq!(cond.invert().invert(), cond);
        assert_ne!(cond.invert(), cond);
    }
}

#[test]
fn loads_and_stores() {
    let register = |base, index, extend, shifted| MemOperand::Register {
        base,
        index,
        extend,
        shifted,
    };
    assert_encodings!(Assembler, disassembler::aarch64, {
        "ldr x0, [x1, #0x8]" => |a| a.ldr(W64, X0, MemOperand::Offset(X1, 8));
        "ldr x0, [x1]" => |a| a.ldr(W64, X0, MemOperand::Offset(X1, 0));
        "ldr x0, [x1, #0x7ff8]" => |a| a.ldr(W64, X0, MemOperand::Offset(X1, 32760));
        "ldur x0, [sp, #-0x8]" => |a| a.ldr(W64, X0, MemOperand::Offset(SP, -8));
        "ldur x0, [x1, #0x4]" => |a| a.ldr(W64, X0, MemOperand::Offset(X1, 4));
        "ldur x0, [x1, #0xff]" => |a| a.ldr(W64, X0, MemOperand::Offset(X1, 255));
        "ldur x0, [x1, #-0x100]" => |a| a.ldr(W64, X0, MemOperand::Offset(X1, -256));
        "ldr w0, [x1, #0x3ffc]" => |a| a.ldr(W32, X0, MemOperand::Offset(X1, 16380));
        "str xzr, [sp, #0x10]" => |a| a.str(W64, XZR, MemOperand::Offset(SP, 16));
        "str w3, [x28, #0x4]" => |a| a.str(W32, X3, MemOperand::Offset(X28, 4));
        "ldrb w0, [x1, #0xfff]" => |a| a.ldrb(X0, MemOperand::Offset(X1, 4095));
        "ldrh w0, [x1, #0x2]" => |a| a.ldrh(X0, MemOperand::Offset(X1, 2));
        "ldurh w0, [x1, #0x1]" => |a| a.ldrh(X0, MemOperand::Offset(X1, 1));
        "ldursb x0, [x1, #-0x1]" => |a| a.ldrsb(W64, X0, MemOperand::Offset(X1, -1));
        "ldrsb w0, [x1]" => |a| a.ldrsb(W32, X0, MemOperand::Offset(X1, 0));
        "ldrsh x0, [x1, #0x2]" => |a| a.ldrsh(W64, X0, MemOperand::Offset(X1, 2));
        "ldrsh w0, [x1, #0x2]" => |a| a.ldrsh(W32, X0, MemOperand::Offset(X1, 2));
        "ldrsw x0, [x1, #0x4]" => |a| a.ldrsw(X0, MemOperand::Offset(X1, 4));
        "strb w0, [x1, #0x1]" => |a| a.strb(X0, MemOperand::Offset(X1, 1));
        "sturh w0, [x1, #-0x2]" => |a| a.strh(X0, MemOperand::Offset(X1, -2));
        "ldr d0, [x1, #0x8]" => |a| a.ldr_d(D0, MemOperand::Offset(X1, 8));
        "str d31, [sp]" => |a| a.str_d(D31, MemOperand::Offset(SP, 0));
        "stur d8, [x29, #-0x8]" => |a| a.str_d(D8, MemOperand::Offset(X29, -8));
        "ldr x0, [x1, x2, lsl #0x3]" => |a| a.ldr(W64, X0, register(X1, X2, Extend::Uxtx, true));
        "ldr x0, [sp, x16]" => |a| a.ldr(W64, X0, register(SP, X16, Extend::Uxtx, false));
        "ldr w0, [x1, w2, sxtw #0x2]" => |a| a.ldr(W32, X0, register(X1, X2, Extend::Sxtw, true));
        "ldrb w0, [x1, w2, uxtw]" => |a| a.ldrb(X0, register(X1, X2, Extend::Uxtw, false));
        "strh w0, [x1, x2, lsl #0x1]" => |a| a.strh(X0, register(X1, X2, Extend::Uxtx, true));
        "str d1, [x1, x2, lsl #0x3]" => |a| a.str_d(D1, register(X1, X2, Extend::Uxtx, true));
        "str x0, [sp, #-0x10]!" => |a| a.str(W64, X0, MemOperand::PreIndex(SP, -16));
        "ldr x0, [sp], #0x10" => |a| a.ldr(W64, X0, MemOperand::PostIndex(SP, 16));
        "str d8, [sp, #-0x10]!" => |a| a.str_d(D8, MemOperand::PreIndex(SP, -16));
        "ldr d8, [sp], #0x10" => |a| a.ldr_d(D8, MemOperand::PostIndex(SP, 16));
        "ldr s0, [x1, #0x4]" => |a| a.load_store(MemOp::LdrS, 0, MemOperand::Offset(X1, 4));
        "str s31, [x1, x2, lsl #0x2]" => |a| a.load_store(MemOp::StrS, 31, register(X1, X2, Extend::Uxtx, true));
        "str q0, [x1, #0x10]" => |a| a.load_store(MemOp::StrQ, 0, MemOperand::Offset(X1, 16));
        "stur q31, [x1, #0x8]" => |a| a.load_store(MemOp::StrQ, 31, MemOperand::Offset(X1, 8));
        "ldr q2, [x3, #0xfff0]" => |a| a.load_store(MemOp::LdrQ, 2, MemOperand::Offset(X3, 0xfff0));
        "stp x29, x30, [sp, #-0x10]!" => |a| a.stp(FP, LR, PairOperand::PreIndex(SP, -16));
        "ldp x29, x30, [sp], #0x10" => |a| a.ldp(FP, LR, PairOperand::PostIndex(SP, 16));
        "stp x19, x20, [sp, #0x10]" => |a| a.stp(X19, X20, PairOperand::Offset(SP, 16));
        "ldp x0, x1, [x2, #-0x200]" => |a| a.ldp(X0, X1, PairOperand::Offset(X2, -512));
        "stp d8, d9, [sp, #-0x10]!" => |a| a.stp_d(D8, D9, PairOperand::PreIndex(SP, -16));
        "ldp d8, d9, [sp, #0x1f8]" => |a| a.ldp_d(D8, D9, PairOperand::Offset(SP, 504));
    });
}

#[test]
#[should_panic(expected = "not encodable")]
fn unencodable_load_offset_is_rejected() {
    bytes(|a| a.ldr(W64, X0, MemOperand::Offset(X1, 32768)));
}

#[test]
fn branches_and_misc() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "b 0x4" => |a| {
            let label = a.new_label();
            a.b(label);
            a.bind(label);
        };
        "bl 0x4" => |a| {
            let label = a.new_label();
            a.bl(label);
            a.bind(label);
        };
        "nop; b.eq 0x0" => |a| {
            let label = a.new_label();
            a.bind(label);
            a.nop();
            a.b_cond(Cond::Eq, label);
        };
        "cbz x3, 0x0" => |a| {
            let label = a.new_label();
            a.bind(label);
            a.cbz(W64, X3, label);
        };
        "cbnz w3, 0x8; nop" => |a| {
            let label = a.new_label();
            a.cbnz(W32, X3, label);
            a.nop();
            a.bind(label);
        };
        "tbz x3, #0x28, 0x4" => |a| {
            let label = a.new_label();
            a.tbz(X3, 40, label);
            a.bind(label);
        };
        "tbnz w3, #0x3, 0x0" => |a| {
            let label = a.new_label();
            a.bind(label);
            a.tbnz(X3, 3, label);
        };
        "adr x0, 0x4" => |a| {
            let label = a.new_label();
            a.adr(X0, label);
            a.bind(label);
        };
        "nop; adr x5, 0x0" => |a| {
            let label = a.new_label();
            a.bind(label);
            a.nop();
            a.adr(X5, label);
        };
        "br x16" => |a| a.br(X16);
        "blr x17" => |a| a.blr(X17);
        "ret" => |a| a.ret();
        "nop" => |a| a.nop();
        "brk #0x0" => |a| a.brk(0);
        "brk #0xf000" => |a| a.brk(0xf000);
        "udf #0x0" => |a| a.udf(0);
    });
}

#[test]
fn scalar_doubles() {
    assert_encodings!(Assembler, disassembler::aarch64, {
        "fmov d0, d1" => |a| a.fmov(D0, D1);
        "fmov d0, x1" => |a| a.fmov_from_gpr(D0, X1);
        "fmov d0, xzr" => |a| a.fmov_from_gpr(D0, XZR);
        "fmov x0, d1" => |a| a.fmov_to_gpr(X0, D1);
        "fmov d0, #1.5" => |a| a.fmov_imm(D0, 1.5);
        "fmov d1, #-2.0" => |a| a.fmov_imm(D1, -2.0);
        "fmov d2, #0.125" => |a| a.fmov_imm(D2, 0.125);
        "fmov d3, #31.0" => |a| a.fmov_imm(D3, 31.0);
        "fadd d0, d1, d2" => |a| a.fadd(D0, D1, D2);
        "fsub d3, d4, d5" => |a| a.fsub(D3, D4, D5);
        "fmul d29, d30, d31" => |a| a.fmul(D29, D30, D31);
        "fdiv d16, d17, d8" => |a| a.fdiv(D16, D17, D8);
        "fsqrt d0, d1" => |a| a.fsqrt(D0, D1);
        "frintm d0, d1" => |a| a.frintm(D0, D1);
        "frintp d30, d2" => |a| a.frintp(D30, D2);
        "fneg d0, d1" => |a| a.fneg(D0, D1);
        "fabs d0, d1" => |a| a.fabs(D0, D1);
        "fcmp d0, d1" => |a| a.fcmp(D0, D1);
        "fcmp d7, #0.0" => |a| a.fcmp_zero(D7);
        "scvtf d0, w1" => |a| a.scvtf(W32, D0, X1);
        "scvtf d0, x1" => |a| a.scvtf(W64, D0, X1);
        "fcvtzs w0, d1" => |a| a.fcvtzs(W32, X0, D1);
        "fcvtzs x0, d1" => |a| a.fcvtzs(W64, X0, D1);
        "fjcvtzs w0, d1" => |a| a.fjcvtzs(X0, D1);
        "fcvt d0, s1" => |a| a.fcvt_double_from_single(D0, D1);
        "fcvt s31, d8" => |a| a.fcvt_single_from_double(D31, D8);
    });
}

#[test]
fn fp_immediates_round_trip() {
    for imm8 in 0..=255u8 {
        // VFPExpandImm for a double.
        let sign = u64::from(imm8 >> 7);
        let b = u64::from((imm8 >> 6) & 1);
        let exponent = ((b ^ 1) << 10) | (if b == 1 { 0xff << 2 } else { 0 }) | u64::from((imm8 >> 4) & 3);
        let fraction = u64::from(imm8 & 0xf) << 48;
        let value = f64::from_bits((sign << 63) | (exponent << 52) | fraction);
        assert_eq!(encode_fp_immediate(value), Some(imm8), "{value}");
    }
    for value in [0.0, -0.0, 0.1, 32.0, 1.0 / 3.0, f64::NAN, f64::INFINITY, 0.0625] {
        assert_eq!(encode_fp_immediate(value), None, "{value}");
    }
}

#[test]
fn backward_branches_out_of_range_use_a_long_branch() {
    // TBZ reaches exactly 32 KiB back.
    let code = bytes(|a| {
        let label = a.new_label();
        a.bind(label);
        for _ in 0..8192 {
            a.nop();
        }
        a.tbz(X3, 3, label);
        a.tbnz(X4, 5, label);
        a.cbz(W64, X5, label);
    });
    assert_eq!(
        disassembler::aarch64_range(&code, 0x8000, code.len()),
        ["tbz w3, #0x3, 0x0", "tbz w4, #0x5, 0x800c", "b 0x0", "cbz x5, 0x0"]
    );

    // B.cond reaches exactly 1 MiB back.
    let code = bytes(|a| {
        let label = a.new_label();
        a.bind(label);
        for _ in 0..(1 << 18) {
            a.nop();
        }
        a.b_cond(Cond::Eq, label);
        a.b_cond(Cond::Lt, label);
        a.cbnz(W32, X0, label);
    });
    assert_eq!(
        disassembler::aarch64_range(&code, 0x10_0000, code.len()),
        ["b.eq 0x0", "b.ge 0x10000c", "b 0x0", "cbz w0, 0x100014", "b 0x0"]
    );
}

/// Follows the branch at `offset` and returns its target.
fn branch_target(code: &[u8], offset: usize) -> usize {
    let word = word_at(code, offset);
    let displacement = if word & 0xfc00_0000 == 0x1400_0000 {
        ((word << 6) as i32 >> 6) * 4
    } else if word & 0x7e00_0000 == 0x3600_0000 {
        ((word << 13) as i32 >> 18) * 4
    } else {
        ((word << 8) as i32 >> 13) * 4
    };
    (offset as i64 + i64::from(displacement)) as usize
}

#[test]
fn forward_branches_get_veneers_before_going_out_of_range() {
    let mut assembler = Assembler::new();
    let far = assembler.new_label();
    let near = assembler.new_label();
    assembler.tbz(X3, 3, far);
    assembler.cbz(W64, X4, far);
    assembler.tbnz(X5, 63, near);
    for _ in 0..8 {
        assembler.nop();
    }
    assembler.bind(near);
    let nop_count = 9000;
    for _ in 0..nop_count {
        assembler.nop();
    }
    assembler.bind(far);
    assembler.ret();
    let far_offset = assembler.label_offset(far).unwrap();
    let code = assembler.finish().unwrap();

    // The near branch was resolved directly.
    assert_eq!(branch_target(&code, 8), 8 + 4 * 9);
    // The TBZ goes through a veneer; the CBZ still reaches directly.
    let veneer = branch_target(&code, 0);
    assert!(veneer < 32 * 1024);
    assert_eq!(word_at(&code, veneer) & 0xfc00_0000, 0x1400_0000);
    assert_eq!(branch_target(&code, veneer), far_offset);
    assert_eq!(branch_target(&code, 4), far_offset);
    // The fallthrough path jumps over the pool.
    let pool_start = veneer - 4;
    assert_eq!(branch_target(&code, pool_start), veneer + 4);
    // Everything else is the nops and the ret.
    let nops = code
        .chunks(4)
        .filter(|word| u32::from_le_bytes((*word).try_into().unwrap()) == NOP)
        .count();
    assert_eq!(nops, nop_count + 8);
    assert_eq!(code.len(), 4 * (3 + 8 + nop_count + 2 + 1));
}

#[test]
fn veneer_pools_do_not_split_a_label_reference_from_its_fixup() {
    // Sweep the distance so that some pool is due exactly before the
    // reference, which is the last instruction before its label.
    let references: [fn(&mut Assembler, Label); 2] = [|a, label| a.b(label), |a, label| a.ldr_literal(X1, label)];
    for reference in references {
        for nop_count in 8100..8192 {
            let mut assembler = Assembler::new();
            let label = assembler.new_label();
            assembler.tbz(X3, 3, label);
            for _ in 0..nop_count {
                assembler.nop();
            }
            reference(&mut assembler, label);
            assembler.bind(label);
            assembler.ret();
            let code = assembler.finish().unwrap();
            assert_eq!(branch_target(&code, code.len() - 8), code.len() - 4, "{nop_count} nops");
        }
    }
}

#[test]
fn many_pending_branches_get_veneers() {
    let mut assembler = Assembler::new();
    let labels: Vec<Label> = (0..300).map(|_| assembler.new_label()).collect();
    let mut branches = Vec::new();
    for (i, label) in labels.iter().enumerate() {
        branches.push((assembler.offset(), *label));
        assembler.tbz(Gpr((i % 29) as u8), (i % 64) as u8, *label);
        if i % 2 == 0 {
            assembler.b_cond(Cond::Ne, *label);
        }
    }
    for _ in 0..(40 * 1024 / 4) {
        assembler.nop();
    }
    for label in &labels {
        assembler.bind(*label);
        assembler.nop();
    }
    let offsets: Vec<usize> = labels
        .iter()
        .map(|label| assembler.label_offset(*label).unwrap())
        .collect();
    let code = assembler.finish().unwrap();
    for (i, (offset, _)) in branches.iter().enumerate() {
        let mut target = branch_target(&code, *offset);
        if target != offsets[i] {
            // A veneer.
            target = branch_target(&code, target);
        }
        assert_eq!(target, offsets[i], "branch {i}");
    }
}

#[test]
fn forward_branch_beyond_branch_range_fails() {
    let mut assembler = Assembler::new();
    let label = assembler.new_label();
    assembler.adr(X0, label);
    for _ in 0..(1 << 18) {
        assembler.nop();
    }
    assembler.bind(label);
    assert_eq!(assembler.finish(), Err(AsmError::BranchOutOfRange));
}

#[test]
fn unbound_label_fails() {
    let mut assembler = Assembler::new();
    let label = assembler.new_label();
    assembler.cbz(W64, X0, label);
    assert_eq!(assembler.finish(), Err(AsmError::UnboundLabel));
}

#[test]
fn alignment_and_data() {
    let code = bytes(|a| {
        a.ret();
        a.align_code(16);
        a.data_u32(0x1234_5678);
        a.data_bytes(&[1]);
        a.align_data(8);
        a.data_u64(u64::MAX);
    });
    assert_eq!(code.len(), 32);
    assert_eq!(word_at(&code, 4), NOP);
    assert_eq!(word_at(&code, 12), NOP);
    assert_eq!(word_at(&code, 16), 0x1234_5678);
    assert_eq!(code[20..24], [1, 0, 0, 0]);
    assert_eq!(code[24..], [0xff; 8]);
}

mod macro_assembler {
    use super::super::super::Address;
    use super::super::super::Condition;
    use super::super::super::DoubleCondition;
    use super::super::super::FprSet;
    use super::super::super::GprSet;
    use super::super::super::NegativeZero;
    use super::super::super::PortableMacroAssembler;
    use super::super::super::Scale;
    use super::*;

    fn base(register: Gpr, displacement: i32) -> Address {
        Address::new(register, displacement)
    }

    fn indexed(register: Gpr, index: Gpr, scale: Scale, displacement: i32) -> Address {
        Address::indexed(register, index, scale, displacement)
    }

    #[test]
    fn addresses_are_legalized() {
        assert_encodings!(MacroAssembler, disassembler::aarch64, {
            "ldr x0, [x1, #0x8]" => |m| m.load64(X0, &base(X1, 8));
            "ldur x0, [x1, #-0x8]" => |m| m.load64(X0, &base(X1, -8));
            "mov x16, #0x10000; ldr x0, [x1, x16]" => |m| m.load64(X0, &base(X1, 0x10000));
            "mov x16, #0x8000; ldr d0, [sp, x16]" => |m| m.load_double(D0, &base(SP, 32768));
            "ldr x0, [x1, x2, lsl #0x3]" => |m| m.load64(X0, &indexed(X1, X2, Scale::Eight, 0));
            "ldrb w0, [x1, x2]" => |m| m.load8(X0, &indexed(X1, X2, Scale::One, 0));
            "add x16, x1, x2, uxtx #0x3; ldr w0, [x16]" => |m| m.load32(X0, &indexed(X1, X2, Scale::Eight, 0));
            "mov x16, #0x10; add x16, sp, x16; ldr x0, [x16, x2, lsl #0x3]" => |m| m.load64(X0, &indexed(SP, X2, Scale::Eight, 16));
            "mov x16, #0x3; add x16, x1, x16, uxtx; add x16, x16, x2, lsl #0x2; ldrb w0, [x16]" => |m| m.load8(X0, &indexed(X1, X2, Scale::Four, 3));
            "ldrsh x3, [x4, #0x2]" => |m| m.load16_sign_extend(X3, &base(X4, 2));
            "ldrsw x3, [x4, #0x4]" => |m| m.load32_sign_extend(X3, &base(X4, 4));
            "str xzr, [sp, #0x8]" => |m| m.store_imm64(&base(SP, 8), 0);
            "mov w17, #0x5; str w17, [x0]" => |m| m.store_imm32(&base(X0, 0), 5);
            "strh w5, [x6, #0x6]" => |m| m.store16(&base(X6, 6), X5);
            "add x0, sp, x1, lsl #0x3; add x0, x0, #0x18" => |m| m.load_effective_address(X0, &indexed(SP, X1, Scale::Eight, 24));
            "mov x0, sp" => |m| m.move64(X0, SP);
        });
    }

    #[test]
    fn operations_are_legalized() {
        assert_encodings!(MacroAssembler, disassembler::aarch64, {
            "sub x0, x1, #0x5" => |m| m.add64_imm(X0, X1, -5);
            "mov x17, #0x3456; movk x17, #0x12, lsl #0x10; add x0, x1, x17" => |m| m.add64_imm(X0, X1, 0x12_3456);
            "add sp, sp, #0x10, lsl #0xc" => |m| m.add64_imm(SP, SP, 0x10000);
            "mov x17, #0x3456; movk x17, #0x12, lsl #0x10; sub sp, sp, x17" => |m| m.sub64_imm(SP, SP, 0x12_3456);
            "and x0, x1, #0xff" => |m| m.and64_imm(X0, X1, 0xff);
            "mov x17, #0x1234; and x0, x1, x17" => |m| m.and64_imm(X0, X1, 0x1234);
            "mov x0, x1" => |m| m.or64_imm(X0, X1, 0);
            "mov w0, w0" => |m| m.and32_imm(X0, X0, 0xffff_ffff);
            "mvn x0, x1" => |m| m.xor64_imm(X0, X1, u64::MAX);
            "mov w0, w0" => |m| m.shl32_imm(X0, X0, 32);
            "asr x0, x1, #0x1" => |m| m.sar64_imm(X0, X1, 65);
            "cmp w1, #0x1, lsl #0xc; cset w0, hi" => |m| m.compare32_imm_set(Condition::Above, X0, X1, 4096);
            "cmp x0, x1; csel x2, x3, x4, lt" => |m| m.select64(Condition::LessThan, X0, X1, X2, X3, X4);
            "ldr x16, 0x8; blr x16" => |m| m.call_absolute(0x1234_5678_9abc);
            "fmov d0, #1.0" => |m| m.move_double_imm(D0, 1.0);
            "fmov d0, xzr" => |m| m.move_double_imm(D0, 0.0);
            "ldr d0, 0x8" => |m| m.move_double_imm(D0, -0.0);
            "fcmp d0, d1; cset w0, mi; csinc w0, w0, wzr, le" => |m| m.compare_double_set(DoubleCondition::NotEqual, X0, D0, D1);
        });
    }

    #[test]
    fn branches() {
        macro_rules! backward {
            ($m:ident, $emit:expr) => {{
                let label = $m.new_label();
                $m.bind(label);
                $emit(&mut $m, label);
            }};
        }
        assert_encodings!(MacroAssembler, disassembler::aarch64, {
            "cbz w0, 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch32_imm(Condition::Equal, X0, 0, l));
            "cmn x0, #0x1; b.lt 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch64_imm(Condition::LessThan, X0, -1, l));
            "mov x17, #0x10000000000; cmp x0, x17; b.eq 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch64_imm(Condition::Equal, X0, 1 << 40, l));
            "tbnz x0, #0x30, 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_test64(Condition::NonZero, X0, 1 << 48, l));
            "tst x0, #0xffff000000000000; b.eq 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_test64(Condition::Zero, X0, 0xffff_0000_0000_0000, l));
            "tst w0, #0x80000000; b.mi 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_test32(Condition::Negative, X0, 0x8000_0000, l));
            "mov x17, #0x1234; tst x0, x17; b.eq 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_test64(Condition::Zero, X0, 0x1234, l));
            "ldr x16, [x1, #0x8]; cmp x16, #0x7; b.ne 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch64_memory_imm(Condition::NotEqual, &base(X1, 8), 7, l));
            "ldr w16, [x1]; cmn w16, #0x2; b.ge 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch32_memory_imm(Condition::GreaterThanOrEqual, &base(X1, 0), -2, l));
            "smull x17, w1, w2; cmp x17, w17, sxtw; b.ne 0x0; mov w0, w17" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_mul32_overflow(X0, X1, X2, l));
            "subs w0, w0, #0x1; b.vs 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_add32_imm_overflow(X0, X0, -1, l));
            "negs w3, w4; b.vs 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_neg32_overflow(X3, X4, l));
            "fcmp d0, d1; b.eq 0x0; b.vs 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_double(DoubleCondition::EqualOrUnordered, D0, D1, l));
            "fcmp d0, d1; b.mi 0x0" => |m| backward!(m, |m: &mut MacroAssembler, l| m.branch_double(DoubleCondition::LessThan, D0, D1, l));
            "fcvtzs w0, d1; scvtf d31, w0; fcmp d31, d1; b.ne 0x0; cbnz w0, 0x1c; fmov x17, d1; tbnz x17, #0x3f, 0x0" => |m| {
                backward!(m, |m: &mut MacroAssembler, l| m.branch_convert_double_to_int32(X0, D1, l, NegativeZero::Fail));
            };
        });
    }

    #[test]
    fn frames() {
        let frame = MacroAssembler::frame(GprSet::of(&[X19, X20, X21, FP, LR, SP]), FprSet::of(&[D8]), 20);
        assert_eq!(frame.saved_gprs, GprSet::of(&[X19, X20, X21]));
        assert_eq!(frame.locals_size, 32);
        assert_eq!(frame.caller_stack_offset, 96);
        assert_encodings!(MacroAssembler, disassembler::aarch64, {
            "stp x29, x30, [sp, #-0x10]!; mov x29, sp; stp x19, x20, [sp, #-0x10]!; str x21, [sp, #-0x10]!; str d8, [sp, #-0x10]!; sub sp, sp, #0x20; add sp, sp, #0x20; ldr d8, [sp], #0x10; ldr x21, [sp], #0x10; ldp x19, x20, [sp], #0x10; ldp x29, x30, [sp], #0x10" => |m| {
                m.emit_prologue(&frame);
                m.emit_epilogue(&frame);
            };
            "stp x29, x30, [sp, #-0x10]!; mov x29, sp; mov x17, #0x10; movk x17, #0x1, lsl #0x10; sub sp, sp, x17; mov x17, #0x10; movk x17, #0x1, lsl #0x10; add sp, sp, x17; ldp x29, x30, [sp], #0x10" => |m| {
                let frame = MacroAssembler::frame(GprSet::EMPTY, FprSet::EMPTY, 0x10008);
                m.emit_prologue(&frame);
                m.emit_epilogue(&frame);
            };
        });
    }

    #[test]
    fn register_sets_are_consistent() {
        type M = MacroAssembler;
        assert!(M::ALLOCATABLE_GPRS.intersection(M::SCRATCH_GPRS).is_empty());
        assert!(M::ALLOCATABLE_FPRS.intersection(M::SCRATCH_FPRS).is_empty());
        assert!(!M::ALLOCATABLE_GPRS.contains(M::STACK_POINTER));
        assert!(!M::ALLOCATABLE_GPRS.contains(M::FRAME_POINTER));
        assert!(!M::ALLOCATABLE_GPRS.contains(LR));
        assert!(!M::ALLOCATABLE_GPRS.contains(X18));
        assert!(M::CALLEE_SAVED_GPRS.intersection(M::CALLER_SAVED_GPRS).is_empty());
        assert!(M::CALLEE_SAVED_FPRS.intersection(M::CALLER_SAVED_FPRS).is_empty());
        assert!(M::SCRATCH_GPRS.difference(M::CALLER_SAVED_GPRS).is_empty());
        assert_eq!(M::ALLOCATABLE_GPRS.len(), 26);
        assert_eq!(M::ALLOCATABLE_FPRS.len(), 31);
        assert_eq!(
            M::CALLEE_SAVED_GPRS.iter().collect::<Vec<_>>(),
            (19..31).map(Gpr).collect::<Vec<_>>()
        );
    }
}
