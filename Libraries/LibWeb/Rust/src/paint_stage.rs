/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Paint thread, and the stage it presents frames on: the frame sink frames reach the compositor through.
//!
//! Presenting a frame takes a [`Presenting`], which only the jobs this module hands the Paint thread are lent, there,
//! and which alone reaches the frame sink. The Paint thread runs its jobs one at a time and in the order they were
//! handed to it, so frames reach the compositor in the order their jobs were handed out, and code that runs anywhere
//! else cannot present a frame: it has no [`Presenting`] to present with.

use crate::render_state::SampledFrame;
use crate::stage_thread::{Relay, Riding, StageThread};
use std::cell::RefCell;
use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::OnceLock;

unsafe extern "C" {
    fn web_frame_sink_release(sink: *mut c_void);
    fn web_frame_sink_submit(sink: *mut c_void, frame: *mut c_void);
    fn web_compositor_frame_destroy(frame: *mut c_void);
    fn web_navigable_presenter_present_sealed_frame(
        presenter: *mut c_void,
        sealed_frame: *mut c_void,
        sink: *mut c_void,
    );
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
    /// The frame sink of the connection to the compositor, while there is one.
    sink: Option<FrameSinkBox>,
}

fn the_paint_thread() -> &'static PaintThread {
    static PAINT_THREAD: OnceLock<PaintThread> = OnceLock::new();
    PAINT_THREAD.get_or_init(|| PaintThread {
        thread: StageThread::spawn("Paint"),
        stage: StageCell(RefCell::default()),
    })
}

/// The Paint thread, for jobs that present nothing.
pub(crate) fn paint_thread() -> &'static StageThread {
    &the_paint_thread().thread
}

/// A reference to a `Web::Compositor::CompositorFrameSink`, which only the Paint stage holds. The pointer makes it
/// neither [`Send`] nor [`Sync`]: it never leaves the Paint thread.
struct FrameSinkBox(NonNull<c_void>);

impl Drop for FrameSinkBox {
    fn drop(&mut self) {
        // SAFETY: The box holds a reference to the sink.
        unsafe { web_frame_sink_release(self.0.as_ptr()) };
    }
}

/// A reference to a frame sink on its way to the Paint stage.
struct IncomingFrameSink(FrameSinkBox);

// SAFETY: A frame sink takes frames from any thread, and nothing reaches this reference on the way.
unsafe impl Send for IncomingFrameSink {}

impl IncomingFrameSink {
    fn arrive(self) -> FrameSinkBox {
        self.0
    }
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

    /// The frame sink frames are presented through, as C++ takes it, or null while the compositor cannot be reached.
    pub(crate) fn sink(&self) -> *mut c_void {
        self.stage
            .sink
            .as_ref()
            .map_or(std::ptr::null_mut(), |sink| sink.0.as_ptr())
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

/// Has the Paint stage present frames through `sink`, a reference to the frame sink of a new connection to the
/// compositor, which the stage takes over, from the frames handed to the Paint thread after this on. Only the
/// connection calls this, which declares it.
///
/// # Safety
///
/// `sink` must be a reference to a frame sink, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn paint_stage_adopt_frame_sink(sink: NonNull<c_void>) {
    let incoming = IncomingFrameSink(FrameSinkBox(sink));
    paint_thread().post(move || {
        Presenting::lend(|presenting| presenting.stage.sink = Some(incoming.arrive()));
    });
}

/// A frame the main thread built, on its way to the Paint thread, which presents it.
struct MainThreadFrame(NonNull<c_void>);

// SAFETY: A frame owns everything its messages carry, so it can be handed to the compositor from any thread.
unsafe impl Send for MainThreadFrame {}

impl Drop for MainThreadFrame {
    fn drop(&mut self) {
        // SAFETY: The box owns the frame.
        unsafe { web_compositor_frame_destroy(self.0.as_ptr()) };
    }
}

/// Presents `frame`, which the main thread built, after the frames handed to the Paint thread before it. Only the
/// connection calls this, which declares it.
///
/// # Safety
///
/// `frame` must be a `Web::Compositor::CompositorFrame`, which the caller gives up.
// FIXME: Only the render owner should sample the frames the Paint thread presents.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn paint_stage_present_frame_from_main_thread(frame: NonNull<c_void>) {
    let frame = MainThreadFrame(frame);
    paint_thread().post(move || {
        Presenting::lend(|presenting| {
            let sink = presenting.sink();
            if sink.is_null() {
                return;
            }
            let frame = std::mem::ManuallyDrop::new(frame);
            // SAFETY: The stage holds the sink, which takes the frame over.
            unsafe { web_frame_sink_submit(sink, frame.0.as_ptr()) };
        });
    });
}

/// A navigable's presenter, and a frame the main thread sealed, which the main thread lends the Paint thread while it
/// waits for the frame to be presented.
struct LentSealedFrame {
    presenter: NonNull<c_void>,
    sealed_frame: NonNull<c_void>,
}

// SAFETY: The main thread lends both while it waits, and nothing else reaches them meanwhile.
unsafe impl Send for LentSealedFrame {}

impl LentSealedFrame {
    fn present(self, presenting: &mut Presenting) {
        // SAFETY: The main thread lends both while it waits, and the stage holds the sink.
        unsafe {
            web_navigable_presenter_present_sealed_frame(
                self.presenter.as_ptr(),
                self.sealed_frame.as_ptr(),
                presenting.sink(),
            );
        }
    }
}

/// Builds the frame `sealed_frame` names, which the main thread sealed, with `presenter`, and presents it after the
/// frames handed to the Paint thread before it, while the caller waits. Only the compositor context calls this, which
/// declares it.
///
/// # Safety
///
/// `presenter` must be a `Web::Compositor::NavigablePresenter` and `sealed_frame` a `Web::Compositor::SealedFrame`,
/// which nothing else reaches until this returns.
// FIXME: Only the render owner should sample the frames the Paint thread presents.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn paint_stage_present_sealed_frame(presenter: NonNull<c_void>, sealed_frame: NonNull<c_void>) {
    let lent = LentSealedFrame {
        presenter,
        sealed_frame,
    };
    paint_thread().run(move || Presenting::lend(|presenting| lent.present(presenting)));
}

#[cfg(test)]
mod ffi_test_stubs {
    use std::ffi::c_void;

    #[unsafe(no_mangle)]
    extern "C" fn web_frame_sink_release(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_frame_sink_submit(_: *mut c_void, _: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_compositor_frame_destroy(_: *mut c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn web_navigable_presenter_present_sealed_frame(_: *mut c_void, _: *mut c_void, _: *mut c_void) {}
}
