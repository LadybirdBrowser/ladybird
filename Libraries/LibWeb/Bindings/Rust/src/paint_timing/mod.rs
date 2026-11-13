/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Paint Timing, https://w3c.github.io/paint-timing/

#[allow(
    clippy::module_inception,
    reason = "the module mirrors PaintTiming/PaintTiming.cpp, which the crate module is named after"
)]
pub(crate) mod paint_timing;
pub(crate) mod paint_timing_mixin;
pub(crate) mod performance_paint_timing;
