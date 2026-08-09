/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared utilities for C++/Rust FFI data.

pub(crate) fn ascii_lowercase(code_unit: u16) -> u16 {
    if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
        code_unit + u16::from(b'a' - b'A')
    } else {
        code_unit
    }
}
