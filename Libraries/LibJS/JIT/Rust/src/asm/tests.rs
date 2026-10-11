/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

fn check_constant_pools<M: PortableMacroAssembler>() {
    let constants: Vec<u64> = (0..1024).map(|index| 0x1234_5678_9abc_0000 | index).collect();
    let vector_constant = |bits: u64| u128::from(bits) | (u128::from(!bits) << 64);
    let assemble = || {
        let mut masm = M::new();
        for bits in constants.iter().chain(constants.iter().rev()) {
            masm.call_absolute(*bits);
            masm.load_imm128(M::ARGUMENT_FPRS[0], vector_constant(*bits));
        }
        masm.finish_with_data_offset().unwrap()
    };
    let (code, data_offset) = assemble();
    let mut expected = vec![0; data_offset.next_multiple_of(16) - data_offset];
    for bits in &constants {
        expected.extend_from_slice(&vector_constant(*bits).to_le_bytes());
    }
    for bits in &constants {
        expected.extend_from_slice(&bits.to_le_bytes());
    }
    assert_eq!(
        code[data_offset..],
        expected,
        "constants are emitted once, in insertion order"
    );
    assert_eq!(
        (code, data_offset),
        assemble(),
        "independent compilations emit identical code"
    );
}

#[test]
fn aarch64_constant_pools_deduplicate_in_insertion_order() {
    check_constant_pools::<aarch64::MacroAssembler>();
}

#[test]
fn x86_64_constant_pools_deduplicate_in_insertion_order() {
    check_constant_pools::<x86_64::MacroAssembler>();
}
