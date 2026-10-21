/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libwasm_engine::validator::{CatchKind, SideTableEntry};
use libwasm_engine::{Error, Module, compile};

fn leb(mut value: u32, out: &mut Vec<u8>) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            out.push(byte);
            return;
        }
        out.push(byte | 0x80);
    }
}

fn section(id: u8, contents: &[u8], out: &mut Vec<u8>) {
    out.push(id);
    leb(contents.len() as u32, out);
    out.extend_from_slice(contents);
}

/// A module with one function of type `params -> results` (value type bytes), with no declared locals and the given
/// instructions, which must end with the function's `end`. Tag 0 has type [i32] -> [].
fn module(params: &[u8], results: &[u8], code: &[u8]) -> Vec<u8> {
    let mut bytes = b"\0asm\x01\0\0\0".to_vec();
    let mut types = vec![2, 0x60];
    leb(params.len() as u32, &mut types);
    types.extend_from_slice(params);
    leb(results.len() as u32, &mut types);
    types.extend_from_slice(results);
    types.extend_from_slice(&[0x60, 1, 0x7f, 0]);
    section(1, &types, &mut bytes);
    section(3, &[1, 0], &mut bytes);
    section(13, &[1, 0, 1], &mut bytes);
    let mut body = vec![0];
    body.extend_from_slice(code);
    let mut function = Vec::new();
    leb(body.len() as u32, &mut function);
    function.extend_from_slice(&body);
    let mut code_section = vec![1];
    code_section.extend_from_slice(&function);
    section(10, &code_section, &mut bytes);
    bytes
}

fn compile_function(params: &[u8], results: &[u8], code: &[u8]) -> (Module, u32) {
    let bytes = module(params, results, code);
    let module = compile(&bytes).unwrap_or_else(|error| panic!("{error}"));
    let start = module.code[0].code.start;
    (module, start)
}

fn entries(module: &Module) -> &[SideTableEntry] {
    &module.function_infos[0].side_table
}

const I32: u8 = 0x7f;

#[test]
fn branches_out_of_blocks_past_their_end() {
    // block (result i32) i32.const 1 i32.const 2 br 0 end drop end
    let code = [0x02, I32, 0x41, 1, 0x41, 2, 0x0c, 0, 0x0b, 0x1a, 0x0b];
    let (module, start) = compile_function(&[], &[], &code);
    let branch = 6;
    let after_end = 9;
    assert_eq!(
        entries(&module),
        [SideTableEntry {
            pc_delta: after_end - branch,
            stp_delta: 1,
            keep: 1,
            drop: 1,
        }]
    );
    assert_eq!(module.function_infos[0].max_stack_slots, 2);
    assert_eq!(module.bytes[(start as usize) + after_end as usize], 0x1a);
}

#[test]
fn branches_back_to_the_loop_instruction() {
    // loop local.get 0 br_if 0 end end, with param i32
    let code = [0x03, 0x40, 0x20, 0, 0x0d, 0, 0x0b, 0x0b];
    let (module, _) = compile_function(&[I32], &[], &code);
    assert_eq!(
        entries(&module),
        [SideTableEntry {
            pc_delta: -4,
            stp_delta: 0,
            keep: 0,
            drop: 0,
        }]
    );
}

#[test]
fn if_jumps_to_its_else_arm_and_the_first_arm_jumps_past_the_end() {
    // local.get 0 if (result i32) i32.const 1 else i32.const 2 end drop end
    let code = [0x20, 0, 0x04, I32, 0x41, 1, 0x05, 0x41, 2, 0x0b, 0x1a, 0x0b];
    let (module, _) = compile_function(&[I32], &[], &code);
    let if_pc = 2;
    let else_pc = 6;
    let end_pc = 9;
    assert_eq!(
        entries(&module),
        [
            SideTableEntry {
                pc_delta: else_pc + 1 - if_pc,
                stp_delta: 2,
                keep: 0,
                drop: 0,
            },
            SideTableEntry {
                pc_delta: end_pc + 1 - else_pc,
                stp_delta: 1,
                keep: 1,
                drop: 0,
            },
        ]
    );
}

