/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Web Animations.

pub(crate) mod animation;
pub mod bindings_api;
pub(crate) mod clock_plan;
pub(crate) mod composite_order;
pub(crate) mod compositor_update;
pub(crate) mod css_events;
pub(crate) mod document_animations;
pub(crate) mod easing;
pub(crate) mod effect;
pub(crate) mod element_animations;
pub(crate) mod ffi;
pub(crate) mod host_effects;
pub(crate) mod host_timing;
pub(crate) mod keyframe_reflection;
pub(crate) mod keyframes;
pub(crate) mod pending_animation_events;
pub(crate) mod procedures;
pub(crate) mod provisional_transitions;
pub(crate) mod rendering_trace;
pub(crate) mod rendering_update;
pub(crate) mod snapshot;
pub(crate) mod throttling;
pub(crate) mod timeline;
pub(crate) mod timeline_host;
pub(crate) mod timeline_state;
pub(crate) mod transitions;
pub(crate) mod weak_record_set;

pub(crate) use libcompositing_rust::animation_timing as timing;
