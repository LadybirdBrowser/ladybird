/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub const fn is_ascii_digit(code_point: u32) -> bool {
    matches!(char::from_u32(code_point), Some('0'..='9'))
}
