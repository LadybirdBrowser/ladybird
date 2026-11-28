/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The first handler of a legacy `try` must not use values from the tag tests of later handlers, which do not
//! dominate it: a `throw` there of a later tested tag gave the exception a stale tag identity. A process of its own,
//! since the JIT's options are the process's.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

use libwasm_engine::runtime::{ExternValue, Store, Value};
use std::sync::Arc;

#[test]
fn first_legacy_handler_throws_a_later_tested_tag() {
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "forced,verify-ir") };
    let path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../Engine/tests/fixtures/legacy_catch_throws.wasm");
    let module = Arc::new(libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap());
    let mut store = Store::new();
    let instance = store.instantiate(module, &[]).unwrap();
    let Some(ExternValue::Function(main)) = store.instance(instance).export("main") else {
        panic!("no main export");
    };
    assert_eq!(store.invoke(main, &[]), Ok(vec![Value::I32(42)]));
}
