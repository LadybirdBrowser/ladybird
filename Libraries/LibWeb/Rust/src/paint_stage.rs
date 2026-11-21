/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Paint thread, and the stage it presents frames on: the poster frames reach the compositor through.
//!
//! Presenting a frame takes a [`Presenting`], which only the jobs this module hands the Paint thread are lent, there,
//! and which alone reaches the poster. The Paint thread runs its jobs one at a time and in the order they were handed to
//! it, so frames reach the compositor in the order their jobs were handed out, and code that runs anywhere else cannot
//! present a frame: it has no [`Presenting`] to present with.

use crate::painting::display_list::chunks::{Chunk, ChunkId};
use crate::painting::display_list::commands::CompositorContextId;
use crate::painting::display_list::storage::{PackedChunkSize, packed_size_of_chunks, write_packed_chunks};
use crate::painting::presentation::SentDisplayListChunks;
use crate::render_state::SampledFrame;
use crate::stage_thread::{InFlight, Relay, Riding, StageThread};
use libcompositing_rust::ipc_types::{
    Compositing_AsyncScrollingMetadata, Compositing_DisplayListProperties, Core_AnonymousBuffer, SharedDisplayList,
};
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libipc_rust::cpp_connection::{FfiThreadSafeMessagePoster, ThreadSafeMessagePoster};
use libipc_rust::wire::{EndpointMessage, File};
use std::cell::RefCell;
use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::OnceLock;

unsafe extern "C" {
    fn web_compositor_post_fonts_and_image_frames(
        poster: *mut c_void,
        context_id: u64,
        fonts: *const *const c_void,
        font_count: usize,
        image_frames: *const *const c_void,
        image_frame_count: usize,
    ) -> bool;
    fn web_compositor_create_shared_buffer(size: usize, out: &mut FfiSharedBuffer) -> bool;
    fn web_compositor_shared_buffer_release(owner: *mut c_void);
    fn web_compositor_post_screenshot_request(poster: *mut c_void, request: *mut c_void) -> bool;
    fn web_screenshot_request_destroy(request: *mut c_void);
    fn web_compositor_stage_rasterization(
        poster: *mut c_void,
        token: u64,
        fonts: *const *const c_void,
        font_count: usize,
        image_frames: *const *const c_void,
        image_frame_count: usize,
        target_bitmap: *const c_void,
    ) -> bool;
}

/// Shared memory C++ made for a message, which the caller writes and then releases: `Web::Compositor::FfiSharedBuffer`.
#[repr(C)]
pub struct FfiSharedBuffer {
    data: *mut u8,
    size: usize,
    /// A descriptor of the memory of its own, which the caller takes over.
    descriptor: i32,
    owner: *mut c_void,
}

/// Posts messages to the compositor from the thread that holds it: Rust encodes what it can, and C++ the fonts, image
/// frames and screenshot targets, which are C++ objects.
pub(crate) struct CompositorPoster(ThreadSafeMessagePoster);

