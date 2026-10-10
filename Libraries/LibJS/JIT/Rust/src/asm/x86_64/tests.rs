/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::super::Address;
use super::super::Scale;
use super::super::Width::W32;
use super::super::Width::W64;
use super::super::disassembler;
use super::super::disassembler::assert_encodings;
use super::*;

fn base(register: Gpr, displacement: i32) -> Address {
    Address::new(register, displacement)
}

fn indexed(register: Gpr, index: Gpr, scale: Scale, displacement: i32) -> Address {
    Address::indexed(register, index, scale, displacement)
}

fn bytes(emit: impl FnOnce(&mut Assembler)) -> Vec<u8> {
    let mut assembler = Assembler::new();
    emit(&mut assembler);
    assembler.finish().unwrap()
}

#[test]
fn register_moves() {
    assert_encodings!(Assembler, disassembler::x86_64, {
        "mov rax, rbx" => |a| a.mov_rr(W64, RAX, RBX);
        "mov r8d, r15d" => |a| a.mov_rr(W32, R8, R15);
        "mov rsp, rbp" => |a| a.mov_rr(W64, RSP, RBP);
        "xchg rcx, r9" => |a| a.xchg_rr(W64, RCX, R9);
        "movzx eax, sil" => |a| a.movzx8_rr(RAX, RSI);
        "movzx r10d, r11b" => |a| a.movzx8_rr(R10, R11);
        "movzx eax, cx" => |a| a.movzx16_rr(RAX, RCX);
        "movsx rax, bl" => |a| a.movsx8_rr(W64, RAX, RBX);
        "movsx ecx, dil" => |a| a.movsx8_rr(W32, RCX, RDI);
        "movsx r12, r13w" => |a| a.movsx16_rr(W64, R12, R13);
        "movsxd rax, ecx" => |a| a.movsxd_rr(RAX, RCX);
        "movsxd r15, r8d" => |a| a.movsxd_rr(R15, R8);
    });
}

#[test]
fn immediate_moves_pick_the_shortest_form() {
    assert_encodings!(Assembler, disassembler::x86_64, {
        "mov eax, 0" => |a| a.mov_ri(RAX, 0);
        "mov r9d, 0x7f" => |a| a.mov_ri(R9, 0x7f);
        "mov eax, 0xffffffff" => |a| a.mov_ri(RAX, 0xffff_ffff);
        "mov rax, 0xffffffffffffffff" => |a| a.mov_ri(RAX, u64::MAX);
        "mov r12, 0xffffffff80000000" => |a| a.mov_ri(R12, 0xffff_ffff_8000_0000);
        "mov rax, 0x123456789abc" => |a| a.mov_ri(RAX, 0x1234_5678_9abc);
        "mov r15, 0x8000000000000000" => |a| a.mov_ri(R15, 1 << 63);
        "mov r11, 0x100000000" => |a| a.movabs(R11, 0x1_0000_0000);
        "mov ecx, 0x12345678" => |a| a.mov_ri32(RCX, 0x1234_5678);
        "mov rdx, 0xfffffffffffffffe" => |a| a.mov_ri_sign_extended(RDX, -2);
    });
    assert_eq!(bytes(|a| a.mov_ri(RAX, 1)).len(), 5);
    assert_eq!(bytes(|a| a.mov_ri(RAX, u64::MAX)).len(), 7);
    assert_eq!(bytes(|a| a.mov_ri(RAX, 1 << 32)).len(), 10);
}

