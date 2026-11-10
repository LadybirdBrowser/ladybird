/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LibURL's C++ is not linked into Rust unit tests.

use std::ffi::c_void;

#[unsafe(no_mangle)]
extern "C" fn url_rust_blob_url_entry_unref(_entry: *const c_void) {
    unreachable!("no unit test has a blob URL entry");
}
