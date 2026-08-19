/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod hit_test;
pub mod paint;
pub mod visual_context;

pub use hit_test::*;
pub use paint::*;
pub use visual_context::*;

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRootBackgroundSource {
    pub use_body_background_properties: bool,
    pub body_layout_node: crate::layout::node_data::NodeSlotId,
}
