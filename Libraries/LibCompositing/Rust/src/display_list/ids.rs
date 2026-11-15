/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The ids display lists go by, which the display lists that nest others name them with.

unsafe extern "C" {
    fn ladybird_gfx_process_next_display_list_id() -> u64;
}

/// A new display list id, unique in the process.
pub fn allocate_display_list_id() -> u64 {
    // The counter is LibGfx's: this crate is compiled into more than one library, and every copy names display lists
    // from the same counter. See `LibGfx/RustProcessState.cpp`.
    // SAFETY: The counter is an atomic on the other side of the boundary.
    unsafe { ladybird_gfx_process_next_display_list_id() }
}
