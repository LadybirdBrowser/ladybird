/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Standalone Rust tests run without the C++ runtime.
#![cfg(not(test))]

use std::ffi::{c_char, c_void};
use std::fmt::Write;

/// cbindgen:ignore
mod ffi {
    use std::ffi::{c_char, c_void};

    pub(super) type Append = extern "C" fn(context: *mut c_void, characters: *const c_char, length: usize);

    unsafe extern "C" {
        pub(super) fn ladybird_set_rust_symbol_demangler(
            demangler: extern "C" fn(*const c_char, usize, Append, *mut c_void) -> bool,
        );
    }
}

use ffi::{Append, ladybird_set_rust_symbol_demangler};

struct Output {
    append: Append,
    context: *mut c_void,
}

impl Write for Output {
    fn write_str(&mut self, text: &str) -> std::fmt::Result {
        (self.append)(self.context, text.as_ptr().cast(), text.len());
        Ok(())
    }
}

extern "C" fn demangle(name: *const c_char, name_length: usize, append: Append, context: *mut c_void) -> bool {
    // SAFETY: AK passes a name of the given length that outlives this call.
    let name = unsafe { std::slice::from_raw_parts(name.cast::<u8>(), name_length) };
    let Ok(name) = std::str::from_utf8(name) else {
        return false;
    };
    let Ok(demangled) = rustc_demangle::try_demangle(name) else {
        return false;
    };

    // The alternate format leaves out crate hashes, like Rust's own backtraces do.
    let mut output = Output { append, context };
    write!(output, "{demangled:#}").is_ok()
}

#[unsafe(export_name = concat!("ladybird_init_rust_demangle_", env!("CARGO_PKG_NAME")))]
extern "C" fn initialize() {
    // SAFETY: AK only stores the function pointer.
    unsafe { ladybird_set_rust_symbol_demangler(demangle) };
}
