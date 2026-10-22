/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! `memory.atomic.wait` in compiled code traps in an agent that cannot block. A process of its own, since the JIT's
//! options are the process's.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

use libwasm_engine::runtime::{ExternValue, Store, Trap, Value};
use std::sync::Arc;

#[test]
fn wait_traps_compiled_in_an_agent_that_cannot_block() {
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "forced") };
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../Engine/tests/fixtures/shared_waits.wasm");
    let module = Arc::new(libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap());
    for can_block in [true, false] {
        let mut store = Store::new();
        store.can_block = can_block;
        let instance = store.instantiate(module.clone(), &[]).unwrap();
        let Some(ExternValue::Function(wait)) = store.instance(instance).export("wait") else {
            panic!("no wait export");
        };
        // The location holds 0, so a wait for 1 returns at once where the agent can block.
        let result = store.invoke(wait, &[Value::I32(0), Value::I32(1), Value::I64(0)]);
        if can_block {
            assert_eq!(result, Ok(vec![Value::I32(1)]));
        } else {
            assert!(matches!(result, Err(Trap::Host(_))), "{result:?}");
        }
    }
}
