/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stand-ins for the LibGfx process-wide counters, which the unit tests link without the C++ runtime.

use std::sync::atomic::{AtomicU64, Ordering};

// One counter stands in for each of LibGfx's process-wide counters: their values only need to be unique.
static NEXT: AtomicU64 = AtomicU64::new(1);

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_process_next_path_identity() -> u64 {
    NEXT.fetch_add(1, Ordering::Relaxed)
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_process_next_structural_epoch() -> u64 {
    NEXT.fetch_add(1, Ordering::Relaxed)
}
