/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::RefCell;
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::HashMap;
use std::rc::Rc;

use libcompositing_rust::display_list::commands::{CanvasId, FontResourceId, ImageFrameResourceId};
use libcompositing_rust::ipc_types::Compositing_Canvas2DCommandStreamSegment;
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::UpdateCanvas2dStream;
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;

use super::host::VideoFrameHold;
use crate::compositor_connection::CompositorConnection;

#[derive(Default)]
pub(crate) struct Canvas2DStream {
    pending: Batch,
}

struct PendingSegment {
    connection_serial: u64,
    segment: Compositing_Canvas2DCommandStreamSegment,
}

#[derive(Default)]
struct Batch {
    segments: Vec<PendingSegment>,
    command_count: usize,
    fonts: HashMap<u64, FontHandle>,
    image_frames: HashMap<u64, ImageFrameHandle>,
    video_frame_holds: Vec<VideoFrameHold>,
}

impl Batch {
    fn put_back_ahead(&mut self, mut earlier: Batch) {
        let later = std::mem::take(self);
        earlier.segments.extend(later.segments);
        earlier.command_count += later.command_count;
        earlier.fonts.extend(later.fonts);
        earlier.image_frames.extend(later.image_frames);
        earlier.video_frame_holds.extend(later.video_frame_holds);
        *self = earlier;
    }
}

impl Canvas2DStream {
    pub(crate) fn append_command(&mut self, connection_serial: u64, canvas_id: CanvasId, record: &[u8]) {
        // A presented segment must keep its position in the stream, so recording after a present marker starts a new
        // segment for the same canvas.
        let segments = &mut self.pending.segments;
        match segments.last_mut() {
            Some(pending)
                if pending.connection_serial == connection_serial
                    && pending.segment.canvas_id == canvas_id
                    && !pending.segment.present =>
            {
                pending.segment.tape.extend_from_slice(record);
            }
            _ => segments.push(PendingSegment {
                connection_serial,
                segment: Compositing_Canvas2DCommandStreamSegment {
                    canvas_id,
                    tape: record.to_vec(),
                    present: false,
                },
            }),
        }
        self.pending.command_count += 1;
    }

    /// Marks the canvas as presentable at this point of the stream.
    pub(crate) fn record_present(&mut self, connection_serial: u64, canvas_id: CanvasId) {
        let segments = &mut self.pending.segments;
        match segments.last_mut() {
            Some(pending)
                if pending.connection_serial == connection_serial && pending.segment.canvas_id == canvas_id =>
            {
                pending.segment.present = true;
            }
            _ => segments.push(PendingSegment {
                connection_serial,
                segment: Compositing_Canvas2DCommandStreamSegment {
                    canvas_id,
                    tape: Vec::new(),
                    present: true,
                },
            }),
        }
    }

    pub(crate) fn add_font(&mut self, font: &FontHandle) -> FontResourceId {
        let id = font.id().0;
        self.pending.fonts.entry(id).or_insert_with(|| font.clone());
        FontResourceId(id)
    }

    pub(crate) fn add_image_frame(&mut self, frame: &ImageFrameHandle) -> ImageFrameResourceId {
        let id = frame.id();
        self.pending.image_frames.entry(id).or_insert_with(|| frame.clone());
        ImageFrameResourceId(id)
    }

    pub(crate) fn retain_video_frame_hold(&mut self, hold: &VideoFrameHold) {
        let holds = &mut self.pending.video_frame_holds;
        if holds.last().is_some_and(|last| last.is_same_hold_as(hold)) {
            return;
        }
        holds.push(hold.clone());
    }

    /// The number of records that the stream holds for every canvas.
    pub(crate) fn pending_command_count(&self) -> usize {
        self.pending.command_count
    }
}