impl CompositorPoster {
    /// Takes over the reference `ffi` holds.
    ///
    /// # Safety
    ///
    /// `ffi` must hold one reference to a live poster on the transport of the connection to the compositor.
    pub(crate) unsafe fn adopt(ffi: FfiThreadSafeMessagePoster) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self(unsafe { ThreadSafeMessagePoster::adopt(ffi) })
    }

    /// Posts `message`, and answers whether the compositor could still be reached.
    pub(crate) fn post<M: EndpointMessage>(&self, message: M) -> bool {
        self.0.try_post(message).expect("a message to the compositor encodes")
    }

    /// Describes `fonts` to the compositor context `context_id` names and registers `image_frames` with the
    /// compositor, ahead of the display list or the visual context tree that draws them, and answers whether the
    /// compositor could still be reached. The caller holds `image_frames` until it has posted what draws them.
    pub(crate) fn post_fonts_and_image_frames(
        &self,
        context_id: CompositorContextId,
        fonts: &[FontHandle],
        image_frames: &[ImageFrameHandle],
    ) -> bool {
        if fonts.is_empty() && image_frames.is_empty() {
            return true;
        }
        let fonts: Vec<*const c_void> = fonts.iter().map(FontHandle::as_raw).collect();
        let image_frames: Vec<*const c_void> = image_frames.iter().map(ImageFrameHandle::as_raw).collect();
        // SAFETY: This holds a reference to the poster, and the handles keep the fonts and frames live for the call, in
        // which C++ takes references of its own.
        unsafe {
            web_compositor_post_fonts_and_image_frames(
                self.0.as_cpp_poster(),
                context_id.0,
                fonts.as_ptr(),
                fonts.len(),
                image_frames.as_ptr(),
                image_frames.len(),
            )
        }
    }

    /// The display list that names `chunks_in_paint_order` and carries `new_chunks`, the ones the compositor does not
    /// keep, with `properties` and `async_scrolling_metadata`, as a message carries it, or none where the chunks cannot
    /// be placed in shared memory.
    pub(crate) fn share_display_list(
        &self,
        chunks_in_paint_order: Vec<ChunkId>,
        new_chunks: &[&Chunk],
        properties: Compositing_DisplayListProperties,
        async_scrolling_metadata: Option<Compositing_AsyncScrollingMetadata>,
    ) -> Option<SharedDisplayList> {
        let packed_chunks = self.shared_buffer_with(packed_size_of_chunks(new_chunks), |destination| {
            write_packed_chunks(new_chunks, destination);
        })?;
        Some(SharedDisplayList {
            chunks_in_paint_order,
            packed_chunks,
            packed_chunk_sizes: new_chunks.iter().map(|chunk| PackedChunkSize::of(chunk)).collect(),
            properties,
            async_scrolling_metadata,
        })
    }

    /// Shared memory of `size` bytes that `write` fills, as a message carries it, or none where it cannot be made. The
    /// memory is sealed where the platform supports it, so the receiver cannot be faulted by the file shrinking.
    fn shared_buffer_with(&self, size: usize, write: impl FnOnce(&mut [u8])) -> Option<Core_AnonymousBuffer> {
        if size == 0 {
            return Some(Core_AnonymousBuffer { shared_memory: None });
        }
        let mut buffer = FfiSharedBuffer {
            data: std::ptr::null_mut(),
            size: 0,
            descriptor: -1,
            owner: std::ptr::null_mut(),
        };
        // SAFETY: C++ fills `buffer` where it answers true.
        if !unsafe { web_compositor_create_shared_buffer(size, &mut buffer) } {
            return None;
        }
        assert_eq!(buffer.size, size);
        // SAFETY: C++ mapped `size` writable bytes at `data`, which stay mapped until the owner is released.
        write(unsafe { ak::slice_from_raw_parts_mut(buffer.data, size) });
        // SAFETY: The owner is released once, after the last write.
        unsafe { web_compositor_shared_buffer_release(buffer.owner) };
        #[cfg(unix)]
        // SAFETY: C++ hands over a descriptor of its own.
        let descriptor = unsafe { <std::os::fd::OwnedFd as std::os::fd::FromRawFd>::from_raw_fd(buffer.descriptor) };
        #[cfg(windows)]
        // SAFETY: C++ hands over a descriptor of its own.
        let descriptor = unsafe { libipc_rust::transport_windows::OwnedDescriptor::from_raw(buffer.descriptor) };
        Some(Core_AnonymousBuffer {
            shared_memory: Some((size as u64, File(descriptor))),
        })
    }

    /// Stages `fonts` and `image_frames` for the rasterization `token` names into `target_bitmap`, a shareable
    /// `Gfx::Bitmap`, and answers whether the compositor could still be reached.
    ///
    /// # Safety
    ///
    /// `target_bitmap` must be live for the call.
    pub(crate) unsafe fn stage_rasterization(
        &self,
        token: u64,
        fonts: &[FontHandle],
        image_frames: &[ImageFrameHandle],
        target_bitmap: *const c_void,
    ) -> bool {
        let fonts: Vec<*const c_void> = fonts.iter().map(FontHandle::as_raw).collect();
        let image_frames: Vec<*const c_void> = image_frames.iter().map(ImageFrameHandle::as_raw).collect();
        // SAFETY: This holds a reference to the poster, the handles keep the fonts and frames live for the call, in
        // which C++ takes references of its own, and the caller keeps the bitmap live for it.
        unsafe {
            web_compositor_stage_rasterization(
                self.0.as_cpp_poster(),
                token,
                fonts.as_ptr(),
                fonts.len(),
                image_frames.as_ptr(),
                image_frames.len(),
                target_bitmap,
            )
        }
    }

    /// The `IPC::ThreadSafeMessagePoster` this posts through, for C++ to post what it encodes.
    pub(crate) fn as_cpp_poster(&self) -> *mut c_void {
        self.0.as_cpp_poster()
    }
}

/// The Paint thread, which records display lists from the frames the render states publish, and presents frames.
struct PaintThread {
    thread: StageThread,
    stage: StageCell,
}

/// The stage, which only the jobs that are lent a [`Presenting`] reach, on the Paint thread.
struct StageCell(RefCell<PaintStage>);