#[test]
fn memory_operands() {
    assert_encodings!(Assembler, disassembler::x86_64, {
        "mov rax, qword ptr [rbx]" => |a| a.mov_rm(W64, RAX, &base(RBX, 0));
        "mov rax, qword ptr [rsp]" => |a| a.mov_rm(W64, RAX, &base(RSP, 0));
        "mov rax, qword ptr [r12]" => |a| a.mov_rm(W64, RAX, &base(R12, 0));
        "mov rax, qword ptr [rbp]" => |a| a.mov_rm(W64, RAX, &base(RBP, 0));
        "mov rax, qword ptr [r13]" => |a| a.mov_rm(W64, RAX, &base(R13, 0));
        "mov rax, qword ptr [rsp+8]" => |a| a.mov_rm(W64, RAX, &base(RSP, 8));
        "mov r9, qword ptr [r12-0x80]" => |a| a.mov_rm(W64, R9, &base(R12, -0x80));
        "mov rax, qword ptr [rbx+0x80]" => |a| a.mov_rm(W64, RAX, &base(RBX, 0x80));
        "mov rax, qword ptr [rbx-0x81]" => |a| a.mov_rm(W64, RAX, &base(RBX, -0x81));
        "mov eax, dword ptr [r13+0x7fffffff]" => |a| a.mov_rm(W32, RAX, &base(R13, i32::MAX));
        "mov rax, qword ptr [rax+rcx*4+0x10]" => |a| a.mov_rm(W64, RAX, &indexed(RAX, RCX, Scale::Four, 0x10));
        "mov rax, qword ptr [rax+rcx]" => |a| a.mov_rm(W64, RAX, &indexed(RAX, RCX, Scale::One, 0));
        "mov r13d, dword ptr [r13+r12*8+0x100]" => |a| a.mov_rm(W32, R13, &indexed(R13, R12, Scale::Eight, 0x100));
        "mov rax, qword ptr [r13+r12*8]" => |a| a.mov_rm(W64, RAX, &indexed(R13, R12, Scale::Eight, 0));
        "mov rax, qword ptr [rbp+r15*2-4]" => |a| a.mov_rm(W64, RAX, &indexed(RBP, R15, Scale::Two, -4));
        "mov rax, qword ptr [rsp+r9]" => |a| a.mov_rm(W64, RAX, &indexed(RSP, R9, Scale::One, 0));
        "mov rax, qword ptr [r12+r13*2]" => |a| a.mov_rm(W64, RAX, &indexed(R12, R13, Scale::Two, 0));
        "mov qword ptr [rdi+8], rsi" => |a| a.mov_mr(W64, &base(RDI, 8), RSI);
        "mov dword ptr [rdi], r9d" => |a| a.mov_mr(W32, &base(RDI, 0), R9);
        "mov qword ptr [rax], 0xffffffffffffffff" => |a| a.mov_mi(W64, &base(RAX, 0), -1);
        "mov dword ptr [rax+4], 7" => |a| a.mov_mi(W32, &base(RAX, 4), 7);
        "mov qword ptr [rsp+0x10], 0x12345678" => |a| a.mov_mi(W64, &base(RSP, 0x10), 0x1234_5678);
        "mov byte ptr [rax], sil" => |a| a.mov8_mr(&base(RAX, 0), RSI);
        "mov byte ptr [rax], bl" => |a| a.mov8_mr(&base(RAX, 0), RBX);
        "mov byte ptr [r8+1], r9b" => |a| a.mov8_mr(&base(R8, 1), R9);
        "mov word ptr [rax], cx" => |a| a.mov16_mr(&base(RAX, 0), RCX);
        "mov word ptr [r10], r11w" => |a| a.mov16_mr(&base(R10, 0), R11);
        "mov byte ptr [rax], 0x80" => |a| a.mov8_mi(&base(RAX, 0), 0x80);
        "mov word ptr [rax+2], 0x1234" => |a| a.mov16_mi(&base(RAX, 2), 0x1234);
        "movzx r10d, byte ptr [r11]" => |a| a.movzx8_rm(R10, &base(R11, 0));
        "movzx eax, word ptr [rcx+2]" => |a| a.movzx16_rm(RAX, &base(RCX, 2));
        "movsx eax, byte ptr [rcx]" => |a| a.movsx8_rm(W32, RAX, &base(RCX, 0));
        "movsx rax, byte ptr [rcx]" => |a| a.movsx8_rm(W64, RAX, &base(RCX, 0));
        "movsx rax, word ptr [rcx]" => |a| a.movsx16_rm(W64, RAX, &base(RCX, 0));
        "movsxd rax, dword ptr [rcx]" => |a| a.movsxd_rm(RAX, &base(RCX, 0));
        "lea rax, [rbx+rcx*8+0x10]" => |a| a.lea(W64, RAX, &indexed(RBX, RCX, Scale::Eight, 0x10));
        "lea eax, [rbx+5]" => |a| a.lea(W32, RAX, &base(RBX, 5));
        "lea r12, [rsp-8]" => |a| a.lea(W64, R12, &base(RSP, -8));
    });
    // [rbp] and [r13] need an explicit zero displacement.
    assert_eq!(bytes(|a| a.mov_rm(W64, RAX, &base(RBP, 0))), [0x48, 0x8b, 0x45, 0x00]);
    assert_eq!(bytes(|a| a.mov_rm(W64, RAX, &base(R13, 0))), [0x49, 0x8b, 0x45, 0x00]);
    // [rsp] and [r12] need a SIB byte.
    assert_eq!(bytes(|a| a.mov_rm(W64, RAX, &base(RSP, 0))), [0x48, 0x8b, 0x04, 0x24]);
    assert_eq!(bytes(|a| a.mov_rm(W64, RAX, &base(R12, 0))), [0x49, 0x8b, 0x04, 0x24]);
}

