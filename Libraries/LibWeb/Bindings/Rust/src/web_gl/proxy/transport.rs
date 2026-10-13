/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Zeroed when it is created.
pub(crate) trait SharedMemory {
    /// Aligned for a `u64`, and stays where it is for as long as the value lives.
    fn as_ptr(&self) -> *mut u8;
    fn len(&self) -> usize;
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct ReadPixelsResult {
    pub(crate) length: i32,
    pub(crate) columns: i32,
    pub(crate) rows: i32,
}

/// A call can find the replayer gone. The host then marks every context lost before the call returns, the one that
/// made the call included. That is all a call does to the recorder that made it: it neither records, nor flushes,
/// nor runs script.
pub(crate) trait Transport {
    type SharedMemory: SharedMemory;
    /// An image that a command names by its position among the frames sent with the commands.
    type Frame;
    type Bitmap;

    fn max_frames_per_message(&self) -> usize;
    fn create_shared_memory(&self, size: usize) -> Option<Self::SharedMemory>;

    fn canvas_id(&self) -> Option<u64>;
    fn destroy_context(&self);

    /// Makes the memory the shared command buffer of the context: a `u64` that the replayer stores the sequence
    /// number of the last flush that it executed in, and a region of records.
    fn set_shared_command_buffer(&self, buffer: &Self::SharedMemory);
    /// `offset` is into the region of records.
    fn send_commands_from_shared_buffer(
        &self,
        offset: u64,
        size_in_bytes: u64,
        flush_sequence_number: u64,
        frames: &[Self::Frame],
    );
    /// Returns once the replayer has executed everything published, or false if it never will.
    fn wait_until_published_commands_executed(&self) -> bool;
    fn send_commands(&self, commands: &[u8], frames: &[Self::Frame]);

    fn present_canvas(&self, preserve_drawing_buffer: bool);
    fn clear_drawing_buffer(&self);

    /// A call that failed appends nothing to `reply`.
    fn sync_call(&self, request: &[u8], reply: &mut Vec<u8>);
    #[expect(clippy::too_many_arguments, reason = "the arguments of the GL function")]
    fn read_pixels_robust_angle(
        &self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        buf_size: i32,
        pixels: Option<&Self::SharedMemory>,
    ) -> ReadPixelsResult;
    fn read_buffer_sub_data(&self, target: u32, offset: i64, size: i64, data: &Self::SharedMemory) -> bool;
    fn read_back_drawing_buffer(&self, x: i32, y: i32, width: i32, height: i32) -> Option<Self::Bitmap>;
}
