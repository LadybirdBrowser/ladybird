/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_paint_timing_is_exposed_for_paint_timing(node: *mut c_void) -> bool;
}

/// https://w3c.github.io/paint-timing/#exposed-for-paint-timing
pub(crate) fn is_exposed_for_paint_timing(node: &CppCell) -> bool {
    // NB: Given a null document. Only elements can be exposed for paint timing.
    // SAFETY: The node is live.
    unsafe { web_paint_timing_is_exposed_for_paint_timing(node.as_ptr()) }
}
