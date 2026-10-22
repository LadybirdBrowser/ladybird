/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libwasm_engine::compile;

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

/// A module with type 0 `(array (mut i32))` and one function of type `[] -> [(ref 0)]` with the given code, which
/// must end with the function's `end`.
fn array_module(code: &[u8]) -> Vec<u8> {
    let mut bytes = b"\0asm\x01\0\0\0".to_vec();
    section(1, &[2, 0x5e, 0x7f, 0x01, 0x60, 0, 1, 0x64, 0], &mut bytes);
    section(3, &[1, 1], &mut bytes);
    let mut body = vec![0];
    body.extend_from_slice(code);
    let mut contents = vec![1];
    leb(body.len() as u32, &mut contents);
    contents.extend_from_slice(&body);
    section(10, &contents, &mut bytes);
    bytes
}

fn array_new_fixed(count: u32) -> Vec<u8> {
    let mut code = vec![0xfb, 0x08, 0];
    leb(count, &mut code);
    code
}

#[test]
fn array_new_fixed_in_unreachable_code_takes_any_count_at_once() {
    // Each of the 2^32 - 1 operands would be a pop of the polymorphic stack, which took seconds.
    let mut code = vec![0x00];
    code.extend(array_new_fixed(u32::MAX));
    code.push(0x0b);
    let start = std::time::Instant::now();
    assert!(compile(&array_module(&code)).is_ok());
    assert!(start.elapsed() < std::time::Duration::from_secs(1));

    // The operands that are on the stack are still checked.
    let mut code = vec![0x00, 0x42, 0x00];
    code.extend(array_new_fixed(u32::MAX));
    code.push(0x0b);
    assert!(compile(&array_module(&code)).is_err());
}

#[test]
fn array_new_fixed_needs_its_operands_in_reachable_code() {
    let mut code = vec![0x41, 0x01, 0x41, 0x02];
    code.extend(array_new_fixed(2));
    code.push(0x0b);
    assert!(compile(&array_module(&code)).is_ok());

    let mut code = vec![0x41, 0x01];
    code.extend(array_new_fixed(2));
    code.push(0x0b);
    assert!(compile(&array_module(&code)).is_err());
}