/// Posts what `stream` holds to the Compositor through `connection`. While the Compositor cannot be reached through
/// it, the records stay in the stream for a flush through a connection that can reach one.
pub(crate) fn flush(stream: &RefCell<Canvas2DStream>, connection: CompositorConnection) {
    let mut batch = std::mem::take(&mut stream.borrow_mut().pending);
    if batch.segments.is_empty() {
        stream.borrow_mut().pending.put_back_ahead(batch);
        return;
    }
    let Some(poster) = connection.poster_while_compositor_is_reachable() else {
        stream.borrow_mut().pending.put_back_ahead(batch);
        return;
    };
    // NB: The canvases that a connection made are named by the IDs that its Compositor handed out, which the next
    //     Compositor hands out again. So the records of canvases of an older connection are dropped, and those of a
    //     newer one wait for it.
    let connection_serial = connection.serial();
    if batch
        .segments
        .iter()
        .any(|pending| pending.connection_serial > connection_serial)
    {
        stream.borrow_mut().pending.put_back_ahead(batch);
        return;
    }
    batch
        .segments
        .retain(|pending| pending.connection_serial == connection_serial);
    if batch.segments.is_empty() {
        return;
    }
    let fonts: Vec<&FontHandle> = batch.fonts.values().collect();
    let image_frames: Vec<&ImageFrameHandle> = batch.image_frames.values().collect();
    if !connection.post_stream_batch_fonts_and_image_frames(&fonts, &image_frames) {
        stream.borrow_mut().pending.put_back_ahead(batch);
        return;
    }
    let message = UpdateCanvas2dStream {
        segments: std::mem::take(&mut batch.segments)
            .into_iter()
            .map(|pending| pending.segment)
            .collect(),
    };
    if !connection.post_or_lose_compositor(&poster, message) {
        return;
    }
    for hold in &batch.video_frame_holds {
        hold.note_sent_to_compositor();
    }
}

/// # Safety
///
/// `stream` must be a stream that web_canvas_2d_stream_create() made, which C++ does not destroy during `'a`.
unsafe fn stream_from_raw<'a>(stream: *mut c_void) -> &'a RefCell<Canvas2DStream> {
    // SAFETY: Guaranteed by the caller.
    unsafe { &*stream.cast::<RefCell<Canvas2DStream>>() }
}

/// # Safety
///
/// `stream` must be a stream that web_canvas_2d_stream_create() made and C++ has not destroyed yet.
pub(crate) unsafe fn shared_stream(stream: NonNull<c_void>) -> Rc<RefCell<Canvas2DStream>> {
    let stream = stream.as_ptr().cast::<RefCell<Canvas2DStream>>().cast_const();
    // SAFETY: The stream is an Rc, of which C++ holds a reference, guaranteed by the caller.
    unsafe {
        Rc::increment_strong_count(stream);
        Rc::from_raw(stream)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_stream_create() -> *mut c_void {
    Rc::into_raw(Rc::new(RefCell::new(Canvas2DStream::default())))
        .cast_mut()
        .cast()
}

/// # Safety
///
/// `stream` must be a stream that web_canvas_2d_stream_create() made, which the caller does not use afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_stream_destroy(stream: *mut c_void) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Rc::from_raw(stream.cast::<RefCell<Canvas2DStream>>().cast_const()) });
}

/// # Safety
///
/// `stream` must be a stream that web_canvas_2d_stream_create() made, and `connection` a live
/// Web::Compositor::CompositorConnection.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_stream_flush(stream: *mut c_void, connection: *mut c_void) {
    let connection = NonNull::new(connection).expect("C++ passes a connection");
    // SAFETY: Guaranteed by the caller.
    unsafe { flush(stream_from_raw(stream), CompositorConnection::lent(connection)) };
}

/// # Safety
///
/// `stream` must be a stream that web_canvas_2d_stream_create() made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_stream_discard(stream: *mut c_void) {
    // SAFETY: Guaranteed by the caller.
    let discarded = std::mem::take(&mut unsafe { stream_from_raw(stream) }.borrow_mut().pending);
    drop(discarded);
}
