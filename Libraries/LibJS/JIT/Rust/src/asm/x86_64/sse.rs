/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Instruction selection for x86-64-v2 hosts. The raw encoder (`Assembler::new`) retains the requested encoding;
//! `Assembler::for_host` selects SSE sequences where AVX/AVX2 are unavailable. No YMM values are used by either JIT.

use super::{Assembler, BitCount, ByteRegisters, Cond, Fpr, Gpr, Prefix, Rm, VexMap, VexOpcode, VexOperand, VexPrefix};
use crate::asm::{Address, Width};

/// Optional instructions beyond the SSE4.2/POPCNT baseline. These are independent CPU features: AVX2 alone does
/// not imply BMI or LZCNT, and AVX detection must also account for OS support for saving the extended registers.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct CpuFeatures {
    pub avx: bool,
    pub avx2: bool,
    pub bmi1: bool,
    pub bmi2: bool,
    pub lzcnt: bool,
}

impl CpuFeatures {
    pub const X86_64_V2: Self = Self {
        avx: false,
        avx2: false,
        bmi1: false,
        bmi2: false,
        lzcnt: false,
    };
    pub const X86_64_V3: Self = Self {
        avx: true,
        avx2: true,
        bmi1: true,
        bmi2: true,
        lzcnt: true,
    };

    pub fn host() -> Self {
        #[cfg(target_arch = "x86_64")]
        {
            static FEATURES: std::sync::OnceLock<CpuFeatures> = std::sync::OnceLock::new();
            *FEATURES.get_or_init(|| Self {
                avx: std::arch::is_x86_feature_detected!("avx"),
                avx2: std::arch::is_x86_feature_detected!("avx2"),
                bmi1: std::arch::is_x86_feature_detected!("bmi1"),
                bmi2: std::arch::is_x86_feature_detected!("bmi2"),
                lzcnt: std::arch::is_x86_feature_detected!("lzcnt"),
            })
        }
        #[cfg(not(target_arch = "x86_64"))]
        Self::X86_64_V2
    }
}

impl VexOpcode {
    /// Only swap inputs whose complete results are identical in either order. Floating-point arithmetic may
    /// select a NaN payload by operand order, and scalar operations also copy upper lanes from the first input.
    fn has_commutative_inputs(self) -> bool {
        use VexMap::{Map0F, Map0F38};
        use VexPrefix::{None as NoPrefix, OperandSize as P66};
        matches!(
            (self.map, self.prefix, self.opcode),
            (Map0F, NoPrefix | P66, 0x54 | 0x56 | 0x57) // AND/OR/XORPS/PD
                | (Map0F, P66,
                    0xd4 | 0xfc..=0xfe // PADDQ/B/W/D
                        | 0xdc | 0xdd | 0xec | 0xed // Saturating byte/word additions
                        | 0xd5 | 0xe4 | 0xe5 | 0xf4 | 0xf5 // PMULLW/HUW/HW/UDQ, PMADDWD
                        | 0xdb | 0xeb | 0xef // PAND/POR/PXOR
                )
                | (Map0F38, P66, 0x0b | 0x28 | 0x40) // PMULHRSW, PMULDQ, PMULLD
        )
    }
}

impl Assembler {
    /// Emit the legacy encoding with the same opcode map, mandatory prefix and operands as a VEX instruction.
    fn legacy(&mut self, opcode: VexOpcode, reg: u8, rm: Rm, immediate_size: usize) {
        let prefix = match opcode.prefix {
            VexPrefix::None => Prefix::None,
            VexPrefix::OperandSize => Prefix::OperandSize,
            VexPrefix::Rep => Prefix::Rep,
            VexPrefix::Repne => Prefix::Repne,
        };
        let bytes = [
            0x0f,
            match opcode.map {
                VexMap::Map0F => 0,
                VexMap::Map0F38 => 0x38,
                VexMap::Map0F3A => 0x3a,
            },
            opcode.opcode,
        ];
        let short = [0x0f, opcode.opcode];
        let bytes = if opcode.map == VexMap::Map0F {
            &short[..]
        } else {
            &bytes[..]
        };
        self.emit_instruction(prefix, opcode.w, bytes, reg, rm, ByteRegisters::No, immediate_size);
    }

