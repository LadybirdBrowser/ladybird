/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod elastic_overscroll;
pub mod overlay_fade;
pub mod scroll_fling;
pub mod scroll_snap_selection;
pub mod smooth_scroll_animation;
pub mod wheel_gesture_identity;

pub use crate::ipc_types::Compositing_ScrollAnimationKind as ScrollAnimationKind;
pub use crate::ipc_types::Compositing_SnapAreaIdentity as SnapAreaIdentity;
pub use crate::ipc_types::Compositing_SnapDestination as SnapDestination;
pub use crate::ipc_types::Compositing_SnappedAreas as SnappedAreas;
pub use crate::ipc_types::Web_AsyncScrollNodeStableID as AsyncScrollNodeStableID;
pub use crate::ipc_types::Web_ScrollGesturePhase as ScrollGesturePhase;
pub use libgfx_rust::Orientation;

pub const USER_SCROLL_SETTLE_DELAY: crate::time::Duration = crate::time::Duration::from_milliseconds(500);

fn min<T: PartialOrd>(a: T, b: T) -> T {
    if b < a { b } else { a }
}

fn max<T: PartialOrd>(a: T, b: T) -> T {
    if a < b { b } else { a }
}