#[test]
#[should_panic(expected = "rsp cannot be an index register")]
fn rsp_cannot_be_an_index() {
    bytes(|a| a.mov_rm(W64, RAX, &indexed(RAX, RSP, Scale::One, 0)));
}

#[test]
fn arithmetic_and_logic() {
    assert_encodings!(Assembler, disassembler::x86_64, {
        "add rax, rbx" => |a| a.add_rr(W64, RAX, RBX);
        "sub r8d, r9d" => |a| a.sub_rr(W32, R8, R9);
        "and rcx, r15" => |a| a.and_rr(W64, RCX, R15);
        "or esi, edi" => |a| a.or_rr(W32, RSI, RDI);
        "xor r11, r11" => |a| a.xor_rr(W64, R11, R11);
        "cmp rax, rdx" => |a| a.cmp_rr(W64, RAX, RDX);
        "adc rax, rbx" => |a| a.alu_rr(AluOp::Adc, W64, RAX, RBX);
        "sbb eax, ebx" => |a| a.alu_rr(AluOp::Sbb, W32, RAX, RBX);
        "add rax, 1" => |a| a.add_ri(W64, RAX, 1);
        "add rax, 0x1000" => |a| a.add_ri(W64, RAX, 0x1000);
        "add rax, 0xffffffffffffffff" => |a| a.add_ri(W64, RAX, -1);
        "sub r15d, 0xffffff80" => |a| a.sub_ri(W32, R15, -128);
        "and eax, 0xff" => |a| a.and_ri(W32, RAX, 0xff);
        "or r9, 0x7f" => |a| a.or_ri(W64, R9, 0x7f);
        "xor esp, 0x80000000" => |a| a.xor_ri(W32, RSP, i32::MIN);
        "cmp r12, 0xffffffffffff8000" => |a| a.cmp_ri(W64, R12, -0x8000);
        "add rax, qword ptr [rbx]" => |a| a.alu_rm(AluOp::Add, W64, RAX, &base(RBX, 0));
        "add qword ptr [rbx], rax" => |a| a.alu_mr(AluOp::Add, W64, &base(RBX, 0), RAX);
        "sub dword ptr [rbx+8], 1" => |a| a.alu_mi(AluOp::Sub, W32, &base(RBX, 8), 1);
        "cmp dword ptr [rax], 0x12345678" => |a| a.cmp_mi(W32, &base(RAX, 0), 0x1234_5678);
        "cmp word ptr [r13+0xe], 0xfffc" => |a| a.cmp16_mi8(&base(R13, 14), -4);
        "cmp qword ptr [r12], r13" => |a| a.cmp_mr(W64, &base(R12, 0), R13);
        "test rax, rax" => |a| a.test_rr(W64, RAX, RAX);
        "test r8d, ecx" => |a| a.test_rr(W32, R8, RCX);
        "test eax, 0x100" => |a| a.test_ri(W32, RAX, 0x100);
        "test r14, 0x100" => |a| a.test_ri(W64, R14, 0x100);
        "test qword ptr [rax], 1" => |a| a.test_mi(W64, &base(RAX, 0), 1);
        "test byte ptr [rax+1], 0x80" => |a| a.test8_mi(&base(RAX, 1), 0x80);
        "neg rax" => |a| a.neg(W64, RAX);
        "not r9d" => |a| a.not(W32, R9);
        "shl rax, 1" => |a| a.shl_ri(W64, RAX, 1);
        "shl ecx, 5" => |a| a.shl_ri(W32, RCX, 5);
        "shr r12, 0x3f" => |a| a.shr_ri(W64, R12, 63);
        "sar r9d, 0x1f" => |a| a.sar_ri(W32, R9, 31);
        "rol rax, 3" => |a| a.shift_ri(ShiftOp::Rol, W64, RAX, 3);
        "ror eax, 1" => |a| a.shift_ri(ShiftOp::Ror, W32, RAX, 1);
        "shl rdx, cl" => |a| a.shift_rcl(ShiftOp::Shl, W64, RDX);
        "shr r10, cl" => |a| a.shift_rcl(ShiftOp::Shr, W64, R10);
        "sar edx, cl" => |a| a.shift_rcl(ShiftOp::Sar, W32, RDX);
        "imul eax, ecx" => |a| a.imul_rr(W32, RAX, RCX);
        "imul r8, r9" => |a| a.imul_rr(W64, R8, R9);
        "imul rax, rbx, 0xa" => |a| a.imul_rri(W64, RAX, RBX, 10);
        "imul eax, r13d, 0x3e8" => |a| a.imul_rri(W32, RAX, R13, 1000);
        "cdq" => |a| a.sign_extend_rax_into_rdx(W32);
        "cqo" => |a| a.sign_extend_rax_into_rdx(W64);
        "idiv ecx" => |a| a.idiv(W32, RCX);
        "idiv r11" => |a| a.idiv(W64, R11);
    });
    // Small immediates use the sign-extended imm8 form.
    assert_eq!(bytes(|a| a.add_ri(W64, RAX, 1)), [0x48, 0x83, 0xc0, 0x01]);
    assert_eq!(bytes(|a| a.add_ri(W64, RAX, 128)).len(), 7);
}