    /// SSE writes its first input. If the output aliases the second input, swap commutative inputs or preserve the
    /// second input in a temporary register, saving the temporary on the stack. Even the JIT's reserved scratch
    /// register may be an input here. LEA preserves flags, and MOVUPS works with any caller stack alignment.
    fn legacy_binary(&mut self, opcode: VexOpcode, dst: u8, mut lhs: u8, mut rhs: Rm, immediate: Option<u8>) {
        if rhs == Rm::Reg(dst) && dst != lhs && opcode.has_commutative_inputs() {
            rhs = Rm::Reg(lhs);
            lhs = dst;
        }
        let temporary = if rhs == Rm::Reg(dst) && dst != lhs {
            let temp = (0..16).find(|&reg| reg != dst && reg != lhs).unwrap();
            let sp = super::RSP;
            self.lea(Width::W64, sp, &Address::new(sp, -16));
            self.legacy(
                VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x11),
                temp,
                Rm::Mem(Address::new(sp, 0)),
                0,
            );
            self.movapd_rr(Fpr(temp), Fpr(dst));
            Some(temp)
        } else {
            None
        };
        if dst != lhs {
            self.movapd_rr(Fpr(dst), Fpr(lhs));
        }
        self.legacy(
            opcode,
            dst,
            temporary.map_or(rhs, Rm::Reg),
            usize::from(immediate.is_some()),
        );
        if let Some(immediate) = immediate {
            self.buffer.emit_u8(immediate);
        }
        if let Some(temp) = temporary {
            let sp = super::RSP;
            self.legacy(
                VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x10),
                temp,
                Rm::Mem(Address::new(sp, 0)),
                0,
            );
            self.lea(Width::W64, sp, &Address::new(sp, 16));
        }
    }

    /// Scalar AVX instructions all have SSE equivalents. Conversions from a GPR cannot alias an XMM input.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn simd_instruction(
        &mut self,
        prefix: VexPrefix,
        map: VexMap,
        w: bool,
        opcode: u8,
        reg: u8,
        lhs: u8,
        rm: Rm,
        immediate_size: usize,
    ) {
        if self.cpu_features().avx {
            self.vex_instruction(prefix, map, w, opcode, reg, lhs, rm, immediate_size);
            return;
        }
        let opcode = VexOpcode { prefix, map, opcode, w };
        let merges = match (map, prefix, opcode.opcode) {
            (VexMap::Map0F, VexPrefix::Rep | VexPrefix::Repne, 0x2a) => {
                if reg != lhs {
                    self.movapd_rr(Fpr(reg), Fpr(lhs));
                }
                false
            }
            (VexMap::Map0F, _, 0x51 | 0x54..=0x5f) | (VexMap::Map0F3A, _, 0x0a | 0x0b) => true,
            _ => false,
        };
        if merges {
            // The immediate, if any, is appended by the scalar emitter. It must be adjacent to the instruction,
            // before restoring a temporary, so handle rounding separately (its two sources are always identical).
            if immediate_size != 0 {
                debug_assert_eq!(rm, Rm::Reg(lhs));
                if reg != lhs {
                    self.movapd_rr(Fpr(reg), Fpr(lhs));
                }
                self.legacy(opcode, reg, rm, immediate_size);
            } else {
                self.legacy_binary(opcode, reg, lhs, rm, None);
            }
        } else {
            self.legacy(opcode, reg, rm, immediate_size);
        }
    }

    pub(super) fn legacy_vector(
        &mut self,
        mut opcode: VexOpcode,
        reg: u8,
        lhs: u8,
        operand: VexOperand,
        mut immediate: Option<u8>,
    ) {
        use VexMap::{Map0F, Map0F3A, Map0F38};
        use VexPrefix::{None as NoPrefix, OperandSize as P66, Rep as PF3, Repne as PF2};
        let rm = match operand {
            VexOperand::Register(reg) => Rm::Reg(reg),
            VexOperand::Memory(address) => Rm::Mem(address),
            VexOperand::Label(label) => Rm::Rip(label),
        };
        let key = (opcode.map, opcode.prefix, opcode.opcode);
        match key {
            (Map0F38, P66, 0x18 | 0x58 | 0x59 | 0x78 | 0x79) => {
                self.broadcast(opcode.opcode, reg, rm);
                return;
            }
            (Map0F3A, P66, 0x02) => {
                // PBLENDW selects words; duplicate each of VPBLENDD's four lane bits.
                let mask = immediate.unwrap();
                immediate = Some((0..4).fold(0, |words, lane| words | ((((mask >> lane) & 1) * 3) << (2 * lane))));
                opcode.opcode = 0x0e;
            }
            (Map0F3A, P66, 0x04) => {
                // VPERMILPS with an immediate is PSHUFD on the same 128 bits.
                opcode.map = Map0F;
                opcode.opcode = 0x70;
                self.legacy(opcode, reg, rm, 1);
                self.buffer.emit_u8(immediate.unwrap());
                return;
            }
            (Map0F3A, P66, 0x05) => {
                // SHUFPD with both sources equal selects the same two lanes as VPERMILPD.
                let Rm::Reg(src) = rm else {
                    panic!("VPERMILPD needs a register")
                };
                opcode.map = Map0F;
                opcode.opcode = 0xc6;
                self.legacy_binary(opcode, reg, src, rm, immediate);
                return;
            }
            (Map0F, P66, 0x71..=0x73) => {
                // VEX puts the destination in vvvv; SSE's group encoding puts it in r/m.
                let Rm::Reg(src) = rm else {
                    panic!("immediate vector shift needs a register")
                };
                if lhs != src {
                    self.movapd_rr(Fpr(lhs), Fpr(src));
                }
                self.legacy(opcode, reg, Rm::Reg(lhs), 1);
                self.buffer.emit_u8(immediate.unwrap());
                return;
            }
            (Map0F, P66, 0xc4) | (Map0F3A, P66, 0x20 | 0x22) => {
                // PINSR's second input is a GPR (or memory), not an XMM register.
                if reg != lhs {
                    self.movapd_rr(Fpr(reg), Fpr(lhs));
                }
                self.legacy(opcode, reg, rm, 1);
                self.buffer.emit_u8(immediate.unwrap());
                return;
            }
            _ => {}
        }
        let unary = matches!(key,
            (Map0F, NoPrefix | P66, 0x10 | 0x11 | 0x28 | 0x2e | 0x50 | 0x51 | 0x5a | 0x5b)
            | (Map0F, P66, 0x6e | 0x7e | 0x70 | 0xd7 | 0xe6)
            | (Map0F, PF3, 0x7e | 0x5b | 0xe6)
            | (Map0F, PF2, 0x12 | 0xe6)
            | (Map0F38, P66, 0x17 | 0x1c..=0x1e | 0x20..=0x25 | 0x30..=0x35)
            | (Map0F3A, P66, 0x08 | 0x09 | 0x14..=0x17)
        ) || (key == (Map0F, PF2, 0x10) && !matches!(rm, Rm::Reg(_)));
        if unary {
            self.legacy(opcode, reg, rm, usize::from(immediate.is_some()));
            if let Some(immediate) = immediate {
                self.buffer.emit_u8(immediate);
            }
        } else {
            self.legacy_binary(opcode, reg, lhs, rm, immediate);
        }
    }

    /// Broadcasts use exact-width loads: a load8_splat at the end of a Wasm memory must only read one byte.
    fn broadcast(&mut self, opcode: u8, dst: u8, src: Rm) {
        use VexMap::{Map0F, Map0F3A};
        use VexPrefix::OperandSize as P66;
        let sse = |map, opcode| VexOpcode::new(P66, map, opcode);
        match src {
            Rm::Reg(src) => {
                if dst != src {
                    self.movapd_rr(Fpr(dst), Fpr(src));
                }
            }
            _ => {
                let (load, immediate) = match opcode {
                    0x78 => (sse(Map0F3A, 0x20), Some(0)),                       // pinsrb
                    0x79 => (sse(Map0F, 0xc4), Some(0)),                         // pinsrw
                    0x59 => (VexOpcode::new(VexPrefix::Rep, Map0F, 0x7e), None), // movq
                    _ => (sse(Map0F, 0x6e), None),                               // movd
                };
                self.legacy(load, dst, src, usize::from(immediate.is_some()));
                if let Some(immediate) = immediate {
                    self.buffer.emit_u8(immediate);
                }
            }
        }
        if opcode == 0x78 {
            self.legacy(sse(Map0F, 0x60), dst, Rm::Reg(dst), 0); // punpcklbw
        }
        if matches!(opcode, 0x78 | 0x79) {
            self.legacy(sse(Map0F, 0x61), dst, Rm::Reg(dst), 0); // punpcklwd
        }
        if opcode == 0x59 {
            self.legacy(sse(Map0F, 0x6c), dst, Rm::Reg(dst), 0); // punpcklqdq
        } else {
            self.legacy(sse(Map0F, 0x70), dst, Rm::Reg(dst), 1); // pshufd
            self.buffer.emit_u8(0);
        }
    }

    pub(super) fn count_zeros(&mut self, op: BitCount, width: Width, dst: Gpr, src: Gpr) {
        let done = self.new_label();
        let zero = self.new_label();
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0x0f, op as u8],
            dst.0,
            Rm::Reg(src.0),
            ByteRegisters::No,
            0,
        );
        self.jcc(Cond::Equal, zero);
        if op == BitCount::Lzcnt {
            self.xor_ri(Width::W32, dst, if width == Width::W64 { 63 } else { 31 });
        }
        self.jmp(done);
        self.bind(zero);
        self.mov_ri32(dst, if width == Width::W64 { 64 } else { 32 });
        self.bind(done);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::asm::disassembler;
    use crate::asm::x86_64::{BitwiseOp, FloatOp, Precision, RAX, RDI, Rounding, XMM1, XMM8, XMM9};

    #[test]
    fn sse_encodings() {
        type Emit = fn(&mut Assembler);
        let cases: &[(&str, Emit)] = &[
            ("movapd xmm8, xmm9; subss xmm8, xmm1", |a| {
                a.vfloat_rrr(FloatOp::Sub, Precision::Single, XMM8, XMM9, XMM1);
            }),
            ("movapd xmm8, xmm9; addsd xmm8, qword ptr [rdi+8]", |a| {
                a.vfloat_rrm(FloatOp::Add, Precision::Double, XMM8, XMM9, &Address::new(RDI, 8));
            }),
            ("movapd xmm8, xmm9; andnps xmm8, xmm1", |a| {
                a.vbitwise_rrr(BitwiseOp::AndNot, XMM8, XMM9, XMM1);
            }),
            ("movapd xmm8, xmm9; roundsd xmm8, xmm9, 0xb", |a| {
                a.vround(Precision::Double, XMM8, XMM9, Rounding::TowardZero);
            }),
            ("movd xmm8, edi", |a| a.vmov_xr(Width::W32, XMM8, RDI)),
            ("movq rax, xmm8", |a| a.vmov_rx(Width::W64, RAX, XMM8)),
            ("movapd xmm8, xmm9; cvtsi2sd xmm8, rdi", |a| {
                a.vcvtsi2s(Precision::Double, Width::W64, XMM8, XMM9, RDI);
            }),
            ("cvttss2si rax, xmm8", |a| {
                a.vcvtts2si(Precision::Single, Width::W64, RAX, XMM8);
            }),
            ("movapd xmm8, xmm9; pblendw xmm8, xmm1, 0x33", |a| {
                a.vex(
                    VexOpcode::new(VexPrefix::OperandSize, VexMap::Map0F3A, 2),
                    8,
                    9,
                    VexOperand::Register(1),
                    Some(5),
                );
            }),
            (
                "pinsrb xmm8, byte ptr [rdi], 0; punpcklbw xmm8, xmm8; punpcklwd xmm8, xmm8; pshufd xmm8, xmm8, 0",
                |a| {
                    a.vex(
                        VexOpcode::new(VexPrefix::OperandSize, VexMap::Map0F38, 0x78),
                        8,
                        0,
                        VexOperand::Memory(Address::new(RDI, 0)),
                        None,
                    );
                },
            ),
            ("movapd xmm9, xmm8; psrlq xmm9, 7", |a| {
                a.vex(
                    VexOpcode::new(VexPrefix::OperandSize, VexMap::Map0F, 0x73),
                    2,
                    9,
                    VexOperand::Register(8),
                    Some(7),
                );
            }),
        ];
        for (expected, emit) in cases {
            let mut asm = Assembler::with_cpu_features(CpuFeatures::X86_64_V2);
            emit(&mut asm);
            assert_eq!(disassembler::x86_64(&asm.finish().unwrap()).join("; "), *expected);
        }
    }

    #[test]
    fn commutative_aliases_need_no_temporary() {
        use VexMap::{Map0F, Map0F38};
        use VexPrefix::{None as NoPrefix, OperandSize as P66};
        for (map, prefix, opcode, mnemonic) in [
            (Map0F, P66, 0xfc, "paddb"),
            (Map0F, P66, 0xfd, "paddw"),
            (Map0F, P66, 0xfe, "paddd"),
            (Map0F, P66, 0xd4, "paddq"),
            (Map0F, P66, 0xdc, "paddusb"),
            (Map0F, P66, 0xdd, "paddusw"),
            (Map0F, P66, 0xec, "paddsb"),
            (Map0F, P66, 0xed, "paddsw"),
            (Map0F, P66, 0xd5, "pmullw"),
            (Map0F, P66, 0xe4, "pmulhuw"),
            (Map0F, P66, 0xe5, "pmulhw"),
            (Map0F, P66, 0xf4, "pmuludq"),
            (Map0F, P66, 0xf5, "pmaddwd"),
            (Map0F38, P66, 0x0b, "pmulhrsw"),
            (Map0F38, P66, 0x28, "pmuldq"),
            (Map0F38, P66, 0x40, "pmulld"),
            (Map0F, P66, 0xdb, "pand"),
            (Map0F, P66, 0xeb, "por"),
            (Map0F, P66, 0xef, "pxor"),
            (Map0F, NoPrefix, 0x54, "andps"),
            (Map0F, NoPrefix, 0x56, "orps"),
            (Map0F, NoPrefix, 0x57, "xorps"),
            (Map0F, P66, 0x54, "andpd"),
            (Map0F, P66, 0x56, "orpd"),
            (Map0F, P66, 0x57, "xorpd"),
        ] {
            let mut asm = Assembler::with_cpu_features(CpuFeatures::X86_64_V2);
            asm.vex(VexOpcode::new(prefix, map, opcode), 9, 8, VexOperand::Register(9), None);
            assert_eq!(
                disassembler::x86_64(&asm.finish().unwrap()),
                [format!("{mnemonic} xmm9, xmm8")],
                "{mnemonic} should reuse its second input without spilling"
            );
        }
    }

    #[cfg(target_arch = "x86_64")]
    use crate::asm::executable_code::ExecutableCode;
    #[cfg(target_arch = "x86_64")]
    use crate::asm::x86_64::{RDX, RSI, XMM0};

    #[test]
    #[cfg(target_arch = "x86_64")]
    fn packed_float_aliases_preserve_nan_payloads() {
        let load = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x10);
        let store = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x11);
        for (prefix, a, b) in [
            (
                VexPrefix::None,
                0x7fc0_0004_7fc0_0003_7fc0_0002_7fc0_0001u128,
                0x7fc0_0008_7fc0_0007_7fc0_0006_7fc0_0005u128,
            ),
            (
                VexPrefix::OperandSize,
                0x7ff8_0000_0000_0002_7ff8_0000_0000_0001u128,
                0x7ff8_0000_0000_0004_7ff8_0000_0000_0003u128,
            ),
        ] {
            for opcode in [0x58, 0x59] {
                // ADDPS/PD and MULPS/PD
                let mut asm = Assembler::with_cpu_features(CpuFeatures::X86_64_V2);
                for (index, dst) in [1, 9].into_iter().enumerate() {
                    asm.vex(load, 8, 0, VexOperand::Memory(Address::new(RDI, 0)), None);
                    asm.vex(load, 9, 0, VexOperand::Memory(Address::new(RSI, 0)), None);
                    asm.vex(
                        VexOpcode::new(prefix, VexMap::Map0F, opcode),
                        dst,
                        8,
                        VexOperand::Register(9),
                        None,
                    );
                    asm.vex(
                        store,
                        dst,
                        0,
                        VexOperand::Memory(Address::new(RDX, index as i32 * 16)),
                        None,
                    );
                }
                asm.ret();
                let code = ExecutableCode::new(&asm.finish().unwrap());
                let run: extern "sysv64" fn(*const u128, *const u128, *mut u128) =
                    unsafe { std::mem::transmute(code.address()) };
                let mut results = [0u128; 2];
                run(&raw const a, &raw const b, results.as_mut_ptr());
                assert_eq!(
                    results[0], results[1],
                    "{prefix:?} {opcode:#x} changed the NaN payload when dst aliases rhs"
                );
            }
        }
    }

    #[test]
    #[cfg(target_arch = "x86_64")]
    fn sse_three_operand_aliases_preserve_other_registers() {
        for op in [FloatOp::Add, FloatOp::Mul, FloatOp::Sub] {
            let load = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x10);
            let store = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x11);
            for lhs in [1, 8, 9] {
                for rhs in [1, 8, 9] {
                    for dst in [1, 8, 9] {
                        let mut asm = Assembler::with_cpu_features(CpuFeatures::X86_64_V2);
                        asm.vex(load, 0, 0, VexOperand::Memory(Address::new(RDI, 0)), None);
                        asm.vmovaps_rr(Fpr(lhs), XMM0);
                        if lhs != rhs {
                            asm.vex(load, rhs, 0, VexOperand::Memory(Address::new(RSI, 0)), None);
                        }
                        asm.vfloat_rrr(op, Precision::Double, Fpr(dst), Fpr(lhs), Fpr(rhs));
                        asm.vex(store, dst, 0, VexOperand::Memory(Address::new(RDX, 0)), None);
                        asm.vex(store, 0, 0, VexOperand::Memory(Address::new(RDX, 16)), None);
                        asm.ret();
                        let code = ExecutableCode::new(&asm.finish().unwrap());
                        let run: extern "sysv64" fn(*const u128, *const u128, *mut u128) =
                            unsafe { std::mem::transmute(code.address()) };
                        let a = (0x1234_5678_9abc_def0u128 << 64) | u128::from(7.5f64.to_bits());
                        let b = (0xfedc_ba98_7654_3210u128 << 64) | u128::from(2.0f64.to_bits());
                        let mut result = [0u128; 2];
                        run(&raw const a, &raw const b, result.as_mut_ptr());
                        let rhs_value = if lhs == rhs { 7.5f64 } else { 2.0f64 };
                        let expected = match op {
                            FloatOp::Add => 7.5 + rhs_value,
                            FloatOp::Mul => 7.5 * rhs_value,
                            FloatOp::Sub => 7.5 - rhs_value,
                            _ => unreachable!(),
                        };
                        assert_eq!(
                            result[0],
                            (a & (!0u128 << 64)) | u128::from(expected.to_bits()),
                            "dst={dst} lhs={lhs} rhs={rhs}"
                        );
                        assert_eq!(result[1], a, "temporary register was clobbered");
                    }
                }
            }
        }
    }

    #[test]
    #[cfg(target_arch = "x86_64")]
    fn bit_scans_handle_zero_and_aliasing() {
        for width in [Width::W32, Width::W64] {
            for op in [BitCount::Lzcnt, BitCount::Tzcnt] {
                for alias in [false, true] {
                    let mut asm = Assembler::with_cpu_features(CpuFeatures::X86_64_V2);
                    let dst = if alias { RDI } else { RAX };
                    asm.bit_count(op, width, dst, RDI);
                    asm.mov_rr(Width::W64, RAX, dst);
                    asm.ret();
                    let code = ExecutableCode::new(&asm.finish().unwrap());
                    let run: extern "sysv64" fn(u64) -> u64 = unsafe { std::mem::transmute(code.address()) };
                    for input in [0, 1, 2, 3, 1 << 31, 1 << 32, 1 << 63, u64::MAX] {
                        let expected = match (op, width) {
                            (BitCount::Lzcnt, Width::W32) => (input as u32).leading_zeros(),
                            (BitCount::Tzcnt, Width::W32) => (input as u32).trailing_zeros(),
                            (BitCount::Lzcnt, _) => input.leading_zeros(),
                            _ => input.trailing_zeros(),
                        };
                        assert_eq!(
                            run(input),
                            u64::from(expected),
                            "{op:?} {width:?} {input:#x} alias={alias}"
                        );
                    }
                }
            }
        }
    }
}
