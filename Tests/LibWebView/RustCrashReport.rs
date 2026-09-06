/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[path = "../../Libraries/RustPanic.rs"]
mod rust_panic;

#[unsafe(no_mangle)]
pub extern "C" fn test_rust_panic(mode: u8) {
    match mode {
        0 => rust_panic::abort_on_panic(|| panic!("expected Rust panic")),
        1 => {
            let _ = std::panic::catch_unwind(|| panic!("recovered Rust panic"));
        }
        2 => rust_panic::abort_on_panic(|| panic!("{}", "x".repeat(10000))),
        3 => {
            let _ = std::thread::spawn(|| panic!("worker Rust panic")).join();
        }
        4 => {
            let _ = std::panic::catch_unwind(|| panic!("recovered Rust panic"));
            rust_panic::abort_on_panic(|| panic!("expected Rust panic"));
        }
        5 => {
            let _ = std::thread::spawn(|| rust_panic::abort_on_panic(|| panic!("expected Rust panic"))).join();
        }
        6 => rust_panic::abort_on_panic(|| std::panic::panic_any(42u32)),
        _ => unreachable!(),
    }
}
