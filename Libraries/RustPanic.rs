/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(not(test))]
unsafe extern "C" {
    fn ladybird_rust_panic(
        message: *const u8,
        message_length: usize,
        filename: *const u8,
        filename_length: usize,
        line: u32,
        column: u32,
        is_panicking: extern "C" fn() -> bool,
    );
    fn ladybird_rust_panic_will_abort();
}

#[cfg(not(test))]
extern "C" fn is_panicking() -> bool {
    // This reads Rust's thread-local panic counter without allocating.
    std::thread::panicking()
}

#[cfg(not(test))]
#[unsafe(export_name = concat!("ladybird_init_rust_panic_", env!("CARGO_PKG_NAME")))]
extern "C" fn initialize() {
    static INITIALIZE: std::sync::Once = std::sync::Once::new();
    INITIALIZE.call_once(|| {
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            let message = info
                .payload()
                .downcast_ref::<&str>()
                .copied()
                .or_else(|| info.payload().downcast_ref::<String>().map(String::as_str))
                .unwrap_or("non-string panic payload");
            let (filename, line, column) = info.location().map_or(("", 0, 0), |location| {
                (location.file(), location.line(), location.column())
            });
            // SAFETY: The bridge copies these borrowed strings before returning.
            unsafe {
                ladybird_rust_panic(
                    message.as_ptr(),
                    message.len(),
                    filename.as_ptr(),
                    filename.len(),
                    line,
                    column,
                    is_panicking,
                );
            }
            previous(info);
        }));
    });
}

#[allow(dead_code)]
pub fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(_) => {
            // Unwinding has ended, so explicitly mark this panic as fatal.
            #[cfg(not(test))]
            unsafe {
                ladybird_rust_panic_will_abort();
            }
            std::process::abort();
        }
    }
}