#[test]
fn condition_codes() {
    let conditions = [
        (Cond::Overflow, "o"),
        (Cond::NoOverflow, "no"),
        (Cond::Below, "b"),
        (Cond::AboveOrEqual, "ae"),
        (Cond::Equal, "e"),
        (Cond::NotEqual, "ne"),
        (Cond::BelowOrEqual, "be"),
        (Cond::Above, "a"),
        (Cond::Sign, "s"),
        (Cond::NotSign, "ns"),
        (Cond::Parity, "p"),
        (Cond::NotParity, "np"),
        (Cond::Less, "l"),
        (Cond::GreaterOrEqual, "ge"),
        (Cond::LessOrEqual, "le"),
        (Cond::Greater, "g"),
    ];
    for (cond, suffix) in conditions {
        let code = bytes(|a| {
            let label = a.new_label();
            a.bind(label);
            a.setcc(cond, RAX);
            a.cmovcc(cond, W64, RBX, R12);
            a.jcc(cond, label);
        });
        assert_eq!(
            disassembler::x86_64(&code),
            [
                format!("set{suffix} al"),
                format!("cmov{suffix} rbx, r12"),
                format!("j{suffix} short 0")
            ]
        );
        assert_eq!(cond.invert().invert(), cond);
        assert_ne!(cond.invert(), cond);
    }
    assert_encodings!(Assembler, disassembler::x86_64, {
        "sete sil" => |a| a.setcc(Cond::Equal, RSI);
        "setl r9b" => |a| a.setcc(Cond::Less, R9);
        "setg bl" => |a| a.setcc(Cond::Greater, RBX);
        "cmovl eax, r8d" => |a| a.cmovcc(Cond::Less, W32, RAX, R8);
    });
}

