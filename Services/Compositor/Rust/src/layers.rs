/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::frame_inputs::FrameInputs;
use libcompositing_rust::display_list::commands::{CanvasId, VideoSinkResourceId};
use libcompositing_rust::display_list::layers::{LayerKind, LayerPlan, LayerPlanInputs, build_layer_plan};
use libcompositing_rust::display_list::storage::storage_from_handle;
use libcompositing_rust::display_list::summary::LiveContent;
use libcompositing_rust::host::RecordSpan;
use libgfx_rust::IntRect;
use std::ffi::c_void;
use std::sync::Arc;

pub struct ContextLayerPlan(pub(crate) LayerPlan);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiLiveMediaKind {
    Canvas,
    Video,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiLiveMedia {
    pub kind: FfiLiveMediaKind,
    pub id: u64,
}

impl FfiLiveMedia {
    fn as_live_content(self) -> LiveContent {
        match self.kind {
            FfiLiveMediaKind::Canvas => LiveContent::Canvas(CanvasId(self.id)),
            FfiLiveMediaKind::Video => LiveContent::VideoFrame(VideoSinkResourceId(self.id)),
        }
    }

    fn of_live_content(content: LiveContent) -> Self {
        match content {
            LiveContent::Canvas(id) => Self {
                kind: FfiLiveMediaKind::Canvas,
                id: id.0,
            },
            LiveContent::VideoFrame(id) => Self {
                kind: FfiLiveMediaKind::Video,
                id: id.0,
            },
            LiveContent::ImageFrame(_) => unreachable!("an image is never a layer of its own"),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiLayerKind {
    Base,
    Painted,
    Media,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiLayer {
    pub kind: FfiLayerKind,
    pub media: FfiLiveMedia,
    pub rect: IntRect,
    pub record_spans: *const RecordSpan,
    pub record_span_count: usize,
}

/// # Safety
///
/// `display_list_storage` must be a live storage handle of a list recorded against the inputs' tree, and `active_media`
/// must address `active_media_count` values for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_frame_inputs_build_layer_plan(
    frame_inputs: &FrameInputs,
    display_list_storage: *const c_void,
    viewport_rect: IntRect,
    active_media: *const FfiLiveMedia,
    active_media_count: usize,
) -> *mut ContextLayerPlan {
    // SAFETY: The caller guarantees a live handle and a valid slice for the call.
    let (storage, active_media) = unsafe {
        (
            storage_from_handle(display_list_storage),
            ak::slice_from_raw_parts(active_media, active_media_count),
        )
    };
    let active_media: Vec<LiveContent> = active_media.iter().map(|media| media.as_live_content()).collect();
    let tree = frame_inputs.visual_context_tree.as_arc();
    let plan = build_layer_plan(&LayerPlanInputs {
        tape: storage.bytes(),
        command_runs: storage.command_runs(),
        summary: storage.summary(),
        effect_clips: storage.effect_clip_plan(tree),
        tree,
        scroll_offsets: frame_inputs.presented_scroll_state_snapshot.device_offsets(),
        viewport_rect,
        active_media: &active_media,
    });
    plan.map_or(std::ptr::null_mut(), |plan| {
        Box::into_raw(Box::new(ContextLayerPlan(plan)))
    })
}

/// # Safety
///
/// `plan` must be a plan `compositor_frame_inputs_build_layer_plan` made that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_layer_plan_destroy(plan: *mut ContextLayerPlan) {
    // SAFETY: The caller hands over the plan.
    drop(unsafe { Box::from_raw(plan) });
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_layer_plan_layer_count(plan: &ContextLayerPlan) -> usize {
    plan.0.layers.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_layer_plan_layer(plan: &ContextLayerPlan, index: usize, ffi_layer: &mut FfiLayer) {
    let layer = &plan.0.layers[index];
    let (kind, media) = match layer.kind {
        LayerKind::Base => (FfiLayerKind::Base, None),
        LayerKind::Painted => (FfiLayerKind::Painted, None),
        LayerKind::Media { content, .. } => (FfiLayerKind::Media, Some(FfiLiveMedia::of_live_content(content))),
    };
    *ffi_layer = FfiLayer {
        kind,
        media: media.unwrap_or(FfiLiveMedia {
            kind: FfiLiveMediaKind::Canvas,
            id: 0,
        }),
        rect: layer.rect,
        record_spans: layer.record_spans.as_ptr(),
        record_span_count: layer.record_spans.len(),
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_layer_plans_have_same_layers(plan: &ContextLayerPlan, other: &ContextLayerPlan) -> bool {
    plan.0.has_same_layers_as(&other.0)
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_frame_inputs_place_content_alike(a: &FrameInputs, b: &FrameInputs) -> bool {
    Arc::ptr_eq(a.visual_context_tree.as_arc(), b.visual_context_tree.as_arc())
        && a.presented_scroll_state_snapshot.device_offsets() == b.presented_scroll_state_snapshot.device_offsets()
}
