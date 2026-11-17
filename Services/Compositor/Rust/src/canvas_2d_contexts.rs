/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::host::WebContentClientId;
use crate::raster_host::RasterHost;
use libcompositing_rust::display_list::canvas_2d::for_each_canvas_2d_command;
use libcompositing_rust::display_list::commands::CanvasId;
use libcompositing_rust::display_list::validate::validate_canvas_2d_tape;
use libcompositing_rust::ipc_types::{Compositing_Canvas2DCommandStreamSegment, Web_Bindings_PredefinedColorSpace};
use libgfx_rust::IntSize;
use std::collections::HashMap;
use std::ops::Range;

const RESERVED_CANVAS_ID_COUNT: u64 = 1 << 20;

#[derive(Default)]
pub struct Canvas2DContexts {
    clients: HashMap<WebContentClientId, ClientContexts>,
}

#[derive(Default)]
struct ClientContexts {
    reserved_ids: Range<u64>,
    drew_since_presenting: HashMap<CanvasId, bool>,
}

pub struct ContextCreation {
    pub canvas_id: CanvasId,
    pub size: IntSize,
    pub alpha: bool,
    pub will_read_frequently: bool,
    pub color_space: Web_Bindings_PredefinedColorSpace,
}

impl Canvas2DContexts {
    pub fn reserve_ids(&mut self, client: WebContentClientId, raster: &RasterHost) -> Result<Range<u64>, &'static str> {
        let contexts = self.clients.entry(client).or_default();
        if !contexts.reserved_ids.is_empty() {
            return Err("WebContent reserved canvas IDs before using up its range");
        }
        let first_id = raster.reserve_canvas_ids(RESERVED_CANVAS_ID_COUNT);
        contexts.reserved_ids = first_id..first_id + RESERVED_CANVAS_ID_COUNT;
        Ok(contexts.reserved_ids.clone())
    }

    pub fn create(
        &mut self,
        client: WebContentClientId,
        raster: &RasterHost,
        creation: ContextCreation,
    ) -> Result<(), &'static str> {
        let contexts = self.clients.entry(client).or_default();
        // NB: WebContent names its contexts with the IDs of its range in order, so each ID names one context only.
        if contexts.reserved_ids.is_empty() || creation.canvas_id.0 != contexts.reserved_ids.start {
            return Err("WebContent named a 2D canvas context with an ID other than the next one it reserved");
        }
        contexts.reserved_ids.start += 1;
        // NB: A context that cannot be created is not a protocol violation: commands recorded for it are dropped.
        if raster.create_canvas_2d_context(
            client.0,
            creation.canvas_id,
            creation.size,
            creation.alpha,
            creation.will_read_frequently,
            creation.color_space,
        ) {
            contexts.drew_since_presenting.insert(creation.canvas_id, false);
        }
        Ok(())
    }

    pub fn destroy(&mut self, client: WebContentClientId, raster: &RasterHost, canvas_id: CanvasId) {
        let Some(contexts) = self.clients.get_mut(&client) else {
            return;
        };
        if contexts.drew_since_presenting.remove(&canvas_id).is_some() {
            raster.destroy_canvas_2d_context(client.0, canvas_id);
        }
    }

    pub fn play_stream(
        &mut self,
        client: WebContentClientId,
        raster: &RasterHost,
        segments: &[Compositing_Canvas2DCommandStreamSegment],
    ) -> Result<(), &'static str> {
        if segments
            .iter()
            .any(|segment| validate_canvas_2d_tape(&segment.tape).is_err())
        {
            return Err("WebContent sent a canvas 2D stream that does not validate");
        }
        let Some(contexts) = self.clients.get_mut(&client) else {
            return Ok(());
        };
        let mut contexts_that_sampled_video_frames = Vec::new();
        for segment in segments {
            // The canvas may have been destroyed while this segment was pending in
            // WebContent, so a missing context is not a protocol violation.
            let Some(drew_since_presenting) = contexts.drew_since_presenting.get_mut(&segment.canvas_id) else {
                continue;
            };
            if !segment.tape.is_empty() {
                let painting = raster.begin_canvas_2d_painting(client.0, segment.canvas_id);
                for_each_canvas_2d_command(&segment.tape, |command_type, payload| {
                    painting.play_command(command_type, payload);
                });
                if painting.end_reporting_whether_video_frames_were_sampled_on_the_gpu()
                    && !contexts_that_sampled_video_frames.contains(&segment.canvas_id)
                {
                    contexts_that_sampled_video_frames.push(segment.canvas_id);
                }
                *drew_since_presenting = true;
            }
            if segment.present
                && *drew_since_presenting
                && raster.try_present_canvas_2d_context(client.0, segment.canvas_id)
            {
                *drew_since_presenting = false;
            }
        }
        // The frames stay held only until WebContent releases them, which follows this stream, so the GPU has to have
        // finished sampling them by then.
        if !contexts_that_sampled_video_frames.is_empty() {
            raster.flush_canvas_2d_contexts(client.0, &contexts_that_sampled_video_frames);
        }
        Ok(())
    }

    pub fn forget_client(&mut self, client: WebContentClientId) {
        self.clients.remove(&client);
    }
}
