/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! OSR compiles of particular shapes of code, with the IR and register allocation verified.

use libwasm_jit::decode::{Decoder, Instruction};
use libwasm_jit::{CompileContext, CompileFailure, CompileRequest, Options, RuntimeInfo};
use std::sync::Arc;

/// The OSR compile of the first defined function of `bytes` at its first loop.
fn compile_at_first_loop(bytes: &[u8]) -> Result<(), CompileFailure> {
    let module = Arc::new(libwasm_engine::compile(bytes).expect("a valid module"));
    let body = &module.code[0];
    let mut decoder = Decoder::new(&module.bytes, body.code.start as usize, body.code.end as usize);
    let pc = loop {
        let pc = decoder.position() as u32;
        if let Instruction::Loop(_) = decoder.decode() {
            break pc;
        }
    };
    let request = CompileRequest {
        module: module.clone(),
        function: module.imported_function_count,
        runtime: RuntimeInfo::default(),
        options: Options {
            verify_ir: true,
            ..Options::default()
        },
        osr: Some(pc),
    };
    libwasm_jit::compile(&mut CompileContext::default(), &request).map(|_| ())
}

#[test]
fn loop_in_a_block_with_a_value_below_it() {
    // (func (param i32) (result i32)
    //   i32.const 7
    //   block
    //     local.get 0
    //     br_if 0                      ;; past the loop, with the 7 below the block
    //     loop
    //       local.get 0 i32.const 1 i32.sub local.tee 0
    //       br_if 0
    //     end
    //   end
    //   local.get 0
    //   i32.add)
    //
    // The 7 reaches the block's end both from the function's start and from the loop, which an OSR compile enters
    // with the frame's value of it. The merge has no phi for values below the block's operand stack, so the loop must
    // not be an OSR entry, or the IR must merge the two values: it used to build IR where a use was not dominated by
    // its definition, which crashed transformersjs in the browser.
    #[rustfmt::skip]
    let bytes = [
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x1c, 0x01, 0x1a, 0x00,
        0x41, 0x07,
        0x02, 0x40,
        0x20, 0x00, 0x0d, 0x00,
        0x03, 0x40,
        0x20, 0x00, 0x41, 0x01, 0x6b, 0x22, 0x00, 0x0d, 0x00,
        0x0b,
        0x0b,
        0x20, 0x00, 0x6a,
        0x0b,
    ];
    assert!(matches!(
        compile_at_first_loop(&bytes),
        Ok(()) | Err(CompileFailure::NoOsrEntry { .. })
    ));
}

#[test]
fn loop_with_a_value_below_it_at_the_function_level() {
    // (func (param i32) (result i32)
    //   i32.const 7
    //   loop
    //     local.get 0 i32.const 1 i32.sub local.tee 0
    //     br_if 0
    //   end
    //   local.get 0
    //   i32.add)
    //
    // The 7 is below no construct but the function, so the loop can be an OSR entry.
    #[rustfmt::skip]
    let bytes = [
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x15, 0x01, 0x13, 0x00,
        0x41, 0x07,
        0x03, 0x40,
        0x20, 0x00, 0x41, 0x01, 0x6b, 0x22, 0x00, 0x0d, 0x00,
        0x0b,
        0x20, 0x00, 0x6a,
        0x0b,
    ];
    assert_eq!(compile_at_first_loop(&bytes), Ok(()));
}
