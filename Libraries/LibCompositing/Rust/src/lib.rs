/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

pub mod animation_timing;
pub mod css_pixels;
pub mod display_list;
pub mod easing;
pub mod fast_hash;
pub mod ffi;
pub mod filter_bytes;
pub mod force_dark;
pub mod host;
pub mod ipc_types;
pub mod node_slot_id;
pub mod overlays;
pub mod scrolling;
#[cfg(test)]
mod test_stubs;
pub mod time;
pub mod visual_animation;
pub mod visual_context;
pub mod webgl;

// The model of the WebGL functions, which the build scripts of the crates that generate from it include, and whose tests
// run with those of this crate.
#[cfg(test)]
#[allow(
    dead_code,
    reason = "the tests do not run the generators, which the model is for as well"
)]
#[path = "../generate/webgl_wire_model.rs"]
mod webgl_wire_model;
#[cfg(test)]
#[path = "../generate/webgl_wire_model_tests.rs"]
mod webgl_wire_model_tests;
