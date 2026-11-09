/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A pattern can nest groups and classes deeper than the native stack holds, so each pass that recurses into them
//! checks this first.

/// The stack left below the limit for what a pass does between two checks.
const HEADROOM: usize = 64 * 1024;

/// Whether recursing into one more nested group or class could run this thread out of stack.
pub(crate) fn is_exhausted() -> bool {
    let Some(lowest_address) = ak::stack::lowest_address_of_this_thread() else {
        return false;
    };
    let marker = 0u8;
    (&raw const marker as usize) < lowest_address.saturating_add(HEADROOM)
}