// SAFETY: The stage is made empty on whichever thread first reaches the Paint thread, and from then on only the Paint
// thread reaches it, in the jobs it runs one at a time.
unsafe impl Send for StageCell {}
unsafe impl Sync for StageCell {}

/// What the Paint thread presents with.
#[derive(Default)]
struct PaintStage {
    /// The poster of the connection to the compositor, while it can be reached.
    poster: Option<CompositorPoster>,
    /// What the compositor at the other end of the poster keeps of the display lists sent for each context, which a
    /// new connection starts without.
    sent_display_list_chunks: std::collections::HashMap<CompositorContextId, SentDisplayListChunks>,
    /// A clock tick that runs as a context is destroyed can still hand the Paint thread a frame of it after it was
    /// forgotten. No context takes the id of a destroyed one, so its frames are dropped.
    destroyed_context_ids: std::collections::HashSet<CompositorContextId>,
}

impl PaintStage {
    fn present_through(&mut self, poster: Option<CompositorPoster>) {
        self.poster = poster;
        self.sent_display_list_chunks.clear();
    }
}

static PAINT_THREAD: OnceLock<PaintThread> = OnceLock::new();

fn the_paint_thread() -> &'static PaintThread {
    PAINT_THREAD.get_or_init(|| PaintThread {
        thread: StageThread::spawn(
            "Paint",
            crate::render_state::release_unused_memory_of_style_layout_threads,
        ),
        stage: StageCell(RefCell::default()),
    })
}

/// The Paint thread, for jobs that present nothing.
pub(crate) fn paint_thread() -> &'static StageThread {
    &the_paint_thread().thread
}

/// What a job on the Paint thread presents with. Only this module makes one, on the Paint thread, for one job at a time.
/// The raw pointer marker makes it neither [`Send`] nor [`Sync`], so it cannot leave the job it is lent to.
pub(crate) struct Presenting<'stage> {
    stage: &'stage mut PaintStage,
    not_send_or_sync: PhantomData<*const ()>,
}

impl Presenting<'_> {
    /// Lends `job` what it presents with. Only a job running on the Paint thread may call this.
    fn lend<R>(job: impl FnOnce(&mut Presenting) -> R) -> R {
        let paint_thread = the_paint_thread();
        debug_assert!(paint_thread.thread.is_current(), "only the Paint thread presents");
        let mut stage = paint_thread.stage.0.borrow_mut();
        job(&mut Presenting {
            stage: &mut stage,
            not_send_or_sync: PhantomData,
        })
    }

    /// The poster frames are presented through, or none while the compositor cannot be reached.
    pub(crate) fn poster(&self) -> Option<&CompositorPoster> {
        self.stage.poster.as_ref()
    }

    /// The poster frames are presented through, with what the compositor keeps of the display lists sent for each
    /// context, or none while the compositor cannot be reached.
    pub(crate) fn poster_and_sent_display_list_chunks(
        &mut self,
    ) -> Option<(
        &CompositorPoster,
        &mut std::collections::HashMap<CompositorContextId, SentDisplayListChunks>,
    )> {
        let stage = &mut *self.stage;
        Some((stage.poster.as_ref()?, &mut stage.sent_display_list_chunks))
    }

    pub(crate) fn context_was_destroyed(&self, context_id: CompositorContextId) -> bool {
        self.stage.destroyed_context_ids.contains(&context_id)
    }

    /// Drops the poster of a compositor that can no longer be reached: the connection learns of that on its own thread,
    /// and hands over the poster of the next one.
    pub(crate) fn lose_compositor(&mut self) {
        self.stage.present_through(None);
    }
}

/// Hands `job` to the Paint thread with `frame`, which the render owner sampled: the Paint thread runs it after the jobs
/// handed to it before, beside whoever holds the ride, and lends it what it presents with. A job that presents is only
/// ever handed a sampled frame, so the Paint thread presents frames in the order the render owner sampled them.
pub(crate) fn ride_presenting<T: Send + 'static, R: Send + 'static>(
    frame: SampledFrame<T>,
    job: impl FnOnce(SampledFrame<T>, &mut Presenting) -> R + Send + 'static,
) -> Riding<R> {
    paint_thread().ride(move || Presenting::lend(|presenting| job(frame, presenting)))
}

/// Hands `job` on to the Paint thread with `frame`, which the render owner sampled, and `relay`, the relay of the job
/// submitted to the render owner that sampled it, which lands what `job` answers (see [`ride_presenting`]).
pub(crate) fn relay_presenting<T: Send + 'static, R: Send + 'static>(
    relay: Relay<R>,
    frame: SampledFrame<T>,
    job: impl FnOnce(SampledFrame<T>, &mut Presenting) -> R + Send + 'static,
) {
    relay.hand_on(paint_thread(), move |_| {
        Presenting::lend(|presenting| job(frame, presenting))
    });
}