#[test]
fn if_without_else_jumps_past_its_end() {
    // local.get 0 if nop end end
    let code = [0x20, 0, 0x04, 0x40, 0x01, 0x0b, 0x0b];
    let (module, _) = compile_function(&[I32], &[], &code);
    assert_eq!(entries(&module)[0].pc_delta, 6 - 2);
    assert_eq!(entries(&module)[0].stp_delta, 1);
}

#[test]
fn br_table_has_an_entry_per_label_with_the_default_last() {
    // block block local.get 0 br_table 0 1 0 end end end
    let code = [0x02, 0x40, 0x02, 0x40, 0x20, 0, 0x0e, 2, 0, 1, 0, 0x0b, 0x0b, 0x0b];
    let (module, _) = compile_function(&[I32], &[], &code);
    let branch = 6;
    let inner_end = 11;
    let outer_end = 12;
    let table = entries(&module);
    assert_eq!(table.len(), 3);
    assert_eq!(table[0].pc_delta, inner_end + 1 - branch);
    assert_eq!(table[1].pc_delta, outer_end + 1 - branch);
    assert_eq!(table[2].pc_delta, inner_end + 1 - branch);
    for (index, entry) in table.iter().enumerate() {
        assert_eq!(entry.stp_delta, 3 - index as i32);
    }
}

#[test]
fn branches_to_the_function_label_target_its_final_end() {
    // i32.const 7 br 0 end, returning i32
    let code = [0x41, 7, 0x0c, 0, 0x0b];
    let (module, _) = compile_function(&[], &[I32], &code);
    assert_eq!(
        entries(&module),
        [SideTableEntry {
            pc_delta: 2,
            stp_delta: 1,
            keep: 1,
            drop: 0,
        }]
    );
}

#[test]
fn counts_vector_values_as_two_slots() {
    // v128.const 0 v128.const 0 drop drop end
    let mut code = vec![0xfd, 0x0c];
    code.extend_from_slice(&[0; 16]);
    code.extend_from_slice(&[0xfd, 0x0c]);
    code.extend_from_slice(&[0; 16]);
    code.extend_from_slice(&[0x1a, 0x1a, 0x0b]);
    let (module, _) = compile_function(&[], &[], &code);
    assert_eq!(module.function_infos[0].max_stack_slots, 4);
}

#[test]
fn records_try_table_handlers() {
    // block (result i32) try_table (catch 0 0) i32.const 1 throw 0 end unreachable end drop end
    let code = [
        0x02, I32, 0x1f, 0x40, 1, 0x00, 0, 0, 0x41, 1, 0x08, 0, 0x0b, 0x00, 0x0b, 0x1a, 0x0b,
    ];
    let (module, start) = compile_function(&[], &[], &code);
    let info = &module.function_infos[0];
    assert_eq!(info.try_regions.len(), 1);
    let region = info.try_regions[0];
    assert_eq!(region.start - start, 8);
    assert_eq!(region.end - start, 12);
    assert_eq!(region.clause_count, 1);
    let clause = info.catch_clauses[0];
    assert_eq!(clause.kind, CatchKind::Catch);
    assert_eq!(clause.tag, 0);
    assert_eq!(clause.target_pc - start, 15);
    assert_eq!(clause.height, 0);
}

#[test]
fn rejects_malformed_and_invalid_functions() {
    // i32.add on an empty stack
    let error = compile(&module(&[], &[], &[0x6a, 0x0b])).unwrap_err();
    assert_eq!(error, Error::invalid("type mismatch"));
    // a missing end
    let error = compile(&module(&[], &[], &[0x01])).unwrap_err();
    assert!(error.is_malformed(), "{error}");
    // br to a label that does not exist
    let error = compile(&module(&[], &[], &[0x0c, 1, 0x0b])).unwrap_err();
    assert_eq!(error, Error::invalid("unknown label"));
}
