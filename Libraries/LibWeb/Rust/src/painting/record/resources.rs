/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;

use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId, VideoSinkResourceId};
use crate::painting::record::vector_images::{VECTOR_IMAGE_PLACEHOLDER_TAG, VectorImageRenderRequest};
use libgfx_rust::font::{FontHandle, FontId};
use libgfx_rust::image_frame::ImageFrameHandle;

#[derive(Default)]
pub(crate) struct RecordingResourceManifest {
    pub(crate) fonts: HashMap<FontId, FontHandle>,
    pub(crate) image_frames: HashMap<u64, ImageFrameHandle>,
    pub(crate) video_sinks: HashMap<u64, u64>,
    pub(crate) vector_image_render_requests: Vec<VectorImageRenderRequest>,
    vector_image_request_indices: HashMap<VectorImageRenderRequest, u32>,
}

impl RecordingResourceManifest {
    pub(crate) fn note_font(&mut self, font: &FontHandle) -> u64 {
        self.fonts.entry(font.id()).or_insert_with(|| font.clone());
        font.id().0
    }

    pub(crate) fn note_image_frame(&mut self, frame: &ImageFrameHandle) -> ImageFrameResourceId {
        self.image_frames.entry(frame.id()).or_insert_with(|| frame.clone());
        ImageFrameResourceId(frame.id())
    }

    pub(crate) fn note_video_sink(&mut self, resource_id: u64, sink_handle: u64) -> VideoSinkResourceId {
        self.video_sinks.entry(resource_id).or_insert(sink_handle);
        VideoSinkResourceId(resource_id)
    }

    pub(crate) fn vector_image_placeholder(&mut self, request: VectorImageRenderRequest) -> DisplayListResourceId {
        let next_index = self.vector_image_render_requests.len() as u32;
        let index = *self.vector_image_request_indices.entry(request).or_insert_with(|| {
            self.vector_image_render_requests.push(request);
            next_index
        });
        DisplayListResourceId(VECTOR_IMAGE_PLACEHOLDER_TAG | u64::from(index))
    }
}