/// Submits `job` to the Paint thread with `frame`, which the render owner sampled (see [`ride_presenting`]), and answers
/// the flight of what it answers.
pub(crate) fn submit_presenting<T: Send + 'static, R: Send + 'static>(
    frame: SampledFrame<T>,
    job: impl FnOnce(SampledFrame<T>, &mut Presenting) -> R + Send + 'static,
) -> InFlight<R> {
    paint_thread().submit(move |_| Presenting::lend(|presenting| job(frame, presenting)))
}

/// A poster on the way to the Paint stage.
struct IncomingPoster(CompositorPoster);

// SAFETY: A poster posts from any thread, and nothing reaches it on the way.
unsafe impl Send for IncomingPoster {}

/// Has the Paint stage present frames through `poster`, a poster on the transport of a new connection to the compositor,
/// from the frames handed to the Paint thread after this on. Only the connection calls this, which declares it.
///
/// # Safety
///
/// `poster` must hold one reference to a live poster, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn paint_stage_adopt_compositor_poster(poster: FfiThreadSafeMessagePoster) {
    // SAFETY: Guaranteed by the caller.
    let incoming = IncomingPoster(unsafe { CompositorPoster::adopt(poster) });
    paint_thread().post(move || {
        let IncomingPoster(poster) = incoming;
        Presenting::lend(|presenting| presenting.stage.present_through(Some(poster)));
    });
}

/// Forgets what the compositor kept of the display lists sent for the context `context_id` names, after the frames
/// handed to the Paint thread before, and drops the frames of the context handed to it after. Only the compositor host
/// calls this, as it destroys the context, which declares it. The compositor never gives a destroyed context's id to
/// another context.
#[unsafe(no_mangle)]
pub extern "C" fn paint_stage_forget_destroyed_compositor_context(context_id: u64) {
    let Some(paint_thread) = PAINT_THREAD.get() else {
        return;
    };
    paint_thread.thread.post(move || {
        Presenting::lend(|presenting| {
            let context_id = CompositorContextId(context_id);
            presenting.stage.sent_display_list_chunks.remove(&context_id);
            presenting.stage.destroyed_context_ids.insert(context_id);
        });
    });
}

/// A screenshot of what the compositor composes for a context (a `Web::Compositor::ScreenshotRequestForPaintThread`), on
/// its way to the Paint thread, which asks the compositor for it after the frames handed to it before.
struct ScreenshotRequest(NonNull<c_void>);

// SAFETY: A request owns everything it carries, and nothing else reaches it on the way.
unsafe impl Send for ScreenshotRequest {}

impl Drop for ScreenshotRequest {
    fn drop(&mut self) {
        // SAFETY: The box owns the request.
        unsafe { web_screenshot_request_destroy(self.0.as_ptr()) };
    }
}

/// Asks the compositor for the screenshot `request` names after the frames handed to the Paint thread before it. Only
/// the connection calls this, which declares it.
///
/// # Safety
///
/// `request` must be a `Web::Compositor::ScreenshotRequestForPaintThread`, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn paint_stage_request_screenshot(request: NonNull<c_void>) {
    let request = ScreenshotRequest(request);
    paint_thread().post(move || {
        Presenting::lend(|presenting| {
            let Some(poster) = presenting.poster() else {
                return;
            };
            let request = std::mem::ManuallyDrop::new(request);
            // SAFETY: The stage holds the poster, and C++ takes the request over.
            if !unsafe { web_compositor_post_screenshot_request(poster.as_cpp_poster(), request.0.as_ptr()) } {
                presenting.lose_compositor();
            }
        });
    });
}

#[cfg(test)]
mod ffi_test_stubs {
    use super::FfiSharedBuffer;
    use std::ffi::c_void;

    #[unsafe(no_mangle)]
    extern "C" fn web_compositor_post_fonts_and_image_frames(
        _: *mut c_void,
        _: u64,
        _: *const *const c_void,
        _: usize,
        _: *const *const c_void,
        _: usize,
    ) -> bool {
        true
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_compositor_create_shared_buffer(_: usize, _: &mut FfiSharedBuffer) -> bool {
        false
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_compositor_shared_buffer_release(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_compositor_post_screenshot_request(_: *mut c_void, _: *mut c_void) -> bool {
        true
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_screenshot_request_destroy(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_compositor_stage_rasterization(
        _: *mut c_void,
        _: u64,
        _: *const *const c_void,
        _: usize,
        _: *const *const c_void,
        _: usize,
        _: *const c_void,
    ) -> bool {
        true
    }
}
