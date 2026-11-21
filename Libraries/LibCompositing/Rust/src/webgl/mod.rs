/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebGL command stream that WebContent records and the Compositor replays, as Libraries/LibCompositing/WebGL/
//! GLFunctions.json defines it.

use libipc_rust::wire::{Decode, Encode};

use crate::display_list::commands::{ImageFrameResourceId, VideoSinkHandle};

pub mod texture_upload;
pub mod vocabulary;
pub mod wire;

/// What an upload of an image to a texture takes the pixels of: an image frame that WebContent registered, or a frame of
/// a video that the Compositor holds for it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
pub enum ImageSource {
    RegisteredImageFrame(ImageFrameResourceId),
    HeldVideoFrame {
        sink_handle: VideoSinkHandle,
        generation: u64,
    },
}

/// The `u64` that the replayer stores the sequence number of the last executed flush in is at the start of the shared
/// command buffer, before the region of records.
pub const SHARED_DATA_REGION_OFFSET: usize = 128;
pub const DEFAULT_SHARED_DATA_REGION_CAPACITY: usize = 32 * 1024 * 1024;
