/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod async_scroll_tree;
pub mod async_scrolling_state;
pub mod display_list;
pub mod elastic_overscroll;
pub mod overlay_fade;
pub mod scroll_fling;
pub mod scroll_snap_selection;
pub mod scroll_state_snapshot;
pub mod smooth_scroll_animation;
pub mod visual_context_tree;
pub mod wheel_gesture_identity;

pub use crate::display_list::commands::{ContextRef, EffectNodeIndex, SpatialNodeIndex};
pub use crate::ipc_types::Compositing_AppliedScrollRequest as AppliedScrollRequest;
pub use crate::ipc_types::Compositing_AsyncScrollOffset as AsyncScrollOffset;
pub use crate::ipc_types::Compositing_KeyboardScrollState as KeyboardScrollState;
pub use crate::ipc_types::Compositing_PendingAsyncScrollUpdates as PendingAsyncScrollUpdates;
pub use crate::ipc_types::Compositing_ScrollAnimationKind as ScrollAnimationKind;
pub use crate::ipc_types::Compositing_ScrollRequest as ScrollRequest;
pub use crate::ipc_types::Compositing_SnapAreaIdentity as SnapAreaIdentity;
pub use crate::ipc_types::Compositing_SnapDestination as SnapDestination;
pub use crate::ipc_types::Compositing_SnappedAreas as SnappedAreas;
pub use crate::ipc_types::Compositing_StartedUserScroll as StartedUserScroll;
pub use crate::ipc_types::Compositing_WheelStepDispatch as WheelStepDispatch;
pub use crate::ipc_types::Web_AsyncScrollNodeKind as AsyncScrollNodeKind;
pub use crate::ipc_types::Web_AsyncScrollNodeStableID as AsyncScrollNodeStableID;
pub use crate::ipc_types::Web_CompositorContextId as CompositorContextId;
pub use crate::ipc_types::Web_DevicePixelPoint as DevicePixelPoint;
pub use crate::ipc_types::Web_InputRoutingDecision as InputRoutingDecision;
pub use crate::ipc_types::Web_KeyEvent as KeyEvent;
pub use crate::ipc_types::Web_KeyEvent_Type as KeyEventType;
pub use crate::ipc_types::Web_MouseEvent as MouseEvent;
pub use crate::ipc_types::Web_MouseEvent_Type as MouseEventType;
pub use crate::ipc_types::Web_OverlayScrollbarVisibility as OverlayScrollbarVisibility;
pub use crate::ipc_types::Web_PinchEvent as PinchEvent;
pub use crate::ipc_types::Web_PointerInputData as PointerInputData;
pub use crate::ipc_types::Web_ScrollGesturePhase as ScrollGesturePhase;
pub use crate::ipc_types::Web_ScrollbarDraggedByCompositor as ScrollbarDraggedByCompositor;
pub use crate::ipc_types::Web_UIEvents_KeyCode as KeyCode;
pub use crate::ipc_types::Web_UIEvents_KeyModifier as KeyModifier;
pub use crate::ipc_types::Web_UIEvents_MouseButton as MouseButton;
pub use crate::ipc_types::Web_UniqueNodeID as UniqueNodeId;
pub use crate::ipc_types::Web_WheelDeltaPrecision as WheelDeltaPrecision;
pub use crate::ipc_types::Web_WheelGestureMark as WheelGestureMark;
pub use crate::visual_context::CompositorScrollNodeKind;
pub use async_scroll_tree::*;
pub use async_scrolling_state::*;
pub use display_list::{AsyncScrollingMetadata, DisplayList};
pub use libgfx_rust::Orientation;
pub use scroll_state_snapshot::{ScrollStateSnapshot, WorldTransformsCache};
pub use visual_context_tree::{
    AccumulatedVisualContextTree, AnimatedContentViewportEffect, ClipBehavior, IncludeVisualViewportTransform,
    TransformWithOrigin, animated_content_may_affect_viewport, resolve_sticky_offsets,
};

pub const USER_SCROLL_SETTLE_DELAY: crate::time::Duration = crate::time::Duration::from_milliseconds(500);

fn min<T: PartialOrd>(a: T, b: T) -> T {
    if b < a { b } else { a }
}

fn max<T: PartialOrd>(a: T, b: T) -> T {
    if a < b { b } else { a }
}
