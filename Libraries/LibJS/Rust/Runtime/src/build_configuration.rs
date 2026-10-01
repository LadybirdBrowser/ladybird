/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Values that depend on how LibGC and the interpreter were built, as the build script determined them.

const fn parse_decimal(text: &str) -> u64 {
    let bytes = text.as_bytes();
    let mut value = 0u64;
    let mut index = 0;
    while index < bytes.len() {
        assert!(bytes[index].is_ascii_digit());
        value = value * 10 + (bytes[index] - b'0') as u64;
        index += 1;
    }
    value
}

pub const HEAP_REGION_OFFSET_MASK: u64 = parse_decimal(env!("LIBJS_RUNTIME_HEAP_REGION_OFFSET_MASK"));
pub const PRIMITIVE_STORAGE_CAGE_OFFSET_MASK: u64 =
    parse_decimal(env!("LIBJS_RUNTIME_PRIMITIVE_STORAGE_CAGE_OFFSET_MASK"));
/// How much native stack must stay free for the interpreter to keep running JavaScript.
pub const VM_STACK_SPACE_LIMIT: u64 = parse_decimal(env!("LIBJS_RUNTIME_VM_STACK_SPACE_LIMIT"));
