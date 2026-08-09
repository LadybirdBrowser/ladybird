/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub(super) fn fnv1a64<T: Into<u64>>(values: impl IntoIterator<Item = T>) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325;
    for value in values {
        hash ^= value.into();
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}
