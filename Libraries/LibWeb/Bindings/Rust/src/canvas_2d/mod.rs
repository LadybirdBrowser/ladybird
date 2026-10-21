/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebContent half of the 2D canvas: the state that the two kinds of 2D context share, which records the commands
//! of a canvas for the Compositor. The recorder and the records are those of the display list tape
//! (Libraries/LibCompositing/Rust/src/display_list/canvas_2d.rs); the attributes and operations of the contexts are
//! in the modules of their IDL files in crate::html. What is here reaches LibWeb, which the tests of the crate do not
//! link.

#[cfg(not(test))]
pub(crate) mod canvas;
#[cfg(not(test))]
pub(crate) mod context;
#[cfg(not(test))]
pub(crate) mod drawing_state;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod host;
#[cfg(not(test))]
pub(crate) mod path;
#[cfg(not(test))]
pub(crate) mod text;

/// A number of an argument in a float, as the C++ contexts took most of their numbers.
#[cfg(not(test))]
#[allow(
    clippy::cast_possible_truncation,
    reason = "these operations take floats, as the C++ did"
)]
pub(crate) fn float(number: f64) -> f32 {
    number as f32
}
