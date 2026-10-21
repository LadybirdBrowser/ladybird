/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The objects that the 2D canvas contexts create and draw with. The contexts are still LibWeb's C++, which creates
//! and reads these objects through the C ABI of `exports`.

pub(crate) mod color_stops;

// What follows runs on the JS runtime, which the tests of the crate do not link.
#[cfg(not(test))]
pub(crate) mod canvas_gradient;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod text_metrics;
