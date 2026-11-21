/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://infra.spec.whatwg.org/#ascii-whitespace
pub const fn is_ascii_whitespace(code_point: u32) -> bool {
    // ASCII whitespace is U+0009 TAB, U+000A LF, U+000C FF, U+000D CR, or U+0020 SPACE.
    matches!(code_point, 0x09 | 0x0A | 0x0C | 0x0D | 0x20)
}
