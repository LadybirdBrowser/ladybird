/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The scroll and input logic of the Compositor's contexts, and the routing of input between them.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../../Libraries/RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../Libraries/RustPanic.rs"]
mod rust_panic;

pub mod async_scroll_update_outbox;
pub mod context_scene;
pub mod context_state;
pub mod core;
pub mod elastic_overscroll_controller;
pub mod ffi;
pub mod frame_inputs;
pub mod host;
pub mod input_event;
pub mod keyboard_scroll_controller;
pub mod scroll_fling_controller;
pub mod scroll_snap_controller;
pub mod scrollbar_controller;
pub mod wheel_gesture_routing;

#[cfg(test)]
mod test_stubs;