#[test]
fn branches_to_labels() {
    assert_encodings!(Assembler, disassembler::x86_64, {
        // Backward branches in rel8 range are short.
        "nop; je short 0" => |a| {
            let label = a.new_label();
            a.bind(label);
            a.nop();
            a.jcc(Cond::Equal, label);
        };
        "jmp short 0" => |a| {
            let label = a.new_label();
            a.bind(label);
            a.jmp(label);
        };
        // Forward branches are near unless requested short.
        "jne 6; nop" => |a| {
            let label = a.new_label();
            a.jcc(Cond::NotEqual, label);
            a.bind(label);
            a.nop();
        };
        "jmp 5" => |a| {
            let label = a.new_label();
            a.jmp(label);
            a.bind(label);
        };
        "jne short 2" => |a| {
            let label = a.new_label();
            a.jcc_short(Cond::NotEqual, label);
            a.bind(label);
        };
        "jmp short 2" => |a| {
            let label = a.new_label();
            a.jmp_short(label);
            a.bind(label);
        };
        "call 5" => |a| {
            let label = a.new_label();
            a.call(label);
            a.bind(label);
        };
        "lea rax, [7]" => |a| {
            let label = a.new_label();
            a.lea_label(RAX, label);
            a.bind(label);
        };
        "movsd xmm9, qword ptr [9]" => |a| {
            let label = a.new_label();
            a.movsd_r_label(XMM9, label);
            a.bind(label);
        };
        "movups xmm9, xmmword ptr [8]" => |a| {
            let label = a.new_label();
            a.movups_r_label(XMM9, label);
            a.bind(label);
        };
        "call rax" => |a| a.call_r(RAX);
        "call r11" => |a| a.call_r(R11);
        "call qword ptr [rax+8]" => |a| a.call_m(&base(RAX, 8));
        "jmp r12" => |a| a.jmp_r(R12);
        "jmp qword ptr [rsp+8]" => |a| a.jmp_m(&base(RSP, 8));
        "push rbx" => |a| a.push(RBX);
        "push r12" => |a| a.push(R12);
        "pop r15" => |a| a.pop(R15);
        "pop rbp" => |a| a.pop(RBP);
        "ret" => |a| a.ret();
        "int3" => |a| a.int3();
        "ud2" => |a| a.ud2();
    });

    // Backward branches just out of rel8 range are near.
    let code = bytes(|a| {
        let label = a.new_label();
        a.bind(label);
        a.nops(126);
        a.jmp(label);
        a.nops(1);
        a.jcc(Cond::Equal, label);
    });
    assert_eq!(&code[126..128], [0xeb, 0x80]);
    assert_eq!(&code[129..135], [0x0f, 0x84, 0x79, 0xff, 0xff, 0xff]);
}

#[test]
fn short_forward_branch_out_of_range_fails() {
    let mut assembler = Assembler::new();
    let label = assembler.new_label();
    assembler.jmp_short(label);
    assembler.nops(127);
    assembler.bind(label);
    assert!(assembler.finish().is_ok());

    let mut assembler = Assembler::new();
    let label = assembler.new_label();
    assembler.jcc_short(Cond::Equal, label);
    assembler.nops(128);
    assembler.bind(label);
    assert_eq!(assembler.finish(), Err(AsmError::BranchOutOfRange));
}

#[test]
fn unbound_label_fails() {
    let mut assembler = Assembler::new();
    let label = assembler.new_label();
    assembler.jmp(label);
    assert_eq!(assembler.finish(), Err(AsmError::UnboundLabel));
}

#[test]
fn nops_and_alignment() {
    for size in 0..=20 {
        let code = bytes(|a| a.nops(size));
        assert_eq!(code.len(), size);
        let lines = disassembler::x86_64(&code);
        // The recommended two-byte NOP is `66 90`, which decodes as xchg.
        assert!(
            lines
                .iter()
                .all(|line| line.starts_with("nop") || line == "xchg ax, ax"),
            "{lines:?}"
        );
    }
    let code = bytes(|a| {
        a.ret();
        a.align_code(16);
        a.int3();
        a.align_data(8);
    });
    assert_eq!(code.len(), 24);
    assert!(code[17..].iter().all(|byte| *byte == 0));
}

#[test]
fn data_emission() {
    let code = bytes(|a| {
        a.data_u8(1);
        a.data_u32(0x0302_0100);
        a.data_u64(0x0b0a_0908_0706_0504);
        a.data_bytes(&[0xc, 0xd]);
    });
    assert_eq!(code, [1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]);
}

