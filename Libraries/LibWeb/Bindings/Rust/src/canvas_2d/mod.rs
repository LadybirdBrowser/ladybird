/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebContent half of the 2D canvas, which records the commands of a canvas for the Compositor. The recorder and
//! the records are those of the display list tape (Libraries/LibCompositing/Rust/src/display_list/canvas_2d.rs); what
//! is here reaches LibWeb, which the tests of the crate do not link.

#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod host;
