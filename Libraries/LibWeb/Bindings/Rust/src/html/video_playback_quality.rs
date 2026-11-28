/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://w3c.github.io/media-playback-quality/#videoplaybackquality-interface

use core::ffi::c_void;

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
pub(crate) struct VideoPlaybackQuality {
    creation_time: f64,
    dropped_video_frames: u32,
    total_video_frames: u32,
}

impl Wrappable for VideoPlaybackQuality {}

impl VideoPlaybackQuality {
    // https://w3c.github.io/media-playback-quality/#dom-videoplaybackquality-creationtime
    pub(crate) fn creation_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.creation_time)
    }

    // https://w3c.github.io/media-playback-quality/#dom-videoplaybackquality-droppedvideoframes
    pub(crate) fn dropped_video_frames(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.dropped_video_frames)
    }

    // https://w3c.github.io/media-playback-quality/#dom-videoplaybackquality-totalvideoframes
    pub(crate) fn total_video_frames(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.total_video_frames)
    }

    // https://w3c.github.io/media-playback-quality/#dom-videoplaybackquality-corruptedvideoframes
    pub(crate) fn corrupted_video_frames(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(0)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_video_playback_quality_create(
    vm: &Vm,
    creation_time: f64,
    dropped_video_frames: u32,
    total_video_frames: u32,
) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| VideoPlaybackQuality {
        creation_time,
        dropped_video_frames,
        total_video_frames,
    }))
}