#[test]
fn scalar_doubles() {
    assert_encodings!(Assembler, disassembler::x86_64, {
        "movsd xmm0, xmm1" => |a| a.movsd_rr(XMM0, XMM1);
        "movapd xmm8, xmm15" => |a| a.movapd_rr(XMM8, XMM15);
        "movsd xmm0, qword ptr [rax+8]" => |a| a.movsd_rm(XMM0, &base(RAX, 8));
        "movsd xmm11, qword ptr [r12+r13*8]" => |a| a.movsd_rm(XMM11, &indexed(R12, R13, Scale::Eight, 0));
        "movsd qword ptr [rsp], xmm9" => |a| a.movsd_mr(&base(RSP, 0), XMM9);
        "movups xmmword ptr [rdx+0x10], xmm15" => |a| a.movups_mr(&base(RDX, 16), XMM15);
        "movq xmm0, rax" => |a| a.movq_xr(XMM0, RAX);
        "movq xmm12, r13" => |a| a.movq_xr(XMM12, R13);
        "movq rax, xmm1" => |a| a.movq_rx(RAX, XMM1);
        "movq r10, xmm10" => |a| a.movq_rx(R10, XMM10);
        "addsd xmm0, xmm1" => |a| a.addsd(XMM0, XMM1);
        "subsd xmm2, xmm3" => |a| a.subsd(XMM2, XMM3);
        "mulsd xmm14, xmm4" => |a| a.mulsd(XMM14, XMM4);
        "divsd xmm9, xmm10" => |a| a.divsd(XMM9, XMM10);
        "sqrtsd xmm5, xmm13" => |a| a.sqrtsd(XMM5, XMM13);
        "roundsd xmm5, xmm13, 9" => |a| a.roundsd(XMM5, XMM13, 9);
        "roundsd xmm0, xmm1, 0xa" => |a| a.roundsd(XMM0, XMM1, 10);
        "ucomisd xmm0, xmm1" => |a| a.ucomisd(XMM0, XMM1);
        "ucomisd xmm15, xmm8" => |a| a.ucomisd(XMM15, XMM8);
        "cvtsi2sd xmm0, eax" => |a| a.cvtsi2sd(W32, XMM0, RAX);
        "cvtsi2sd xmm1, r8" => |a| a.cvtsi2sd(W64, XMM1, R8);
        "cvttsd2si eax, xmm0" => |a| a.cvttsd2si(W32, RAX, XMM0);
        "cvttsd2si r9, xmm15" => |a| a.cvttsd2si(W64, R9, XMM15);
        "xorpd xmm3, xmm3" => |a| a.xorpd(XMM3, XMM3);
        "andpd xmm6, xmm11" => |a| a.andpd(XMM6, XMM11);
        "movmskpd r11d, xmm7" => |a| a.movmskpd(R11, XMM7);
        "movss xmm0, dword ptr [rax+4]" => |a| a.movss_rm(XMM0, &base(RAX, 4));
        "movss xmm9, dword ptr [r12+r13*4]" => |a| a.movss_rm(XMM9, &indexed(R12, R13, Scale::Four, 0));
        "movss dword ptr [rsp], xmm15" => |a| a.movss_mr(&base(RSP, 0), XMM15);
        "cvtss2sd xmm1, xmm2" => |a| a.cvtss2sd(XMM1, XMM2);
        "cvtsd2ss xmm15, xmm8" => |a| a.cvtsd2ss(XMM15, XMM8);
    });
}

mod macro_assembler {
    use super::super::super::Condition;
    use super::super::super::DoubleCondition;
    use super::super::super::FprSet;
    use super::super::super::GprSet;
    use super::super::super::NegativeZero;
    use super::super::super::PortableMacroAssembler;
    use super::*;

    #[test]
    fn operations_are_legalized() {
        assert_encodings!(MacroAssembler, disassembler::x86_64, {
            "add rax, rbx" => |m| m.add64(RAX, RBX, RAX);
            "mov r11, rbx; sub r11, rax; mov rax, r11" => |m| m.sub64(RAX, RBX, RAX);
            "mov ecx, edx; sub ecx, esi" => |m| m.sub32(RCX, RDX, RSI);
            "lea rax, [rbx+8]" => |m| m.add64_imm(RAX, RBX, 8);
            "add rax, qword ptr [8]" => |m| m.add64_imm(RAX, RAX, 1 << 40);
            "mov eax, ebx" => |m| m.and64_imm(RAX, RBX, 0xffff_ffff);
            "and rax, qword ptr [8]" => |m| m.and64_imm(RAX, RAX, 0xffff_0000_0000_0000);
            "mov rax, rbx; or rax, qword ptr [0x10]" => |m| m.or64_imm(RAX, RBX, 0xffff_0000_0000_0000);
            "mov rax, rbx; sub rax, qword ptr [0x10]" => |m| m.sub64_imm(RAX, RBX, 1 << 40);
            "mov r11, rbx; mov r10, rcx; mov rcx, rdx; shl r11, cl; mov rcx, r10; mov rax, r11" => |m| m.shl64(RAX, RBX, RDX);
            "mov eax, ebx; shl eax, cl" => |m| m.shl32(RAX, RBX, RCX);
            "mov eax, eax" => |m| m.shl32_imm(RAX, RAX, 32);
            "cmp rbx, rcx; setl al; movzx eax, al" => |m| m.compare64_set(Condition::LessThan, RAX, RBX, RCX);
            "cmp rdi, rsi; mov rax, rcx; cmovl rax, rdx" => |m| m.select64(Condition::LessThan, RDI, RSI, RAX, RDX, RCX);
            "cmp rdi, rsi; cmovge rdx, rcx" => |m| m.select64(Condition::LessThan, RDI, RSI, RDX, RDX, RCX);
            "call qword ptr [8]" => |m| m.call_absolute(0x1234_5678_9abc);
            "mov r11, qword ptr [0x10]; jmp r11" => |m| m.jump_absolute(0x1234_5678_9abc);
            "mov r11, qword ptr [0x10]; cmp qword ptr [rdi+8], r11; je short 0" => |m| {
                let label = m.new_label();
                m.bind(label);
                m.branch64_memory_imm(Condition::Equal, &base(RDI, 8), 1 << 40, label);
            };
            "test rax, rax; je short 0" => |m| {
                let label = m.new_label();
                m.bind(label);
                m.branch64_imm(Condition::Equal, RAX, 0, label);
            };
            "ucomisd xmm0, xmm1; jp 0xc; je short 0" => |m| {
                let label = m.new_label();
                m.bind(label);
                m.branch_double(DoubleCondition::Equal, XMM0, XMM1, label);
            };
            "ucomisd xmm1, xmm0; ja short 0" => |m| {
                let label = m.new_label();
                m.bind(label);
                m.branch_double(DoubleCondition::LessThan, XMM0, XMM1, label);
            };
            "ucomisd xmm0, xmm1; setne al; setp r11b; or eax, r11d; movzx eax, al" => |m| {
                m.compare_double_set(DoubleCondition::NotEqualOrUnordered, RAX, XMM0, XMM1);
            };
            "movapd xmm15, xmm1; subsd xmm15, xmm0; movapd xmm0, xmm15" => |m| m.sub_double(XMM0, XMM1, XMM0);
            "movsd xmm15, qword ptr [0x18]; movapd xmm2, xmm3; xorpd xmm2, xmm15" => |m| m.neg_double(XMM2, XMM3);
            "cvttsd2si eax, xmm0; xorpd xmm15, xmm15; cvtsi2sd xmm15, eax; ucomisd xmm15, xmm0; jne short 0; jp short 0; test eax, eax; jne 0x2d; movmskpd r11d, xmm0; test r11d, 1; jne short 0" => |m| {
                let fail = m.new_label();
                m.bind(fail);
                m.branch_convert_double_to_int32(RAX, XMM0, fail, NegativeZero::Fail);
            };
        });
    }

    #[test]
    fn frames() {
        let frame = MacroAssembler::frame(GprSet::of(&[RBX, R12, RBP, RSP]), FprSet::of(&[XMM8]), 20);
        assert_eq!(frame.saved_gprs, GprSet::of(&[RBX, R12]));
        assert_eq!(frame.locals_size, 24);
        assert_eq!(frame.caller_stack_offset, 64);
        assert_encodings!(MacroAssembler, disassembler::x86_64, {
            "push rbp; mov rbp, rsp; push rbx; push r12; sub rsp, 0x20; movsd qword ptr [rsp+0x18], xmm8; movsd xmm8, qword ptr [rsp+0x18]; add rsp, 0x20; pop r12; pop rbx; pop rbp" => |m| {
                m.emit_prologue(&frame);
                m.emit_epilogue(&frame);
            };
            "push rbp; mov rbp, rsp; pop rbp" => |m| {
                let frame = MacroAssembler::frame(GprSet::EMPTY, FprSet::EMPTY, 0);
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
        assert!(M::CALLEE_SAVED_GPRS.intersection(M::CALLER_SAVED_GPRS).is_empty());
        assert!(M::SCRATCH_GPRS.difference(M::CALLER_SAVED_GPRS).is_empty());
        assert_eq!(M::ALLOCATABLE_GPRS.len(), 12);
        assert_eq!(M::ALLOCATABLE_FPRS.len(), 15);
    }
}
